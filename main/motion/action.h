#ifndef _ACTION_H_
#define _ACTION_H_

#include <memory>

#include "motion/action_types.h"
#include "motion/cell.h"

/// 所有动作的抽象基类。
///
/// 子类必须实现 Update（每帧驱动）、Reset（重置状态）和 WaitForMs（预估等待时间）。
/// is_done_ 由子类在动作完成时设为 true，之后 Pipeline 不再驱动该动作。
///
/// 线程安全：Action 的方法不是线程安全的，调用方（Pipeline/MotionEngine）负责同步。
class Action {
public:
    virtual ~Action() = default;

    /// 深拷贝动作，返回新实例。
    virtual std::unique_ptr<Action> Clone() const = 0;

    /// 驱动动作执行一帧。
    ///
    /// @param tick 距离动作开始的 tick 数（每个 tick 间隔 125ms）
    /// @param cells_map 当前全局 DMX 通道状态，Action 读取/修改此映射
    /// @return 当前执行状态
    virtual ActionStatus Update(int tick, CellsMap& cells_map) = 0;

    /// 重置动作到初始状态，使其可以重新执行。
    /// 通常由 ActionRepeat 在新一轮循环开始时调用。
    virtual void Reset() = 0;

    /// 返回该动作预估的等待时间（毫秒）。
    /// 用于 Pipeline 计算 waitForMs（所有并行动作的最大等待时间）。
    /// @return 等待毫秒数，0 表示无需等待
    virtual int WaitForMs() const = 0;

    /// 查询动作是否已完成。子类可重写以自定义完成逻辑。
    /// @return true 表示动作已结束，后续 Update 不再产生有意义输出
    virtual bool IsDone() const { return is_done_; }

protected:
    bool is_done_ = false;  // 子类在动作完成时设为 true
};

#endif  // _ACTION_H_
