# alg_log 日志模块设计与使用说明

`alg_log` 为 SDK 提供异步日志、滚动文件和多目标输出能力，兼容 `ALG_LOGE`、`ALG_LOGW`、`ALG_LOGI`、`ALG_LOGD` 调用方式。异步队列和输出目标的组织方式参考 spdlog 的设计，启动信息采用与 glog 类似的记录格式。

## 设计说明

日志格式化在调用线程执行，异步模式下的文件和终端写入由后台线程处理，以减少日志 I/O 对推理耗时的影响。队列采用互斥锁保护；队列满时丢弃消息并累计数量，不等待空闲槽位。该机制减少了等待日志写入的时间，但仍存在格式化、加锁和入队开销。

| 能力 | 实现方式 |
|------|----------|
| 异步写入 | 调用线程格式化并入队，后台线程批量输出 |
| 文件滚动 | 按单文件大小滚动，按保留数量清理最早的记录 |
| 控制台 | Error / Warn 输出到标准错误，Info / Debug 输出到标准输出 |
| Android logcat | Android 编译环境下使用 `__android_log_print`，默认标签为 `alg` |
| 启动信息 | 记录启动时间、主机、进程和日志格式，区分多次运行记录 |
| 长度限制 | 单条消息超过缓冲上限时截断 |

未启用 `ALG_LOG_FILE` 时，SDK 日志宏直接调用 `fprintf`。所有等级都会编入，输出前先通过原子等级判断；被过滤的日志不会格式化参数或执行 I/O。默认等级为 Warn。

## 编译配置

在已配置平台工具链和后端依赖的构建目录中执行：

```bash
cmake .. -DALG_LOG_FILE=ON \
  -DALG_LOG_FILE_PATH=/data/log/alg.log \
  -DALG_LOG_FILE_MAX_SIZE=10485760 \
  -DALG_LOG_FILE_MAX_FILES=5 \
  -DALG_LOG_INFO=ON
```

`ALG_LOG_FILE_MAX_SIZE` 单位为字节。`ALG_LOG_INFO=ON` 和 `ALG_LOG_DEBUG=ON` 分别把启动默认等级改为 Info 和 Debug，不影响运行期可选等级。Android 构建可设置 `ALG_LOG_ANDROID=ON`，并由对应构建配置链接日志依赖。

## 运行期配置

集成方使用公共 C 接口动态控制日志等级：

```c
AlgSetLogLevel(ALG_LOG_INFO);   /* Error/Warn/Info */
AlgSetLogLevel(ALG_LOG_OFF);    /* 关闭全部 SDK 日志 */
```

接口可在 `AlgCreate` 前调用，也可以在运行期间调用，设置作用于同一动态库的全部实例。非法等级返回 `ALG_E_INVALID_ARG`。异步模式下，修改前已经入队的消息仍可能完成输出。

日志模块还提供独立的内部 C++ 配置入口，声明见 [alg_log.h](alg_log.h)，用于配置文件路径和 sink：

```cpp
#include "core/log/alg_log.h"

void ConfigureSdkLog()
{
    alg::log::Config config;
    config.file_path = "/data/log/alg.log";
    config.max_file_size = 10 * 1024 * 1024;
    config.max_files = 5;
    config.to_console = false;
    config.level = alg::log::Level::Info;
    alg::log::init(config);
}

void ShutdownSdkLog()
{
    alg::log::shutdown();
}
```

部署前应确认日志目录存在且具备写入权限。`init` 可重新配置日志模块；未显式调用时，首次写入采用默认配置初始化。应用结束时可调用 `shutdown` 刷新日志并停止后台线程；模块单例析构时也会执行关闭操作。

`set_level` 调整内部运行期过滤级别，`flush` 请求刷新日志。公共集成应优先使用 `AlgSetLogLevel`。

## 默认参数

| 宏 | 默认值 | 含义 |
|----|--------|------|
| `ALG_LOG_FILE_PATH` | `"alg_sdk.log"` | 文件基础路径 |
| `ALG_LOG_FILE_MAX_SIZE` | `10485760` | 单文件大小阈值，单位为字节 |
| `ALG_LOG_FILE_MAX_FILES` | `5` | 文件保留数量配置 |
| `ALG_LOG_QUEUE_SIZE` | `1024` | 队列槽位数 |
| `ALG_LOG_MSG_MAX` | `1024` | 单条消息缓冲字节数 |

路径、文件大小和保留数量可通过同名 CMake 参数设置；队列及消息缓冲上限的宏定义见 [alg_log.h](alg_log.h)，队列大小还可通过 `Config.queue_size` 调整。
