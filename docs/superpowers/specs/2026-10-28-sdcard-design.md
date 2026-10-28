# SD 卡能力移植设计（来自 dms-pager）

日期：2026-10-28 · 状态：实施中

## 需求（用户确认）

1. 从 `~/esp32_work/dms-pager` 导入 SD 卡能力（SPI 模式）。
2. 引脚：**MISO=G13 / SCK=G14 / MOSI=G21 / CS=G47**（S3 外接模块）。
3. 范围：**文件管理**（列表/上传/下载/删除/重命名 + 设备页）+ **素材优先存 SD**
   （挂载成功时图片/音效写 SD；无卡时回落现有 flash 槽）。

## 移植来源与改造

| dms-pager | lucky-page 改造 |
|---|---|
| `storage/sd_card.{h,cc}` + `sd_spi_backend` + `sd_mmc_backend` | 精简为单 SPI 实现 `main/storage/sd_card.{h,cc}`（S3 只保留 SPI；保留延迟挂载/3 次重试/卸载/容量查询） |
| 板级 `CreateSdCardBackend()` | 直接由 `boards/esp32s3_lucky/config.h` 提供 4 个引脚宏 + static_assert 冲突护栏 |
| Web 7 接口 `/api/sdcard/*` | 原样语义移植：list/upload/download/file(DELETE)/rename/mount/unmount；补路径穿越防护（纯函数 + 单测） |
| `tab-sdcard.js` 文件管理 UI | 新页 `sdcard.html`（Lucky Studio 风格，第 9 页）：面包屑、列表、上传、下载、删除、重命名、挂载状态/容量 |

## 挂载与降级（规则 7）

- 启动后**延迟 1s** 挂载（避免阻塞开机）；失败重试 3 次，仍失败只打日志；
- **SD 未挂载时一切照常**：抽奖/打印/Web 不受影响；素材读写自动回落 flash 槽；
- 挂载点 `/sdcard`（FAT，`format_if_mount_failed=false`，max_files=10）。

## 素材分层存储

- 目录：`/sdcard/lucky/assets/<id>.lgo`、`/sdcard/lucky/sounds/<role>.pcm`（挂载时自动创建）
- 读：SD 文件优先；不存在则回落 flash 槽（保证旧素材仍可打印）
- 写：SD 已挂载 → 写 SD（不占 flash 槽）；未挂载 → 写 flash 槽（原行为）
- 删除：两处都删（避免同名 id 复活）
- 列表/容量：SD 挂载时按 SD 文件枚举；否则按 flash 槽（保持接口形状不变）

## 验证

- 单测：路径安全/规范化、素材文件名校验、容量格式化等纯函数
- 真机：插入卡 → `/api/sdcard/list` 见容量与目录；上传/下载/删除/重命名；拔卡 → 接口 mounted=false、素材回落 flash
- 回归：315+ 单测、web_smoke、mobile_layout_check、api_verify（新增 SD 只读用例）
