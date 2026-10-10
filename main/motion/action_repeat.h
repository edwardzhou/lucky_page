#ifndef _ACTION_REPEAT_H_
#define _ACTION_REPEAT_H_

#include <memory>

#include "motion/action.h"

/// 重复执行模式。
enum class RepeatMode : uint8_t {
    kCount = 0,    // 固定次数重复
    kForever = 1,  // 无限循环重复
};

/// 重复动作：将子动作重复执行指定次数或无限循环。
///
/// 每次子动作完成后，调用子动作的 Reset() 重置其状态，然后重新执行。
/// kCount 模式：达到指定次数后完成；kForever 模式：永不完成。
class ActionRepeat : public Action {
public:
    /// @param action 要重复执行的子动作
    /// @param count 重复次数（仅 kCount 模式有效）
    ActionRepeat(std::unique_ptr<Action> action, int count);
    ~ActionRepeat() override = default;

    /// 创建无限循环重复动作的便捷工厂方法。
    /// @param action 要无限重复的子动作
    static std::unique_ptr<ActionRepeat> Forever(
        std::unique_ptr<Action> action);

    ActionStatus Update(int tick, CellsMap& cells_map) override;
    void Reset() override;
    int WaitForMs() const override;
    std::unique_ptr<Action> Clone() const override;

private:
    std::unique_ptr<Action> action_;           // 子动作
    RepeatMode mode_ = RepeatMode::kCount;    // 重复模式
    int repeat_count_ = 0;                    // 目标重复次数（kCount 模式）
    int repeat_num_ = 0;                      // 当前已完成的重复次数
};

#endif  // _ACTION_REPEAT_H_
