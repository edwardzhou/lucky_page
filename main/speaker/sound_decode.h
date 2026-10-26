// sound_decode.h — 音效上传解码（纯函数，2026-10-26）
// 只支持 PCM WAV（浏览器端已统一转 24kHz 单声道；这里仍容错 8/16bit、单双声道、
// 8k~48k 采样率），输出 24kHz 单声道 16bit PCM。无 ESP-IDF 依赖，可上单测宿主。
#ifndef LUCKY_PAGE_SPEAKER_SOUND_DECODE_H_
#define LUCKY_PAGE_SPEAKER_SOUND_DECODE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sound_decode {

constexpr uint32_t kTargetRate = 24000;
constexpr double kMaxSeconds = 4.0;
constexpr size_t kMaxWavBytes = 400 * 1024;

struct Result {
    bool ok = false;
    std::string error;            // 失败原因（中文）
    std::vector<int16_t> pcm;     // 24kHz 单声道 16bit
    uint32_t duration_ms = 0;
};

/// 解码 WAV（PCM 8/16bit，1/2 声道，8k~48k）→ 24kHz 单声道 16bit
Result Decode(const uint8_t* data, size_t len);

}  // namespace sound_decode

#endif  // LUCKY_PAGE_SPEAKER_SOUND_DECODE_H_
