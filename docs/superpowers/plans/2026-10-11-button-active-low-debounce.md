# 计划：GPIO8 低电平触发 + 去抖 + 1 秒限流

> 设计：`specs/2026-10-11-button-active-low-debounce.md`

## Task 1 纯逻辑 + 单测（红→绿）
- [x] Step 1.1 `main/button_gate.{h,cc}`（去抖 + 限流 + 统计）
- [x] Step 1.2 `test_apps/main/test_button_gate.cc`；两处 CMakeLists 登记（main + test_apps 的 MAIN_SOURCES）
- [x] Step 1.3 reconfigure + 跑片上单测

## Task 2 板级参数与接线
- [x] Step 2.1 `config.h`：`BUTTON_MODULE_ACTIVE_LEVEL=0`、`DEBOUNCE=30`、`COOLDOWN=1000` + static_assert
- [x] Step 2.2 `application.{h,cc}`：改用常量；抽奖与自检页各一 `ButtonGate`；拒绝原因打日志

## Task 3 串口自检
- [x] Step 3.1 `btbtn`：引脚/触发电平/当前电平/阈值/闸门统计/上次触发时间；help 更新

## Task 4 真机与回归
- [x] Step 4.1 编译 + 整包烧录；串口 `btbtn` 确认电平 = 1（上拉未按下）
- [x] Step 4.2 片上单测全绿 + `api_verify`（不出纸）+ `web_smoke`；设备数据不变
- [x] Step 4.3 提交推送

## Task 5 文档
- [x] Step 5.1 评审 `reviews/2026-10-11-button-active-low-debounce.md`
- [x] Step 5.2 `AGENTS.md`：§1.5 按钮更正为 active_low + 去抖/限流参数；串口命令速查加 `btbtn`
