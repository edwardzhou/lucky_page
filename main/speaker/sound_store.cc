// sound_store.cc — 自定义音效存储实现
#include "speaker/sound_store.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "printer/asset_store.h"

#include <cstdio>
#include <sys/stat.h>

#include "storage/sd_card.h"
#include "storage/sd_paths.h"

#define TAG "SoundStore"

namespace sound_store {
namespace {

constexpr uint32_t kMagic = 0x31444E53;   // "SND1"（小端）
constexpr size_t kHeaderBytes = 32;
const char* kPartName = "media";
const esp_partition_t* s_part = nullptr;
const esp_partition_t* kMissing = nullptr;

const esp_partition_t* Part() {
    if (s_part == nullptr) {
        const esp_partition_t* p =
            esp_partition_find_first((esp_partition_type_t)ESP_PARTITION_TYPE_DATA,
                                     (esp_partition_subtype_t)0x41, kPartName);
        s_part = (p == nullptr) ? kMissing : p;
        if (p == nullptr) ESP_LOGE(TAG, "找不到 media 分区（音效存储不可用）");
    }
    return s_part == kMissing ? nullptr : s_part;
}

uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
void Wr32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

uint32_t Crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool SdReady() { return sd_card::IsMounted(); }

void EnsureSoundDir() {
    mkdir(sd_paths::kRoot, 0777);
    mkdir("/sdcard/lucky", 0777);
    mkdir(sd_paths::kSoundDir, 0777);
}

}  // namespace

const char* RoleName(int role) {
    switch (role) {
        case kWelcome: return "welcome";
        case kWin: return "win";
        case kLose: return "lose";
        default: return nullptr;
    }
}

int RoleFromName(const std::string& s) {
    if (s == "welcome") return kWelcome;
    if (s == "win") return kWin;
    if (s == "lose") return kLose;
    return -1;
}

bool ValidRole(int role) { return role >= 0 && role < kRoles; }

size_t SlotOffset(int role) {
    if (!ValidRole(role)) return 0;
    // 音效区在图片 48 槽之后（asset_store::SlotBytes() × kSlots）
    const size_t base = asset_store::SlotBytes() * (size_t)asset_store::kSlots;
    return base + (size_t)role * kSlotBytes;
}

void PackHeader(uint8_t* out, int role, uint32_t len, uint32_t crc) {
    memset(out, 0, kHeaderBytes);
    Wr32(out, kMagic);
    Wr32(out + 4, (uint32_t)role);
    Wr32(out + 8, len);
    Wr32(out + 12, crc);
    Wr32(out + 16, kRate);
    Wr32(out + 20, len / 2 * 1000 / kRate);   // 时长 ms
}

bool ParseHeader(const uint8_t* hdr, size_t hdr_len, int expect_role, Info* info,
                 uint32_t* data_len) {
    if (hdr == nullptr || hdr_len < kHeaderBytes) return false;
    if (Rd32(hdr) != kMagic) return false;
    int role = (int)Rd32(hdr + 4);
    if (role != expect_role) return false;
    uint32_t len = Rd32(hdr + 8);
    if (len == 0 || len > kSlotBytes - kHeaderBytes || (len & 1) != 0) return false;
    if (info != nullptr) {
        info->custom = true;
        info->bytes = len;
        info->duration_ms = Rd32(hdr + 20);
    }
    if (data_len != nullptr) *data_len = len;
    return true;
}

namespace {
bool ClearFlashRole(int role) {
    const esp_partition_t* p = Part();
    if (p == nullptr) return false;
    return esp_partition_erase_range(p, SlotOffset(role), kSlotBytes) == ESP_OK;
}
}  // namespace

bool Save(int role, const int16_t* pcm, size_t samples) {
    if (!ValidRole(role) || pcm == nullptr || samples == 0) return false;
    const size_t len = samples * 2;
    if (len > kSlotBytes - kHeaderBytes) {
        ESP_LOGW(TAG, "音效过长：%u 字节 > %u", (unsigned)len,
                 (unsigned)(kSlotBytes - kHeaderBytes));
        return false;
    }
    if (SdReady()) {
        EnsureSoundDir();
        std::string path = sd_paths::SoundFile(RoleName(role));
        std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f != nullptr) {
            bool ok = fwrite(pcm, 1, len, f) == len;
            fclose(f);
            if (ok) {
                remove(path.c_str());       // FAT rename 不覆盖已存在目标
                ok = rename(tmp.c_str(), path.c_str()) == 0;
            }
            if (ok) {
                ClearFlashRole(role);   // 只清 flash 旧副本（不能删刚写的 SD 文件）
                return true;
            }
            remove(tmp.c_str());
            ESP_LOGW(TAG, "SD 写入音效失败 → 回落 flash");
        }
    }
    const esp_partition_t* p = Part();
    if (p == nullptr) return false;
    if (SlotOffset(role) + kSlotBytes > p->size) {
        ESP_LOGE(TAG, "音效槽超出分区（offset=%u size=%u part=%u）",
                 (unsigned)SlotOffset(role), (unsigned)kSlotBytes, (unsigned)p->size);
        return false;
    }
    static uint8_t* buf = nullptr;
    if (buf == nullptr) buf = (uint8_t*)heap_caps_malloc(kSlotBytes, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) return false;
    memset(buf, 0, kSlotBytes);
    PackHeader(buf, role, (uint32_t)len, Crc32((const uint8_t*)pcm, len));
    memcpy(buf + kHeaderBytes, pcm, len);
    const size_t off = SlotOffset(role);
    if (esp_partition_erase_range(p, off, kSlotBytes) != ESP_OK) {
        ESP_LOGE(TAG, "擦除音效槽 %d 失败", role);
        return false;
    }
    if (esp_partition_write(p, off, buf, kSlotBytes) != ESP_OK) {
        ESP_LOGE(TAG, "写入音效槽 %d 失败", role);
        return false;
    }
    return true;
}

bool Load(int role, uint8_t* out, size_t out_cap, size_t* out_len) {
    if (!ValidRole(role) || out == nullptr) return false;
    if (SdReady()) {
        std::string path = sd_paths::SoundFile(RoleName(role));
        FILE* f = fopen(path.c_str(), "rb");
        if (f != nullptr) {
            size_t n = fread(out, 1, out_cap, f);
            fclose(f);
            if (n > 0 && (n & 1) == 0) {
                if (out_len != nullptr) *out_len = n;
                return true;
            }
        }
    }
    const esp_partition_t* p = Part();
    if (p == nullptr) return false;
    const size_t off = SlotOffset(role);
    uint8_t hdr[kHeaderBytes];
    if (esp_partition_read(p, off, hdr, sizeof(hdr)) != ESP_OK) return false;
    uint32_t len = 0;
    if (!ParseHeader(hdr, sizeof(hdr), role, nullptr, &len) || len > out_cap) return false;
    if (esp_partition_read(p, off + kHeaderBytes, out, len) != ESP_OK) return false;
    if (Crc32(out, len) != Rd32(hdr + 12)) {
        ESP_LOGW(TAG, "音效槽 %d CRC 校验失败", role);
        return false;
    }
    if (out_len != nullptr) *out_len = len;
    return true;
}

bool Erase(int role) {
    if (!ValidRole(role)) return false;
    if (SdReady()) remove(sd_paths::SoundFile(RoleName(role)).c_str());
    const esp_partition_t* p = Part();
    if (p == nullptr) return false;
    return esp_partition_erase_range(p, SlotOffset(role), kSlotBytes) == ESP_OK;
}

Info ReadInfo(int role) {
    Info info;
    if (!ValidRole(role)) return info;
    if (SdReady()) {
        struct stat st;
        std::string path = sd_paths::SoundFile(RoleName(role));
        if (!path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_size > 0 && st.st_size <= (off_t)kSlotBytes) {
            info.custom = true;
            info.bytes = (uint32_t)st.st_size;
            info.duration_ms = (uint32_t)((uint64_t)st.st_size / 2 * 1000 / kRate);
            return info;
        }
    }
    const esp_partition_t* p = Part();
    if (p == nullptr) return info;
    uint8_t hdr[kHeaderBytes];
    if (esp_partition_read(p, SlotOffset(role), hdr, sizeof(hdr)) != ESP_OK) return info;
    if (!ParseHeader(hdr, sizeof(hdr), role, &info, nullptr)) return Info{};
    return info;
}

}  // namespace sound_store
