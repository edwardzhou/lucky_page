# 计划：支持用户修改 AP 密码

> 设计：`specs/2026-10-10-ap-password.md`

## Task 1 纯函数 + 单测（红→绿）
- [x] Step 1.1 `ValidateApPassword`（8..63 字节、无控制字符）
- [x] Step 1.2 `test_wifi_config.cc` 加用例；reconfigure + 跑片上单测

## Task 2 Settings + manager
- [x] Step 2.1 `ap_pass` 字段（成员 + getter/setter + 字段表）
- [x] Step 2.2 启动用 `WifiApPassword()`（合法自定义否则默认）+ authmode 随之设置
- [x] Step 2.3 `SetApPassword`（空=复原默认；写 Settings + 立即 set_config）
- [x] Step 2.4 `ApPasswordForConsole()` 供串口打印

## Task 3 HTTP
- [x] Step 3.1 `PUT /api/wifi/ap` 支持 `ap_ssid`/`ap_pass`（至少一个；各校验；空=复原）
- [x] Step 3.2 `GET /api/wifi` 增加 `ap_pass_custom`（**不回密码**）

## Task 4 串口
- [x] Step 4.1 `wifi` 打印真实 AP 密码；新增 `wifi appass <密码>`；help 更新

## Task 5 前端
- [x] Step 5.1 状态行「AP 密码」显示 默认/已自定义（不显示）
- [x] Step 5.2 内联编辑区加密码输入 + 保存/复原默认；页脚提示文案自适应

## Task 6 测试脚本
- [x] Step 6.1 `api_verify.py`：改→读回 `ap_pass_custom`→**响应不含密码**→复原；4 个负例
- [x] Step 6.2 离线 mock + 断言；`web_smoke.py` 真机断言（含不泄露）

## Task 7 真机验证
- [x] Step 7.1 编译 + 整包烧录
- [x] Step 7.2 改密码 → 串口复核真实值 → `httptest GET /api/wifi` 确认不泄露 → 复原默认
- [x] Step 7.3 全量回归 + 设备数据核对（AP 名/密码/DHCP/网络/模板/偏移）

## Task 8 文档
- [x] Step 8.1 评审记录 `reviews/2026-10-10-ap-password.md`
- [x] Step 8.2 `AGENTS.md` §1.7 补 AP 密码能力与"绝不回显 + 串口救援"纪律
