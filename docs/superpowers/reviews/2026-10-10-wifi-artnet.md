# 评审：WiFi 页学习 artnet-controller（A 一键连接 / B 异步扫描 / C 改热点名 / D 静态 IP / E 显式断开）

> 设计 `specs/2026-10-10-wifi-artnet.md`　|　计划 `plans/2026-10-10-wifi-artnet.md`
> 参考：`~/esp32_work/artnet-controller`（`wifi.html` 936 行 + `main/wifi/wifi_station.{h,cc}`）

## 现象 / 差距（对照参考实现）

| 项 | artnet | lucky-page 改造前 |
|---|---|---|
| 扫描结果 | 每行**内联密码框 + 「连接」**，一步连上 | 点结果只**填 SSID**，还要手输密码再点保存（两步）|
| 扫描方式 | **异步 + 轮询** | `POST /api/wifi/scan` **阻塞 3s**（手机上像卡死）|
| 热点名 | **可改**（改/复原） | `lucky-page-XXXX` 由 MAC 生成，**不可改** |
| IP | **静态 IP / DHCP 可配** + 模式标签 | 只有 DHCP |
| 断开 | **显式「断开连接」**（保留凭据） | 只有「忘记全部」（清凭据）|

## 修法

### 后端

1. **纯函数**（`wifi_config.{h,cc}`，7 个新单测）：`ParseIpv4`（严格 4 段/无前导零/无空段）、
   `ValidateApSsid`（1..31 字节、无控制字符、无首尾空格）、
   `ValidateStaticIp`（格式 + **掩码必须连续 1** + **网关必须同网段** + 网关≠本机 IP）
2. **Settings** 新字段（走 D22 字段表）：`ap_ssid` / `ip_mode` / `st_ip` / `st_mask` / `st_gw`
3. **wifi_manager**：
   - `StartAsyncScan` + `PollScan` + `WIFI_EVENT_SCAN_DONE` 抓结果（**沿用 `WifiStaCanScan` 白名单**：
     已连接时仍拒绝扫描，理由不变 —— 这条既有保护不能因异步化丢失）
   - `DisconnectSta` / `ReconnectSta` + `s_user_disconnected`：**退避任务在用户主动断开后不再自动重连**
     （否则 1 秒后被连回来，"断开"就是假的）；`ApplySta` 清该标志
   - `SetApSsid`（空串 = **复原默认名**，立即 `esp_wifi_set_config(AP)` 生效，信道仍保持 0=自适应）
   - `SetStaticIp` / `UseDhcp` → `ApplyStoredIpConfig()`（`dhcpc_stop/start` + `set_ip_info`），启动时也应用
4. **HTTP 接口**（路由 33 → **38** ≤ `kMaxUriHandlers(40)`，编译期 static_assert 保护）：
   `PUT /api/wifi/ap`、`PUT /api/wifi/ip`、`POST /api/wifi/disconnect`、`POST /api/wifi/reconnect`、
   `GET /api/wifi/scan`（轮询）；`POST /api/wifi/scan` 改为**启动异步扫描**；
   `GET /api/wifi` 增加 `ip_mode/static_*/ap_ssid_custom/user_disconnected`
5. **串口兜底**：`wifi disc` / `wifi on` / `wifi ascan` / `wifi ap <名>` / `wifi ip <ip> <mask> <gw>` /
   `wifi dhcp`；`httptest` 支持 `[GET|POST|PUT|DELETE] <path>`

### 前端（`wifi.html`）

状态卡加「修改热点名」「修改 IP 设置」（inline 编辑 + 保存/复原/默认 DHCP）+ IP 模式标签；
已连接时显示「断开连接」、断开后显示「重新连接」；扫描改异步 + 1s 轮询；
**扫描结果每行内联密码框 + 「连接」**；保留手工输入兜底与已保存网络（优先/删除/忘记全部）。

## 过程中抓到的三个问题（都是真机验证抓出来的）

### ① 静态 IP 字节序反了（**最严重**，真机会失联）
- **现象**：串口设静态 IP 后状态行显示 `IP=252.0.168.192`（应为 192.168.0.252）。
- **根因**：`ParseIpv4` 返回的是"大端数值"（192.168.0.252 → 0xC0A800FC），而
  `esp_netif_ip_info_t` 用的是 lwIP 内部序；我直接 `info.ip.addr = ip` → 字节反序。
- **修法**：改用 `IP4_ADDR(&info.ip, o1,o2,o3,o4)`（内部自动 htonl），掩码/网关同理。
- **验证**：修复后串口显示 `IP=192.168.0.252`，且**设静态后 HTTP 仍可达**、`GET /api/wifi` 报 `ip_mode=1`。
- **教训**：这正是"必须真机验 + 热点永不关"的价值 —— 若没有 `192.168.4.1` 兜底，
  反序的静态 IP 会直接把设备踢出网络。

### ② `httptest` 支持方法时漏发请求体
- **现象**：设备自测 `httptest POST /api/wifi/reconnect` 返回 `HTTP 408 Server closed this connection`。
- **根因**：`esp_http_client_open(cli, len)` 只是**声明** Content-Length，body 必须再 `esp_http_client_write`；
  漏了这一步，httpd 一直等 body → 超时关闭连接。
- **修法**：open 后补 `esp_http_client_write`。**这不是接口的问题，是自测工具的问题**
  （真机排障工具自身缺陷会伪装成产品缺陷 —— §四-4）。
- **验证**：修复后 `POST /api/wifi/reconnect → HTTP 200 {"status":"connecting"}`，
  `PUT /api/wifi/ap` 空体 → 400，证明 body 真的发出去了。

### ③ 断开接口会忽略坏 JSON 照做（自测用例暴露的真实缺陷）
- **现象/风险**：`POST /api/wifi/disconnect` 原本不解析请求体 → 带坏 JSON 也会**真的断开**，
  与全站"坏 JSON → 400"不一致，且调用方会误以为参数错会被拒。
- **修法**：先读体，非空则必须能解析成 JSON，否则 400；空体（正常调用）放行。
- **验证**：`api_verify` 新增用例「带坏 JSON → 400」，且**设备保持在线**（证明没真断）。

### 另外两处设计取舍

- **断开后界面必须能自己恢复**：WiFi 密码不回显（§强制规则 9），前端无法自行重连 →
  补 `POST /api/wifi/reconnect`（用已保存凭据）。否则"断开"在界面上是**单向门**，只能靠串口救。
- **风险操作不通过 HTTP 自测**：断开会让设备离开本网段、静态 IP 配错会改地址 ——
  这两类的"真做一次"验证走**串口通道**（`wifi disc` / `wifi ip` / `httptest POST ...`），
  HTTP 只测校验负例与可逆项（改热点名只影响 AP 接口，安全）。

## 验证证据

| 层级 | 命令 / 动作 | 结果 |
|---|---|---|
| 编译 | `bash scripts/build.sh s3` | `Project build complete`（路由 38/40，static_assert 通过）|
| 片上单测 | `test_apps/run_tests.py` | **254 PASSED / 0 FAILED**（新增 7 项：`ParseIpv4` 11 例、`ValidateApSsid` 2 例、`ValidateStaticIp` 4 例）|
| HTTP 验收 | `api_verify.py 192.168.0.252` | **74 / 0**（新增 ⑤c：进阶字段、热点名改→读回→复原、4 个改名负例、5 个静态 IP 负例、2 个坏 JSON 拦截）|
| 离线界面校验 | `mobile_layout_check.py` | **101 / 0**（新增 ②c：控件齐全、异步扫描轮询出列表、内联密码框、无脚本异常、PC 样式一致性仍全过）|
| 真机前端冒烟 | `web_smoke.py 192.168.0.252` | **55 / 0**（新增 ③b：控件齐全、显示热点名、IP 模式标签、**已连接时扫描仍被拒并给原因**、无脚本异常）|
| 真机·异步扫描 | 串口 `wifi ascan`（断开态） | `异步扫描完成，找到 11 个网络` |
| 真机·静态 IP | 串口 `wifi ip 192.168.0.252 255.255.255.0 192.168.0.1` | 状态显示 `IP=192.168.0.252`；**HTTP 仍可达**、`ip_mode=1`；`wifi dhcp` 复原 |
| 真机·断开/重连 | 串口 `wifi disc` → `httptest POST /api/wifi/reconnect` | 断开后主机不可达（预期）→ 设备自测 **HTTP 200** → 主机恢复可达、`user_disconnected=false` |
| 设备数据 | 归还核对 | 热点名回默认、DHCP、已保存网络两项保留、标定偏移全 0、模板 15/18/9、奖池 30 页 4 级；6 页 HTTP 200 |

## 布局返工：按用户偏好改成 artnet 的布局

**用户反馈**："我更倾向于 artnet-controller 的 WiFi 页面布局" —— 功能对了但布局不是他要的。
把 artnet 原版渲染出来做对照（`/tmp/lucky-mobile/artnet-wifi-ref.png`），提炼出它的布局特征并逐条搬过来：

| artnet 布局特征 | 本页实现 |
|---|---|
| 页标题（大号）| `<h1>网络设置</h1>` |
| 卡片标题**带下边框分隔** | `.card-title{border-bottom:1px solid #ffffff12}` |
| **状态行**：标签左（次要色）/ **加粗值右** + 紧贴值的「修改」小按钮 | `.status-row/.status-label/.status-value/.small-btn`（8 行：AP 名称/STA 状态/STA IP+模式标签/子网掩码/信号/已连网络/时间同步/AP 客户端）|
| **通栏主操作按钮**（大号、全宽、主色/危险色）| `.action-btn.primary/danger`：「断开 WiFi 连接」「重新连接」「扫描 WiFi」「清除所有已保存 WiFi」，按状态显隐且文案随状态变化（未连上时显示「停止连接 XXX」）|
| **空状态提示**居中灰字 | `#aplist` 初始为 `点击上方按钮扫描附近 WiFi` |
| 扫描/网络行：名称 + 🔒 + 信号 dBm，下一行密码 + 「连接」 | `.ap/.ap-main/.wifi-name/.wifi-rssi/.ap-act` |
| 内联编辑区（在卡片内展开） | `.ip-edit-area` 承载「改热点名」与「IP/掩码/网关」两组内联表单 |
| ~~**窄列**（原版 400px）~~ | **已取消**：先按用户偏好做了 `body.narrow` = 480px，随后用户要求"WiFi 页面宽度改与其他一致" → 回到 **760px**，与其余 5 页统一（`body.narrow` 规则已删除，唯一宽度例外只剩小票编辑页）|

**用户的两次取舍（后一次覆盖前一次）**：① 起初选"WiFi 页用窄列"→ 随后改为"**宽度与其他页一致**"（760px）；
② **保留本项目的顶部菜单与配色**，只搬布局结构（不采用 artnet 的 bottom tab 栏与其配色）。

> 当前状态：**布局结构是 artnet 的，宽度（760px）与其余 5 页一致**。

**为支撑状态行还补了一个后端字段**：`GET /api/wifi` 增加 `netmask`（artnet 状态行有「子网掩码」，
本页原来没暴露）——快照加字段 + `FillStaIp` 同时填掩码 + 纯函数 JSON 加键。

### 返工中两处"测试而非产品"的失败（都已修正断言，没有放宽）
1. `异步扫描轮询出结果`：连接按钮在新布局里不再有 `.conn` 类（改按 `.ap-act button` 文本匹配）。
2. `artnet 布局：扫描区空状态提示`：断言写在扫描**之后**（那时列表已渲染）→ 改为在扫描**前**
   断言空状态，扫描后另断言"已渲染出网络行"。
3. 真机冒烟 `WiFi 页显示本机热点名`：读的是旧 id `#apinfo`，新布局为 `#apName` → 同步更新，
   并**加了 4 条真机布局不变量**（窄列 480 / 状态行左右对齐+值加粗 / 标题下边框 / 主按钮通栏）。

## 结论

WiFi 页已具备 artnet 的进阶能力（一步连接 / 非阻塞扫描 / 改热点名 / 静态 IP / 显式断开重连），
并且每一项都在真机上验过；风险操作全部走串口兜底通道，验证完设备已完整复原。
期间修掉 1 个会导致真机失联的字节序缺陷 + 1 个自测工具缺陷 + 1 个输入校验缺陷。
