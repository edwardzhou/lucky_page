#include "settings.h"

#include <cstdint>
#include <vector>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_saver.h"

#define TAG "Settings"

Settings& Settings::GetInstance() {
    static Settings instance;
    return instance;
}

// ============================================================================
// 字段描述表（设计 D22）——Load()/SaveAll() 的唯一数据源。
// ⚠️ 新增字段三步：① settings.h 加成员+getter/setter ② 下表加一行 ③ 完成。
//    不进这张表的字段【不会】被 Load/Save 覆盖——这正是 page-player 踩过的坑。
// ============================================================================

namespace {

struct IntField {
    const char* key;
    int Settings::*storage;
    int defval;
};

struct I64Field {
    const char* key;
    int64_t Settings::*storage;
    int64_t defval;
};

struct StrField {
    const char* key;
    std::string Settings::*storage;
    const char* defval;
};

}  // namespace（表访问器需要全局作用域以匹配 settings.h 的 friend 声明）

struct SettingsFieldAccess {
static const std::vector<IntField>& IntFields() {
    static const std::vector<IntField> kFields = {
        {"cur_page",         &Settings::current_page_,        0},
        {"total_pages",      &Settings::total_pages_,         30},
        {"step_speed",       &Settings::step_speed_,          60},
        {"paging_ch_cnt",    &Settings::paging_channel_count_, 7},
        {"dmx_start_addr",   &Settings::dmx_start_addr_,      1},
        {"stroke_min_dmx",   &Settings::stroke_min_dmx_,      0},
        {"stroke_max_dmx",   &Settings::stroke_max_dmx_,      255},
        {"min_page_gap",    &Settings::min_page_gap_dmx_,    8},   // ≤15（NVS 限制）
        {"depletion_pol",   &Settings::depletion_policy_,    0},   // ≤15（NVS 限制）
        {"tmpl_mode",        &Settings::active_tmpl_mode_, 0},   // 当前生效模式
        {"pagemode",         &Settings::printer_pagemode_,     0},   // 键 <=14 字符
        {"total_draws",      &Settings::total_draws_,        0},    // 累计开奖（持久化）
        {"printer_baud",     &Settings::printer_baud_,        115200},
        {"printer_tx_pin",   &Settings::printer_tx_pin_,      9},    // ESP32 TX
        {"printer_rx_pin",   &Settings::printer_rx_pin_,      10},   // ESP32 RX
        {"printer_flpin",    &Settings::printer_flow_pin_,    11},   // 打印机 CTS→G11
        {"printer_flmod",    &Settings::printer_flow_mode_,   0},    // 0关/1CTS/2RTS/3双向
        {"paper_cols",       &Settings::paper_cols_,          32},
        {"qr_enabled",       &Settings::qr_enabled_,          1},
        {"logo_enabled",     &Settings::logo_enabled_,        0},
        {"auto_return_ms",   &Settings::auto_return_ms_,      1500},
        {"serial_daily",     &Settings::serial_daily_,        0},
        {"validity_days",    &Settings::validity_days_,       7},
        {"ip_mode",          &Settings::ip_mode_,          0},   // 0=DHCP / 1=静态 IP（学习 artnet）
        {"net_rst_cnt",    &Settings::net_reset_count_,  0},
    };
    return kFields;
}

static const std::vector<I64Field>& I64Fields() {
    static const std::vector<I64Field> kFields = {
        {"last_known_ep", &Settings::last_known_epoch_, 0},   // ≤15 字符（NVS_KEY_LEN_MAX，真机踩坑）
        {"net_rst_epoch",  &Settings::net_reset_epoch_,  0},
    };
    return kFields;
}

static const std::vector<StrField>& StrFields() {
    static const std::vector<StrField> kFields = {
        {"prize_pool",        &Settings::prize_pool_,        ""},
        {"prize_pool_bak",  &Settings::prize_pool_backup_, ""},    // ≤15（NVS 限制）
        {"wifi_list",        &Settings::wifi_list_,          ""},
        {"wifi_ssid",        &Settings::wifi_ssid_,          ""},
        {"wifi_pass",        &Settings::wifi_pass_,          ""},
        {"ap_ssid",          &Settings::ap_ssid_,          ""},   // 自定义热点名
        {"ap_pass",          &Settings::ap_pass_,          ""},   // 自定义热点密码（键 ≤15）
        {"st_ip",            &Settings::static_ip_,        ""},   // 静态 IP（键 ≤15）
        {"st_mask",          &Settings::static_mask_,      ""},
        {"st_gw",            &Settings::static_gw_,        ""},
        {"ticket_prefix",     &Settings::ticket_prefix_,     ""},
        {"serial_date",       &Settings::serial_date_,       ""},
        {"shop_name",         &Settings::shop_name_,         "幸运翻页机"},
        {"win_title",         &Settings::win_title_,         "恭喜中奖"},
        {"lose_title",        &Settings::lose_title_,        "感谢参与"},
        {"receipt_tmpl",      &Settings::receipt_tmpl_,      ""},   // flow 模板（沿用旧键）
        {"tmpl_canvas",       &Settings::tmpl_canvas_,       ""},   // canvas 模板
        {"tmpl_banner",       &Settings::tmpl_banner_,       ""},   // banner 模板
        {"receipt_header",    &Settings::receipt_header_,
         "{shop}{nl}=================={nl}"},   // ⚠️ 勿用 ═（GBK 无此码位）
        {"receipt_footer",    &Settings::receipt_footer_,
         "------------------------{nl}请凭此票到服务台领奖{nl}有效期至：{validity}{nl}"},
        {"timezone",          &Settings::timezone_str_,      "CST-8"},
    };
    return kFields;
}

};  // struct SettingsFieldAccess

// ----------------------------------------------------------------------------
// 通用 key-value（低频路径）
// ----------------------------------------------------------------------------

bool Settings::GetString(const std::string& key, std::string& value,
                         const std::string& default_value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        value = default_value;
        return false;
    }
    size_t required_size = 0;
    err = nvs_get_str(handle, key.c_str(), nullptr, &required_size);
    if (err == ESP_OK) {
        char* buffer = new (std::nothrow) char[required_size];
        if (buffer) {
            err = nvs_get_str(handle, key.c_str(), buffer, &required_size);
            if (err == ESP_OK) value.assign(buffer);
            delete[] buffer;
        } else {
            err = ESP_ERR_NO_MEM;
        }
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        value = default_value;
        return false;
    }
    return true;
}

int Settings::GetInt(const std::string& key, int default_value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &handle);
    if (err != ESP_OK) return default_value;
    int32_t value = 0;
    err = nvs_get_i32(handle, key.c_str(), &value);
    nvs_close(handle);
    return (err == ESP_OK) ? static_cast<int>(value) : default_value;
}

bool Settings::SetInt(const std::string& key, int value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_i32(handle, key.c_str(), static_cast<int32_t>(value));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_i32(%s) 失败: %s", key.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

bool Settings::SetString(const std::string& key, const std::string& value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_str(handle, key.c_str(), value.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) 失败: %s", key.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Load / SaveAll（字段表驱动）
// ----------------------------------------------------------------------------

bool Settings::Load() {
    for (const auto& f : SettingsFieldAccess::IntFields())
        this->*f.storage = GetInt(f.key, f.defval);
    for (const auto& f : SettingsFieldAccess::I64Fields()) {
        nvs_handle_t handle;
        if (nvs_open("settings", NVS_READONLY, &handle) == ESP_OK) {
            int64_t v = f.defval;
            if (nvs_get_i64(handle, f.key, &v) == ESP_OK) this->*f.storage = v;
            nvs_close(handle);
        } else {
            this->*f.storage = f.defval;
        }
    }
    for (const auto& f : SettingsFieldAccess::StrFields()) {
        std::string v;
        GetString(f.key, v, f.defval);
        this->*f.storage = std::move(v);
    }

    // R2.1（Task 0.3 Step 2c）：cell_count 恒为 1，提前到加载期强制。
    // lucky-page 无 direct_rows/cols（单机），无需检查。

    ESP_LOGI(TAG, "配置加载: pages=%d speed=%d paging_ch=%d dmx_addr=%d "
                  "stroke=[%d,%d] gap=%d printer=%d(baud)%d/%d flow=%d paper=%d "
                  "qr=%d logo=%d depletion=%d",
             total_pages_, step_speed_, paging_channel_count_, dmx_start_addr_,
             stroke_min_dmx_, stroke_max_dmx_, min_page_gap_dmx_,
             printer_baud_, printer_tx_pin_, printer_rx_pin_, printer_flow_mode_, paper_cols_,
             qr_enabled_, logo_enabled_, depletion_policy_);
    return true;
}

bool Settings::SaveAll() {
    // 陷阱日志：已存网络"凭空消失"过（真机事故）。若内存为空而 NVS 里有，
    // 说明是某处把 RAM 里的值清了再落盘 —— 这里喊一声，避免再查半天。
    {
        std::string on_disk;
        if (wifi_list_.empty() && GetString("wifi_list", on_disk, "") && !on_disk.empty()) {
            ESP_LOGW(TAG, "⚠ wifi_list 将被清空（NVS 中原本有 %zu 字节）—— 谁改的请看此处调用栈",
                     on_disk.size());
        }
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("settings", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return false;
    }
    bool ok = true;
    for (const auto& f : SettingsFieldAccess::IntFields())
        if (nvs_set_i32(handle, f.key, static_cast<int32_t>(this->*f.storage)) != ESP_OK) ok = false;
    for (const auto& f : SettingsFieldAccess::I64Fields())
        if (nvs_set_i64(handle, f.key, this->*f.storage) != ESP_OK) ok = false;
    for (const auto& f : SettingsFieldAccess::StrFields())
        if (nvs_set_str(handle, f.key, (this->*f.storage).c_str()) != ESP_OK) ok = false;
    if (nvs_commit(handle) != ESP_OK) ok = false;
    nvs_close(handle);
    if (!ok) ESP_LOGE(TAG, "SaveAll: 部分字段写入失败");
    return ok;
}

std::vector<std::string> Settings::DebugAllKeys() {
    std::vector<std::string> keys;
    for (const auto& f : SettingsFieldAccess::IntFields()) keys.push_back(f.key);
    for (const auto& f : SettingsFieldAccess::I64Fields()) keys.push_back(f.key);
    for (const auto& f : SettingsFieldAccess::StrFields()) keys.push_back(f.key);
    return keys;
}

void Settings::ResetNetworkConfig() {
    // 只回"网络"这一组：热点名/密码 + 已保存的 STA 网络与凭据 + IP 模式。
    // 用户在页面上做过的任何内容（奖池/模板/标定/计数）都必须原封不动。
    ap_ssid_.clear();        // → 默认 lucky-page-XXXX
    ap_pass_.clear();        // → 默认 12345678
    ip_mode_ = 0;            // → DHCP
    static_ip_.clear();
    static_mask_.clear();
    static_gw_.clear();
    wifi_ssid_.clear();      // 忘记当前网络
    wifi_pass_.clear();
    wifi_list_.clear();      // 忘记全部已保存网络
    SaveSync();
    ESP_LOGW("Settings", "网络配置已恢复默认（热点名/密码 + 已保存网络 + DHCP）；用户数据未改动");
}

void Settings::RequestSave() {
    NvsSaver::GetInstance().RequestSave();
}

bool Settings::SaveSync() {
    return SaveAll();   // D22：同步路径直接全量写（单次 open/commit）
}
