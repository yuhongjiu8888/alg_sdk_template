## 说明 ##
这里存放已经编译好的第三方库的头文件和库。

- `libyuv/hi3516`：海思 ARM32 静态库，供 `svp_acl_cv610`（Hi3516CV610）使用。
- `libyuv/3519dv500`：海思 aarch64 静态库，供 `svp_acl_dv500`（Hi3519DV500）使用。
  两份头文件内容相同，只是目标架构不同；CMake 按芯片选择 `LIBYUV_ROOT`。

SDK 的动态 AIPP 路径使用 `NV12Copy` / `NV21Copy` 将独立 Y、UV/VU 平面按原尺寸
写入连续 staging，NV21 的 VU 字节顺序保持不变；缩放由 AIPP 完成。
