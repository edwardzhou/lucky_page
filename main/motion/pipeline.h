#ifndef _PIPELINE_H_
#define _PIPELINE_H_

#include <memory>
#include <vector>

#include "motion/action.h"
#include "motion/action_group.h"
#include "motion/action_sequence.h"

/// 动作调度器：管理串行队列和并行组的执行。
///
/// 对应 Elixir Action.Pipeline。Pipeline 持有两套动作：
/// - schedule_（串行队列）：ActionSequence，按顺序执行
/// - parallels_（并行组）：ActionGroup，所有动作并行执行
///
/// Run() 执行流程：
/// 1. 驱动 parallels_ 并行组的 Update
/// 2. 驱动 schedule_ 串行队列的 Update
/// 3. 合并两者的 CellsMap 输出
///
/// 状态组合逻辑：kRunning > kWaiting > kDone
/// - 任何动作为 kRunning → 返回 kRunning
/// - 否则任何动作为 kWaiting → 返回 kWaiting
/// - 全部 kDone → 返回 kDone
class Pipeline {
public:
    Pipeline();

    /// 追加单个动作到串行队列末尾。
    void AddAction(std::unique_ptr<Action> action);

    /// 追加动作到串行队列末尾。
    void AddActions(std::vector<std::unique_ptr<Action>> actions);

    /// 替换整个串行队列。
    void Reschedule(std::vector<std::unique_ptr<Action>> actions);

    /// 追加动作到并行组末尾。
    void SpawnActions(std::vector<std::unique_ptr<Action>> actions);

    /// 替换整个并行组。
    void Reparallels(std::vector<std::unique_ptr<Action>> actions);

    /// 执行一个 tick：先 parallels 再 schedule，合并 CellsMap 结果。
    /// @param tick 距离 Pipeline 创建/重置的 tick 数
    /// @param cells_map 当前全局 DMX 通道状态，动作的输出会合并到此映射
    /// @return 组合后的执行状态
    ActionStatus Run(int tick, CellsMap& cells_map);

    /// 重置 Pipeline，清除所有动作。
    void Clear();

    /// 判断 Pipeline 是否已完成（schedule 和 parallels 都已完成）。
    bool IsDone() const;

private:
    std::unique_ptr<ActionSequence> schedule_;   // 串行队列
    std::unique_ptr<ActionGroup> parallels_;     // 并行组
};

#endif  // _PIPELINE_H_
