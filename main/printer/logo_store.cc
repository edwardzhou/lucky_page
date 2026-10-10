#include "printer/logo_store.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

#define TAG "LogoStore"

namespace {

constexpr uint32_t kMagic = 0x4F474F4Cu;   // "LOGO"
constexpr size_t kHeaderBytes = 12;        // magic + len + crc32
const char* kPartName = "logo";
const esp_partition_t* s_part = nullptr;

const esp_partition_t* Part() {
    if (s_part == nullptr) {
        s_part = esp_partition_find_first((esp_partition_type_t)ESP_PARTITION_TYPE_DATA,
                                         (esp_partition_subtype_t)0x40, kPartName);
        if (s_part == nullptr) {
            ESP_LOGE(TAG, "找不到 logo 分区（分区表缺少 logo 项？）");
        } else {
            ESP_LOGI(TAG, "素材分区就绪：%s @0x%lx %luB", s_part->label,
                     (unsigned long)s_part->address, (unsigned long)s_part->size);
        }
    }
    return s_part;
}

/// 48KB 级别的校验缓冲：**绝不能放栈上**（HTTP 任务栈仅几 KB；
/// 真机事故：栈溢出把 USB 外设一起拖死，串口都不出字）。统一放 PSRAM。
uint8_t* Scratch() {
    static uint8_t* p = nullptr;
    if (p == nullptr) {
        p = (uint8_t*)heap_caps_malloc(PrinterLogo::kMaxLogoBytes, MALLOC_CAP_SPIRAM);
        if (p == nullptr) ESP_LOGE(TAG, "PSRAM 分配 %d 字节失败",
                                  (int)PrinterLogo::kMaxLogoBytes);
    }
    return p;
}

uint32_t Crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

void PutU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

uint32_t GetU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/// 读头部并校验；返回数据长度（0 = 无效/不存在）
size_t ReadValidLength() {
    const esp_partition_t* part = Part();
    if (part == nullptr) return 0;
    uint8_t hdr[kHeaderBytes] = {0};
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK) return 0;
    if (GetU32(hdr) != kMagic) return 0;                 // 空分区（0xFF）也走这里
    const uint32_t len = GetU32(hdr + 4);
    const uint32_t crc = GetU32(hdr + 8);
    if (len == 0 || len > (uint32_t)PrinterLogo::kMaxLogoBytes) return 0;
    if (kHeaderBytes + len > part->size) return 0;
    uint8_t* buf = Scratch();
    if (buf == nullptr) return 0;
    if (esp_partition_read(part, kHeaderBytes, buf, len) != ESP_OK) return 0;
    if (Crc32(buf, len) != crc) {
        ESP_LOGW(TAG, "素材 CRC 校验失败（掉电写坏？）");
        return 0;
    }
    return len;
}

}  // namespace

bool LogoStore::Save(const uint8_t* buf, size_t len) {
    const esp_partition_t* part = Part();
    if (part == nullptr || buf == nullptr || len == 0 ||
        len > (size_t)PrinterLogo::kMaxLogoBytes || kHeaderBytes + len > part->size) {
        ESP_LOGE(TAG, "Save: 参数非法 (len=%zu)", len);
        return false;
    }
    // 擦除覆盖范围（按 4KB 扇区对齐）
    const size_t need = kHeaderBytes + len;
    const size_t erase = ((need + 4095) / 4096) * 4096;
    if (esp_partition_erase_range(part, 0, erase) != ESP_OK) {
        ESP_LOGE(TAG, "擦除失败（保留旧数据）");
        return false;
    }
    uint8_t hdr[kHeaderBytes];
    PutU32(hdr, kMagic);
    PutU32(hdr + 4, (uint32_t)len);
    PutU32(hdr + 8, Crc32(buf, len));
    if (esp_partition_write(part, 0, hdr, sizeof(hdr)) != ESP_OK ||
        esp_partition_write(part, kHeaderBytes, buf, len) != ESP_OK) {
        ESP_LOGE(TAG, "写入失败");
        return false;
    }
    // 回读校验（写坏就当场发现，而不是打印时才发现）
    if (ReadValidLength() != len) {
        ESP_LOGE(TAG, "写入后校验失败");
        return false;
    }
    ESP_LOGI(TAG, "素材已保存: %zu B（独立分区，烧固件不影响）", len);
    return true;
}

bool LogoStore::Load(uint8_t* out, size_t out_cap, size_t* out_len) {
    if (out == nullptr || out_len == nullptr) return false;
    const esp_partition_t* part = Part();
    if (part == nullptr) return false;
    const size_t len = ReadValidLength();
    if (len == 0 || out_cap < len) return false;
    if (esp_partition_read(part, kHeaderBytes, out, len) != ESP_OK) return false;
    *out_len = len;
    return true;
}

bool LogoStore::Remove() {
    const esp_partition_t* part = Part();
    if (part == nullptr) return false;
    const size_t erase = ((kHeaderBytes + PrinterLogo::kMaxLogoBytes + 4095) / 4096) * 4096;
    const size_t sz = erase < part->size ? erase : part->size;
    if (esp_partition_erase_range(part, 0, sz) != ESP_OK) return false;
    ESP_LOGI(TAG, "素材已删除");
    return true;
}

bool LogoStore::Exists() { return ReadValidLength() > 0; }

LogoInfo LogoStore::ReadInfo() {
    LogoInfo info;
    const esp_partition_t* part = Part();
    if (part == nullptr) {
        info.error = "无 logo 分区";
        return info;
    }
    uint8_t* buf = Scratch();
    if (buf == nullptr) {
        info.error = "内存不足";
        return info;
    }
    size_t len = 0;
    if (!Load(buf, (size_t)PrinterLogo::kMaxLogoBytes, &len)) {
        info.error = "未上传或数据损坏";
        return info;
    }
    return PrinterLogo::Validate(buf, len);
}

void LogoStore::MigrateFromSpiffsIfNeeded() {
    if (Exists()) return;                        // 新分区已有 → 不动
    FILE* f = fopen(kLegacyPath, "rb");
    if (f == nullptr) return;                    // 没有旧图 → 无需迁移
    uint8_t* buf = Scratch();
    if (buf == nullptr) return;
    const size_t n = fread(buf, 1, (size_t)PrinterLogo::kMaxLogoBytes, f);
    fclose(f);
    if (n == 0) return;
    if (Save(buf, n)) {
        ESP_LOGI(TAG, "已把旧 SPIFFS 里的素材迁移到独立分区（%zu B）", n);
    }
}
