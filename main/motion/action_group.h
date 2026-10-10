#ifndef _ACTION_GROUP_H_
#define _ACTION_GROUP_H_

#include <memory>
#include <vector>

#include "motion/action.h"

/// 并行动作组。
///
/// 每个 tick 同时驱动所有未完成的子动作，将它们的输出 cells_map 合并。
/// 合并规则：相同 key 的 Cell 使用 Cell::MergeFrom 的"非零值覆盖"语义。
/// 所有子动作完成后 Group 标记完成。
///
/// 状态组合逻辑：kRunning > kWaiting > kDone
/// - 任何一个子动作为 kRunning → Group 返回 kRunning
/// - 否则任何一个为 kWaiting → Group 返回 kWaiting
/// - 全部 kDone → Group 返回 kDone
class ActionGroup : public Action {
public:
    /// @param children 子动作列表，所有子动作并行执行
    /// @param keep_steps 是否保留原始子动作用于 Reset/Clone
    explicit ActionGroup(std::vector<std::unique_ptr<Action>> children,
                         bool keep_steps = true);

    /// 追加子动作到并行组。如果组已完成，重置为未完成。
    void Append(std::vector<std::unique_ptr<Action>> children);

    ActionStatus Update(int tick, CellsMap& cells_map) override;
    void Reset() override;
    int WaitForMs() const override;
    std::unique_ptr<Action> Clone() const override;

private:
    std::vector<std::unique_ptr<Action>> children_;
    std::vector<std::unique_ptr<Action>> org_children_;
    bool keep_steps_;
};

#endif  // _ACTION_GROUP_H_
