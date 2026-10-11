# 计划：6 个页面视觉统一（PC + 手机）

> 设计：`specs/2026-10-10-ui-consistency.md`

## Task 1 先立断言（红）
- [x] Step 1.1 `mobile_layout_check.py` 新增「PC 风格一致性」段（1280×800）：逐页读计算样式，
  断言 body 宽度/字号、卡片内距/圆角、按钮内距/字号/圆角、输入框内距/字号/圆角、h3 字号，
  并跨页比对"值完全一致"
- [x] Step 1.2 跑一遍确认**失败**（当前 6 页各不相同），记录红基线

## Task 2 共享基础层
- [x] Step 2.1 `app.css` 顶部加**设计令牌**（色板 + 尺度：--w-page/--r-card/--r-ctl/--pad-*/--fs-*）
- [x] Step 2.2 `app.css` 加**基础层**（全宽度生效）：body/.card/h3/button/input/select/textarea/
  table/.note/.warn/.ok/.row/label/code
- [x] Step 2.3 保留既有手机层（≤640px）与 nav 段；顺序：令牌 → 基础层 → nav → actbar → 手机层

## Task 3 各页清理（只留特有部分）
- [x] Step 3.1 `index.html`：删重复基础规则，留 #state/#draw/#last/.stats/.fault
- [x] Step 3.2 `prizes.html`：删重复，留 .prob/.dep/数字居中
- [x] Step 3.3 `logo.html`：删重复，留 canvas/文件/滑块
- [x] Step 3.4 `wifi.html`：删重复，留 .st/.dot/.kv/.ap/.bars/.lock（含其手机块）
- [x] Step 3.5 `calibrate.html`：删重复，留 .status/.grid3/.grid4/#tbl/details（含其手机块）
- [x] Step 3.6 `receipt.html`：`body` 加 `wide`（宽版例外），删重复，留画布/抽屉/工具条

## Task 4 验证
- [x] Step 4.1 离线校验 PC + 手机断言全绿（含截图）
- [x] Step 4.2 编译 + 整包烧录（页面在 SPIFFS）
- [x] Step 4.3 真机 `api_verify.py` + `web_smoke.py` 全通过；设备数据复核无污染
- [x] Step 4.4 PC 6 页 + 手机 6 页截图人眼确认

## Task 5 文档
- [x] Step 5.1 评审记录 `reviews/2026-10-10-ui-consistency.md`（含前后对比截图）
- [x] Step 5.2 `AGENTS.md`：新增"页面样式只走 app.css 基础层 + 页面特有部分"约定
