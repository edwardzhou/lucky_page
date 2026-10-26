#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#include <string>
#include <vector>

struct SettingsFieldAccess;  // 字段描述表访问器（settings.cc 内部用）

/// lucky-page 配置管理（数据驱动字段表，设计 D22）。
///
/// ⚠️ 本文件与 page-player 的 Settings 有本质区别（评审 BLOCKER B3 的修复）：
/// page-player 的 NvsSaver::DoSave() 是硬编码字段清单，新增字段不会落盘；
/// lucky-page 用【字段描述表】统一驱动 Load()/SaveAll()——
/// 新增字段只需：①加成员 ②在 .cc 的字段表里加一行，Load/Save 自动覆盖。
///
/// 同步/异步划分（D22）：
/// - 库存、票号流水 → SaveSync()（低频、要求强持久化）
/// - current_page 等高频状态 → RequestSave()（可容忍丢失）
class Settings {
public:
    static Settings& GetInstance();

    bool Load();
    /// 全字段写 NVS（单次 open/commit 批量写）
    bool SaveAll();

    void RequestSave();   // 异步（队列满静默丢弃——仅用于可容忍丢失的字段）
    bool SaveSync();      // 同步阻塞写盘（库存/票号必须走这条）

    // ---- 通用 key-value（Web/RDM 等低频路径用；不走字段表） ----
    bool GetString(const std::string& key, std::string& value, const std::string& default_value = "");
    bool SetString(const std::string& key, const std::string& value);
    int GetInt(const std::string& key, int default_value = 0);
    bool SetInt(const std::string& key, int value);

    // ---- 翻页 ----
    int current_page() const { return current_page_; }
    void set_current_page(int v) { current_page_ = v; }
    int total_pages() const { return total_pages_; }
    void set_total_pages(int v) { total_pages_ = v; }
    int step_speed() const { return step_speed_; }
    void set_step_speed(int v) { step_speed_ = v; }
    int paging_channel_count() const { return paging_channel_count_; }
    void set_paging_channel_count(int v) { paging_channel_count_ = v; }
    int dmx_start_addr() const { return dmx_start_addr_; }
    void set_dmx_start_addr(int v) { dmx_start_addr_ = v; }

    /// lucky-page 恒为单机（R2.1）。设置其他值会被强制纠正并记日志。
    int cell_count() const { return 1; }

    // ---- 行程跨度（设计 §4.2b / D21） ----
    int stroke_min_dmx() const { return stroke_min_dmx_; }
    void set_stroke_min_dmx(int v) { stroke_min_dmx_ = v; }
    int stroke_max_dmx() const { return stroke_max_dmx_; }
    void set_stroke_max_dmx(int v) { stroke_max_dmx_ = v; }
    int min_page_gap_dmx() const { return min_page_gap_dmx_; }
    void set_min_page_gap_dmx(int v) { min_page_gap_dmx_ = v; }

    // ---- 奖池（JSON，见设计 §7.2） ----
    const std::string& prize_pool() const { return prize_pool_; }
    void set_prize_pool(const std::string& v) { prize_pool_ = v; }
    const std::string& prize_pool_backup() const { return prize_pool_backup_; }
    void set_prize_pool_backup(const std::string& v) { prize_pool_backup_ = v; }
    int depletion_policy() const { return depletion_policy_; }
    void set_depletion_policy(int v) { depletion_policy_ = v; }

    // ---- 打印机 ----
    bool printer_pagemode() const { return printer_pagemode_ != 0; }
    void set_printer_pagemode(bool v) { printer_pagemode_ = v ? 1 : 0; }
    int total_draws() const { return total_draws_; }
    void set_total_draws(int v) { total_draws_ = v; }
    int printer_baud() const { return printer_baud_; }
    void set_printer_baud(int v) { printer_baud_ = v; }
    /// ⚠️ D18：默认 G9=TX / G10=RX / G11=CTS（设计 §4.3 实物标定值）。
    /// G43/G44 是板上 TX0/RX0（接 USB-UART 桥），禁止占用。
    int printer_tx_pin() const { return printer_tx_pin_; }
    void set_printer_tx_pin(int v) { printer_tx_pin_ = v; }
    int printer_rx_pin() const { return printer_rx_pin_; }
    int printer_flow_pin() const { return printer_flow_pin_; }
    int printer_flow_mode() const { return printer_flow_mode_; }
    void set_printer_flow_pin(int v) { printer_flow_pin_ = v; }
    void set_printer_flow_mode(int v) { printer_flow_mode_ = v; }
    void set_printer_rx_pin(int v) { printer_rx_pin_ = v; }
    int paper_cols() const { return paper_cols_; }
    void set_paper_cols(int v) { paper_cols_ = v; }
    bool qr_enabled() const { return qr_enabled_ != 0; }
    void set_qr_enabled(bool v) { qr_enabled_ = v ? 1 : 0; }
    bool logo_enabled() const { return logo_enabled_ != 0; }
    void set_logo_enabled(bool v) { logo_enabled_ = v ? 1 : 0; }

    // ---- 小票文案（设计 §7.3 占位符模板） ----
    const std::string& shop_name() const { return shop_name_; }
    void set_shop_name(const std::string& v) { shop_name_ = v; }
    const std::string& win_title() const { return win_title_; }
    void set_win_title(const std::string& v) { win_title_ = v; }
    const std::string& lose_title() const { return lose_title_; }
    void set_lose_title(const std::string& v) { lose_title_ = v; }
    const std::string& wifi_list() const { return wifi_list_; }
    void set_wifi_list(const std::string& v) { wifi_list_ = v; }
    const std::string& wifi_ssid() const { return wifi_ssid_; }
    void set_wifi_ssid(const std::string& v) { wifi_ssid_ = v; }
    const std::string& wifi_pass() const { return wifi_pass_; }
    void set_wifi_pass(const std::string& v) { wifi_pass_ = v; }
    // 联网进阶（学习 artnet）
    const std::string& ap_ssid() const { return ap_ssid_; }
    void set_ap_ssid(const std::string& v) { ap_ssid_ = v; }
    const std::string& ap_pass() const { return ap_pass_; }
    void set_ap_pass(const std::string& v) { ap_pass_ = v; }
    int ip_mode() const { return ip_mode_; }                 // 0=DHCP / 1=静态
    void set_ip_mode(int v) { ip_mode_ = v; }
    int net_reset_count() const { return net_reset_count_; }
    void set_net_reset_count(int v) { net_reset_count_ = v; }
    int64_t net_reset_epoch() const { return net_reset_epoch_; }
    void set_net_reset_epoch(int64_t v) { net_reset_epoch_ = v; }
    const std::string& static_ip() const { return static_ip_; }
    void set_static_ip(const std::string& v) { static_ip_ = v; }
    const std::string& static_mask() const { return static_mask_; }
    void set_static_mask(const std::string& v) { static_mask_ = v; }
    const std::string& static_gw() const { return static_gw_; }
    void set_static_gw(const std::string& v) { static_gw_ = v; }
    const std::string& receipt_tmpl() const { return receipt_tmpl_; }
    void set_receipt_tmpl(const std::string& v) { receipt_tmpl_ = v; }
    const std::string& tmpl_canvas() const { return tmpl_canvas_; }
    void set_tmpl_canvas(const std::string& v) { tmpl_canvas_ = v; }
    const std::string& tmpl_banner() const { return tmpl_banner_; }
    void set_tmpl_banner(const std::string& v) { tmpl_banner_ = v; }
    const std::string& page_content() const { return page_content_; }
    void set_page_content(const std::string& v) { page_content_ = v; }
    int active_tmpl_mode() const { return active_tmpl_mode_; }
    void set_active_tmpl_mode(int v) { active_tmpl_mode_ = v; }
    /// 按模式取模板槽（0=flow 1=canvas 2=banner）——设计 §7.3d
    const std::string& tmpl_slot(int mode) const {
        if (mode == 1) return tmpl_canvas_;
        if (mode == 2) return tmpl_banner_;
        return receipt_tmpl_;
    }
    void set_tmpl_slot(int mode, const std::string& v) {
        if (mode == 1) tmpl_canvas_ = v;
        else if (mode == 2) tmpl_banner_ = v;
        else receipt_tmpl_ = v;
    }
    const std::string& receipt_header() const { return receipt_header_; }
    void set_receipt_header(const std::string& v) { receipt_header_ = v; }
    const std::string& receipt_footer() const { return receipt_footer_; }
    void set_receipt_footer(const std::string& v) { receipt_footer_ = v; }
    int validity_days() const { return validity_days_; }
    void set_validity_days(int v) { validity_days_ = v; }
    const std::string& ticket_prefix() const { return ticket_prefix_; }
    void set_ticket_prefix(const std::string& v) { ticket_prefix_ = v; }

    // ---- 抽奖运行时 ----
    int auto_return_ms() const { return auto_return_ms_; }
    void set_auto_return_ms(int v) { auto_return_ms_ = v; }
    int serial_daily() const { return serial_daily_; }
    void set_serial_daily(int v) { serial_daily_ = v; }
    const std::string& serial_date() const { return serial_date_; }
    void set_serial_date(const std::string& v) { serial_date_ = v; }
    int64_t last_known_epoch() const { return last_known_epoch_; }
    void set_last_known_epoch(int64_t v) { last_known_epoch_ = v; }
    const std::string& timezone() const { return timezone_str_; }
    void set_timezone(const std::string& v) { timezone_str_ = v; }

    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    /// 恢复**网络相关**默认配置（硬件恢复按钮 / 串口 wifi resetnet 用）。
    ///
    /// ⚠ 只允许动这几项：ap_ssid / ap_pass / ip_mode / st_ip / st_mask / st_gw /
    ///   wifi_ssid / wifi_pass / wifi_list。**绝不许碰用户数据**（奖池、三种小票模板、
    ///   标定偏移、累计开奖数、票号、店铺名…）—— 有单测用哨兵值锁死这条不变量。
    void ResetNetworkConfig();

    /// 字段表全部 key（防复发单测用：断言每个 ≤ NVS_KEY_LEN_MAX-1）
    static std::vector<std::string> DebugAllKeys();

private:
    Settings() = default;
    friend struct SettingsFieldAccess;  // 允许字段表通过成员指针读写

    // ---- 整型字段 ----
    int current_page_ = 0;
    int total_pages_ = 30;        // R6.1 默认 30 页
    int step_speed_ = 60;
    int paging_channel_count_ = 7;
    int dmx_start_addr_ = 1;
    int stroke_min_dmx_ = 0;      // D21 行程跨度
    int stroke_max_dmx_ = 255;
    int min_page_gap_dmx_ = 8;
    int depletion_policy_ = 0;    // 0=kFallbackToLose, 1=kRefuse
    int printer_pagemode_ = 0;        // 页面模式（背景叠加）开关 0/1（NVS 整数表统一用 int）
    int total_draws_ = 0;         // 累计开奖次数（持久化；界面"累计"用它）
    int printer_baud_ = 115200;
    int printer_tx_pin_ = 9;    // ESP32 TX（接打印机 RX）；禁止 43/44
    int printer_rx_pin_ = 10;   // ESP32 RX（接打印机 TX）
    int printer_flow_pin_ = 11;  // 打印机 CTS→G11
    int printer_flow_mode_ = 0;  // 0关/1CTS/2RTS(打印方向)/3双向
    int paper_cols_ = 32;         // 58mm=32, 80mm=48
    int qr_enabled_ = 1;
    int logo_enabled_ = 0;        // 默认关（未上传时不出票头）
    int auto_return_ms_ = 1500;
    int serial_daily_ = 0;
    int validity_days_ = 7;
    int64_t last_known_epoch_ = 0;  // 时间基准回退（Task 2.4）
    // ---- 联网进阶（学习 artnet：AP 名可改 / 静态 IP 可配）----
    int ip_mode_ = 0;             // 0=DHCP / 1=静态 IP
    // 网络恢复默认的持久化证据（排查"没人按却被清"这类事故：看次数与时间）
    int net_reset_count_ = 0;     // 复位次数
    int64_t net_reset_epoch_ = 0; // 上次复位时间（epoch 秒）

    // ---- 字符串字段 ----
    std::string prize_pool_;            // JSON（设计 §7.2）
    std::string prize_pool_backup_;     // 三级兜底第二级（Task 2.1c）
    std::string wifi_list_;   // 多网络 JSON（含密码；借鉴 page-player SsidManager）
    std::string wifi_ssid_;   // 当前/最近使用的 SSID（空 = 仅 AP）
    std::string wifi_pass_;   // 明文存 NVS；接口/日志绝不回显
    std::string ap_ssid_;     // 自定义热点名（空 = 用默认 lucky-page-XXXX）
    std::string ap_pass_;     // 自定义热点密码（空 = 用默认 12345678；接口绝不回显）
    std::string static_ip_;   // 静态 IP（ip_mode_==1 时生效）
    std::string static_mask_; // 子网掩码
    std::string static_gw_;   // 网关
    std::string ticket_prefix_;
    std::string serial_date_;
    std::string shop_name_ = "幸运翻页机";
    std::string win_title_ = "恭喜中奖";
    std::string lose_title_ = "感谢参与";
    std::string receipt_header_ = "{shop}{nl}=================={nl}";   // ⚠️ 勿用 ═/─（GBK 无此码位，会打成 ?）
    std::string receipt_tmpl_;        // flow（纯文字）模板 JSON（沿用旧键 receipt_tmpl）
    std::string tmpl_canvas_;         // canvas（画布）模板 JSON —— 设计 §7.3d 按模式分槽
    std::string tmpl_banner_;
    std::string page_content_;   // 每页打印内容 JSON（page_content.cc 编解码）         // banner（花边）模板 JSON
    int active_tmpl_mode_ = 0;        // 当前生效模式 0=flow 1=canvas 2=banner
    std::string receipt_footer_ =
        "------------------------{nl}请凭此票到服务台领奖{nl}有效期至：{validity}{nl}";
    std::string timezone_str_ = "CST-8";
};

#endif  // _SETTINGS_H_
