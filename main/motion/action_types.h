#ifndef _ACTION_TYPES_H_
#define _ACTION_TYPES_H_

#include <cstdint>
#include <unordered_map>

// 前向声明
class Cell;

/// Action 单次 Update 的执行结果状态。
///
/// Pipeline 在合并多个并行动作的状态时使用以下优先级：
/// kRunning > kWaiting > kDone（只要有动作在运行，整体就是运行中）。
enum class ActionStatus : uint8_t {
    kRunning = 0,  // 动作正在执行（有中间状态变化）
    kWaiting = 1,  // 动作在等待（如延时未到期，不产生输出）
    kDone = 2,     // 动作已完成，后续 Update 不再产生有意义输出
};

/// DMX 起始通道号到 Cell 的映射表。
///
/// key: DMX 起始通道号 (1-512)
/// value: Cell 数据，每个 Cell 最多包含 kMaxChannels（18）个连续通道值
///
/// 多个 Action 的输出 CellsMap 会通过 Pipeline 合并：
/// 相同 key 的 Cell 使用 Cell::MergeFrom 的"非零值覆盖"语义。
using CellsMap = std::unordered_map<int, Cell>;

#endif  // _ACTION_TYPES_H_
