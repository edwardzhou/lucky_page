// test_receipt_layout.cc — 排版工具函数（v3 块渲染器复用它们）
// 注：v2 的 Render/Substitute 已被 v3 取代，其行为断言见 test_receipt_template.cc
#include "lucky_test.h"

#include <string>
#include <vector>

#include "printer/gbk_encoder.h"
#include "printer/receipt_layout.h"

TEST(ReceiptLayoutUtil, DisplayWidthAsciiIsOne) {
    EXPECT_EQ(5u, ReceiptLayout::DisplayWidth("abcde"));
}

TEST(ReceiptLayoutUtil, DisplayWidthChineseIsTwoPerChar) {
    EXPECT_EQ(6u, ReceiptLayout::DisplayWidth("一等奖"));   // 3 字 × 2 列
}

TEST(ReceiptLayoutUtil, DisplayWidthMixed) {
    EXPECT_EQ(9u, ReceiptLayout::DisplayWidth("P3 一等奖"));   // 3 + 6
}

TEST(ReceiptLayoutUtil, DisplayWidthEmptyIsZero) {
    EXPECT_EQ(0u, ReceiptLayout::DisplayWidth(""));
}

TEST(ReceiptLayoutUtil, WrapToDoesNotSplitMultibyte) {
    // 20 个汉字 = 40 列；按 32 列折行 → 16 字 + 4 字，且每行都是合法 UTF-8
    std::string s;
    for (int i = 0; i < 20; ++i) s += "奖";
    auto rows = ReceiptLayout::WrapTo(s, 32);
    ASSERT_EQ(2u, rows.size());
    EXPECT_EQ(32u, ReceiptLayout::DisplayWidth(rows[0]));
    EXPECT_EQ(8u, ReceiptLayout::DisplayWidth(rows[1]));
    for (auto& r : rows) EXPECT_EQ(0u, r.size() % 3);   // 汉字 3 字节，不得被切断
}

TEST(ReceiptLayoutUtil, WrapToShortLineIsSingleRow) {
    auto rows = ReceiptLayout::WrapTo("abc", 32);
    ASSERT_EQ(1u, rows.size());
    EXPECT_STREQ("abc", rows[0].c_str());
}

TEST(ReceiptLayoutUtil, AlignedLeftCenterRight) {
    EXPECT_STREQ("AB   ", ReceiptLayout::Aligned("AB", 5, 0).c_str());
    EXPECT_STREQ(" AB  ", ReceiptLayout::Aligned("AB", 5, 1).c_str());
    EXPECT_STREQ("   AB", ReceiptLayout::Aligned("AB", 5, 2).c_str());
}

TEST(ReceiptLayoutUtil, AlignedWiderThanColsReturnsAsIs) {
    EXPECT_STREQ("ABCDEFG", ReceiptLayout::Aligned("ABCDEFG", 5, 1).c_str());
}

TEST(ReceiptLayoutUtil, ToGbkEncodesChinese) {
    EXPECT_STREQ("D2BBB5C8BDB1", [](const std::string& s) {
        std::string out;
        char buf[4];
        for (unsigned char c : s) {
            snprintf(buf, sizeof(buf), "%02X", c);
            out += buf;
        }
        return out;
    }(ReceiptLayout::ToGbk("一等奖")).c_str());
}
