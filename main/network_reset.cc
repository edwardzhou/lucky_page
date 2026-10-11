#include <ctime>

#include "network_reset.h"

#include "esp_log.h"
#include "settings.h"
#include "wifi/wifi_manager.h"

#define TAG "NetReset"

bool ResetNetworkConfigToDefault(const char* origin) {
    // 持久化证据：次数 + 时间戳（页面会显示）。真机事故中"没人按却被清"无法定位，
    // 有了它下次就能直接看到"什么时候被复位过、共几次"。
    auto& st = Settings::GetInstance();
    st.set_net_reset_count(st.net_reset_count() + 1);
    st.set_net_reset_epoch((int64_t)time(nullptr));
    ESP_LOGW(TAG, "=== 恢复网络默认配置（来源：%s）第 %d 次，时间 %lld ===", origin,
             st.net_reset_count(), (long long)st.net_reset_epoch());
    // ① 清 Settings 的网络字段（只清网络；用户数据由单测保证不动）
    st.ResetNetworkConfig();
    // ② 应用到运行时：热点名/密码回默认、IP 回 DHCP、断开并清空 STA 凭据
    std::string err;
    const bool ok_ssid = wifi_manager::SetApSsid("", &err);
    const bool ok_pass = wifi_manager::SetApPassword("", &err);
    const bool ok_dhcp = wifi_manager::UseDhcp(&err);
    wifi_manager::ForgetSta();          // 断开 STA + 清空已保存网络（页面会显示"仅热点"）
    ESP_LOGW(TAG, "热点名回默认=%d 密码回默认=%d IP回DHCP=%d；重新连接热点 %s / 12345678 → 192.168.4.1",
             ok_ssid, ok_pass, ok_dhcp, wifi_manager::Status().ap_ssid);
    return ok_ssid && ok_pass && ok_dhcp;
}
