// win_sound.cc — 中奖音效合成实现（2026-10-25）
#include "speaker/win_sound.h"

#include <cmath>

namespace speaker {
namespace {

constexpr int      kNoteMs     = 130;   // 琶音每音时长
constexpr int      kTailMs     = 450;   // 尾音衰减时长
constexpr float    kPeak       = 0.8f;   // 峰值（满幅比例）
constexpr uint32_t kFadeSample = 48;    // 首尾渐变 ≈2ms@24kHz

// 毫秒 → 样本数（合成时长与采样率解耦）
inline size_t rate_scale(int ms, uint32_t rate) {
    return (size_t)((uint64_t)ms * rate / 1000);
}

// 一个音符的正弦段：写入 out，start..end 为绝对样本区间，fade 控制首尾渐变
void RenderNote(std::vector<int16_t>& out, size_t start, size_t len,
                float freq, uint32_t rate, bool exp_decay) {
    // ⚠ 全程 float：S3 只有单精度 FPU，double 的 sin/exp 走软浮点，
    //   合成 970ms 音效要数百 ms（真机实测 chime 完成耗时超理论 680ms 的原因）
    const float w = 2.0f * (float)M_PI * freq / (float)rate;
    for (size_t i = 0; i < len; ++i) {
        float amp = kPeak;
        if (exp_decay) amp *= expf(-3.0f * (float)i / (float)len);   // 尾音指数衰减
        // 首尾 2ms 线性渐变（防爆音）
        if (i < kFadeSample) amp *= (float)i / (float)kFadeSample;
        if (len - i <= kFadeSample) amp *= (float)(len - i) / (float)kFadeSample;
        float v = sinf(w * (float)i) * amp;
        long x = lrintf(v * 32767.0f);
        out[start + i] = (int16_t)(x < -32767 ? -32767 : (x > 32767 ? 32767 : x));
    }
}

}  // namespace

std::vector<int16_t> SynthWinChime(uint32_t sample_rate) {
    const float notes[] = {523.25f, 659.25f, 783.99f, 1046.5f};   // C5 E5 G5 C6
    size_t note_len = (size_t)rate_scale(kNoteMs, sample_rate);
    size_t tail_len = (size_t)rate_scale(kTailMs, sample_rate);
    std::vector<int16_t> out(note_len * 4 + tail_len);
    for (int n = 0; n < 4; ++n) {
        RenderNote(out, (size_t)n * note_len, note_len, notes[n], sample_rate, false);
    }
    RenderNote(out, 4 * note_len, tail_len, notes[3], sample_rate, true);
    return out;
}

std::vector<int16_t> SynthLoseChime(uint32_t sample_rate) {
    const float notes[] = {329.63f, 261.63f};   // E4 C4（下行）
    const int lens_ms[] = {150, 320};
    size_t len0 = rate_scale(lens_ms[0], sample_rate);
    size_t len1 = rate_scale(lens_ms[1], sample_rate);
    std::vector<int16_t> out(len0 + len1);
    RenderNote(out, 0, len0, notes[0], sample_rate, false);
    RenderNote(out, len0, len1, notes[1], sample_rate, true);
    // 峰值压到 0.6（比中奖音轻，落空不必喧宾夺主）；
    // 整数除法截断换浮点比例，避免低幅尾部产生 3-4 级台阶量化噪声
    for (auto& x : out) x = (int16_t)lrintf((float)x * 0.75f);
    // 收尾归零：渐出窗口内正弦相位任意，最后样本可能停在 -18 级别微残值
    //（主机实测），统一从尾部倒数 24 样本线性压到精确 0
    for (size_t i = 0; i < 24 && i < out.size(); ++i) {
        float k = (float)(24 - i) / 24.0f;
        out[out.size() - 1 - i] = (int16_t)lrintf((float)out[out.size() - 1 - i] * k);
    }
    if (!out.empty()) out[out.size() - 1] = 0;
    return out;
}

}  // namespace speaker
