# 设计：WiFi 页学习 artnet-controller（A 一键连接 / B 异步扫描 / C 改热点名 / D 静态 IP / E 显式断开）

> 日期：2026-10-10　|　状态：已评审（用户已确认：**A/B/C/D/E 全部要做**）
> 参考：`~/esp32_work/artnet-controller`（`main/assets/spiffs_contents/wifi.html` 936 行 + `main/wifi/wifi_station.{h,cc}`）

## 1. 参考实现的可学点（对照 lucky-page 现状）

| # | artnet 做法 | lucky-page 现状 | 差距 |
|---|---|---|---|
| A | 扫描结果每行**内联密码框 + 「连接」按钮**，一步连上 | 点结果只**填 SSID**，还要手输密码再点「保存并连接」（两步） | 交互多两步 |
| B | 扫描**异步 + 轮询**（`scanWifi()` → `pollScanResult()`） | `POST /api/wifi/scan` **阻塞 3s** | 手机上像卡死，且无进度 |
| C | 热点名**可改**（修改/保存/复原） | `lucky-page-XXXX` 由 MAC 生成，**不可改** | 多台同场难区分 |
| D | **静态 IP / DHCP 可配** + 模式标签 | 只有 DHCP | 现场无 DHCP 时无法固定 IP |
| E | **显式「断开连接」**（保留凭据） | 只有「忘记全部」（清凭据） | 无法"断开但记住" |

## 2. 后端设计

### 2.1 新增纯函数（`wifi_config.{h,cc}`，可上单测宿主）
- `bool ParseIpv4(const std::string& s, uint32_t* out)` —— 严格点分十进制（4 段、0..255、无前导零歧义）
- `std::string ValidateApSsid(const std::string& ssid)` —— 1..31 字节、无控制字符、无首尾空格
- `std::string ValidateStaticIp(ip, mask, gw)` —— 三者格式合法、**掩码必须连续 1**、网关与 IP 同网段
  （掩码/网关错误是现场最常见的"配了就失联"原因，必须在接口层拦掉）

### 2.2 Settings 新增字段（走 D22 字段表，Load/Save 自动覆盖）

| key（≤15） | 类型 | 默认 | 含义 |
|---|---|---|---|
| `ap_ssid` | string | "" | 自定义热点名；空 = 用默认 `lucky-page-XXXX` |
| `ip_mode` | int | 0 | 0=DHCP / 1=静态 |
| `st_ip` | string | "" | 静态 IP |
| `st_mask` | string | "" | 子网掩码 |
| `st_gw` | string | "" | 网关 |

### 2.3 `wifi_manager` 新增能力（驱动层薄，逻辑尽量留在纯函数）

| 函数 | 说明 |
|---|---|
| `bool StartAsyncScan(std::string* error)` | 非阻塞 `esp_wifi_scan_start(cfg, false)`；沿用 `WifiStaCanScan` 白名单（已连接扫描仍被拒绝，理由不变）|
| `bool PollScan(std::vector<WifiScanEntry>* out, bool* running, std::string* error)` | 在 `WIFI_EVENT_SCAN_DONE` 里抓取并缓存结果；供 GET 轮询 |
| `bool DisconnectSta(std::string* error)` | **置 `s_user_disconnected`** + `esp_wifi_disconnect()` + 状态 `kIdle`；`RetryTask` 在该标志下**不再自动重连**（否则"断开"立刻被退避任务连回来） |
| `bool ReconnectSta(std::string* error)` | 清该标志并用已保存凭据重连（"连接"按钮与串口 `wifi on` 复用） |
| `bool SetApSsid(const std::string&, std::string* error)` | 校验 → 写 Settings → `esp_wifi_set_config(WIFI_IF_AP)` 生效 |
| `bool ApplyStaticIp(...)` / `bool UseDhcp(...)` | 写 Settings + 立即应用（`esp_netif_dhcpc_stop` + `esp_netif_set_ip_info`；DHCP 则 `dhcpc_start`）|

- `ApplySta()`（连新网络）**清 `s_user_disconnected`**，语义闭环。
- 启动时若 `ip_mode==1`，在 STA netif 起来后应用静态配置。

### 2.4 新增/变更 HTTP 接口（路由 33 → 36，`kMaxUriHandlers=40` 且编译期断言保护）

| 方法 | 路径 | 请求 | 说明 |
|---|---|---|---|
| PUT | `/api/wifi/ap` | `{ap_ssid}` | 改热点名（校验失败 400） |
| PUT | `/api/wifi/ip` | `{dhcp:true}` 或 `{ip,netmask,gateway}` | 切 DHCP / 静态（校验失败 400） |
| POST | `/api/wifi/disconnect` | – | 断开 STA（**保留凭据**） |
| POST | `/api/wifi/scan` | – | **改为启动异步扫描**，立即返回 `{status:"started"}`；状态不允许仍 400 |
| GET | `/api/wifi/scan` | – | 轮询：`{running:true}` 或 `{done:true, aps:[...]}` 或 `{error}` |

### 2.5 串口兜底（**必须有**：断开后 HTTP 不可达）

- `wifi on` → 重连（清 user-disconnected，用已保存凭据）
- `wifi disc` → 断开（保留凭据）
- 语义提醒：`wifi off` 保持既有"忘记全部"不变（不做语义破坏）

## 3. 前端设计（`wifi.html`，沿用统一设计系统）

- **当前状态**：AP 名称行 + 「修改」→ 内联输入 + 保存/复原；STA IP 行 + **模式标签（DHCP/静态）** +
  「修改」→ IP/掩码/网关 + 保存/取消/「默认(DHCP)」；已连接时显示「断开连接」
- **扫描**：点「扫描 WiFi」→ 立即返回并显示"扫描中…" → 每 1s `GET /api/wifi/scan` 轮询 →
  出列表；**每行 = SSID + 🔒 + 信号格 + 密码框（加密时）+ 「连接」**（一步连接）
- 保留手工输入 SSID/密码 + 「保存并连接」兜底；保留已保存网络（优先/删除）与「忘记全部」
- 提示统一走 `#msg`（沿用现有 ok/warn 样式）

## 4. 测试与验证（含"不能把自己测失联"的纪律）

| 层级 | 内容 |
|---|---|
| 单测 | `ParseIpv4` / `ValidateApSsid` / `ValidateStaticIp` 的合法与非法用例（含掩码不连续、网关跨网段、SSID 超长/含控制符） |
| HTTP 验收 | AP 改名**改→读回→复原**（只影响 AP 接口，主机走 STA，安全）；静态 IP 校验负例（坏 IP/坏掩码/跨网段 → 400）；`/api/wifi/ap` 与 `/api/wifi/ip` 坏 JSON → 400；断开接口**只测坏 JSON → 400**；扫描改异步后：POST 启动 → GET 轮询到 done 并含列表；"已连接时扫描被拒绝"用例保留 |
| 真机（风险操作，走串口兜底） | ① 静态 IP：设成**当前 IP** 验证模式切换与可达 → 复原 DHCP；② 断开：`POST /api/wifi/disconnect` 后用**串口 `wifi on`** 恢复并核对重新拿到 IP（HTTP 靠不住，串口是救援通道）；③ AP 改名：改→读回→复原 |
| 离线/真机前端 | 离线断言（内联密码框+连接按钮、AP/IP 修改入口、断开按钮、轮询函数存在、无横向溢出）；真机 `web_smoke` 断言（扫描轮询出结果、内联连接按钮存在、AP 名可读） |
| 设备数据 | 结束时核对：AP 名复原、IP 模式回 DHCP、已保存网络与奖池、模板未变 |

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| 改静态 IP 把自己测失联 | 只设成**当前 IP** 做验证；AP 始终在广播（`192.168.4.1` 兜底）；结束时恢复 DHCP |
| 断开后无法用 HTTP 恢复 | 串口 `wifi on` 作为救援通道（先实现它，再做断开验证） |
| 异步扫描把"已连接时禁止扫描"的既有保护弄丢 | 启动仍走 `WifiStaCanScan` 白名单；api_verify 保留该用例（含"扫描后 Wi-Fi 仍可达"）|
| 热点改名把已连接客户端踢掉 | 改名只影响 AP 接口；UI 明确提示"已连接的客户端会断开需重连" |
| 静态 IP 配错导致失联（现场） | 接口层严格校验（掩码连续、网关同网段）；UI 明示"配错不会丢热点，仍可连 lucky-page-XXXX 进来改" |
