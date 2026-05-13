# alg_sdk — 红绿灯检测 + 巴西限速牌识别

面向端侧 CV 推理的 C++ SDK。本分支 (`tld_speedlimit`) 在 master 通用框架基础上裁掉
原通用示例，聚焦两个落地模型：

- **红绿灯检测 (`traffic_light.json`)** — 单阶段 YOLOX，416×416 RGB，4 类
  red/yellow/green/off；对应训练侧 [`alg_traffic_light_detection`](../alg_traffic_light_detection/)。
- **巴西限速牌识别 (`speed_limit.json`)** — 二阶段 SpeedSignNet 检测 + 双头数字识别
  装配 9 类 10/20/.../80/100；对应训练侧 [`alg_speed_limit`](../alg_speed_limit/)。
  分类置信度 < 0.7 的框由 `classify_into:` 语义直接 drop。
- **二合一并行 (`all.json`)** — 上面两个 solution 在同一份配置里跑；ChainSolution 把
  多个 `produces:"objects"` 的 stage 在最终聚合处取并集，两个检测器互不依赖。

应用区分两个检测器的输出统一通过 attributes：

| attribute name | value_str 示例                              | 含义                              |
|----------------|---------------------------------------------|-----------------------------------|
| `category`     | `"traffic_light"` / `"speed_limit"`         | 这个框是哪个检测器产出的           |
| `class`        | `"red_light"` / `"green_light"` / `"60"`     | 实际类别名（避开 box.label 命名空间冲突） |

三个变化维度仍然全部解耦：

- **换芯片** = 只重写一个 `IInferer` 实现。
- **加模型** = 新建一个目录，派生一个 `IPostprocessor` + 一行 `REGISTER_ALG_POST` 注册。
- **串业务**（检测 → 分类 → ...）= 不写 C++，**写一份 JSON 配置**就行。
- **调阈值 / 改输入尺寸 / 换均值方差** = 改 JSON，**不需要重新编译**。

## 目录结构（本分支）

```
include/                       公共 C ABI（alg_types.h, alg_interface.h）
resources/
  traffic_light.json           红绿灯检测：单阶段 YOLOX
  speed_limit.json             限速牌：检测 + 双头分类（带 classify_into 过滤）
  all.json                     上述两条 solution 合并的并行版本（一份配置跑两件事）

src/
  interface/                   C API → ChainSolution 胶水层
  core/
    object.h/cpp               内部 C++ Object（box + attributes + drop 标记）
    tensor.h                   芯片中立的 TensorView
    status.h logger.h
    config/                    JSON → SolutionConfig 解析层
    infer/inferer.h            IInferer（每芯片一份实现）
    preprocess/                IPreprocessor + 通用 LetterboxPreprocessor
    postprocess/               IPostprocessor 基类 + 通用 NMS
    registry/                  后处理类型注册表（按名字 → builder）
    instance/                  ModelInstance（单个网络的 pre/infer/post 三件套）
    solution/                  ChainSolution（多模型编排，含 classify_into）
                               + CropFromBox 工具

  models/                      每种模型类型一个目录
    yolox_det/                 mmyolo YOLOXHead 多尺度（红绿灯）
    yolov5_anchor_det/         单尺度 anchor + sigmoid 解码（SpeedSignNet）
    dualhead_classifier/       双头数字识别 + 装配 + 0.7 置信度过滤

  backend/                     每种芯片一个目录
    xmm/                       XMM（xmedia_cl + MMZ）
    rk/                        Rockchip RKNN（桩示例）

cmake/CMakeLists_linux_aarch64.cmake
test/test_runner.cpp           通用 runner：./test_runner <solution.json> <image>
```

## 公共 API

```c
AlgHandle h = NULL;
AlgCreate(&h, "/data/traffic_light.json");           // 或 speed_limit.json
AlgRun(h, &image, &result);

for (int i = 0; i < result.object_count; ++i) {
    AlgObject* o = &result.objects[i];
    if (o->field_mask & ALG_FIELD_BOX)        use_box(&o->box);
    if (o->field_mask & ALG_FIELD_ATTRIBUTES) use_attrs(o->attributes);
    /* 限速牌：box.label = 9 类 idx，box.score = det × cls 联合置信度，
     *         attributes[0] = { name="class",    value_str="60",            value_int=5, ... }
     *         attributes[1] = { name="category", value_str="speed_limit",   ...           } */
}

AlgFreeResult(&result);
AlgDestroy(h);
```

## JSON 配置

### 红绿灯（单阶段 YOLOX）

```json
{
  "solution": { "type": "chain", "stages": [
    { "name": "tld", "model": "tld_cfg", "input": "image", "produces": "objects" }
  ]},
  "models": {
    "tld_cfg": {
      "model_path": "/data/traffic_light_int8.xmm",
      "preprocess":  { "input_size": [416, 416], "color": "RGB",
                       "resize": "letterbox_tl", "layout": "NCHW", "pad_value": 114 },
      "postprocess": { "type": "yolox_det",
                       "num_classes": 4, "strides": [8, 16, 32],
                       "conf_threshold": 0.4, "nms_threshold": 0.5,
                       "class_names": ["red_light","yellow_light","green_light","off_light"] }
    }
  }
}
```

### 限速牌（检测 + 双头分类，带 classify_into 过滤）

```json
{
  "solution": { "type": "chain", "stages": [
    { "name": "detector",   "model": "ssn_cfg", "input": "image",
      "produces": "objects" },
    { "name": "classifier", "model": "cls_cfg",
      "input": "objects_from:detector",
      "crop":  { "expand_ratio": 1.5, "square": true },
      "produces": "classify_into:detector" }
  ]},
  "models": {
    "ssn_cfg": {
      "model_path": "/data/speedsignnet.xmm",
      "preprocess":  { "input_size": [576, 320], "color": "RGB",
                       "resize": "letterbox_center", "pad_value": 114 },
      "postprocess": { "type": "yolov5_anchor_det",
                       "num_classes": 1, "stride": 8, "anchor": [36, 36],
                       "conf_threshold": 0.25, "nms_threshold": 0.45 }
    },
    "cls_cfg": {
      "model_path": "/data/classifier.xmm",
      "preprocess":  { "input_size": [64, 64], "color": "RGB",
                       "resize": "stretch" },
      "postprocess": { "type": "dualhead_classifier",
                       "head_a_classes": 8, "head_b_classes": 2,
                       "is_3digit_class": 1, "three_digit_class_index": 8,
                       "conf_threshold": 0.7,
                       "class_names": ["10","20","30","40","50","60","70","80","100"] }
    }
  }
}
```

### Stage 语义新增

| produces 取值                    | 行为                                                   |
|----------------------------------|--------------------------------------------------------|
| `"objects"`                      | 本 stage 自己产生 top-level 对象                        |
| `"attributes_into:<stage>"`      | 把属性数组合并到上游 stage 的 box                       |
| **`"classify_into:<stage>"`**    | **分类器：合并 attributes 到 src，box.label 改成分类 id，box.score 乘以分类置信度；分类器返回空（如低于阈值）→ 直接 drop 掉这个 src 框** |

`classify_into:` 是本分支为支持「检测 + 识别 + 阈值过滤」二阶段链路新增的语义，
框架最小改动：
- `Object` 加了一个内部 `drop` 标记（不暴露到 C ABI）；
- `ChainSolution::Run` 在最终聚合时跳过 `drop=true` 的对象。

## 编译

```bash
./build.sh linux aarch64 xmm
# 产物：build_linux_aarch64_xmm/libalg_sdk.so + test_runner

./test_runner resources/traffic_light.json /data/test.jpg out/
./test_runner resources/speed_limit.json   /data/test.jpg out/
./test_runner resources/all.json           /data/test.jpg out/      # 两件事一起跑
```

依赖：jsoncpp 静态库（路径通过 `-DJSONCPP_ROOT=...` 配置，默认
`/root/opensource/jsoncpp/build_arm/install`）。

## 训练侧契约对照

| 项                  | 红绿灯 (yolox_det)              | 限速牌 (yolov5_anchor_det + dualhead_classifier) |
|---------------------|---------------------------------|---------------------------------------------------|
| 训练框架            | mmyolo 0.6.0                    | 自研（YOLOv5 风格 head + 双头分类）               |
| 输入分辨率          | 416×416 RGB                     | 320×576 RGB（检测）/ 64×64 RGB（分类）            |
| letterbox pad_value | 114                             | 114                                               |
| 归一化              | NPU 入口 scale=255 内部完成      | (x-mean)/std 烘进量化模型                         |
| 检测 head channels  | 9 = 4(box) + 1(obj) + 4(cls)    | 6 = 4(box) + 1(obj) + 1(cls)                      |
| 解码 grid offset    | 0（mmdet `MlvlPointGenerator`） | YOLOv5 `(σ*2-0.5+grid)*stride`                    |
| 解码 wh             | `exp(w_log) * stride`           | `(σ(t)*2)^2 * anchor`                             |
| score 公式          | `σ(obj) * σ(max(cls))`          | `σ(obj) * σ(cls)`                                 |
| NMS                 | class-aware                     | class-aware                                       |
| 默认阈值            | conf 0.4 / iou 0.5              | conf 0.25 / iou 0.45（检测）+ joint 0.7（分类）    |

## 如何加新模型 / 换芯片

参考 `ARCHITECTURE.md`，三轴正交。本分支聚焦红绿灯 + 限速牌两个落地模型，C ABI 仅
保留 box + attributes（不含 keypoints / embedding）；如需关键点 / embedding 等其它
输出形态，回到 master 分支或基于 master 拉新分支扩展。
