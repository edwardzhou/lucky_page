#define TAG "ActionRepeat"

#include "motion/action_repeat.h"

#include "esp_log.h"

ActionRepeat::ActionRepeat(std::unique_ptr<Action> action, int count)
    : action_(std::move(action)), repeat_count_(count) {
    // count < 0 表示无限循环，否则为固定次数
    mode_ = (count < 0) ? RepeatMode::kForever : RepeatMode::kCount;
}

std::unique_ptr<ActionRepeat> ActionRepeat::Forever(
    std::unique_ptr<Action> action) {
    return std::make_unique<ActionRepeat>(std::move(action), -1);
}

ActionStatus ActionRepeat::Update(int tick, CellsMap& cells_map) {
    if (is_done_ || !action_) {
        is_done_ = true;
        return ActionStatus::kDone;
    }

    ActionStatus result = action_->Update(tick, cells_map);

    if (action_->IsDone()) {
        repeat_num_++;
        if (mode_ == RepeatMode::kCount && repeat_num_ >= repeat_count_) {
            is_done_ = true;
            return ActionStatus::kDone;
        }
        action_->Reset();
    }

    return ActionStatus::kRunning;
}

void ActionRepeat::Reset() {
    is_done_ = false;
    repeat_num_ = 0;
    if (action_) {
        action_->Reset();
    }
}

int ActionRepeat::WaitForMs() const {
    if (is_done_ || !action_) return 0;
    return action_->WaitForMs();
}

std::unique_ptr<Action> ActionRepeat::Clone() const {
    auto cloned = std::make_unique<ActionRepeat>(action_->Clone(), repeat_count_);
    cloned->mode_ = mode_;
    return cloned;
}
