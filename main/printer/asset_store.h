// asset_store.h — 多图素材库（独立 media 分区，2026-10-26）
//
// 结构：定长槽 × 48，每槽 48,400B（= PrinterLogo::kMaxLogoBytes，最大 576×672 1bpp）。
//   槽头（64B）：magic "AST1" + id + len + crc32 + width + height + name[24] + 保留
//   槽体：LGO1 数据（8B 头 + 1bpp 栅格）
// 为什么定长：无需分配器/碎片整理；48 槽对 30 页足够，用空间换可靠性。
// 写入前只擦本槽 → 写 → 回读校验，失败不动旧数据（D15）。
#ifndef LUCKY_PAGE_PRINTER_ASSET_STORE_H_
#define LUCKY_PAGE_PRINTER_ASSET_STORE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace asset_store {

constexpr int kSlots = 48;
constexpr int kMaxNameBytes = 24;

struct Info {
    int id = -1;
    bool used = false;
    std::string name;
    int width = 0;
    int height = 0;
    size_t bytes = 0;
};

bool ValidId(int id);

// ---- 纯逻辑（单测宿主可测，不碰 flash）----
size_t SlotBytes();                    // 单槽字节数
size_t SlotOffset(int id);             // 槽在分区内的偏移（id 越界返回 0）
// 校验槽头：magic/长度/CRC/尺寸；成功时填 info 与 data_len
bool ParseSlotHeader(const uint8_t* hdr, size_t hdr_len, int expect_id,
                     Info* info, uint32_t* data_len);
// 打包槽头（out 至少 64B）
void PackSlotHeader(uint8_t* out, int id, size_t len, uint32_t crc,
                    int width, int height, const std::string& name);

// ---- Flash 操作 ----
bool Save(int id, const uint8_t* lgo1, size_t len, const std::string& name);
bool Load(int id, uint8_t* out, size_t out_cap, size_t* out_len);
bool Erase(int id);
Info ReadInfo(int id);
std::vector<Info> List();
int FindFree();                        // 返回第一个空槽，-1 = 满了
int Count();

}  // namespace asset_store

#endif  // LUCKY_PAGE_PRINTER_ASSET_STORE_H_
