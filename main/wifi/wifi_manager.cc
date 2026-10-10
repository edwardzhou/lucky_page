// wifi_manager.cc — AP + 可选 STA（设计 §8.1b）
//
// 真机踩过的坑（全部保留注释，避免重犯）：
//   ① esp_netif_init → esp_event_loop_create_default → esp_wifi_init → create netif
//      顺序错会 abort（缺事件循环时 wifi_default.c 内部 ESP_ERROR_CHECK 失败）
//   ② 控制台在 USB-Serial-JTAG 时，任务里 fgets 无输入会忙转饿死 IDLE
//   ③ 任何网络失败都不得 abort（抽奖机不能因为 WiFi 变砖）
#include "wifi/wifi_manager.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lottery/time_source.h"
#include "wifi/dns_server.h"
#include "settings.h"

#define TAG "WifiMgr"

static constexpr const char* kApPass = "12345678";
static constexpr int kApChannel = 6;
static constexpr int kApMaxConn = 4;

namespace {

bool s_up = false;
bool s_sta_running = false;
int s_retry = 0;
uint8_t s_last_reason = 0;
SemaphoreHandle_t s_lock = nullptr;
esp_netif_t* s_ap_netif = nullptr;
esp_netif_t* s_sta_netif = nullptr;
bool s_handlers_registered = false;
WifiStaState s_state = WifiStaState::kIdle;   // 显式状态机（借鉴 page-player）
std::vector<WifiSavedNetwork> s_candidates;   // 待尝试网络（按优先级）
size_t s_cand_index = 0;
bool s_scan_running = false;
bool s_scan_ready = false;
std::vector<WifiScanEntry> s_scan_cache;

void SetState(WifiStaState s) {
    if (s_state != s) {
        ESP_LOGI(TAG, "STA 状态: %s → %s", WifiStaStateName(s_state), WifiStaStateName(s));
    }
    s_state = s;
}

/// 由驱动事实刷新状态（事件是异步的，这里兜底对齐）
void WiFiApState() {
    if (!s_sta_running) {
        if (!s_scan_running) SetState(WifiStaState::kIdle);
        return;
    }
    if (s_scan_running) return;   // 扫描中不覆盖
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        SetState(WifiStaState::kConnected);
    } else if (s_state == WifiStaState::kConnected) {
        SetState(WifiStaState::kDisconnected);
    }
}
TaskHandle_t s_retry_task = nullptr;

void Lock() {
    if (s_lock != nullptr) xSemaphoreTake(s_lock, portMAX_DELAY);
}
void Unlock() {
    if (s_lock != nullptr) xSemaphoreGive(s_lock);
}

void FillStaIp(WifiStatusSnapshot* snap) {
    if (s_sta_netif == nullptr) return;
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(snap->ip, sizeof(snap->ip), IPSTR, IP2STR(&ip.ip));
    }
}

// 指数退避重连任务：连不上不刷屏、不阻塞任何人
void RetryTask(void*) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_sta_running) continue;
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_retry = 0;
            continue;   // 已连上
        }
        if (s_retry == 0) continue;   // 未处于失败态
        int delay_s = WifiBackoffMs(s_retry) / 1000;
        // 同一网络试满 3 次仍失败 → 换下一个已保存网络（多店铺/多 SSID 场景）
        if (s_retry > 3 && s_candidates.size() > 1) {
            s_cand_index = (s_cand_index + 1) % s_candidates.size();
            s_retry = 1;
            const auto& n = s_candidates[s_cand_index];
            ESP_LOGW(TAG, "换用下一个已保存网络：\"%s\"", n.ssid.c_str());
            wifi_config_t wc = {};
            memcpy(wc.sta.ssid, n.ssid.data(), n.ssid.size() > 32 ? 32 : n.ssid.size());
            memcpy(wc.sta.password, n.password.data(),
                   n.password.size() > 63 ? 63 : n.password.size());
            wc.sta.threshold.authmode = n.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
            wc.sta.pmf_cfg.capable = true;
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            SetState(WifiStaState::kConnecting);
            esp_wifi_connect();
            continue;
        }
        ESP_LOGW(TAG, "STA 重连（第 %d 次，%d s 后，当前 \"%s\"）", s_retry, delay_s,
                 s_candidates.empty() ? "" : s_candidates[s_cand_index].ssid.c_str());
        vTaskDelay(pdMS_TO_TICKS(WifiBackoffMs(s_retry)));
        if (!s_sta_running) continue;
        esp_wifi_connect();
    }
}

void StaEventHandler(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "STA 已关联 AP，等待 DHCP…");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
                s_last_reason = d ? d->reason : 0;
                if (s_retry < 100) ++s_retry;
                SetState(WifiStaState::kDisconnected);
                ESP_LOGW(TAG, "STA 断开（reason=%u）；AP 与抽奖不受影响，后台按退避重连",
                         (unsigned)s_last_reason);
                break;
            }
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "有客户端连上本机 AP");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "客户端离开本机 AP");
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = static_cast<ip_event_got_ip_t*>(data);
        s_retry = 0;
        SetState(WifiStaState::kConnected);
        ESP_LOGI(TAG, "STA 拿到 IP: " IPSTR "（网关 " IPSTR "）", IP2STR(&e->ip_info.ip),
                 IP2STR(&e->ip_info.gw));
        // 有网了 → 立刻重新发起 SNTP（否则要等默认 1 小时的重试间隔）
        s_last_reason = 0;   // 已连上，清掉上次失败原因（否则界面一直显示旧错误）
        TimeSource::GetInstance().KickSntp();
    }
}

void RegisterHandlersOnce() {
    if (s_handlers_registered) return;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &StaEventHandler, nullptr,
                                        nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &StaEventHandler, nullptr,
                                        nullptr);
    s_handlers_registered = true;
}

/// 载入已保存网络列表为候选队列
void LoadCandidates() {
    std::string err;
    s_candidates = WifiSavedListFromJson(Settings::GetInstance().wifi_list(), &err);
    s_cand_index = 0;
}

esp_err_t StartSta(const std::string& ssid, const std::string& pass) {
    wifi_config_t wc = {};
    memcpy(wc.sta.ssid, ssid.data(), ssid.size() > 32 ? 32 : ssid.size());
    memcpy(wc.sta.password, pass.data(), pass.size() > 63 ? 63 : pass.size());
    wc.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    // 模式在 Start() 里已统一设为 APSTA（扫描需要 STA 接口使能）
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) return err;
    s_sta_running = true;
    s_retry = 0;
    SetState(WifiStaState::kConnecting);
    err = esp_wifi_connect();
    ESP_LOGI(TAG, "STA 连接已发起: SSID=\"%s\" 密码%s", ssid.c_str(),
             pass.empty() ? "为空（开放网络）" : "已设置");
    return err;
}

void StopSta() {
    s_sta_running = false;
    s_retry = 0;
    esp_wifi_disconnect();
    // 保持 APSTA 模式：STA 接口留着，WiFi 配置页才能继续扫描
    wifi_config_t empty = {};
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    ESP_LOGI(TAG, "已断开路由器（AP 与扫描仍可用）");
}

}  // namespace

namespace wifi_manager {

bool IsUp() { return s_up; }

WifiStatusSnapshot Status() {
    WifiStatusSnapshot snap{};
    auto& s = Settings::GetInstance();
    const std::string& ssid = s.wifi_ssid();
    snap.configured = !ssid.empty();
    snprintf(snap.ssid, sizeof(snap.ssid), "%s", ssid.c_str());
    // 对外语义 = 是否配置了路由器（驱动内部恒为 APSTA 以便扫描，属实现细节）
    snap.mode_is_apsta = snap.configured;
    snap.last_disconnect_reason = s_last_reason;
    snap.time_synced = TimeSource::GetInstance().IsSynced();

    // 本机热点信息（UI 必须能看到，否则用户会以为"AP 不见了"）
    snap.ap_up = s_up;
    if (s_up) {
        wifi_config_t apc = {};
        if (esp_wifi_get_config(WIFI_IF_AP, &apc) == ESP_OK) {
            snprintf(snap.ap_ssid, sizeof(snap.ap_ssid), "%s", (const char*)apc.ap.ssid);
        }
        uint8_t primary = 0;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK) snap.ap_channel = primary;
    }

    Lock();
    WiFiApState();
    wifi_ap_record_t ap = {};
    if (s_sta_running && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snap.sta_connected = true;
        snap.rssi = (int8_t)ap.rssi;
    }
    snap.sta_state = (uint8_t)s_state;
    FillStaIp(&snap);
    wifi_sta_list_t list = {};
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
        snap.ap_clients = (uint8_t)(list.num > 255 ? 255 : list.num);
    }
    Unlock();
    return snap;
}

bool Scan(std::vector<WifiScanEntry>* out, std::string* error) {
    if (!s_up) {
        if (error) *error = "WiFi 未启动";
        return false;
    }
    // ⚠️ 状态白名单必须真的生效（page-player 状态表：CONNECTING/CONNECTED 禁止扫描）。
    // 实测教训：连着路由器时扫描会打断 STA 连接（射频离开服务信道），
    // 甚至让设备在网络上失联一段时间 —— 写了规则不接线等于没写。
    Lock();
    const WifiStaState st = s_state;
    Unlock();
    if (!WifiStaCanScan(st)) {
        if (error) {
            *error = (st == WifiStaState::kConnected)
                         ? "已连接路由器，扫描会中断网络。如需更换网络请先在下方删除已保存网络"
                         : "正在连接中，请稍候再扫描";
        }
        ESP_LOGW(TAG, "拒绝扫描：当前状态 %s（避免打断已建立的连接）", WifiStaStateName(st));
        return false;
    }
    Lock();
    s_scan_running = true;
    SetState(WifiStaState::kScanning);
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        s_scan_running = false;
        SetState(WifiStaState::kIdle);
        Unlock();
        if (error) *error = std::string("扫描启动失败: ") + esp_err_to_name(err);
        return false;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;
    std::vector<wifi_ap_record_t> recs(n);
    if (n > 0 && esp_wifi_scan_get_ap_records(&n, recs.data()) != ESP_OK) {
        s_scan_running = false;
        SetState(WifiStaState::kIdle);
        Unlock();
        if (error) *error = "读取扫描结果失败";
        return false;
    }
    s_scan_running = false;
    SetState(WifiStaState::kIdle);   // 扫描结束回到空闲（是否已连由状态刷新兜底）
    Unlock();

    out->clear();
    for (uint16_t i = 0; i < n; ++i) {
        WifiScanEntry e;
        e.ssid = reinterpret_cast<const char*>(recs[i].ssid);
        if (e.ssid.empty()) continue;   // 隐藏 SSID
        bool dup = false;
        for (auto& x : *out) {
            if (x.ssid == e.ssid) {     // 同名多 AP：留信号强的
                if (recs[i].rssi > x.rssi) x.rssi = recs[i].rssi;
                dup = true;
                break;
            }
        }
        if (dup) continue;
        e.rssi = recs[i].rssi;
        e.secure = recs[i].authmode != WIFI_AUTH_OPEN;
        out->push_back(e);
    }
    for (size_t i = 1; i < out->size(); ++i) {   // 信号强→弱（插入排序，n 很小）
        WifiScanEntry key = (*out)[i];
        size_t j = i;
        while (j > 0 && (*out)[j - 1].rssi < key.rssi) {
            (*out)[j] = (*out)[j - 1];
            --j;
        }
        (*out)[j] = key;
    }
    return true;
}

bool ApplySta(const std::string& ssid, const std::string& password, std::string* error) {
    std::string err = ValidateWifiCredentials(ssid, password);
    if (!err.empty()) {
        if (error) *error = err;
        return false;
    }
    auto& s = Settings::GetInstance();
    // 多网络：加入列表（最近使用排最前），并同步兼容字段
    LoadCandidates();
    WifiSavedAdd(&s_candidates, ssid, password);
    s.set_wifi_list(WifiSavedListToJsonInternal(s_candidates));
    s.set_wifi_ssid(ssid);
    s.set_wifi_pass(password);
    if (!s.SaveSync()) {
        if (error) *error = "保存失败（NVS 写入异常）";
        return false;
    }
    if (!s_up) {
        if (error) *error = "WiFi 未启动";
        return false;
    }
    Lock();
    esp_err_t e = StartSta(ssid, password);
    Unlock();
    if (e != ESP_OK) {
        if (error) *error = std::string("连接发起失败: ") + esp_err_to_name(e);
        return false;
    }
    return true;
}

std::vector<WifiSavedNetwork> SavedNetworks() {
    Lock();
    auto copy = s_candidates;
    Unlock();
    return copy;
}

bool RemoveSaved(int index) {
    auto& s = Settings::GetInstance();
    Lock();
    bool ok = WifiSavedRemove(&s_candidates, index);
    if (ok) {
        s.set_wifi_list(WifiSavedListToJsonInternal(s_candidates));
        if (s_candidates.empty()) {
            s.set_wifi_ssid("");
            s.set_wifi_pass("");
            s_cand_index = 0;
            if (s_up) StopSta();
        } else {
            s_cand_index = 0;
            s.set_wifi_ssid(s_candidates[0].ssid);
        }
        ok = s.SaveSync();
    }
    Unlock();
    ESP_LOGI(TAG, "删除已保存网络 index=%d: %s", index, ok ? "成功" : "失败");
    return ok;
}

bool ForgetSta() {
    auto& s = Settings::GetInstance();
    s.set_wifi_ssid("");
    s.set_wifi_pass("");
    s.set_wifi_list("");
    s_candidates.clear();
    s_cand_index = 0;
    bool ok = s.SaveSync();
    if (s_up) {
        Lock();
        StopSta();
        Unlock();
    }
    ESP_LOGI(TAG, "已忘记网络，回到 AP-only");
    return ok;
}

esp_err_t Start() {
    // ⚠️ 顺序不可乱（真机踩坑两次，均以 abort 形式暴露）
    esp_netif_init();
    esp_err_t el = esp_event_loop_create_default();
    if (el != ESP_OK && el != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "默认事件循环创建失败: %s（Web 不可用，抽奖照常）", esp_err_to_name(el));
        return el;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;   // 借鉴 page-player：驱动不写自己的 NVS 副本，省 flash 写入
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s（Web 不可用，抽奖照常）", esp_err_to_name(err));
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == nullptr) {
        ESP_LOGE(TAG, "创建 AP netif 失败（Web 不可用，抽奖照常）");
        return ESP_FAIL;
    }
    // ⚠️ 即使不连路由器也要建 STA netif：否则 AP-only 下 esp_wifi_scan_start 返回
    // ESP_FAIL，"先扫描再选网络"的配置流程直接不可用（真机踩坑）。
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == nullptr) {
        ESP_LOGW(TAG, "创建 STA netif 失败：WiFi 扫描将不可用（连接照常可试）");
    }
    RegisterHandlersOnce();
    if (s_lock == nullptr) s_lock = xSemaphoreCreateMutex();

    // AP 配置（SSID 用 MAC 后缀，避免多台同名）
    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "lucky-page-%02X%02X", mac[4], mac[5]);

    wifi_config_t apc = {};
    memcpy(apc.ap.ssid, ap_ssid, strlen(ap_ssid));
    apc.ap.ssid_len = strlen(ap_ssid);
    memcpy(apc.ap.password, kApPass, strlen(kApPass));
    // ⚠️ 信道必须为 0（自动跟随 STA 信道）——借鉴 page-player：
    // 写死信道在 APSTA 下会因运行时信道切换导致 STA auth 失败（真机教训）
    apc.ap.channel = 0;
    apc.ap.max_connection = kApMaxConn;
    apc.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    auto& s = Settings::GetInstance();
    const std::string ssid = s.wifi_ssid();
    const std::string pass = s.wifi_pass();
    const WifiMode mode = WifiDecideMode(ssid);

    // 统一以 APSTA 起：AP 保证 Web 可用，STA 接口使能以便扫描（是否连接看是否配了 SSID）
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) 失败: %s，退回纯 AP", esp_err_to_name(err));
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK) return err;
    }
    if ((err = esp_wifi_set_config(WIFI_IF_AP, &apc)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "AP 启动失败: %s（Web 不可用，抽奖照常）", esp_err_to_name(err));
        return err;
    }
    s_up = true;
    // 把 DHCP 下发的 DNS 指向本机，客户端 captive portal（CNA）探测才会打到我们的 DNS
    // 否则手机不会自动弹配置页（借鉴 page-player wifi_board.cc）
    if (s_ap_netif != nullptr) {
        esp_netif_ip_info_t ip_info = {};
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_ip_info(s_ap_netif, &ip_info);
        ip4_addr_t dns_addr = {};
        IP4_ADDR(&dns_addr, 192, 168, 4, 1);
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                               &dns_addr, sizeof(dns_addr));
        esp_netif_dhcps_start(s_ap_netif);
    }
    // 不省电（借鉴 page-player）：保证 HTTP/抽奖响应及时
    esp_wifi_set_ps(WIFI_PS_NONE);
    // 捕获门户 DNS：DHCP 已把客户端 DNS 指向本机，必须有服务在 53 端口应答，
    // 否则手机 CNA 探测解析失败 → 不自动弹配置页（真机问题 1）
    dns_server_start("192.168.4.1");
    ESP_LOGI(TAG, "AP 已启动: SSID=%s 密码=%s IP=192.168.4.1 模式=%s（信道自适应）", ap_ssid,
             kApPass, WifiModeName(mode));

    LoadCandidates();
    if (mode == WifiMode::kApSta) {
        if (xTaskCreate(RetryTask, "wifi_retry", 3072, nullptr, 4, &s_retry_task) != pdPASS) {
            ESP_LOGW(TAG, "重连任务创建失败（不影响 AP 与抽奖）");
        }
        std::string e2;
        if (!ApplySta(ssid, pass, &e2)) {
            ESP_LOGW(TAG, "STA 启动失败: %s（保持 AP-only）", e2.c_str());
        }
    }
    return ESP_OK;
}

}  // namespace wifi_manager
