# 评审 37：UI 重设计 v2（Lucky Studio）集成

日期：2026-10-26 · 结果：**通过**（6 页真机零异常，数据已还原）

## 现象（需求）

采用 `ui-design/` 交付的新前端（侧栏 + 顶栏 + 工作台卡片布局），替换设备 6 页 UI，
保留页面地址与设备接口，行为（抽奖/打印/网络）不变。

## 关键决策

1. **只做增量 API 兼容，不改既有语义**：新 UI 需要 `/api/lottery/status` 的
   `current_page` 与 `last.prize_note`（旧 UI 不需要）→ 快照结构加 2 字段
   （定长上限 128→160，seqlock 语义不变），其余接口逐项核对全部兼容，零改动。
2. **新 UI 的 4 个新脚本**（workspace/prizes/demo/page-enhancements）随页面入库；
   `demo.js` 仅在 `?demo=1` 生效，真机默认走真实同源 API（不会回退假数据）。
3. **两处移动端缺陷修复**（app.css 移动层，样式单一来源）：
   - 小票工具栏 `#paper/#mode` 与标定基础映射 `#tbl input` 字号 12px → 16px
     （iOS 聚焦会自动放大页面）；
   - 其余保持新设计（侧栏抽屉、底部导航、紧凑工具栏）。

## 改动清单

| 文件 | 改动 |
|---|---|
| `main/assets/spiffs_contents/*` | 整体替换为 ui-design 版本（6 html + app.css/js + 4 js） |
| `main/lottery/lottery_status.h` | snapshot 加 `current_page`/`prize_note`，上限 160 |
| `main/lottery/lottery_controller.{h,cc}` | `last_.prize_note`；`UpdateSnapshot` 填充 |
| `main/web/web_server.cc` | status 输出 `current_page`、`last.prize_note` |
| `scripts/mobile_layout_check.py` | **重写**：新设计量纲（侧栏 224/抽屉、底部导航、跨页组件一致、触控与字号），例外显式枚举 |
| `scripts/web_smoke.py` | **重写**：新 DOM 交互（奖池编辑→保存→回读→还原、标定 ±5、模板切换与预览、移动视口），保持"快照→整体还原→复核" |
| `main/assets/spiffs_contents/app.css` | 移动端输入框字号修正（2 条规则） |
| `ui-design/` | 设计源包入库 |

## 验证证据

1. 编译 `Project build complete`；片上单测 **287 PASSED / 0 FAILED**（未受影响）。
2. 真机 6 页（Playwright，1440×900）：**零 JS 异常、零 API 4xx/5xx、零横向溢出**。
3. `web_smoke.py`：**34/34 PASS**（含奖池保存回读、标定 ±5 往返、模板切换/预览、
   移动视口、最终奖池/模板/偏移与快照逐项一致）。
4. `mobile_layout_check.py`（本地 demo，390×844 + 1440×900）：**69/69 PASS**，
   截图 `/tmp/lucky-mobile-v2/`。
5. `api_verify.py`：**82/82 PASS**（默认不出纸/不抽奖）。
6. 真机截图：桌面工作台、移动工作台渲染正常（`/tmp/dev_*.png`、`m-*.png`）。

## 过程中的坑

1. **串口一开就复位设备**：pyserial 打开 `/dev/cu.*` 会短暂拉 DTR/RTS →
   板上 EN 复位（日志 `rst:0x15 USB_UART_CHIP_RESET`）。本次冒烟中途遇到 HTTP
   连接被重置，就是"我在跑 HTTP 测试时又开了串口读日志"造成的。
   **纪律：HTTP/浏览器测试期间不要同时开串口；需要串口时先停测试。**
2. **PUT 还原遇到偶发 408**：设备主线程/NVS 写忙时 httpd 回 408。还原脚本
   改成"只发设备接受的字段 + 3 次重试"，之后稳定。
3. **网络等待策略**：新 UI 页面有轮询，`networkidle` 可能永不满足 →
   统一 `domcontentloaded + 固定等待`。
4. **奖池表头也是 `<tr>`**：`#tiers tr` 会多数一行，选择器须
   `#tiers tr:not(.hdr)`（与旧 UI 结构不同）。

## 遗留

- 真实抽奖/出票仍须人工确认（脚本默认跳过，会驱动机械与打印）。
- 新 UI 的 `ui-design/preview.html` 是设计方交付的离线预览，不入固件。
