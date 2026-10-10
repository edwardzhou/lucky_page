// test_gbk_encoder.cc — UTF-8→GBK 转码（Task 1.2，TDD）
// 期望值来自 python3 实测："一等奖".encode('gbk').hex().upper() == "D2BBB5C8BDB1"
#include "lucky_test.h"
#include <string>
#include "printer/gbk_encoder.h"

static std::string Hex(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        out += kHex[c >> 4];
        out += kHex[c & 0xF];
    }
    return out;
}

TEST(GbkEncoder, ChinesePrize) {
    auto r = GbkEncoder::Encode("一等奖");
    EXPECT_STREQ("D2BBB5C8BDB1", Hex(r.bytes).c_str());   // 末字节 B1 不是 31
    EXPECT_EQ(0u, r.replaced);
}

TEST(GbkEncoder, AllDefaultTierNames) {
    // 默认奖池的全部等级名 + 常用小票字样都必须零替换（GB2312 覆盖）
    auto r = GbkEncoder::Encode("一等奖二等奖三等奖谢谢参与恭喜中奖感谢参与"
                                "幸运翻页机奖品扫地机器人蓝牙音箱定制水杯"
                                "票号时间页码校验请凭此到服务台领奖有效期至欢迎下次再来");
    EXPECT_EQ(0u, r.replaced);
}

TEST(GbkEncoder, AsciiPassThrough) {
    auto r = GbkEncoder::Encode("A1-2026");
    EXPECT_STREQ("A1-2026", r.bytes.c_str());
    EXPECT_EQ(0u, r.replaced);
}

TEST(GbkEncoder, MixedChineseAscii) {
    auto r = GbkEncoder::Encode("票号:2026");
    // "票号"=C6B1BAC5，':'=3A，"2026"=32303236 —— Hex() 整串 hex 化，期望也须全 hex
    EXPECT_STREQ("C6B1BAC53A32303236", Hex(r.bytes).c_str());
    EXPECT_EQ(0u, r.replaced);
}

TEST(GbkEncoder, UnmappedBecomesQuestion) {
    auto r = GbkEncoder::Encode("\xF0\x9F\x98\x80");  // U+1F600 emoji，BMP 之外
    EXPECT_STREQ("?", r.bytes.c_str());
    EXPECT_EQ(1u, r.replaced);
}

TEST(GbkEncoder, InvalidUtf8DoesNotCrash) {
    auto r = GbkEncoder::Encode("\xFF\xFE\x80");
    EXPECT_TRUE(r.replaced >= 1u);
}

TEST(GbkEncoder, LookupGbkKnownValues) {
    EXPECT_EQ((uint16_t)0xD2BB, GbkEncoder::LookupGbk(0x4E00));  // 一
    EXPECT_EQ((uint16_t)'A', GbkEncoder::LookupGbk('A'));        // ASCII
    EXPECT_EQ(0u, GbkEncoder::LookupGbk(0x1F600));               // emoji
}

TEST(GbkEncoder, TableSanity) {
    // 生成表的自检在脚本内已断言；这里验证链接产物
    EXPECT_EQ(24066u, GbkTableForwardLen());
    EXPECT_EQ(7478u, GbkTableReverseLen());
    // 反查表必须 cp 升序（二分查找的前提）
    const GbkReverseEntry* t = GbkTableReverse();
    for (size_t i = 1; i < GbkTableReverseLen(); ++i) {
        EXPECT_TRUE(t[i - 1].cp < t[i].cp);
    }
}

TEST(GbkEncoder, DecorationFallsBackInsteadOfQuestionMark) {
    // 实测覆盖（勿凭直觉假设 GBK 缺哪些符号）：
    //   ═ U+2550 → 缺码位，走降级 "="
    //   ─ U+2500 / ★ U+2605 / ├ U+251C / │ U+2502 → GBK 本就有（A9A4/A1EF/A9C0/A9A6）
    auto eq = GbkEncoder::Encode("═");
    EXPECT_EQ(0u, eq.replaced);          // 不是「无解」
    EXPECT_EQ(1u, eq.fallback);          // 而是「降级」
    EXPECT_STREQ("=", std::string(eq.bytes.begin(), eq.bytes.end()).c_str());

    auto direct = GbkEncoder::Encode("─★├│");
    EXPECT_EQ(0u, direct.replaced);
    EXPECT_EQ(0u, direct.fallback);      // 全部直接命中 GBK
    EXPECT_STREQ("A9A4A1EFA9C0A9A6", Hex(direct.bytes).c_str());

    // 组合串：一个降级 + 四个直编，且绝不出现 '?'
    auto mix = GbkEncoder::Encode("═─★├│");
    EXPECT_EQ(0u, mix.replaced);
    EXPECT_EQ(1u, mix.fallback);
    EXPECT_EQ(std::string::npos,
              std::string(mix.bytes.begin(), mix.bytes.end()).find('?'));
}

TEST(GbkEncoder, DefaultSeparatorsEncodable) {
    auto a = GbkEncoder::Encode("==================");
    auto b = GbkEncoder::Encode("------------------------");
    EXPECT_EQ(0u, a.replaced);
    EXPECT_EQ(0u, a.fallback);
    EXPECT_EQ(0u, b.replaced);
    EXPECT_EQ(0u, b.fallback);
    EXPECT_EQ(18u, a.bytes.size());
    EXPECT_EQ(24u, b.bytes.size());
}
