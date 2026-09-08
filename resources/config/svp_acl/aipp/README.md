# 动态 AIPP 模型转换配置

另一台 ATC 服务器上的完整转换步骤和交付清单见 [MODEL_CONVERSION.md](MODEL_CONVERSION.md)。

这些文件通过 ATC 的 `--insert_op_conf` 参数使用。运行时默认输入 NV21，同时兼容 NV12：

- NV12 设置 `SVP_ACL_YUV420SP_U8`。
- NV21 设置 `SVP_ACL_YVU420SP_U8`。
- 如果目标版本不允许同一个 OM 在两个枚举间切换，则使用 YUV420SP 并仅对 NV21 打开 U/V swap；必须用色条金样确认通道顺序。

全帧检测器只接收 `source_id=0` 的原始 VPSS 帧，当前最大源尺寸为 1920×1080，缩放由动态 AIPP 完成。CV610 AIPP 的常量 padding 值固定为 0，无法直接复现现有模型的 114，因此转换检测模型前需要在图首增加常量 `Pad`：

- `speedsignnet`：包装后的图输入为 NCHW `1×3×320×568`，左右各 Pad 4，常量值 114，送入原 576×320 网络。
- `license_detection`：包装后的图输入为 NCHW `1×3×360×640`，下方 Pad 88，常量值 114，送入原 640×448 网络。

ATC 的 `--input_shape` 应使用包装后 Pad 节点之前的尺寸，`--insert_op_conf` 使用本目录相应模板。配置中的 `aipp_output_size` 和 `aipp_graph_padding` 必须与包装图完全一致；运行时也会按实际源尺寸复算并校验。1920×1080 及不超过最大尺寸、能产生相同有效区和补边布局的输入可以直接使用，其他宽高比会返回前处理错误。

分类和识别仍由 SDK 从检测框裁剪 ROI，当前继续使用普通 OM 和 CPU 前处理；本目录对应模板只为后续接入硬件裁剪预留，不能直接替换现有 ROI 模型。

生成后的动态模型使用独立文件名，例如 `speedsignnet_aipp.om`。完成板端一致性验证前不要覆盖仓库中的基准 OM。
