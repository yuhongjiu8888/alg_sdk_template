#!/usr/bin/env python3
"""图片 → NV21 原始帧批量转换，用于板端 loop_runner 逐帧测试。

用途：判断板端检测分数偏低是"某一张图的特例"还是"普遍偏低"。
把多张 jpg/png 各转成一个 raw_<W>x<H>_nv21.yuv，拷到板端后用
loop_runner 逐张跑、记录每张 LP 分数对比即可。

转换语义（与 SDK/AIPP 一致）：
  1) resize 到目标尺寸（默认 1920x1080，与板端 VPSS/AIPP 最大源一致）
  2) BGR → NV21：OpenCV BT.601 limited-range 编码（COLOR_BGR2YUV_I420
     就是相机/VPSS 的 limited-range YUV 域），再把平面重排为 NV21 的
     Y + VU 交错布局
  3) 按 raw_<W>x<H>_nv21.yuv 命名，loop_runner 可自动从文件名解析尺寸

依赖：numpy + opencv-python(-headless)。

用法：
  python3 test/img2nv21.py a.jpg b.png                    # 默认 1920x1080 输出
  python3 test/img2nv21.py -o nv21_out a.jpg b.jpg       # 指定输出目录
  python3 test/img2nv21.py --size 1920x1080 imgs/*.jpg    # 批量
输出：
  <out>/raw_1920x1080_nv21/a.jpg → <out>/raw_1920x1080_nv21/a_1920x1080_nv21.yuv
  并生成 <out>/nv21_file_list.txt 便于拷板后逐个跑。
"""
import argparse
import os
import sys
from pathlib import Path

import cv2
import numpy as np


def bgr_to_nv21(bgr: np.ndarray) -> np.ndarray:
    """BGR(BT.601 limited-range 编码) → NV21 单帧 (H*3/2, W) uint8。"""
    h, w = bgr.shape[:2]
    assert h % 2 == 0 and w % 2 == 0, "尺寸必须为偶数（YUV420 要求）"
    i420 = cv2.cvtColor(bgr, cv2.COLOR_BGR2YUV_I420)  # BT.601 limited-range
    flat = i420.reshape(-1).astype(np.uint8)
    y = flat[: h * w].reshape(h, w)
    uv_h, uv_w = h // 2, w // 2
    u = flat[h * w: h * w + uv_h * uv_w].reshape(uv_h, uv_w)
    v = flat[h * w + uv_h * uv_w:].reshape(uv_h, uv_w)
    uv = np.empty((uv_h, w), dtype=np.uint8)
    uv[:, 0::2] = v   # NV21：先 V 后 U
    uv[:, 1::2] = u
    return np.vstack([y, uv])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+", help="输入图片（jpg/png/bmp）")
    ap.add_argument("-o", "--out", default="nv21_out", help="输出目录（默认 nv21_out/）")
    ap.add_argument("--size", default="1920x1080", help="目标尺寸 WxH（默认 1920x1080）")
    ap.add_argument("--list", default="nv21_file_list.txt", help="输出文件清单名")
    args = ap.parse_args()

    try:
        w, h = (int(x) for x in args.size.lower().split("x"))
    except ValueError:
        print(f"bad --size: {args.size}（期望 WxH）", file=sys.stderr)
        return 1
    if w <= 0 or h <= 0 or (w & 1) or (h & 1):
        print(f"--size {w}x{h} 必须为正偶数", file=sys.stderr)
        return 1

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    exts = {".jpg", ".jpeg", ".png", ".bmp"}
    lines = []
    ok = 0
    for img_path in args.images:
        p = Path(img_path)
        if p.suffix.lower() not in exts:
            print(f"skip 非图片: {img_path}", file=sys.stderr)
            continue
        bgr = cv2.imread(str(p), cv2.IMREAD_COLOR)
        if bgr is None:
            print(f"skip 读图失败: {img_path}", file=sys.stderr)
            continue
        if bgr.shape[1] != w or bgr.shape[0] != h:
            if (bgr.shape[1] * h) != (bgr.shape[0] * w):
                print(f"warn {img_path} 宽高比与 {w}x{h} 不一致，将被拉伸", file=sys.stderr)
            bgr = cv2.resize(bgr, (w, h), interpolation=cv2.INTER_LINEAR)
        nv21 = bgr_to_nv21(bgr)
        stem = p.stem.replace(" ", "_")
        out_name = out_dir / f"{stem}_{w}x{h}_nv21.yuv"
        out_name.write_bytes(nv21.tobytes())
        lines.append(str(out_name.resolve()))
        ok += 1
        print(f"ok   {img_path} -> {out_name} ({out_name.stat().st_size} B)")

    list_path = out_dir / args.list
    if lines:
        list_path.write_text("\n".join(lines) + "\n")
        print(f"list -> {list_path}（{len(lines)} 条）")

    print("\n拷到板端后逐张测（每张跑几十帧取稳定值）：")
    for ln in lines[:5]:
        print(f"  ./loop_runner resources/config/svp_acl/all.json "
              f"{Path(ln).name} -fmt nv21 -n 30")
    if len(lines) > 5:
        print(f"  ... 共 {len(lines)} 张，其余见 {list_path.name}")
    print(f"\n完成 {ok}/{len(args.images)}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
