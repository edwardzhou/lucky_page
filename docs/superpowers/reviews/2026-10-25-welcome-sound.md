# 评审 35：开机欢迎音（小智 S3 喇叭配置移植）

日期：2026-10-25 · 结果：**通过**（真人耳确认待喇叭接线后复核，见"遗留"）

## 现象（需求）

参考 xiaozhi-esp32 小智 S3（bread-compact-wifi）的喇叭配置，设备开机后播放
欢迎音；素材复用 artnet-controller 的 welcome.ogg。

## 根因/关键决策（为什么这么做）

1. **引脚**：小智 S3 喇叭 = BCLK=G15 / LRCK=G16 / DOUT=G7。核对 lucky-page 全部
   引脚表（打印机 9/10/11、DMX 17/18/4、按钮 8、复位 5、IR 3、禁用 43/44/19/20），
   三脚全空闲 → 原样借用，并加 `SPEAKER_PINS_OK` 宏 + static_assert 护栏防将来冲突。
2. **不移植 OGG 解码**：welcome.ogg 是 Opus（2.06s/48kHz）。artnet-controller 为播它
   引入 esp_audio_codec + 自写 OGG demuxer + esp_audio_effects 重采样三套依赖；
   ESP-IDF 无内置解码器。欢迎音只 2 秒 → 主机一次性转 16kHz/16bit/单声道 WAV
   （64KB）EMBED 进固件（font24.bin 630KB 先例），运行时零解码依赖。
3. **C++ 前置声明踩坑**：头文件里 `typedef struct i2s_channel_t*` 与
   esp_driver_i2s 的 `struct i2s_channel_obj_t*` 冲突编译失败 → 改为与
   i2s_types.h 完全一致的前置声明。

## 修法（改动清单）

| 文件 | 改动 |
|---|---|
| `main/boards/esp32s3_lucky/config.h` | 喇叭三脚宏 + 采样率 + static_assert 冲突护栏 |
| `main/speaker/wav_data.{h,cc}` | 新增：WAV 头解析纯函数（PCM/16bit/mono 校验、data 越界截断） |
| `main/speaker/speaker_service.{h,cc}` | 新增：I2S(esp_driver_i2s std 模式) 懒初始化 + 异步播放任务 |
| `main/assets/welcome.{ogg,wav}` | 素材入库（ogg 原件凭据 + wav 嵌入产物） |
| `tools/make_welcome_wav.sh` | 转换命令脚本（ffmpeg 16kHz/mono/s16le） |
| `main/CMakeLists.txt` | speaker 源 GLOB、EMBED welcome.wav、REQUIRES esp_driver_i2s |
| `main/application.cc` | Start() 末尾 `PlayWelcome()`（异步、失败只降级，规则 7） |
| `main/console_cmd.cc` | `sndplay` 命令 + help 文案 |
| `test_apps/main/test_wav_data.cc` | 7 项单测（TDD 先写） |
| `test_apps/main/CMakeLists.txt` | 收录 wav_data.cc（reconfigure 后生效，规则 21） |

降级验证路径：I2S 申请失败 → `init_failed_` 置位只告警一次，不影响抽奖/打印/Web。

## 验证证据

1. 编译：`bash scripts/build.sh s3` → `Project build complete`（welcome.wav.S 已链接）。
2. 片上单测：`run_tests.py` → **278 PASSED, 0 FAILED**（基线 271 + 新增 7；
   `nm test_wav_data.cc.obj` 确认 7 个 TEST_WavData_* 符号在测试固件内）。
   跑完已整包烧回主固件（`build.sh s3 flash`，规则 §五）。
3. 真机开机日志（复位后串口捕获）：
   `I (3149) SpeakerSvc: 欢迎音播放完成（65620 字节 PCM）`
   —— I2S 通道建立、WAV 校验、65620B PCM（≈2.05s@16kHz）全部写入完成。

## 遗留

- **人耳确认**：需按 G15→BCLK / G16→LRC / G7→DIN 接好 MAX98357A + 喇叭后，
  上电听开机音 / 串口 `sndplay` 复听。音量偏小时可把功放 GAIN 脚接 GAIN 焊盘调档，
  或 SD 脚接高电平选纯左声道（避免 (L+R)/2 减半）。
- MAX98357A 的 SD 悬空 = (L+R)/2：I2S mono 模式数据只在左槽 → 实际音量约一半，
  属预期行为，不修。


---

# 评审 35 续：真机失真排障（同日第二轮）

## 现象

真机播放"沙哑失真"→ 修 BCLK 后"稍好仍有爆破"→ 修音量后"还是不清晰"。

## 根因（三层，逐层剥出）

1. **BCLK 过低（已修）**：初版 16bit 槽 → BCLK=512kHz，MAX98357A 内部 PLL 失锁 →
   沙哑。修为 32bit 槽（BCLK=1.024MHz，对齐小智 no_audio_codec.cc 真机配置：
   `data_bit_width=32BIT / mono / LEFT / mclk_multiple=256`），PCM 16bit 样本
   `<<16` 左对齐进 32bit 槽 → "稍微好一些"。
2. **功放削顶（已修）**：素材峰值 -1.2dB 近满幅 + MAX98357A 默认 9dB 增益 → 削顶爆破。
   加软件音量平方律 70%（=小智默认 output_volume_=70，-6.2dB）→ "仍有爆破"。
3. **供电不足（真根因，硬件侧待用户改线）**：`sndtest` 正弦对照实验 + 用户确认
   "正弦也破音 + 功放接 3V3 + 4Ω 喇叭" → 3.3V/4Ω 峰值电流 1A+，板载 3V3 LDO
   还要与 WiFi/PSRAM 分摊 → 功放欠压破音。两层软件修复只是降低抽流缓解了症状。
   **修法：功放 VIN 改接 5V/VIN（可就近并 470µF 电解）**。

## 修法（本轮代码）

- `speaker_service.cc`：32bit 槽 + `I2S_MCLK_MULTIPLE_256` + `slot_mask=LEFT`；
  16bit→32bit 样本转换；平方律软件音量 `kPlayVolumePercent=70`（编译期常量）。
- 新增 `PlayTone(freq,ms)` + 串口 `sndtest [Hz] [ms]`：**软件生成 440Hz 纯正弦做
  对照实验**，用于分离"素材问题 vs 硬件链问题"——本轮定性全靠它。

## 验证证据

- 编译 `Project build complete`；烧录后串口 `SpeakerSvc: 欢迎音播放完成` 正常。
- `sndtest` 正弦仍破音（用户人耳）→ 定位到硬件供电（见根因 3）。
- 待办：用户改 5V 供电后复听 `sndtest`/`sndplay` 人耳确认。

## 教训（通用原则）

- **模拟音频问题先做"最干净信号对照"**（正弦 vs 素材），别对着素材连改三版参数。
- MAX98357A 三连坑：① BCLK≥64fs（16bit 槽必失真）② 增益 9dB + 满幅素材必削顶
  ③ 3V3 带 4Ω 必欠压。三个都踩了才到真根因。


---

# 评审 35 终章：失真真根因 = 功放 GND 虚接（同日第三轮）

## 现象

软件侧连续五版修复（16bit槽→32bit槽→软件音量→24kHz→stereo复刻）后失真依旧，
且 20% 音量仍失真（与幅度无关），三个"已知正常"参考配置（小智 32bit-mono、
卖家例程 16bit-stereo、期间自试 16bit-mono）全部复刻无效。

## 根因（真根因）

**功放板 GND 未与 ESP32 接通（虚接）**——用户把 GND 接通后声音立即正常。
DIN/BCLK/LRC 没有公共参考地 → 信号电平漂浮 + 噪声耦合 → 严重失真。
完美解释全部症状：与音量无关、与 I2S 格式无关、与采样率无关、与 WiFi 无关。

## 排障链复盘（六轮，供日后参照）

| 轮次 | 假设 | 动作 | 结果 |
|---|---|---|---|
| 1 | BCLK 过低 PLL 失锁 | 16bit槽→32bit槽(64fs) | 稍好 |
| 2 | 功放削顶 | 软件音量 70% 平方律 | 稍好仍爆破 |
| 3 | 采样率差异 | 对齐小智 24kHz | 更差（伪相关） |
| 4 | 幅度相关验证 | sndvol 20% 对照 | 仍失真→排除幅度类 |
| 5 | 槽结构差异 | 复刻卖家例程 16bit-stereo | 仍失真→排除格式类 |
| 6 | **公共地** | **用户接通 GND** | **正常** ✅ |

## 最终固件形态

- 24kHz / 16bit / 立体声双槽（L=R 复制，卖家例程等效）+ 平方律软件音量 70%
- 串口命令族：`sndplay`（欢迎音）/ `sndtest [Hz] [ms]`（正弦诊断）/
  `sndvol <1-100>`（运行时音量，幅度对照诊断）
- 正弦模式日志修正：打印频率/时长而非"0 字节 PCM"

## 验证证据

- 278 PASSED / 0 FAILED（片上单测全量），已烧回主固件
- 用户真机确认：GND 接通后声音正常
- `wifi off/on` A/B 序列已执行（用于交叉验证，非必需）

## 教训（铁律级）

1. **模拟/数字混合外设问题第一步先查公共地**，再查供电，最后才轮到信号格式。
   本案例六轮里有五轮在软件层追凶，真根因是一根没接的地线。
2. "同一模块在别处正常"≠"你的接线和它一致"——用户口述"接线一样"时，
   GND 这种"默认应该有"的连接最容易被遗漏，要显式逐线核对。
3. 与幅度无关 + 与格式无关的失真 → 物理层（地/供电/接线）问题，软件无解，
   此时继续烧固件是浪费轮次——应第一时间要求逐线核对接线。


---

# 附：音量最终定档（同日）

- 默认音量 70% → **100%**（用户要求最大音量）。
- 真机确认：**100% 满幅 + 模块 12dB 增益下声音干净无破音**（GND 修复后）。
  之前"削顶"判断实为 GND 虚接的表象之一，非真削顶。
- 运行时回调通道保留：串口 `sndvol <1-100>`（重启回 100%）。
- 人耳确认（规则五"纸面"级）：✅ 由用户完成。
