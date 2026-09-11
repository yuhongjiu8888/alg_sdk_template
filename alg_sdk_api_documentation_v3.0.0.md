# ALG SDK 接口文档

| 项目 | 版本 | 更新日期 |
|------|------|----------|
| 限速牌 / 禁令停车牌 / 车牌识别（alg_sdk） | Version-3.0.0 | 2026 年 09 月 10 日 |

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
| 2.0.0 | — | 2026.09.09 | 结果改为固定容量内联数组，移除 `AlgFreeResult` |
| 2.1.0 | — | 2026.09.10 | 新增运行期日志等级控制接口 `AlgSetLogLevel` |
| 3.0.0 | — | 2026.09.10 | `AlgImage` 新增独立图像平面，支持分地址 NV12/NV21 输入 |

本文档说明 alg_sdk 3.0.0 的公共函数、枚举和结构体。接口声明见交付头文件 `alg_interface.h`，数据类型见 `alg_types.h`。应用应使用与动态库配套的头文件。

## 目录

- [一．接口说明](#一接口说明)
- [二．数据类型与结构体](#二数据类型与结构体)

## 一．接口说明

公共接口支持 C / C++ 调用，在 C++ 中采用 `extern "C"` 声明。返回 `AlgStatus` 的函数以 `ALG_OK`（0）表示成功，负数表示错误，错误码见 2.2。

除 `AlgSetLogLevel` 外，SDK 不保证接口线程安全；应用应串行调用实例接口，并同步管理输入图像和结果结构的访问。`AlgSetLogLevel` 的等级读写使用原子操作，可独立动态调整。

| 接口 | 功能 | 返回类型 |
|------|------|----------|
| `AlgCreate` | 创建并初始化实例 | `AlgStatus` |
| `AlgDestroy` | 销毁实例 | `AlgStatus` |
| `AlgRun` | 同步处理一帧图像 | `AlgStatus` |
| `AlgSetLogLevel` | 动态设置 SDK 全局日志等级 | `AlgStatus` |
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

`AlgResult` 使用内联数组，不依赖实例内存；销毁句柄后，调用方持有的结果结构仍可读取。

### 1.3 AlgRun

```c
AlgStatus AlgRun(AlgHandle handle, const AlgImage* image, AlgResult* result);
```

同步处理一帧图像，并返回限速牌、禁令 / 停车牌及车牌结果。

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | 输入 | 有效的算法实例句柄 |
| `image` | 输入 | 图像描述指针，不得为 `NULL`；字段要求见 2.5 |
| `result` | 输出 | 结果结构指针，不得为 `NULL`；建议在栈上零初始化，例如 `AlgResult result = {0};` |

返回 `ALG_OK` 表示本帧处理完成，没有检出目标也返回成功。返回非零时，本次结果不得用于业务处理。

调用要求：

- 图像缓冲由调用方持有，在函数返回前须保持有效且不被修改。
- 复用同一结果结构时，SDK 直接覆盖数量和本帧有效元素。
- 若任一顶层参数指针为 `NULL`，返回 `ALG_E_INVALID_ARG`，原有结果不变。
- 通过顶层指针检查后，后续处理若返回错误码，结果数组为空、数量为 0，`frame_id` 不更新。

调用方负责像素指针、图像布局和缓冲长度的有效性；非法图像数据不保证以错误码返回。

### 1.4 AlgSetLogLevel

```c
AlgStatus AlgSetLogLevel(AlgLogLevel level);
```

动态设置 SDK 全局日志过滤等级。无需创建实例，建议在 `AlgCreate` 前调用，以便同时控制模型加载和初始化日志；也可以在运行过程中调用，新的等级立即对后续日志生效。

| 参数 | 方向 | 说明 |
|------|------|------|
| `level` | 输入 | `ALG_LOG_OFF`、`ALG_LOG_ERROR`、`ALG_LOG_WARN`、`ALG_LOG_INFO` 或 `ALG_LOG_DEBUG` |

合法等级返回 `ALG_OK`，其他数值返回 `ALG_E_INVALID_ARG` 且不修改当前等级。标准构建默认等级为 `ALG_LOG_WARN`；构建时可覆盖启动默认等级。等级越高，输出越详细；所选等级及更严重的日志会被输出。该设置作用于同一动态库中的所有算法实例，等级读写使用原子操作。

异步日志模式下，修改等级前已经进入队列的消息仍可能完成输出。开启 `ALG_LOG_DEBUG` 会执行额外的张量诊断统计，只建议排查问题时短时使用。

```c
AlgSetLogLevel(ALG_LOG_INFO);  /* 输出 Error/Warn/Info */
/* ... AlgCreate / AlgRun ... */
AlgSetLogLevel(ALG_LOG_OFF);   /* 关闭全部 SDK 日志 */
```

### 1.5 AlgVersion

```c
const char* AlgVersion(void);
```

返回版本字符串，格式为 `"alg_sdk.v3.0.0+<backend>"`，例如 `"alg_sdk.v3.0.0+svp_acl"`。3.0 扩展了 `AlgImage`，应用必须使用配套头文件重新编译。

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

### 2.3 AlgLogLevel：日志等级

```c
typedef enum AlgLogLevel_ {
    ALG_LOG_OFF   = -1,
    ALG_LOG_ERROR = 0,
    ALG_LOG_WARN  = 1,
    ALG_LOG_INFO  = 2,
    ALG_LOG_DEBUG = 3,
} AlgLogLevel;
```

| 枚举 | 输出范围 |
|------|----------|
| `ALG_LOG_OFF` | 不输出 SDK 日志 |
| `ALG_LOG_ERROR` | 仅错误 |
| `ALG_LOG_WARN` | 错误和警告（默认） |
| `ALG_LOG_INFO` | 错误、警告和运行信息，包括各阶段耗时 |
| `ALG_LOG_DEBUG` | 全部日志，包括诊断信息 |

### 2.4 AlgPixelFormat：像素格式

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

### 2.5 AlgImage：输入图像

```c
typedef struct AlgImage_ {
    AlgPixelFormat format;
    int            width;
    int            height;
    int            stride;
    int            data_len;
    const void*    data;

    const void*    plane_data[4];
    int            plane_stride[4];
    int            plane_data_len[4];
} AlgImage;
```

| 字段 | 说明 |
|------|------|
| `format` | 像素格式，取值见 2.4 |
| `width` | 图像宽度，单位为像素，须大于 0 |
| `height` | 图像高度，单位为像素，须大于 0 |
| `stride` | 连续模式的行跨度；分平面模式下可作为未单独填写平面 stride 时的公共回退值 |
| `data_len` | 连续模式的完整缓冲长度；0 表示调用方不提供长度检查信息 |
| `data` | 连续模式的起始地址；设为 `NULL` 时启用分平面模式 |
| `plane_data[4]` | 分平面地址；NV12/NV21 中 `[0]=Y`、`[1]=UV/VU`，其余保留为 `NULL` |
| `plane_stride[4]` | 各平面独立行跨度；0 表示回退到 `stride`，两者均为 0 时按紧凑宽度 |
| `plane_data_len[4]` | 各平面可用长度；0 表示调用方不提供该平面的长度检查信息 |

调用方必须先用 `{0}` 或 `memset` 将整个结构体清零，再填写字段。`data != NULL` 时 SDK 只读取兼容字段，不读取新增平面字段；`data == NULL` 时必须填写 `plane_data[0]`，NV12/NV21 还必须填写 `plane_data[1]`。

连续模式下，设 `W=width`、`H=height`、`S` 为实际行字节跨度，内存要求为：

| 格式 | `stride` 要求 | 完整帧缓冲要求 |
|------|---------------|----------------|
| BGR / RGB | `0` 表示 `S=W*3`；正数时须 `S>=W*3` | 至少 `S*H` 字节 |
| GRAY | `0` 表示 `S=W`；正数时须 `S>=W` | 至少 `S*H` 字节 |
| NV12 / NV21 | `0` 表示 `S=W`；正数为实际 Y/UV 共用行跨度 | 至少 `S*H*3/2` 字节 |

分平面 NV12/NV21 模式下，宽高须为偶数。Y 平面至少包含 `plane_stride[0]*H` 字节，UV/VU 平面至少包含 `plane_stride[1]*(H/2)` 字节；两个 stride 都须不小于 `W`，可各自不同。NV12 的 `[1]` 为 UV 交错，NV21 的 `[1]` 为 VU 交错。

长度字段大于 0 时 SDK 会检查其是否满足最小布局，等于 0 时跳过长度检查。无论是否填写长度，调用方均须保证实际内存有效，并在 `AlgRun` 返回前保持所有平面不被释放或改写。

海思动态 AIPP 的连续 NV12/NV21 输入直接绑定 `data`，不复制、不执行 CPU cache flush，也不释放该地址。该模式要求：

- Y 与 UV/VU 位于同一个 MMZ/VB 物理内存块，且 `phy_uv == phy_y + stride*height`；
- Y 与 UV/VU 使用相同 stride；
- `data` 是覆盖完整 Y+UV/VU 区域的非 cached 连续虚拟映射，例如 `ss_mpi_sys_mmap(phy_y, total_bytes)` 的返回地址；
- `data_len` 填写映射长度，映射在 `AlgRun` 返回前保持有效。

连续 NV21 接入示例：

```c
td_u32 y_bytes = stride * frame_height;
td_u32 total_bytes = y_bytes + stride * (frame_height / 2);

/* phy_vu 必须等于 phy_y + y_bytes，且整个范围属于同一个 MMZ/VB 块。 */
void *frame_base = ss_mpi_sys_mmap(phy_y, total_bytes);
if (frame_base == NULL) {
    /* 映射失败处理 */
}

AlgImage image = {0};
image.format = ALG_PIX_NV21;
image.width = frame_width;
image.height = frame_height;
image.stride = stride;
image.data = frame_base;
image.data_len = (int)total_bytes;

AlgStatus status = AlgRun(handle, &image, &result);
ss_mpi_sys_munmap(frame_base, total_bytes);
```

同一 VB 缓冲重复使用时，可复用对应的连续映射，避免在帧循环内重复 mmap/munmap；释放或归还 VB 缓冲前再解除映射。普通堆内存中的 YUV 文件数据不满足海思动态 AIPP 连续输入约束，应使用下述分平面模式。

分平面 NV21 接入示例（Y 与 VU 地址不要求连续）：

```c
AlgImage image = {0};
image.format = ALG_PIX_NV21;
image.width = frame_width;
image.height = frame_height;
image.data = NULL;
image.plane_data[0] = y_virtual_address;
image.plane_data[1] = vu_virtual_address;
image.plane_stride[0] = y_stride;
image.plane_stride[1] = vu_stride;
image.plane_data_len[0] = y_stride * frame_height;
image.plane_data_len[1] = vu_stride * (frame_height / 2);

AlgStatus status = AlgRun(handle, &image, &result);
```

### 2.6 AlgBox：检测框

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

### 2.7 AlgSpeedLimitValue / AlgSpeedLimit：限速牌

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

### 2.8 AlgSignType / AlgSign：禁令及停车牌

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

`box` 为原图检测框及置信度，`type` 表示牌种。不同牌种的分数含义见 2.6。

### 2.9 AlgLicensePlate：车牌

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

车牌文本使用数字和大写英文字母，不含分隔符，例如 `ABC1D23`。未识别到任何字符的候选会在 SDK 内部丢弃，不进入 `license_plates`；调用方仍应按业务要求检查文本格式。

### 2.10 AlgResult：单帧结果

```c
typedef struct AlgResult_ {
    long long        frame_id;

    int              speed_limit_count;
    AlgSpeedLimit    speed_limits[ALG_MAX_SPEED_LIMIT_RESULTS];

    int              sign_count;
    AlgSign          signs[ALG_MAX_SIGN_RESULTS];

    int              license_plate_count;
    AlgLicensePlate  license_plates[ALG_MAX_LICENSE_PLATE_RESULTS];
} AlgResult;
```

| 字段 | 说明 |
|------|------|
| `frame_id` | `AlgRun` 成功调用序号 |
| `speed_limit_count` | 限速牌结果数量 |
| `speed_limits` | 限速牌数组，元素类型为 `AlgSpeedLimit` |
| `sign_count` | 禁令 / 停车牌结果数量 |
| `signs` | 禁令 / 停车牌数组，元素类型为 `AlgSign` |
| `license_plate_count` | 车牌结果数量 |
| `license_plates` | 车牌数组，元素类型为 `AlgLicensePlate` |

每个数组按对应数量遍历；未返回该类结果时数量为 0。固定容量分别为限速牌 5、禁令 / 停车牌 5、车牌 2；数组索引不表示跨帧跟踪 ID。

`AlgRun` 的 `frame_id` 由同一 SDK 动态库的所有句柄共享，每次成功后递增，包括成功但没有检出目标的帧；该计数不随句柄销毁重建而重置，失败调用不更新该字段。

结果数组内联在 `AlgResult` 中，不发生堆分配，也不需要释放。调用方可将结构放在栈上；结构体赋值会完整复制当前帧结果。
