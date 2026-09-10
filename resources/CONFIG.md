# alg_sdk JSON 配置说明

业务配置由 `solution` 和 `models` 两个对象组成，分别定义执行阶段和模型实例。本文说明配置字段及处理规则，完整示例位于 [配置目录](config)。

## 顶层结构

`solution` 与 `models` 均为必填项，`models` 和 `solution.stages` 不得为空。模型文件路径以进程工作目录为基准解析；配置修改后需重新创建 SDK 实例。

---

## solution

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|---|---|---|---|---|
| `type` | string | 否 | `"chain"` | 执行策略，本版本仅支持 `"chain"` |
| `stages` | array | 是 | - | 按执行顺序排列的阶段，数组不得为空 |

### solution.stages[*]

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|---|---|---|---|---|
| `name` | string | 是 | - | 阶段名称，应保持唯一，供后续阶段引用 |
| `model` | string | 是 | - | 引用顶层 `models` 中的模型实例名称 |
| `input` | string | 否 | `"image"` | `"image"` 或 `"objects_from:<stage>"` |
| `crop` | object | 否 | - | 裁剪配置，仅用于 `objects_from:` 输入 |
| `crop.expand_ratio` | float | 否 | `1.0` | 围绕检测框中心的裁剪扩展比例 |
| `crop.square` | bool | 否 | `false` | 将裁剪区域扩展为正方形 |
| `crop.pad_value` | int | 否 | `-1` | `0`～`255`：使用指定灰度值填充越界区域，保持裁剪几何；`-1`：裁到图内，越界时裁剪形状可能改变。限速牌识别使用与训练裁剪一致的 `114` |
| `produces` | string | 否 | `"objects"` | 阶段输出规则，见下文 |
| `score_threshold` | float | 否 | `0.0` | 仅 `classify_into:`：联合分 `det_score × cls_conf` 低于此值时丢弃对象；独立于检测和识别各自的阈值。`0` 表示关闭，不作用于透传分支 |
| `passthrough` | array | 否 | `[]` | 仅 `objects_from:`：匹配上游 `label` 后设置 `category` 和业务值，跳过子模型 |
| `min_box_short` | object | 否 | `{}` | `{ "<category>": <px> }`：按类别检查合并或透传后的原图框，短边小于配置值时丢弃 |

### 透传规则 `passthrough[*]`

PARE 等由检测阶段确定的类别采用透传方式，跳过第二阶段 OCR，保留检测置信度。

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `label` | int | **必填** | 上游检测类别索引，例如 PARE 为 1；匹配后跳过裁剪和子模型 |
| `category` | string | **必填** | 写入对象的 `category`，用于公共结果映射，例如 `"pare"` |
| `sign_value` | int | `0` | 写入 `object.value`，例如 `SIGN_PARE=2`；公共牌种仍按 `category` 确定 |
| `min_score` | float | `0.0` | 检测分 `box.score` 低于此值时丢弃，`0` 表示关闭额外过滤；阈值按业务验证结果设置 |

### 输出规则 `produces`

- `"objects"`：生成顶层对象，参与最终结果汇总。
- `"classify_into:<stage>"`：用首个子结果更新上游类别、业务值和属性，将识别分乘到检测分；子结果为空时丢弃上游框。
- `"attributes_into:<stage>"`：用首个子结果替换上游属性集合，不改变检测分。
- `"keypoints_into:<stage>"` / `"mask_into:<stage>"`：写回内部预留字段，公共 C ABI 未定义对应输出。

二阶段输入和写回目标应引用同一上游阶段，例如 `objects_from:detector` 与 `classify_into:detector`。模型引用须存在，阶段引用须指向此前的阶段。

### 固定 ROI

`input="image"` 的阶段可配置 `roi` 对象，字段为 `x`、`y`、`width`、`height`，单位为原图像素。宽高应为正，区域应与实际输入分辨率匹配。SDK 裁剪后执行模型，并将检测框恢复到原图坐标。`objects_from:` 阶段不得同时配置固定 ROI。

---

## models

`models` 以模型实例名称为键，定义各实例的模型路径、前处理和后处理参数。创建 SDK 时会初始化其中的全部模型。

### models.\<name\>

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `model_path` | string | 是 | 模型文件路径 |
| `preprocess` | object | 是 | 前处理配置 |
| `postprocess` | object | 是 | 后处理配置（必须包含 `type` 字段） |

---

## models.\<name\>.preprocess

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|---|---|---|---|---|
| `input_size` | `[w, h]` | 二选一 | - | 网络输入尺寸，与 `input_width`/`input_height` 二选一 |
| `input_width` | int | 备选 | `0` | 网络输入宽度（`input_size` 不存在时使用） |
| `input_height` | int | 备选 | `0` | 网络输入高度（`input_size` 不存在时使用） |
| `color` | string | 否 | `"BGR"` | 像素颜色顺序：`"BGR"` / `"RGB"` / `"GRAY"` |
| `resize` | string | 否 | `"letterbox_tl"` | 缩放策略（见下表） |
| `layout` | string | 否 | `"NCHW"` | 网络输入布局；通用前处理仅输出 `"NCHW"`，配置 `"NHWC"` 返回前处理错误 |
| `mean` | `[f, f, f]` | 否 | `[0, 0, 0]` | 归一化均值（逐通道） |
| `std` | `[f, f, f]` | 否 | `[1, 1, 1]` | 归一化标准差（逐通道） |
| `scale` | float | 否 | `1.0` | 全局缩放系数 |
| `pad_value` | int | 否 | `0` | letterbox 填充值 (0-255) |
| `engine` | string | 否 | `"auto"` | `"auto"` / `"aipp"` / `"opencv"`；动态 AIPP OM 使用 AIPP |
| `input_format` | string | 否 | `"auto"` | `"auto"` / `"NV12"` / `"NV21"`；auto 接受两种格式 |
| `max_input_size` | `[w, h]` | 否 | `[1920, 1080]` | 动态 AIPP OM 允许的最大源图尺寸 |
| `aipp_output_size` | `[w, h]` | 否 | 未设置 | 动态 AIPP 缩放后的有效图尺寸；使用图内补边时必填 |
| `aipp_graph_padding` | `[l,t,r,b]` | 否 | 未设置 | OM 图首常量 Pad 的四边宽度，与 AIPP 输出相加后必须等于 `input_size` |

FLOAT32 输入的有效图像区域采用 `y = (x - mean) * (scale / std)`，`std` 不得为 0；填充区域直接写入 `pad_value`。UINT8 输入要求 `mean=0`、`std=1`、`scale=1`，归一化由模型按其约定完成。输入宽高须与模型实际尺寸一致。

海思配置经 `AlgRun` 接收原分辨率 NV12/NV21 帧。动态 AIPP 完成转色、缩放和归一化；CV610 AIPP 无法生成值为 114 的大范围 padding，因此 `aipp_graph_padding` 对应的常量 Pad 必须在转换前加入模型图。运行时会按实际原图尺寸复算有效区和四边补边，不匹配时返回前处理错误。

ATC 动态 AIPP 模板位于 [`config/svp_acl/aipp/`](config/svp_acl/aipp/)。动态 OM 应使用独立文件名；`engine=auto` 会根据模型是否带动态 AIPP 自动选择硬件或 OpenCV 路径。

### 缩放策略

| 值 | 说明 |
|---|---|
| `"stretch"` | 直接拉伸，会改变宽高比 |
| `"letterbox_tl"` | 比例为网络宽除以原图较长边，图像贴左上角；须检查缩放后高度是否超过网络高度 |
| `"letterbox_tl_fit"` | 宽高比例取较小值，保持宽高比，图像贴左上角，右侧和下方填充 |
| `"letterbox_center"` | 宽高比例取较小值，保持宽高比并居中填充 |

---

## models.\<name\>.postprocess

`type` 字段决定使用哪个后处理器，其余参数由具体后处理器解读。

### type: "yolox_det"： YOLOX 多尺度无锚框检测

期望 N 个输出 tensor（每个 stride 一个），形状 `(1, bbox+obj+cls, H, W)` 或 `(1, H, W, C)`。

channel 布局：`[bbox_channels..., obj_channels..., num_classes...]`

| 字段 | 类型 | 默认值 | 说明 |
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

### type: "yolov5_anchor_det"： YOLOv5 锚框单尺度检测

期望 1 个输出 tensor，channel 布局同 yolox_det。

检测分为 `score = sigmoid(obj) × softmax(cls)`。类别分支采用 softmax；单类时该概率为 1，检测分等于 `sigmoid(obj)`。模型导出与部署后处理应保持这一约定。

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `num_classes` | int | `1` | 检测类别数 |
| `bbox_channels` | int | `4` | bbox 回归通道数 (tx, ty, tw, th) |
| `obj_channels` | int | `1` | objectness 通道数 |
| `stride` | int | `8` | 特征图步长（标量） |
| `anchor` | `[w, h]` | `[36, 36]` | 锚框尺寸（像素） |
| `conf_threshold` | float | `0.25` | 置信度阈值（对 `sigmoid(obj)×softmax(cls)`） |
| `nms_threshold` | float | `0.45` | NMS IoU 阈值 |
| `max_det` | int | `5` | NMS 后最大保留数；海思车牌检测配置使用 `2` |
| `obj_prefilter` | float | `0.05` | objectness 早剪枝阈值 |

### type: "ocr_classifier"：逐位 OCR 与门控分类

OCR 后处理根据 `class_names` 建立可接受的数字组合。限速牌使用三位字符，分别表示百位、十位和个位；每位包含数字和 blank，共 11 个类别。

单输出模型按每位 `num_chars` 连续读取，三位数字占 33 个元素；启用三路门控后共 36 个元素。多输出模型使用独立字符头，通过 `head_names` 优先匹配，名称不可用时按 `head_indices` 读取。

类名按数字串右对齐，不足位补 blank，例如 `"90"` 对应 `(blank,9,0)`，`"120"` 对应 `(1,2,0)`。各位经 softmax 和 argmax 得到字符后查询解码表；不属于配置组合的结果予以拒识。OCR 分类分为各位最大概率的最小值，低于 `conf_threshold` 时返回空结果。输出业务值为类名数值除以 10，与类别列表顺序无关。

#### 门控分类

`gate_channels>0` 时启用门控，根据最大概率类别判断处理路径：

- `speed`：门控限速概率达到 `gate_threshold` 后接受 OCR 结果，公共联合分为检测分乘 OCR 分。
- `no_parking`：设置 `nopark_category` 和 `nopark_sign_value`，识别分使用禁停门控概率；不使用数字 OCR 阈值过滤。
- 其他类别或限速概率不足：返回空结果，由 `classify_into` 丢弃上游框。

单输出模式下，门控位于 `output[0]` 的 `num_positions*num_chars` 偏移之后；多输出模式下，通过 `gate_head_name` / `gate_head_index` 定位独立门控头。`gate_channels=0` 时仅进行 OCR。

禁停最小尺寸由 stage 的 `min_box_short` 按原图框短边过滤。分类器只接收裁剪图，因此尺寸规则在编排层执行。

XMM 使用单输出布局，以避免特定多输出模型被划分为 NPU / CPU 子图后输出不完整的问题。多输出头形状相同时，使用张量名称明确各字符位的对应关系。单输出模式不使用 `head_names` 和 `head_indices`。

#### OCR 参数

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `num_positions` | int | 由 `class_names` 最长位数推导 | 字符位数，按高位到低位排列 |
| `num_chars` | int | `11` | 每头类别数（`'0'..'9'` + blank） |
| `blank_index` | int | `10` | 占位符在字符表中的下标 |
| `head_names` | `[str]` | `[]` | 仅多输出模式：按名称匹配各位对应的输出张量，按高位到低位排列 |
| `head_indices` | `[int]` | `[0,1,..]` | **仅多输出模式**：name 不可用时的备选索引（高位→低位） |
| `conf_threshold` | float | `0.5` | `min(各位 prob)` 低于此值时丢弃 |
| `class_names` | `[str]` | **必填** | 类名列表（限速字符串，解码 LUT 与 value 的唯一来源） |
| `category` | string | `""` | 分类器类别标签（如 `"speed_limit"`） |
| `gate_channels` | int | `0` | 门控路数（随附配置为 3）；`0`=无门控纯 OCR。单输出时门控在 OCR 段之后 |
| `gate_speed_index` | int | `1` | `P(限速)` 在门控向量里的下标 |
| `gate_nopark_index` | int | `2` | `P(禁停)` 在门控向量里的下标 |
| `gate_threshold` | float | `0.5` | 限速门控概率低于此值时拒识；仅在门控启用的限速分支生效 |
| `gate_head_name` | string | `""` | **仅多输出**：门控头张量名（如 `"logits_gate"`） |
| `gate_head_index` | int | `num_positions` | **仅多输出**：门控头备选索引（默认 = OCR 头之后） |
| `nopark_category` | string | `""` | 门控判 no_parking 时产出的 `category`（如 `"no_parking"`，映射到 `signs[]`） |
| `nopark_sign_value` | int | `0` | 门控判 no_parking 时写入 `value`（`AlgSignType`，`SIGN_NO_PARKING=1`） |

### type: "dualhead_classifier"：双头 softmax 分类器

该处理器用于双头模型兼容。随附限速牌配置使用 `ocr_classifier`，覆盖 10～120 km/h 的 12 类限速值。

期望 2 个输出 tensor（head_a: 首位数字, head_b: 位数判断）。

组装规则：`argmax(head_b) == is_3digit_class` 时 `cls_id = three_digit_class_index`，否则 `cls_id = argmax(head_a)`。

| 字段 | 类型 | 默认值 | 说明 |
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

## 车牌处理器

`rtmdet_det` 与 `lprnet_rec` 分别用于车牌检测和 CTC 识别。配套参数见 [车牌配置](config/svp_acl/license_plate.json)，包括检测阈值、NMS、字符集、时间步和 blank 索引。

车牌识别分为保留字符概率的乘积，`lprnet_rec.conf_threshold` 默认 `0.0`，不进行识别分过滤。文本为空时也可能保留结果，应用需检查文本及业务格式。公共字段定义见 [接口文档](../alg_sdk_api_documentation_v3.0.0.md)。

## 完整示例

- [SVP ACL 限速牌](config/svp_acl/speed_limit.json)：检测、OCR 和禁令 / 停车牌处理。
- [SVP ACL 组合业务](config/svp_acl/all.json)：限速牌、禁令 / 停车牌及车牌识别。
- [XMM 组合业务](config/xmm/all.json)、[MNN 组合业务](config/mnn/all.json)：内部红绿灯及限速牌处理；公共结果不包含红绿灯。
