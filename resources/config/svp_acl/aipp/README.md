# 动态 AIPP 模型转换配置

另一台 ATC 服务器上的完整转换步骤和交付清单见 [MODEL_CONVERSION.md](MODEL_CONVERSION.md)。

这些文件通过 ATC 的 `--insert_op_conf` 参数使用。运行时默认输入 NV21，同时兼容 NV12：

- NV12 设置 `SVP_ACL_YUV420SP_U8`。
- NV21 设置 `SVP_ACL_YVU420SP_U8`。
- 如果目标版本不允许同一个 OM 在两个枚举间切换，则使用 YUV420SP 并仅对 NV21 打开 U/V swap；必须用色条金样确认通道顺序。

全帧检测器接收原始 VPSS 帧，当前最大源尺寸为 1920×1080。连续 NV12/NV21 由动态 AIPP 缩放；Y 与 UV/VU 为独立虚拟地址时，SDK 使用 libyuv 缩放并直接合并到连续 ACL staging，随后 AIPP 做 CSC、包装模型图做 padding/归一化。CV610 AIPP 的常量 padding 值固定为 0，无法直接复现现有模型的 114，因此转换检测模型前需要在图首增加常量 `Pad`：

连续输入下，同一 `ChainSolution` 中使用同一原始帧、stride 和格式的多个动态 AIPP 检测器共享 ACL staging 缓冲：首个检测器复制并 flush，后续检测器只绑定并复用，避免重复搬运整帧。

分平面输入经 libyuv 缩放后的有效区尺寸与具体模型绑定。`all.json` 中两个检测模型有效区不同，因此分别执行缩放，不能共享缩放后的 staging；这是用 CPU 合并海思分离平面的必要代价。应在板端同时比较连续输入和分平面输入的 CPU、端到端耗时及检测一致性。

- `speedsignnet`：包装后的图输入为 NCHW `1×3×320×568`，左右各 Pad 4，常量值 114，送入原 576×320 网络。
- `license_detection`：包装后的图输入为 NCHW `1×3×360×640`，下方 Pad 88，常量值 114，送入原 640×448 网络。

ATC 的 `--input_shape` 应使用包装后 Pad 节点之前的尺寸，`--insert_op_conf` 使用本目录相应模板。配置中的 `aipp_output_size` 和 `aipp_graph_padding` 必须与包装图完全一致；运行时也会按实际源尺寸复算并校验。1920×1080 及不超过最大尺寸、能产生相同有效区和补边布局的输入可以直接使用，其他宽高比会返回前处理错误。

分类和识别仍由 SDK 从检测框裁剪 ROI，当前继续使用普通 OM 和 CPU 前处理；本目录对应模板只为后续接入硬件裁剪预留，不能直接替换现有 ROI 模型。

## 后续优化记录：二阶段移除 OpenCV

当前暂不修改二阶段。后续若板端 CPU 数据表明 ROI 前处理成为瓶颈，优先将
`src/core/solution/crop_util.cpp` 中 NV12/NV21 的 ROI 裁剪、缩放及 YUV→BGR
转换改为 libyuv（或芯片硬件裁剪/转换），并复用固定容量的 ROI 缓冲，减少
OpenCV `Mat`、`warpAffine` 和 `cvtColorTwoPlane` 带来的 CPU 与临时内存开销。

替换时须保持现有裁剪几何、越界补 114、NV12/NV21 色度顺序以及分类/识别模型
输入格式完全一致。板端需对比修改前后的 ROI 像素或模型输出、端到端结果、CPU、
单帧耗时和峰值内存；验证通过后，才可移除海思二阶段路径的 OpenCV 依赖。普通
OM 的整帧 OpenCV 回退路径是否保留，作为独立兼容性决策处理。

生成后的动态模型使用独立文件名，例如 `speedsignnet_aipp.om`。完成板端一致性验证前不要覆盖仓库中的基准 OM。

## 转换完成与接入记录（2026-09-08）

`speedsignnet_aipp.om` 已生成并拷入 `resources/model/svp_acl/`，`speed_limit.json`
与 `all.json` 的 `ssn_cfg` 已切到 `speedsignnet_aipp.om`，板端验证通过（NV21 整帧 +
动态 AIPP 正常检出）。

> 归一化说明（2026-09-08 复测修正）：speedsignnet 动态 AIPP 版在 Pad 后插了
> `×1/255` 图内归一化节点（wrap 参数 `--scale-after-pad 0.00392156862745098`），
> 使卷积输入量化 scale≈255、即模型吃到训练域的 `像素/255`。未做此归一化时板端误检
> 过多（校准层输入 scale=1、无 /255）。运行时可把该归一化全部交给图内节点，AIPP
> DT 保持恒等（cfg 无需设 mean/std）。

⚠️ `ssn_cfg`、`license_det_cfg`/`det_cfg` 的 `engine` 现在临时是 `"aipp"`
（OM 未含动态 AIPP 会直接报错）。**板端一致性/性能验证通过后需把 `engine` 改回
`"auto"`** 再发版。

> 车牌 `license_detection_aipp.om`（Concat 方案，2026-09-08）：RTMDet 640×448 底部需补
> 88 行常量 114。CV610 AIPP 硬件补边上限 15 且值 0，只能图内补；但 **ONNX Pad 算子会让
> RTMDet 数据层量化成 FP16**，与动态 AIPP 开 resize 要求的 S8/U8 冲突（编译报
> `Data layer[...] only support S8/U8 when aipp resize on, get FP16`）。改用 **`Concat` 拼接
> 常量 114 平面**（`make_aipp_input.py`）后数据层保持 S8、编译通过且无 CPU 节点。
> 两个检测模型的图内归一化均在该包装 ONNX 中完成（speedsign ×1/255；车牌 per-channel
> (x-mean)/std），运行时 AIPP DT 保持恒等、cfg/JSON 无需设 mean/std。
