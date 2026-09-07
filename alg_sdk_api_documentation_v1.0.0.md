# ALG SDK 接口文档

| 项目 | 版本 | 更新日期 |
|------|------|----------|
| 限速牌 / 禁令停车牌 / 车牌识别（alg_sdk） | Version-1.0.0 | 2026 年 09 月 07 日 |

## 文档控制

| | 姓名 | 职务 | 日期 |
|------|------|------|------|
| 作者 | 喻伟 | 算法工程师 | 2026.07.17 |
| 审核 | | | |
| 通过 | | | |

## 文档发布

| 版本 | 作者 | 日期 | 说明 |
|------|------|------|------|
| 1.0.0 | 喻伟 | 2026.07.17 | 初版接口文档：限速牌及停车牌识别 |
| 1.0.0（文档修订） | 喻伟 | 2026.09.07 | 完善公共接口参数、返回值及数据结构说明 |

本文档说明 alg_sdk 1.0.0 的公共函数、枚举和结构体。接口声明见交付头文件 `alg_interface.h`，数据类型见 `alg_types.h`。应用应使用与动态库配套的头文件。

## 目录

- [一．接口说明](#一接口说明)
- [二．数据类型与结构体](#二数据类型与结构体)

## 一．接口说明

公共接口支持 C / C++ 调用，在 C++ 中采用 `extern "C"` 声明。返回 `AlgStatus` 的函数以 `ALG_OK`（0）表示成功，负数表示错误，错误码见 2.2。

SDK 不保证线程安全，应用应串行调用接口，并同步管理输入图像和结果结构的访问。

| 接口 | 功能 | 返回类型 |
|------|------|----------|
| `AlgCreate` | 创建并初始化实例 | `AlgStatus` |
| `AlgDestroy` | 销毁实例 | `AlgStatus` |
| `AlgRun` | 同步处理一帧图像 | `AlgStatus` |
| `AlgFreeResult` | 释放结果数组 | `void` |
| `AlgVersion` | 获取版本信息 | `const char*` |
| `AlgBackendName` | 获取运行后端名称 | `const char*` |

### 1.1 AlgCreate

```c
AlgStatus AlgCreate(AlgHandle* handle, const char* config_json_path);
```

创建算法实例，成功后通过 `handle` 返回句柄。

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | 输出 | 句柄变量的地址，不得为 `NULL`；调用前将句柄变量初始化为 `NULL` |
| `config_json_path` | 输入 | 随 SDK 提供的初始化文件路径，不得为 `NULL` |

返回 `ALG_OK` 表示创建成功；失败时返回对应错误码，不改写句柄变量。任一参数为 `NULL` 时返回 `ALG_E_INVALID_ARG`。

创建成功的句柄可重复用于处理多帧图像。每个成功创建的实例须调用一次 `AlgDestroy`，不得用新句柄直接覆盖尚未销毁的句柄。

### 1.2 AlgDestroy

```c
AlgStatus AlgDestroy(AlgHandle handle);
```

释放实例占用的资源。

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | 输入 | 由 `AlgCreate` 成功创建且尚未销毁的句柄 |

有效句柄返回 `ALG_OK`；`NULL` 返回 `ALG_E_INVALID_ARG`。销毁后，调用方应将句柄变量设为 `NULL`，不得再次销毁或继续用于推理。

该函数不释放已经返回的 `AlgResult` 数组，结果须通过 `AlgFreeResult` 单独释放。

### 1.3 AlgRun

```c
AlgStatus AlgRun(AlgHandle handle, const AlgImage* image, AlgResult* result);
```

同步处理一帧图像，并返回限速牌、禁令 / 停车牌及车牌结果。

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | 输入 | 有效的算法实例句柄 |
| `image` | 输入 | 图像描述指针，不得为 `NULL`；字段要求见 2.4 |
| `result` | 输入 / 输出 | 结果结构指针，不得为 `NULL`；首次使用必须零初始化，例如 `AlgResult result = {0};` |

返回 `ALG_OK` 表示本帧处理完成，没有检出目标也返回成功。返回非零时，本次结果不得用于业务处理。

调用要求：

- 图像缓冲由调用方持有，在函数返回前须保持有效且不被修改。
- 复用同一结果结构时，SDK 会先释放其中的旧数组，再写入本帧结果；旧数组指针随之失效。
- 若任一顶层参数指针为 `NULL`，返回 `ALG_E_INVALID_ARG`，原有结果不变。
- 通过顶层指针检查后，后续处理若返回错误码，结果数组为空、数量为 0，`frame_id` 不更新。
- 最后一次结果使用结束后，仍须调用 `AlgFreeResult`。

调用方负责像素指针、图像布局和缓冲长度的有效性；非法图像数据不保证以错误码返回。

### 1.4 AlgFreeResult

```c
void AlgFreeResult(AlgResult* result);
```

释放 SDK 分配的结果数组。

| 参数 | 方向 | 说明 |
|------|------|------|
| `result` | 输入 / 输出 | 零初始化、由 SDK 填充或已经释放的有效结果结构指针 |

释放 `speed_limits`、`signs` 和 `license_plates`，将对应指针设为 `NULL`、数量设为 0。该函数不释放 `AlgResult` 结构体本身，不修改 `frame_id`，也不释放输入图像。

无返回值。传入 `NULL`、零初始化的结果或已释放的结果均安全。不得对同一结果的多个浅拷贝分别调用此函数，也不得将调用方自行管理的数组交给该函数释放。

### 1.5 AlgVersion

```c
const char* AlgVersion(void);
```

返回版本字符串，格式为 `"alg_sdk.v1.0.0+<backend>"`，例如 `"alg_sdk.v1.0.0+svp_acl"`。

无需创建实例即可调用。字符串由 SDK 持有，调用方不可修改或释放；动态库卸载后不可继续访问该指针。

### 1.6 AlgBackendName

```c
const char* AlgBackendName(void);
```

返回 SDK 使用的后端名称字符串，例如 `"xmm"`、`"svp_acl"` 或 `"mnn"`，具体内容由交付版本决定。

无需创建实例即可调用。字符串由 SDK 持有，调用方不可修改或释放；动态库卸载后不可继续访问该指针。

## 二．数据类型与结构体

### 2.1 AlgHandle：实例句柄

```c
typedef struct AlgContext* AlgHandle;
```

不透明句柄，由 `AlgCreate` 创建、`AlgDestroy` 销毁。调用方不得访问或修改句柄指向的内容。

### 2.2 AlgStatus：返回码

```c
typedef enum AlgStatus_ {
    ALG_OK                = 0,
    ALG_E_INVALID_ARG     = -1,
    ALG_E_NOT_INITIALIZED = -2,
    ALG_E_BACKEND         = -3,
    ALG_E_MODEL_NOT_FOUND = -4,
    ALG_E_PREPROCESS      = -5,
    ALG_E_POSTPROCESS     = -6,
    ALG_E_OOM             = -7,
    ALG_E_IO              = -8,
    ALG_E_CONFIG          = -9,
    ALG_E_UNKNOWN         = -99,
} AlgStatus;
```

| 返回码 | 含义 |
|--------|------|
| `ALG_OK` | 成功，包括处理完成但未检出目标 |
| `ALG_E_INVALID_ARG` | 参数非法 |
| `ALG_E_NOT_INITIALIZED` | 相关处理模块尚未初始化；传入空句柄时返回 `ALG_E_INVALID_ARG` |
| `ALG_E_BACKEND` | 后端初始化、模型加载或推理失败 |
| `ALG_E_MODEL_NOT_FOUND` | 所需后处理类型不可用，不是模型文件缺失的统一返回码 |
| `ALG_E_PREPROCESS` | 图像前处理失败 |
| `ALG_E_POSTPROCESS` | 模型输出处理失败 |
| `ALG_E_OOM` | 检测到内存分配失败 |
| `ALG_E_IO` | 初始化文件无法打开 |
| `ALG_E_CONFIG` | 初始化文件格式或内容错误 |
| `ALG_E_UNKNOWN` | 未知错误，保留值 |

### 2.3 AlgPixelFormat：像素格式

```c
typedef enum AlgPixelFormat_ {
    ALG_PIX_BGR  = 0,
    ALG_PIX_RGB  = 1,
    ALG_PIX_GRAY = 2,
    ALG_PIX_NV12 = 3,
    ALG_PIX_NV21 = 4,
} AlgPixelFormat;
```

| 格式 | 像素排列 |
|------|----------|
| `ALG_PIX_BGR` | 每像素 3 字节，按 B、G、R 交错排列 |
| `ALG_PIX_RGB` | 每像素 3 字节，按 R、G、B 交错排列 |
| `ALG_PIX_GRAY` | 每像素 1 字节灰度值 |
| `ALG_PIX_NV12` | YUV420 半平面格式，Y 平面后紧接交错 UV 平面 |
| `ALG_PIX_NV21` | YUV420 半平面格式，Y 平面后紧接交错 VU 平面 |

输入均为原始 8 位像素。JPEG / PNG 等压缩数据须由调用方解码后再传入。

### 2.4 AlgImage：输入图像

```c
typedef struct AlgImage_ {
    AlgPixelFormat format;
    int            width;
    int            height;
    int            stride;
    int            data_len;
    const void*    data;
} AlgImage;
```

| 字段 | 说明 |
|------|------|
| `format` | 像素格式，取值见 2.3 |
| `width` | 图像宽度，单位为像素，须大于 0 |
| `height` | 图像高度，单位为像素，须大于 0 |
| `stride` | 每行字节跨度，取值要求见下表 |
| `data_len` | 实际可用像素缓冲字节数，计算时须避免整数溢出并确保可由 `int` 表示 |
| `data` | 调用方持有的可读像素缓冲地址，不得为 `NULL` |

设 `W=width`、`H=height`、`S` 为实际行字节跨度，完整帧的内存要求为：

| 格式 | `stride` 要求 | 完整帧缓冲要求 |
|------|---------------|----------------|
| BGR / RGB | `0` 表示 `S=W*3`；正数时须 `S>=W*3` | 至少 `S*H` 字节 |
| GRAY | `0` 表示 `S=W`；正数时须 `S>=W` | 至少 `S*H` 字节 |
| NV12 / NV21 | 仅支持紧凑布局，填 `0` 或 `W`，不用于调整行跨度 | 至少 `W*H*3/2` 字节 |

`stride` 不应为负。NV12 / NV21 的宽高须为偶数，色度平面从 `data + W*H` 开始，不支持分离平面指针、行尾额外填充或平面间隔。存在对齐填充的图像须先整理为紧凑连续帧。

SDK 不通过 `data_len` 完成缓冲边界检查，调用方须保证实际可用内存满足上述布局，并在 `AlgRun` 返回前保持缓冲有效。

### 2.5 AlgBox：检测框

```c
typedef struct AlgBox_ {
    int   xmin, ymin, xmax, ymax;
    float score;
} AlgBox;
```

| 字段 | 说明 |
|------|------|
| `xmin` / `ymin` | 检测框左上角坐标 |
| `xmax` / `ymax` | 检测框右下角坐标 |
| `score` | 检测或识别结果的置信度，含义见下表 |

坐标以传入 `AlgRun` 的原图左上角为原点，x 向右、y 向下，单位为像素。框宽高分别按 `xmax-xmin`、`ymax-ymin` 计算，调用方直接使用返回坐标。

| 结果类型 | `score` 含义 |
|----------|--------------|
| 限速牌 | 检测置信度 × 数字识别置信度 |
| 禁止停车牌 | 检测置信度 × 禁停分类置信度 |
| PARE 停车让行牌 | 检测置信度 |
| 车牌 | 检测置信度 × `rec_score` |

### 2.6 AlgSpeedLimitValue / AlgSpeedLimit：限速牌

```c
typedef enum AlgSpeedLimitValue_ {
    SLV_INVALID = 0,
    SLV_10      = 1,
    SLV_20      = 2,
    SLV_30      = 3,
    SLV_40      = 4,
    SLV_50      = 5,
    SLV_60      = 6,
    SLV_70      = 7,
    SLV_80      = 8,
    SLV_90      = 9,
    SLV_100     = 10,
    SLV_110     = 11,
    SLV_120     = 12,
} AlgSpeedLimitValue;

typedef struct AlgSpeedLimit_ {
    AlgBox             box;
    AlgSpeedLimitValue value;
} AlgSpeedLimit;
```

| 字段 | 说明 |
|------|------|
| `box` | 原图检测框及联合置信度 |
| `value` | 限速类别；有效值乘 10 得到 km/h，例如 `SLV_60=6` 表示 60 km/h |

`SLV_INVALID` 表示无效限速值，不应作为有效限速结果使用。

### 2.7 AlgSignType / AlgSign：禁令及停车牌

```c
typedef enum AlgSignType_ {
    SIGN_INVALID    = 0,
    SIGN_NO_PARKING = 1,
    SIGN_PARE       = 2,
} AlgSignType;

typedef struct AlgSign_ {
    AlgBox      box;
    AlgSignType type;
} AlgSign;
```

| 牌种 | 含义 |
|------|------|
| `SIGN_INVALID` | 无效牌种 |
| `SIGN_NO_PARKING` | 禁止停车牌 |
| `SIGN_PARE` | PARE 停车让行牌，红色八边形 |

`box` 为原图检测框及置信度，`type` 表示牌种。不同牌种的分数含义见 2.5。

### 2.8 AlgLicensePlate：车牌

```c
typedef struct AlgLicensePlate_ {
    AlgBox box;
    char   text[32];
    float  rec_score;
} AlgLicensePlate;
```

| 字段 | 说明 |
|------|------|
| `box` | 原图检测框；`box.score` 为检测置信度与识别置信度的乘积 |
| `text` | 以 `\0` 结尾的车牌文本，最多 31 字节有效内容；更长结果会截断 |
| `rec_score` | 单独的识别置信度，为识别过程中保留字符概率的乘积 |

车牌文本使用数字和大写英文字母，不含分隔符，例如 `ABC1D23`。未识别到字符时，`text` 可为空字符串，`rec_score` 保持初始值 `1.0`；调用方应同时检查 `text[0] != '\0'` 和业务要求的文本格式。

### 2.9 AlgResult：单帧结果

```c
typedef struct AlgResult_ {
    long long        frame_id;

    int              speed_limit_count;
    AlgSpeedLimit*   speed_limits;

    int              sign_count;
    AlgSign*         signs;

    int              license_plate_count;
    AlgLicensePlate* license_plates;
} AlgResult;
```

| 字段 | 说明 |
|------|------|
| `frame_id` | 成功处理的帧序号，从 0 开始 |
| `speed_limit_count` | 限速牌结果数量 |
| `speed_limits` | 限速牌数组，元素类型为 `AlgSpeedLimit` |
| `sign_count` | 禁令 / 停车牌结果数量 |
| `signs` | 禁令 / 停车牌数组，元素类型为 `AlgSign` |
| `license_plate_count` | 车牌结果数量 |
| `license_plates` | 车牌数组，元素类型为 `AlgLicensePlate` |

每个数组按对应数量遍历；未返回该类结果时，数量为 0、指针为 `NULL`。数组索引不表示跨帧跟踪 ID。

`frame_id` 由同一 SDK 动态库的所有句柄共享，每次 `AlgRun` 成功后递增，包括成功但没有检出目标的帧。该字段不随句柄销毁重建而重置，也不在失败调用或 `AlgFreeResult` 时更新。需要关联采集帧号或时间戳时，由调用方自行记录。

结果数组由 SDK 分配，通过 `AlgFreeResult` 统一释放。数组有效期截止于显式释放，或同一结果结构下一次通过顶层指针检查的 `AlgRun` 调用。跨帧保存时应复制数组内容；仅复制 `AlgResult` 结构体不会获得独立的数组所有权。
