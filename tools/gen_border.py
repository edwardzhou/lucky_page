#!/usr/bin/env python3
"""生成小票花纹边框背景图（384 点宽，58mm 热敏）

用法: python3 tools/gen_border.py [--out /tmp/border.lgo] [--preview /tmp/border.png]

版式设计：
  · 顶部装饰带（0~62 点）：连续卷草/菱形花纹 —— 店名会**压在这条花纹带上**
  · 双线边框（62 点以下）：外粗内细，四角带花饰
  · 左右边线重复小菱形，底部收口花纹
顶部花纹带做成"满幅图案"是有意的：这样叠加上去的文字与图案**像素必然相交**，
便于真机确认叠加是否真的生效。
"""
import argparse
from PIL import Image, ImageDraw

# 背景图高度上限 kMaxLogoHeight = 672 点（28 行文字，约 100mm）
# 目标：**框内正好 24 行**（24×24 = 576 点）
#   0..34    顶部花纹带（店名 2 倍字号 48 点压在它上面）
#   34..46   外框/内框上沿
#   48..624  框内可用区 = 576 点 = **24 行**
#   624..660 底部收口花纹 + 下沿
W, H = 384, 672
TOP_BAND = 34
M = 10                   # 边框外边距


def diamond(d, cx, cy, r, fill=False):
    d.polygon([(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)],
              outline=0, fill=(0 if fill else None))


def flourish(d, cx, cy, s):
    """卷草花饰：一个菱形 + 两侧弧线"""
    diamond(d, cx, cy, s)
    d.arc([cx - 3 * s, cy - 2 * s, cx + 3 * s, cy + 2 * s], 200, 340, fill=0)
    d.arc([cx - 3 * s, cy - 2 * s, cx + 3 * s, cy + 2 * s], 20, 160, fill=0)


def build():
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)

    # ---- 顶部装饰带：连续花纹（文字将压在其上）----
    for y in (6, TOP_BAND - 8):
        d.line((M, y, W - M, y), fill=0, width=1)
    step = 32
    for i, cx in enumerate(range(M + step // 2, W - M, step)):
        cy = (6 + TOP_BAND - 8) // 2
        flourish(d, cx, cy, 7 if i % 2 == 0 else 5)
        if i % 2 == 1:
            diamond(d, cx - step // 2 + 4, cy, 3, fill=True)

    # ---- 双线边框 ----
    d.rectangle([M, TOP_BAND, W - 1 - M, H - 1 - M], outline=0, width=2)          # 外粗
    d.rectangle([M + 6, TOP_BAND + 6, W - 1 - M - 6, H - 1 - M - 6], outline=0)   # 内细

    # ---- 四角花饰 ----
    for cx, cy in ((M + 2, TOP_BAND + 2), (W - 3 - M, TOP_BAND + 2),
                   (M + 2, H - 3 - M), (W - 3 - M, H - 3 - M)):
        diamond(d, cx, cy, 9)
        diamond(d, cx, cy, 4, fill=True)

    # ---- 左右边线：重复小菱形 + 短横 ----
    y = TOP_BAND + 34
    while y < H - M - 30:
        diamond(d, M + 3, y, 3)
        diamond(d, W - 4 - M, y, 3)
        d.line((M + 8, y, M + 16, y), fill=0)
        d.line((W - 17 - M, y, W - 9 - M, y), fill=0)
        y += 30

    # ---- 底部收口花纹（贴紧内框下沿，不留废白）----
    by = H - M - 40
    d.line((M + 14, by, W - M - 14, by), fill=0)
    for cx in range(M + 24, W - M - 20, 24):
        diamond(d, cx, by, 4, fill=True)
    return img


def to_lgo(img):
    w, h = img.size
    bpr = (w + 7) // 8
    buf = bytearray(8 + bpr * h)
    buf[0:4] = b"LGO1"
    buf[4], buf[5] = w & 255, w >> 8
    buf[6], buf[7] = h & 255, h >> 8
    px = img.load()
    black = 0
    for y in range(h):
        for x in range(w):
            if px[x, y] < 128:
                buf[8 + y * bpr + (x >> 3)] |= 0x80 >> (x & 7)
                black += 1
    return bytes(buf), black / (w * h) * 100


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/border.lgo")
    ap.add_argument("--preview", default="/tmp/border.png")
    a = ap.parse_args()
    img = build()
    img.save(a.preview)
    blob, pct = to_lgo(img)
    open(a.out, "wb").write(blob)
    print(f"边框图已生成 {a.out}（{img.size[0]}×{img.size[1]} 点，"
          f"{len(blob)} 字节，黑点占比 {pct:.1f}%）")
    print(f"预览图 {a.preview}（约占 {-(-img.size[1]//24)} 行文字高）")
