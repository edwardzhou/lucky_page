# 计划：标定页改造为「页码调整模式」

> 设计：`specs/2026-10-10-calibrate-adjust-mode.md`　|　参考：`~/esp32_work/page-player`

## Task 1 先写会失败的测试（红）
- [x] Step 1.1 `test_apps/main/test_page_adjust.cc`：钳位/越界/Resize 保留/ResetAll/SetOffset 往返
- [x] Step 1.2 `scripts/api_verify.py` 新增 ⑧ 行程调整节（GET 结构 + PUT 往返还原 + 4 个负例）
- [x] Step 1.3 `scripts/mobile_layout_check.py` 新增调整模式断言（mock `/api/adjust`）
- [x] Step 1.4 跑 1.1/1.3 确认**失败**（接口与界面尚未实现），记录红基线

## Task 2 后端接口
- [x] Step 2.1 `web_server.h`：声明 `AdjustGetHandler` / `AdjustPutHandler` / `AdjustResetHandler`
- [x] Step 2.2 `web_server.cc`：实现三个 handler（MainSync + 显式校验 + 明确错误信息）
- [x] Step 2.3 注册路由 `GET/PUT /api/adjust`、`POST /api/adjust/reset`
- [x] Step 2.4 编译通过 + 单测全 PASS

## Task 3 前端调整模式
- [x] Step 3.1 `calibrate.html`：状态卡（当前页/基础值/偏移/实际值）
- [x] Step 3.2 微调按钮组（±10/±5/±1）→ `PUT /api/adjust`，用响应回填状态卡
- [x] Step 3.3 翻页：上页/下页/首页/回零 + 「跳到第 N 页」输入 + 点动（本地游标）
- [x] Step 3.4 清零本页 / 清零全部
- [x] Step 3.5 「高级：基础映射」折叠区：表格初始值改读真实 `/api/pages` values
- [x] Step 3.6 手机布局（44px 触控、与共享 app.css 协同）

## Task 4 回归与真机
- [x] Step 4.1 离线校验全绿（含调整模式断言 + 截图）
- [x] Step 4.2 `web_smoke.py` 新增标定页真机用例（+5 → 校验 → -5 → **逐页复原**）
- [x] Step 4.3 `bash scripts/build.sh s3 flash` 整包烧录
- [x] Step 4.4 真机：`api_verify.py` + `web_smoke.py` 全通过，偏移复原核对

## Task 5 文档
- [x] Step 5.1 评审记录 `reviews/2026-10-10-calibrate-adjust-mode.md`
- [x] Step 5.2 `AGENTS.md`：标定=调整模式（基础映射+偏移分离）、点动不写 current_page、接口清单更新
