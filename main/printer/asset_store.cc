// asset_store.cc — 多图素材库实现（media 分区，定长槽）
#include "printer/asset_store.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "printer/printer_logo.h"

#include <cstdio>
#include <sys/stat.h>

#include "storage/sd_card.h"
#include "storage/sd_paths.h"

#define TAG "AssetStore"

namespace asset_store {
namespace {

constexpr uint32_t kMagic = 0x31545341;   // "AST1"（小端）
constexpr size_t kHeaderBytes = 64;
const char* kPartName = "media";
const esp_partition_t* s_part = nullptr;
const esp_partition_t* kMissing = nullptr;

const esp_partition_t* Part() {
    if (s_part == nullptr) {
        const esp_partition_t* p =
            esp_partition_find_first((esp_partition_type_t)ESP_PARTITION_TYPE_DATA,
                                     (esp_partition_subtype_t)0x41, kPartName);
        if (p == nullptr) {
            ESP_LOGE(TAG, "找不到 media 分区（分区表缺少 media 项？）");
            s_part = kMissing;   // 避免每次重复查找/刷日志
        } else {
            ESP_LOGI(TAG, "素材库分区就绪：%s @0x%lx %luB（%d 槽）", p->label,
                     (unsigned long)p->address, (unsigned long)p->size, kSlots);
            s_part = p;
        }
    }
    return s_part == kMissing ? nullptr : s_part;
}

uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
void Wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
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

/// 校验/写入用暂存：48KB 级缓冲绝不放栈上（HTTP 任务栈仅几 KB）
uint8_t* Scratch() {
    static uint8_t* p = nullptr;
    if (p == nullptr) {
        p = (uint8_t*)heap_caps_malloc(PrinterLogo::kMaxLogoBytes, MALLOC_CAP_SPIRAM);
        if (p == nullptr) ESP_LOGE(TAG, "PSRAM 分配 %d 字节失败",
                                  (int)PrinterLogo::kMaxLogoBytes);
    }
    return p;
}

// ---- SD 卡存储（优先；2026-10-28）----
bool SdReady() { return sd_card::IsMounted(); }

void EnsureDirs() {
    mkdir(sd_paths::kRoot, 0777);
    mkdir("/sdcard/lucky", 0777);
    mkdir(sd_paths::kAssetDir, 0777);
    mkdir(sd_paths::kSoundDir, 0777);
}

bool SdAssetExists(int id) {
    std::string path = sd_paths::AssetFile(id);
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// 名称侧车文件（FAT 无扩展属性）：<id>.name，纯文本 ≤24 字节
std::string SdNamePath(int id) { return sd_paths::AssetFile(id) + ".name"; }

void SdWriteName(int id, const std::string& name) {
    FILE* f = fopen(SdNamePath(id).c_str(), "wb");
    if (f == nullptr) return;
    fwrite(name.data(), 1, name.size() > (size_t)kMaxNameBytes ? (size_t)kMaxNameBytes : name.size(), f);
    fclose(f);
}

std::string SdReadName(int id) {
    FILE* f = fopen(SdNamePath(id).c_str(), "rb");
    if (f == nullptr) return "";
    char buf[kMaxNameBytes + 1] = {};
    size_t n = fread(buf, 1, kMaxNameBytes, f);
    fclose(f);
    return std::string(buf, n);
}

/// SD 上保存（写临时文件再改名，避免半写；FAT rename 需先删目标）
bool SdSave(int id, const uint8_t* lgo1, size_t len, const std::string& name) {
    EnsureDirs();
    std::string path = sd_paths::AssetFile(id);
    if (path.empty()) return false;
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    bool ok = fwrite(lgo1, 1, len, f) == len;
    fclose(f);
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    remove(path.c_str());
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    SdWriteName(id, name);
    return true;
}

namespace {
// 只清 flash 槽（SD 写入成功后清旧副本用；不能动 SD 上的新文件）
bool ClearFlashSlot(int id) {
    const esp_partition_t* p = Part();
    if (p == nullptr) return false;
    return esp_partition_erase_range(p, SlotOffset(id), SlotBytes()) == ESP_OK;
}
}  // namespace

}  // namespace

bool ValidId(int id) { return id >= 0 && id < kSlots; }

// ⚠ 槽大小必须按 flash 扇区（4KB）对齐：esp_partition_erase_range 的 size
// 必须为扇区整数倍，否则返回 ESP_ERR_INVALID_ARG（真机 500 事故 2026-10-26）。
// 48,392 → 上取整 49,152（48KiB，12 个扇区）；48 槽共 2.25MB < 2.875MB 分区。
size_t SlotBytes() {
    constexpr size_t kSector = 4096;
    return ((PrinterLogo::kMaxLogoBytes + kSector - 1) / kSector) * kSector;
}

size_t SlotOffset(int id) {
    if (!ValidId(id)) return 0;
    return (size_t)id * SlotBytes();
}

void PackSlotHeader(uint8_t* out, int id, size_t len, uint32_t crc, int width,
                    int height, const std::string& name) {
    memset(out, 0, kHeaderBytes);
    Wr32(out, kMagic);
    Wr32(out + 4, (uint32_t)id);
    Wr32(out + 8, (uint32_t)len);
    Wr32(out + 12, crc);
    Wr16(out + 16, (uint16_t)width);
    Wr16(out + 18, (uint16_t)height);
    size_t n = name.size() < (size_t)kMaxNameBytes ? name.size() : (size_t)kMaxNameBytes;
    memcpy(out + 20, name.data(), n);
}

bool ParseSlotHeader(const uint8_t* hdr, size_t hdr_len, int expect_id, Info* info,
                     uint32_t* data_len) {
    if (hdr == nullptr || hdr_len < kHeaderBytes) return false;
    if (Rd32(hdr) != kMagic) return false;
    int id = (int)Rd32(hdr + 4);
    if (id != expect_id) return false;
    uint32_t len = Rd32(hdr + 8);
    if (len < 8 || len > PrinterLogo::kMaxLogoBytes - kHeaderBytes) return false;
    int w = Rd16(hdr + 16);
    int h = Rd16(hdr + 18);
    char name[kMaxNameBytes + 1] = {};
    memcpy(name, hdr + 20, kMaxNameBytes);
    if (info != nullptr) {
        info->id = id;
        info->used = true;
        info->name = name;
        info->width = w;
        info->height = h;
        info->bytes = len;
    }
    if (data_len != nullptr) *data_len = len;
    return true;
}

bool Save(int id, const uint8_t* lgo1, size_t len, const std::string& name) {
    if (!ValidId(id)) return false;
    if (lgo1 == nullptr || len < 8 || len > PrinterLogo::kMaxLogoBytes) return false;
    if (SdReady()) {
        if (!SdSave(id, lgo1, len, name)) {
            ESP_LOGW(TAG, "SD 写入失败（id=%d）→ 回落 flash 槽", id);
        } else {
            ClearFlashSlot(id);        // 只清 flash 旧副本（避免无卡时旧图复活）
            return true;
        }
    }
    const esp_partition_t* p = Part();
    if (p == nullptr) return false;
    if (SlotOffset(id) + SlotBytes() > p->size) {
        ESP_LOGE(TAG, "槽 %d 超出分区", id);
        return false;
    }
    uint8_t* buf = Scratch();
    if (buf == nullptr) return false;
    // LGO1 头：magic + width(u16) + height(u16)，见 printer_logo.cc
    int w = (int)Rd16(lgo1 + 4);
    int h = (int)Rd16(lgo1 + 6);
    memset(buf, 0, SlotBytes());
    PackSlotHeader(buf, id, len, Crc32(lgo1, len), w, h, name);
    memcpy(buf + kHeaderBytes, lgo1, len);
    size_t off = SlotOffset(id);
    if (esp_partition_erase_range(p, off, SlotBytes()) != ESP_OK) {
        ESP_LOGE(TAG, "擦除槽 %d 失败", id);
        return false;
    }
    if (esp_partition_write(p, off, buf, SlotBytes()) != ESP_OK) {
        ESP_LOGE(TAG, "写入槽 %d 失败", id);
        return false;
    }
    // 回读校验（D15：失败也不破坏其它槽；本槽可能半写，但 next Save 会重擦）
    memset(buf, 0, kHeaderBytes);
    if (esp_partition_read(p, off, buf, kHeaderBytes) != ESP_OK) return false;
    Info chk;
    uint32_t dl = 0;
    if (!ParseSlotHeader(buf, kHeaderBytes, id, &chk, &dl) || dl != len) {
        ESP_LOGE(TAG, "槽 %d 回读校验失败", id);
        return false;
    }
    return true;
}

bool Load(int id, uint8_t* out, size_t out_cap, size_t* out_len) {
    if (!ValidId(id) || out == nullptr) return false;
    if (SdReady() && SdAssetExists(id)) {
        std::string path = sd_paths::AssetFile(id);
        FILE* f = fopen(path.c_str(), "rb");
        if (f != nullptr) {
            size_t n = fread(out, 1, out_cap, f);
            fclose(f);
            if (n >= 8 && PrinterLogo::Validate(out, n).valid) {
                if (out_len != nullptr) *out_len = n;
                return true;
            }
            ESP_LOGW(TAG, "SD 素材 %d 校验失败 → 尝试 flash 副本", id);
        }
    }
    const esp_partition_t* p = Part();
    if (p == nullptr) return false;
    size_t off = SlotOffset(id);
    uint8_t hdr[kHeaderBytes];
    if (esp_partition_read(p, off, hdr, sizeof(hdr)) != ESP_OK) return false;
    Info info;
    uint32_t len = 0;
    if (!ParseSlotHeader(hdr, sizeof(hdr), id, &info, &len) || len > out_cap) return false;
    if (esp_partition_read(p, off + kHeaderBytes, out, len) != ESP_OK) return false;
    if (Crc32(out, len) != Rd32(hdr + 12)) {
        ESP_LOGW(TAG, "槽 %d CRC 校验失败", id);
        return false;
    }
    if (out_len != nullptr) *out_len = len;
    return true;
}

bool Erase(int id) {
    if (!ValidId(id)) return false;
    if (SdReady()) {
        remove(sd_paths::AssetFile(id).c_str());
        remove(SdNamePath(id).c_str());
    }
    return ClearFlashSlot(id);
}

Info ReadInfo(int id) {
    Info info;
    if (!ValidId(id)) return info;
    if (SdReady() && SdAssetExists(id)) {
        std::string path = sd_paths::AssetFile(id);
        FILE* f = fopen(path.c_str(), "rb");
        if (f != nullptr) {
            uint8_t hdr[8];
            size_t n = fread(hdr, 1, sizeof(hdr), f);
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fclose(f);
            if (n == 8 && memcmp(hdr, "LGO1", 4) == 0 && len > 8) {
                info.id = id;
                info.used = true;
                info.name = SdReadName(id);
                info.width = (int)(hdr[4] | (hdr[5] << 8));
                info.height = (int)(hdr[6] | (hdr[7] << 8));
                info.bytes = (size_t)len;
                return info;
            }
        }
    }
    const esp_partition_t* p = Part();
    if (p == nullptr) return info;
    uint8_t hdr[kHeaderBytes];
    if (esp_partition_read(p, SlotOffset(id), hdr, sizeof(hdr)) != ESP_OK) return info;
    uint32_t len = 0;
    if (!ParseSlotHeader(hdr, sizeof(hdr), id, &info, &len)) return Info{};
    return info;
}

std::vector<Info> List() {
    std::vector<Info> out;
    for (int i = 0; i < kSlots; ++i) {
        Info info = ReadInfo(i);
        if (info.used) out.push_back(info);
    }
    return out;
}

int FindFree() {
    for (int i = 0; i < kSlots; ++i) {
        if (!ReadInfo(i).used) return i;
    }
    return -1;
}

int Count() {
    int n = 0;
    for (const auto& it : List()) n++;
    return n;
}

}  // namespace asset_store
