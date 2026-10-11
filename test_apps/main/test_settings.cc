// Settings 单测：数据驱动字段表 + 网络恢复默认的**边界**
//
// 重点：ResetNetworkConfig（硬件恢复按钮/串口 wifi resetnet 走它）只能动网络相关字段。
// 这里用"哨兵值"锁死：给用户数据字段塞入可识别的值，复位后必须原封不动 ——
// 防止以后有人往复位里加一行把用户的奖池/模板/标定清掉（数据安全事故）。
#include "lucky_test.h"
#include "settings.h"

TEST(Settings, FieldTableCoversAllKeysForSave) {
    auto& s = Settings::GetInstance();
    // 字段表是 Load/Save 的唯一数据源：key 必须非空且 ≤15 字节（NVS_KEY_LEN_MAX）
    auto keys = s.DebugAllKeys();
    EXPECT_TRUE(keys.size() > 30);
    for (const auto& k : keys) {
        EXPECT_TRUE(!k.empty());
        EXPECT_TRUE(k.size() <= 15);
    }
}

TEST(Settings, NetworkResetClearsOnlyNetworkFields) {
    auto& s = Settings::GetInstance();

    // ---- 网络字段：故意设成"非默认" ----
    s.set_ap_ssid("my-shop-ap");
    s.set_ap_pass("my-shop-pass-2026");
    s.set_ip_mode(1);
    s.set_static_ip("192.168.1.100");
    s.set_static_mask("255.255.255.0");
    s.set_static_gw("192.168.1.1");
    s.set_wifi_ssid("SomeRouter");
    s.set_wifi_pass("router-pass-123");

    // ---- 用户数据哨兵：复位后必须一模一样 ----
    const std::string sentinel_pool = "{\"tiers\":[{\"name\":\"一等奖\",\"stock\":7}]}";
    const std::string sentinel_tmpl = "{\"blocks\":[{\"type\":\"text\",\"text\":\"哨兵-flow\"}]}";
    const std::string sentinel_canvas = "{\"blocks\":[{\"type\":\"bg\",\"src\":\"sentinel\"}]}";
    const std::string sentinel_shop = "哨兵店铺名";
    s.set_prize_pool(sentinel_pool);
    s.set_receipt_tmpl(sentinel_tmpl);
    s.set_tmpl_canvas(sentinel_canvas);
    s.set_shop_name(sentinel_shop);
    const int sentinel_draws = s.total_draws() + 123;
    s.set_total_draws(sentinel_draws);
    const std::string sentinel_prefix = "SENTINEL-";
    s.set_ticket_prefix(sentinel_prefix);

    s.ResetNetworkConfig();

    // ① 网络字段回默认
    EXPECT_STREQ("", s.ap_ssid().c_str());
    EXPECT_STREQ("", s.ap_pass().c_str());        // → 运行时等于默认密码 12345678
    EXPECT_EQ(0, s.ip_mode());                     // → DHCP
    EXPECT_STREQ("", s.static_ip().c_str());
    EXPECT_STREQ("", s.static_mask().c_str());
    EXPECT_STREQ("", s.static_gw().c_str());
    EXPECT_STREQ("", s.wifi_ssid().c_str());
    EXPECT_STREQ("", s.wifi_pass().c_str());

    // ② ⭐ 用户数据必须原封不动（这是本测试的核心）
    EXPECT_STREQ(sentinel_pool.c_str(), s.prize_pool().c_str());
    EXPECT_STREQ(sentinel_tmpl.c_str(), s.receipt_tmpl().c_str());
    EXPECT_STREQ(sentinel_canvas.c_str(), s.tmpl_canvas().c_str());
    EXPECT_STREQ(sentinel_shop.c_str(), s.shop_name().c_str());
    EXPECT_EQ(sentinel_draws, s.total_draws());
    EXPECT_STREQ(sentinel_prefix.c_str(), s.ticket_prefix().c_str());

    // 收尾：把两个字段还原成出厂默认，避免影响后续测试/真机数据
    s.set_shop_name("幸运翻页机");
    s.set_total_draws(sentinel_draws - 123);
    s.SaveSync();
}
