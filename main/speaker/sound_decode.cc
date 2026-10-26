// sound_decode.cc — WAV → 24kHz 单声道 16bit（线性重采样）
#include "speaker/sound_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sound_decode {
namespace {

uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

Result Fail(const char* msg) {
    Result r;
    r.error = msg;
    return r;
}

}  // namespace

Result Decode(const uint8_t* data, size_t len) {
    if (data == nullptr || len < 44) return Fail("WAV 数据过短");
    if (len > kMaxWavBytes) return Fail("WAV 超过 400KB 上限");
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        return Fail("不是 WAV 文件");

    // 找 fmt / data 块
    int format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcm_bytes = 0;
    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t* id = data + pos;
        uint32_t size = Rd32(data + pos + 4);
        size_t body = pos + 8;
        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= len) {
            format = Rd16(data + body);
            channels = Rd16(data + body + 2);
            rate = Rd32(data + body + 4);
            bits = Rd16(data + body + 14);
        } else if (memcmp(id, "data", 4) == 0) {
            pcm = data + body;
            pcm_bytes = std::min((size_t)size, len - body);
        }
        if (size > len || body + size > len) break;
        pos = body + size + (size & 1);
    }
    if (format != 1) return Fail("只支持 PCM WAV（浏览器会自动转换）");
    if (channels < 1 || channels > 2) return Fail("只支持单声道或双声道");
    if (bits != 8 && bits != 16) return Fail("只支持 8bit 或 16bit");
    if (rate < 8000 || rate > 48000) return Fail("采样率需在 8k~48k");
    if (pcm == nullptr || pcm_bytes < (size_t)(bits / 8 * channels))
        return Fail("WAV 没有音频数据");

    const size_t bytes_per_sample = bits / 8;
    const size_t frame_bytes = bytes_per_sample * (size_t)channels;
    const size_t frames = pcm_bytes / frame_bytes;
    if (frames == 0) return Fail("WAV 没有音频数据");

    // 先按目标采样率估算时长，超限直接拒绝（避免分配过大）
    const double seconds = (double)frames / rate;
    if (seconds > kMaxSeconds + 0.01) {
        Result r;
        r.error = "音效时长超过 4 秒（当前 " + std::to_string((int)(seconds * 1000)) + "ms）";
        return r;
    }

    // 解码为单声道 int16（8bit 左移扩展到 16bit）
    std::vector<int16_t> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        int32_t acc = 0;
        for (int c = 0; c < channels; ++c) {
            const uint8_t* p = pcm + i * frame_bytes + (size_t)c * bytes_per_sample;
            int32_t v = (bytes_per_sample == 2) ? (int16_t)Rd16(p) : ((int32_t)p[0] - 128) << 8;
            acc += v;
        }
        mono[i] = (int16_t)(acc / channels);
    }

    // 线性重采样到 24kHz
    const size_t out_frames = (size_t)((double)frames * kTargetRate / rate);
    Result r;
    r.ok = true;
    r.pcm.resize(std::max<size_t>(1, out_frames));
    const double step = (double)rate / kTargetRate;
    for (size_t i = 0; i < r.pcm.size(); ++i) {
        const double src = (double)i * step;
        const size_t i0 = (size_t)src;
        const size_t i1 = std::min(i0 + 1, frames - 1);
        const double frac = src - (double)i0;
        const double v = (double)mono[std::min(i0, frames - 1)] * (1.0 - frac) +
                         (double)mono[i1] * frac;
        r.pcm[i] = (int16_t)std::lround(std::max(-32767.0, std::min(32767.0, v)));
    }
    r.duration_ms = (uint32_t)(r.pcm.size() * 1000 / kTargetRate);
    return r;
}

}  // namespace sound_decode
