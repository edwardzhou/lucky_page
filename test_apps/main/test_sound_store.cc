// test_sound_store.cc — 自定义音效槽头/偏移纯逻辑单测
#include "lucky_test.h"
#include <cstring>
#include "speaker/sound_store.h"
#include "printer/asset_store.h"

using namespace sound_store;

TEST(SoundStore, RoleNames) {
    EXPECT_STREQ("welcome", RoleName(kWelcome));
    EXPECT_STREQ("win", RoleName(kWin));
    EXPECT_STREQ("lose", RoleName(kLose));
    EXPECT_TRUE(RoleName(3) == nullptr);
    EXPECT_EQ(0, RoleFromName("welcome"));
    EXPECT_EQ(2, RoleFromName("lose"));
    EXPECT_EQ(-1, RoleFromName("bogus"));
}

TEST(SoundStore, SlotOffsetsAfterImageArea) {
    const size_t base = asset_store::SlotBytes() * (size_t)asset_store::kSlots;
    EXPECT_EQ(base, SlotOffset(0));
    EXPECT_EQ(base + kSlotBytes, SlotOffset(1));
    EXPECT_EQ(base + 2 * kSlotBytes, SlotOffset(2));
    // 必须 4KB 对齐（flash 擦除要求）
    for (int r = 0; r < kRoles; ++r) EXPECT_EQ(0u, SlotOffset(r) % 4096);
}

TEST(SoundStore, HeaderRoundTrip) {
    uint8_t hdr[32];
    PackHeader(hdr, kWin, 48000, 0x12345678);
    Info info;
    uint32_t len = 0;
    EXPECT_TRUE(ParseHeader(hdr, sizeof(hdr), kWin, &info, &len));
    EXPECT_EQ(48000u, len);
    EXPECT_TRUE(info.custom);
    EXPECT_EQ(1000u, info.duration_ms);
}

TEST(SoundStore, HeaderRejectsWrongRoleAndBadLen) {
    uint8_t hdr[32];
    PackHeader(hdr, kWelcome, 48000, 0);
    Info info;
    EXPECT_FALSE(ParseHeader(hdr, sizeof(hdr), kWin, &info, nullptr));
    PackHeader(hdr, kWelcome, 0, 0);
    EXPECT_FALSE(ParseHeader(hdr, sizeof(hdr), kWelcome, nullptr, nullptr));
    PackHeader(hdr, kWelcome, (uint32_t)kSlotBytes, 0);   // 超过槽体
    EXPECT_FALSE(ParseHeader(hdr, sizeof(hdr), kWelcome, nullptr, nullptr));
}
