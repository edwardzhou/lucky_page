# 开机欢迎音 实施计划

设计：`specs/2026-10-25-welcome-sound-design.md`

## Step 1 素材入库与转换（TDD 外的资产步骤）
- [x] `welcome.ogg` → `main/assets/`（原件入库）
- [x] `tools/make_welcome_wav.sh`（ffmpeg 转换命令，16kHz/mono/s16le）
- [x] 生成 `main/assets/welcome.wav`（≈64KB）

## Step 2 纯逻辑：WAV 头解析（先写失败测试）
- [x] `test_apps/main/test_wav_data.cc`（先写，此时编译失败 = 红灯）
- [x] `main/speaker/wav_data.{h,cc}` 最小实现转绿
- [x] test_apps CMakeLists 收录（注意规则 21：新增文件后 reconfigure）

## Step 3 驱动薄层：SpeakerService
- [x] `main/speaker/speaker_service.{h,cc}`：I2S 初始化 + PlayWelcome 任务
- [x] `main/boards/esp32s3_lucky/config.h` 加 3 个引脚宏 + static_assert 冲突护栏
- [x] `main/CMakeLists.txt`：GLOB speaker 源、EMBED welcome.wav、REQUIRES esp_driver_i2s
- [x] `main/idf_component.yml`：无需改（esp_driver_i2s 是 IDF 内置组件，REQUIRES 即可）

## Step 4 集成
- [x] `Application::Start()` 末尾异步播放（失败只告警）
- [x] `console_cmd.cc` 加 `sndplay` 命令 + help 文案

## Step 5 验证（规则：全过才算完成）
- [x] `bash scripts/build.sh s3` 编译过
- [x] `python3 test_apps/run_tests.py` 全 PASS（+新增 N 项）→ **烧回主固件**
- [ ] 真机 `sndplay` 人耳确认（需喇叭接线后进行）

## Step 6 评审记录
- [x] `reviews/2026-10-25-welcome-sound.md`
