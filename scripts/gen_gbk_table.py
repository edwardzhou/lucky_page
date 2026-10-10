#!/usr/bin/env python3
"""gen_gbk_table.py — 生成 main/printer/gbk_table.cc（GBK 编码表，设计 §6.2 / D5）

结构（stride=191，评审 n5 修正：stride 190 会越界）：
  - 正查表 kGbkForward[126 * 191]：槽位 → Unicode 码点（uint16，0=空槽）
        index = (hi - 0x81) * 191 + (lo - 0x40)
        lo 取值 0x40..0xFE 含端点共 191 个位置；0x7F 保留为空占位槽
  - 反查表 kGbkReverse[7478]：码点 → GBK 编码（cp 升序，二分查找）
        子集 = GB2312 一级(3755) + 二级(3008) + 全角标点(715)

自检内建（不依赖人肉比对输出；Task 1.2 Step 2 / M-1）：
  assert len(forward) == 24066
  assert len(reverse) == 7478
  assert total == 78044
"""
import sys
import os

ROWS = 0xFE - 0x81 + 1        # 126 行（hi: 0x81..0xFE）
STRIDE = 0xFE - 0x40 + 1      # 191 列（lo: 0x40..0xFE，0x7F 为空占位槽）
assert STRIDE == 191, STRIDE

forward = [0] * (ROWS * STRIDE)
for hi in range(0x81, 0xFF):
    for lo in range(0x40, 0xFF):
        if lo == 0x7F:
            continue  # GBK 不使用 0x7F 作第二字节；保留占位槽保证 stride 自洽
        try:
            ch = bytes([hi, lo]).decode('gbk')
        except (UnicodeDecodeError, ValueError):
            continue
        cp = ord(ch)
        if cp > 0xFFFF:
            continue  # 仅 BMP（uint16 存不下增补平面）
        forward[(hi - 0x81) * STRIDE + (lo - 0x40)] = cp

# 反查子集：GB2312 一级 + 二级 + 全角标点区
reverse = []
for hi in list(range(0xB0, 0xD8)) + list(range(0xD8, 0xF8)) + list(range(0xA1, 0xAA)):
    for lo in range(0xA1, 0xFF):
        try:
            ch = bytes([hi, lo]).decode('gbk')
        except (UnicodeDecodeError, ValueError):
            continue
        if len(ch) == 1 and ord(ch) <= 0xFFFF:
            reverse.append((ord(ch), (hi << 8) | lo))
reverse.sort()

# ---- 自检（M-1：让脚本在写文件前校验，而不是靠人眼比对打印） ----
assert len(forward) == 24066, len(forward)
assert len(reverse) == 7478, len(reverse)
total = len(forward) * 2 + len(reverse) * 4
assert total == 78044, total
# 越界复核：最大合法索引必须 < 表长
assert (0xFE - 0x81) * STRIDE + (0xFE - 0x40) == 24065 < 24066

def emit(out, name, values, per_line=16, hexfmt="0x%04X"):
    out.write(f"const uint16_t {name}[{len(values)}] = {{\n")
    for i in range(0, len(values), per_line):
        chunk = ", ".join(hexfmt % v for v in values[i:i + per_line])
        out.write(f"    {chunk},\n")
    out.write("};\n\n")

def main():
    dst = os.path.join(os.path.dirname(__file__), "..", "main", "printer", "gbk_table.cc")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8") as f:
        f.write("// ============================================================\n")
        f.write("// gbk_table.cc — 生成物，勿手改（scripts/gen_gbk_table.py）\n")
        f.write("// 正查 24066 槽位(uint16) = 48132 B；反查 7478 项 = 29912 B\n")
        f.write("// 合计 78044 B ≈ 76.2 KB flash（设计 §6.2 / D5）\n")
        f.write("// ============================================================\n")
        f.write("#include \"printer/gbk_encoder.h\"\n\n")
        f.write("namespace {\n\n")
        emit(f, "kGbkForwardSlotToCp", forward)
        f.write(f"const GbkReverseEntry kGbkReverse[{len(reverse)}] = {{\n")
        for i in range(0, len(reverse), per_line := 8):
            chunk = ", ".join(f"{{0x{cp:04X}, 0x{g:04X}}}" for cp, g in reverse[i:i + 8])
            f.write(f"    {chunk},\n")
        f.write("};\n\n}  // namespace\n\n")
        f.write("""const uint16_t* GbkTableForward() { return kGbkForwardSlotToCp; }
size_t GbkTableForwardLen() { return sizeof(kGbkForwardSlotToCp) / sizeof(uint16_t); }
const GbkReverseEntry* GbkTableReverse() { return kGbkReverse; }
size_t GbkTableReverseLen() { return sizeof(kGbkReverse) / sizeof(GbkReverseEntry); }
""")
    print(f"forward_slots={len(forward)} reverse_entries={len(reverse)} total_bytes={total}")
    print(f"已生成: {os.path.normpath(dst)}")

if __name__ == "__main__":
    main()
