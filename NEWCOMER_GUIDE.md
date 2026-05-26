# 新手指导 — 接口调用关系与架构速览

本文档帮助新人快速理解 alg_sdk_template 的整体架构、接口调用关系和数据流向。

## 项目简介

alg_sdk_template 是一个面向边缘端 CV 推理的 C++ SDK，当前支持两个业务模型：

- **红绿灯检测** — 单阶段 YOLOX，416x416 RGB，4 类 (red/yellow/green/off)
- **巴西限速牌识别** — 两阶段流水线：SpeedSignNet 检测 + OCR 三头逐位识别，12 类 (10~120 km/h)

SDK 对外暴露 **强类型 C ABI**（6 个函数），内部通过 JSON 配置驱动整个推理流水线，切换模型/调参不需要重新编译 C++ 代码。

---

## 整体分层架构

```
+---------------------------------------------------------------------+
|                      应用层 (test_runner / 用户代码)                   |
|         AlgCreate -> AlgRun -> AlgFreeResult -> AlgDestroy            |
+--------------------------------+------------------------------------+
                                 |  C ABI (alg_interface.h / alg_types.h)
+--------------------------------v------------------------------------+
|                   接口层 (src/interface/alg_interface.cpp)            |
|            Context { ChainSolution }  + FillAlgResult                 |
+--------------------------------+------------------------------------+
                                 |
+--------------------------------v------------------------------------+
|               编排层 (src/core/solution/chain_solution.cpp)           |
|     顺序执行 stages -> ROI 裁剪 -> classify_into 合并 -> drop 过滤    |
+------+-----------------+-----------------+-------------------------+
       |                 |                 |
+------v------+  +-------v-------+  +------v------+
| ModelInstance|  | ModelInstance |  |ModelInstance|
| pre->infer->|  | pre->infer->  |  |pre->infer-> |
|    post     |  |    post       |  |   post      |
+------+------+  +-------+-------+  +------+------+
       |                 |                 |
  +----v-----+     +-----v-----+     +----v-----+
  |Preprocess |     | Inferer   |     |Postproc  |
  |(Letterbox)|     | (XMM/RK)  |     |(Registry)|
  +----------+     +-----------+     +----------+
```

三个正交变化轴：
- **换芯片** — 在 `src/backend/<chip>/` 实现 `IInferer` 接口即可
- **加模型** — 在 `src/models/<type>/` 实现 `IPostprocessor` + 一行 `REGISTER_ALG_POST` 注册
- **串业务** — 写 JSON 配置文件，描述 stages 编排关系，无需改 C++ 代码

---

## 公共 C API（6 个函数）

所有对外接口定义在 `include/alg_interface.h`，类型定义在 `include/alg_types.h`。

```c
// ---- 生命周期 ----
AlgStatus AlgCreate(AlgHandle* handle, const char* config_json_path);
AlgStatus AlgDestroy(AlgHandle handle);

// ---- 推理 ----
AlgStatus AlgRun(AlgHandle handle, const AlgImage* image, AlgResult* result);
void      AlgFreeResult(AlgResult* result);   // 释放 result 内部分配的数组

// ---- 信息查询 ----
const char* AlgVersion(void);       // "alg_sdk.v1.0.0+xmm"
const char* AlgBackendName(void);   // "xmm" / "rk"
```

### 核心数据结构

```c
// 输入图像描述（用户持有 data 指针）
typedef struct AlgImage_ {
    AlgPixelFormat format;   // BGR / RGB / GRAY / NV12 / NV21
    int width, height;
    int stride;              // 每行字节数，0 = 自动推算
    int data_len;
    const void* data;
} AlgImage;

// 输出结果（SDK 内部分配，用户通过 AlgFreeResult 释放）
typedef struct AlgResult_ {
    long long           frame_id;
    int                 traffic_light_count;
    AlgTrafficLight*    traffic_lights;     // 红绿灯结果数组
    int                 speed_limit_count;
    AlgSpeedLimit*      speed_limits;       // 限速牌结果数组
} AlgResult;
```

---

## 完整调用时序

```
用户代码                         SDK 内部
--------                         --------
AlgCreate(&h, "xxx.json")
  |
  +---> LoadSolutionConfig()           // JSON -> SolutionConfig (POD 结构体)
  +---> ChainSolution::Init()
  |       +---> ModelInstance::Init() x N
  |               +---> MakeInferer() -> Load(model_path)       // 后端加载模型
  |               +---> LetterboxPreprocessor::Configure()      // 前处理配置
  |               +---> PostprocessorRegistry::Create(type)
  |                       +---> Configure(inferer, json_params) // 后处理配置
  |
  v
AlgRun(h, &image, &result)
  |
  +---> ChainSolution::Run()
  |       +---> DecodeSourceToBgrInto()       // 解码原图 (仅多阶段时)
  |       +---> RunStage(0)
  |       |       +---> ModelInstance::Run()
  |       |               +---> pre_->Apply()        // LetterboxPreprocessor
  |       |               |       输出: TensorView (NCHW, 写入 NPU input buffer)
  |       |               +---> inferer_->Forward()  // NPU 推理
  |       |               +---> post_->Apply()       // 后处理 -> Object[]
  |       +---> RunStage(1)
  |       |       +---> CropFromDecoded()     // ROI 裁剪 (零拷贝)
  |       |       +---> ModelInstance::Run()  // 裁剪图跑子模型
  |       |       +---> MergeFields()         // classify_into: label覆盖 + score相乘
  |       +---> ...更多 stages...
  |       +---> 聚合: 收集所有 produces="objects" 的 stage 结果, 过滤 drop=true 的
  |
  +---> FillAlgResult(objs, result)           // Object[] -> 强类型 C 结构体
  |       按 category attribute 分桶:
  |         "traffic_light" -> AlgTrafficLight[]  (Object.value -> color enum)
  |         "speed_limit"   -> AlgSpeedLimit[]    (Object.value -> SLV enum, value×10=km/h)
  v
result 返回给用户

AlgFreeResult(&result)     // 释放 traffic_lights[] / speed_limits[]
AlgDestroy(h)              // 销毁整个 Context
```

---

## 单模型实例内部流水线 (ModelInstance)

每个 `ModelInstance` 封装一个模型的完整推理三件套：

```
ModelInstance::Run(image, &objects)
    |
    +-- pre_->Apply(image, inputView, state)     // LetterboxPreprocessor
    |      解码像素 -> resize -> mean/std 归一化
    |      输出: TensorView (NCHW, 填充到 NPU input buffer)
    |
    +-- inferer_->Forward()                       // IInferer (XMM/RK)
    |      输入/输出: TensorView (零拷贝到芯片内存)
    |
    +-- post_->Apply(inferer, state, &objects)    // IPostprocessor
           解码模型输出 -> NMS -> letterbox 坐标反变换
           输出: vector<Object> (原图坐标系)
```

---

## 后处理器注册表

三种后处理器通过 `REGISTER_ALG_POST` 宏自动注册到全局单例 `PostprocessorRegistry`：

| 注册名                | 实现文件                                                    | 用途                    |
|-----------------------|-------------------------------------------------------------|------------------------|
| `yolox_det`           | `src/models/yolox_det/yolox_det_postprocessor.cpp`          | 红绿灯检测 (多尺度 YOLOX head) |
| `yolov5_anchor_det`   | `src/models/yolov5_anchor_det/yolov5_anchor_det_postprocessor.cpp` | 限速牌检测 (anchor-based YOLOv5) |
| `ocr_classifier`      | `src/models/ocr_classifier/ocr_classifier_postprocessor.cpp` | 限速数字识别 (OCR 三头逐位 + class_names 解码，12 类) |
| `dualhead_classifier` | `src/models/dualhead_classifier/dualhead_classifier_postprocessor.cpp` | 旧限速数字分类 (双头 softmax，9 类，保留向后兼容) |

注册机制：编译期通过 `__attribute__((used))` 保证静态注册不被 `--gc-sections` 丢弃，运行期 `ModelInstance::Init` 通过 `PostprocessorRegistry::Create(cfg.post_type)` 按名字查找。

---

## 数据流全景

```
用户传入 AlgImage (BGR/RGB/NV12/...)
    |
    v
LetterboxPreprocessor::Apply()
    |  解码像素 -> resize (letterbox/stretch/center) -> mean/std 归一化
    |  输出: TensorView (NCHW, 写入 NPU input buffer)
    v
IInferer::Forward()
    |  NPU 推理，结果在 output buffer
    v
IPostprocessor::Apply()
    |  解码模型输出 -> NMS -> letterbox 坐标反变换
    |  输出: vector<Object> { box, label, attributes, drop }
    v
ChainSolution 编排
    |  ROI 裁剪 (零拷贝 cv::Mat 视图)
    |  classify_into: label 覆盖 + score 相乘
    |  drop 过滤
    v
FillAlgResult()
    |  按 category attribute 分桶:
    |    "traffic_light" -> AlgTrafficLight[]  (Object.value -> color enum)
    |    "speed_limit"   -> AlgSpeedLimit[]    (Object.value -> SLV enum, value×10=km/h)
    v
AlgResult (强类型 C 结构体, 用户直接访问)
```

---

## JSON 配置驱动关系

### 单阶段示例：红绿灯 (traffic_light.json)

```json
{
  "solution": {
    "type": "chain",
    "stages": [
      { "name": "tld", "model": "tld_cfg", "input": "image", "produces": "objects" }
    ]
  },
  "models": {
    "tld_cfg": {
      "model_path": "/data/traffic_light_int8.xmm",
      "preprocess":  { "input_size": [416, 416], "color": "RGB", "resize": "letterbox_tl" },
      "postprocess": { "type": "yolox_det", "num_classes": 4, "conf_threshold": 0.4 }
    }
  }
}
```

只有一个 stage，`input: "image"` 表示从原图输入，`produces: "objects"` 表示直接输出检测结果。

### 两阶段示例：限速牌 (speed_limit.json)

```json
{
  "solution": {
    "type": "chain",
    "stages": [
      { "name": "detector",   "model": "ssn_cfg", "input": "image",                "produces": "objects" },
      { "name": "classifier", "model": "cls_cfg",  "input": "objects_from:detector",
        "crop": { "expand_ratio": 1.5, "square": true },
        "produces": "classify_into:detector" }
    ]
  }
}
```

关键字段语义：
- `input: "image"` — 从原图输入 (第一阶段)
- `input: "objects_from:detector"` — 用 detector 的检测框作为 ROI 输入 (第二阶段)
- `crop` — ROI 裁剪参数 (expand_ratio 扩边比例, square 是否正方形)
- `produces: "classify_into:detector"` — 分类结果覆盖 detector 对象的 label，联合置信度 = det_score x cls_score，低于阈值则 drop

---

## 内部核心类关系

```
                     +----------------+
                     |  IInferer      |  <-- 芯片后端接口 (唯一的芯片变化点)
                     |  Load()        |
                     |  Forward()     |
                     |  InputView()   |
                     |  OutputView()  |
                     +-------+--------+
                             |
              +--------------+--------------+
              |                             |
     +--------v--------+          +--------v--------+
     |  XmmInferer     |          |  RkInferer      |
     |  (xmedia_cl)    |          |  (桩实现)        |
     +-----------------+          +-----------------+

                     +------------------+
                     |  IPostprocessor  |  <-- 后处理接口 (按模型类型派生)
                     |  Configure()     |
                     |  Apply()         |
                     +--------+---------+
                              |
           +------------------+------------------+
           |                  |                  |
  +--------v-------+ +-------v--------+ +------v--------------+
  | YoloxDet       | | Yolov5Anchor   | | OcrClassifier       |
  | Postprocessor  | | DetPostprocessor| | Postprocessor       |
  | (红绿灯)       | | (限速牌检测)    | | (限速数字 OCR)       |
  +----------------+ +----------------+ +---------------------+

                     +------------------+
                     |  IPreprocessor   |  <-- 前处理接口 (目前只有一个实现)
                     |  Configure()     |
                     |  Apply()         |
                     +--------+---------+
                              |
                     +--------v---------+
                     | Letterbox        |
                     | Preprocessor     |
                     | (NV12/BGR/RGB/   |
                     |  GRAY, NEON优化)  |
                     +------------------+

     +-------------------+
     |  ModelInstance     |  <-- 组装 pre + infer + post 三件套
     |  Init(config)      |
     |  Run(image, objs)  |
     +--------+----------+
              |
     +--------v----------+
     |  ChainSolution     |  <-- 多模型编排器
     |  Init(config)      |      解析 stage 引用 -> 索引
     |  Run(image, objs)  |      顺序执行 + ROI裁剪 + 合并 + drop
     +-------------------+
```

---

## 快速上手

### 1. 编译

```bash
./build.sh linux aarch64 xmm   # 或 rk
# 产物: build/linux_aarch64/libalg_sdk.so + test_runner
```

### 2. 运行测试

```bash
# 红绿灯检测
./build/linux_aarch64/test_runner resources/traffic_light.json /data/test.jpg out/

# 限速牌识别
./build/linux_aarch64/test_runner resources/speed_limit.json /data/test.jpg out/

# 二合一并行
./build/linux_aarch64/test_runner resources/all.json /data/test.jpg out/
```

### 3. 集成到应用

```c
#include "alg_interface.h"

// 1. 创建实例
AlgHandle h;
AlgCreate(&h, "/data/speed_limit.json");

// 2. 准备图像
AlgImage img = { ALG_PIX_BGR, width, height, stride, data_len, data };

// 3. 推理
AlgResult r = {};
AlgRun(h, &img, &r);

// 4. 读取结果
for (int i = 0; i < r.traffic_light_count; i++)
    printf("灯: %d, 置信度: %.2f\n", r.traffic_lights[i].color, r.traffic_lights[i].box.score);

for (int i = 0; i < r.speed_limit_count; i++)
    // value 是 AlgSpeedLimitValue 枚举，value × 10 == km/h（SLV_60=6 → 60 km/h）
    printf("限速: %d km/h, 置信度: %.2f\n", r.speed_limits[i].value * 10, r.speed_limits[i].box.score);

// 5. 释放
AlgFreeResult(&r);
AlgDestroy(h);
```

---

## 扩展指南

| 扩展方向       | 做法                                                              |
|---------------|------------------------------------------------------------------|
| 新增芯片后端   | 在 `src/backend/<chip>/` 实现 `IInferer`，提供 `MakeInferer()` + `BackendName()` |
| 新增后处理算法 | 在 `src/models/<type>/` 实现 `IPostprocessor`，用 `REGISTER_ALG_POST("name", ...)` 注册 |
| 新增业务场景   | 写一个 JSON 配置文件，引用已有的 model/postprocess type，无需改 C++ 代码 |
| 新增结果类型   | 在 `alg_types.h` 添加结构体，在 `object.cpp` 的 `FillAlgResult()` 中添加分桶逻辑 |

---

## 关键源文件索引

| 文件                                          | 职责                                      |
|----------------------------------------------|------------------------------------------|
| `include/alg_types.h`                        | C ABI 类型定义 (AlgImage, AlgResult, 枚举等)  |
| `include/alg_interface.h`                    | C API 声明 (6 个公共函数)                   |
| `src/interface/alg_interface.cpp`            | C API 实现, Context 包装 ChainSolution      |
| `src/core/solution/chain_solution.cpp`       | 多模型编排核心: stage 执行 + ROI + 合并       |
| `src/core/instance/model_instance.cpp`       | 单模型 pre->infer->post 流水线              |
| `src/core/preprocess/letterbox_preprocessor.cpp` | 图像前处理 (NEON 优化)                   |
| `src/core/postprocess/nms.cpp`               | 通用 NMS 实现                              |
| `src/core/registry/postprocessor_registry.cpp` | 后处理器注册表                            |
| `src/core/object.cpp`                        | FillAlgResult: 内部 Object -> 强类型 ABI     |
| `src/core/config/config.cpp`                 | JSON 配置解析                              |
| `src/core/tensor.h`                          | TensorView: 芯片中立的张量描述符              |
