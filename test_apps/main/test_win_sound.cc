// test_win_sound.cc — 中奖音效合成纯函数单测（TDD 先写测试）
#include "lucky_test.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include "speaker/win_sound.h"

// 24kHz（= SPEAKER_SAMPLE_RATE）为真机采样率
static constexpr uint32_t kRate = 24000;

TEST(WinChime, DurationMatchesNotes) {
    auto pcm = speaker::SynthWinChime(kRate);
    // 4 音符 ×130ms + 尾音 450ms = 970ms（见 win_sound.h 注释）
    EXPECT_EQ((size_t)(kRate * 970 / 1000), pcm.size());
}

TEST(WinChime, StartsWithNearSilence) {
    auto pcm = speaker::SynthWinChime(kRate);
    // 起播防爆音 = 斜率受限 + 起点为零。主机实测轨迹（g++）：
    //   0,75,296,655,1138,1727,2400,3129,3885,4637 —— 渐入每样本增量 <800。
    // 突变起播的相邻差会是数万量级，斜率断言可稳定拦截。
    // （教训 ×2：先按 2ms 全窗口近零断言，再按前 8 样本阈值断言，均量纲错误）
    EXPECT_EQ(0, pcm[0]);
    for (size_t i = 1; i < 16 && i < pcm.size(); ++i) {
        int diff = pcm[i] - pcm[i - 1];
        EXPECT_TRUE(diff < 1000 && diff > -1000);
    }
}

TEST(WinChime, EndsWithNearSilence) {
    auto pcm = speaker::SynthWinChime(kRate);
    // 末 2ms 同理（尾音衰减到近零）
    for (size_t i = pcm.size() - 48; i < pcm.size(); ++i) {
        EXPECT_TRUE(pcm[i] < 2000 && pcm[i] > -2000);
    }
}

TEST(WinChime, PeakReachesAudibleLevel) {
    auto pcm = speaker::SynthWinChime(kRate);
    int16_t peak = 0;
    for (int16_t s : pcm) {
        if (s > peak) peak = s;
        if (-s > peak) peak = -s;
    }
    EXPECT_TRUE(peak > 15000);   // 峰值 ≥ ~0.46 满幅，保证响度（设计 0.8≈26000）
}

TEST(WinChime, AllSamplesInInt16Range) {
    auto pcm = speaker::SynthWinChime(kRate);
    for (int16_t s : pcm) {
        EXPECT_TRUE(s >= -32768 && s <= 32767);   // 编译器层面必然，锁语义防回归
    }
}

TEST(WinChime, Deterministic) {
    auto a = speaker::SynthWinChime(kRate);
    auto b = speaker::SynthWinChime(kRate);
    EXPECT_EQ(a.size(), b.size());
    EXPECT_EQ(0, memcmp(a.data(), b.data(), a.size() * sizeof(int16_t)));
}

TEST(LoseChime, DurationAndShape) {
    auto pcm = speaker::SynthLoseChime(kRate);
    // E4 150ms + C4 320ms = 470ms
    EXPECT_EQ((size_t)(kRate * 470 / 1000), pcm.size());
    // 起点为零 + 前段斜率受限（防爆音，与 WinChime 同判据）
    EXPECT_EQ(0, pcm[0]);
    for (size_t i = 1; i < 16 && i < pcm.size(); ++i) {
        int diff = pcm[i] - pcm[i - 1];
        EXPECT_TRUE(diff < 1000 && diff > -1000);
    }
}

TEST(LoseChime, PeakBelowWinChime) {
    // 落空音应比中奖音轻（0.6 vs 0.8 峰值）——不超过 0.75 满幅
    auto lose = speaker::SynthLoseChime(kRate);
    int16_t peak = 0;
    for (int16_t x : lose) {
        if (x > peak) peak = x;
        if (-x > peak) peak = -x;
    }
    EXPECT_TRUE(peak < 24575);   // 0.75 × 32767
    EXPECT_TRUE(peak > 8000);    // 但仍要听得见
}

TEST(LoseChime, Deterministic) {
    auto a = speaker::SynthLoseChime(kRate);
    auto b = speaker::SynthLoseChime(kRate);
    EXPECT_EQ(a.size(), b.size());
    EXPECT_EQ(0, memcmp(a.data(), b.data(), a.size() * sizeof(int16_t)));
}
