# 开机欢迎音（小智 S3 喇叭配置移植）设计

日期：2026-10-25 · 状态：已评审（用户确认三个决策点）

## 需求

设备开机后通过 I2S 功放播放欢迎音（素材复用 artnet-controller 的
`main/assets/locales/zh-CN/welcome.ogg`，2.06 秒 Opus 48kHz 单声道，3.4KB）。
另需串口测试命令，便于真机验证。

## 硬件方案（参考 xiaozhi-esp32 小智 S3 = bread-compact-wifi）

- 功放：**MAX98357A I2S 数字功放板**（用户确认），无源喇叭接功放输出。
- 引脚完全对齐小智 S3 喇叭配置（已核对 lucky-page 无冲突，见 §引脚）：

| 信号 | GPIO | 接 MAX98357A |
|---|---|---|
| BCLK | **G15** | BCLK |
| LRCK(WS) | **G16** | LRC |
| DOUT | **G7** | DIN |
| 3V3 / GND | — | VIN / GND（SD 脚悬空 = (L+R)/2 单声道输出，正好） |

- 冲突核对（static_assert 锁定）：打印机 G9/10/11、DMX G17/18/4、抽奖按钮 G8、
  复位按钮 G5、IR G3、禁用 G43/44/19/20 —— **G7/G15/G16 均空闲**。

## 素材方案：OGG 一次性转 WAV 嵌入固件（不做运行时解码）

- ESP-IDF **没有内置 OGG/Opus 解码器**。artnet-controller 为播这个 ogg 引入了
  `esp_audio_codec`(opus dec) + 自写 OGG demuxer + `esp_audio_effects`(重采样) 三套依赖。
- 欢迎音仅 2 秒，引入解码栈不成比例 → **主机上一次性转换**：
  `ffmpeg -i welcome.ogg -ar 16000 -ac 1 -c:a pcm_s16le welcome.wav` ≈ 64KB。
- `welcome.wav` 经 CMake `EMBED_FILES` 进固件（与 font24.bin 同法，≈630KB 先例）；
  `welcome.ogg` 原件一并入库（来源凭据），`tools/make_welcome_wav.sh` 记录转换命令。
- 16kHz/16bit/单声道 = MAX98357A 支持的最低复杂度格式（fs 8k–96k、32fs BCLK）。

## 软件结构（规则 5 分层 + 纯逻辑下沉）

```
main/speaker/
  wav_data.{h,cc}     纯函数：WAV 头解析（RIFF/fmt/data 块定位、参数校验）
                      → 可上 test_apps 单测宿主（TDD）
  speaker_service.{h,cc}  I2S 驱动薄层 + 播放任务（唯一硬件访问点）
```

- `SpeakerService`（单例，懒初始化）：
  - `Init()`：i2s_new_channel(TX) + std 模式 16bit/16kHz，失败**只打日志**（规则 7，
    喇叭坏绝不影响抽奖）。
  - `PlayWelcome()`：互斥防重入；独立低优先级任务写入 PCM（分块 20ms，
    避免长阻塞）；播放中可被下一次调用打断。
  - I2S 用 v6 新驱动（`esp_driver_i2s` 组件，`i2s_channel.h`）。
- 启动时机：`Application::Start()` 末尾（全部初始化完成后），**异步**播放，
  不阻塞主循环与 WebServer。
- 串口命令：`sndplay`（重播欢迎音，真机验证用）。

## 失败降级（规则 7）

I2S 通道申请失败 / WAV 嵌入数据校验失败 → `ESP_LOGW` 一条，继续运行；
抽奖、打印、Web 全不受影响。**初始化路径禁止 ESP_ERROR_CHECK**。

## 测试

1. 纯逻辑单测（test_apps/test_wav_data.cc）：合法头解析、坏 RIFF、坏 fmt、
   非 PCM 格式、data 块定位、空数据拒绝。
2. 编译：`bash scripts/build.sh s3`。
3. 片上单测：`python3 test_apps/run_tests.py`（**跑完烧回主固件**）。
4. 真机：串口 `sndplay` 听声（人耳，规则"纸面人眼确认"同类）。

## 明确不做（本次范围外）

- Web 配置开关 / Settings 字段（用户选"仅开机播放 + 串口命令"）
- 音量软件调节（MAX98357A 用 GAIN 引脚硬件调）
- 抽奖中奖音效（后续可复用 SpeakerService）
