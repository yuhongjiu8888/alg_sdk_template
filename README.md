# alg_sdk

面向端侧 CV 推理的 C++ SDK。三个变化维度全部解耦：

- **换芯片** = 只重写一个 `IInferer` 实现。
- **加模型** = 新建一个目录，派生一个 `IPostprocessor` + 一行 `REGISTER_ALG_POST` 注册。
- **串业务**（检测 → 关键点 → 属性 → ...）= 不写 C++，**写一份 JSON 配置**就行。
- **调阈值 / 改输入尺寸 / 换均值方差** = 改 JSON，**不需要重新编译**。

## 目录结构

```
include/                       公共 C ABI（alg_types.h, alg_interface.h）
resources/                     示例 JSON 配置
  face_only.json               单模型：纯人脸检测
  face_full.json               三模型链：检测 → 关键点 → 属性

src/
  interface/                   C API → ChainSolution 胶水层
  core/
    object.h/cpp               内部 C++ Object（Solution 操作的对象表示）
    tensor.h                   芯片中立的 TensorView
    status.h logger.h
    config/                    JSON → SolutionConfig 解析层
    infer/inferer.h            IInferer（每芯片一份实现）
    preprocess/                IPreprocessor + 通用 LetterboxPreprocessor
    postprocess/               IPostprocessor 基类 + 通用 NMS
    registry/                  后处理类型注册表（按名字 → builder）
    instance/                  ModelInstance（单个网络的 pre/infer/post 三件套）
    solution/                  ChainSolution（多模型编排）+ CropFromBox 工具
  models/                      每种模型类型一个目录
    fcos_face/                 FCOS 人脸检测
    pfld_landmark/             PFLD 风格 N 点关键点
    face_attribute/            多任务人脸属性（softmax/sigmoid/regress）
  backend/                     每种芯片一个目录
    xmm/                       XMM（xmedia_cl + MMZ）
    rk/                        Rockchip RKNN（桩示例）

cmake/CMakeLists_linux_aarch64.cmake
test/test_facedet.cpp
```

## 三层抽象（每层用一个纯虚类）

| 抽象类           | 角色                | 位置                                  | 何时派生              |
|------------------|---------------------|---------------------------------------|-----------------------|
| `IInferer`       | 芯片前向推理         | `src/backend/<chip>/`                 | 移植新芯片            |
| `IPostprocessor` | 单网络的后处理       | `src/models/<type>/`                  | 新增模型类型          |
| `IPreprocessor`  | 前处理               | `src/core/preprocess/`                | 出现异形前处理才派生（通常不需要） |

`ChainSolution` 在最外层把多个 `ModelInstance` 串起来；它本身是**唯一**的业务编排实现，
具体业务流靠 JSON 描述。

## 公共 API

```c
AlgHandle h = NULL;
AlgCreate(&h, "/data/face_full.json");
AlgRun(h, &image, &result);

for (int i = 0; i < result.object_count; ++i) {
    AlgObject* o = &result.objects[i];
    if (o->field_mask & ALG_FIELD_BOX)        use_box(&o->box);
    if (o->field_mask & ALG_FIELD_KEYPOINTS)  use_kps(o->keypoints);
    if (o->field_mask & ALG_FIELD_ATTRIBUTES) use_attrs(o->attributes);
    if (o->field_mask & ALG_FIELD_EMBEDDING)  use_emb(o->embedding);
}

AlgFreeResult(&result);
AlgDestroy(h);
```

输出结构 `AlgObject` 用 `field_mask` 标记自己带了哪些子结果。同一个 SDK 既能跑
"只检测"（仅 BOX 位），也能跑"检测+关键点+属性"（BOX|KEYPOINTS|ATTRIBUTES 三位都置 1）。

## JSON 配置怎么写

一份完整的人脸全套链路（见 `resources/face_full.json`）：

```json
{
  "solution": {
    "type": "chain",
    "stages": [
      { "name": "detector",  "model": "detector_cfg", "input": "image",
        "produces": "objects" },
      { "name": "landmark",  "model": "landmark_cfg",
        "input": "objects_from:detector",
        "crop":  { "expand_ratio": 1.25, "square": true },
        "produces": "keypoints_into:detector" },
      { "name": "attribute", "model": "attribute_cfg",
        "input": "objects_from:detector",
        "crop":  { "expand_ratio": 1.10, "square": true },
        "produces": "attributes_into:detector" }
    ]
  },
  "models": {
    "detector_cfg": {
      "model_path": "/data/face_det.xmm",
      "preprocess":  { "input_size": [320, 320], "color": "BGR",
                       "resize": "letterbox_tl", "layout": "NCHW" },
      "postprocess": { "type": "fcos_face",
                       "conf_threshold": 0.6, "nms_threshold": 0.4,
                       "strides": [4, 8, 16, 32] }
    },
    "landmark_cfg":  { "...": "见 face_full.json" },
    "attribute_cfg": { "...": "见 face_full.json" }
  }
}
```

### Stage 语义

- `input`：
  - `"image"` —— 把原帧喂给本 stage 的模型（常用于第一个检测 stage）
  - `"objects_from:<stage_name>"` —— 遍历之前某个 stage 产出的每个对象，**逐个**裁剪后喂给本 stage 的模型
- `crop`：当 `input` 是 `objects_from:...` 时生效。`expand_ratio` 在 box 周围按比例扩边，
  `square` 把扩边后的区域补成正方形（对关键点对齐很重要）
- `produces`：
  - `"objects"` —— 本 stage 自己就是 detector，产出 top-level 对象
  - `"keypoints_into:<stage>"` —— 把本 stage 的关键点产物**合并回**那个 stage 的对应对象
  - `"attributes_into:<stage>"` —— 同上，合并到 attributes 字段
  - `"embedding_into:<stage>"` —— 同上，合并到 embedding 字段

ChainSolution 会自动把子模型在 crop 坐标系输出的关键点坐标加上 crop offset 映射回原帧。

### Preprocess 字段

| 字段          | 含义                                                              |
|---------------|-------------------------------------------------------------------|
| `input_size`  | `[w, h]`，网络输入分辨率                                          |
| `color`       | `"BGR"` / `"RGB"` / `"GRAY"`                                       |
| `resize`      | `"stretch"` / `"letterbox_tl"`（左上对齐）/ `"letterbox_center"`   |
| `layout`      | `"NCHW"`（目前仅支持此项）                                         |
| `mean`, `std` | `[c0,c1,c2]`，浮点输入做 `(x-mean)/std * scale`                    |
| `scale`       | 全局缩放系数（默认 1）                                             |
| `pad_value`   | letterbox 填充值                                                   |

## 编译

```bash
./build.sh linux aarch64 xmm
# 产物：build_linux_aarch64_xmm/libalg_sdk.so + test_facedet

./test_facedet resources/face_full.json /data/test.jpg out/
```

依赖：jsoncpp 静态库（路径通过 `-DJSONCPP_ROOT=...` 配置，默认
`/root/opensource/jsoncpp/build_arm/install`）。

## 如何加新模型（举例：人脸特征 embedding）

1. `mkdir src/models/face_recognition/` 并写一个后处理类：
   ```cpp
   class FaceRecognitionPost : public IPostprocessor {
     Status Configure(const IInferer&, const Json::Value& params) override;
     Status Apply(const IInferer&, const PreprocessState&,
                  std::vector<Object>* out) override;
   };
   ```
   `Apply` 里把 embedding 向量塞进 `Object::embedding`，置 `field_mask |= ALG_FIELD_EMBEDDING`。
2. 加 `face_recognition_register.cpp`：
   ```cpp
   REGISTER_ALG_POST("face_recognition", []{
       return std::unique_ptr<IPostprocessor>(new FaceRecognitionPost());
   });
   ```
3. 在 CMake 的 `ALG_MODEL_SRCS` 追加两行。
4. 业务 JSON 加一个 stage：
   ```json
   { "name": "feature", "model": "feature_cfg",
     "input": "objects_from:detector",
     "crop": { "expand_ratio": 1.2, "square": true },
     "produces": "embedding_into:detector" }
   ```

完成。核心代码、芯片后端、C API、Solution 一行都不用动。

## 如何换芯片（举例：Rockchip）

1. `src/backend/rk/rk_inferer.cpp` 里实现 `IInferer::Load` / `Forward`（已有桩示例）。
2. `rk_backend.cpp` 注册工厂入口：
   ```cpp
   std::unique_ptr<IInferer> alg::MakeInferer() { return std::unique_ptr<IInferer>(new RkInferer()); }
   const char*               alg::BackendName() { return "rk"; }
   ```
3. CMake 里给 `ALG_BACKEND STREQUAL "rk"` 那个分支配 RKNN 头文件路径和库。
4. `./build.sh linux aarch64 rk`

模型代码、Solution、C API 都不动。同一份 JSON 配置在新芯片上跑同一条业务链。

## 设计取舍

- **前处理为什么是"一个类 + JSON 配置"**：端侧前处理操作组合空间很小（resize/letterbox/
  颜色转换/归一化/NCHW），让每个模型派生子类会出现 N 份几乎一样的代码。做成数据驱动后，
  每个模型在 JSON 里描述自己的输入需求就够了。
- **TensorView 而不是更"重"的 Tensor 类**：非拥有式描述符。内存由芯片后端持有，
  前/后处理通过 view 读写。前处理 buffer 与芯片输入 buffer 共用一份内存，零拷贝。
- **业务编排为什么不固化在 C++**：业务流（检测哪个模型、裁剪扩多少、关键点合并到哪个对象）
  在产品迭代里改得很频繁。固化在 C++ 里每次都要重编、重发布；写在 JSON 里只需要替换配置。
- **自注册工厂上的 `__attribute__((used))`**：release 链接默认带 `-Wl,--gc-sections`，
  会把"看上去没人引用"的注册代码删掉。`used` 告诉链接器这块代码必须保留。
