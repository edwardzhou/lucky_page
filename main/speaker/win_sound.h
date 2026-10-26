// win_sound.h — 中奖音效合成纯函数（2026-10-25）
// 程序生成上行琶音"胜利和弦"，无素材依赖；无 ESP-IDF 依赖，可上单测宿主。
#ifndef LUCKY_PAGE_SPEAKER_WIN_SOUND_H_
#define LUCKY_PAGE_SPEAKER_WIN_SOUND_H_

#include <cstdint>
#include <vector>

namespace speaker {

// 合成中奖提示音：C5(523Hz)→E5(659)→G5(784)→C6(1047) 各 130ms，
// 尾音 C6 延长 450ms 指数衰减；正弦 + 每音符 2ms 渐入渐出防爆音，
// 峰值 0.8 满幅。总时长 970ms。返回 16bit mono PCM（调用侧负责音量与 I2S 播放）。
std::vector<int16_t> SynthWinChime(uint32_t sample_rate);

// 合成落空提示音：E4(330Hz) 150ms → C4(262Hz) 320ms 指数衰减的下行双音，
// 低沉短促（与中奖上行琶音对比鲜明），峰值 0.6（比中奖音轻）。总时长 470ms。
std::vector<int16_t> SynthLoseChime(uint32_t sample_rate);

}  // namespace speaker

#endif  // LUCKY_PAGE_SPEAKER_WIN_SOUND_H_
