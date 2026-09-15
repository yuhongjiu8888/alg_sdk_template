# 动态 AIPP 模型转换配置

另一台 ATC 服务器上的完整转换步骤和交付清单见 [MODEL_CONVERSION.md](MODEL_CONVERSION.md)。

这些文件通过 ATC 的 `--insert_op_conf` 参数使用。运行时默认输入 NV21，同时兼容 NV12：

- NV12 设置 `SVP_ACL_YUV420SP_U8`。
- NV21 设置 `SVP_ACL_YVU420SP_U8`。
- 如果目标版本不允许同一个 OM 在两个枚举间切换，则使用 YUV420SP 并仅对 NV21 打开 U/V swap；必须用色条金样确认通道顺序。

全帧检测器接收原始 VPSS 帧，当前最大源尺寸为 1920×1080。连续 NV12/NV21 使用整帧非 cached MMZ/VB 映射并直接绑定到 ACL data buffer，缩放交给动态 AIPP。Y 与 UV/VU 分平面且同时提供物理地址时，IVE 先缩放到模型有效区，再写入一块 Y/VU 连续的非 cached MMZ，并零拷贝绑定到 ACL。没有物理地址的普通内存仅作兼容路径，使用 libyuv 按原尺寸合并后交给 AIPP 缩放。AIPP 负责 CSC 和运行时 DTC（当前两个已转换检测模型的 DTC 保持恒等，归一化在图内），包装模型图负责 padding。CV610 AIPP 的常量 padding 值固定为 0，无法直接复现现有模型的 114，因此当前检测模型在图首增加常量 `Pad`：

同一 `ChainSolution` 中的多个动态 AIPP 检测器可直接绑定同一个连续输入地址。物理分平面模式下，每个检测模型按自己的有效区尺寸复用模型实例内的 IVE 输出 MMZ；普通内存兼容路径才使用共享 ACL staging。

- `speedsignnet`：包装后的图输入为 NCHW `1×3×320×568`，左右各 Pad 4，常量值 114，送入原 576×320 网络。
- `license_detection`：包装后的图输入为 NCHW `1×3×360×640`，下方 Pad 88，常量值 114，送入原 640×448 网络。

ATC 的 `--input_shape` 应使用包装后 Pad 节点之前的尺寸，`--insert_op_conf` 使用本目录相应模板。配置中的 `aipp_output_size` 和 `aipp_graph_padding` 必须与包装图完全一致；运行时也会按实际源尺寸复算并校验。1920×1080 及不超过最大尺寸、能产生相同有效区和补边布局的输入可以直接使用，其他宽高比会返回前处理错误。

分类和识别仍由 SDK 从检测框裁剪 ROI，当前继续使用普通 OM 和 CPU 前处理；本目录对应模板只为后续接入硬件裁剪预留，不能直接替换现有 ROI 模型。

## Resize 路径决策与静态 AIPP 演进记录（2026-09-15）

当前每种算法只需一份动态 AIPP OM，不需要为“IVE resize”和
“AIPP resize”分别制作模型。`scf_switch` 不在 JSON 或 ATC 的
dynamic AIPP `.cfg` 中配置，而是 SDK 每帧调用
`svp_acl_mdl_set_aipp_scf_params` 时根据实际路径自动设置：

- 物理 Y/VU 分平面：IVE 已缩放到 `aipp_output_size`，AIPP 源尺寸与目标尺寸相同，`scf_switch=0`。
- 连续原图或无物理地址的兼容路径：AIPP 接收原图尺寸并缩放到 `aipp_output_size`，`scf_switch=2`。
- 两条路径最终给 AIPP/模型图的有效区尺寸一致，因此可共用同一份动态 AIPP OM，不会重复 resize。
- 当前动态 AIPP 的 crop 和 padding 开关均为 0；值为 114 的补边固化在模型图中。

后续可评估将整个 letterbox 交给 IVE：IVE 输出固定的网络输入尺寸，
在有效 ROI 内 resize，并在其余区域完成 padding；然后改用静态 AIPP
仅完成 YUV 格式解析、CSC 和归一化。该方向尚未实现，落地前必须满足：

1. 重新生成静态 AIPP OM；当前动态 AIPP OM 不能直接当作静态 AIPP OM 复用。
2. 移除或调整当前模型图内的常量 Pad，否则会二次 padding，且输入 shape 不匹配。
3. 后端新增静态 AIPP 模型识别和 IVE MMZ 直接绑定逻辑；当前 `SupportsDynamicAipp`/`PrepareDynamicAipp` 只支持动态 AIPP。
4. 确认 RGB=114 在当前 BT.601 limited-range CSC 下对应的 Y/UV 填充值，并用金样校验；不直接假定 Y=114 就与图内 RGB Pad 完全等价。
5. 验证 IVE 的 ROI 输出偏移、stride、宽高/色度偶数对齐、补边初始化方式，以及多模型时的 MMZ 占用和端到端性能。
6. 对比当前动态 AIPP 方案的像素、量化输入、检测结果和坐标回映，板端验证通过后再切换默认路径。

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
