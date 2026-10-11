#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include <string>
#include <vector>

#include "esp_err.h"
#include "wifi/wifi_config.h"

/// WiFi 管理（AP + 可选 STA；设计 §8.1b）
///
/// 设计原则：
/// - **AP 永不消失**：配了 STA 也保留 AP，密码配错时商家仍能进 192.168.4.1 救回来
/// - **绝不阻断抽奖**：任何 WiFi 错误只记日志，不 abort、不阻塞主循环
/// - STA 掉线按指数退避（1s..60s）自动重连
namespace wifi_manager {

/// 启动 AP（+ 已配置时的 STA）。返回 ESP_OK 表示 AP 已起来（Web 可用）。
esp_err_t Start();

/// 当前是否已启动
bool IsUp();

/// 状态快照（线程安全；定长 POD）
WifiStatusSnapshot Status();

/// 扫描周边 AP（阻塞 2~4s；按信号强→弱排序）。失败返回 false 并写 *error。
bool Scan(std::vector<WifiScanEntry>* out, std::string* error);

/// 保存凭据并立即重连（空 SSID = 关闭 STA 回到 AP-only）。
/// 凭据非法时返回 false 并写 *error，**不落库**。
bool ApplySta(const std::string& ssid, const std::string& password, std::string* error);

/// 忘记网络（清空凭据 + 断开 STA），回到 AP-only
bool ForgetSta();

/// 已保存网络列表（不含密码，供接口/UI 展示；按优先级排序）
std::vector<WifiSavedNetwork> SavedNetworks();

/// 删除第 index 个已保存网络（0 = 最高优先）；列表空则断开 STA
bool RemoveSaved(int index);

// ---- 以下为学习 artnet-controller 的进阶能力（2026-10-10）----

/// 启动**异步**扫描（非阻塞，结果在 WIFI_EVENT_SCAN_DONE 时抓取）。
/// 沿用与同步 Scan 相同的状态白名单（已连接/连接中/扫描中会拒绝并写 *error）。
bool StartAsyncScan(std::string* error);

/// 轮询异步扫描结果：*running=true 表示仍在扫描；完成后 *out 为结果（按信号强→弱）。
/// 返回 false 表示"还没发起过扫描"或调用失败（*error 给原因）。
bool PollScan(std::vector<WifiScanEntry>* out, bool* running, std::string* error);

/// 用户主动断开 STA（**保留已保存凭据**）。置位后后台退避任务不再自动重连，
/// 直到再次连接（ApplySta / ReconnectSta）或重启。
bool DisconnectSta(std::string* error);

/// 用已保存的最高优先网络重新连接（清"用户主动断开"标志）。
bool ReconnectSta(std::string* error);

/// 修改本机热点名（校验 1..31 字节；立即生效，已连接客户端需重连）。
bool SetApSsid(const std::string& ssid, std::string* error);

/// 修改本机热点密码（校验 8..63 字节；**空串 = 复原默认密码**）。
/// 立即生效；已连接热点的客户端需用新密码重连。密码**绝不回显**（HTTP 层只回布尔）。
bool SetApPassword(const std::string& pass, std::string* error);

/// **仅供串口**读取当前生效的 AP 密码（物理接触的救援通道：忘记自定义密码时用它）。
/// HTTP 层绝不可调用（强制规则 9：密码绝不回显）。
std::string ApPasswordForConsole();

/// 设为静态 IP（校验格式/掩码连续/网关同网段；写 Settings 并立即应用）。
bool SetStaticIp(const std::string& ip, const std::string& mask, const std::string& gw,
                 std::string* error);

/// 切回 DHCP（写 Settings 并立即应用）。
bool UseDhcp(std::string* error);

/// 是否处于"用户主动断开"状态（UI 显示与「断开」按钮显隐用）
bool IsUserDisconnected();

}  // namespace wifi_manager

#endif  // _WIFI_MANAGER_H_
