// test_wav_data.cc — WAV 头解析纯函数单测（开机欢迎音，TDD 先写测试）
// 期望值来自对 main/assets/welcome.wav 的十六进制实测与 RIFF 规范。
#include "lucky_test.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include "speaker/wav_data.h"

// 构造一个最小合法 WAV（16kHz/16bit/单声道，N 个采样）
static std::vector<uint8_t> MakeWav(uint16_t channels, uint32_t rate,
                                    uint16_t bits, size_t pcm_bytes) {
    std::vector<uint8_t> b;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(v >> (8 * i)); };
    auto put16 = [&](uint16_t v) { for (int i = 0; i < 2; i++) b.push_back(v >> (8 * i)); };
    auto tag = [&](const char* t) { for (int i = 0; i < 4; i++) b.push_back(t[i]); };
    tag("RIFF"); put32(36 + pcm_bytes); tag("WAVE");
    tag("fmt "); put32(16); put16(1); put16(channels); put32(rate);
    put32(rate * channels * bits / 8); put16(channels * bits / 8); put16(bits);
    tag("data"); put32(pcm_bytes);
    b.insert(b.end(), pcm_bytes, 0x55);
    return b;
}

TEST(WavData, ParseMinimalValidWav) {
    auto b = MakeWav(1, 16000, 16, 32);
    auto r = wav::ParseWav(b.data(), b.size());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(16000u, r.sample_rate);
    EXPECT_EQ(1u, r.channels);
    EXPECT_EQ(16u, r.bits_per_sample);
    EXPECT_EQ(32u, r.data_size);
    EXPECT_EQ(44u, r.data_offset);   // 标准 44 字节头之后即 PCM
}

TEST(WavData, ParseRejectsShortBuffer) {
    auto b = MakeWav(1, 16000, 16, 4);
    EXPECT_FALSE(wav::ParseWav(b.data(), 3).ok);          // 连 RIFF 头都不够
    EXPECT_FALSE(wav::ParseWav(b.data(), 0).ok);
    EXPECT_FALSE(wav::ParseWav(nullptr, 100).ok);
}

TEST(WavData, ParseRejectsBadMagic) {
    auto b = MakeWav(1, 16000, 16, 8);
    b[0] = 'X';
    EXPECT_FALSE(wav::ParseWav(b.data(), b.size()).ok);
    b[0] = 'R'; b[8] = 'X';                                 // WAVE 魔数错
    EXPECT_FALSE(wav::ParseWav(b.data(), b.size()).ok);
}

TEST(WavData, ParseRejectsCompressedFormat) {
    auto b = MakeWav(1, 16000, 16, 8);
    b[20] = 0x50; b[21] = 0x01;                             // audio_format=336 → 非 PCM
    EXPECT_FALSE(wav::ParseWav(b.data(), b.size()).ok);
}

TEST(WavData, ParseRejectsStereoAnd8Bit) {
    auto st = MakeWav(2, 16000, 16, 8);
    auto r2 = wav::ParseWav(st.data(), st.size());
    EXPECT_FALSE(r2.ok);                                     // 本项目喇叭只收单声道
    auto b8 = MakeWav(1, 16000, 8, 8);
    EXPECT_FALSE(wav::ParseWav(b8.data(), b8.size()).ok);    // 只收 16bit
}

TEST(WavData, ParseRejectsEmptyData) {
    auto b = MakeWav(1, 16000, 16, 0);
    EXPECT_FALSE(wav::ParseWav(b.data(), b.size()).ok);
}

TEST(WavData, ParseClampsDataSizeToBuffer) {
    auto b = MakeWav(1, 16000, 16, 16);
    // 声称 data 有 1KB，但文件只有 60B → 截到实际剩余，而不是越界
    b[40] = 0x00; b[41] = 0x04;                              // size=1024
    auto r = wav::ParseWav(b.data(), b.size());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(b.size() - 44u, r.data_size);
}
