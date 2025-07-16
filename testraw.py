#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
查看并可选修正 RGB565 RAW 图片
"""

import argparse
import logging
from pathlib import Path
from PIL import Image
import numpy as np

logging.basicConfig(
    level=logging.INFO,
    format="%(levelname)s | %(message)s"
)

def rgb565_to_rgb888(word):
    r = (word >> 11) & 0x1F
    g = (word >> 5)  & 0x3F
    b =  word        & 0x1F
    # 扩展到 8 bit
    r = (r << 3) | (r >> 2)
    g = (g << 2) | (g >> 4)
    b = (b << 3) | (b >> 2)
    return r, g, b

def load_rgb565_raw(path: Path, w=240, h=240, endian="le"):
    data = path.read_bytes()
    if len(data) != w * h * 2:
        raise ValueError(f"文件大小与分辨率不符: {len(data)}")
    arr16 = np.frombuffer(data, dtype="<u2" if endian=="le" else ">u2").reshape((h, w))
    rgb = np.empty((h, w, 3), dtype=np.uint8)
    for y in range(h):
        for x in range(w):
            rgb[y, x] = rgb565_to_rgb888(arr16[y, x])
    return rgb

def analyse(rgb, tag="当前"):
    flat = rgb.reshape(-1, 3)
    total = flat.shape[0]
    pure_black = np.all(flat == 0, axis=1).sum()
    near_black = np.all(flat <= 8, axis=1).sum() - pure_black
    logging.info("%s像素统计 | 总:%d  纯黑:%d  近黑:%d  其它:%d",
                 tag, total, pure_black, near_black, total-pure_black-near_black)

def fix_near_black(rgb, th):
    mask = np.all(rgb <= th, axis=2)
    rgb[mask] = 0

def main():
    ap = argparse.ArgumentParser(description="查看/修正 RGB565 RAW 图像")
    ap.add_argument("raw_file", type=Path, help=".raw 文件路径")
    ap.add_argument("--width", "-w", type=int, default=240)
    ap.add_argument("--height", "-H", type=int, default=240)
    ap.add_argument("--endian", choices=("le", "be"), default="le",
                    help="像素字节序")
    ap.add_argument("--fix", action="store_true",
                    help="将近黑像素转换为纯黑")
    ap.add_argument("--th", type=int, default=8,
                    help="近黑阈值 (默认 8)")
    args = ap.parse_args()

    rgb = load_rgb565_raw(args.raw_file, args.width, args.height, args.endian)
    analyse(rgb, "修正前")

    if args.fix:
        fix_near_black(rgb, args.th)
        analyse(rgb, "修正后")

    Image.fromarray(rgb, "RGB").show(title=f"{args.raw_file.name} ({args.endian}){'-fixed' if args.fix else ''}")

if __name__ == "__main__":
    main()