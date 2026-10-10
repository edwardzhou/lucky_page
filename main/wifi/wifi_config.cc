#include "wifi/wifi_config.h"

#include <cstdio>
#include <cstring>

#include <algorithm>

#include <cJSON.h>

#include "settings.h"

namespace {

/// JSON 字符串转义（SSID 是外部输入：路由器名可能含引号/反斜杠/控制字符）
std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;   // UTF-8 多字节原样透传（合法 JSON）
                }
        }
    }
    return out;
}

const char* WifiStatusReasonText(uint8_t reason) {
    switch (reason) {
        case 0: return "none";
        case 15: return "密码错误或认证失败(4WAY_HANDSHAKE_TIMEOUT)";
        case 201: return "找不到该 AP(NO_AP_FOUND)";
        case 203: return "认证失败(AUTH_FAIL)";
        case 205: return "连接失败(CONNECTION_FAIL)";
        default: return "其他";
    }
}

}  // namespace

std::string ValidateWifiCredentials(const std::string& ssid, const std::string& password) {
    if (ssid.empty()) return "SSID 不能为空";
    if (ssid.size() > 32) {
        return "SSID 超过 32 字节（当前 " + std::to_string(ssid.size()) + "；中文按 3 字节计）";
    }
    for (unsigned char c : ssid) {
        if (c < 0x20 || c == 0x7F) return "SSID 含控制字符（不可见字符）";
    }
    if (!password.empty()) {
        if (password.size() < 8) {
            return "密码少于 8 位（WPA2 要求 8..63 位；开放网络请留空）";
        }
        if (password.size() > 63) {
            return "密码超过 63 字节（当前 " + std::to_string(password.size()) + "）";
        }
        for (unsigned char c : password) {
            if (c < 0x20 || c == 0x7F) return "密码含控制字符（不可见字符）";
        }
    }
    return "";
}

WifiMode WifiDecideMode(const std::string& ssid) {
    return ssid.empty() ? WifiMode::kApOnly : WifiMode::kApSta;
}

const char* WifiModeName(WifiMode m) { return m == WifiMode::kApSta ? "APSTA" : "AP"; }

int WifiBackoffMs(int attempt) {
    if (attempt < 1) attempt = 1;
    const int kBase = 1000;
    const int kCap = 60000;
    int ms = kBase;
    for (int i = 1; i < attempt; ++i) {
        if (ms >= kCap) return kCap;
        ms *= 2;
    }
    return ms > kCap ? kCap : ms;
}

uint8_t WifiRssiToBars(int rssi) {
    if (rssi == 0) return 0;       // 未连接
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

std::string WifiScanEntryToJson(const WifiScanEntry& e) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"rssi\":%d,\"bars\":%u,\"secure\":%s}",
             JsonEscape(e.ssid).c_str(), e.rssi, (unsigned)WifiRssiToBars(e.rssi),
             e.secure ? "true" : "false");
    return buf;
}

std::string WifiScanListToJson(const std::vector<WifiScanEntry>& list) {
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i != 0) out += ",";
        out += WifiScanEntryToJson(list[i]);
    }
    out += "]";
    return out;
}

std::string WifiStatusToJson(const WifiStatusSnapshot& s) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"configured\":%s,\"sta_connected\":%s,\"mode\":\"%s\",\"ssid\":\"%s\","
             "\"ip\":\"%s\",\"rssi\":%d,\"bars\":%u,\"ap_clients\":%u,"
             "\"sta_state\":\"%s\",\"time_synced\":%s,\"last_reason\":\"%s\","
             "\"ap_up\":%s,\"ap_ssid\":\"%s\",\"ap_channel\":%u}",
             s.configured ? "true" : "false", s.sta_connected ? "true" : "false",
             s.mode_is_apsta ? "APSTA" : "AP", JsonEscape(s.ssid).c_str(), s.ip, (int)s.rssi,
             (unsigned)WifiRssiToBars(s.rssi), (unsigned)s.ap_clients,
             WifiStaStateName((WifiStaState)s.sta_state), s.time_synced ? "true" : "false",
             WifiStatusReasonText(s.last_disconnect_reason), s.ap_up ? "true" : "false",
             JsonEscape(s.ap_ssid).c_str(), (unsigned)s.ap_channel);
    return buf;
}

std::string SettingsSummaryForLog() {
    auto& s = Settings::GetInstance();
    // ⚠️ 只输出"密码是否已设置"，绝不输出明文（有测试锁定）
    std::string out = "wifi_ssid=" + s.wifi_ssid() + " wifi_pass_set=" +
                      (s.wifi_pass().empty() ? "0" : "1");
    return out;
}

const char* WifiStaStateName(WifiStaState s) {
    switch (s) {
        case WifiStaState::kIdle: return "IDLE";
        case WifiStaState::kScanning: return "SCANNING";
        case WifiStaState::kConnecting: return "CONNECTING";
        case WifiStaState::kConnected: return "CONNECTED";
        case WifiStaState::kDisconnected: return "DISCONNECTED";
    }
    return "?";
}

bool WifiStaCanScan(WifiStaState s) {
    // 扫描与连接/已连接互斥（ESP-IDF 会返回 ESP_ERR_WIFI_STATE；
    // page-player 曾因静默忽略该错误导致"点扫描无效果"）
    return s == WifiStaState::kIdle || s == WifiStaState::kDisconnected;
}

bool WifiStaCanConnect(WifiStaState s) {
    return s == WifiStaState::kIdle || s == WifiStaState::kDisconnected;
}

bool WifiSavedAdd(std::vector<WifiSavedNetwork>* list, const std::string& ssid,
                  const std::string& password) {
    if (list == nullptr || ssid.empty()) return false;
    for (auto& n : *list) {
        if (n.ssid == ssid) {
            n.password = password;             // 覆盖密码
            WifiSavedNetwork hit = n;
            list->erase(std::remove_if(list->begin(), list->end(),
                                       [&](const WifiSavedNetwork& x) { return x.ssid == ssid; }),
                        list->end());
            list->insert(list->begin(), hit);  // 提到最前（最近使用优先）
            return true;
        }
    }
    if ((int)list->size() >= kWifiMaxSaved) list->pop_back();   // 满则丢最旧
    list->insert(list->begin(), {ssid, password});
    return true;
}

bool WifiSavedPromote(std::vector<WifiSavedNetwork>* list, const std::string& ssid) {
    if (list == nullptr) return false;
    for (size_t i = 0; i < list->size(); ++i) {
        if ((*list)[i].ssid == ssid) {
            WifiSavedNetwork hit = (*list)[i];
            list->erase(list->begin() + i);
            list->insert(list->begin(), hit);
            return true;
        }
    }
    return false;
}

bool WifiSavedRemove(std::vector<WifiSavedNetwork>* list, int index) {
    if (list == nullptr || index < 0 || index >= (int)list->size()) return false;
    list->erase(list->begin() + index);
    return true;
}

std::string WifiSavedListToJsonInternal(const std::vector<WifiSavedNetwork>& list) {
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i != 0) out += ",";
        out += "{\"ssid\":\"" + JsonEscape(list[i].ssid) + "\",\"password\":\"" +
               JsonEscape(list[i].password) + "\"}";
    }
    out += "]";
    return out;
}

std::string WifiSavedListToJson(const std::vector<WifiSavedNetwork>& list) {
    // 只输出 SSID 与"是否有密码"，绝不回显密码明文
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i != 0) out += ",";
        out += "{\"ssid\":\"" + JsonEscape(list[i].ssid) + "\",\"has_password\":" +
               (list[i].password.empty() ? "false" : "true") + "}";
    }
    out += "]";
    return out;
}

std::vector<WifiSavedNetwork> WifiSavedListFromJson(const std::string& json, std::string* error) {
    std::vector<WifiSavedNetwork> out;
    if (json.empty()) return out;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        if (error) *error = "WiFi 列表 JSON 解析失败";
        return out;
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        if (error) *error = "WiFi 列表必须是数组";
        return out;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if ((int)out.size() >= kWifiMaxSaved) break;
        cJSON* ssid = cJSON_GetObjectItem(item, "ssid");
        cJSON* pass = cJSON_GetObjectItem(item, "password");
        if (!cJSON_IsString(ssid) || ssid->valuestring == nullptr) continue;
        WifiSavedNetwork n;
        n.ssid = ssid->valuestring;
        if (cJSON_IsString(pass) && pass->valuestring) n.password = pass->valuestring;
        if (ValidateWifiCredentials(n.ssid, n.password).empty()) out.push_back(n);
    }
    cJSON_Delete(root);
    return out;
}

std::string WebPathClean(const std::string& uri) {
    std::string s = uri;
    size_t cut = s.find_first_of("?#");
    if (cut != std::string::npos) s.resize(cut);
    return s;
}

bool WebPathIsSafe(const std::string& clean_path) {
    if (clean_path.empty() || clean_path[0] != '/') return false;
    if (clean_path.find("..") != std::string::npos) return false;   // 目录穿越
    if (clean_path.find('\0') != std::string::npos) return false;
    return true;
}

StaticRouteAction WebStaticDecide(const std::string& uri, bool file_exists) {
    const std::string clean = WebPathClean(uri);
    if (clean == "/" || clean == "/index.html") return StaticRouteAction::kServeIndex;
    if (!WebPathIsSafe(clean)) return StaticRouteAction::kCaptiveRedirect;   // 不安全 → 不碰文件系统
    if (file_exists) return StaticRouteAction::kServeFile;
    return StaticRouteAction::kCaptiveRedirect;
}
