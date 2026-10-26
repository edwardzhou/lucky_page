# 每页打印内容（页面文字 + 页面图片）设计

日期：2026-10-26 · 状态：实施中 · 前置：评审 37（UI v2）、模板 v3 体系

## 需求（用户确认）

1. **打印仍是模板化**：整票版式由模板决定，不改为"每页一个模板"。
2. 在模板基础上，**每一页可以有自己的文字和图片**（用户可设置）。
3. 图片素材**先上传到素材库**，页面设置时从库里选择。
4. 模板编辑器里可以放置 `页面文字` / `页面图片` 两类元素，并**拖动调整位置**。
5. 图片只在 **canvas** 模式生效（flow/banner 无位图能力，选择降级为不打印并告警）。
6. 30 页的文字/图片在**独立页面**里编辑。

## 数据模型

### 每页内容（NVS，JSON）

```json
{"version":1,"pages":[
  {"page":0,"text":"一等奖 · 扫地机器人","asset":0},
  {"page":3,"text":"谢谢参与","asset":2}
]}
```

- 未列出的页 = 空内容（模板里的页面元素自动跳过）
- `text` ≤ 128 字节（UTF-8）；`asset` = 素材槽号，-1/缺省 = 无图
- 存 Settings 新字符串字段 `page_content`（走既有 NVS 字段表，含哨兵单测保护）

### 素材库（独立 `media` 分区）

- 分区表新增：`media, data, 0x41, 0xD20000, 0x2E0000`（2.875MB）
  —— 与 `logo`（0xD00000, 128KB）**分开**，旧 logo 与上传流程不受影响
- 固定槽存储：每槽 **49,152B（48KiB，按 flash 扇区对齐）**→ 共 48 槽（容量 2.25MB）
  - 槽头：`AST1` + 槽序号 + 长度 + CRC32 + 宽 + 高 + 名称(24B)
  - 写入前擦除本槽 → 写 → 回读校验；失败保留原数据（D15）
  - 简单、无碎片、无需压缩整理；小图会占满整槽（48 槽对 30 页足够，空间换简单）
- 与品牌 Logo 的关系：`logo` 块仍用 LogoStore 的单张品牌图；`page_image` 用素材库

### 模板新增块

| 类型 | 字段 | 行为 |
|---|---|---|
| `page_text` | text 为空（内容来自页面属性）、align/size/bold/col/row | 打印当页文字；空则整块跳过 |
| `page_image` | col/row（位置）、bg（背景层，同 logo 语义） | 打印当页图片；**仅 canvas**；flow/banner 跳过并记录告警 |

- 渲染签名扩展：`Render(t, p, logo, page_content)`，`page_content` 由 PrinterService
  按 `params.page` 从存储加载（含图片数据，PSRAM 缓冲）
- 预览：`RenderPreviewText` 用当页文字替代 `{page_text}` 展示

## API

| 接口 | 说明 |
|---|---|
| `GET /api/assets` | `{slots:[{id,name,w,h,bytes,used}], count}` |
| `POST /api/assets?name=xxx` | 原始 LGO1 上传（复用现有校验），返回 `{id}` |
| `DELETE /api/assets?id=N` | 删除该槽 |
| `GET /api/assets/raw?id=N` | 预览图（image/bmp 或原始 LGO1 渲染）|
| `GET /api/pages/content` | `{total_pages, items:[{page,text,asset}]}` |
| `PUT /api/pages/content` | `{page, text, asset}` 单页保存（立即落盘）|

## UI

- 新页面 `pages.html`（侧栏「页面内容」）：
  - 页表：页码 / 文字输入 / 图片下拉（素材库缩略图 + 名称）/ 预览
  - 素材库卡片：上传（文件选择 → 转为 LGO1，沿用 logo 页的转换逻辑）、列表、删除
- 小票编辑器：元素面板新增「页面文字」「页面图片」；画布上可拖动定位（复用现有拖动）
- 预览：编辑器预览按当前选中页（页表游标）渲染

## 验证

- 单测：页面内容 JSON 往返/越界/空值；素材槽号计算与校验；新块类型的模板校验
- 真机：上传 2 张图 → 设置第 1/2 页文字+图片 → canvas 模板放置元素 → 打印第 1 页
  与第 2 页，票面不同（人眼确认）
- 回归：287 单测 + web_smoke + mobile_layout_check + api_verify
