// test_printer_logo.cc — LGO1 头校验 + GS v 0 分包（Task 1.4，TDD；含真实断言）
#include "lucky_test.h"
#include <string.h>
#include <vector>
#include "printer/printer_logo.h"

static void WriteLe16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static std::vector<uint8_t> MakeLogo(uint16_t w, uint16_t h) {
    std::vector<uint8_t> b(8 + (size_t)(w / 8) * h, 0);
    memcpy(b.data(), "LGO1", 4);
    WriteLe16(b.data() + 4, w);
    WriteLe16(b.data() + 6, h);
    return b;
}
#define U(s, i) ((unsigned char)((s)[(i)]))

// ---------- 头部校验（7 类非法输入全被拒） ----------
TEST(Logo, RejectsBadMagic) {
    auto buf = MakeLogo(384, 1);
    memcpy(buf.data(), "LGO2", 4);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    EXPECT_FALSE(info.valid);
    EXPECT_TRUE(info.error != nullptr);
}
TEST(Logo, RejectsNonMultipleOf8) {
    auto buf = MakeLogo(380, 1);   // 380 不是 8 的倍数
    EXPECT_FALSE(PrinterLogo::Validate(buf.data(), buf.size()).valid);
}
TEST(Logo, RejectsTooWide) {
    auto buf = MakeLogo(584, 1);   // > 576
    EXPECT_FALSE(PrinterLogo::Validate(buf.data(), buf.size()).valid);
}
TEST(Logo, RejectsTooTall) {
    auto buf = MakeLogo(384, 673);  // > 672（上限已由 256 抬到 672）
    EXPECT_FALSE(PrinterLogo::Validate(buf.data(), buf.size()).valid);
}
TEST(Logo, RejectsTruncatedData) {
    std::vector<uint8_t> buf(8 + 100, 0);
    memcpy(buf.data(), "LGO1", 4);
    WriteLe16(buf.data() + 4, 384);
    WriteLe16(buf.data() + 6, 96);   // 声明需要 8+4608 字节，只给 108
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    EXPECT_FALSE(info.valid);
    EXPECT_TRUE(info.error != nullptr);
}
TEST(Logo, RejectsExcessData) {
    auto buf = MakeLogo(384, 2);
    buf.push_back(0);   // 多 1 字节
    EXPECT_FALSE(PrinterLogo::Validate(buf.data(), buf.size()).valid);
}
TEST(Logo, AcceptsValid384x96) {
    auto buf = MakeLogo(384, 96);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(384, info.width_dots);
    EXPECT_EQ(96, info.height_dots);
    EXPECT_EQ(48 * 96, info.data_bytes);
}

// ---------- GS v 0 分包 ----------
TEST(Logo, RasterHeaderBytes) {
    // 384 dots 宽 = 48 B/行；96 行、每包 24 行 → 恰好 4 包
    auto buf = MakeLogo(384, 96);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    auto cmds = PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24);
    ASSERT_EQ(4u, cmds.size());
    const std::string& c = cmds[0];
    // GS v 0 m xL xH yL yH → 1D 76 30 00 30 00 18 00
    EXPECT_EQ(0x1D, U(c, 0));
    EXPECT_EQ(0x76, U(c, 1));
    EXPECT_EQ(0x30, U(c, 2));
    EXPECT_EQ(0x00, U(c, 3));
    EXPECT_EQ(0x30, U(c, 4));   // xL = 48
    EXPECT_EQ(0x00, U(c, 5));   // xH
    EXPECT_EQ(0x18, U(c, 6));   // yL = 24
    EXPECT_EQ(0x00, U(c, 7));   // yH
    EXPECT_EQ(8 + 24 * 48, (int)c.size());
}
TEST(Logo, SplitsIntoMultipleCommandsWhenTall) {
    // 分包理由 = 超时（M-2 修正）：12288B@115200≈1067ms > 1000ms 必然超时；
    // 分包成 1152B≈100ms 留 ~10 倍余量。
    auto buf = MakeLogo(384, 96);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    auto cmds = PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24);
    EXPECT_EQ(4u, cmds.size());
    for (auto& c : cmds) EXPECT_LE((int)c.size() - 8, 1152);   // 单包数据 ≤ 1152B
    for (auto& c : cmds) EXPECT_LT(c.size(), (size_t)10000);   // 防"一条装满 12KB"回归
}
TEST(Logo, LastPacketCarriesRemainder) {
    // 100 行 / 每包 24 行 → 5 包：24×4 + 余数 4 行（24*4=96, 100-96=4）
    auto buf = MakeLogo(384, 100);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    auto cmds = PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24);
    ASSERT_EQ(5u, cmds.size());
    EXPECT_EQ(24, U(cmds[0], 6));
    EXPECT_EQ(24, U(cmds[1], 6));
    EXPECT_EQ(24, U(cmds[2], 6));
    EXPECT_EQ(24, U(cmds[3], 6));    // 第 4 包仍满
    EXPECT_EQ(4, U(cmds[4], 6));     // 第 5 包余数 yL=4
    EXPECT_EQ(0x00, U(cmds[4], 7));
    EXPECT_EQ(8 + 4 * 48, (int)cmds[4].size());
}
TEST(Logo, TotalPayloadEqualsBitmapSize) {
    // 各包数据字节数之和 == width/8*height（不重不漏）
    const int w = 384, h = 100;
    auto buf = MakeLogo(w, h);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    auto cmds = PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24);
    size_t sum = 0;
    for (auto& c : cmds) sum += c.size() - 8;
    EXPECT_EQ((size_t)((w / 8) * h), sum);
}
TEST(Logo, EachPacketIsIndependentCommand) {
    // 每条指令必须以 1D 76 30 00 自带头（不能依赖上一条的包头）
    auto buf = MakeLogo(384, 96);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    for (auto& c : PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24)) {
        ASSERT_TRUE(c.size() >= 8u);
        EXPECT_EQ(0x1D, U(c, 0));
        EXPECT_EQ(0x76, U(c, 1));
        EXPECT_EQ(0x30, U(c, 2));
        EXPECT_EQ(0x00, U(c, 3));
    }
}
TEST(Logo, BitOrderIsMsbFirst) {
    // cmds[0] 是完整 GS v 0 指令 = 7 字节头 + 数据；第一个【数据】字节下标是 8
    // （.back() 是 data[47]=0x00，断言它 ==0x80 必然失败——N-1 的教训）
    auto buf = MakeLogo(384, 1);
    buf[8] = 0x80;   // 最左像素点亮
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    auto cmds = PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24);
    ASSERT_EQ(1u, cmds.size());
    EXPECT_EQ(0x80, U(cmds[0], 8));   // 首数据字节 MSB 优先（0x80 不是 0x01）
    EXPECT_EQ(0x00, U(cmds[0], 9));   // 第二字节仍为空
}
TEST(Logo, ReadsLe16NotHostOrder) {
    // 禁止 reinterpret_cast<LogoHeader*>：必须逐字节解析（buf[4] | buf[5]<<8）
    auto buf = MakeLogo(384, 1);
    buf[4] = 0x80;
    buf[5] = 0x01;   // LE → 0x0180 = 384
    // 宽度合法（384 是 8 的倍数）但数据长度按 384 宽重算：8 + 48*1 = 56 字节
    std::vector<uint8_t> full(buf.begin(), buf.begin() + 8);
    full.resize(8 + 48, 0);
    memcpy(full.data(), "LGO1", 4);
    full[4] = 0x80;
    full[5] = 0x01;
    WriteLe16(full.data() + 6, 1);
    auto info = PrinterLogo::Validate(full.data(), full.size());
    ASSERT_TRUE(info.valid);
    EXPECT_EQ(384, info.width_dots);
}
