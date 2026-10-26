# UI 重设计 v2 集成设计（Lucky Studio）

日期：2026-10-26 · 输入：`ui-design/`（用户交付的新前端包，含 `spiffs_contents/`）
· 状态：实施中

## 目标

把 `ui-design/spiffs_contents/` 的六页新 UI 装入设备 SPIFFS，保持 6 个页面地址与
现有 `/api/*` 不变；不改机械/打印/网络行为。旧页面整体备份可回滚。

## 新 UI 特性（相对旧版）

- 侧栏导航 + 顶栏 + 工作台卡片式布局（`.lp-*` 设计系统，app.css 39KB）
- 新脚本：`workspace.js`（工作台）、`prizes.js`（奖池）、`demo.js`（仅 ?demo=1
  启用模拟）、`page-enhancements.js`（6 页增强）
- 全部资源本地化（无 CDN/外链），总 224KB，SPIFFS 容量充足（现用 128KB / 3.44MB）

## 兼容性核对（UI 期望 vs 固件实际）

| 接口 | UI 期望 | 固件现状 | 结论 |
|---|---|---|---|
| `/api/lottery/status` | `current_page`、`last.prize_note` | 缺 | **补字段**（快照加 2 字段） |
| `/api/prizes` | version/total_pages/policy/tiers(含 remaining/probability/depleted) | 一致 | ✅ |
| `/api/pages` GET/PUT | total_pages/values；PUT {total_pages} | 一致 | ✅ |
| `/api/adjust` GET/PUT | offsets[][], mapping, base/offset/value；PUT 含 cell | 一致 | ✅ |
| `/api/page-mapping` PUT | {values} | 一致 | ✅ |
| `/api/receipt/template` GET/PUT | v3 模板（mode/blocks/paper_cols…） | 一致 | ✅ |
| `/api/receipt/params` | params[] + default_template | 一致（已有） | ✅ |
| `/api/logo` GET/POST/DELETE | exists/width/height；POST 原始 LGO1 | 一致 | ✅ |
| `/api/wifi*` | 状态/saved/ap/ip/disconnect/reconnect/scan | 一致 | ✅ |
| `/api/draw`、`/api/page/goto`、`/api/printer/test`、`/api/time` | 一致 | ✅ |

## 固件改动（仅增量，不改既有语义）

1. `LotteryStatusSnapshot` 增加 `uint8_t current_page` 与 `char prize_note[32]`
   （结构体上限 128 → 160；仍是定长 POD，seqlock 语义不变）。
2. `UpdateSnapshot()` 填 `current_page = Settings::current_page()`、
   `prize_note = 当前奖项 tier.note`（`last_` 增加 prize_note 字段）。
3. `LotteryStatusHandler` 输出上述两字段。

## 验证门

- 编译 + 片上单测（287 基线全过）
- `mobile_layout_check.py`：按新设计更新 `WANT` 宽度表（旧 760px 已不适用）
- `web_smoke.py`：按新 DOM 适配（旧选择器会失效）
- 真机：6 页加载无 JS 异常、关键 API 200、手机视口不溢出
- `api_verify.py` 全过（后端未变，回归确认）
