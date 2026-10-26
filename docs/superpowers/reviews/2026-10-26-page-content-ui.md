# 评审：页面内容页（pages.html / pages.js）与导航第 7 项

日期：2026-10-26 · 范围：前端（SPIFFS 内页面）；对应设计
`docs/superpowers/specs/2026-10-26-per-page-content-design.md`

## 现象 / 需求

- 每页打印文字与页面图片此前只能靠固件配置，现场无法逐页编辑。
- 6 个现有页面（index / prizes / receipt / logo / calibrate / wifi）的侧栏与移动端
  底部导航只有 6 项，没有「页面内容」入口。
- 新页在 `?demo=1`（demo.js 尚无 `/api/pages/content`、`/api/assets` 模拟）时必须
  给出可读错误而不是白屏或 JS 异常。

## 根因（为什么之前没有）

- UI v2「Lucky Studio」只实现了 6 个业务页，页面内容接口（`/api/pages/content`、
  `/api/assets*`）当时尚未注册（本任务期间由固件侧补齐，见 `web_server.cc`）。
- 移动端底部导航 `.lp-mobile-nav` 的列数硬编码为 `repeat(4,1fr)`（4 项），
  加第 5 项会掉到第二行；需要把列数改成 5。

## 修法

| 文件 | 改动 |
|---|---|
| `main/assets/spiffs_contents/pages.html` | **新增**。复用现有页面骨架（侧栏、顶栏、页脚、移动端导航逐字一致），两张卡片：页面表（30 行：页码 / 文字 / 图片下拉 / 缩略图 / 保存本页）+ 素材库（选图 → 二值化预览 → 名称 → 上传 → 列表 / 删除）。页面特有样式全部内联并限定在 `.lp-page-pages` 作用域。 |
| `main/assets/spiffs_contents/pages.js` | **新增**。`GET /api/pages`、`GET /api/pages/content`、`PUT /api/pages/content`（逐页）、`GET/POST/DELETE /api/assets`、`GET /api/assets/raw`；浏览器侧 1-bit LGO1 编码（阈值 128 + Floyd–Steinberg，宽度 8 的倍数、≤576×672）；未保存标记 + 单行/全部保存；删除素材后把引用页设为「无图片」并标脏。 |
| `index/prizes/receipt/logo/calibrate/wifi.html` | 侧栏两组 nav 与移动端 nav 各插入 1 个「页面内容」链接（位于「小票设计」之后），共 2 处/页；本次只新增该锚点（脚本核对；`receipt.html` 另外还有并行任务的小票编辑器改动，不属于本页改动）。 |
| `main/assets/spiffs_contents/app.css` | 仅 1 行：`@media(max-width:760px)` 内 `.lp-mobile-nav` 的 `repeat(4,1fr)` → `repeat(5,1fr)`（父任务批准）。 |
| `scripts/mobile_layout_check.py` / `scripts/web_smoke.py` | PAGES 增加 `pages`（手机布局 / 真机冒烟覆盖第 7 页）。 |

### 关键取舍

1. **数据安全优先**：`GET /api/pages/content` 失败时表格**只读**（输入禁用、保存按钮禁用），
   避免"读到空表 → 用户写入 → 覆盖设备里已有内容"。只有 `/api/assets` 失败时文字仍可保存。
2. **降级不阻断**：三个 GET 用 `Promise.allSettled` 并行读取，单接口失败只影响对应卡片；
   `?demo=1` 下两个未模拟接口返回 501，页面显示红色 `.msg` + 顶部 chip「页面读取失败 /
   素材读取失败」，仍渲染 30 行骨架。
3. **缩略图**：`/api/assets/raw` 的 LGO1 在浏览器解码为 canvas，再按 ≤192 点宽编码成
   data URL（48 张素材也不至于卡顿）；同一素材的请求按 Promise 缓存。
4. **只读/脏状态可辨**：卡片头 `.lp-unsaved` + 行 `.dirty` 暖色描边，手机上页码行额外
   显示「未保存」标签。

## 验证证据（均未连接真机）

| 项 | 命令 | 结果 |
|---|---|---|
| JS 语法 | `node --check main/assets/spiffs_contents/pages.js` | 通过 |
| 手机/桌面布局 + 7 页零 JS 异常 | `python3 scripts/mobile_layout_check.py` | **80 通过 / 0 失败**（含 `/pages.html` 移动端无溢出、触控 ≥44/32、输入 ≥16px、桌面侧栏 224px、跨页卡片/按钮/输入框一致）；截图 `/tmp/lucky-mobile-v2/` |
| `?demo=1` 优雅降级 | 本地 `python3 -m http.server` + Playwright 打开 7 页 | 30 行表格、错误 `.msg`、chips「页面读取失败 / 素材读取失败」、0 pageerror |
| 无 demo（静态服务 404） | 同上不带 `?demo=1` | 只读降级 + 可读错误，0 pageerror |
| 真实接口形状（mock `/api/**`） | Playwright 路由 mock | 初始 30 行/2 素材/下拉 3 选项/缩略图 2 张；单行保存 PUT `{page:0,text,asset:0}`；改两行「保存全部」2 次 PUT；43 汉字（129B）被拦；删除素材（确认框）后引用行变 `-1` 且标脏；上传 900×300 → LGO1 `576×192`、13,832B、`Content-Type` 与 query name 正确；保存失败保留未保存状态、重试成功 |
| 图片边界 | 同上 | 2000×1500→576×432；100×2000→32×672；5×5→8×5；名称 10 个汉字 → 截为 8 个汉字（24B） |
| 刷新守卫 | 同上 | 有未保存更改时点「刷新」弹确认；取消保留编辑，确认后丢弃 |
| 导航 | Playwright 点击 | index `?demo=1` → 点侧栏「页面内容」→ `pages.html?demo=1`，侧栏与底部导航均高亮本页，`document.body.dataset.page=pages` |
| 导航一致性 | 脚本比对 | 6 页侧栏/移动端 nav 各新增 1 个锚点（首次核对时除锚点外与 HEAD 逐字节一致；receipt.html 随后有并行编辑器的改动）；pages.html 的侧栏/移动端 nav 与 prizes 逐字节一致 |

## 已知限制

1. **1366×768 及更矮的桌面视口**：侧栏新增第 7 项后内容高度 795px > 768px，底部装饰卡
   （"设备本地工作空间 / Designed for every lucky moment"）被裁约 27px；**导航全部可见**。
   若要彻底解决，需要在 `app.css` 给桌面 `.lp-sidebar` 加 `overflow-y:auto`（本次未获准改）。
2. **demo.js 未实现新接口**：`?demo=1` 下本页按设计只读；等 demo.js 补 `/api/pages/content`
   与 `/api/assets*` 模拟后即可交互（页面无需改动）。
3. **真机未验**：按任务要求未连接设备；固件路由与页面字段（`page/text/asset`、
   `slots[].id/name/width/height/bytes`、LGO1）已与 `web_server.cc` / `asset_store.h` 对齐。
