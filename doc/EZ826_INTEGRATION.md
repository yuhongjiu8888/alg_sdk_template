# 亿智 EZ826 / JC371 接入说明

本适配面向 `ALG_BACKEND=ez`，使用 EEasyTech NDK 2.0 的 OpenVX / NN API。相机 NV21 输入、全图缩放、固定 ROI 和检测后子模型裁剪均走亿智图像接口；EZ 生产动态库不依赖 OpenCV。

当前服务器按要求未执行编译。以下构建命令供具备交叉工具链的集成环境使用；性能和精度需在 EZ826 / JC371 板端验证。本仓库没有配套的亿智业务模型，`resources/config/ez/` 是转换后使用的模板。

## 参考依据

- `EEasyTech_NPU_API_Ref_V826.pdf`：模型创建、输入导入、图执行、设备内存、原始输出导出及 Image Processing API。
- `examples/826_JC371/inference/sample.cpp`：`LoadNetModel`、`CreateNetGraph`、输入输出查询、`vxProcessGraph` / `vxFinish` 和资源释放。
- `examples/826_JC371/img-process/img.cpp`：`AllocDeviceImageBuffer`、`ImageConvert`、物理地址输入及缓存刷新。
- `examples/826_JC371/in-out-raw-data/sample.cpp`：原始输出导出及硬件张量布局处理。

文档与 sample 位于本次任务给出的 `/home/yuwei/work_dir/test/ez/` SDK 目录；部署不需要把该目录或 PDF 打包到设备上。

## 数据路径和 CPU 开销

```text
相机 NV21 / NV12 的 Y、VU / UV 平面
  ├─ 物理地址、stride 满足硬件要求：直接供硬件读取
  └─ 普通虚拟内存或布局不满足要求：逐行复制到可复用设备 staging
          ↓
ImageConvert：裁剪 + 缩放 → BGRA_HWC 目标缓冲的有效区域
          ↓
ImportNetInputDataFromImage：同尺寸导入网络
          ↓
vxProcessGraph + vxFinish
          ↓
ExportNetOutputRawData：导出密集 NCHW 浮点张量
          ↓
现有后处理、NMS 和结果合并
```

`NV21` 对应 SDK 的 `YVU420_SP`，`NV12` 对应 `YUV420_SP`。不要只改 JSON 而沿用错误的相机色度顺序。

缩放和裁剪由 `ImageConvert` 的显式输入、输出矩形控制。NPU API 文档中 `ImportNetInputDataFromImage` 的 `crop_out_*` 已不需要设置，不能仅按 sample 对这些字段赋值来实现任意 letterbox。

设备 staging 和目标图像缓冲由模型实例持有并复用。无源物理地址时仍有逐行拷贝；同一帧中首个模型复制并刷新一次源图，后续全图模型和 ROI 子模型在源指针、尺寸、stride、格式、长度和布局匹配时共享这份设备输入，避免重复复制整帧。共享描述在每次 `AlgRun` 开始时重置，设备内存仍归原模型实例所有，不缓存上一帧像素。

补边初始化和输出导出也会消耗 CPU，因此硬件前处理不代表整个 `AlgRun` 零 CPU。现有后处理继续在 CPU 上运行。BGRA_HWC 的硬件目标横坐标要求 16 对齐；居中补边或越界 ROI 的有效区不满足时，硬件先输出到 scratch，再按行复制小尺寸 BGRA 有效区到最终画布，保留原始裁剪几何。

## 相机帧接入

相机集成建议使用分平面描述，明确填写两块内存的虚拟映射、物理地址和字节 stride。`data` 必须为空，避免进入连续内存解析分支：

```c
#include "alg_interface.h"

/* y_virt / vu_virt、物理地址、stride 和长度从相机 SDK 获取。
 * 长度应覆盖各平面的 stride × 行数；这里以 1920×1080 为例。 */
AlgImage image = {0};
image.format = ALG_PIX_NV21;
image.width = 1920;
image.height = 1080;
image.plane_data[0] = y_virt;
image.plane_data[1] = vu_virt;
image.plane_phys_addr[0] = y_phys;
image.plane_phys_addr[1] = vu_phys;
image.plane_stride[0] = y_stride;
image.plane_stride[1] = vu_stride;
image.plane_data_len[0] = y_stride * image.height;
image.plane_data_len[1] = vu_stride * (image.height / 2);

AlgResult result = {0};
AlgStatus status = AlgRun(handle, &image, &result);
/* 此后才能释放或归还相机帧，并按 status 判断 result 是否可用。 */
```

硬件直接读取的条件是：Y 与 VU / UV 均提供有效物理地址，两平面 stride 相同且为 32 的倍数。stride 可以大于有效图宽；SDK 以 stride 描述硬件存储宽度，以图像 width 描述有效区。图像宽高必须为偶数。条件不满足时从虚拟映射逐行合并到对齐的设备 staging，再用硬件缩放和转色；不会退回 OpenCV。

只有虚拟连续内存的测试帧可使用 `data`、`stride`、`data_len`：色度平面从 `data + stride * height` 开始，两个平面使用同一 stride。想让相机物理帧直接供硬件读取时，应使用上面的分平面方式。

调用方负责保证：

1. 相机生产已完成，输入平面直到同步 `AlgRun` 返回前有效，且不被覆盖。
2. 物理地址属于亿智图像硬件可访问的设备内存；虚拟映射与所填物理地址对应。
3. 缓存相干符合相机 SDK 约定。外部内存由调用方管理，SDK 不持有其内存句柄，无法替外部 CPU 写入执行 cache flush；需要 staging 读取时，CPU 映射也必须能看到生产者的最新数据。
4. 同一句柄的 `AlgRun` 串行调用。帧描述结构必须零初始化；长度和 stride 使用字节数。

`solution.target_fps` 默认 `0`。需要持有相机帧直至推理完成时，建议在应用取帧前控制节奏；若启用 SDK 内部限速，等待也发生在 `AlgRun` 内，会延长帧的持有时间。

## 前处理与配置

模板位于 [resources/config/ez](../resources/config/ez/README.md)。`engine: hardware` 明确要求硬件前处理，`auto` 也会选择亿智硬件接口；仅支持这两种前处理引擎。`aipp` 为海思专用，亿智配置使用它会在初始化时被拒绝。

本实现面向 NV21 / NV12。BGR、RGB、GRAY 相机输入在此后端不支持，不会静默转成 CPU 路径。`input_format` 可设置为 `NV21`、`NV12` 或 `auto`；固定格式与实际输入不符会报错。`max_input_size` 是允许的源图尺寸上限，应覆盖相机分辨率。

硬件中间图为 `BGRA_HWC`。运行期配置采用 `color: BGR`、`mean=[0,0,0]`、`std=[1,1,1]`、`scale=1`，颜色交换、归一化和量化由转换后的网络负责。模板中的原业务网络数学输入仍为 RGB，转换模型时必须固化 BGR 到 RGB 的通道交换，避免二次交换或遗漏。

`input_size` 必须等于模型完整输入尺寸。BGRA_HWC 设备缓冲宽度按 16 对齐，导入网络时裁出有效宽度，不要求修改模型尺寸。硬件目标有效区横坐标未对齐时走上面的小图 scratch 复制路径。

固定 `stage.roi` 和 `objects_from:` 子模型直接从原 NV21 / NV12 帧读取；`crop.expand_ratio`、`crop.square` 和补边由共享裁剪几何决定。子模型沿用硬件路径，不生成整幅 BGR 裁剪图。补边和几何边界的具体对齐行为以实现日志和板端结果为准，需重点比对靠近画面边缘的目标。

海思配置中的 `aipp_output_size` / `aipp_graph_padding` 不适用于本路径。亿智模板按完整网络输入做前处理，不应使用已经固化了海思补边的 ONNX 再重复补边。

## 模型转换和后处理约定

模板采用 `.ezb` 定义文件和同目录、同名 `.bin` 权重。`model_path` 指定 `.ezb`；后端按照官方 sample 的加密模型加载方式读取这对文件，也支持 SDK 的 `.eezb` 合并模型文件。示例文件清单见 [模型目录](../resources/model/ez/README.md)。

转换时应确认以下事项：

- 使用与目标 EZ826 / JC371、NDK 和板端运行库相匹配的官方转换工具。
- 保留本项目后处理期望的原始输出头、名称、顺序和数值语义；不能擅自加入 sigmoid、softmax、NMS 或改变分类字符顺序。
- 模型输入使用图像导入可处理的约定，并将原业务网络所需的颜色交换、mean / std / scale 固化到转换产物。与原 ONNX / CPU 推理逐项比对输入样本和输出结果。
- 转换得到的图像输入尺寸与 JSON 中 `input_size` 完全一致；限速检测 576×320、OCR 96×96、车牌检测 640×448、识别 256×64。
- 输出通过 SDK 原始导出接口转为密集 NCHW float 后提供给现有后处理，避免把 NPU pack 内存直接当连续数组读取。

官方 YOLOv5n 为三尺度、每尺度三 anchor 的 COCO 80 类检测器；本项目 `yolov5_anchor_det` 是业务单尺度单 anchor 头，类别分支也有不同语义。官方 YOLOv8n 同样没有对应的现成后处理。官方模型适合验证 SDK 自身 demo，不能直接替换业务模板中的模型来证明业务链路正确。

## 构建与部署

构建集成位于 `cmake/CMakeLists_linux_ez.cmake` 和 `toolchain/ez_linux.toolchain.cmake`。EZ826 / JC371 为 Linux ARM32 平台，使用 `arm ez` 构建和打包入口及 ARM 32 位 hard-float 工具链，发布包和应用必须匹配该 ABI。

构建环境需要解压官方 `826(823)-ndk.zip`，使用其中 Linux / MDP 的头文件和运行库。不能把 NDK 压缩包目录直接作为包含 `VX/vx.h` 和 `libnn/net_api.h` 的 SDK 根目录，也不能混用 822、RTOS 或其他芯片的库。

先编辑以下文件中的集中配置，再在具备工具链的环境中执行构建命令，无需附加路径参数：

| 文件 | 配置项 |
|---|---|
| `cmake/CMakeLists_linux_ez.cmake` | `EZ_NDK_ROOT`：解压后的 `ndk-826` 根目录 |
| 同上 | `EZ_NDK_LIB_DIR`：NDK 动态库目录 |
| 同上 | `EZ_PLATFORM_LIB_DIR`：板端 SDK 的平台库目录 |
| 同上 | `EZ_PLATFORM_LIBS`：MDP 默认 `log;mbase;ax`，其他框架按平台调整 |
| 同上 | `ALG_BUILD_EZ_RUNNER`：默认开启纯 NV21 验证程序，改为 `OFF` 仅构建 SDK |
| `toolchain/ez_linux.toolchain.cmake` | `EZ_TOOLCHAIN_DIR`、`EZ_TOOLCHAIN_PREFIX`、`EZ_SYSROOT`：ARM32 工具链与 sysroot |

```bash
./build.sh linux arm ez
./builddeploy.sh arm ez
```

脚本不接收额外 CMake 路径参数。SDK 和工具链路径由上述文件直接设置，不从同名环境变量读取。EZ 默认不构建依赖 OpenCV 的图片版 `test_runner` / `loop_runner`，使用独立的 `ez_nv21_runner`。CMake 生成 `ez_runtime_libraries.txt` 记录本次选择的 NDK 运行库，打包脚本据此收集动态库。

将配置和业务模型分别放入 `resources/config/ez/`、`resources/model/ez/` 后，执行 `./builddeploy.sh arm ez`。打包脚本原样拷贝这两个目录及其子目录，不解析 JSON、不检查模型配对，也不修改 `model_path`。模型文件和路径由集成方配置。部署需携带 `libalg_sdk.so`、公共头文件、JSON、全部模型及相匹配的亿智运行库；NPU 驱动和设备内存设施由平台环境提供。

在板端从仓库一级构建目录运行原始 NV21 文件：

```bash
cd build_linux_arm_ez
LD_LIBRARY_PATH=.:/opt/ez/lib ./ez_nv21_runner \
  ../resources/config/ez/all.json /data/frame.nv21 1920 1080 1920 100
```

参数依次为 `config.json frame.nv21 width height [stride] [iterations]`。文件必须恰好包含一帧 `stride × height × 3/2` 字节，重复运行使用同一帧；输出报告墙钟和线程 CPU 时间。此程序使用普通虚拟内存，验证的是 staging 路径；相机物理帧直读应在实际相机集成应用中验证。

部署时按进程工作目录填写配置中的 `model_path`，或使用设备上的绝对路径。保留模板中的 `../resources/model/ez/` 时，可从测试包根目录下的 `run/` 子目录启动：

```bash
mkdir -p run
cd run
LD_LIBRARY_PATH=..:../lib ../ez_nv21_runner \
  ../resources/config/ez/all.json /data/frame.nv21 1920 1080 1920 100
```

板端 SDK 的平台依赖库也应能被动态加载器找到。

## 板端验收

本次未编译、未运行 NPU。集成环境需要完成：

- 先运行同版 SDK 的官方 inference / img-process demo，确认驱动、动态库、设备内存和模型版本匹配。
- 用同一 NV21 样本对照原业务网络，检查输入颜色、归一化、输出头顺序及检测坐标。
- 对比紧凑输入、带 stride 输入、物理双平面输入，覆盖固定 ROI、越界 / 正方形扩展、letterbox 和无目标帧。
- 长时间运行并观察内存，确认相机帧归还及时、设备缓冲复用正常；在相同帧率与日志级别下测量 CPU、前处理和推理耗时。

生产日志建议使用 Warn；Info 下模型日志的 `pre/pre_cpu`、`infer/infer_cpu` 和 `post/post_cpu` 可帮助定位 CPU 时间与硬件等待。当前没有板端性能数据，不承诺具体帧率或 CPU 降幅。
