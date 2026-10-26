# 评审：小票编辑器新增「页面文字 / 页面图片」元素

日期：2026-10-26 · 范围：仅前端 `main/assets/spiffs_contents/receipt.html`
（未改 `app.css`、其它页面、`demo.js` 与固件）。对应设计
`docs/superpowers/specs/2026-10-26-per-page-content-design.md`，固件侧字段见
`main/printer/receipt_template.cc` / `receipt_render.cc` / `web_server.cc`。

## 现象 / 需求

固件模板 v3 已支持 `page_text`、`page_image` 两块（内容来自「页面内容」当前页），
但旧编辑器只认 line/separator/qr/logo/feed：

- 元素面板没有入口，服务端已有这两种块的模板载入后 `fromServer()` 会把字段丢掉，
  画布上也不显示 → 保存一次就丢块（违反 AGENTS §四-2「新增块字段必须同时改三处」）。
- 编辑器没有「当前页」概念，`/api/receipt/template/preview` 不带 `?page=`，预览永远按第 0 页。
- 用户看不到「这一页会打什么字 / 哪张图」，只能盲配。

## 根因

1. 编辑器内部块模型是手写的字段归一化（`fromServer`/`payload`），新增类型必须逐项补；
   漏一处就是"载入后现场丢失 + 保存回写丢字段"。
2. 排版 `layout()` 与固件 `BuildCanvas` 是**两套实现、同一规则**：page_text 的
   `size=2 → 半幅折行 + 每行占 2 行`、page_image 的 `ceil(h/24)` 必须照抄，否则
   画布位置与纸面不符（真机历史问题：大字被裁、二维码压住后续行）。
3. 预览页游标与页面数据（文字/素材槽号/素材尺寸）此前从未进入编辑器。

## 修法（全部在 receipt.html 内）

| 位置 | 改动 |
|---|---|
| 元素面板 | 新增「＋ 页面文字」「＋ 页面图片」两个按钮（与既有 `.sm` 按钮同款）；`add()` 默认值按要求：page_text `{col:0,row:-1,align:'center',size:1,bold:false}`、page_image `{col:0,row:-1,align:'center',bg:false}`。 |
| `fromServer()` | page_text 置 `text:''` 并取 `size`（align/bold 由公共字段取）；page_image 只取公共的 align/bg/col/row。固件字段名一致（`content`/`char` 这类别名不适用于页面块）。 |
| `layout()` | page_text：`box = size==2 ? floor(avail/2) : avail`，按**当前页文字**折行，`span = 行数 × (size==2?2:1)`；page_image：`span = max(1, ceil(当前页图片高/24))`，无图/尺寸未知按 1 行占位（编辑器可选中；打印由固件跳过）。 |
| `paint()` | page_text 以等比字形渲染「页面文字：<当页文字 / （未设置）>」（与文字行同款按列宽 pad，因此 align 可见）；空文字加 `.nosrc` 灰字 + title「打印时整块跳过」。page_image 虚线框 + `<img>` 缩略图（`/api/assets/raw?id=N` 取 LGO1，浏览器解码为 PNG dataURL）；无图/失效/读取失败分别给出文案；`bg` 继续用 `.el.bg`，与 Logo 背景层同一套半透明 + 斜纹视觉。 |
| `paintProps()` | page_text：对齐 / 字号（1|2 倍）/ 加粗 + 当页文字与来源提示；page_image：对齐 + 「作为背景层」开关 + 素材名/尺寸/占行 + 「仅画布模式打印」提示（flow 且勾 bg 时额外提示无法保存）。两者仍显示既有 col/row/自动输入。修正原先"只有 Logo 能做背景"的通用提示（页面图片同样允许）。 |
| `payload()` | page_text 输出 `align/size/bold`（**不带 text**），page_image 输出 `align`（bg 走通用分支，**不带 text**）；col/row 沿用原规则。 |
| `render()` | 预览请求改为 `/api/receipt/template/preview?page=<当前页>`；界面 1-based → 接口 0-based（固件 `ReceiptParams.page` 与 `page_content` 均为 0-based，`{page}` 输出 +1）。 |
| 顶部控件 | 画布卡片顶部新增一行：「预览页」数字输入（默认 1，max=total_pages，越界自动夹取）+ 当页状态（`P1 · 文字已设置 · 图片：xxx`）+「页面内容」链接（`./pages.html`，由 app.js 统一保留 `?demo=1`）。手机端单独保留「预览页」标签（其余 `.tool-label` 仍按既有约定隐藏）。 |
| 数据加载 | 新增 `loadPageData()`：进入编辑器时**一次性**并行读 `GET /api/pages/content`（items: page/text/asset）与 `GET /api/assets`（slots: id/name/width/height）；总页数在页面内容接口不可用时回退 `GET /api/pages`。`setPreviewPage()` / `applyPreviewPageBounds()` / `updatePageSrc()` 负责游标与状态。失败只把 `pagesOk/assetsOk` 置 false → 画布与面板全部按「未设置」显示。 |
| 缩略图缓存 | `lgoDecode()`（LGO1 → PNG dataURL，尺寸校验 8 的倍数、≤576×672）+ `pageImg()`：每槽只请求一次、8s 超时、失败缓存为不可用；素材清单已加载而槽号不存在（页面引用了被删素材）时**直接判失效、不发请求**，避免在设备日志与冒烟脚本里制造 404。 |
| CSS | 新增样式全部写在页面内联 `<style>` 并限定 `.lp-page-receipt` 作用域：`.el.page_image`（flex 居中 + 非 bg 虚线框）、`.pi-thumb`（像素化缩略图）、`.nosrc`（灰字）、`.el.page_image.sel:not(.bg)`（选中态，否则会被高特异性 `:not(.bg)` 规则盖掉）。**刻意不覆盖 `.el.bg`**，保证与 Logo 背景层视觉一致。 |

### 关键取舍

1. **空页文字仍占 1 行（仅编辑器）**：固件遇到空文字整块跳过（占 0 行），但编辑器若给 0 行，
   元素就不可见、无法选中/拖动。与既有"无 Logo 时 logo 块占 1 行占位"同一处理，已在
   注释中写明"打印时整块跳过"。
2. **不在块里缓存页面内容**：`page_text.text` 恒为空、`page_image` 不存图，内容只来自
   「页面内容」；这样同一模板在不同页打印不同内容，保存也不会把当页文字固化进模板。
3. **降级不阻断编辑**：`?demo=1` 下 `/api/pages/content`、`/api/assets` 返回 501，编辑器
   只显示"页面内容不可用 / 未设置"，仍可正常加元素、拖动、保存与预览（预览固定按第 0 页，
   与 demo.js 既有行为一致，未改 demo.js）。
4. **预览页只读一次**：按需求"进入编辑器时加载一次"；在「页面内容」页改完需重进本页生效，
   面板提示已写明。

## 验证证据（未连接真机、未出纸）

| 项 | 方式 | 结果 |
|---|---|---|
| JS 语法 | 抽出内联脚本 `node --check` | 通过 |
| 接口/渲染/交互 | 本地 Python mock（`/api/receipt/*`、`/api/pages/content`、`/api/assets[/raw]`、`/api/logo`、`/api/time`）+ Playwright 桌面 1440×900 | **58 通过 / 0 失败**（见下） |
| 手机/桌面布局回归 | `python3 scripts/mobile_layout_check.py` | **80 通过 / 0 失败**（7 页，含 receipt 移动端无溢出、输入 ≥16px、跨页一致） |
| `?demo=1` 降级 | 同上本地服务 + Playwright | 0 pageerror、无错误条、状态显示"页面内容不可用"、新元素可添加并显示（未设置）、模板照常渲染 |
| 缩略图 | mock 返回 384×480 LGO1（边框+对角线） | 画布出现 `<img class="pi-thumb">`，naturalWidth=384；截图人眼确认虚线框 + 图 |

58 项断言覆盖：默认值与 payload 字段集；fromServer 归一化；page_text span（短文字 1 行、
2 倍字号长文字 4×2=8 行）；page_image span=ceil(480/24)=20、无图 1 行；当前页切换后
文字/图片随页变化；`?page=1`（0-based）确实发出；page_text/page_image payload 均无 text；
page_text/page_image 拖动 28px=2 列、44/22px=2/1 行（桌面 colw=14/lineh=22 未变）；
既有 line 拖动与 6 种块 payload 与原行为逐字段一致；bg 视觉与 Logo 背景层逐项一致；
失效素材不请求 raw；手机视口无横向溢出、预览页输入 16px、"预览页"标签可见。

临时验证脚本：`/tmp/receipt-check/smoke.py`（mock 服务 + Playwright，非仓库文件）；
截图：`/tmp/receipt-check/desktop-page-image-sel.png`、`desktop-page-text.png`、
`mobile-receipt.png`。

## 已知限制 / 待办

1. **真机未验**：`web_smoke.py` / `api_verify.py` 未跑（无设备）；固件预览 `?page=` 与
   `page_content` 数据源已在代码层对齐，真机仍应打印两页（不同文字/图片）人眼确认。
2. 空页文字在编辑器占 1 行（见取舍 1），因此画布上后续自动块的位置比实际打印**低 1 行**；
   这是可选中的必要代价，与既有"无 Logo 时 logo 占位"一致。
3. 页面内容只在进入编辑器时读一次；在「页面内容」页保存后回到本页需刷新（提示已写明）。
   若后续要做"切回标签页自动刷新"，可加 `visibilitychange` 重读，但会与"编辑中不打扰"
   的既有约定冲突，本次未做。
4. `page_image` 仍只在 canvas 模式打印（flow/banner 固件跳过）；编辑器在属性面板给出提示，
   但不阻止保存（与固件校验一致：只拒绝 flow + bg）。
