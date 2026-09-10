# Hi3516CV610 动态 AIPP 模型转换交接说明

## 1. 转换范围

本次只转换两个使用整帧输入的检测模型：

| 模型 | 原 ONNX 输入 | 动态 AIPP 输出 | 图内常量 Pad | Pad 后网络输入 |
|---|---:|---:|---:|---:|
| 限速牌 `speedsignnet` | NCHW 1×3×320×576 | 568×320 | 左 4、右 4、值 114 | 576×320 |
| 车牌 `license_detection` | NCHW 1×3×448×640 | 640×360 | 下 88、值 114 | 640×448 |

输入是 VPSS 原分辨率 YUV420SP，最大 1920×1080。运行时默认 NV21，也必须兼容 NV12。分类器 `classifier` 和车牌识别器 `license_recognizer` 当前接收 SDK 裁剪后的 BGR ROI，继续使用普通 OM，不在本次转换范围内。

转换后的 OM 必须保留原模型输出节点、输出顺序、量化方式和后处理语义。不要覆盖现有基准 OM。

## 2. 需要从本仓库带到转换服务器的文件

复制整个目录：

```text
resources/config/svp_acl/aipp/
```

其中检测模型使用：

```text
speedsignnet_dynamic_aipp.cfg
license_detection_dynamic_aipp.cfg
wrap_onnx_input_pad.py
```

转换服务器需要 Python 3、`onnx` 包以及支持 `Hi3516CV610` 的 ATC 环境。记录 `atc --version` 和 `python3 -c "import onnx; print(onnx.__version__)"` 的输出，随产物一起交付。

## 3. 为什么必须先修改 ONNX 图

CV610 动态 AIPP 可以完成 YUV 转 RGB、双线性缩放和归一化，但硬件 padding 的四边最大为 15，而且常量填充值为 0，无法复现当前模型使用的 114。车牌模型还需要下补 88，超过硬件限制。

因此动态 AIPP 只输出缩放后的有效图区域，再由 ONNX 图输入后的常量 `Pad` 补到原网络输入尺寸。Pad 位于模型图中，常量值必须使用原模型输入 tensor 的数据类型；随附脚本会自动处理数据类型和输入 shape。

## 4. 生成带 Pad 的 ONNX

先确认原始 ONNX 的业务输入是四维 NCHW，且限速模型为 1×3×320×576、车牌模型为 1×3×448×640。若模型有多个业务输入，命令中增加 `--input-name 原输入名`。

限速牌模型：

```bash
python3 wrap_onnx_input_pad.py \
  --input-model /path/to/speedsignnet.onnx \
  --output-model /path/to/speedsignnet_aipp_input.onnx \
  --content-size 568 320 \
  --pads 4 0 4 0 \
  --value 114
```

车牌检测模型：

```bash
python3 wrap_onnx_input_pad.py \
  --input-model /path/to/license_detection.onnx \
  --output-model /path/to/license_detection_aipp_input.onnx \
  --content-size 640 360 \
  --pads 0 0 0 88 \
  --value 114
```

脚本会输出包装后的新输入名和 ATC `input_shape`，通常分别类似：

```text
images_aipp:1,3,320,568
images_aipp:1,3,360,640
```

实际命令必须使用脚本打印的输入名，不能直接假定为 `images_aipp`。如果 ONNX opset 太旧而不支持三输入形式的 Pad，请先用原模型导出工具升级到 ATC 支持的 opset，再执行脚本；不要在不验证输出的情况下改写算子版本。

## 5. 使用 ATC 生成动态 AIPP OM

以原来已经验证通过的 ATC 转换命令为基础，保留原有的框架、量化校准、精度模式、输出节点和其他参数，只增加或替换以下三项：

```text
--soc_version=Hi3516CV610
--insert_op_conf=<本目录对应的 dynamic_aipp.cfg>
--input_shape=<脚本打印的新输入名和尺寸>
```

限速模型命令模板：

```bash
atc \
  --framework=5 \
  --soc_version=Hi3516CV610 \
  --model=/path/to/speedsignnet_aipp_input.onnx \
  --output=/path/to/speedsignnet_aipp \
  --input_shape="<新输入名>:1,3,320,568" \
  --insert_op_conf=/path/to/speedsignnet_dynamic_aipp.cfg \
  <原模型其余已验证的转换参数>
```

车牌检测模型命令模板：

```bash
atc \
  --framework=5 \
  --soc_version=Hi3516CV610 \
  --model=/path/to/license_detection_aipp_input.onnx \
  --output=/path/to/license_detection_aipp \
  --input_shape="<新输入名>:1,3,360,640" \
  --insert_op_conf=/path/to/license_detection_dynamic_aipp.cfg \
  <原模型其余已验证的转换参数>
```

`--framework` 应与服务器上原 ONNX 转换流程一致；如果原命令不是 `5`，使用原值。ATC 的输出通常为命令中 `--output` 路径加 `.om`。

动态 AIPP 配置中的 `related_input_rank: 0` 假定图像是第一个业务输入。如果脚本处理的图像输入不是 rank 0，需要同步修改 cfg，并在板端确认 `svp_acl_mdl_get_input_aipp_type` 返回的关联关系正确。

## 6. 产物命名与 SDK 配置

建议产物名：

```text
speedsignnet_aipp.om
license_detection_aipp.om
```

复制到 SDK 的 `resources/model/svp_acl/` 后，修改对应 JSON 的 `model_path`：

| 模型 | 需要修改的配置 |
|---|---|
| `speedsignnet_aipp.om` | `resources/config/svp_acl/speed_limit.json`、`all.json` |
| `license_detection_aipp.om` | `resources/config/svp_acl/license_plate.json`、`all.json` |

配置中已有以下运行参数，不要删除或改成 VPSS 缩放输入：

```json
"engine": "auto",
"input_format": "auto",
"max_input_size": [1920, 1080]
```

限速模型还必须保留：

```json
"aipp_output_size": [568, 320],
"aipp_graph_padding": [4, 0, 4, 0]
```

车牌检测模型还必须保留：

```json
"aipp_output_size": [640, 360],
"aipp_graph_padding": [0, 0, 0, 88]
```

首次验证时可临时把检测模型的 `engine` 改为 `"aipp"`。这样 OM 未包含动态 AIPP 时会直接报错，不会自动回退 OpenCV；确认模型正确后恢复 `"auto"`。

## 7. 板端验收

使用同一批原始 1920×1080 图像分别跑旧 OM/OpenCV 路径和新 OM/AIPP 路径。至少完成以下检查：

1. 使用同一画面生成 NV21 和 NV12 两份输入，并分别走连续缓冲与独立 Y/UV(VU) 地址，确认各路径的类别、框位置和颜色相关结果一致。
2. 使用彩条或红蓝明显的画面确认 U/V 顺序；若红蓝互换，先核对 `AlgImage.format`，再检查运行时 YVU 枚举及 U/V swap。
3. 比较检测数量、类别、框坐标和置信度。AIPP 的 CSC 与插值可能与 OpenCV 有轻微数值差异，容差应根据验证集统计确定，不能只检查单张图。
4. 确认检测框映射回 1920×1080 原图坐标正确，重点检查图像边缘目标。
5. 分别记录连续 MMZ/VB 零拷贝绑定与分平面 libyuv 合并路径的总 CPU、单帧前处理时间、推理时间、端到端时间和峰值内存。两种输入均由 AIPP 缩放。
6. 连续运行稳定性测试，覆盖无目标、多目标和最大检测框数量场景。

运行时常见错误对应关系：

| 现象或日志 | 优先检查 |
|---|---|
| `dynamic AIPP is unavailable` | ATC 是否真正插入 dynamic AIPP、是否关联了正确输入 |
| `source ... produces AIPP ... but OM expects ...` | 原图宽高比、JSON 有效区尺寸、ONNX Pad 四边是否一致 |
| `attach dynamic AIPP failed` | AIPP 输出 shape 是否等于包装 ONNX 的外部输入 shape、输入 rank 是否正确 |
| NV21 颜色异常而 NV12 正常 | YVU420SP 支持情况以及 U/V swap |
| 框整体偏移 | Pad 左上偏移及检测后处理的 `resize` 配置 |

## 8. 回传清单

将以下内容回传到本项目或随版本归档：

- 两个带 Pad 的 ONNX；
- `speedsignnet_aipp.om` 和 `license_detection_aipp.om`；
- 完整 ATC 命令和控制台日志；
- ATC、ONNX 和转换环境版本；
- 原 ONNX、包装 ONNX、OM 的 SHA-256；
- NV21/NV12 金样结果与板端性能记录。

在精度、颜色和性能验证通过前，保留现有 `speedsignnet.om` 与 `license_detection_10m.om` 作为回退基准。
