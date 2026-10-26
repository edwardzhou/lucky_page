// test_sound_decode.cc — 自定义音效 WAV 解码/重采样纯逻辑单测
#include "lucky_test.h"
#include <cstring>
#include <vector>
#include "speaker/sound_decode.h"

using sound_decode::Decode;

// 构造 PCM WAV（正弦波）
static std::vector<uint8_t> MakeWav(uint32_t rate, int channels, int bits, double seconds) {
    const size_t frames = (size_t)(rate * seconds);
    const size_t bytes_per = bits / 8;
    const size_t data_bytes = frames * channels * bytes_per;
    std::vector<uint8_t> b;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(v >> (8 * i)); };
    auto put16 = [&](uint16_t v) { for (int i = 0; i < 2; i++) b.push_back(v >> (8 * i)); };
    auto tag = [&](const char* t) { for (int i = 0; i < 4; i++) b.push_back(t[i]); };
    tag("RIFF"); put32(36 + data_bytes); tag("WAVE");
    tag("fmt "); put32(16); put16(1); put16((uint16_t)channels); put32(rate);
    put32(rate * channels * bytes_per); put16((uint16_t)(channels * bytes_per)); put16((uint16_t)bits);
    tag("data"); put32(data_bytes);
    for (size_t i = 0; i < frames; ++i) {
        const double v = 0.5 * 32767.0 * (i % 2 ? -1 : 1);
        for (int c = 0; c < channels; ++c) {
            if (bits == 16) put16((uint16_t)(int16_t)v);
            else b.push_back(128 + (int)(v / 256));
        }
    }
    return b;
}

TEST(SoundDecode, RejectsNonWav) {
    std::vector<uint8_t> junk(100, 0x55);
    auto r = Decode(junk.data(), junk.size());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(!r.error.empty());
    EXPECT_FALSE(Decode(nullptr, 0).ok);
}

TEST(SoundDecode, Converts48kStereoTo24kMono) {
    // 只用 0.25s：48k 双声道 1s 的 WAV 有 192KB，测试宿主堆不够会 abort（真机教训）
    auto wav = MakeWav(48000, 2, 16, 0.25);
    auto r = Decode(wav.data(), wav.size());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ((size_t)(24000 * 0.25), r.pcm.size());
    EXPECT_EQ(250u, r.duration_ms);
}

TEST(SoundDecode, Resamples8kAnd441k) {
    auto a = Decode(MakeWav(8000, 1, 16, 0.5).data(), MakeWav(8000, 1, 16, 0.5).size());
    EXPECT_TRUE(a.ok);
    EXPECT_EQ(12000u, (unsigned)a.pcm.size());
    auto w = MakeWav(44100, 1, 16, 0.2);
    auto b = Decode(w.data(), w.size());
    EXPECT_TRUE(b.ok);
    EXPECT_TRUE(b.pcm.size() >= 4700 && b.pcm.size() <= 4900);
}

TEST(SoundDecode, Accepts8Bit) {
    auto wav = MakeWav(16000, 1, 8, 0.25);
    auto r = Decode(wav.data(), wav.size());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(6000u, (unsigned)r.pcm.size());
}

TEST(SoundDecode, RejectsOver4Seconds) {
    // 用 8k 采样率构造 4.5s（72KB），避免测试主机内存压力
    auto wav = MakeWav(8000, 1, 16, 4.5);
    auto r = Decode(wav.data(), wav.size());
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.find("4 秒") != std::string::npos);
}

TEST(SoundDecode, RejectsUnsupportedRateOrBits) {
    auto bad_rate = MakeWav(6000, 1, 16, 0.2);
    EXPECT_FALSE(Decode(bad_rate.data(), bad_rate.size()).ok);
    auto bad_bits = MakeWav(24000, 1, 16, 0.2);
    // 改 fmt 的 bits 字段（偏移 34）
    bad_bits[34] = 24; bad_bits[35] = 0;
    EXPECT_FALSE(Decode(bad_bits.data(), bad_bits.size()).ok);
}
