# 评审 40：自定义音效（欢迎 / 中奖 / 落空）

日期：2026-10-26 · 结果：通过（人耳听感待确认）

## 现象（需求）

欢迎音、中奖音效、落空音效支持用户上传自定义音频；未上传时回落默认；可试听/删除。

## 实现

| 层 | 内容 |
|---|---|
| 解码 | `sound_decode`（纯函数）：PCM WAV 8/16bit、1/2 声道、8k~48k → **24kHz 单声道 16bit**（线性重采样）；>4 秒或 >400KB 拒绝 |
| 存储 | `sound_store`：media 分区图片 48 槽之后，**3 槽 × 192KiB**（≈4s PCM）；槽头 magic/role/len/crc/rate；写前擦槽、写后 CRC 校验；4KB 对齐 |
| 播放 | `SpeakerService` 新增 `kCustom` 模式：欢迎/中奖/落空先查自定义，存在则从 flash 读入 PSRAM 走既有串行队列；不存在回落内置 WAV / 合成音 |
| API | `GET /api/sounds`、`POST?role=`、`DELETE?role=`、`GET /api/sounds/raw?role=`、`POST /api/sounds/play?role=`；路由上限 56 |
| UI | 新页 `sound.html` + `sound.js`：三张角色卡（状态/试听/上传/恢复默认）；浏览器用 AudioContext 解码任意音频 → OfflineAudioContext 渲染 24kHz 单声道 → 手工写 WAV 上传；8 页侧栏新增「音效」 |

## 视频/音频边界

- 只接受 PCM WAV（前端已转码；格式不支持时后端给中文提示）
- 时长 ≤4.0s（前端先校验并提示当前秒数），大小 ≤400KB
- 自定义存在但读取/CRC 失败 → 本次不出声并告警，**不影响抽奖/打印**（规则 7）

## 踩坑

1. **测试宿主反复重启**：解码测试用 48kHz 双声道 1s WAV（192KB）构造样本 →
   片上堆不足 → abort 重启，run_tests 把 6 轮 PASS 累加成 1446。
   改为 0.25s / 8kHz 构造后单轮 315 项全过。
   → 测试大缓冲素材必须按**片上堆**而不是主机内存估算。
2. **GLOB 新文件**：`sound_store.cc` 加入 speaker/ 后未 reconfigure → 链接 undefined；
   reconfigure 解决（规则 21 再现）。
3. `QueryValue` 需前置声明，否则音效段在定义之前编译不过（挪到公共匿名命名空间）。

## 验证证据

- 片上单测 **315 PASSED / 0 FAILED**（新增 SoundDecode 5 项 + SoundStore 4 项）
- 真机 API：上传 3 段 44100Hz 双声道 1.2s WAV → 均保存成功（14400ms? 实际 duration_ms=1200，
  bytes=57600 = 24k×1.2s×2B）；`/api/sounds/play?role=lose` 返回 playing；
  `/api/sounds/raw?role=win` 返回 57644B WAV（RIFF 头）；删除三个后全部回默认
- `sound.html` 真机加载：3 张卡片 / 3 个文件选择 / 试听与恢复默认按钮齐全，
  零 JS 异常、零 API 4xx5xx（截图 `/tmp/dev_sound.png`）
- `mobile_layout_check.py` 91/91（8 页）；`web_smoke.py` 38/38（8 页）
- 播放是否真的用了自定义音色需人耳确认（自动化只能验到接口/存储/队列）

## 遗留

- 人耳确认：上传自己的音效 → 试听 / 真实抽奖 / 重启开机，确认播放的是自定义且重启后仍在。
- 小图/音频槽为定长，浪费空间属设计取舍（48 图片槽 + 3 音效槽，够 30 页场景）。
