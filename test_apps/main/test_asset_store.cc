// test_asset_store.cc — 素材库槽头/偏移纯逻辑单测（不碰 flash）
#include "lucky_test.h"
#include <cstring>
#include <vector>
#include "printer/asset_store.h"
#include "printer/printer_logo.h"

using namespace asset_store;

TEST(AssetStore, SlotMath) {
    EXPECT_TRUE(ValidId(0));
    EXPECT_TRUE(ValidId(kSlots - 1));
    EXPECT_FALSE(ValidId(-1));
    EXPECT_FALSE(ValidId(kSlots));
    EXPECT_EQ(0u, SlotOffset(0));
    EXPECT_EQ(SlotBytes() * 3u, SlotOffset(3));
    EXPECT_TRUE(SlotOffset(kSlots) == 0u);   // 越界返回 0（调用方须先 ValidId）
    // 槽大小按 flash 扇区对齐（49,152 = 48KiB）；48,392 不能 erase（真机 500 事故）
    constexpr size_t kAligned = ((PrinterLogo::kMaxLogoBytes + 4095) / 4096) * 4096;
    EXPECT_EQ(kAligned, SlotBytes());
    EXPECT_EQ(0u, SlotBytes() % 4096);
}

TEST(AssetStore, HeaderRoundTrip) {
    uint8_t hdr[64];
    PackSlotHeader(hdr, 7, 1234, 0xDEADBEEF, 576, 672, "prize-1");
    Info info;
    uint32_t len = 0;
    EXPECT_TRUE(ParseSlotHeader(hdr, sizeof(hdr), 7, &info, &len));
    EXPECT_EQ(7, info.id);
    EXPECT_EQ(1234u, len);
    EXPECT_EQ(576, info.width);
    EXPECT_EQ(672, info.height);
    EXPECT_EQ(std::string("prize-1"), info.name);
    EXPECT_TRUE(info.used);
}

TEST(AssetStore, HeaderRejectsWrongSlot) {
    uint8_t hdr[64];
    PackSlotHeader(hdr, 2, 100, 1, 8, 8, "x");
    Info info;
    EXPECT_FALSE(ParseSlotHeader(hdr, sizeof(hdr), 3, &info, nullptr));
}

TEST(AssetStore, HeaderRejectsCorruptMagicAndBadLength) {
    uint8_t hdr[64];
    PackSlotHeader(hdr, 0, 100, 1, 8, 8, "x");
    hdr[0] ^= 0xFF;
    EXPECT_FALSE(ParseSlotHeader(hdr, sizeof(hdr), 0, nullptr, nullptr));

    PackSlotHeader(hdr, 0, 4, 1, 8, 8, "x");   // 长度 < 8（不足 LGO1 头）
    EXPECT_FALSE(ParseSlotHeader(hdr, sizeof(hdr), 0, nullptr, nullptr));

    PackSlotHeader(hdr, 0, PrinterLogo::kMaxLogoBytes, 1, 8, 8, "x");  // 超过槽体
    EXPECT_FALSE(ParseSlotHeader(hdr, sizeof(hdr), 0, nullptr, nullptr));
}

TEST(AssetStore, NameIsTruncatedToCapacity) {
    uint8_t hdr[64];
    PackSlotHeader(hdr, 0, 100, 1, 8, 8, "123456789012345678901234567890");
    Info info;
    EXPECT_TRUE(ParseSlotHeader(hdr, sizeof(hdr), 0, &info, nullptr));
    EXPECT_EQ((size_t)kMaxNameBytes, info.name.size());
}
