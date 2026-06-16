# alg_log —— 异步多 sink 日志模块（借鉴 spdlog）

给端侧 SDK 在不改动现有 `ALG_LOGE/W/I/D` 调用接口的前提下，增加**异步落盘**能力。

## 解决什么问题
原来日志只走 `fprintf` 到终端：单次 `fprintf` ~10–50µs（系统调用 + locale + 流锁），
**单行太长时还会阻塞算法热路径**。本模块把"格式化"留在调用线程、把"真正的 write/fflush"
甩到后台线程，并对单行做长度上限截断，从根上消除这个阻塞。

## 设计
- **异步**：调用线程 `vsnprintf` 进定长环形缓冲槽位即返回；后台 worker 线程批量落盘。
  队列满时**丢弃 + 计数**（绝不阻塞推理），并周期性打一条 `log queue overflow: N dropped`。
- **多 sink**：滚动文件 / 控制台 / Android logcat，任意组合。
  - 文件：单文件写满 `max_file_size`（默认 **10MB**）即滚动，循环保留 `max_files`（默认 5）份：
    `alg.log, alg.log.1, … alg.log.N`（`.N` 最旧，被覆盖）。
  - 控制台：沿用旧行为，`E/W → stderr`，`I/D → stdout`。
  - Android：`__ANDROID__` 下用 `__android_log_print`，tag 默认 `alg`。
- **零默认开销**：不开 `ALG_LOG_FILE` 时，`ALG_LOGE/W/I/D` 行为与改动前逐字节一致
  （直接 `fprintf`，`I/D` 仍编译期消除），且不链接本模块。

## 开启（编译期）
```bash
cmake .. -DLINUX_AARCH64=ON -DALG_BACKEND=xmm \
         -DALG_LOG_FILE=ON \
         -DALG_LOG_FILE_PATH=/data/log/alg.log \
         -DALG_LOG_FILE_MAX_SIZE=10485760 \   # 单文件上限(字节)
         -DALG_LOG_FILE_MAX_FILES=5 \         # 滚动保留份数
         -DALG_LOG_INFO=ON                    # 想把 info 也落盘才加；debug 用 -DALG_LOG_DEBUG=ON
# Android NDK 构建时加 -DALG_LOG_ANDROID=ON（自动链接 liblog）
```

## 运行期覆盖（路径常在部署时才确定）
```cpp
#include "core/log/alg_log.h"
alg::log::Config c;          // 缺省值来自上面的编译期 -D
c.file_path     = "/data/log/alg.log";
c.max_file_size = 10 * 1024 * 1024;
c.max_files     = 5;
c.to_console    = false;     // 板端通常关掉终端输出
c.level         = alg::log::Level::Info;
alg::log::init(c);
// ... 正常用 ALG_LOGE/W/I/D ...
alg::log::shutdown();        // 进程退出前刷盘（单例析构时也会自动做）
```

## 可调宏（`-D` 覆盖，默认值见 `alg_log.h`）
| 宏 | 默认 | 含义 |
|---|---|---|
| `ALG_LOG_FILE_PATH` | `"alg_sdk.log"` | 文件基路径 |
| `ALG_LOG_FILE_MAX_SIZE` | `10485760` | 单文件字节上限 |
| `ALG_LOG_FILE_MAX_FILES` | `5` | 滚动保留份数（循环覆盖） |
| `ALG_LOG_QUEUE_SIZE` | `1024` | 环形槽位数（×单行上限 ≈ 内存占用） |
| `ALG_LOG_MSG_MAX` | `1024` | 单行字节上限，超长截断 |
