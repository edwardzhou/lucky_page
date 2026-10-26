# 自定义音效 实施计划

设计：`specs/2026-10-26-custom-sounds-design.md`

## Step 1 解码与存储
- [x] `sound_decode`（WAV 8/16bit、1/2 声道、8k~48k → 24k mono）
- [x] `sound_store`（media 分区尾部 3 槽 × 192KiB，CRC/回读校验）
- [x] 单测：解码 5 项 + 槽头 4 项

## Step 2 播放接入
- [x] SpeakerService `kCustom` 模式；三音效先查自定义后回落默认
- [x] 队列/段间隔/音量/时钟门控沿用

## Step 3 API
- [x] `/api/sounds` GET/POST/DELETE、`/raw`、`/play`；路由上限 56

## Step 4 UI
- [x] `sound.html` + `sound.js`（三卡：状态/试听/上传/恢复默认；浏览器转码 24k mono WAV）
- [x] 8 页侧栏导航「音效」

## Step 5 验证
- [x] 315 片上单测全过
- [x] 真机：上传 3 段 WAV → 保存/试听/raw/删除回默认
- [x] 布局 91/91、冒烟 38/38、api_verify 全过
- [ ] 人耳确认自定义音色与重启保持

## Step 6 评审/提交
- [x] `reviews/2026-10-26-custom-sounds.md`
- [ ] 提交推送
