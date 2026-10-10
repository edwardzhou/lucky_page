#define TAG "ActionGroup"

#include "motion/action_group.h"

#include <algorithm>

#include "esp_log.h"

ActionGroup::ActionGroup(std::vector<std::unique_ptr<Action>> children,
                         bool keep_steps)
    : keep_steps_(keep_steps) {
    if (keep_steps_) {
        for (auto& child : children) {
            org_children_.push_back(child->Clone());
        }
    }
    children_ = std::move(children);
    if (children_.empty()) {
        is_done_ = true;
    }
}

void ActionGroup::Append(std::vector<std::unique_ptr<Action>> children) {
    is_done_ = false;
    for (auto& child : children) {
        if (keep_steps_) {
            org_children_.push_back(child->Clone());
        }
        children_.push_back(std::move(child));
    }
}

ActionStatus ActionGroup::Update(int tick, CellsMap& cells_map) {
    if (is_done_) {
        return ActionStatus::kDone;
    }

    ActionStatus combined = ActionStatus::kDone;

    for (auto& child : children_) {
        if (child->IsDone()) {
            continue;
        }

        ActionStatus child_status = child->Update(tick, cells_map);

        if (child_status == ActionStatus::kRunning) {
            combined = ActionStatus::kRunning;
        } else if (child_status == ActionStatus::kWaiting && combined != ActionStatus::kRunning) {
            combined = ActionStatus::kWaiting;
        }
    }

    bool all_done = true;
    for (auto& child : children_) {
        if (!child->IsDone()) {
            all_done = false;
            break;
        }
    }

    if (all_done) {
        is_done_ = true;
        return ActionStatus::kDone;
    }

    return combined;
}

void ActionGroup::Reset() {
    is_done_ = false;
    if (!org_children_.empty()) {
        children_.clear();
        for (auto& child : org_children_) {
            children_.push_back(child->Clone());
        }
    } else {
        for (auto& child : children_) {
            child->Reset();
        }
    }
}

std::unique_ptr<Action> ActionGroup::Clone() const {
    auto& source = org_children_.empty() ? children_ : org_children_;
    std::vector<std::unique_ptr<Action>> cloned;
    for (auto& child : source) {
        cloned.push_back(child->Clone());
    }
    return std::make_unique<ActionGroup>(std::move(cloned), keep_steps_);
}

// 返回所有未完成子动作中最短的剩余等待时间，用于引擎精确休眠
int ActionGroup::WaitForMs() const {
    int min_wait = 0;
    for (const auto& child : children_) {
        if (child->IsDone()) {
            continue;
        }
        int wait = child->WaitForMs();
        if (wait > 0 && (min_wait == 0 || wait < min_wait)) {
            min_wait = wait;
        }
    }
    return min_wait;
}
