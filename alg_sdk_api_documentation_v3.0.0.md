# ALG SDK 接口文档

| 项目 | 版本 | 更新日期 |
|------|------|----------|
| 限速牌 / 禁令停车牌 / 车牌识别（alg_sdk） | Version-3.0.0 | 2026 年 11 月 13 日 |

本文档用于客户接入，说明公共接口、输入要求和结果含义。函数和类型定义以交付的 `alg_interface.h`、`alg_types.h` 为准，头文件须与动态库配套使用。

## 一．接口说明

SDK 支持 C / C++ 调用。返回 `AlgStatus` 的接口以 `ALG_OK`（0）表示成功，负数表示失败，错误码见 2.2。

调用流程：`AlgCreate` 创建实例 → 循环调用 `AlgRun` 处理图像 → `AlgDestroy` 释放实例。应用应串行调用实例接口，输入图像在 `AlgRun` 返回前须保持有效且不被修改。

| 接口 | 功能 |
|------|------|
| `AlgCreate` | 创建并初始化实例 |
| `AlgDestroy` | 释放实例 |
| `AlgRun` | 同步处理一帧图像 |
| `AlgSetLogLevel` | 设置全局日志等级 |
| `AlgSetAippScaleMode` | 设置动态 AIPP 缩放模式（DV500） |
| `AlgSetTargetFps` | 设置实例处理帧率上限 |
| `AlgVersion` | 获取版本信息 |
| `AlgBackendName` | 获取推理后端名称 |

### 1.1 AlgCreate

```c
AlgStatus AlgCreate(AlgHandle* handle, const char* config_json_path);
```

创建并初始化实例，成功后通过 `handle` 返回句柄。

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | 输出 | 句柄变量的地址，不得为 `NULL`；句柄变量初始化为 `NULL` |
| `config_json_path` | 输入 | 配套 JSON 配置文件路径，不得为 `NULL` |

返回 `ALG_OK` 表示创建成功；失败时返回错误码，不改写句柄变量。每个成功创建的实例须调用一次 `AlgDestroy`。

### 1.2 AlgDestroy

```c
AlgStatus AlgDestroy(AlgHandle handle);
```

释放实例占用的资源。

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | 输入 | 由 `AlgCreate` 成功创建且尚未销毁的句柄 |

返回 `ALG_OK` 表示成功；传入 `NULL` 返回 `ALG_E_INVALID_ARG`。销毁后应将句柄变量设为 `NULL`，不得继续使用。

### 1.3 AlgRun

```c
AlgStatus AlgRun(AlgHandle handle, const AlgImage* image, AlgResult* result);
```

同步处理一帧图像，返回限速牌、禁令 / 停车牌及车牌结果，具体业务由配套配置决定。

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | 输入 | 有效实例句柄 |
| `image` | 输入 | 图像描述指针，不得为 `NULL`，字段要求见 2.5 |
| `result` | 输出 | 调用方提供的结果结构指针，不得为 `NULL`，建议零初始化 |

返回 `ALG_OK` 表示本帧处理完成，未检出目标也返回成功。失败时返回错误码，本次结果不可使用。成功时覆盖各类结果数量和有效元素；结果无需额外释放，可复用同一结构接收下一帧。

### 1.4 AlgSetLogLevel

```c
AlgStatus AlgSetLogLevel(AlgLogLevel level);
```

设置全局日志等级，对所有实例生效，可在创建实例前或运行期间调用。

| 参数 | 方向 | 说明 |
|------|------|------|
| `level` | 输入 | 日志等级，见 2.3；默认 `ALG_LOG_WARN` |

合法取值返回 `ALG_OK`；其他值返回 `ALG_E_INVALID_ARG`，当前等级不变。

### 1.5 AlgSetAippScaleMode

```c
AlgStatus AlgSetAippScaleMode(AlgAippScaleMode mode);
```

设置动态 AIPP 缩放模式，对所有实例生效，可在创建实例前或运行期间调用。仅对 Hi3519DV500 生效，其他后端忽略该设置。

| 参数 | 方向 | 说明 |
|------|------|------|
| `mode` | 输入 | 缩放模式，类型为 `AlgAippScaleMode`，取值见 2.11 |

合法取值返回 `ALG_OK`；其他值返回 `ALG_E_INVALID_ARG`，当前模式不变。配套 DV500 动态 AIPP 配置要求源图宽高比为 16:9。

### 1.6 AlgVersion

```c
const char* AlgVersion(void);
```

无需输入参数或实例句柄。返回版本字符串，格式为 `"alg_sdk.v3.0.0+<backend>"`。字符串由 SDK 持有，不得修改或释放，动态库卸载后指针失效。

### 1.7 AlgBackendName

```c
const char* AlgBackendName(void);
```

无需输入参数或实例句柄。返回后端名称：`"xmm"`、`"svp_acl"`、`"ez"` 或 `"mnn"`，以交付版本为准。字符串由 SDK 持有，不得修改或释放，动态库卸载后指针失效。

### 1.8 AlgSetTargetFps

```c
AlgStatus AlgSetTargetFps(AlgHandle handle, int fps);
```

设置指定实例的处理帧率上限，覆盖配置中的 `solution.target_fps`。

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | 输入 | 有效实例句柄 |
| `fps` | 输入 | 帧率上限，单位为帧/秒，范围 `[0, 1000]`，`0` 表示不限速 |

返回 `ALG_OK` 表示成功；空句柄或帧率超出范围返回 `ALG_E_INVALID_ARG`。未调用时沿用配置值，未配置时为 `0`。限速等待发生在 `AlgRun` 内；持有相机帧期间调用会延长帧的占用时间。

## 二．数据类型与结构体

### 2.1 AlgHandle：实例句柄

```c
typedef struct AlgContext* AlgHandle;
```

不透明句柄，由 `AlgCreate` 创建、`AlgDestroy` 释放。调用方不得访问或修改其内部内容。

### 2.2 AlgStatus：返回码

```c
typedef enum AlgStatus_ {
    ALG_OK = 0,
    ALG_E_INVALID_ARG = -1,
    ALG_E_NOT_INITIALIZED = -2,
    ALG_E_BACKEND = -3,
    ALG_E_MODEL_NOT_FOUND = -4,
    ALG_E_PREPROCESS = -5,
    ALG_E_POSTPROCESS = -6,
    ALG_E_OOM = -7,
    ALG_E_IO = -8,
    ALG_E_CONFIG = -9,
    ALG_E_UNKNOWN = -99,
} AlgStatus;
```

| 返回码 | 值 | 含义 |
|--------|----|------|
| `ALG_OK` | 0 | 成功，包含未检出目标的情况 |
| `ALG_E_INVALID_ARG` | -1 | 参数非法 |
| `ALG_E_NOT_INITIALIZED` | -2 | 相关处理模块尚未初始化 |
| `ALG_E_BACKEND` | -3 | 后端初始化、模型加载或推理失败 |
| `ALG_E_MODEL_NOT_FOUND` | -4 | 所需后处理类型未注册 |
| `ALG_E_PREPROCESS` | -5 | 图像前处理失败 |
| `ALG_E_POSTPROCESS` | -6 | 模型输出后处理失败 |
| `ALG_E_OOM` | -7 | 内存分配失败 |
| `ALG_E_IO` | -8 | 配置文件无法打开 |
| `ALG_E_CONFIG` | -9 | 配置格式或内容错误 |
| `ALG_E_UNKNOWN` | -99 | 未知错误，保留值 |

### 2.3 AlgLogLevel：日志等级

```c
typedef enum AlgLogLevel_ {
    ALG_LOG_OFF = -1,
    ALG_LOG_ERROR = 0,
    ALG_LOG_WARN = 1,
    ALG_LOG_INFO = 2,
    ALG_LOG_DEBUG = 3,
} AlgLogLevel;
```

| 枚举 | 值 | 输出范围 |
|------|----|----------|
| `ALG_LOG_OFF` | -1 | 关闭 SDK 日志 |
| `ALG_LOG_ERROR` | 0 | 错误 |
| `ALG_LOG_WARN` | 1 | 错误和警告（默认） |
| `ALG_LOG_INFO` | 2 | 错误、警告和运行信息 |
| `ALG_LOG_DEBUG` | 3 | 全部日志，建议排查问题时使用 |

### 2.4 AlgPixelFormat：像素格式

```c
typedef enum AlgPixelFormat_ {
    ALG_PIX_BGR = 0,
    ALG_PIX_RGB = 1,
    ALG_PIX_GRAY = 2,
    ALG_PIX_NV12 = 3,
    ALG_PIX_NV21 = 4,
} AlgPixelFormat;
```

| 枚举 | 值 | 像素排列 |
|------|----|----------|
| `ALG_PIX_BGR` | 0 | 每像素 3 字节，B、G、R 交错排列 |
| `ALG_PIX_RGB` | 1 | 每像素 3 字节，R、G、B 交错排列 |
| `ALG_PIX_GRAY` | 2 | 每像素 1 字节灰度值 |
| `ALG_PIX_NV12` | 3 | Y 平面和交错 UV 平面 |
| `ALG_PIX_NV21` | 4 | Y 平面和交错 VU 平面 |

输入均为原始 8 位像素，压缩图像须先解码。NV12/NV21 宽高须为偶数。格式支持范围以交付后端为准，亿智后端仅支持 NV12/NV21。

### 2.5 AlgImage：输入图像

```c
#define ALG_MAX_IMAGE_PLANES 4

typedef struct AlgImage_ {
    AlgPixelFormat format;
    int width;
    int height;
    int stride;
    int data_len;
    const void* data;

    const void* plane_data[ALG_MAX_IMAGE_PLANES];
    int plane_stride[ALG_MAX_IMAGE_PLANES];
    int plane_data_len[ALG_MAX_IMAGE_PLANES];
    uint64_t plane_phys_addr[ALG_MAX_IMAGE_PLANES];
} AlgImage;
```

使用前将结构体零初始化。`ALG_MAX_IMAGE_PLANES` 为 4；所有 stride 和长度均以字节为单位。

| 字段 | 说明 |
|------|------|
| `format` | 像素格式，见 2.4 |
| `width` / `height` | 图像宽高（像素），均须大于 0 |
| `stride` | 行跨度；`0` 表示按像素格式计算紧凑行跨度 |
| `data_len` | 连续缓冲长度；`0` 表示不提供长度检查信息 |
| `data` | 连续缓冲起始地址；为 `NULL` 时使用分平面输入 |
| `plane_data[]` | 平面虚拟地址；NV12/NV21 的 `[0]` 为 Y，`[1]` 为 UV/VU |
| `plane_stride[]` | 各平面行跨度；为 `0` 时沿用 `stride`，两者均为 `0` 时按紧凑宽度计算 |
| `plane_data_len[]` | 各平面缓冲长度；`0` 表示不提供长度检查信息 |
| `plane_phys_addr[]` | 硬件可访问的平面物理地址，须与虚拟地址对应；普通内存填 `0` |

连续输入时填写 `data`、`stride`、`data_len`；NV12/NV21 的色度平面紧接 Y 平面，两个平面共用行跨度。设实际行跨度为 `S`、图像宽高为 `W`、`H`：

| 格式 | 行跨度要求 | 最小缓冲长度 |
|------|------------|--------------|
| BGR / RGB | `S >= W*3` | `S*H` |
| GRAY | `S >= W` | `S*H` |
| NV12 / NV21 | `S >= W` | `S*H*3/2` |

分平面 NV12/NV21 输入时，`data` 设为 `NULL`，填写两个 `plane_data`。两平面行跨度均不得小于 `W`，Y 和 UV/VU 的缓冲分别至少覆盖 `H` 行和 `H/2` 行。未使用的平面字段保持为 `0`。

调用方须保证缓冲长度和图像布局有效，并在 `AlgRun` 返回前保持输入不被释放或改写。硬件输入还需满足：

- 海思动态 AIPP 连续输入使用整帧非 cached MMZ/VB 连续虚拟映射，Y 和 UV/VU 属于同一物理内存块；普通内存测试图像使用分平面输入。
- 硬件分平面输入的两个物理地址须同时有效；CV610 IVE 输入的行跨度须为 16 字节对齐，亿智硬件直接读取要求两平面行跨度相同且为 32 的倍数。
- 调用前须确认图像生产完成；若通过 cached 映射写入外部设备内存，由调用方按平台要求完成缓存同步。

### 2.6 AlgBox：检测框

```c
typedef struct AlgBox_ {
    int xmin, ymin, xmax, ymax;
    float score;
} AlgBox;
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `xmin` / `ymin` | `int` | 左上角坐标 |
| `xmax` / `ymax` | `int` | 右下角坐标 |
| `score` | `float` | 置信度，含义见下表 |

坐标以输入原图左上角为原点，x 向右、y 向下，单位为像素；框宽高为 `xmax-xmin`、`ymax-ymin`。

| 结果类型 | `score` 含义 |
|----------|--------------|
| 限速牌 | 检测置信度 × 数字识别置信度 |
| 禁止停车牌 | 检测置信度 × 禁停分类置信度 |
| PARE 停车让行牌 | 检测置信度 |
| 车牌 | 检测置信度 × `rec_score` |

### 2.7 AlgSpeedLimitValue / AlgSpeedLimit：限速牌

```c
typedef enum AlgSpeedLimitValue_ {
    SLV_INVALID = 0,
    SLV_10 = 1,
    SLV_20 = 2,
    SLV_30 = 3,
    SLV_40 = 4,
    SLV_50 = 5,
    SLV_60 = 6,
    SLV_70 = 7,
    SLV_80 = 8,
    SLV_90 = 9,
    SLV_100 = 10,
    SLV_110 = 11,
    SLV_120 = 12,
} AlgSpeedLimitValue;

typedef struct AlgSpeedLimit_ {
    AlgBox box;
    AlgSpeedLimitValue value;
} AlgSpeedLimit;
```

`AlgSpeedLimitValue` 中，`SLV_INVALID=0` 表示无效值；`SLV_10` 至 `SLV_120` 的枚举值依次为 1 至 12，表示 10 至 120 km/h、间隔 10 的限速值。

| 字段 | 类型 | 说明 |
|------|------|------|
| `box` | `AlgBox` | 原图检测框及置信度 |
| `value` | `AlgSpeedLimitValue` | 有效枚举值乘 10 得到 km/h，例如 `SLV_60=6` 表示 60 km/h |

### 2.8 AlgSignType / AlgSign：禁令及停车牌

```c
typedef enum AlgSignType_ {
    SIGN_INVALID = 0,
    SIGN_NO_PARKING = 1,
    SIGN_PARE = 2,
} AlgSignType;

typedef struct AlgSign_ {
    AlgBox box;
    AlgSignType type;
} AlgSign;
```

| 枚举 | 值 | 含义 |
|------|----|------|
| `SIGN_INVALID` | 0 | 无效牌种 |
| `SIGN_NO_PARKING` | 1 | 禁止停车牌 |
| `SIGN_PARE` | 2 | PARE 停车让行牌，红色八边形 |

| 字段 | 类型 | 说明 |
|------|------|------|
| `box` | `AlgBox` | 原图检测框及置信度 |
| `type` | `AlgSignType` | 牌种 |

### 2.9 AlgLicensePlate：车牌

```c
typedef struct AlgLicensePlate_ {
    AlgBox box;
    char text[32];
    float rec_score;
} AlgLicensePlate;
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `box` | `AlgBox` | 原图检测框及置信度 |
| `text` | `char[32]` | 以空字符结尾的车牌文本，最多 31 字节有效内容 |
| `rec_score` | `float` | 单独的识别置信度 |

车牌文本使用数字和大写英文字母，不含分隔符，例如 `ABC1D23`。调用方应按业务要求校验文本格式。

### 2.10 AlgResult：单帧结果

```c
#define ALG_MAX_SPEED_LIMIT_RESULTS 5
#define ALG_MAX_SIGN_RESULTS 5
#define ALG_MAX_LICENSE_PLATE_RESULTS 2

typedef struct AlgResult_ {
    long long frame_id;

    int speed_limit_count;
    AlgSpeedLimit speed_limits[ALG_MAX_SPEED_LIMIT_RESULTS];

    int sign_count;
    AlgSign signs[ALG_MAX_SIGN_RESULTS];

    int license_plate_count;
    AlgLicensePlate license_plates[ALG_MAX_LICENSE_PLATE_RESULTS];
} AlgResult;
```

| 字段 | 说明 |
|------|------|
| `frame_id` | SDK 内共享的成功处理帧编号 |
| `speed_limit_count` / `speed_limits` | 限速牌数量及数组，容量 `ALG_MAX_SPEED_LIMIT_RESULTS=5` |
| `sign_count` / `signs` | 禁令 / 停车牌数量及数组，容量 `ALG_MAX_SIGN_RESULTS=5` |
| `license_plate_count` / `license_plates` | 车牌数量及数组，容量 `ALG_MAX_LICENSE_PLATE_RESULTS=2` |

每个数组仅前 `count` 个元素有效，未检出时数量为 `0`。数组索引不表示跨帧跟踪 ID。结果由调用方持有，无需额外释放，可直接复制整个结构体。

### 2.11 AlgAippScaleMode：动态 AIPP 缩放模式

```c
typedef enum AlgAippScaleMode_ {
    ALG_AIPP_SCALE_AUTO = 0,
    ALG_AIPP_SCALE_EXTERNAL = 1,
} AlgAippScaleMode;
```

用于 `AlgSetAippScaleMode`，仅对 Hi3519DV500 生效。

| 枚举 | 值 | 含义 |
|------|----|------|
| `ALG_AIPP_SCALE_AUTO` | 0 | 默认，SDK 按配套配置完成缩放 |
| `ALG_AIPP_SCALE_EXTERNAL` | 1 | 调用方将图像缩放到配置的 `aipp_output_size`；尺寸不符时 `AlgRun` 返回 `ALG_E_PREPROCESS` |
