# 亿智 EZ826 / JC371 配置模板

本目录为原业务网络转换到亿智后的配置模板，**仓库未提供配套 `.ezb` / `.bin`，不能直接运行**。

- `speed_limit.json`：限速牌检测、OCR 和禁令 / 停车牌。
- `license_plate.json`：车牌检测和识别。
- `all.json`：组合上述两个流程。

模板选择 `engine: hardware`、`input_format: NV21`。全图缩放和二阶段裁剪由亿智图像硬件处理，EZ 动态库不依赖 OpenCV。NV12 集成时将 `input_format` 改为 `NV12`，或使用 `auto` 兼容两种输入。

`color: BGR` 表示交给亿智图像导入接口的 BGR 硬件图像约定；原业务网络所需的 RGB 通道交换和归一化应在模型转换中固化。此处不再设置 CPU 归一化。`input_size` 对应完整网络输入，不使用海思的 `aipp_output_size` / `aipp_graph_padding`。

模型路径以进程工作目录为准。源模板中的 `../resources/model/ez/` 适用于从仓库一级构建目录运行；也可以改为设备上的绝对路径。`builddeploy.sh` 原样拷贝 `resources/config/ez/` 和 `resources/model/ez/`，不解析配置或修改路径，部署时应按实际工作目录配置 `model_path`。模型文件清单见 [模型目录](../../model/ez/README.md)，接入要求、硬件对齐限制和构建参数见 [亿智接入说明](../../../doc/EZ826_INTEGRATION.md)。
