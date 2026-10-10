#ifndef _ACTION_PAGING_H_
#define _ACTION_PAGING_H_

#include <cstdint>

#include "motion/action.h"

/// 翻页动作：一次性设置 Cell 的多个通道值。
///
/// 在首次 Update 时将 page 和所有通道值写入 cells_map 对应的 Cell，
/// 然后立即标记完成（单帧动作）。
///
/// 支持可配置的 DMX 通道数（paging_channel_count）：
/// - 7 通道：直接按序写入 ch0-ch6
/// - 3 通道：映射写入 ch0=行程, ch1=速度(ch3), ch2=重置(ch7)
class ActionPaging : public Action {
public:
    /// @param cell_id DMX 起始通道号
    /// @param page 翻页编号（写入通道偏移 0）
    /// @param page_value page 对应的 DMX 值
    /// @param ch02 通道 2 的值
    /// @param ch03 通道 3 的值（速度）
    /// @param ch04 通道 4 的值
    /// @param ch05 通道 5 的值
    /// @param ch06 通道 6 的值
    /// @param ch07 通道 7 的值（重置）
    /// @param paging_channel_count 活跃 DMX 通道数（3 或 7），默认 7
    ActionPaging(int cell_id, int page, uint8_t page_value,
                 uint8_t ch02 = 0, uint8_t ch03 = 0, uint8_t ch04 = 0,
                 uint8_t ch05 = 0, uint8_t ch06 = 0, uint8_t ch07 = 0,
                 int paging_channel_count = 7);
    ~ActionPaging() override = default;

    std::unique_ptr<Action> Clone() const override;
    ActionStatus Update(int tick, CellsMap& cells_map) override;
    void Reset() override;
    int WaitForMs() const override;

private:
    int cell_id_;                  // DMX 起始通道号
    int page_;                     // 翻页编号
    int paging_channel_count_;     // 活跃 DMX 通道数（3 或 7）
    uint8_t channels_[7] = {0};    // 7 个通道值（内部始终存储完整 7 通道）
};

#endif  // _ACTION_PAGING_H_
