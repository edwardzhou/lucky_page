# 评审 39：每页打印内容（页面文字 + 页面图片 + 素材库）

日期：2026-10-26 · 结果：通过（纸张验证待做）

## 现象（需求）

打印仍是模板化，但**每一页**可以有自己的文字和图片；图片从**多图素材库**选择；
模板编辑器可放置「页面文字/页面图片」元素并调整位置；图片只在 canvas 生效。

## 实现

| 层 | 内容 |
|---|---|
| 存储 | 新增 `media` 分区（0xD20000, 2.875MB）；`asset_store` 48 槽 × 48KiB，槽头 magic/len/crc/宽高/名称，写前擦槽、写后回读校验；页面内容存 NVS 新字段 `page_content` |
| 模型 | `page_content`（纯函数）：JSON 往返、页码/文字长度校验、Upsert/Find；损坏时按空处理 |
| 模板 | 新块 `page_text`（align/size/bold/col/row，内容来自页面）与 `page_image`（col/row/align/bg） |
| 渲染 | BuildCanvas 支持两种块；canvas/hybrid 绘制页面图（含背景层）；flow/banner 跳过图片；预览按 `?page=` 渲染 |
| 打印 | PrinterService 按 `params.page` 读页面文字与素材图（PSRAM 缓冲，失败只降级） |
| API | `/api/assets` GET/POST/DELETE、`/api/assets/raw`、`/api/pages/content` GET/PUT；路由上限 40→48 |
| UI | 新页 `pages.html`（30 页表格 + 素材库）；7 页导航；小票编辑器两个新元素 + 预览页选择 |

## 踩坑（真机）

1. **槽大小必须 4KB 对齐**：48,392B 直接 `esp_partition_erase_range` → `ESP_ERR_INVALID_ARG`
   → 上传 500。改为 49,152B（48KiB）后一次通过。教训：任何擦除长度先按扇区取整。
2. **查询参数需 URL 解码**：素材名 `%E4%B8%80...` 原样入库，页面显示乱码 →
   `QueryValue` 增加 percent/`+` 解码，中文名正常。
3. **媒体分区验证**：整包 `build.sh s3 flash`（含分区表/SPIFFS）后素材与页面内容仍在，
   证明"独立分区不被烧录冲掉"成立。
4. 渲染占位与纸面一致性：`page_image` 在 flow/banner 预览中也不显示占位，
   避免"预览有图、纸上没有"的误导。

## 验证证据

- 片上单测 **305 PASSED / 0 FAILED**（新增 page_content 9 项、asset_store 5 项、
  page 块渲染/模板 4 项）。
- 真机 API：上传 192×64 中文名素材 → `/api/assets` 返回 `一等奖图`；
  `PUT /api/pages/content` 保存 P1 文字+素材；模板预览 `?page=0` 显示
  「[页面图片]」+「P1 一等奖 · 扫地机器人」，`?page=1` 为空（块跳过）。
- `web_smoke.py` 36/36；`mobile_layout_check.py` 80/80（7 页）；
  `api_verify.py` 全过（默认不出纸）。
- 设备截图：`/tmp/dev_pages_new.png`（页面内容页）、`/tmp/dev_receipt_new.png`。
- **模板端到端（含页面元素）**：保存 canvas 模板
  `[page_image, page_text, line, feed]` → 回读块类型逐一正确、生效模式=canvas →
  `?page=0` 预览含页面文字；随后按快照整体还原（flow 15 块 / canvas 18 块 / banner 9 块，
  生效模式 flow）并复核。还原顺序教训：三槽按 0→1→2 写会把生效模式留成最后一个槽，
  必须最后再写一次原生效槽。

## 遗留

- **纸张验证**：需保存一个含页面元素的 canvas 模板后真实出票，确认页面图与文字位置。
  保存模板会改动用户当前版式，按规则需先确认（可用快照→还原方式做）。
- 素材库小图占整槽（48KiB）；48 槽对 30 页足够，后续如不够再改变长分配。
