# alg_sdk 开发接入指南

本指南说明 alg_sdk 的工程接入流程、主要模块和问题定位方法，适用于应用集成和后续维护。完整接口定义见 [接口文档](alg_sdk_api_documentation_v3.0.0.md)，设计说明见 [架构文档](ARCHITECTURE.md)。

## 1. 接入范围

SDK 通过六个 C 函数完成实例管理、同步推理和日志控制，公共结果包含限速牌、禁令 / 停车牌和车牌。红绿灯模型及处理流程保留在工程内部，未纳入公共结果结构。

应用接入需要准备目标架构、后端运行库、配套模型和业务配置。XMM、SVP ACL（Hi3516CV610 / Hi3519DV500）和 MNN 提供推理实现。

## 2. 构建和验证

在仓库根目录选择对应平台：

```bash
./build.sh linux aarch64 xmm
./build.sh linux aarch64 svp_acl_cv610
./build.sh linux aarch64 svp_acl_dv500
./build.sh linux x86_64 mnn
```

构建目录默认为 `build_linux_<arch>_<backend>/`。脚本会清理对应目录后重新构建，测试数据及交付文件应保存在目录之外。海思构建参数沿用 `aarch64` 名称：`svp_acl_cv610` 实际工具链为 ARM 32 位 `arm-linux-musleabi`，`svp_acl_dv500` 为 aarch64。

各后端的配置与模型分目录存放（`resources/config/svp_acl/<chip>/`、`resources/model/svp_acl/<chip>/`），
`<chip>` 为 `cv610`（CV610）或 `dv500`（DV500）。

程序在匹配的运行环境中执行。以 DV500 构建目录为例：

```bash
cd build_linux_aarch64_svp_acl_dv500
./test_runner ../resources/config/svp_acl/dv500/speed_limit.json /data/test.jpg out
./test_runner ../resources/config/svp_acl/dv500/license_plate.json /data/plate.jpg out
./loop_runner ../resources/config/svp_acl/dv500/all.json /data/test.jpg -n 1000
```

CV610 把上述路径中的 `dv500` 换成 `cv610`、构建目录换成 `build_linux_aarch64_svp_acl_cv610`。

`test_runner` 支持图片或图片目录，输出结果明细和标注图片。`loop_runner` 用于连续推理验证，可使用 `-n` 指定循环次数，使用 `-size`、`-fmt` 指定原始 YUV 帧的尺寸和格式。

配置中的模型相对路径以进程工作目录为基准。默认的 `../resources/model/...` 路径要求程序从仓库一级构建目录运行；设备部署目录不同时，应同步调整配置或改用绝对路径。

## 3. 公共接口与资源管理

| 接口 | 调用要求 |
|------|----------|
| `AlgCreate` | 传入配置路径和句柄输出指针，检查返回状态后使用句柄 |
| `AlgRun` | 同步处理一帧；输入缓冲在返回前保持有效 |
| `AlgDestroy` | 释放有效句柄，应用随后将句柄变量设为 `NULL` |
| `AlgSetLogLevel` | 无需句柄，动态设置 SDK 全局日志等级 |
| `AlgVersion` | 获取版本字符串，由 SDK 持有 |
| `AlgBackendName` | 获取编译后端名称，由 SDK 持有 |

`AlgResult` 使用固定容量内联数组，建议在栈上零初始化。`AlgRun` 会覆盖数量和有效元素，无需释放；结构体复制也是完整的值复制。

下面示例用于单帧接入验证，输入图像由调用方准备。连续采集场景应将创建和销毁移到帧循环之外，复用同一句柄。

```c
#include <stdio.h>
#include "alg_interface.h"

AlgStatus RunImage(const char* config_path, const AlgImage* image)
{
    AlgHandle handle = NULL;
    AlgResult result = {0};
    AlgStatus status;

    if (!image) return ALG_E_INVALID_ARG;
    status = AlgSetLogLevel(ALG_LOG_WARN);
    if (status != ALG_OK) return status;
    status = AlgCreate(&handle, config_path);
    if (status != ALG_OK) return status;

    status = AlgRun(handle, image, &result);
    if (status == ALG_OK) {
        printf("frame=%lld, speed_limits=%d, signs=%d, license_plates=%d\n",
               result.frame_id, result.speed_limit_count,
               result.sign_count, result.license_plate_count);
        for (int i = 0; i < result.speed_limit_count; ++i) {
            const AlgSpeedLimit* item = &result.speed_limits[i];
            if (item->value != SLV_INVALID)
                printf("limit=%d km/h, score=%.3f\n",
                       (int)item->value * 10, item->box.score);
        }
        for (int i = 0; i < result.sign_count; ++i) {
            const AlgSign* item = &result.signs[i];
            printf("sign=%d, score=%.3f\n", (int)item->type, item->box.score);
        }
        for (int i = 0; i < result.license_plate_count; ++i) {
            const AlgLicensePlate* item = &result.license_plates[i];
            if (item->text[0] != '\0')
                printf("plate=%s, score=%.3f\n", item->text, item->box.score);
        }
    }

    {
        AlgStatus destroy_status = AlgDestroy(handle);
        handle = NULL;
        if (status == ALG_OK) status = destroy_status;
    }
    return status;
}
```

应用侧应串行调用 SDK。同一句柄复用内部执行状态，不同句柄还共享非原子帧计数器，多个句柄之间也不保证线程安全。

## 4. 图像和结果约定

| 内容 | 接入要求 |
|------|----------|
| BGR / RGB | 8 位交错三通道数据，`stride` 为每行字节数，0 表示 `width*3` |
| GRAY | 8 位单通道数据，`stride=0` 表示 `width` |
| NV12 / NV21 连续模式 | `data` 指向 Y，UV/VU 紧跟在 `data + stride*height`；海思动态 AIPP 要求该地址为整帧非 cached MMZ/VB 连续映射 |
| NV12 / NV21 分平面模式 | `data=NULL`，`plane_data[0]` 指向 Y，`plane_data[1]` 指向 UV/VU，可用独立 stride；海思生产输入同时填写对应的 `plane_phys_addr[0/1]` |
| 长度字段 | `data_len` 或 `plane_data_len[]` 大于 0 时 SDK 校验最小长度；0 表示未知 |
| 检测坐标 | 对应传入原图，SDK 负责恢复配置 ROI 和裁剪产生的偏移 |
| 限速值 | `SLV_10`～`SLV_120` 的枚举值乘 10 得到 km/h |
| PARE 分数 | 第一阶段检测分 |
| 禁停分数 | 检测分乘门控概率 |
| 限速分数 | 检测分乘 OCR 分类分 |
| 车牌分数 | 检测分乘 `rec_score`；有效车牌还需检查文本和业务格式 |
| `frame_id` | `AlgRun` 为动态库内共享计数 |

图片文件应先解码为像素再传入。JSON 的 `preprocess.color` 定义模型输入颜色顺序，`AlgImage.format` 定义源图像格式，两者不要求相同。

海思 VPSS 实时流使用 `AlgRun`：连续 NV12/NV21 的 `data` 直接绑定动态 AIPP；Y 与 UV/VU 地址分离时，同时传入两个物理地址，由 IVE 缩放并写入 SDK 复用的连续 MMZ，随后零拷贝绑定 ACL。普通内存测试数据没有物理地址时才走 libyuv 回退。IVE/AIPP/模型图分别承担缩放、CSC/归一化和 padding，不需要额外创建模型尺寸的 VPSS 通道。调用方输入在 `AlgRun` 返回前须保持有效。

DV500 目标没有 IVE，且它的 AIPP 不能负责缩放（大图缩放会固定多出约 128ms/帧）。
送进 `AlgRun` 的帧需要已经是配置里的 `aipp_output_size`：默认由 SDK 内部用 libyuv 预缩放
（`ALG_AIPP_SCALE_AUTO`，内部先做精确 1/2 折半，同一帧内多个全图模型的中间帧会共享），
也可以由 VPSS 直接出这个尺寸的通道并切到 `ALG_AIPP_SCALE_EXTERNAL` 把 CPU 开销降到零。

检测后的分类/识别子模型在 NV12/NV21 源上直接从原图 ROI 缩放/转色，不再生成整张 BGR
裁剪图（`-log info` 中带 `(fused_roi)`）。帧率策略可以在 `solution.target_fps` 中配置
（`0` = 不限速）；`AlgRun` 是同步接口，SDK 会在内部限速，集成方不需要自行 `usleep`。

## 5. 配置与执行关系

业务配置分为 `solution` 和 `models`。前者描述阶段顺序和数据来源，后者描述模型文件、前处理参数和后处理类型。

限速牌的阶段配置片段如下，完整配置还需包含对应的 `models`：

```json
{
  "type": "chain",
  "stages": [
    {
      "name": "detector", "model": "ssn_cfg",
      "input": "image", "produces": "objects"
    },
    {
      "name": "classifier", "model": "cls_cfg",
      "input": "objects_from:detector",
      "crop": { "expand_ratio": 1.5, "square": true, "pad_value": 114 },
      "score_threshold": 0.6,
      "passthrough": [
        { "label": 1, "category": "pare", "sign_value": 2, "min_score": 0.0 }
      ],
      "min_box_short": { "no_parking": 40 },
      "produces": "classify_into:detector"
    }
  ]
}
```

`image` 表示原图输入；`objects_from:detector` 表示依次裁剪检测框；`classify_into:detector` 将识别类别和分数写回原检测框。未通过识别的框被标记为丢弃，PARE 命中透传规则后跳过 OCR。完整参数见 [配置说明](resources/CONFIG.md)。

一次调用的内部执行顺序为：

1. `LoadSolutionConfig` 解析配置，`ChainSolution::Init` 建立模型和阶段引用。
2. `ModelInstance::Init` 加载后端模型并配置前后处理。
3. `ChainSolution::Run` 顺序执行阶段，按输入格式和 ROI 需求选择图像转换及裁剪路径。
4. 每个模型依次执行 `pre_->Apply`、`inferer_->Forward` 和 `post_->Apply`。
5. 编排层执行识别写回与过滤，`FillAlgResult` 转换为公共数组。

## 6. 模块维护入口

| 文件或目录 | 维护内容 |
|------------|----------|
| [alg_interface.h](include/alg_interface.h)、[alg_types.h](include/alg_types.h) | 公共接口和数据结构 |
| [alg_interface.cpp](src/interface/alg_interface.cpp) | 句柄、结果转换和帧号 |
| [chain_solution.cpp](src/core/solution/chain_solution.cpp) | 阶段执行、裁剪及结果写回 |
| [model_instance.cpp](src/core/instance/model_instance.cpp) | 单模型初始化和执行 |
| [config.cpp](src/core/config/config.cpp) | 配置字段及引用校验 |
| [letterbox_preprocessor.cpp](src/core/preprocess/letterbox_preprocessor.cpp) | 图像前处理 |
| [object.cpp](src/core/object.cpp) | 公共结果类别映射 |
| [postprocessor_registry.cpp](src/core/registry/postprocessor_registry.cpp) | 后处理器注册和创建 |
| [模型后处理目录](src/models) | 检测、OCR 和车牌识别后处理 |
| [后端目录](src/backend) | 平台适配和后端资源管理 |

新增后端需维护工具链和链接依赖；新增模型类型需实现后处理并加入构建；新增公共结果类型需同时维护类型定义、转换和调用示例。具体流程见 [架构设计](ARCHITECTURE.md)。

## 7. 问题定位

按配置加载、模型加载、图像前处理、后处理和结果转换的顺序定位问题。首先记录 `AlgVersion()`、`AlgBackendName()`、配置路径和返回码，再检查对应阶段的日志。

无检测结果时，应分别检查检测阈值、识别阈值、联合分、ROI 和最小框过滤，并确认最终类别属于公共结果范围。模型成功加载不代表输入布局或模型配置正确。

日志级别和文件输出见 [日志说明](src/core/log/README.md)。修改模型或配置后，应保留测试环境、输入数据、结果和耗时记录，作为版本比较和交付依据。

## 8. 接口维护说明（内部）

本节承接客户接口文档中的版本演进、实现行为和平台排查细节，供内部维护使用。客户接入以 [接口文档](alg_sdk_api_documentation_v3.0.0.md) 中的调用约定为准。

### 8.1 历史版本记录

以下为原接口文档保留的发布记录。

| 版本 | 作者 | 日期 | 变更 |
|------|------|------|------|
| 1.0.0 | 喻伟 | 2026.07.17 | 初版接口文档：限速牌及停车牌识别 |
| 1.0.0（文档修订） | 喻伟 | 2026.09.07 | 完善公共接口参数、返回值及数据结构说明 |
| 2.0.0 | — | 2026.09.09 | 结果改为固定容量内联数组，移除 `AlgFreeResult` |
| 2.1.0 | — | 2026.09.10 | 新增运行期日志等级控制接口 `AlgSetLogLevel` |
| 3.0.0 | — | 2026.09.10 | `AlgImage` 新增独立图像平面，支持分地址 NV12/NV21 输入 |

公共结构体布局变化后，交付时须同步更新头文件和动态库，调用方须重新编译。客户接口文档不再列出历史函数删除和内部整改过程。

### 8.2 返回结果与日志实现

`AlgRun` 先检查顶层指针，再调用 `ResolveImageView` 校验图像布局；这两个阶段失败时不改写结果。通过校验后才清零三个结果数量，再执行流水线。流水线失败时数量为 0，数组存储内容不保证清空，`frame_id` 不更新。客户统一按“失败时不使用本次结果”处理，避免依赖具体失败阶段。

`frame_id` 使用动态库内共享的非原子计数器，从 0 开始，每次成功调用后递增，包含无目标帧；销毁或重建句柄不会重置。并发约束见第 3 节。

`AlgSetLogLevel` 和 `AlgSetAippScaleMode` 的全局状态读写使用原子操作。日志启动等级可由构建选项覆盖；异步模式下，修改等级前已入队的消息仍可能完成输出。Debug 日志会执行额外的张量诊断统计。构建选项和队列行为见 [日志说明](src/core/log/README.md)。

`ALG_E_MODEL_NOT_FOUND` 表示所需后处理类型未注册。模型文件缺失不统一返回此码，应结合实际后端错误和加载日志定位。车牌的 `rec_score` 是 CTC 解码保留字符概率的乘积；空文本候选在识别阶段丢弃，超过公共文本容量的内容在结果转换时截断。

### 8.3 海思动态 AIPP 内存路径

连续输入直接借用 `data` 绑定 ACL data buffer，不复制、不执行 CPU cache flush，也不释放调用方地址。需要确认：

- Y 与 UV/VU 属于同一个 MMZ/VB 物理内存块，`phy_uv == phy_y + stride*height`，共用行跨度。
- 使用 `ss_mpi_sys_mmap(phy_y, total_bytes)` 映射整帧非 cached 连续区域，其中 `total_bytes = stride*height*3/2`，并将映射长度填入 `data_len`。
- 映射在 `AlgRun` 返回前有效。同一 VB 缓冲可复用映射，释放或归还缓冲前解除映射，避免帧循环中重复 mmap/munmap。
- 普通堆内存中的 YUV 文件数据使用分平面输入。

分平面 NV21 描述示例，所用地址、尺寸和行跨度由平台提供：

```c
AlgImage image = {0};
image.format = ALG_PIX_NV21;
image.width = frame_width;
image.height = frame_height;
image.plane_data[0] = y_virtual_address;
image.plane_data[1] = vu_virtual_address;
image.plane_phys_addr[0] = y_physical_address;
image.plane_phys_addr[1] = vu_physical_address;
image.plane_stride[0] = y_stride;
image.plane_stride[1] = vu_stride;
image.plane_data_len[0] = y_stride * frame_height;
image.plane_data_len[1] = vu_stride * (frame_height / 2);
```

CV610 启用 IVE 且两个物理地址均有效时，直接构造源 IVE 图像，调用 `ss_mpi_ive_resize` 缩放到 `aipp_output_size`，写入模型实例复用的非 cached 连续 MMZ，再绑定为 `svp_acl_data_buffer`。源平面行跨度须为 16 字节对齐。AIPP 完成 CSC 和归一化，模型图完成常量 letterbox padding。

未提供物理地址的分平面测试输入由 libyuv 合并到 ACL staging；只填写一个物理地址返回参数错误。SDK 假定 VPSS 等硬件生产者已完成写入；若 CPU 曾通过 cached 映射改写源平面，调用前须自行将 cache 刷到 DDR。

DV500 不含 IVE，按 `AlgSetAippScaleMode` 选择 SDK 预缩放或调用方缩放。原接口文档中的 AIPP 缩放约 128ms、无缩放约 4.8ms、CPU 预缩放约 11ms/帧属于特定模型和设备的测量记录，不作为接口性能承诺；测量条件、根因分析及后续优化见 [DV500 配置与性能说明](resources/config/svp_acl/dv500/README.md)。
