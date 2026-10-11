# 设计：GPIO 恢复默认配置按钮（长按 5 秒）

> 日期：2026-10-10　|　状态：已评审（用户确认：**按钮换到其它空闲脚**、**恢复范围仅网络配置**）

## 1. 需求

用户要一个硬件按钮：默认**上拉高电平**、**按下为低电平**、**长按 5 秒**触发"恢复默认配置"，
其中 **AP 密码恢复为 12345678**。

## 2. 关键冲突（已与用户确认结论）

**GPIO4 目前是 DMX512 的 DE（发送使能）输出脚**（`DMX512_DE_PIN`，`main/boards/esp32s3_lucky/config.h`）。
把它改成按钮输入会与 DMX 冲突 → 用户选择**按钮换到其它空闲脚**（G4 保持 DE 不变）。

**选定：`RESET_BUTTON_GPIO = GPIO_NUM_5`**
- 非 strapping 脚（避开 G0/G3/G45/G46）、不接板上外设、无 USB/Flash/PSRAM 复用
  （N16R8 的 Flash/PSRAM 占用 26..37）
- 已占用清单核对：G3(IR) G4(DMX DE) G8(抽奖键) G9/G10/G11(打印机) G17/G18(DMX) G19/G20(USB) G43/G44(UART 桥)
- 用**编译期断言**锁死：不得与上述任一功能脚相同（沿用项目 D18 的静态断言风格）

## 3. 恢复范围 = **仅网络配置**（用户选择；绝不碰用户数据）

| 复位 | 保持不动 |
|---|---|
| `ap_ssid` → 默认 `lucky-page-XXXX` | 奖池 `prize_pool` / 库存 |
| `ap_pass` → 默认 **12345678** | 三种小票模板 `receipt_tmpl` / `tmpl_canvas` / `tmpl_banner` |
| `ip_mode` → 0(DHCP)、`st_ip/st_mask/st_gw` → 空 | 标定偏移 `page_adjust.json` / 基础映射 |
| `wifi_ssid` / `wifi_pass` / `wifi_list` → 空（忘记所有已保存网络）| 累计开奖数、票号序列、店铺名、打印机参数 |

**可测不变量**：复位只允许改上面这几项 —— 用**单测**给数据字段塞哨兵值，
复位后断言"哨兵值原封不动"，防止以后有人往里加一行把用户模板清掉（§四-19 的数据安全）。

## 4. 实现要点

1. `Settings::ResetNetworkConfig()`：只改网络字段 + `SaveSync()`（**纯 Settings 逻辑，可在单测宿主验证**）。
2. `main/network_reset.{h,cc}`：`bool ResetNetworkConfigToDefault()` =
   写日志 → `Settings::ResetNetworkConfig()` → 应用到 WiFi：
   `SetApSsid("")`（默认名）· `SetApPassword("")`（→12345678）· `UseDhcp()` · `ForgetSta()`（断开并清空凭据）。
   → **按钮与串口命令调用同一个函数**，因此"按钮会执行的那段代码"能在真机上用串口验证。
3. `application.cc`：`reset_button_ = Button(RESET_BUTTON_GPIO, /*active_level=*/0, /*long_press_ms=*/5000)`；
   `OnLongPress` 回调在 **esp_timer 任务上下文**，禁止阻塞 → 用既有 `Schedule()` 投递到主线程再执行。
4. 串口：`wifi resetnet`（走同一条路径）+ `rstbtn`（打印引脚号/有效电平/长按阈值/**当前电平**，
   用于验证上拉与接线：未按下应为 1）。
5. 前端：WiFi 页加一行说明（现场可发现性）：设备上长按恢复按钮 5 秒 = 复位网络配置。

**不加 HTTP 接口**：网络可达即可触发的"恢复出厂网络"是危险面（任何人连上热点就能清）；
用户要的是**物理按钮**，串口命令仅作调试。路由预算也保持 38/40 不变。

## 5. 验证

| 层级 | 内容 |
|---|---|
| 单测 | ①`ResetNetworkConfig` 把 9 个网络字段回默认；②**数据哨兵字段全部不变**（奖池/三模板/店铺名/累计数/票号）|
| 编译期 | 引脚静态断言：与 IR/DMX DE/DMX TX,RX/抽奖键/打印机三脚/USB/UART 桥全不相同；长按阈值 = 5000 |
| 真机 | 串口 `rstbtn`：引脚号/阈值正确 + **当前电平 = 1（上拉未按下）**；串口 `wifi resetnet` → `GET /api/wifi` 复核 AP 名/密码/已保存网络/IP 模式全部回默认，且**奖池/模板/标定偏移一字不变**；然后恢复用户原网络配置（重新连接 ZTE_C97FF0）|
| 离线/真机前端 | WiFi 页出现"长按恢复按钮"说明；既有断言全过 |

**诚实边界**：物理按下按钮这一动作我无法远程执行 —— 用"同一函数的串口路径 + 引脚电平读回 +
编译期断言"覆盖，并在报告里明确写出这一点；真机上按一下会在串口打印触发日志。
