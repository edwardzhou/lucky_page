#define TAG "ActionDelay"

#include "motion/action_delay.h"

#include <memory>

#include "esp_log.h"

std::unique_ptr<Action> ActionDelay::Clone() const {
    return std::make_unique<ActionDelay>(millis_);
}

ActionStatus ActionDelay::Update(int tick, CellsMap& cells_map) {
    if (is_done_) {
        return ActionStatus::kDone;
    }

    elapsed_ += tick;

    if (elapsed_ >= millis_) {
        is_done_ = true;
        return ActionStatus::kDone;
    }

    return ActionStatus::kWaiting;
}

void ActionDelay::Reset() {
    elapsed_ = 0;
    is_done_ = false;
}

int ActionDelay::WaitForMs() const {
    if (is_done_ || millis_ <= elapsed_) {
        return 0;
    }
    return millis_ - elapsed_;
}
