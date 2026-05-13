# alg_sdk 架构设计

本文档是 alg_sdk 的**设计说明**：解释为什么这样分层、各层用什么 C++ 抽象、
变化点落在哪里。README 是用法入口，本文档是阅读源码的导览。

---

## 1. 设计目标

| 目标                       | 含义                                                              |
|----------------------------|-------------------------------------------------------------------|
| 移植芯片成本接近为零        | 换 NPU 厂商时只重写一个类，前/后处理、业务编排、C API 完全不动     |
| 加模型类型成本接近为零      | 新模型 = 一个目录 + 一行注册；不改公共头、不改 C API、不改 Solution |
| 业务编排无需重编            | 检测→识别→...的整条流水线写在 JSON 里                              |
| 参数调优无需重编            | 阈值、输入尺寸、均值方差等全部 JSON 化                              |
| 同一个 .so 适配不同业务     | 红绿灯检测、限速牌识别、并行版共用一个二进制，只换 JSON               |
| 零额外内存拷贝              | 前处理直接写进芯片输入张量，不走中间 buffer                         |

---

## 2. 三个正交的变化轴

整个架构围绕"业务变化轴"对齐源码目录，每个轴只有**一个**扩展点：

```
                       ┌──────────────────────┐
                       │      C 公共 API       │  ← 跨芯片 / 跨业务 都不变
                       └─────────┬─────────────┘
                                 │
                       ┌──────────────────────┐
                       │   ChainSolution      │  ← 业务编排：JSON 描述
                       └─────────┬─────────────┘
                                 │ 持有 N 个
                       ┌──────────────────────┐
                       │   ModelInstance      │  ← 单网络 = pre + infer + post
                       └─────┬─────┬─────┬─────┘
                             │     │     │
   ┌─────────────────────────┘     │     └────────────────────────────┐
   ▼                               ▼                                  ▼
IPreprocessor                  IInferer                          IPostprocessor
(数据驱动)                     (每芯片一份)                       (每模型类型一份)
LetterboxPreprocessor          XmmInferer / RkInferer / ...      YoloxDet / Yolov5AnchorDet / DualheadClassifier / ...
```

| 变化轴                 | 抽象              | 源码位置                    | 用到的 C++ 特性                     |
|------------------------|-------------------|-----------------------------|-------------------------------------|
| **芯片平台**           | `IInferer`        | `src/backend/<chip>/`       | 纯虚类 + CMake 编译期选择            |
| **模型拓扑**           | `IPostprocessor`  | `src/models/<type>/`        | 纯虚类 + 自注册工厂（按类型名）       |
| **业务编排**           | JSON              | `resources/<scene>.json`    | jsoncpp 解析 → POD 配置结构体        |
| 前处理                 | `IPreprocessor`   | `src/core/preprocess/`      | 唯一实现 `LetterboxPreprocessor`，数据驱动 |

三条轴**两两正交**：换芯片只动 backend；加模型只动 models；调业务只动 JSON。

---

## 3. 目录结构

```
include/                       公共 C ABI（应用方唯一依赖）
  alg_types.h                  AlgResult / AlgTrafficLight / AlgSpeedLimit / 错误码、像素格式
  alg_interface.h              AlgCreate / AlgRun / AlgDestroy / AlgFreeResult

resources/                     示例 JSON 业务配置（本分支落地模型）
  traffic_light.json           红绿灯：单阶段 YOLOX 4 类（416×416 RGB）
  speed_limit.json             巴西限速牌：检测 + 双头分类（classify_into 过滤 < 0.7）

src/
  interface/                   C ABI → C++ 实现的胶水层
    alg_interface.cpp

  core/                        与芯片、模型、业务都无关的内核
    tensor.h                   芯片中立的 TensorView（shape/dtype/quant/data*）
    object.h, object.cpp       内部 C++ Object 表示 + 转 C ABI 工具
    status.h, logger.h
    config/                    JSON → SolutionConfig 解析层
    infer/inferer.h            IInferer（每芯片一份实现）
    preprocess/                IPreprocessor 基类 + LetterboxPreprocessor
    postprocess/               IPostprocessor 基类 + 通用 NMS
    registry/                  PostprocessorRegistry（按类型名 → builder）
    instance/                  ModelInstance（单网络的 pre+infer+post 三件套）
    solution/                  ChainSolution（多模型业务编排）+ CropFromBox

  models/                      每种模型类型一个目录
    yolox_det/                 mmyolo YOLOXHead 多尺度（offset=0 grid，class-aware NMS）
    yolov5_anchor_det/         单尺度 anchor + sigmoid decode（SpeedSignNet）
    dualhead_classifier/       双头数字识别 + 装配规则 + joint 置信度阈值过滤

  backend/                     每种芯片一个目录
    backend_factory.h          MakeInferer() / BackendName() 入口（编译期绑定）
    xmm/                       XMM（xmedia_cl + MMZ）
    rk/                        Rockchip RKNN（桩示例）

cmake/                         构建配置（按 ALG_PLATFORM + ALG_BACKEND 拼接）
test/test_runner.cpp           通用 runner：./test_runner <solution.json> <image>
```

---

## 4. 核心抽象

### 4.1 `TensorView` —— 跨层数据契约

**位置**：`src/core/tensor.h`

```cpp
struct TensorView {
    std::string  name;
    DataType     dtype;     // U8/I8/U16/I16/F16/F32/I32
    Layout       layout;    // NCHW/NHWC/ND
    Shape        shape;     // ndims + dims[]
    QuantInfo    quant;     // scale, zero_point
    void*        data;      // 宿主可见指针；芯片后端持有的真实内存
    size_t       size_bytes;
};
```

**作用**：层与层之间唯一的数据描述符。它**不拥有内存**，谁分配谁释放。
- 芯片后端 `IInferer` 持有真实的 MMZ / DMA-buf / malloc 内存，往里塞 `data*`。
- 前处理把宿主像素直接写到这个 `data*`（**零拷贝**）。
- 后处理读这个 `data*`。

这是整个 SDK 的"窄腰"。**没有这一层抽象，芯片细节就会污染到所有上层。**

### 4.2 `IInferer` —— 芯片抽象

**位置**：`src/core/infer/inferer.h`

```cpp
class IInferer {
public:
    virtual Status Load(const std::string& model_path) = 0;
    virtual Status Forward() = 0;
    int NumInputs() const;  int NumOutputs() const;
    TensorView&       InputView(int i);
    const TensorView& OutputView(int i) const;
};
```

**契约**：
1. `Load()` 解析模型文件、分配芯片侧"宿主可见"的 I/O 内存、把 `TensorView` 填到 `input_views_`/`output_views_`。
2. `Forward()` 前刷新输入 cache，运行 NPU graph，运行后失效输出 cache，确保宿主能读最新数据。
3. 之外**任何事情都不归它管** —— 不做前处理、不知道模型是检测器还是分类器。

**实现示例**：`XmmInferer`（`src/backend/xmm/`）—— 全套 xmedia_cl + MMZ 流程。

### 4.3 `IPostprocessor` —— 模型类型抽象

**位置**：`src/core/postprocess/postprocessor.h`

```cpp
class IPostprocessor {
public:
    virtual Status Configure(const IInferer& inferer, const Json::Value& params) = 0;
    virtual Status Apply(const IInferer& inferer, const PreprocessState& state,
                         std::vector<Object>* out) = 0;
};
```

**契约**：
1. `Configure()` 一次性接收该模型在 JSON 里的 `postprocess` 节点 —— 阈值、stride、类目数全部从这里读。
2. `Apply()` 解码 inferer 的输出张量，**输出 `vector<Object>`**（不是直接写 `AlgResult`，便于 Solution 进一步合并）。
3. 坐标系：postprocessor 把坐标映射回"喂给本模型的那张图"（用 `PreprocessState` 里的 letterbox 信息）。Solution 再加 crop offset 映射回原帧。

**注册机制**：每个实现在自己的 `_register.cpp` 里调一次 `REGISTER_ALG_POST(type_name, builder)`，进程启动时静态构造期完成注册。`__attribute__((used))` 防止 `-Wl,--gc-sections` 在 release 链接时把没人引用的注册代码当成死代码砍掉。

### 4.4 `IPreprocessor` / `LetterboxPreprocessor` —— 前处理

**位置**：`src/core/preprocess/`

```cpp
class IPreprocessor {
public:
    virtual Status Configure(const PreprocessConfig& cfg, const TensorView& input) = 0;
    virtual Status Apply(const AlgImage& image, TensorView& input,
                         PreprocessState& state) = 0;
};

struct PreprocessConfig {
    int        net_width, net_height;
    ColorOrder color;      // BGR / RGB / Gray
    ResizeMode resize;     // stretch / letterbox_tl / letterbox_center
    Layout     layout;     // NCHW
    float      mean[3], std[3], scale;
    uint8_t    pad_value;
};
```

**有意为之**：抽象保留，但唯一实现就是数据驱动的 `LetterboxPreprocessor`。
端侧前处理操作组合空间很小（resize / 颜色转换 / 平面分离 / 归一化 / 布局），
按数据配置足以覆盖；按模型派生子类反而会出现 N 份几乎一样的代码。

只有真的出现完全异形的前处理（多 crop 拼接、复合输入等）才派生新子类。

### 4.5 `ModelInstance` —— 单网络的"三件套"组合

**位置**：`src/core/instance/`

```cpp
class ModelInstance {
public:
    Status Init(const ModelInstanceConfig& cfg);
    Status Run(const AlgImage& image, std::vector<Object>* out);

private:
    std::unique_ptr<IInferer>       inferer_;
    std::unique_ptr<IPreprocessor>  pre_;
    std::unique_ptr<IPostprocessor> post_;
};
```

把"一个网络的从图像到 Object" 封装成一个原子单位。多个 ModelInstance 由 ChainSolution 编排。

### 4.6 `ChainSolution` —— 业务编排

**位置**：`src/core/solution/`

通用编排器，**所有业务变化都在 JSON 里**。一次 `Run()` 按 `stages` 顺序：

```
for stage in stages:
    if stage.input == "image":
        objects = stage.model.Run(image)
        state[stage.name] = objects
    else (stage.input == "objects_from:<earlier>"):
        for each object in state[earlier]:
            crop, xf = CropFromBox(image, object.box, stage.crop)
            sub_objs = stage.model.Run(crop)
            map_back_to_original(sub_objs, xf)
            if stage.produces == "objects":
                state[stage.name].append(sub_objs)
            else:  // attributes_into / classify_into
                merge sub_objs[0]'s field into object (classify_into 还会更新 label / box.score 或 drop)

return ⋃ { state[s].objects | s in stages if s.produces == "objects" }
```

并行检测（场景 C，多个 `produces:"objects"` 的 stage）天然就在最后那行 ⋃ 处合并。

### 4.7 内部 `Object` ↔ 外部 `AlgResult.traffic_lights[] / speed_limits[]`

| 内部 (`alg::Object`)                        | 外部 (强类型 ABI)                                |
|---------------------------------------------|--------------------------------------------------|
| C++，统一形态：box + attributes + drop      | C ABI，按业务分桶：AlgTrafficLight / AlgSpeedLimit |
| Solution / postprocessor 操作               | 应用层看到的最终结构                              |

`FillAlgResult()` 在 C API 边界做一次翻译：
- 按 `attributes["category"].value_str` 分桶到 `traffic_lights[]` / `speed_limits[]`
- 内部 `Object.label`（JSON class_names 下标）+1 映射到强类型 enum（0 留给 INVALID）
- 限速牌另外查表得到 `value_kmh`
- malloc 出来的两个数组由用户通过 `AlgFreeResult()` 一次释放

---

## 5. 数据流

### 5.1 一次 `AlgRun` 的全过程（以限速牌二阶段为例）

```
应用层调用 AlgRun(image)
        │
        ▼
ChainSolution::Run(image, &objects)
        │
        ├── stage "detector" (SpeedSignNet):
        │      ModelInstance::Run(image, &objs)
        │      ├── LetterboxPreprocessor::Apply ──┐
        │      │   image → xmm_input_tensor (零拷贝)│
        │      │   填 PreprocessState (scale/pad)   │
        │      ├── XmmInferer::Forward             │  ╲
        │      │   flush input cache               │   ╲ 上层不感知
        │      │   xmedia_cl_graph_process         │    >
        │      │   invalidate output cache         │   ╱
        │      └── Yolov5AnchorDetPostprocessor::Apply ──┘
        │          sigmoid + (σ*2-0.5+grid)*stride 解码 → NMS → 映射回原帧
        │          → objs = [box(label=0,score=det_conf), ...]
        │      state["detector"] = objs
        │
        ├── stage "classifier" (SpeedSignClassifier, classify_into:detector):
        │      for each obj in state["detector"] (跳过 obj.drop):
        │          CropFromBox(image, obj.box, expand=1.5, square=true)
        │              → cropped_image + CropTransform xf
        │          ModelInstance::Run(cropped_image, &sub)
        │              (双头 softmax + 装配)
        │          if sub.empty():           # joint_conf < 0.7
        │              obj.drop = true
        │          else:
        │              obj.label     = sub[0].label        # 9 类 idx
        │              obj.box.score *= sub[0].box.score   # det × cls 联合
        │              obj.attributes = sub[0].attributes
        │              obj.field_mask |= ALG_FIELD_ATTRIBUTES
        │
        └── 汇总：所有 produces=="objects" 的 stage 输出 → out_objects
                  跳过 obj.drop=true 的对象
                  → FillAlgResult(out_objects, &result)
                  → 应用层拿到 AlgResult.traffic_lights[] / speed_limits[]
```

红绿灯单阶段更简单：只有 `stage "tld"`（YoloxDetPostprocessor），整个 classifier 段省略。

### 5.2 内存所有权时序

```
内存                      由谁分配                生命周期跨度
──────────────────────── ────────────────────── ────────────────
NPU workspace/weight    XmmInferer (MMZ)        IInferer 析构
NPU 输入/输出张量        XmmInferer (MMZ)        IInferer 析构
前处理临时 Mat（cv::Mat）LetterboxPreprocessor   按帧覆盖（复用）
裁剪 ROI（cv::Mat）      ChainSolution           按对象一次性
内部 Object/Keypoints   std::vector            ChainSolution::Run 内
AlgTrafficLight[]/AlgSpeedLimit[]  malloc (C API 出口)  用户 AlgFreeResult 释放
```

---

## 6. JSON 配置 schema

完整结构：

```json
{
  "solution": {
    "type": "chain",
    "stages": [
      {
        "name": "<stage_id>",
        "model": "<key in models map>",
        "input": "image" | "objects_from:<earlier_stage>",
        "crop":  { "expand_ratio": <float>, "square": <bool> },
        "produces": "objects"
                  | "attributes_into:<earlier_stage>"
                  | "classify_into:<earlier_stage>"
      }
    ]
  },
  "models": {
    "<model_cfg_id>": {
      "model_path": "<path to chip model file>",
      "preprocess": {
        "input_size": [W, H],
        "color":  "BGR" | "RGB" | "GRAY",
        "resize": "stretch" | "letterbox_tl" | "letterbox_center",
        "layout": "NCHW",
        "mean": [c0, c1, c2],
        "std":  [c0, c1, c2],
        "scale": <float>,
        "pad_value": <uint8>
      },
      "postprocess": {
        "type": "<registered postprocessor type>",
        "...":  "type-specific params, read by that postprocessor's Configure()"
      }
    }
  }
}
```

### 6.1 Stage 语义速查

| stage.input               | stage.produces               | 行为                                       |
|---------------------------|------------------------------|--------------------------------------------|
| `image`                   | `objects`                    | 在原图上做检测                              |
| `image`                   | `attributes_into:s`          | 全图分类（少见，但合法）                    |
| `objects_from:s`          | `attributes_into:s`          | 在 s 的每个 box 上做属性识别                |
| `objects_from:s`          | `classify_into:s`            | **在 s 的每个 box 上做识别**：合并 attributes，把 sub.label 写回 src.label，src.box.score *= sub.box.score 作联合置信度；分类器返回空 → 直接 drop 掉这个 src 框 |
| `objects_from:s`          | `objects`                    | 在 s 的每个 ROI 上再做检测（少见）           |

`classify_into` 与 `attributes_into` 的差别：前者是**带过滤的识别器**——分类器
`Apply` 返回空 vector 即视为「该框未通过识别」，ChainSolution 在最终聚合阶段
跳过被标记 `drop` 的对象。典型用法是限速牌二阶段链路里 joint_conf < 0.7 的丢弃。

### 6.2 解析约束

`LoadSolutionConfig()` 启动期校验：
- 所有 `stage.model` 必须命中 `models` 表里的 key
- `objects_from:s` 引用的 stage 必须在当前 stage 之前
- `..._into:s` 引用的 stage 必须在当前 stage 之前

不通过直接返回 `ALG_E_CONFIG`，附详细 err_msg。

---

## 7. 扩展规则

### 7.1 加新芯片

1. `src/backend/<chip>/<chip>_inferer.h/.cpp` 实现 `IInferer`。
2. 同目录 `<chip>_backend.cpp` 实现两个工厂入口：
   ```cpp
   std::unique_ptr<IInferer> alg::MakeInferer() { return std::unique_ptr<IInferer>(new ChipInferer()); }
   const char*               alg::BackendName() { return "<chip>"; }
   ```
3. `cmake/CMakeLists_linux_aarch64.cmake` 加一个 `elseif(ALG_BACKEND STREQUAL "<chip>")` 分支配置该芯片的 SDK 头文件和库。
4. `./build.sh linux aarch64 <chip>`。

**影响范围**：仅 `src/backend/<chip>/`。模型代码、Solution、C API、JSON 全部不动。

### 7.2 加新模型类型

1. `src/models/<type>/<type>_postprocessor.h/.cpp` 派生 `IPostprocessor`，在 `Apply()` 里把输出塞进 `Object` 的对应字段（box / attributes），置 `field_mask`。
2. 同目录 `<type>_register.cpp`：
   ```cpp
   REGISTER_ALG_POST("<type_name>", []{
       return std::unique_ptr<IPostprocessor>(new MyPostprocessor());
   });
   ```
3. CMake 的 `ALG_MODEL_SRCS` 追加这两个 cpp。
4. 业务 JSON 里在某个 model 的 `postprocess.type` 写新类型名。

**影响范围**：仅 `src/models/<type>/`。芯片代码、Solution、C API、其它模型全部不动。

### 7.3 加新业务流（**无需写 C++**）

写一份新 JSON：

```json
{
  "solution": { "type": "chain", "stages": [ /* ... */ ] },
  "models":   { /* ... */ }
}
```

配置以下三类东西的组合：
- 每个 stage 喂哪张图（原图 / 上游某 stage 的 ROIs）
- 每个 stage 用哪个模型实例 + 怎么裁剪
- 每个 stage 的产物落到哪个对象的哪个字段

**影响范围**：零代码修改。`AlgCreate` 时传入新 JSON 路径即可。

---

## 8. 设计取舍

### 为什么前处理是"一个类 + JSON"而非"按模型派生子类"

端侧前处理的操作组合空间很小：
```
{resize | letterbox_tl | letterbox_center}
× {BGR | RGB | Gray}
× {NCHW | NHWC}
× {可选 mean/std/scale}
× {可选 NV21/NV12 → BGR 平面分离}
```

让每个模型派生一个 `IPreprocessor` 子类，会出现 N 份 95% 重复的代码。
做成 `PreprocessConfig` 数据驱动，每个模型在 JSON 里描述自己的输入需求就够了。

抽象基类**保留**，给真正异形的前处理（多 crop 拼接、3D 输入等）留扩展点。

### 为什么 `TensorView` 是非拥有式

| 方案                     | 后果                                                    |
|--------------------------|---------------------------------------------------------|
| `TensorView` 拥有内存    | 芯片专属分配器（MMZ 等）必须暴露到核心 → 抽象失败          |
| `TensorView` 非拥有式（当前） | 内存由 IInferer 私有；TensorView 只是描述符 → 解耦干净       |

代价：调用方必须明白"`data` 的生命周期跟着 IInferer"。这一点用注释 + 编译期 `const`（output view 是 const TensorView&）来约束。

### 为什么业务编排走 JSON 而不是 C++ 子类

替代方案是每个业务派生一个 `ISolution`（traffic_light / speed_limit / ...），
但实际上端侧业务的迭代频率比模型/芯片高一个数量级：

- 检测阈值要 A/B 测试 → 改 JSON 不重编
- ROI 裁剪扩边比例要调 → 改 JSON 不重编
- 增加一个识别头 → 加一行 JSON
- 减少一个 stage → 删一行 JSON

JSON 化让算法工程师不依赖 C++ 工程师就能完成产品迭代。
C++ 侧只暴露**通用机制**（裁剪 + 字段合并 + 坐标反映射）。

### 为什么 ABI 用 `field_mask` 而不是多个独立结构

输出形态由 solution 决定：
- 单阶段检测：仅 box
- 检测 + 识别：box + attributes（attributes 里再装 class / category / 联合置信度）

用 `field_mask` + 可空 attributes 子指针的好处：
- 用 union：要么很难表达"既有 box 又有 attributes"，要么内部需要标签字段（等于 field_mask）
- 每业务一个独立的 result 结构：C API 爆炸，应用侧难写通用代码
- 用 `field_mask` + 可空子指针：单一稳定 ABI，组合性自然

代价：应用方必须先查 `field_mask & ALG_FIELD_X` 再用 `objects[i].x`。约束清晰，可接受。

### 为什么自注册工厂上要挂 `__attribute__((used))`

release 链接默认带 `-Wl,--gc-sections` + `-fdata-sections`。注册器是文件作用域的静态对象，
对外**没有任何引用**（注册的副作用全靠构造函数），链接器会判定为死代码、丢掉整个 section，
注册不发生，运行时 `Create("yolox_det")` 返回 `nullptr`。

`__attribute__((used))` 告诉编译器"无论看上去有没有引用，都保留"。这是自注册工厂模式
在 release 构建下的标准坑。
