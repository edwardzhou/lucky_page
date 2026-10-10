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

}  // namespace wifi_manager

#endif  // _WIFI_MANAGER_H_
