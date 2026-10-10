#ifndef _ACTION_SEQUENCE_H_
#define _ACTION_SEQUENCE_H_

#include <memory>
#include <vector>

#include "motion/action.h"

/// 串行动作序列。
///
/// 按顺序执行 steps 中的子动作。每个 tick 驱动当前步骤，
/// 若当前步骤立即完成（返回 kDone），则移除该步骤并继续下一步，
/// 直到某个步骤返回 kWaiting 或 kRunning 时停止。
/// 所有步骤完成后序列结束。
///
/// 已完成的步骤会被自动移除，避免内存无限增长。
class ActionSequence : public Action {
public:
    /// @param steps 子动作列表，按执行顺序排列
    /// @param keep_steps 是否保留原始步骤用于 Reset/Clone
    explicit ActionSequence(std::vector<std::unique_ptr<Action>> steps,
                            bool keep_steps = true);

    /// 追加子动作到序列末尾。如果序列已完成，重置为未完成。
    void Append(std::vector<std::unique_ptr<Action>> steps);

    ActionStatus Update(int tick, CellsMap& cells_map) override;
    void Reset() override;
    int WaitForMs() const override;
    std::unique_ptr<Action> Clone() const override;

private:
    std::vector<std::unique_ptr<Action>> steps_;
    std::vector<std::unique_ptr<Action>> org_steps_;
    bool keep_steps_;
};

#endif  // _ACTION_SEQUENCE_H_
