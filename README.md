# alg_sdk 项目说明

alg_sdk 是面向车载及嵌入式设备的视觉推理 SDK，负责将检测、识别和结果处理封装为统一的 C 接口。项目将芯片适配、模型后处理和业务编排分层实现，用于复用不同设备上的算法接入流程，集中维护模型配置及部署资源。

## 功能范围

| 业务 | 实现方案 | 交付接口 |
|------|----------|----------|
| 巴西限速牌识别 | SpeedSignNet 检测与逐位 OCR 识别，覆盖 10～120 km/h、间隔 10 的 12 类限速值 | `AlgResult.speed_limits[]` |
| 禁令 / 停车牌识别 | PARE 由检测阶段输出；禁止停车由第二阶段门控分类 | `AlgResult.signs[]` |
| 车牌识别 | RTMDet 检测与 LPRNet CTC 识别，提供 SVP ACL 模型和配置 | `AlgResult.license_plates[]` |
| 红绿灯检测 | 保留 YOLOX 模型、后处理和配置，支持内部推理流程 | 未纳入公共结果结构 |

限速牌和车牌均采用检测后裁剪识别的处理流程，识别结果写回原检测框。业务配置可组合多个模型，执行顺序由 `solution.stages` 确定。`all.json` 表示组合业务配置，具体包含的业务按后端区分，不表示模型并发执行。

## 平台适配

| 后端 | 使用环境 | 模型格式 | 实现状态 |
|------|----------|----------|----------|
| `xmm` | XMM 板端运行环境 | `.xmm` | 提供模型加载、推理和内存管理实现 |
| `svp_acl` | 海思 v610 / SVP ACL | `.om` | 提供模型加载、推理及辅助缓冲管理实现 |
| `mnn` | x86_64 / MNN | `.mnn` | 提供本地推理实现 |
| `rk` | Rockchip RKNN | — | 保留接口桩，模型加载返回 `ALG_E_BACKEND` |

一份动态库在编译时选择一个后端。模型文件、运行依赖和目标架构须与动态库配套。SVP ACL 后端管理模型所需的 `task_buf`、`work_buf`，这些辅助缓冲不作为业务图像输入暴露给上层。

## 工程结构

```text
include/                  公共 C API 和数据类型
resources/
  config/<backend>/       按后端组织的业务配置
  model/<backend>/        与配置配套的模型文件
  CONFIG.md               配置字段说明
src/
  interface/              C API 实现和句柄管理
  core/
    config/               配置解析与引用检查
    instance/             单模型前处理、推理和后处理
    solution/             多阶段编排、ROI 裁剪和结果合并
    preprocess/           图像转换、缩放与张量写入
    postprocess/          后处理接口及通用 NMS
    registry/             后处理器注册
    log/                  日志输出与文件管理
    tensor.h              跨后端张量描述
    object.h / object.cpp 内部对象及公共结果转换
  backend/                XMM、SVP ACL、MNN 和 RK 适配
  models/                 检测及识别后处理器
cmake/                    平台构建配置
toolchain/                交叉编译工具链
test/                    图片验证和循环推理程序
backup/                   历史接口归档
```

芯片适配通过 `IInferer` 封装，模型输出解析通过 `IPostprocessor` 实现，已支持模型之间的业务组合由 JSON 配置描述。新增后端或模型类型还需要维护构建配置；新增公共结果类型需要同步调整头文件和结果转换。

## 公共接口

| 接口 | 职责 |
|------|------|
| `AlgCreate` | 读取配置、加载模型并创建实例 |
| `AlgRun` | 同步处理一帧图像并返回业务结果数组 |
| `AlgDestroy` | 销毁实例及其内部资源 |
| `AlgSetLogLevel` | 动态设置全局日志等级（Off/Error/Warn/Info/Debug） |
| `AlgVersion` | 返回 SDK 版本和后端标识 |
| `AlgBackendName` | 返回编译后端名称 |

调用流程为创建实例、循环处理帧、销毁实例。`AlgResult` 内联固定容量数组，建议使用 `AlgResult result = {0};` 在栈上创建；`AlgRun` 直接覆盖数量和有效元素，不需要额外释放。

日志默认等级为 Warn。`AlgSetLogLevel` 无需句柄，建议在 `AlgCreate` 前调用，也可在运行期间调整；设置作用于动态库中的全部实例。

完整函数声明、数据类型和调用示例见 [接口文档](alg_sdk_api_documentation_v3.0.0.md)。公共 API 不包含红绿灯、关键点和分割结果字段。

## 模型与配置

| 配置 | 处理内容 |
|------|----------|
| [xmm/speed_limit.json](resources/config/xmm/speed_limit.json) | 限速牌及禁令 / 停车牌 |
| [mnn/speed_limit.json](resources/config/mnn/speed_limit.json) | 限速牌及禁令 / 停车牌，本地推理 |
| [svp_acl/speed_limit.json](resources/config/svp_acl/speed_limit.json) | 限速牌及禁令 / 停车牌，海思平台 |
| [svp_acl/license_plate.json](resources/config/svp_acl/license_plate.json) | 车牌检测与识别 |
| [svp_acl/all.json](resources/config/svp_acl/all.json) | 限速牌、禁令 / 停车牌与车牌组合流程 |
| [xmm/all.json](resources/config/xmm/all.json)、[mnn/all.json](resources/config/mnn/all.json) | 内部红绿灯与限速牌组合流程；公共输出为限速牌及禁令 / 停车牌 |

限速牌检测输入为 576×320，识别输入为 96×96，均采用 RGB。OCR 支持单输出及多输出布局；三位数字加三路门控的单输出共 36 个元素。多输出模型通过张量名称和索引配置识别各输出头。

车牌检测输入为 640×448，采用 `letterbox_tl_fit`；识别输入为 256×64，采用居中 letterbox。识别字符集为数字和大写英文字母，使用 CTC greedy 解码。

`classify_into` 将识别分乘到检测分，并过滤未通过识别的对象；PARE 通过 `passthrough` 保留检测分。各模型阈值、裁剪范围、最小框尺寸及归一化参数由配套配置定义，参数明细见 [配置说明](resources/CONFIG.md)。

配置在实例创建时加载，调整后需重建实例。配置中的相对模型路径以进程工作目录为基准；部署时可改为设备上的绝对路径。输入尺寸和归一化必须符合模型约定，固定尺寸模型不能仅通过修改 JSON 改变输入形状。

## 构建与运行

在仓库根目录选择对应构建命令：

```bash
./build.sh linux aarch64 xmm
./build.sh linux aarch64 svp_acl
./build.sh linux x86_64 mnn
```

默认产物位于 `build_linux_<arch>_<backend>/`，包含动态库、`test_runner` 和 `loop_runner`。构建脚本会清理并重建对应目录，交付资料和测试数据应存放在构建目录之外。

海思构建沿用脚本中的 `aarch64` 入口名称，实际采用 ARM 32 位 `arm-linux-musleabi` 工具链。平台依赖见 [板端构建配置](cmake/CMakeLists_linux_aarch64.cmake)、[本地构建配置](cmake/CMakeLists_linux_x86_64.cmake) 和 [海思工具链](toolchain/hisi_v610_linux.toolchain.cmake)。JSONCPP 源码随工程编入。

海思环境的工具链及依赖路径可通过 CMake 参数 `HISI_TOOLCHAIN_ROOT`、`SVP_ACL_ROOT`、`SVP_ACL_LIB_DIR`、`HISI_SECUREC_LIB_DIR` 和 `SVP_OPENCV_ROOT` 配置。
动态 AIPP 默认通过 `ALG_SVP_DYNAMIC_AIPP=ON` 编译；需要兼容不含动态 AIPP API 的旧版 ACL 头文件时可显式关闭。
分平面 NV12/NV21 的缩放与合并使用 `third_party/libyuv/hi3516`，可通过 `LIBYUV_ROOT` 覆盖头文件和静态库目录。

在匹配的目标设备或本地运行环境中，从仓库一级构建目录启动验证程序：

```bash
cd build_linux_aarch64_svp_acl
./test_runner ../resources/config/svp_acl/all.json /data/test.jpg out
./loop_runner ../resources/config/svp_acl/all.json /data/test.jpg -n 1000
./loop_runner ../resources/config/svp_acl/license_plate.json /data/frame.yuv \
  -size 1920x1080 -fmt nv12 -n 1000
./loop_runner ../resources/config/svp_acl/all.json /data/frame.yuv \
  -size 1920x1080 -fmt nv21 -split-plane -n 1000
```

`test_runner` 支持图片或图片目录，`loop_runner` 支持图片和原始 YUV 帧循环输入。上述相对路径要求部署目录保留构建目录与 `resources` 的同级关系。

## 接入与交付约束

- 图像格式支持 BGR、RGB、GRAY、NV12 和 NV21。NV12/NV21 既可通过 `data` 传连续内存，也可通过 `plane_data[0]` 和 `[1]` 分别传 Y 与 UV/VU，分平面支持独立 stride。默认 VPSS 格式为 NV21，同时兼容 NV12。
- 海思通过 `AlgRun` 传入原分辨率 VPSS 帧，不要求为各模型增加 VPSS 缩放通道。
- 带动态 AIPP 的检测 OM 在 `engine=auto` 时完成 NV12/NV21 转色；连续输入由 AIPP 缩放，分平面输入由 libyuv 缩放并合并。值为 114 的 letterbox 补边固化在模型图中，普通 OM 保持 OpenCV 路径。
- 网络前处理输出 NCHW，支持 UINT8 和 FLOAT32 输入类型，输入尺寸须与模型一致。
- SDK 调用由应用串行调度；共享结果和图像缓冲的访问需要同步。
- 车牌空文本仍可能返回结果，应用需结合非空文本和业务格式判断有效性。
- 每次交付应配套提供头文件、动态库、配置、模型及目标运行依赖。实际性能和识别效果以对应设备、模型和测试数据的验证记录为依据。

## 文档索引

| 文档 | 用途 |
|------|------|
| [接口文档](alg_sdk_api_documentation_v3.0.0.md) | 公共接口、数据类型、资源管理和部署约束 |
| [架构设计](ARCHITECTURE.md) | 模块职责、数据流和扩展方式 |
| [开发接入指南](NEWCOMER_GUIDE.md) | 工程接入、配置选择和问题定位 |
| [配置说明](resources/CONFIG.md) | 业务编排和模型参数 |
| [日志模块](src/core/log/README.md) | 日志编译选项及运行期配置 |
| [红绿灯接口归档](backup/traffic_light_revert/README.md) | 历史公共接口与恢复流程 |
