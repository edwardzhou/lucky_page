#ifndef _WIFI_CONFIG_H_
#define _WIFI_CONFIG_H_

#include <cstdint>
#include <string>
#include <vector>

/// WiFi 工作模式（设计 §8.1b）
enum class WifiMode {
    kApOnly,   ///< 仅 AP（未配置 SSID）
    kApSta,    ///< AP + STA 同时（已配置 SSID）——**绝不因配 STA 而丢掉 AP**
};

/// STA 连接状态机（借鉴 page-player：用显式状态+操作白名单替代散落的布尔标志）
/// 背景：布尔标志组合（manual_disconnect_/was_connected_/reconnect_count_）会导致
/// "点扫描无效果""连不上不重试""扫描堆叠"等 bug，故显式建模。
enum class WifiStaState {
    kIdle = 0,          ///< 空闲（初始 / 手动断开后 / 无匹配 AP）
    kScanning,          ///< 扫描中
    kConnecting,        ///< 已调用 connect，等待结果
    kConnected,         ///< 已连接并拿到 IP
    kDisconnected,      ///< 连接后断开，等待退避重连
};

const char* WifiStaStateName(WifiStaState s);

/// 操作白名单（来自 page-player 的状态×操作表；有单测锁定）
bool WifiStaCanScan(WifiStaState s);      ///< kScanning/kConnecting/kConnected 时禁止再扫
bool WifiStaCanConnect(WifiStaState s);   ///< kScanning/kConnecting/kConnected 时禁止再连

/// 状态快照：定长 POD，跨线程读（与设计 D23 seqlock 同规）
struct WifiStatusSnapshot {
    bool configured = false;      ///< 是否已保存 SSID
    bool sta_connected = false;   ///< STA 是否已连上并拿到 IP
    bool mode_is_apsta = false;
    uint8_t sta_state = 0;        ///< WifiStaState（POD 里用整型，避免跨线程枚举细节）
    bool time_synced = false;     ///< SNTP 是否已校时
    int8_t rssi = 0;              ///< 已连接时的信号强度 dBm（信号格由它派生，不另存）
    uint8_t ap_clients = 0;       ///< 连到本机 AP 的客户端数
    uint8_t last_disconnect_reason = 0;
    bool ap_up = false;           ///< 本机热点是否在广播
    uint8_t ap_channel = 0;       ///< 热点当前信道（APSTA 下跟随 STA）
    char ap_ssid[33] = {};        ///< 本机热点 SSID（UI 必须显示，否则用户"看不到 AP"）
    char ssid[33] = {};           ///< 已配置（STA 要连）的 SSID
    char ip[16] = {};             ///< STA 拿到的 IPv4 点分十进制
    char netmask[16] = {};        ///< STA 子网掩码（状态行展示用，学习 artnet）
};

/// 扫描结果条目
struct WifiScanEntry {
    std::string ssid;
    int rssi = 0;
    bool secure = false;   ///< 需要密码
};

/// 校验凭据。返回空串 = 通过；否则中文原因（Web 400 直接回传）
std::string ValidateWifiCredentials(const std::string& ssid, const std::string& password);

/// 解析 IPv4 点分十进制（严格：4 段、每段 0..255、无空段/非数字/超长段）。
/// 返回 false 表示格式非法（*out 不被修改）。
bool ParseIpv4(const std::string& s, uint32_t* out);

/// 校验自定义热点名（学习 artnet 的"AP 名可改"）。
/// 规则：1..31 字节（SSID 上限 32，留 1 字节）、无控制字符、无首尾空格。
/// 返回空串 = 通过；否则中文原因。
std::string ValidateApSsid(const std::string& ssid);

/// 校验**本机热点密码**（用户可改）。
/// 规则：8..63 字节（WPA2 要求）、无控制字符。空串不做校验（语义 = 复原默认密码）。
/// 返回空串 = 通过；否则中文原因。
std::string ValidateApPassword(const std::string& pass);

/// 校验静态 IP 配置（学习 artnet 的静态 IP/DHCP）。
/// 规则：三者均为合法 IPv4；掩码必须是**连续的 1**；**网关必须与 IP 同网段**
/// （配错就失联 —— 现场最常见事故，必须在接口层拦掉）。
/// 返回空串 = 通过；否则中文原因。
std::string ValidateStaticIp(const std::string& ip, const std::string& mask,
                             const std::string& gateway);

/// 模式决策：无 SSID → AP-only；有 SSID → APSTA
WifiMode WifiDecideMode(const std::string& ssid);

/// 模式名（"AP" / "APSTA"）
const char* WifiModeName(WifiMode m);

/// 第 attempt 次（从 1 起）重连的退避毫秒数：1s 起指数翻倍，上限 60s
int WifiBackoffMs(int attempt);

/// RSSI → 信号格 0..4（0 表示未连接/不可用）
uint8_t WifiRssiToBars(int rssi);

/// 单条扫描结果 → JSON（**必须转义** SSID 中的引号/反斜杠/控制字符）
std::string WifiScanEntryToJson(const WifiScanEntry& e);

/// 扫描结果列表 → JSON 数组（按信号强→弱排序由调用方保证）
std::string WifiScanListToJson(const std::vector<WifiScanEntry>& list);

/// ---- Web 静态请求路径判定（纯函数，可测）----
enum class StaticRouteAction {
    kServeIndex,     ///< / → index.html
    kServeFile,      ///< 已存在的文件
    kCaptiveRedirect ///< 未知路径 → 302 配置页（手机 CNA 弹窗关键）
};

/// 清洗请求路径（去掉 ?query 与 #frag）
std::string WebPathClean(const std::string& uri);

/// 路径是否安全（拒绝含 .. 的穿越尝试；拒绝非 / 开头）
bool WebPathIsSafe(const std::string& clean_path);

/// 判定动作。file_exists 由调用方注入（便于纯函数测试）
StaticRouteAction WebStaticDecide(const std::string& uri, bool file_exists);

/// ---- 多网络列表（借鉴 page-player SsidManager：可存多个，最近用的排最前）----
struct WifiSavedNetwork {
    std::string ssid;
    std::string password;
};

/// 上限（与 page-player 一致取 10；超出时丢弃最旧的）
constexpr int kWifiMaxSaved = 10;

/// 保存网络：已存在则覆盖密码并提到最前；满则丢最旧。返回是否成功。
bool WifiSavedAdd(std::vector<WifiSavedNetwork>* list, const std::string& ssid,
                  const std::string& password);

/// 提到最前（设为优先）
bool WifiSavedPromote(std::vector<WifiSavedNetwork>* list, const std::string& ssid);

/// 按索引删除
bool WifiSavedRemove(std::vector<WifiSavedNetwork>* list, int index);

/// 列表 → JSON（**含密码**，仅供设备内部存 NVS 使用，绝不能回给接口）
std::string WifiSavedListToJsonInternal(const std::vector<WifiSavedNetwork>& list);

/// 列表 → JSON（**不含密码**，供 UI 展示）
std::string WifiSavedListToJson(const std::vector<WifiSavedNetwork>& list);

/// JSON → 列表（含密码；解析失败返回空并写 *error）
std::vector<WifiSavedNetwork> WifiSavedListFromJson(const std::string& json, std::string* error);

/// 状态 → JSON。**绝不包含密码字段**（安全要求，有测试锁定）
std::string WifiStatusToJson(const WifiStatusSnapshot& s);

/// Settings 打印摘要（供日志用）。**不得包含密码明文**（有测试锁定）
std::string SettingsSummaryForLog();

#endif  // _WIFI_CONFIG_H_
