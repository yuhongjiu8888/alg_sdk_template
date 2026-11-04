# 亿智 EZ826 / JC371 模型目录

本目录不包含已转换的业务模型。`resources/config/ez/` 下的 JSON 是集成模板，需先将本项目对应的原始业务网络转换为亿智模型，再放入以下文件：

| 业务 | 网络定义 | 权重 | 网络输入 W×H |
|---|---|---|---|
| 限速牌检测 | `speedlimit_detect.ezb` | `speedlimit_detect.bin` | 576×320 |
| 限速牌 OCR / 门控 | `speedlimit_classifier.ezb` | `speedlimit_classifier.bin` | 96×96 |
| 车牌检测 | `license_detection.ezb` | `license_detection.bin` | 640×448 |
| 车牌识别 | `license_recognizer.ezb` | `license_recognizer.bin` | 256×64 |

后端以 `model_path` 指定 `.ezb`，自动加载同路径、同名的 `.bin`。转换配置必须与 SDK 版本、芯片、输入颜色和归一化约定一致；模板使用完整输入尺寸，由 SDK 硬件做 letterbox，不沿用海思模型的 AIPP 图内补边。

官方 `826_JC371/model&&data` 的 YOLOv5n / YOLOv8n 是通用检测样例，不能直接替代上述业务模型；其输出头和本项目现有后处理不匹配。详细转换要求和相机接入方式见 [亿智接入说明](../../../doc/EZ826_INTEGRATION.md)。
