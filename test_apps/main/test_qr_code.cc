// test_qr_code.cc — ESC/POS QR (GS ( k) 字节流精确断言（Task 1.1，TDD）
//
// 设计 §6.3 的五条指令（逐字节核对过 Epson Function 165/167/169/180/181）：
//   设模型     1D 28 6B 04 00 31 41 32 00    (9 字节，n2=00 不可省！)
//   设模块尺寸 1D 28 6B 03 00 31 43 06
//   设纠错     1D 28 6B 03 00 31 45 31       (49 = M 级 15%)
//   存数据     1D 28 6B <pL=k+3> <pH=00> 31 50 30 <data>
//   打印       1D 28 6B 03 00 31 51 30
#include "lucky_test.h"
#include <string>
#include <string.h>
#include "raw_printer_qr.h"

static std::string Build(const char* text, uint8_t sz, uint8_t ecc) {
    uint8_t buf[192];
    size_t len = 0;
    TEST_ASSERT_EQUAL_MESSAGE(PRINTER_OK, raw_printer_qr_build(text, sz, ecc, buf, sizeof(buf), &len),
                              text);
    return std::string(reinterpret_cast<char*>(buf), len);
}
#define B(s, i) ((unsigned char)((s)[(i)]))

TEST(QrCode, FullSequenceFor2026) {
    // "2026" → k=4 → pL=7；总长 9+8+8+(8+4)+8 = 45 字节
    auto b = Build("2026", 6, 49);
    EXPECT_EQ(45u, b.size());
    // 设模型（9 字节，最后一个必须是 n2=0x00）
    EXPECT_EQ(0x1D, B(b, 0)); EXPECT_EQ(0x28, B(b, 1)); EXPECT_EQ(0x6B, B(b, 2));
    EXPECT_EQ(0x04, B(b, 3)); EXPECT_EQ(0x00, B(b, 4));   // pL=4, pH=0
    EXPECT_EQ(0x31, B(b, 5)); EXPECT_EQ(0x41, B(b, 6));   // cn=49, fn=65
    EXPECT_EQ(0x32, B(b, 7)); EXPECT_EQ(0x00, B(b, 8));   // n1=50, n2=0  ← 关键
    // 设模块尺寸
    EXPECT_EQ(0x1D, B(b, 9)); EXPECT_EQ(0x28, B(b, 10)); EXPECT_EQ(0x6B, B(b, 11));
    EXPECT_EQ(0x03, B(b, 12)); EXPECT_EQ(0x00, B(b, 13));
    EXPECT_EQ(0x31, B(b, 14)); EXPECT_EQ(0x43, B(b, 15)); EXPECT_EQ(0x06, B(b, 16));
    // 设纠错
    EXPECT_EQ(0x1D, B(b, 17)); EXPECT_EQ(0x28, B(b, 18)); EXPECT_EQ(0x6B, B(b, 19));
    EXPECT_EQ(0x03, B(b, 20)); EXPECT_EQ(0x00, B(b, 21));
    EXPECT_EQ(0x31, B(b, 22)); EXPECT_EQ(0x45, B(b, 23)); EXPECT_EQ(0x31, B(b, 24));
    // 存数据
    EXPECT_EQ(0x1D, B(b, 25)); EXPECT_EQ(0x28, B(b, 26)); EXPECT_EQ(0x6B, B(b, 27));
    EXPECT_EQ(0x07, B(b, 28)); EXPECT_EQ(0x00, B(b, 29));   // pL = 4+3 = 7
    EXPECT_EQ(0x31, B(b, 30)); EXPECT_EQ(0x50, B(b, 31)); EXPECT_EQ(0x30, B(b, 32));
    EXPECT_EQ('2', B(b, 33)); EXPECT_EQ('0', B(b, 34));
    EXPECT_EQ('2', B(b, 35)); EXPECT_EQ('6', B(b, 36));
    // 打印
    EXPECT_EQ(0x1D, B(b, 37)); EXPECT_EQ(0x28, B(b, 38)); EXPECT_EQ(0x6B, B(b, 39));
    EXPECT_EQ(0x03, B(b, 40)); EXPECT_EQ(0x00, B(b, 41));
    EXPECT_EQ(0x31, B(b, 42)); EXPECT_EQ(0x51, B(b, 43)); EXPECT_EQ(0x30, B(b, 44));
}

TEST(QrCode, ModelCommandIsNineBytesWithN2) {
    // 漏 n2 的实现会在"下一条指令的起点"暴露错位——用边界锁死
    auto b = Build("X", 3, 48);
    EXPECT_EQ(0x00, B(b, 8));       // n2
    EXPECT_EQ(0x1D, B(b, 9));       // 若漏 n2，这里会是 0x00 → 立刻红灯
    EXPECT_EQ(0x28, B(b, 10));
    EXPECT_EQ(0x6B, B(b, 11));
    EXPECT_EQ(0x03, B(b, 12));      // 尺寸指令的 pL
    EXPECT_EQ(0x43, B(b, 15));      // fn = 67 设模块尺寸
}

TEST(QrCode, StoreDataLengthFieldIsLenPlusThree) {
    // 票号 "20260929-0007"(13) + 校验 4 位 = 17 字节 → pL = 20 = 0x14
    auto b = Build("20260929-0007XXXX", 6, 49);
    EXPECT_EQ(9u + 8u + 8u, 25u);   // 前三条指令占 25 字节
    EXPECT_EQ(0x1D, B(b, 25));
    EXPECT_EQ(20, B(b, 28));        // pL = 17 + 3 = 20
    EXPECT_EQ(0x00, B(b, 29));      // pH
    EXPECT_EQ(0x31, B(b, 30));      // cn
    EXPECT_EQ(0x50, B(b, 31));      // fn = 80 存数据
    EXPECT_EQ(0x30, B(b, 32));      // m = 48
    EXPECT_EQ(58u, b.size());       // 9+8+8+(8+17)+8 = 58
}

TEST(QrCode, RejectsBadParams) {
    uint8_t buf[192]; size_t len = 0;
    EXPECT_TRUE(raw_printer_qr_build("ok", 0, 49, buf, sizeof(buf), &len) < 0);   // size<1
    EXPECT_TRUE(raw_printer_qr_build("ok", 17, 49, buf, sizeof(buf), &len) < 0);  // size>16
    EXPECT_TRUE(raw_printer_qr_build("ok", 6, 47, buf, sizeof(buf), &len) < 0);   // ecc<48
    EXPECT_TRUE(raw_printer_qr_build("ok", 6, 52, buf, sizeof(buf), &len) < 0);   // ecc>51
    EXPECT_TRUE(raw_printer_qr_build("", 6, 49, buf, sizeof(buf), &len) < 0);     // 空串
    // 缓冲不足 → NO_SPACE（不是截断）
    uint8_t tiny[16];
    EXPECT_EQ(PRINTER_ERR_NO_SPACE, raw_printer_qr_build("2026", 6, 49, tiny, sizeof(tiny), &len));
}
