# alg_sdk 开发接入指南

本指南说明 alg_sdk 的工程接入流程、主要模块和问题定位方法，适用于应用集成和后续维护。完整接口定义见 [接口文档](alg_sdk_api_documentation_v1.0.0.md)，设计说明见 [架构文档](ARCHITECTURE.md)。

## 1. 接入范围

SDK 通过六个 C 函数完成实例管理、同步推理和结果释放，公共结果包含限速牌、禁令 / 停车牌和车牌。红绿灯模型及处理流程保留在工程内部，未纳入公共结果结构。

应用接入需要准备目标架构、后端运行库、配套模型和业务配置。XMM、SVP ACL 和 MNN 提供推理实现；RK 保留接口桩，不用于业务验证。

## 2. 构建和验证

在仓库根目录选择对应平台：

```bash
./build.sh linux aarch64 xmm
./build.sh linux aarch64 svp_acl
./build.sh linux x86_64 mnn
```

构建目录默认为 `build_linux_<arch>_<backend>/`。脚本会清理对应目录后重新构建，测试数据及交付文件应保存在目录之外。海思构建参数沿用 `aarch64` 名称，实际工具链为 ARM 32 位 `arm-linux-musleabi`。

程序在匹配的运行环境中执行。以海思构建目录为例：

```bash
cd build_linux_aarch64_svp_acl
./test_runner ../resources/config/svp_acl/speed_limit.json /data/test.jpg out
./test_runner ../resources/config/svp_acl/license_plate.json /data/plate.jpg out
./loop_runner ../resources/config/svp_acl/all.json /data/test.jpg -n 1000
```

`test_runner` 支持图片或图片目录，输出结果明细和标注图片。`loop_runner` 用于连续推理验证，可使用 `-n` 指定循环次数，使用 `-size`、`-fmt` 指定原始 YUV 帧的尺寸和格式。

配置中的模型相对路径以进程工作目录为基准。默认的 `../resources/model/...` 路径要求程序从仓库一级构建目录运行；设备部署目录不同时，应同步调整配置或改用绝对路径。

## 3. 公共接口与资源管理

| 接口 | 调用要求 |
|------|----------|
| `AlgCreate` | 传入配置路径和句柄输出指针，检查返回状态后使用句柄 |
| `AlgRun` | 同步处理一帧；输入缓冲在返回前保持有效 |
| `AlgFreeResult` | 释放三个结果数组并清空数量，可对已清空结果重复调用 |
| `AlgDestroy` | 释放有效句柄，应用随后将句柄变量设为 `NULL` |
| `AlgVersion` | 获取版本字符串，由 SDK 持有 |
| `AlgBackendName` | 获取编译后端名称，由 SDK 持有 |

`AlgResult` 首次使用必须零初始化。通过顶层指针检查后，`AlgRun` 会先释放结果结构中的旧数组；`AlgDestroy` 不代替应用释放已经返回的结果。跨帧保存数据时应复制数组内容，避免浅拷贝后重复释放。

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

    AlgFreeResult(&result);
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
| NV12 / NV21 | 宽高为偶数，Y/UV 两平面共用 stride；色度平面从 `data + stride*height` 开始 |
| `data_len` | 填写实际可用字节数；应用负责缓冲大小和边界校验 |
| 检测坐标 | 对应传入原图，SDK 负责恢复配置 ROI 和裁剪产生的偏移 |
| 限速值 | `SLV_10`～`SLV_120` 的枚举值乘 10 得到 km/h |
| PARE 分数 | 第一阶段检测分 |
| 禁停分数 | 检测分乘门控概率 |
| 限速分数 | 检测分乘 OCR 分类分 |
| 车牌分数 | 检测分乘 `rec_score`；有效车牌还需检查文本和业务格式 |
| `frame_id` | `AlgRun` 为动态库内共享计数；`AlgRunNative` 为输入 PTS |

图片文件应先解码为像素再传入。JSON 的 `preprocess.color` 定义模型输入颜色顺序，`AlgImage.format` 定义源图像格式，两者不要求相同。

海思 VPSS 实时流建议使用 `AlgRunNative`：应用只需传入 `source_id=0` 的原分辨率帧，检测模型的缩放由动态 AIPP 完成，不需要额外创建模型尺寸的 VPSS 通道。默认使用 NV21，也可传 NV12；两种格式均允许常见的 VPSS 行对齐 stride。

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
| [alg_interface.cpp](src/interface/alg_interface.cpp) | 句柄、结果释放和帧号 |
| [chain_solution.cpp](src/core/solution/chain_solution.cpp) | 阶段执行、裁剪及结果写回 |
| [model_instance.cpp](src/core/instance/model_instance.cpp) | 单模型初始化和执行 |
| [config.cpp](src/core/config/config.cpp) | 配置字段及引用校验 |
| [letterbox_preprocessor.cpp](src/core/preprocess/letterbox_preprocessor.cpp) | 图像前处理 |
| [object.cpp](src/core/object.cpp) | 公共结果类别映射 |
| [postprocessor_registry.cpp](src/core/registry/postprocessor_registry.cpp) | 后处理器注册和创建 |
| [模型后处理目录](src/models) | 检测、OCR 和车牌识别后处理 |
| [后端目录](src/backend) | 平台适配和后端资源管理 |

新增后端需维护工具链和链接依赖；新增模型类型需实现后处理并加入构建；新增公共结果类型需同时维护类型定义、转换、释放和调用示例。具体流程见 [架构设计](ARCHITECTURE.md)。

## 7. 问题定位

按配置加载、模型加载、图像前处理、后处理和结果转换的顺序定位问题。首先记录 `AlgVersion()`、`AlgBackendName()`、配置路径和返回码，再检查对应阶段的日志。

无检测结果时，应分别检查检测阈值、识别阈值、联合分、ROI 和最小框过滤，并确认最终类别属于公共结果范围。模型成功加载不代表输入布局或模型配置正确。

日志级别和文件输出见 [日志说明](src/core/log/README.md)。修改模型或配置后，应保留测试环境、输入数据、结果和耗时记录，作为版本比较和交付依据。
