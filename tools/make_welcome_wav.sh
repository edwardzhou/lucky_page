#!/bin/bash
# 欢迎音素材再生成脚本（来源凭据）
# 原件 welcome.ogg 复制自 artnet-controller（main/assets/locales/zh-CN/welcome.ogg，
# 2.06s Opus 48kHz 单声道）。ESP-IDF 无 OGG/Opus 解码器，故一次性转成
# 24kHz/16bit/单声道（对齐小智 AUDIO_OUTPUT_SAMPLE_RATE） PCM WAV 嵌入固件（设计见 specs/2026-10-25-welcome-sound-design.md）。
#
# 用法：bash tools/make_welcome_wav.sh（需要 ffmpeg）
set -euo pipefail
cd "$(dirname "$0")/.."
ffmpeg -v error -y -i main/assets/welcome.ogg -ar 24000 -ac 1 -af "areverse,silenceremove=start_periods=1:start_silence=0.15:start_threshold=-50dB,areverse" -c:a pcm_s16le main/assets/welcome.wav
echo "已生成 main/assets/welcome.wav："
ffprobe -v error -show_entries stream=codec_name,sample_rate,channels \
  -show_entries format=duration -of default=noprint_wrappers=1 main/assets/welcome.wav
