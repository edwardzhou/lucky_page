// test_wifi_config.cc — WiFi STA 配置纯逻辑（设计 §8.1b）
// 覆盖：凭据校验 / 模式决策 / 退避 / JSON 转义 / 信号格 / 状态快照
#include "lucky_test.h"

#include <string>
#include <type_traits>
#include <vector>

#include "settings.h"
#include "wifi/wifi_config.h"

// ============================================================================
// 凭据校验
// ============================================================================
TEST(WifiCred, AcceptsTypical) {
    EXPECT_STREQ("", ValidateWifiCredentials("603sys", "12345678").c_str());
    EXPECT_STREQ("", ValidateWifiCredentials("MyShop-5G", "passw0rd!@#$").c_str());
}

TEST(WifiCred, OpenNetworkAllowsEmptyPassword) {
    EXPECT_STREQ("", ValidateWifiCredentials("FreeWiFi", "").c_str());
}

TEST(WifiCred, RejectsEmptySsid) {
    EXPECT_NE(std::string::npos, ValidateWifiCredentials("", "12345678").find("SSID"));
}

TEST(WifiCred, SsidLengthBoundaries) {
    // 32 字节（ASCII）= 上限，合法
    EXPECT_STREQ("", ValidateWifiCredentials(std::string(32, 'a'), "12345678").c_str());
    // 33 字节 → 拒绝
    EXPECT_NE(std::string::npos, ValidateWifiCredentials(std::string(33, 'a'), "12345678").find("32"));
}

TEST(WifiCred, SsidLengthCountsBytesNotCharacters) {
    // 中文 SSID：UTF-8 每字 3 字节 → 10 字 = 30 字节合法；11 字 = 33 字节越界
    std::string ok;
    for (int i = 0; i < 10; ++i) ok += "店";
    EXPECT_STREQ("", ValidateWifiCredentials(ok, "12345678").c_str());

    std::string too_long;
    for (int i = 0; i < 11; ++i) too_long += "店";
    EXPECT_NE(std::string::npos, ValidateWifiCredentials(too_long, "12345678").find("32"));
}

TEST(WifiCred, PasswordLengthRules) {
    // WPA2 密码：8..63；1..7 非法（很多路由器也拒绝）
    EXPECT_NE(std::string::npos, ValidateWifiCredentials("ap", "1234567").find("8"));
    EXPECT_STREQ("", ValidateWifiCredentials("ap", "12345678").c_str());
    EXPECT_STREQ("", ValidateWifiCredentials("ap", std::string(63, 'x')).c_str());
    EXPECT_NE(std::string::npos, ValidateWifiCredentials("ap", std::string(64, 'x')).find("63"));
}

TEST(WifiCred, RejectsControlCharacters) {
    EXPECT_NE(std::string::npos, ValidateWifiCredentials(std::string("ap\n"), "12345678").find("控制"));
    EXPECT_NE(std::string::npos, ValidateWifiCredentials("ap", std::string("1234\n567")).find("控制"));
    EXPECT_NE(std::string::npos, ValidateWifiCredentials(std::string("ap\tx"), "12345678").find("控制"));
}

TEST(WifiCred, AllowsSpacesAndSymbols) {
    // SSID 允许空格与常见符号（路由器常见）
    EXPECT_STREQ("", ValidateWifiCredentials("My Shop WiFi", "12345678").c_str());
    EXPECT_STREQ("", ValidateWifiCredentials("Cafe\"s WiFi", "12345678").c_str());
    EXPECT_STREQ("", ValidateWifiCredentials("a\\b", "12345678").c_str());
}

// ============================================================================
// 模式决策
// ============================================================================
TEST(WifiMode, NoSsidIsApOnly) {
    EXPECT_STREQ("AP", WifiModeName(WifiDecideMode("")));
}

TEST(WifiMode, SsidConfiguredIsApSta) {
    // 关键：有 SSID 时仍保留 AP，避免"密码配错就再也进不去配置页"
    EXPECT_STREQ("APSTA", WifiModeName(WifiDecideMode("603sys")));
}

TEST(WifiMode, AfterForgetBackToApOnly) {
    EXPECT_STREQ("AP", WifiModeName(WifiDecideMode("")));
}

// ============================================================================
// 重连退避
// ============================================================================
TEST(WifiBackoff, DoublesAndCaps) {
    EXPECT_EQ(1000, WifiBackoffMs(1));
    EXPECT_EQ(2000, WifiBackoffMs(2));
    EXPECT_EQ(4000, WifiBackoffMs(3));
    EXPECT_EQ(8000, WifiBackoffMs(4));
    EXPECT_EQ(16000, WifiBackoffMs(5));
    EXPECT_EQ(32000, WifiBackoffMs(6));
    EXPECT_EQ(60000, WifiBackoffMs(7));    // 32000*2=64000 → 封顶 60000
    EXPECT_EQ(60000, WifiBackoffMs(8));
    EXPECT_EQ(60000, WifiBackoffMs(1000));
}

TEST(WifiBackoff, HandlesInvalidAttempt) {
    EXPECT_EQ(1000, WifiBackoffMs(0));     // 0/负数按第 1 次
    EXPECT_EQ(1000, WifiBackoffMs(-5));
}

// ============================================================================
// 信号格
// ============================================================================
TEST(WifiSignal, RssiToBars) {
    EXPECT_EQ(4, WifiRssiToBars(-30));
    EXPECT_EQ(4, WifiRssiToBars(-55));
    EXPECT_EQ(3, WifiRssiToBars(-60));
    EXPECT_EQ(2, WifiRssiToBars(-70));
    EXPECT_EQ(1, WifiRssiToBars(-80));
    EXPECT_EQ(0, WifiRssiToBars(-95));
    EXPECT_EQ(0, WifiRssiToBars(0));       // 未连接
}

// ============================================================================
// JSON 序列化（安全：转义 + 绝不回显密码）
// ============================================================================
TEST(WifiJson, EscapesQuotesAndBackslashes) {
    // 路由器叫 `"; drop` 这类名字时，JSON 必须仍是合法的（否则前端解析崩/注入）
    WifiScanEntry e;
    e.ssid = "Cafe\"s \\ WiFi\n";
    e.rssi = -50;
    e.secure = true;
    std::string json = WifiScanEntryToJson(e);
    EXPECT_EQ(std::string::npos, json.find("\n"));      // 换行必须转义
    EXPECT_NE(std::string::npos, json.find("\\\""));
    EXPECT_NE(std::string::npos, json.find("\\\\"));
    // 引号成对（转义后不应出现裸引号破坏结构）
    int quotes = 0;
    for (size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '"' && (i == 0 || json[i - 1] != '\\')) ++quotes;
    }
    EXPECT_EQ(0, quotes % 2);
}

TEST(WifiJson, ScanListIsArray) {
    std::vector<WifiScanEntry> list = {{"A", -40, true}, {"B", -70, false}};
    std::string json = WifiScanListToJson(list);
    EXPECT_EQ('[', json[0]);
    EXPECT_EQ(']', json[json.size() - 1]);
    EXPECT_NE(std::string::npos, json.find("\"A\""));
    EXPECT_NE(std::string::npos, json.find("\"B\""));
}

TEST(WifiJson, ScanListEmptyIsEmptyArray) {
    EXPECT_STREQ("[]", WifiScanListToJson({}).c_str());
}

TEST(WifiJson, BarsAreDerivedFromRssiNotStored) {
    // 设计上只存 rssi，信号格由 WifiRssiToBars 派生 —— 结构上不可能出现"rssi 与格数矛盾"
    const int rssis[] = {-30, -60, -70, -80, -95, 0};
    for (int r : rssis) {
        WifiStatusSnapshot s{};
        s.rssi = (int8_t)r;
        std::string json = WifiStatusToJson(s);
        std::string want = "\"bars\":" + std::to_string(WifiRssiToBars(r));
        EXPECT_NE(std::string::npos, json.find(want));
    }
}

TEST(WifiJson, StatusNeverLeaksPassword) {
    // 状态 JSON 绝不能包含密码字段（哪怕已配置）
    WifiStatusSnapshot s{};
    s.configured = true;
    s.sta_connected = true;
    s.mode_is_apsta = true;
    s.ap_clients = 1;
    s.rssi = -55;
    s.time_synced = true;
    snprintf(s.ssid, sizeof(s.ssid), "%s", "603sys");
    snprintf(s.ip, sizeof(s.ip), "%s", "192.168.5.130");
    std::string json = WifiStatusToJson(s);
    EXPECT_EQ(std::string::npos, json.find("password"));
    EXPECT_EQ(std::string::npos, json.find("passwd"));
    EXPECT_NE(std::string::npos, json.find("603sys"));
    EXPECT_NE(std::string::npos, json.find("192.168.5.130"));
    EXPECT_NE(std::string::npos, json.find("\"configured\":true"));
    EXPECT_NE(std::string::npos, json.find("\"time_synced\":true"));
    EXPECT_NE(std::string::npos, json.find("\"bars\":4"));
}

TEST(WifiJson, StatusEscapesSsid) {
    WifiStatusSnapshot s{};
    s.configured = true;
    snprintf(s.ssid, sizeof(s.ssid), "%s", "a\"b\\c");
    std::string json = WifiStatusToJson(s);
    EXPECT_NE(std::string::npos, json.find("\\\""));
    EXPECT_EQ(std::string::npos, json.find("a\"b"));
}

// ============================================================================
// 状态快照：跨线程读，必须是平凡可拷贝的定长 POD（与 D23 同规）
// ============================================================================
TEST(WifiStatus, SnapshotIsTriviallyCopyablePod) {
    EXPECT_TRUE(std::is_trivially_copyable<WifiStatusSnapshot>::value);
    EXPECT_TRUE(std::is_standard_layout<WifiStatusSnapshot>::value);
    EXPECT_LE(sizeof(WifiStatusSnapshot), (size_t)192);
}

TEST(WifiStatus, SnapshotDefaultsAreSafe) {
    WifiStatusSnapshot s{};
    EXPECT_FALSE(s.configured);
    EXPECT_FALSE(s.sta_connected);
    EXPECT_EQ(0, s.ap_clients);
    EXPECT_EQ(0, s.rssi);
    EXPECT_EQ('\0', s.ssid[0]);
    EXPECT_EQ('\0', s.ip[0]);
}

// ============================================================================
// 持久化（Settings 往返；密码不进摘要日志）
// ============================================================================
TEST(WifiSettings, SsidPassRoundTripAndForget) {
    auto& s = Settings::GetInstance();
    const std::string saved_ssid = s.wifi_ssid();
    const std::string saved_pass = s.wifi_pass();

    s.set_wifi_ssid("TestNet-UT");   // 不用真实网络名
    s.set_wifi_pass("unit-test-pass");
    EXPECT_TRUE(s.SaveSync());

    s.set_wifi_ssid(saved_ssid);
    s.set_wifi_pass(saved_pass);
    EXPECT_TRUE(s.SaveSync());
}

TEST(WifiSettings, StatusSummaryDoesNotPrintPassword) {
    auto& s = Settings::GetInstance();
    std::string summary = SettingsSummaryForLog();
    EXPECT_NE(std::string::npos, summary.find("wifi_ssid="));
    EXPECT_EQ(std::string::npos, summary.find("unit-test-pass"));
    // 只暴露"是否已配置"
    EXPECT_NE(std::string::npos, summary.find("wifi_pass_set="));
}

// ============================================================================
// STA 状态机（借鉴 page-player：用显式状态+操作白名单替代散落布尔标志）
// ============================================================================
TEST(WifiStaState, NamesAreStable) {
    EXPECT_STREQ("IDLE", WifiStaStateName(WifiStaState::kIdle));
    EXPECT_STREQ("SCANNING", WifiStaStateName(WifiStaState::kScanning));
    EXPECT_STREQ("CONNECTING", WifiStaStateName(WifiStaState::kConnecting));
    EXPECT_STREQ("CONNECTED", WifiStaStateName(WifiStaState::kConnected));
    EXPECT_STREQ("DISCONNECTED", WifiStaStateName(WifiStaState::kDisconnected));
}

TEST(WifiStaState, ScanAllowedOnlyWhenIdleOrDisconnected) {
    // 这条规则直接对应 page-player 修的 bug："连接中/已连接时点扫描静默无效"
    EXPECT_TRUE(WifiStaCanScan(WifiStaState::kIdle));
    EXPECT_TRUE(WifiStaCanScan(WifiStaState::kDisconnected));
    EXPECT_FALSE(WifiStaCanScan(WifiStaState::kScanning));    // 防扫描堆叠
    EXPECT_FALSE(WifiStaCanScan(WifiStaState::kConnecting));
    EXPECT_FALSE(WifiStaCanScan(WifiStaState::kConnected));
}

TEST(WifiStaState, ConnectAllowedOnlyWhenIdleOrDisconnected) {
    EXPECT_TRUE(WifiStaCanConnect(WifiStaState::kIdle));
    EXPECT_TRUE(WifiStaCanConnect(WifiStaState::kDisconnected));
    EXPECT_FALSE(WifiStaCanConnect(WifiStaState::kScanning));
    EXPECT_FALSE(WifiStaCanConnect(WifiStaState::kConnecting));   // 防重复 connect
    EXPECT_FALSE(WifiStaCanConnect(WifiStaState::kConnected));
}

// ============================================================================
// 多网络列表（借鉴 page-player SsidManager）
// ============================================================================
TEST(WifiSavedList, AddInsertsAtFront) {
    std::vector<WifiSavedNetwork> l;
    EXPECT_TRUE(WifiSavedAdd(&l, "A", "12345678"));
    EXPECT_TRUE(WifiSavedAdd(&l, "B", "12345678"));
    ASSERT_EQ(2u, l.size());
    EXPECT_STREQ("B", l[0].ssid.c_str());   // 最近使用优先
    EXPECT_STREQ("A", l[1].ssid.c_str());
}

TEST(WifiSavedList, AddExistingOverwritesPasswordAndPromotes) {
    std::vector<WifiSavedNetwork> l;
    WifiSavedAdd(&l, "A", "oldpass1");
    WifiSavedAdd(&l, "B", "12345678");
    WifiSavedAdd(&l, "A", "newpass1");      // 改密码
    ASSERT_EQ(2u, l.size());                // 不重复添加
    EXPECT_STREQ("A", l[0].ssid.c_str());   // 提到最前
    EXPECT_STREQ("newpass1", l[0].password.c_str());
}

TEST(WifiSavedList, CapDropsOldest) {
    std::vector<WifiSavedNetwork> l;
    for (int i = 0; i < kWifiMaxSaved + 3; ++i) {
        WifiSavedAdd(&l, "net" + std::to_string(i), "12345678");
    }
    EXPECT_EQ(kWifiMaxSaved, (int)l.size());
    // 最新的在最前，最旧的被丢弃
    EXPECT_STREQ(("net" + std::to_string(kWifiMaxSaved + 2)).c_str(), l[0].ssid.c_str());
    for (auto& n : l) EXPECT_TRUE(n.ssid != "net0");
}

TEST(WifiSavedList, PromoteRemoveAndRejectEmptySsid) {
    std::vector<WifiSavedNetwork> l;
    WifiSavedAdd(&l, "A", "12345678");
    WifiSavedAdd(&l, "B", "12345678");
    EXPECT_TRUE(WifiSavedPromote(&l, "A"));
    EXPECT_STREQ("A", l[0].ssid.c_str());
    EXPECT_FALSE(WifiSavedPromote(&l, "not-there"));
    EXPECT_TRUE(WifiSavedRemove(&l, 0));
    EXPECT_STREQ("B", l[0].ssid.c_str());
    EXPECT_FALSE(WifiSavedRemove(&l, 99));
    EXPECT_FALSE(WifiSavedAdd(&l, "", "12345678"));   // 空 SSID 拒绝
}

TEST(WifiSavedList, JsonNeverLeaksPasswords) {
    std::vector<WifiSavedNetwork> l;
    WifiSavedAdd(&l, "ShopWiFi", "sup3rsecret");
    WifiSavedAdd(&l, "OpenNet", "");
    std::string json = WifiSavedListToJson(l);
    EXPECT_EQ(std::string::npos, json.find("sup3rsecret"));
    EXPECT_NE(std::string::npos, json.find("ShopWiFi"));
    EXPECT_NE(std::string::npos, json.find("\"has_password\":true"));
    EXPECT_NE(std::string::npos, json.find("\"has_password\":false"));
}

TEST(WifiSavedList, JsonRoundTripKeepsPasswordsForDeviceUse) {
    // 设备内部用（存 NVS）的 JSON 必须带密码，否则重启后连不上
    const char* internal = R"([{"ssid":"A","password":"12345678"},{"ssid":"B","password":""}])";
    std::string err;
    auto l = WifiSavedListFromJson(internal, &err);
    EXPECT_STREQ("", err.c_str());
    ASSERT_EQ(2u, l.size());
    EXPECT_STREQ("A", l[0].ssid.c_str());
    EXPECT_STREQ("12345678", l[0].password.c_str());
    EXPECT_STREQ("B", l[1].ssid.c_str());
    EXPECT_TRUE(l[1].password.empty());   // 开放网络
}

TEST(WifiSavedList, JsonParseRejectsGarbageAndInvalidEntries) {
    std::string err;
    EXPECT_TRUE(WifiSavedListFromJson("{not json", &err).empty());
    EXPECT_FALSE(err.empty());
    err.clear();
    // 非法项（空 SSID / 密码过短）被丢弃，合法项保留
    auto l = WifiSavedListFromJson(
        R"([{"ssid":"","password":"12345678"},{"ssid":"OK","password":"12345678"},{"ssid":"BAD","password":"123"}])",
        &err);
    ASSERT_EQ(1u, l.size());
    EXPECT_STREQ("OK", l[0].ssid.c_str());
}

// ============================================================================
// 本机热点信息必须出现在状态 JSON（用户反馈"看不到 AP"——UI 原本不显示热点名）
// ============================================================================
TEST(WifiJson, StatusExposesApInfo) {
    WifiStatusSnapshot s{};
    s.ap_up = true;
    s.ap_channel = 6;
    snprintf(s.ap_ssid, sizeof(s.ap_ssid), "%s", "lucky-page-0E6D");
    std::string json = WifiStatusToJson(s);
    EXPECT_NE(std::string::npos, json.find("\"ap_up\":true"));
    EXPECT_NE(std::string::npos, json.find("lucky-page-0E6D"));
    EXPECT_NE(std::string::npos, json.find("\"ap_channel\":6"));
}

TEST(WifiJson, StatusShowsApDownWhenNotStarted) {
    WifiStatusSnapshot s{};
    std::string json = WifiStatusToJson(s);
    EXPECT_NE(std::string::npos, json.find("\"ap_up\":false"));
}

TEST(WifiStatus, ApInfoDefaultsEmpty) {
    WifiStatusSnapshot s{};
    EXPECT_FALSE(s.ap_up);
    EXPECT_EQ(0, s.ap_channel);
    EXPECT_EQ('\0', s.ap_ssid[0]);
}

// ============================================================================
// Web 静态请求路径判定（真机 404 与路径穿越防护）
// 背景：ESP-IDF 默认只做精确匹配，/* 通配必须显式设 uri_match_fn，
// 否则所有页面 404 "Nothing matches the given URI"（真机踩坑，已修）
// ============================================================================
TEST(WebPath, CleanStripsQueryAndFragment) {
    EXPECT_STREQ("/wifi.html", WebPathClean("/wifi.html?x=1").c_str());
    EXPECT_STREQ("/wifi.html", WebPathClean("/wifi.html#top").c_str());
    EXPECT_STREQ("/", WebPathClean("/").c_str());
}

TEST(WebPath, RejectsTraversal) {
    EXPECT_FALSE(WebPathIsSafe("/../etc/passwd"));
    EXPECT_FALSE(WebPathIsSafe("/spiffs/../../secret"));
    EXPECT_FALSE(WebPathIsSafe("../../x"));
    EXPECT_FALSE(WebPathIsSafe(""));            // 必须 / 开头
    EXPECT_FALSE(WebPathIsSafe("relative"));
    EXPECT_TRUE(WebPathIsSafe("/wifi.html"));
    EXPECT_TRUE(WebPathIsSafe("/spiffs/logo.bin"));
}

TEST(WebPath, DecisionServeIndexFileOrRedirect) {
    EXPECT_TRUE(WebStaticDecide("/", false) == StaticRouteAction::kServeIndex);
    EXPECT_TRUE(WebStaticDecide("/index.html", false) == StaticRouteAction::kServeIndex);
    EXPECT_TRUE(WebStaticDecide("/wifi.html", true) == StaticRouteAction::kServeFile);
    // 未知路径 → 捕获跳转（手机 CNA 才会弹配置页）
    EXPECT_TRUE(WebStaticDecide("/generate_204", false) == StaticRouteAction::kCaptiveRedirect);
    EXPECT_TRUE(WebStaticDecide("/hotspot-detect.html", false) ==
                StaticRouteAction::kCaptiveRedirect);
    // 穿越尝试绝不落到文件系统分支
    EXPECT_TRUE(WebStaticDecide("/../settings", true) == StaticRouteAction::kCaptiveRedirect);
}

TEST(WebPath, KnownPagesAreServedWhenPresent) {
    const char* pages[] = {"/wifi.html", "/receipt.html", "/prizes.html",
                           "/logo.html", "/calibrate.html"};
    for (const char* p : pages) {
        EXPECT_TRUE(WebStaticDecide(p, true) == StaticRouteAction::kServeFile);
    }
}

// ============================================================================
// 联网参数校验（学习 artnet：AP 名可改 / 静态 IP 可配）
// 这些是"配错就失联"的现场事故点，必须在接口层就拦掉，故逐条锁定。
// 断言风格沿用本文件既有约定：EXPECT_STREQ(期望, 结果.c_str()) + 子串检查原因。
// ============================================================================

TEST(Ipv4, ParseAcceptsValidAndRejectsJunk) {
    uint32_t v = 0;
    EXPECT_TRUE(ParseIpv4("192.168.1.100", &v));
    EXPECT_EQ(0xC0A80164u, v);
    EXPECT_TRUE(ParseIpv4("0.0.0.0", &v));
    EXPECT_EQ(0u, v);
    EXPECT_TRUE(ParseIpv4("255.255.255.255", &v));
    EXPECT_EQ(0xFFFFFFFFu, v);

    EXPECT_FALSE(ParseIpv4("", &v));
    EXPECT_FALSE(ParseIpv4("192.168.1", &v));          // 段数不足
    EXPECT_FALSE(ParseIpv4("192.168.1.1.1", &v));      // 段数过多
    EXPECT_FALSE(ParseIpv4("192.168..1", &v));         // 空段
    EXPECT_FALSE(ParseIpv4("192.168.1.", &v));         // 结尾点
    EXPECT_FALSE(ParseIpv4("256.1.1.1", &v));          // 越界
    EXPECT_FALSE(ParseIpv4("192.168.1.01", &v));       // 前导零（易误读）
    EXPECT_FALSE(ParseIpv4("192.168.1.a", &v));        // 非数字
    EXPECT_FALSE(ParseIpv4("192.168.1.1 ", &v));       // 尾随空格
    EXPECT_FALSE(ParseIpv4("9999.1.1.1", &v));         // 段超长
}

TEST(ApSsid, AcceptsValidNames) {
    EXPECT_STREQ("", ValidateApSsid("lucky-page").c_str());
    EXPECT_STREQ("", ValidateApSsid("店铺一号").c_str());           // 中文 4 字 = 12 字节
    EXPECT_STREQ("", ValidateApSsid(std::string(31, 'a')).c_str()); // 31 字节正好合法
}

TEST(ApSsid, RejectsEmptyControlSpaceAndTooLong) {
    EXPECT_NE(std::string::npos, ValidateApSsid("").find("不能为空"));
    EXPECT_NE(std::string::npos, ValidateApSsid(" 前导空格").find("空格"));
    EXPECT_NE(std::string::npos, ValidateApSsid("尾随空格 ").find("空格"));
    EXPECT_NE(std::string::npos,
              ValidateApSsid(std::string("a") + '\x01' + "b").find("控制字符"));
    EXPECT_NE(std::string::npos, ValidateApSsid(std::string(32, 'a')).find("31"));
}

TEST(StaticIp, AcceptsSameSubnetConfig) {
    EXPECT_STREQ("", ValidateStaticIp("192.168.1.100", "255.255.255.0", "192.168.1.1").c_str());
    EXPECT_STREQ("", ValidateStaticIp("10.0.5.7", "255.255.0.0", "10.0.0.1").c_str());
}

TEST(StaticIp, RejectsNonContiguousMask) {
    EXPECT_NE(std::string::npos,
              ValidateStaticIp("192.168.1.100", "255.0.255.0", "192.168.1.1").find("掩码"));
    EXPECT_NE(std::string::npos,
              ValidateStaticIp("192.168.1.100", "255.255.255.1", "192.168.1.1").find("掩码"));
    EXPECT_NE(std::string::npos,
              ValidateStaticIp("192.168.1.100", "0.0.0.0", "192.168.1.1").find("掩码"));
}

TEST(StaticIp, RejectsGatewayOutsideSubnetOrEqualIp) {
    // 网关跨网段 → 配了就失联（现场最常见事故）
    EXPECT_NE(std::string::npos,
              ValidateStaticIp("192.168.1.100", "255.255.255.0", "192.168.2.1").find("网段"));
    EXPECT_NE(std::string::npos,
              ValidateStaticIp("192.168.1.100", "255.255.255.0", "192.168.1.100").find("网关"));
}

TEST(StaticIp, RejectsBadFormatAndUnusableAddress) {
    EXPECT_NE(std::string::npos,
              ValidateStaticIp("192.168.1", "255.255.255.0", "192.168.1.1").find("IP"));
    EXPECT_NE(std::string::npos,
              ValidateStaticIp("0.0.0.0", "255.255.255.0", "192.168.1.1").find("IP"));
    EXPECT_NE(std::string::npos,
              ValidateStaticIp("192.168.1.100", "255.255.255.0", "abc").find("网关"));
}

TEST(ApPassword, ValidationRules) {
    // 合法：8..63 字节
    EXPECT_STREQ("", ValidateApPassword("12345678").c_str());
    EXPECT_STREQ("", ValidateApPassword(std::string(63, 'x')).c_str());
    EXPECT_STREQ("", ValidateApPassword("我的店铺2026密码").c_str());   // 中文按字节计
    EXPECT_STREQ("", ValidateApPassword("").c_str());   // 空 = 复原默认（不报错）
    // 太短 / 太长
    EXPECT_NE(std::string::npos, ValidateApPassword("1234567").find("8 位"));
    EXPECT_NE(std::string::npos, ValidateApPassword(std::string(64, 'x')).find("63"));
    // 控制字符
    EXPECT_NE(std::string::npos,
              ValidateApPassword(std::string("abcd") + '\t' + "efgh").find("控制字符"));
}
