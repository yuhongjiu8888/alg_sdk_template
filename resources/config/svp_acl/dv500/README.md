# Hi3519DV500 配置与模型

本目录是海思 Hi3519DV500（aarch64）的运行时配置，与 `resources/model/svp_acl/dv500/`
下的 `.om` 配套使用。参数与 `../cv610/` 逐字相同，只替换了模型文件名：

| 配置项 | cv610 模型 | dv500 模型 |
|---|---|---|
| `ssn_cfg`（限速牌检测） | `speedsignnet_aipp.om` | `speedsignnet_aipp.om` |
| `cls_cfg`（限速牌分类） | `classifier.om` | `classifier.om` |
| `license_det_cfg`（车牌检测） | `license_detection_aipp.om` | `rtmdet_tiny_plate_10m.om` |
| `license_rec_cfg`（车牌识别） | `license_recognizer.om` | `plate_recognizer_lprnet.om` |

## AIPP 大比例缩放是 DV500 的性能陷阱（重要）

**不要让 DV500 的 AIPP 负责缩放。** 实测（`license_plate.json` 的检测模型，
`[det_cfg] infer` 即 `svp_acl_mdl_execute` 耗时）：

| 源尺寸 | AIPP 缩放比 | `scf_switch` | `[det_cfg] infer` |
|---|---|---|---|
| 1920×1080 | 1/3 | 2（BILINEAR） | **128.4 ms** |
| 1280×720 | 1/2 | 2（BILINEAR） | **128.5 ms** |
| 960×540 | 2/3 | 2（BILINEAR） | **128.5 ms** |
| 640×360 | 1.0（不缩放） | 0 | **4.8 ms** |

只要 AIPP 参与缩放，就是一笔**固定的 ~128ms**，与源数据量、与缩放比都无关（源从
0.78MB 到 3.1MB 差 4 倍，耗时纹丝不动）。NPU 执行图本身只要 4.8ms（同进程里
`plate_recognizer_lprnet` 的 `infer` 仅 0.5ms，可证驱动与 NPU 正常）。

根因是 AIPP 的两种缩放模式语义不同（见《应用开发指南》`svp_acl_mdl_set_aipp_scf_params`）：

- `scf_switch=0` 不缩放；`=1` 为 **NEAREST**（顺序：抠图→padding→**缩放**→色域转换→归一化）；
- `=2` 为 **BILINEAR**（顺序：抠图→padding→色域转换→**归一化**→缩放），
  即在一个已转色、已归一化的中间量上做大图缩放，DV500 上代价极高。

约束：NEAREST 只支持缩放比 {1/4, 1/2, 1, 2}，因此 1920×1080→640×360（1/3）
**用不了 NEAREST**，只能走慢的 BILINEAR 或不缩放。

cv610 的生产链路用 IVE 缩放后 AIPP 是 `scf=0`，所以一直没暴露这条路径；
而且 IVE 只在「分平面 + 填了 `plane_phys_addr`」时才启用
（`use_ive_resize = uses_separate_planes && has_any_phys`），文件回放测试同样不走 IVE。

### 规避办法：送到 AIPP 的帧必须已经是 `aipp_output_size`

让 AIPP 源尺寸等于目标尺寸 → `scf_switch=0` → 回到 4.8ms 的快路径。谁来做这件事
由运行时开关 `AlgSetAippScaleMode` 决定，两种模式都保留：

| 模式 | 含义 | 适用 |
|---|---|---|
| `ALG_AIPP_SCALE_AUTO`（默认） | 源尺寸不等于 `aipp_output_size` 时，**SDK 内部**用 libyuv 预缩放 | 不动管线；代价约 11ms/帧 CPU |
| `ALG_AIPP_SCALE_EXTERNAL` | 缩放由**调用方/VPSS** 完成；源尺寸不符则直接报错 | 让 VPSS 出一路 `aipp_output_size` 通道，CPU 零开销 |

```c
AlgSetAippScaleMode(ALG_AIPP_SCALE_EXTERNAL);  /* VPSS 已经缩好 */
AlgSetAippScaleMode(ALG_AIPP_SCALE_AUTO);      /* 交回 SDK 自己缩 */
```

可在 `AlgCreate` 前或运行期随时调用，便于按实机 CPU 余量动态切换。默认 `AUTO`，
因此**默认配置下不会再掉进那 128ms**。EXTERNAL 模式下源尺寸不符会打印
`aipp_scale=external but source is 1920x1080, expected 640x360` 并返回
`ALG_E_PREPROCESS` —— 故意不降级，避免"VPSS 通道配错了没人发现、帧率悄悄掉到 7fps"。

**前提：源必须是 16:9。** OM 里烤死了 88 行的图内 padding，几何校验要求
`w·s == 640 且 h·s == 360`（`s = min(640/w, 448/h)`），只有 16:9 能同时满足；
非 16:9 的源会被 SDK 直接拒绝并打印 `produces AIPP ... but OM expects ...`。
所以预缩放永远是干净的等比缩小，不涉及 letterbox 补边，`state.scale_ratio` 的
坐标反算语义与"由 AIPP 缩放"时完全一致。

### 预缩放的实现选择

SDK 内部用 `libyuv::NV12Scale(..., kFilterBox)`。它内部对 UV 平面调用
`UVScale(..., width/2, ...)`，**按「UV 对」缩放、不解释 U/V 字节语义**，
所以对 NV21 同样正确（NV21 只是对内的字节顺序不同）。

`AUTO` 模式下这段耗时计入 `svp_acl_mdl_execute`，在 `-log info` 的
`[det_cfg] aipp_pre ... copy=...` 和 `[det_cfg] infer=...` 里能看到：
检测模型大约 `infer ≈ 16ms`（4.8ms NPU + 约 11ms 预缩放）。

> **不要**把交错 UV 平面当「宽 = `srcW` 的单通道图」去缩：INTER_AREA 一类的
> 3 字节窗口会取到 `[V0,U0,V1]`，把 U 和 V 混在一起。实测 PSNR 31.5 dB，
> 而按 UV 对缩放是 47.7 dB。

同样 1920×1080 → 640×360 的其它实现（`loop_runner -scale 640x360 -scale-mode ...`，
仅用于对比，不是 SDK 路径）：

| 实现 | 每帧 CPU | 画质（vs 标准区域平均的 PSNR） | 车牌识别 |
|---|---|---|---|
| `libyuv`（**SDK 采用**） | 10.8 ms | 与区域平均一致 | score 0.657 / rec 0.764 |
| `opencv` | 22.3 ms | 同上，但 `CV_8UC2` 走了无 SIMD 的慢路径 | score 0.507 / rec 0.592 |
| 标量 box | 41.3 ms | 45.5 dB | 可用 |
| 最近邻 | 4.9 ms | 26.2 dB（3 倍下采样每 9 像素丢 8 个） | **失败**，rec 掉到阈值下 |

最近邻不要用：它便宜但抽稀像素，车牌细笔画被丢掉，识别置信度会掉到
`score_threshold` 以下。

## 已确认支持的输入形态

DV500 的 acllib 只发布 `acllib/`，**不包含 SVP/IVE**（无 `ss_mpi_ive.h` /
`libss_mpi_ive.a`），因此 dv500 构建的 `ALG_SVP_IVE_PREPROCESS` 默认为 `OFF`：

- **连续 NV12/NV21**（VPSS 整帧或连续 MMZ/VB 映射）→ 直接绑定动态 AIPP，零拷贝、零 flush。
- Y/UV **分平面**输入 → 由 libyuv 合并到 AIPP staging（`ALG_SVP_LIBYUV_FALLBACK=ON`，
  默认开启，走 aarch64 版 `third_party/libyuv/3519dv500`）。

源尺寸与 `aipp_output_size` 的关系决定走哪条：

| 源尺寸 | `AUTO`（默认） | `EXTERNAL` |
|---|---|---|
| 等于 `aipp_output_size` | 零拷贝直绑（连续帧）/ 原尺寸合并（分平面），`scf=0`，最快 | 同左 |
| 大于 `aipp_output_size` | 失去零拷贝，SDK 用 libyuv 缩放到目标尺寸后绑 staging，约 11ms/帧 | **直接报错** |

预缩放要 CPU 读源地址，因此**连续帧的调用方仍需保证该映射是相干的**
（现有契约：SYS mmap 的非 cached MMZ/VB 映射）。

`loop_runner` 传 `.yuv` 文件时按分平面路径传入，可直接在 dv500 上验证：
`-aipp-scale auto` 会走 SDK 内部预缩放，`-aipp-scale external` 配合
`-scale 640x360` 则模拟"VPSS 已经缩好"的场景。

> **别让两边都缩。** SDK 只按「源尺寸 != `aipp_output_size`」判断要不要缩，
> 所以测试程序 `-scale 640x360` 之后 SDK 不会再缩（源已等于目标）。但若 `-scale`
> 到别的尺寸（如 960×540），SDK 会**再缩一次** —— 结果仍正确，但画质白损一遍、
> CPU 白花一遍，且是静默的。为此 `loop_runner` 在给了 `-scale` 时默认隐含
> `-aipp-scale external`：此时尺寸不符会直接报错，而不是闷声二次缩放。

## 模型转换注意事项

`../cv610/aipp/` 下的 `.cfg` 是 `--soc_version=Hi3516CV610` 的 ATC 转换配置，
**不能直接用于 DV500**。为 DV500 重新转换 OM 时需要：

1. 用对应 DV500 的 ATC 与 `--soc_version`，`--insert_op_conf` 不能照搬 cv610 的 `.cfg`；
2. 重新确认 AIPP 的 padding 上限：

   > 当前 cv610 的检测模型在 ONNX 图内用常量补齐有效区（`speedsignnet` 补 114、
   > 车牌检测底部补 88 行），是因为 CV610 的 AIPP 硬件 pad 能力有限。
   > DV500 的 AIPP 能力不同，**需实测后再决定是否保留图内 padding**。

   `all.json` / `license_plate.json` 里的 `aipp_output_size` 与 `aipp_graph_padding`
   必须与转换出的图一致，否则 letterbox 映射会错位。

`rtmdet_tiny_plate_10m.om` 的文件名虽没有 `aipp` 后缀，但**确认是动态 AIPP 模型**，
可以走 `"engine": "aipp"` 的硬件前处理路径。

本目录四个模型的输入尺寸、类别数与 `../cv610/` 一致，其中两个检测模型
（`speedsignnet_aipp.om`、`rtmdet_tiny_plate_10m.om`）的 `aipp_output_size` /
`aipp_graph_padding` 沿用了 cv610 的值（`[568,320]` / `[4,0,4,0]`、
`[640,360]` / `[0,0,0,88]`）。若 DV500 转换时改了图内 padding，需同步这两处配置，
否则 letterbox 映射会错位。
