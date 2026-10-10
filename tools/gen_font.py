#!/usr/bin/env python3
"""生成打印机点阵字库（图形模式用）

用法:
    python3 tools/gen_font.py                 # 默认字符集（ASCII + 常用汉字）
    python3 tools/gen_font.py --chars 额外字符
    python3 tools/gen_font.py --all-gbk       # 全 GBK（约 540KB，慎用）

为什么需要：要让文字和图**真正叠加**，必须把整张小票在设备端合成成一张点阵图
（GS v 0 整幅打印）。打印机的页面模式本机不支持（实测），所以只能自己画像素。

字形尺寸与打印机 FONT A 对齐，保证图形模式与文字模式行宽一致：
    ASCII 12×24（半宽）  汉字 24×24（全宽）

输出格式 font24.bin：
    magic "F24A"(4B) | count(uint32 LE) | 索引项×count | 点阵数据
    索引项：code(uint32 LE, Unicode) + w(uint16) + h(uint16) + offset(uint32)
    点阵：逐行 MSB 优先，每行 ceil(w/8) 字节，1 = 黑点
"""
import argparse
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

# 字体优先级：STHeiti 放第一 —— 实测 PingFang 对部分常用字（稳/敌/杂/艳/虑…）
# 在 22px 下渲染为**空白**（0 黑点），若不做逐字回退就会缺字
FONTS = [
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/PingFang.ttc",
]
ASCII_W, ASCII_H = 12, 24
CJK_W, CJK_H = 24, 24

BASE = (
    "".join(chr(c) for c in range(0x20, 0x7F))          # ASCII 可见字符
    + "　、。，．：；！？（）【】《》“”‘’—…·×÷°"
    + "０１２３４５６７８９"
    + "幸运翻页机抽奖专用店名恭喜中奖谢谢参与一等奖二等奖三等奖四五六"
    + "票号时间页码校验有效期至请凭此到服务台领奖本票据当日有效"
    + "打印链路自检测试背景叠加文字应压在二维码上方式下方是普通"
    + "元年月日时分秒星期第页共张数量剩余库存中未参与奖"
    + "欢迎光临祝您好运客服电话地址网址微信扫码关注公众号"
)


def load_font(px, path=None):
    for p in ([path] if path else FONTS):
        try:
            return ImageFont.truetype(p, px)
        except Exception:
            continue
    raise SystemExit("找不到可用中文字体，请用 --font 指定 .ttf/.ttc")


def render(ch, font, w, h):
    img = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(img)
    # 用 baseline 对齐：汉字居中，ASCII 顶部留 1px
    try:
        d.text((w // 2, h // 2), ch, font=font, fill=0, anchor="mm")
    except Exception:
        d.text((0, 0), ch, font=font, fill=0)
    px = img.load()
    rows = []
    black = 0
    for y in range(h):
        row = bytearray((w + 7) // 8)
        for x in range(w):
            if px[x, y] < 128:
                row[x >> 3] |= 0x80 >> (x & 7)
                black += 1
        rows.append(bytes(row))
    return b"".join(rows), black


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chars", default="", help="额外字符")
    ap.add_argument("--all-gbk", action="store_true", help="覆盖全 GBK（约 1.5MB，慎用）")
    ap.add_argument("--gb2312", action="store_true",
                    help="覆盖完整 GB2312（6763 汉字 + 符号，约 540KB）——推荐")
    ap.add_argument("--out", default="main/assets/font24.bin")
    ap.add_argument("--font", default=None)
    args = ap.parse_args()

    chars = set(BASE) | set(args.chars)
    if args.gb2312:
        for hi in range(0xA1, 0xF8):
            for lo in range(0xA1, 0xFF):
                try:
                    chars.add(bytes([hi, lo]).decode("gb2312"))
                except Exception:
                    pass
    if args.all_gbk:
        for hi in range(0x81, 0xFF):
            for lo in range(0x40, 0xFF):
                try:
                    chars.add(bytes([hi, lo]).decode("gbk"))
                except Exception:
                    pass
    chars.discard("\n")
    chars.discard("\r")

    def load_all(px):
        out = []
        for p in ([args.font] if args.font else FONTS):
            try:
                out.append(ImageFont.truetype(p, px))
            except Exception:
                pass
        if not out:
            raise SystemExit("找不到可用中文字体，请用 --font 指定")
        return out

    ascii_fonts = load_all(18)              # 12×24 格子里放 18px 字身
    cjk_fonts = load_all(22)                # 24×24 格子

    entries, data = [], bytearray()
    missing = []
    for ch in sorted(chars):
        code = ord(ch)
        wide = code > 0x7F
        w, h = (CJK_W, CJK_H) if wide else (ASCII_W, ASCII_H)
        # 逐字体回退：某字体渲染为空白就换下一个（否则会静默缺字）
        bits, black = b"", 0
        for f in (cjk_fonts if wide else ascii_fonts):
            bits, black = render(ch, f, w, h)
            if black > 0:
                break
        if wide and black == 0:
            missing.append(ch)
        off = len(data)
        data += bits
        entries.append((code, w, h, off))

    blob = bytearray()
    blob += b"F24A"
    blob += struct.pack("<I", len(entries))
    for code, w, h, off in entries:
        blob += struct.pack("<IHHI", code, w, h, off)
    blob += data
    open(args.out, "wb").write(blob)

    cjk = sum(1 for c, w, h, o in entries if c > 0x7F)
    print(f"字库已生成 {args.out}")
    print(f"  字形 {len(entries)} 个（ASCII {len(entries)-cjk} / 汉字 {cjk}）")
    print(f"  体积 {len(blob)/1024:.1f} KB（点阵数据 {len(data)/1024:.1f} KB）")
    if missing:
        print(f"  ⚠ 无字形（{len(missing)} 个）: {''.join(missing[:20])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
