#ifndef _PAGE_ADJUST_H_
#define _PAGE_ADJUST_H_

#include <cstdint>
#include <vector>

/// 翻页行程微调偏移管理。
/// 每 Cell 每页独立偏移值（int8_t），SPIFFS JSON 持久化。
/// 实际 DMX 值 = clamp(mapping[page] + offsets[cell][page], 0, 255)
class PageAdjust {
public:
    static PageAdjust& GetInstance();

    void LoadAdjust();
    void SaveAdjust();

    int8_t GetOffset(int cell, int page) const;
    void SetOffset(int cell, int page, int8_t offset);
    uint8_t GetAdjustedValue(int cell, int page) const;
    void ResetAll();
    void Resize(int cell_count, int total_pages);

    const std::vector<std::vector<int8_t>>& GetOffsets() const;

    PageAdjust(const PageAdjust&) = delete;
    PageAdjust& operator=(const PageAdjust&) = delete;

private:
    PageAdjust() = default;

    std::vector<std::vector<int8_t>> offsets_;
    bool loaded_ = false;
};

#endif  // _PAGE_ADJUST_H_
