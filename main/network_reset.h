#ifndef _NETWORK_RESET_H_
#define _NETWORK_RESET_H_

/// 网络配置恢复默认（**仅网络**：热点名/密码 + 已保存 STA 网络与凭据 + IP 模式回 DHCP）。
///
/// 调用方：
/// - 硬件恢复按钮（GPIO5 长按 5 秒，见 application.cc）
/// - 串口命令 `wifi resetnet`（调试/验证 —— 与按钮**同一条代码路径**）
///
/// ⚠ 绝不触碰用户数据（奖池、三种小票模板、标定偏移、计数…）；该不变量有单测锁死。
/// 在 esp_timer 回调里**不可直接调用**（内部会写 NVS + 操作 WiFi），
/// 必须先 Schedule 到主线程。
bool ResetNetworkConfigToDefault();

#endif  // _NETWORK_RESET_H_
