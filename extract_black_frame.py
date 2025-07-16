#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
extract_black_frame.py
======================

从 MP4 视频中提取一帧，并统计纯黑 / 近黑 像素数量与比例。

用法示例：
    python extract_black_frame.py video.mp4            # 默认读取第 0 帧
    python extract_black_frame.py video.mp4 -f 100     # 指定第 100 帧
    python extract_black_frame.py video.mp4 -f 0 --th 8 --save frame.png

参数说明：
    -f / --frame    提取哪一帧（从 0 开始），默认 0
    --th            近黑阈值 (默认 8)。像素 R,G,B 全部 <= th 视为近黑
    --save          把提取出的帧保存为 PNG 文件
"""
import argparse
import logging
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

logging.basicConfig(level=logging.INFO, format="%(levelname)s | %(message)s")

def analyse(rgb: np.ndarray, th: int):
    h, w, _ = rgb.shape
    total = h * w
    flat = rgb.reshape(-1, 3)
    pure_black = np.all(flat == 0, axis=1).sum()
    near_black = np.all(flat <= th, axis=1).sum() - pure_black
    logging.info("帧分辨率: %dx%d", w, h)
    logging.info("像素统计 | 总:%d  纯黑:%d (%.2f%%)  近黑:%d (%.2f%%)  其它:%d (%.2f%%)",
                 total,
                 pure_black, pure_black / total * 100,
                 near_black, near_black / total * 100,
                 total - pure_black - near_black,
                 (total - pure_black - near_black) / total * 100)


def main():
    ap = argparse.ArgumentParser(description="从 MP4 提取一帧并统计黑色像素比例")
    ap.add_argument("video", type=Path, help="MP4 文件路径")
    ap.add_argument("--frame", "-f", type=int, default=0, help="提取第几帧 (0 基)" )
    ap.add_argument("--th", type=int, default=8, help="近黑阈值 (默认 8)")
    ap.add_argument("--save", type=Path, default=None, help="可选：保存帧为 PNG")
    args = ap.parse_args()

    if not args.video.exists():
        ap.error(f"视频文件不存在: {args.video}")

    cap = cv2.VideoCapture(str(args.video))
    if not cap.isOpened():
        ap.error("无法打开视频文件")

    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    target = min(args.frame, max(0, total_frames - 1))
    logging.info("视频帧数: %d, 读取第 %d 帧", total_frames, target)
    cap.set(cv2.CAP_PROP_POS_FRAMES, target)

    ret, frame_bgr = cap.read()
    cap.release()
    if not ret:
        ap.error(f"读取帧 {target} 失败")

    # 转 RGB
    frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)

    analyse(frame_rgb, args.th)

    # 可选保存
    if args.save:
        Image.fromarray(frame_rgb).save(args.save)
        logging.info("已保存帧到 %s", args.save)

    # 弹窗显示
    Image.fromarray(frame_rgb).show(title=f"frame {target}")

if __name__ == "__main__":
    main() 