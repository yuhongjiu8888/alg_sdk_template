## 说明 ##
这里存放已经编译好的第三方库的头文件和库。

- `libyuv/hi3516`：海思 ARM32 静态库。SDK 的动态 AIPP 路径使用
  `NV12Copy` / `NV21Copy` 将独立 Y、UV/VU 平面按原尺寸写入连续 staging，
  NV21 的 VU 字节顺序保持不变；缩放由 AIPP 完成。
