#ifndef _ACTION_DELAY_H_
#define _ACTION_DELAY_H_

#include "motion/action.h"

/// 延时等待动作。
///
/// 在指定毫秒数内返回 kWaiting 状态，不产生任何 DMX 输出。
/// 到期后返回 kDone。常用于 ActionSequence 中步骤之间的间隔。
///
/// 注意：延时精度取决于 MotionEngine 的 tick 间隔（默认 125ms），
/// 实际延时可能略长于指定值。
class ActionDelay : public Action {
public:
    /// @param millis 延时等待的毫秒数
    explicit ActionDelay(int millis) : millis_(millis), elapsed_(0) {}

    std::unique_ptr<Action> Clone() const override;
    ActionStatus Update(int tick, CellsMap& cells_map) override;
    void Reset() override;
    int WaitForMs() const override;

private:
    int millis_ = 0;       // 目标延时毫秒数
    int elapsed_ = 0;      // 已经过的毫秒数
};

#endif  // _ACTION_DELAY_H_
