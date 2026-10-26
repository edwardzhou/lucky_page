# 每页打印内容 实施计划

设计：`specs/2026-10-26-per-page-content-design.md`

## Step 1 存储与模型
- [x] 分区表新增 `media`（0xD20000, 2.875MB）
- [x] `asset_store`（48 槽 × 48KiB，槽头含 magic/len/crc/宽高/名称）
- [x] `page_content` 纯模型（JSON 往返/校验/Upsert/Find）+ Settings 字段 `page_content`
- [x] 单测：page_content 9 项、asset_store 槽头/偏移 5 项

## Step 2 模板与渲染
- [x] 块类型 `page_text` / `page_image`（JSON、校验、类型名表）
- [x] BuildCanvas 支持两种块（文字折行/图片按高度占行）
- [x] 渲染：canvas/hybrid 绘制页面图片；flow/banner 跳过（仅 canvas 生效）
- [x] PrinterService 按 `params.page` 加载文字与素材图；预览接口带 `?page=`

## Step 3 API
- [x] `/api/assets`（GET/POST/DELETE）、`/api/assets/raw`
- [x] `/api/pages/content`（GET/PUT）
- [x] 路由数上限 40→48（新增 6 条）

## Step 4 UI
- [x] `pages.html` + `pages.js`（30 页编辑 + 素材库上传/删除/缩略图）
- [x] 7 页导航（侧栏 + 移动端）；app.css 底部导航 5 列 + 侧栏可滚动
- [x] 小票编辑器新增「页面文字/页面图片」元素、预览页选择、缩略图

## Step 5 验证
- [x] 305 片上单测全过
- [x] 真机：上传素材（中文名）→ 设置第 1 页文字+图 → 带页面元素模板预览（P1 显示、P2 跳过）
- [x] 冒烟 36/36、布局 80/80、api_verify 全过
- [ ] 真机纸张验证（需保存 canvas 模板并出纸，待用户确认）

## Step 6 评审/提交
- [x] `reviews/2026-10-26-per-page-content.md`
- [ ] 提交推送
