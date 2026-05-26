# Pipeline JSON Configuration Reference

## Top-level structure

```json
{
  "solution": { ... },
  "models": { ... }
}
```

Both keys are required.

---

## solution

| Key | Type | Required | Default | Description |
|---|---|---|---|---|
| `type` | string | no | `"chain"` | Execution strategy, currently only `"chain"` |
| `stages` | array | **yes** | - | Ordered pipeline stages (non-empty) |

### solution.stages[*]

| Key | Type | Required | Default | Description |
|---|---|---|---|---|
| `name` | string | **yes** | - | Unique stage ID, referenced by later stages |
| `model` | string | **yes** | - | Key into top-level `models` object |
| `input` | string | no | `"image"` | `"image"` or `"objects_from:<stage>"` |
| `crop` | object | no | - | Crop config, only for `objects_from:` input |
| `crop.expand_ratio` | float | no | `1.0` | Bbox expansion factor before cropping |
| `crop.square` | bool | no | `false` | Expand crop to square |
| `produces` | string | no | `"objects"` | Output routing (see below) |

`produces` 可选值：
- `"objects"` -- 产出新的顶层检测框
- `"classify_into:<stage>"` -- 分类结果合并到目标 stage 的检测框
- `"attributes_into:<stage>"` -- 属性写入目标 stage 的检测框
- `"keypoints_into:<stage>"` / `"mask_into:<stage>"` -- 预留

---

## models

`models` 是一个对象，每个 key 是自定义的模型实例名，value 包含该模型的完整配置。

### models.\<name\>

| Key | Type | Required | Description |
|---|---|---|---|
| `model_path` | string | **yes** | 模型文件路径 |
| `preprocess` | object | **yes** | 前处理配置 |
| `postprocess` | object | **yes** | 后处理配置（必须包含 `type` 字段） |

---

## models.\<name\>.preprocess

| Key | Type | Required | Default | Description |
|---|---|---|---|---|
| `input_size` | `[w, h]` | yes* | - | 网络输入尺寸，与 `input_width`/`input_height` 二选一 |
| `input_width` | int | fallback | `0` | 网络输入宽度（`input_size` 不存在时使用） |
| `input_height` | int | fallback | `0` | 网络输入高度（`input_size` 不存在时使用） |
| `color` | string | no | `"BGR"` | 像素颜色顺序：`"BGR"` / `"RGB"` / `"GRAY"` |
| `resize` | string | no | `"letterbox_tl"` | 缩放策略（见下表） |
| `layout` | string | no | `"NCHW"` | Tensor 内存布局：`"NCHW"` / `"NHWC"` |
| `mean` | `[f, f, f]` | no | `[0, 0, 0]` | 归一化均值（逐通道） |
| `std` | `[f, f, f]` | no | `[1, 1, 1]` | 归一化标准差（逐通道） |
| `scale` | float | no | `1.0` | 全局缩放系数 |
| `pad_value` | int | no | `0` | letterbox 填充值 (0-255) |

归一化公式：`y = (x - mean) * (scale / std)`

**resize 策略：**

| 值 | 说明 |
|---|---|
| `"stretch"` | 直接拉伸，会改变宽高比 |
| `"letterbox_tl"` | 保持宽高比，padding 补到左上 |
| `"letterbox_center"` | 保持宽高比，padding 居中 |

---

## models.\<name\>.postprocess

`type` 字段决定使用哪个后处理器，其余参数由具体后处理器解读。

### type: "yolox_det" -- YOLOX 多尺度无锚框检测

期望 N 个输出 tensor（每个 stride 一个），形状 `(1, bbox+obj+cls, H, W)` 或 `(1, H, W, C)`。

channel 布局：`[bbox_channels..., obj_channels..., num_classes...]`

| Key | Type | Default | Description |
|---|---|---|---|
| `num_classes` | int | `1` | 检测类别数 |
| `bbox_channels` | int | `4` | bbox 回归通道数 (cx, cy, w, h) |
| `obj_channels` | int | `1` | objectness 通道数 |
| `strides` | `[int]` | `[8, 16, 32]` | 特征图步长，每个元素对应一个输出 tensor |
| `conf_threshold` | float | `0.4` | 置信度阈值 |
| `nms_threshold` | float | `0.5` | NMS IoU 阈值 |
| `max_det` | int | `100` | NMS 后最大保留数 |
| `obj_prefilter` | float | `0.05` | objectness 早剪枝阈值 |
| `class_names` | `[str]` | `[]` | 类名列表，提供时输出 Object 附带 `"class"` 属性 |
| `category` | string | `""` | 检测器类别标签，提供时输出 Object 附带 `"category"` 属性 |

### type: "yolov5_anchor_det" -- YOLOv5 锚框单尺度检测

期望 1 个输出 tensor，channel 布局同 yolox_det。

| Key | Type | Default | Description |
|---|---|---|---|
| `num_classes` | int | `1` | 检测类别数 |
| `bbox_channels` | int | `4` | bbox 回归通道数 (tx, ty, tw, th) |
| `obj_channels` | int | `1` | objectness 通道数 |
| `stride` | int | `8` | 特征图步长（标量） |
| `anchor` | `[w, h]` | `[36, 36]` | 锚框尺寸（像素） |
| `conf_threshold` | float | `0.25` | 置信度阈值 |
| `nms_threshold` | float | `0.45` | NMS IoU 阈值 |
| `max_det` | int | `64` | NMS 后最大保留数 |
| `obj_prefilter` | float | `0.05` | objectness 早剪枝阈值 |

### type: "ocr_classifier" -- 定长多位字符 OCR 分类器（限速牌 v3.4，当前方案）

逐位读数字的 OCR 网络后处理：期望 P 个输出 tensor（P = 位数，默认 3 = 百/十/个位），
每个形状 `(1, num_chars)`（默认 11 = `'0'..'9'` + blank）。对接训练侧
`alg_speed_limit/src/classifier_src/classifier.py` 的 SpeedSignOCR 三头。

解码完全由 `class_names` 推导（**不硬编码**字符表）：每个类名按数字串右对齐拆成 P 位字符
（缺位补 blank），如 `"90"→(blank,9,0)`、`"120"→(1,2,0)`，据此构建解码 LUT；推理时三头各
`softmax + argmax` 得 `(h,t,u)`，查 LUT 得 `cls_id`，非法组合（含个位非 `'0'`）→ 拒识。
联合置信度 `cls_conf = min(三头 max-prob)`，`< conf_threshold` → 丢弃（开放集兜底）。
输出 Object 的 `value = 类名数值 ÷ 10`（即 `AlgSpeedLimitValue`），与 `class_names` 排列顺序无关。

> 三头同形状 `(1,11)`，无法靠 size 区分。后处理**优先按输出张量 name 匹配**（`head_names`），
> name 不可用（如 XMM 输出名为空）时退回 `head_indices`。MNN 会按张量名保留输出，故
> name 匹配可覆盖 MNN 输出顺序被打乱的情况。

| Key | Type | Default | Description |
|---|---|---|---|
| `num_positions` | int | 由 `class_names` 最长位数推导 | 位数 = 头数（高位→低位） |
| `num_chars` | int | `11` | 每头类别数（`'0'..'9'` + blank） |
| `blank_index` | int | `10` | 占位符在字符表中的下标 |
| `head_names` | `[str]` | `[]` | 按名匹配各位对应的输出张量（高位→低位），推荐填 |
| `head_indices` | `[int]` | `[0,1,..]` | name 不可用时的索引兜底（高位→低位） |
| `conf_threshold` | float | `0.5` | `min(三头 prob)` 低于此值 → drop |
| `class_names` | `[str]` | **必填** | 类名列表（限速字符串，解码 LUT 与 value 的唯一来源） |
| `category` | string | `""` | 分类器类别标签（如 `"speed_limit"`） |

### type: "dualhead_classifier" -- 双头 softmax 分类器（旧方案，保留向后兼容）

> v3.4 起限速牌改用 `ocr_classifier`（支持 90/110/120）。本类型仍注册可用，仅作向后兼容。

期望 2 个输出 tensor（head_a: 首位数字, head_b: 位数判断）。

组装规则：`argmax(head_b) == is_3digit_class` 时 `cls_id = three_digit_class_index`，否则 `cls_id = argmax(head_a)`。

| Key | Type | Default | Description |
|---|---|---|---|
| `head_a_index` | int | `0` | 第一个输出 tensor 索引 |
| `head_b_index` | int | `1` | 第二个输出 tensor 索引 |
| `head_a_classes` | int | `8` | head_a 类别数（首位数字） |
| `head_b_classes` | int | `2` | head_b 类别数（位数判断） |
| `is_3digit_class` | int | `1` | head_b 中表示"三位数"的 argmax 索引 |
| `three_digit_class_index` | int | `head_a_classes` | 三位数时的最终类别 ID |
| `conf_threshold` | float | `0.7` | 联合置信度阈值 |
| `class_names` | `[str]` | `[]` | 类名列表 |
| `category` | string | `""` | 分类器类别标签 |

---

## 完整示例

见 [all.json](all.json)，包含三种后处理类型的完整三阶段链式 pipeline（检测 -> 检测 -> 分类）。
