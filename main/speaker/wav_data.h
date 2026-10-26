// wav_data.h — WAV 头解析纯函数（开机欢迎音，Task 2026-10-25）
// 只支持本项目素材形态：PCM / 16bit / 单声道（tools/make_welcome_wav.sh 产出）。
// 纯逻辑，无 ESP-IDF 依赖，可上 test_apps 单测宿主。
#ifndef LUCKY_PAGE_SPEAKER_WAV_DATA_H_
#define LUCKY_PAGE_SPEAKER_WAV_DATA_H_

#include <cstddef>
#include <cstdint>

namespace wav {

struct WavInfo {
    bool ok = false;             // 整体是否可播
    uint32_t sample_rate = 0;    // 采样率（Hz）
    uint16_t channels = 0;       // 声道数（本模块只接受 1）
    uint16_t bits_per_sample = 0;// 位深（本模块只接受 16）
    size_t data_offset = 0;      // PCM 数据在缓冲内的起始偏移
    size_t data_size = 0;        // PCM 字节数（已按缓冲实际长度截断）
};

// 解析 RIFF/WAVE 头。任何不符合"PCM/16bit/单声道"的情况都返回 ok=false，
// 绝不越界读取：data_size 会被截断到 buf_len - data_offset。
WavInfo ParseWav(const uint8_t* buf, size_t buf_len);

}  // namespace wav

#endif  // LUCKY_PAGE_SPEAKER_WAV_DATA_H_
