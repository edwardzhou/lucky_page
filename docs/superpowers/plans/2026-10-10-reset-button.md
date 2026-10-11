# 计划：GPIO 恢复默认配置按钮（长按 5 秒）

> 设计：`specs/2026-10-10-reset-button.md`

## Task 1 纯逻辑 + 单测（红→绿）
- [x] Step 1.1 `Settings::ResetNetworkConfig()`（只改 9 个网络字段 + SaveSync）
- [x] Step 1.2 单测：网络字段回默认 + **数据哨兵字段不变**
- [x] Step 1.3 reconfigure + 跑片上单测

## Task 2 引脚与按钮
- [x] Step 2.1 `RESET_BUTTON_GPIO = GPIO_NUM_5` + 编译期断言（不得与任何功能脚冲突）
- [x] Step 2.2 `main/network_reset.{h,cc}`：`ResetNetworkConfigToDefault()`（Settings + WiFi 应用）
- [x] Step 2.3 `application.cc` 注册按钮：active_level=0、长按 5000ms、回调经 `Schedule()` 投主线程

## Task 3 串口
- [x] Step 3.1 `wifi resetnet`（与按钮同一函数）
- [x] Step 3.2 `rstbtn`：打印引脚/有效电平/阈值/当前电平；help 更新

## Task 4 前端
- [x] Step 4.1 WiFi 页加"长按恢复按钮 5 秒"说明；离线断言

## Task 5 真机验证
- [x] Step 5.1 编译 + 整包烧录
- [x] Step 5.2 串口 `rstbtn`：当前电平 = 1（上拉正常）
- [x] Step 5.3 串口 `wifi resetnet` → HTTP 复核：AP 名/密码=12345678/已保存网络清空/IP=DHCP，且**奖池+模板+标定偏移不变**
- [x] Step 5.4 恢复用户原网络配置（重连 ZTE_C97FF0）→ 全量回归（离线/HTTP/冒烟）

## Task 6 文档
- [x] Step 6.1 评审 `reviews/2026-10-10-reset-button.md`（含"物理按压未远程验证"的诚实边界）
- [x] Step 6.2 `AGENTS.md`：§1.5 引脚表加恢复按钮；§1.7 或新节说明复位范围；串口命令速查
