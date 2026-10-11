# 评审：支持用户修改 AP（热点）密码

> 设计 `specs/2026-10-10-ap-password.md`　|　计划 `plans/2026-10-10-ap-password.md`
> 用户需求："WiFi AP 密码，支持用户修改"

## 现象 / 差距

AP 密码硬编码在 `wifi_manager.cc` 的 `kApPass = "12345678"`（启动配置、启动日志、串口状态三处引用），
用户无法修改；页面上"密码 12345678"也是写死的文案。

## 修法

1. **纯函数** `ValidateApPassword`：8..63 字节（WPA2 要求）、无控制字符；空串 = 复原默认（不报错）。
2. **Settings** 新增 `ap_pass`（空 = 用默认）。
3. **manager**：
   - `ApPasswordEffective()`：Settings 有值且合法 → 用它，否则默认；启动配置与 `authmode` 随之设置；
   - `SetApPassword(pass, err)`：空串 → 清 Settings 用默认；否则校验 → 存 → 立即
     `esp_wifi_set_config(WIFI_IF_AP)`（保留 SSID、信道 0、最大连接数；`authmode` 恒 WPA2）；
   - `ApPasswordForConsole()`：**读驱动的当前配置**（见下）供串口打印。
4. **HTTP**（**不加路由**，扩展 `PUT /api/wifi/ap`）：请求体支持 `ap_ssid` 和/或 `ap_pass`，
   至少一个；各自校验，任一失败 400。空串语义：`ap_ssid:""` = 复原热点名、`ap_pass:""` = **复原默认密码**。
   `GET /api/wifi` 增加 `ap_pass_custom`（布尔）；**密码绝不回显**。
5. **前端**：状态行「AP 密码」显示"默认 12345678 / 已自定义（不显示）"+「修改」内联编辑
   （新密码输入 + 保存/复原默认/取消）；页脚提示与救援办法（串口 `wifi`）随状态自适应。
6. **串口**：`wifi` 打印**真实生效密码**；新增 `wifi appass <密码>`（空 = 复原默认）。

## 过程中抓到的两个问题

### ① 复合 handler 的"短路写法"导致只传密码必然失败（真机测试抓到）
- **现象**：`PUT /api/wifi/ap {"ap_pass":"..."}` 返回 400；只传 `ap_ssid` 时正常。
- **根因**：handler 写成
  `if (has_ssid) ctx->ok = SetApSsid(...); if (ctx->ok && has_pass) ctx->ok = SetApPassword(...);`
  —— 只传 `ap_pass` 时 `has_ssid=false` → `ctx->ok` 保持**初值 false** → 密码那步被跳过，
  最后按"失败"返回。
- **修法**：改为"全部成功才置 ok"：
  `if (has_ssid && !SetApSsid(...)) return; if (has_pass && !SetApPassword(...)) return; ctx->ok = true;`
- **教训**：多字段可选参数的 handler 不能用"前一步的 ok"做后一步的门禁；`ok` 的语义应是
  "所有被请求的操作都成功"。（已进 AGENTS 铁律 §四-24）

### ② "Settings 里有值" ≠ "驱动真的生效了"
- **问题**：串口原本打印的是 Settings 里的值，因此"改完是否真的应用上"其实没法从串口判断
  （`esp_wifi_set_config` 静默失败时看不出来）。
- **修法**：`ApPasswordForConsole()` 改为**优先读驱动当前配置**（`esp_wifi_get_config(WIFI_IF_AP)`），
  读不到才回退 Settings。
- **验证价值**：改密码后串口立刻显示新值（`密码=shop-pass-2026（自定义）`）、复原后显示默认
  —— 这才真正证明了"驱动接受了新配置"。

## 密码安全设计（强制规则 9 的落地）

| 面 | 处理 |
|---|---|
| HTTP 响应 | 只回 `ap_pass_custom` 布尔；**有断言验证响应全文不含明文**（api_verify + 离线 + 真机冒烟三处）|
| 页面显示 | 未自定义 → 「默认 12345678」；已自定义 → 「已自定义（不显示）」；**有断言验证页面文本不含明文** |
| 启动日志 | 不再打印明文（只打"默认/自定义"），避免日志被采集时泄露 |
| 串口 | `wifi` **打印真实密码** —— 串口需物理接触，是"忘记密码"时的救援通道（页面文案里也明确告知）|
| 空串语义 | 空串 = **复原默认**（不是设成开放网络）；`authmode` 恒为 WPA2，杜绝误配成开放热点 |

## 验证证据

| 层级 | 命令 / 动作 | 结果 |
|---|---|---|
| 编译 | `bash scripts/build.sh s3` | `Project build complete`（路由数不变 38/40）|
| 片上单测 | `test_apps/run_tests.py` | **255 PASSED / 0 FAILED**（新增 `ApPassword.ValidationRules`：8/63 位合法、7 位拒、64 位拒、控制字符拒、中文按字节计）|
| HTTP 验收 | `api_verify.py 192.168.0.252` | **82 / 0**（新增：改密码 200、`ap_pass_custom=true`、**响应全文不含明文**、无 `ap_pass` 字段、复原默认、3 个负例）|
| 离线校验 | `mobile_layout_check.py` | **113 / 0**（新增：密码行/修改按钮、默认显示 12345678、保存后显示"已自定义"、**页面不含明文**）|
| 真机冒烟 | `web_smoke.py 192.168.0.252` | **63 / 0**（新增：`/api/wifi` 只回 `ap_pass_custom`、布尔类型、页面与设备状态一致、已自定义不显示明文）|
| 真机·驱动侧 | 串口 `wifi`（读驱动配置） | 改后 `密码=shop-pass-2026（自定义）`；复原后 `密码=12345678（默认）` |
| 真机·串口命令 | `wifi appass console-pass-99` / `wifi appass` | 两个方向都生效，并回显"当前生效密码" |
| 设备数据 | 归还核对 | AP 名默认、**密码回默认**、DHCP、已保存网络两项、标定偏移全 0、模板 15/18/9、奖池 4 级 |

## 结论

AP 密码现在可由用户在页面上修改（也可串口改），立即生效、重启后仍生效、可一键复原默认；
密码在任何 HTTP 响应、页面文本、启动日志中都不出现，只保留"物理接触的串口"作为救援通道。
