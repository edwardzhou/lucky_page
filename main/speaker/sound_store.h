// sound_store.h — 自定义音效存储（media 分区尾部，2026-10-26）
// 布局：图片 48 槽（48KiB）之后，3 个音效槽 × 192KiB：
//   role 0=欢迎 1=中奖 2=落空；槽头 32B：magic "SND1" + role + len + crc + rate
// 存的是 24kHz 单声道 16bit PCM（上传时已解码/重采样）。
#ifndef LUCKY_PAGE_SPEAKER_SOUND_STORE_H_
#define LUCKY_PAGE_SPEAKER_SOUND_STORE_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace sound_store {

constexpr int kRoles = 3;                       // welcome / win / lose
constexpr int kWelcome = 0;
constexpr int kWin = 1;
constexpr int kLose = 2;
constexpr size_t kSlotBytes = 192 * 1024;       // 192KiB ≈ 4.0s @24k mono 16bit
constexpr uint32_t kRate = 24000;

struct Info {
    bool custom = false;
    uint32_t bytes = 0;
    uint32_t duration_ms = 0;
};

const char* RoleName(int role);          // "welcome"/"win"/"lose"，非法返回 nullptr
int RoleFromName(const std::string& s);  // -1 = 非法

// ---- 纯逻辑（单测）----
size_t SlotOffset(int role);             // media 分区内偏移；非法 role 返回 0
bool ValidRole(int role);
bool ParseHeader(const uint8_t* hdr, size_t hdr_len, int expect_role, Info* info,
                 uint32_t* data_len);
void PackHeader(uint8_t* out, int role, uint32_t len, uint32_t crc);

// ---- Flash ----
bool Save(int role, const int16_t* pcm, size_t samples);
bool Load(int role, uint8_t* out, size_t out_cap, size_t* out_len);
bool Erase(int role);
Info ReadInfo(int role);

}  // namespace sound_store

#endif  // LUCKY_PAGE_SPEAKER_SOUND_STORE_H_
