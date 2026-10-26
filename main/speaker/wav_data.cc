// wav_data.cc — WAV 头解析纯函数实现（开机欢迎音，Task 2026-10-25）
#include "speaker/wav_data.h"

#include <cstring>

namespace wav {
namespace {

// 小端读 16/32 位（RIFF 规范为小端，与 ESP32-S3 一致，但按字节拼以保证可移植）
inline uint16_t Rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
inline bool TagIs(const uint8_t* p, const char* tag) {
    return memcmp(p, tag, 4) == 0;
}

constexpr size_t kMinHeader = 44;   // RIFF(12) + fmt(24) + data 头(8)

}  // namespace

WavInfo ParseWav(const uint8_t* buf, size_t buf_len) {
    WavInfo r;
    if (buf == nullptr || buf_len < kMinHeader) return r;
    if (!TagIs(buf, "RIFF") || !TagIs(buf + 8, "WAVE")) return r;
    if (!TagIs(buf + 12, "fmt ")) return r;

    // fmt 块：audio_format(2) channels(2) rate(4) byte_rate(4) block_align(2) bits(2)
    const uint8_t* fmt = buf + 20;
    if (Rd16(fmt) != 1) return r;               // 只收 PCM（audio_format=1）
    r.channels = Rd16(fmt + 2);
    r.sample_rate = Rd32(fmt + 4);
    r.bits_per_sample = Rd16(fmt + 14);
    if (r.channels != 1 || r.bits_per_sample != 16) return r;  // 本喇叭只收单声道 16bit

    // data 块：标准布局在偏移 36。宽松处理 fmt 块加长的情况：从 36 起线性找 "data"
    size_t pos = 36;
    while (pos + 8 <= buf_len && !TagIs(buf + pos, "data")) ++pos;
    if (pos + 8 > buf_len) {
        r = WavInfo{};                            // 没有 data 块
        return r;
    }
    size_t claimed = Rd32(buf + pos + 4);
    r.data_offset = pos + 8;
    if (r.data_offset >= buf_len) {
        r = WavInfo{};
        return r;
    }
    // 头里声称的长度可能越界（截断/损坏）→ 截到实际剩余
    size_t avail = buf_len - r.data_offset;
    r.data_size = (claimed < avail) ? claimed : avail;
    if (r.data_size == 0) {
        r = WavInfo{};
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace wav
