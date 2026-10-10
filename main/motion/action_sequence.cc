#define TAG "ActionSequence"

#include "motion/action_sequence.h"

#include "esp_log.h"

ActionSequence::ActionSequence(std::vector<std::unique_ptr<Action>> steps,
                                 bool keep_steps)
    : keep_steps_(keep_steps) {
    if (keep_steps_) {
        for (auto& step : steps) {
            org_steps_.push_back(step->Clone());
        }
    }
    steps_ = std::move(steps);
    if (steps_.empty()) {
        is_done_ = true;
    }
}

void ActionSequence::Append(std::vector<std::unique_ptr<Action>> steps) {
    is_done_ = false;
    for (auto& step : steps) {
        if (keep_steps_) {
            org_steps_.push_back(step->Clone());
        }
        steps_.push_back(std::move(step));
    }
}

ActionStatus ActionSequence::Update(int tick, CellsMap& cells_map) {
    if (is_done_ || steps_.empty()) {
        is_done_ = true;
        return ActionStatus::kDone;
    }

    while (!steps_.empty()) {
        Action& current = *steps_.front();
        ActionStatus status = current.Update(tick, cells_map);

        if (!current.IsDone()) {
            return status;
        }
        steps_.erase(steps_.begin());
    }

    is_done_ = true;
    return ActionStatus::kDone;
}

void ActionSequence::Reset() {
    is_done_ = false;
    if (!org_steps_.empty()) {
        steps_.clear();
        for (auto& step : org_steps_) {
            steps_.push_back(step->Clone());
        }
    } else {
        for (auto& step : steps_) {
            step->Reset();
        }
    }
}

std::unique_ptr<Action> ActionSequence::Clone() const {
    auto& source = org_steps_.empty() ? steps_ : org_steps_;
    std::vector<std::unique_ptr<Action>> cloned;
    for (auto& step : source) {
        cloned.push_back(step->Clone());
    }
    return std::make_unique<ActionSequence>(std::move(cloned), keep_steps_);
}

int ActionSequence::WaitForMs() const {
    if (is_done_ || steps_.empty()) {
        return 0;
    }
    return steps_.front()->WaitForMs();
}
