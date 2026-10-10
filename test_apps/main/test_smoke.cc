// 冒烟测试（Task 0.4 Step 4）：验证测试宿主 + 兼容层可用
#include "lucky_test.h"
#include "page_mapping.h"
#include "settings.h"

TEST(Smoke, HostBuilds) {
    EXPECT_EQ(1 + 1, 2);
}

TEST(Smoke, SettingsDefaults) {
    // 字段表驱动（D22）：默认值随 Settings 实例可用
    auto& s = Settings::GetInstance();
    EXPECT_EQ(30, s.total_pages());
    EXPECT_EQ(1, s.cell_count());       // R2.1 恒为 1
    EXPECT_EQ(9, s.printer_tx_pin());     // ESP32 TX（用户实测接线）
    EXPECT_EQ(10, s.printer_rx_pin());   // ESP32 RX
    EXPECT_EQ(11, s.printer_flow_pin());  // 打印机 CTS
    EXPECT_EQ(0, s.printer_flow_mode());  // 默认关流控（防两个输出抢线）
    EXPECT_EQ(0, s.stroke_min_dmx());   // D21
    EXPECT_EQ(255, s.stroke_max_dmx());
}

TEST(Smoke, SettingsFieldTableRoundtrip) {
    // 字段表驱动的落盘/读回（B3 修复的核心验证：新字段必须能持久化）
    auto& s = Settings::GetInstance();
    int saved_pages = s.total_pages();
    s.set_total_pages(29);
    s.set_prize_pool("{\"version\":2,\"smoke\":true}");
    EXPECT_TRUE(s.SaveSync());
    EXPECT_TRUE(s.Load());
    EXPECT_EQ(29, s.total_pages());
    EXPECT_STREQ("{\"version\":2,\"smoke\":true}", s.prize_pool().c_str());
    // 还原
    s.set_total_pages(saved_pages);
    s.set_prize_pool("");
    s.SaveSync();
}

TEST(Smoke, AllNvsKeysFitLimit) {
    // 防复发：字段表所有 key ≤ 14 字符（NVS_KEY_LEN_MAX=15 含结束符）。
    // 真机踩坑两次：last_known_epoch(16)/min_page_gap_dmx(16) 写入静默失败。
    for (const auto& k : Settings::DebugAllKeys()) {
        EXPECT_LE(k.size(), (size_t)14);   // NVS_KEY_LEN_MAX=15 含 '\0' → 实可用 14
    }
}
