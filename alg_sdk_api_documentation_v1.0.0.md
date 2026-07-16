# ALG SDK 接口文档

| 项目 | 版本 | 日期 |
|------|------|------|
| 红绿灯检测 + 限速牌识别（alg_sdk） | Version-1.0.0 | 2026 年 07 月 17 日 |

## 文档控制

| | 姓名 | 职务 | 日期 |
|------|------|------|------|
| 作者 | 喻伟 | 算法工程师 | 2026.07.17 |
| 审核 | | | |
| 通过 | | | |

## 文档发布

| 版本 | 作者 | 日期 | 细节 |
|------|------|------|------|
| 1.0.0 | 喻伟 | 2026.07.17 | 初版接口文档； 巴西限速牌+停车牌识别两个落地模型，强类型 C ABI |

## 目录

- [一． 算法库介绍](#一-算法库介绍)
- [二． 算法库调用流程介绍](#二-算法库调用流程介绍)
- [三． 接口说明](#三-接口说明)
- [四． 数据类型说明](#四-数据类型说明)

---

## 一． 算法库介绍

本算法库运用深度学习技术，面向端侧（NPU / 嵌入式）CV 推理场景，对车载前视摄像头图像进行实时识别，当前落地两个模型能力：

- **限速牌停车牌识别**：识别巴西限速牌 10 / 20 / … / 120 km/h 共 12 类限速值及其检测框与联合置信度，礼让行人PARE停车牌及禁止泊车E停车牌。
整个 SDK 由一份 JSON 配置驱动启动，JSON 描述 solution 编排（用哪几个模型、如何串接）、每个模型走哪个芯片后端的哪个模型文件、以及前 / 后处理参数。**调阈值 / 改输入尺寸 / 换均值方差均改 JSON 即可，无需重新编译**。

该算法库主要分为两部分：

1. **头文件部分**：提供 C 接口声明（`alg_interface.h`）与数据结构定义（`alg_types.h`）。
2. **动态库部分**：`libalg_sdk.so`，对应算法的动态库（已编入指定芯片后端，如 `xmm` / `rk`）。

上述两部分缺一不可，任何一个部分的不匹配（头文件与库版本、后端与模型文件不一致）都将导致系统无法正常使用。

**返回码约定**：所有返回 `AlgStatus` 的接口，`ALG_OK`（0）表示成功，负数表示对应错误码（见[四．数据类型说明](#四-数据类型说明)）。

---

## 二． 算法库调用流程介绍

典型调用顺序如下（创建一次、循环复用、退出销毁）：

```
            ┌─────────────────────────────────────────────┐
            │  AlgCreate(&handle, "speed_limit.json")      │  从 JSON 配置创建实例并初始化
            └───────────────────────┬─────────────────────┘
                                    │
            ┌───────────────────────▼─────────────────────┐
            │                  循环：每一帧                  │
            │  ┌────────────────────────────────────────┐  │
            │  │ 填充 AlgImage（格式 / 宽高 / 跨度 / data）│  │
            │  └───────────────────┬────────────────────┘  │
            │  ┌───────────────────▼────────────────────┐  │
            │  │ AlgRun(handle, &image, &result)         │  │  跑完整条 solution 流水线
            │  └───────────────────┬────────────────────┘  │
            │  ┌───────────────────▼────────────────────┐  │
            │  │ 读取 result.traffic_lights[] /          │  │  应用按强类型字段处理结果
            │  │       result.speed_limits[]             │  │
            │  └───────────────────┬────────────────────┘  │
            │  ┌───────────────────▼────────────────────┐  │
            │  │ AlgFreeResult(&result)                  │  │  释放本帧 result 内部数组
            │  └─────────────────────────────────────────┘  │
            └───────────────────────┬─────────────────────┘
                                    │
            ┌───────────────────────▼─────────────────────┐
            │  AlgDestroy(handle)                          │  销毁实例，释放全部资源
            └─────────────────────────────────────────────┘
```

流程要点：

- `AlgCreate` 较重（加载模型、初始化后端），**只调用一次**，句柄在整个生命周期内复用。
- 每帧调用 `AlgRun` 前填充 `AlgImage`，其中 `data` 缓冲区由调用方持有；`AlgRun` 不接管该内存。
- 每帧调用 `AlgRun` 后，`AlgResult` 内的 `speed_limits` / `signs` 数组由 SDK 分配，**必须配对调用 `AlgFreeResult`** 释放，否则内存泄漏。
- 结果按 solution 类型填充：限速牌配置填 `speed_limits`（限速值）+ `signs`（PARE / 禁止停车等牌种），二合一配置多者均可能非空。

最小调用示例：

```c
#include "alg_interface.h"

AlgHandle h = NULL;
if (AlgCreate(&h, "/data/speed_limit.json") != ALG_OK) return -1;

AlgImage img = {0};
img.format   = ALG_PIX_BGR;
img.width    = w;
img.height   = h_;
img.stride   = stride;          /* 每行字节跨度；0 表示按 width*bpp 推算 */
img.data_len = stride * h_;
img.data     = pixel_buffer;    /* 调用方持有 */

AlgResult r = {0};
if (AlgRun(h, &img, &r) == ALG_OK) {
    for (int i = 0; i < r.speed_limit_count; ++i) {
        AlgSpeedLimit* sl = &r.speed_limits[i];
        printf("%d km/h @ (%d,%d) score=%.2f\n",
               sl->value * 10, sl->box.xmin, sl->box.ymin, sl->box.score);
    }
}
AlgFreeResult(&r);

AlgDestroy(h);
```

---

## 三． 接口说明

> 接口声明见 `include/alg_interface.h`，全部以 `extern "C"` 导出，C / C++ 均可调用。

### 3.1 AlgCreate

```c
AlgStatus AlgCreate(AlgHandle* handle, const char* config_json_path);
```

**函数说明**：从一份 JSON 配置创建 SDK 实例并完成初始化（解析配置、加载模型、初始化芯片后端）。

**参数**：

- `handle`：输出参数，成功时写入创建好的 SDK 句柄（`AlgHandle`）。
- `config_json_path`：JSON 配置文件路径，如 `traffic_light.json` / `speed_limit.json` / `all.json`。

**返回值**：返回 `ALG_OK`（0）表示成功；非 0 表示失败（如 `ALG_E_CONFIG` 配置解析失败、`ALG_E_MODEL_NOT_FOUND` 模型 / 后处理类型未注册、`ALG_E_BACKEND` 后端错误）。

### 3.2 AlgDestroy

```c
AlgStatus AlgDestroy(AlgHandle handle);
```

**函数说明**：销毁 SDK 实例，释放其占用的全部资源（模型、后端上下文、内部缓冲）。

**参数**：

- `handle`：`AlgCreate` 返回的 SDK 句柄。

**返回值**：返回 `ALG_OK`（0）表示成功；非 0 表示失败。

### 3.3 AlgRun

```c
AlgStatus AlgRun(AlgHandle handle, const AlgImage* image, AlgResult* result);
```

**函数说明**：对一帧图像跑完整条 solution 流水线（前处理 → 推理 → 后处理 → 结果聚合），输出该帧识别结果。

**参数**：

- `handle`：`AlgCreate` 返回的 SDK 句柄。
- `image`：输入图像描述指针（`AlgImage`），`data` 像素缓冲区由调用方持有。
- `result`：输出结果指针（`AlgResult`），SDK 填充 `traffic_lights` / `speed_limits` 等字段。调用前建议零初始化（`AlgResult r = {0};`）。

**返回值**：返回 `ALG_OK`（0）表示成功；非 0 表示失败（如 `ALG_E_PREPROCESS` / `ALG_E_POSTPROCESS` / `ALG_E_BACKEND`）。

> **注意**：成功返回后，`result` 内的数组由 SDK 分配，须配对调用 `AlgFreeResult` 释放。

### 3.4 AlgFreeResult

```c
void AlgFreeResult(AlgResult* result);
```

**函数说明**：释放 SDK 在 `result` 内部分配的内存（`traffic_lights[]` / `speed_limits[]` / `signs[]` 三个数组）。对零值 / 已释放的 `result` 调用是安全的。

**参数**：

- `result`：`AlgRun` 填充过的结果指针。

**返回值**：无。

### 3.5 AlgVersion

```c
const char* AlgVersion(void);
```

**函数说明**：获取版本信息，形如 `"alg_sdk.v1.0.0+<backend>"`，方便日志记录与崩溃定位。

**返回值**：版本信息字符串（SDK 持有，调用方不可释放）。

### 3.6 AlgBackendName

```c
const char* AlgBackendName(void);
```

**函数说明**：获取库编译进来的芯片后端名（如 `"xmm"` / `"rk"`）。

**返回值**：后端名字符串（SDK 持有，调用方不可释放）。

### 接口一览表

| 接口 | 说明 | 返回值 |
|------|------|--------|
| `AlgCreate` | 从 JSON 配置创建并初始化实例 | `AlgStatus` |
| `AlgDestroy` | 销毁实例，释放全部资源 | `AlgStatus` |
| `AlgRun` | 对一帧图像跑完整 solution | `AlgStatus` |
| `AlgFreeResult` | 释放 result 内部数组 | `void` |
| `AlgVersion` | 获取版本字符串 | `const char*` |
| `AlgBackendName` | 获取后端名 | `const char*` |

---

## 四． 数据类型说明

> 类型定义见 `include/alg_types.h`。

### 4.1 基础类型

#### AlgHandle —— 不透明 SDK 句柄

```c
typedef struct AlgContext* AlgHandle;
```

#### AlgStatus —— 返回码（0 = 成功，负数 = 错误）

```c
typedef enum AlgStatus_ {
    ALG_OK                = 0,
    ALG_E_INVALID_ARG     = -1,   // 参数非法
    ALG_E_NOT_INITIALIZED = -2,   // 未初始化
    ALG_E_BACKEND         = -3,   // 芯片后端错误
    ALG_E_MODEL_NOT_FOUND = -4,   // 后处理类型 / 模型未注册
    ALG_E_PREPROCESS      = -5,   // 前处理失败
    ALG_E_POSTPROCESS     = -6,   // 后处理失败
    ALG_E_OOM             = -7,   // 内存不足
    ALG_E_IO              = -8,   // IO 错误
    ALG_E_CONFIG          = -9,   // JSON 配置文件解析失败
    ALG_E_UNKNOWN         = -99,  // 未知错误
} AlgStatus;
```

#### AlgPixelFormat —— 输入像素格式

```c
typedef enum AlgPixelFormat_ {
    ALG_PIX_BGR  = 0,
    ALG_PIX_RGB  = 1,
    ALG_PIX_GRAY = 2,
    ALG_PIX_NV12 = 3,
    ALG_PIX_NV21 = 4,
} AlgPixelFormat;
```

#### AlgImage —— 输入图像描述

```c
typedef struct AlgImage_ {
    AlgPixelFormat format;     // 像素格式
    int            width;      // 图像宽度
    int            height;     // 图像高度
    int            stride;     // 每行像素跨度（字节）；0 表示按 width * bpp 推算
    int            data_len;   // 像素数据字节数
    const void*    data;       // 像素数据缓冲区（调用方持有）
} AlgImage;
```

#### AlgBox —— 原图坐标系下的 2D 检测框

```c
typedef struct AlgBox_ {
    int   xmin, ymin, xmax, ymax;
    float score;
} AlgBox;
```

`score` 含义：

- **限速牌**：联合置信度 = 检测置信度 × OCR 分类置信度。

应用统一只看这一个分数即可。



### 4.2 限速牌识别（speed limit sign）

#### AlgSpeedLimitValue —— 限速牌类别

枚举编号即 km/h ÷ 10（`SLV_10`=1 → 10 km/h，…，`SLV_120`=12 → 120 km/h），限速值均为 10 的倍数，故 `value × 10 == km/h`。`value` 由后处理器从 `cls_cfg.postprocess.class_names` 推导（解析类名数值 ÷ 10），不依赖 `class_names` 的排列顺序。0 留作 `INVALID`。

```c
typedef enum AlgSpeedLimitValue_ {
    SLV_INVALID = 0,   // 置信度过低 / 字符组合非法的框已由 SDK 内部 drop，正常不出现
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
```

#### AlgSpeedLimit —— 单个限速牌识别结果

```c
typedef struct AlgSpeedLimit_ {
    AlgBox              box;     // 原图坐标系下的框，box.score = det × cls 联合置信度
    AlgSpeedLimitValue  value;   // 限速值类别（枚举编号即隐含 km/h，value × 10 = km/h）
} AlgSpeedLimit;
```

### 4.3 禁令 / 停车牌

非限速、不带可读数字的牌种（无法用 `AlgSpeedLimitValue` 表达），单列一个数组。

#### AlgSignType —— 牌种

```c
typedef enum AlgSignType_ {
    SIGN_INVALID    = 0,
    SIGN_NO_PARKING = 1,   // R-6c 禁止停车（红圈 + 黑 E + 红色 ✕），Stage2 门控识别
    SIGN_PARE       = 2,   // 停车让行（巴西 R-1，红色八边形），Stage1 检测直出
} AlgSignType;
```

#### AlgSign —— 单个禁令 / 停车牌识别结果

```c
typedef struct AlgSign_ {
    AlgBox       box;    // 原图坐标系下的框，box.score = 联合置信度
    AlgSignType  type;
} AlgSign;
```

### 4.5 完整结果

#### AlgResult —— 一帧的算法输出

```c
typedef struct AlgResult_ {
    long long           frame_id;              // 算法处理的图像帧 ID


    int                 speed_limit_count;     // 限速牌结果数量
    AlgSpeedLimit*      speed_limits;          // 限速牌结果数组（SDK 持有）

    int                 sign_count;            // 禁令/停车牌结果数量（v3.5）
    AlgSign*            signs;                 // 禁令/停车牌结果数组（SDK 持有）
} AlgResult;
```


两个数组由 SDK 持有，应用通过 `AlgFreeResult` 一次性释放。
