# 计划：WiFi 页学习 artnet（A~E 全做）

> 设计：`specs/2026-10-10-wifi-artnet.md`

## Task 1 纯函数 + 单测（红→绿）
- [x] Step 1.1 `wifi_config.{h,cc}` 加 `ParseIpv4` / `ValidateApSsid` / `ValidateStaticIp`
- [x] Step 1.2 `test_apps/main/test_wifi_config.cc` 加用例（合法/非法：掩码不连续、网关跨网段、SSID 超长/控制符）
- [x] Step 1.3 reconfigure + 跑片上单测（应为 247 + 新增数 全 PASS）

## Task 2 Settings 字段
- [x] Step 2.1 `ap_ssid` / `ip_mode` / `st_ip` / `st_mask` / `st_gw`（成员 + getter/setter + 字段表各一行）
- [x] Step 2.2 编译通过（字段表防复发单测覆盖 key 长度）

## Task 3 wifi_manager 能力
- [x] Step 3.1 异步扫描：`StartAsyncScan` + `PollScan` + `WIFI_EVENT_SCAN_DONE` 抓结果（保留状态白名单）
- [x] Step 3.2 `s_user_disconnected` 标志：`DisconnectSta` / `ReconnectSta`，`RetryTask` 与断线事件尊重该标志；`ApplySta` 清标志
- [x] Step 3.3 `SetApSsid`（写 Settings + AP 配置生效）
- [x] Step 3.4 `ApplyStaticIp` / `UseDhcp`（写 Settings + `dhcpc_stop/start` + `set_ip_info`）；启动时按 `ip_mode` 应用

## Task 4 HTTP 接口
- [x] Step 4.1 `PUT /api/wifi/ap`、`PUT /api/wifi/ip`、`POST /api/wifi/disconnect`
- [x] Step 4.2 `POST /api/wifi/scan` 改启动异步；新增 `GET /api/wifi/scan` 轮询
- [x] Step 4.3 GET `/api/wifi` 增加 `ip_mode` / `static_ip` / `static_mask` / `static_gw` / `user_disconnected` 字段供 UI 显示
- [x] Step 4.4 路由数核对（33→36 ≤ 40，静态断言保护）+ 编译通过

## Task 5 控制台兜底
- [x] Step 5.1 `wifi on`（重连）/ `wifi disc`（断开保留凭据）；`help` 更新

## Task 6 前端
- [x] Step 6.1 状态卡：AP 名可改（修改/保存/复原）
- [x] Step 6.2 状态卡：IP 模式标签 + 静态 IP 编辑（保存/取消/默认 DHCP）
- [x] Step 6.3 「断开连接」按钮（已连接时显示）
- [x] Step 6.4 扫描：异步启动 + 1s 轮询 + 进度提示
- [x] Step 6.5 扫描列表：内联密码框 + 「连接」一键连（A）
- [x] Step 6.6 保留手工输入兜底与已保存网络管理

## Task 7 测试脚本
- [x] Step 7.1 `api_verify.py`：AP 改名往返、静态 IP 负例、坏 JSON 负例、扫描异步轮询、保留"已连接禁止扫描"
- [x] Step 7.2 `mobile_layout_check.py`：WiFi 页新控件与 mock（异步扫描 GET/POST、ap/ip 接口）
- [x] Step 7.3 `web_smoke.py`：真机扫描轮询 + 内联连接按钮 + AP 名可读

## Task 8 真机验证（风险操作按设计走串口兜底）
- [x] Step 8.1 编译 + 整包烧录
- [x] Step 8.2 静态 IP 设成当前 IP → 校验模式切换与可达 → 复原 DHCP
- [x] Step 8.3 `POST /api/wifi/disconnect` → 串口 `wifi on` 恢复 → 核对重新拿到 IP
- [x] Step 8.4 AP 改名 → 读回 → 复原
- [x] Step 8.5 全量回归（api_verify / web_smoke / 离线校验）+ 设备数据核对

## Task 9 文档
- [x] Step 9.1 评审记录 `reviews/2026-10-10-wifi-artnet.md`
- [x] Step 9.2 `AGENTS.md`：WiFi 能力（AP 改名/静态IP/断开/异步扫描）、串口 `wifi on|disc` 救援通道
