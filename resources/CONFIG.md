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
| `crop.pad_value` | int | no | `-1` | `>=0`：扩边框越界处用该灰度值填充、保正方不形变（等价训练端 `cv2.warpAffine(borderValue=…)`）；`-1`：旧行为，clamp 到图内（越界时 ROI 非正方，下游 resize 会形变）。逐位 OCR 等对裁剪几何敏感的分类器应设为训练裁剪用的灰边值（限速牌为 `114`） |
| `produces` | string | no | `"objects"` | Output routing (see below) |
| `score_threshold` | float | no | `0.0` | 仅 `classify_into:`：合并后联合分 `det_score × cls_conf`（最终对外 `box.score`）低于此值则 drop。检测/分类各自的 `conf_threshold` 只卡各自分数，两头都勉强过线时乘积仍可能偏低，此项按联合分兜底过滤。`0` = 关闭 |
| `passthrough` | array | no | `[]` | 仅 `objects_from:`：**终端类透传**。上游某 `label` 的框不进本 stage 子模型，直接打 `category`/牌种透出（见下） |
| `min_box_short` | object | no | `{}` | `{ "<category>": <px> }`：合并/透传后的框，若其 `category` 命中且**框短边 < 阈值**则 drop（精度优先，如禁停 `nopark_min_size`） |

**`passthrough[*]`**（v3.5：Stage1 终端类如 `pare` 八边形，不进 Stage2 OCR 直接输出）：

| Key | Type | Default | Description |
|---|---|---|---|
| `label` | int | **必填** | 上游检测的类别 idx（如 pare=1）。命中即跳过裁剪+子模型 |
| `category` | string | **必填** | 透出的 `category`（`FillAlgResult` 据此分桶，如 `"pare"`） |
| `sign_value` | int | `0` | 写入 `object.value`（如 `AlgSignType` 的 `SIGN_PARE=2`） |
| `min_score` | float | `0.0` | 检测分 `box.score` 低于此值则 drop（`0`=不额外过滤；pare 弱类易误检可设 `0.85`，对齐 deploy 的 `pare_score_thr`） |

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

打分（与训练 `model_src/postprocess.py` 一致）：`score = sigmoid(obj) × softmax(cls)`。
cls 分支用 **softmax**（非 sigmoid）：单类时 softmax 恒为 1 → `score = sigmoid(obj)`；
该 cls 通道在单类下 CE 梯度为 0、从未被训练，若误用 sigmoid 会乘上任意值把分数压低导致漏检。

| Key | Type | Default | Description |
|---|---|---|---|
| `num_classes` | int | `1` | 检测类别数 |
| `bbox_channels` | int | `4` | bbox 回归通道数 (tx, ty, tw, th) |
| `obj_channels` | int | `1` | objectness 通道数 |
| `stride` | int | `8` | 特征图步长（标量） |
| `anchor` | `[w, h]` | `[36, 36]` | 锚框尺寸（像素） |
| `conf_threshold` | float | `0.25` | 置信度阈值（对 `sigmoid(obj)×softmax(cls)`） |
| `nms_threshold` | float | `0.45` | NMS IoU 阈值 |
| `max_det` | int | `64` | NMS 后最大保留数 |
| `obj_prefilter` | float | `0.05` | objectness 早剪枝阈值 |

### type: "ocr_classifier" -- 定长多位字符 OCR 分类器 + 门控（限速牌 v3.5，当前方案）

逐位读数字的 OCR 网络后处理。对接训练侧
`alg_speed_limit/src/classifier_src/classifier.py` 的 SpeedSignOCR（P=3 位：百/十/个位，
每位 `num_chars`=11 = `'0'..'9'` + blank）。后处理**自动适配两种模型输出布局**：

- **单输出模式（推荐，XMM 板端默认）**：模型出 1 个 `(1, P*num_chars)`/`(1, P, num_chars)`
  张量（如 `(1,33)`），各位在该张量内按 `[p*num_chars, p*num_chars+num_chars)` 连续切片。
  `NumOutputs==1 且 P>1` 时自动启用。
- **多输出头模式（向后兼容）**：模型出 P 个 `(1, num_chars)` 张量，按 name/index 解析各头。
  `NumOutputs>=P` 时启用。

解码完全由 `class_names` 推导（**不硬编码**字符表）：每个类名按数字串右对齐拆成 P 位字符
（缺位补 blank），如 `"90"→(blank,9,0)`、`"120"→(1,2,0)`，据此构建解码 LUT；推理时各位
`softmax + argmax` 得 `(h,t,u)`，查 LUT 得 `cls_id`，非法组合（含个位非 `'0'`）→ 拒识。
联合置信度 `cls_conf = min(各位 max-prob)`，`< conf_threshold` → 丢弃（开放集兜底）。
输出 Object 的 `value = 类名数值 ÷ 10`（即 `AlgSpeedLimitValue`），与 `class_names` 排列顺序无关。

**v3.5 门控（3 路 other/speed/no_parking）**：`gate_channels > 0` 时启用。门控向量 `argmax` 定牌种：
- `speed`（且 `P(限速) ≥ gate_threshold`）→ 走上面的 OCR 解码，输出限速值（`category` = `category` 参数，如 `speed_limit`）
- `no_parking` → **不读 OCR**，直接产出禁停正类：`category` = `nopark_category`（如 `"no_parking"`）、
  `value` = `nopark_sign_value`（`AlgSignType` 的 `SIGN_NO_PARKING=1`）、`box.score` = `P(禁停)`。
  `FillAlgResult` 据 `category` 分桶到 `AlgResult.signs[]`。禁停的最小框尺寸（`nopark_min_size`）由
  **stage 的 `min_box_short`**（按 `category` 键）施加——分类器拿不到原图框尺寸，故放在 ChainSolution。
- `other`（或 `P(限速) < gate_threshold`）→ 拒识（返回空 → `classify_into` drop 掉 src 框）

门控位置：**单输出模式**在 `output[0]` 内、偏移 `P*num_chars`（如 `(1,36)` 的末 3 路）；
**多输出模式**是独立门控头（`gate_head_name`/`gate_head_index`，如 MNN 的 `logits_gate`）。
`gate_channels=0`（默认）= 无门控纯 OCR（向后兼容旧模型）。

> **为何默认单输出**：三头各带不被 NPU 支持的算子，XMM 导出时图被切成 NPU+CPU 混合多输出，
> 板端 runtime 只落 head0、head1/head2 不产出 → **0 检出**（详见提交 575bc8d）。改单输出
> 后该坑消除（检测器单输出在该板一直正常）。训练侧 `export_onnx.py` 已默认导出单输出
> `(1,33)`，`--multi-output` 保留旧三头供 MNN/调试。
>
> 多输出头模式下三头同形状 `(1,11)` 无法靠 size 区分顺序，后处理**优先按输出张量 name 匹配**
> （`head_names`），name 不可用（如 XMM 输出名为空）时退回 `head_indices`；MNN 会按张量名
> 保留输出，故 name 匹配可覆盖 MNN 输出顺序被打乱的情况。单输出模式下 `head_names`/`head_indices`
> 不参与（各位都读 `output[0]`）。

| Key | Type | Default | Description |
|---|---|---|---|
| `num_positions` | int | 由 `class_names` 最长位数推导 | 位数 = 头数（高位→低位） |
| `num_chars` | int | `11` | 每头类别数（`'0'..'9'` + blank） |
| `blank_index` | int | `10` | 占位符在字符表中的下标 |
| `head_names` | `[str]` | `[]` | **仅多输出模式**：按名匹配各位对应的输出张量（高位→低位），推荐填 |
| `head_indices` | `[int]` | `[0,1,..]` | **仅多输出模式**：name 不可用时的索引兜底（高位→低位） |
| `conf_threshold` | float | `0.5` | `min(各位 prob)` 低于此值 → drop |
| `class_names` | `[str]` | **必填** | 类名列表（限速字符串，解码 LUT 与 value 的唯一来源） |
| `category` | string | `""` | 分类器类别标签（如 `"speed_limit"`） |
| `gate_channels` | int | `0` | 门控路数（v3.5=3）；`0`=无门控纯 OCR。单输出时门控在 OCR 段之后 |
| `gate_speed_index` | int | `1` | `P(限速)` 在门控向量里的下标 |
| `gate_nopark_index` | int | `2` | `P(禁停)` 在门控向量里的下标 |
| `gate_threshold` | float | `0.5` | `P(限速) < 此值` → 非限速牌拒识（`gate_channels>0` 生效） |
| `gate_head_name` | string | `""` | **仅多输出**：门控头张量名（如 `"logits_gate"`） |
| `gate_head_index` | int | `num_positions` | **仅多输出**：门控头索引兜底（默认 = OCR 头之后） |
| `nopark_category` | string | `""` | 门控判 no_parking 时产出的 `category`（如 `"no_parking"`，分桶到 `signs[]`） |
| `nopark_sign_value` | int | `0` | 门控判 no_parking 时写入 `value`（`AlgSignType`，`SIGN_NO_PARKING=1`） |

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
