#include "motion/pipeline.h"

#include "esp_log.h"

#define TAG "Pipeline"

Pipeline::Pipeline()
    : schedule_(std::make_unique<ActionSequence>(
          std::vector<std::unique_ptr<Action>>())),
      parallels_(std::make_unique<ActionGroup>(
          std::vector<std::unique_ptr<Action>>())) {}

void Pipeline::AddAction(std::unique_ptr<Action> action) {
    std::vector<std::unique_ptr<Action>> actions;
    actions.push_back(std::move(action));
    AddActions(std::move(actions));
}

void Pipeline::AddActions(std::vector<std::unique_ptr<Action>> actions) {
    if (actions.empty()) return;
    schedule_->Append(std::move(actions));
    ESP_LOGD(TAG, "AddActions: schedule 现有动作已追加");
}

void Pipeline::Reschedule(std::vector<std::unique_ptr<Action>> actions) {
    schedule_ = std::make_unique<ActionSequence>(std::move(actions), false);
    ESP_LOGD(TAG, "Reschedule: schedule 已替换");
}

void Pipeline::SpawnActions(std::vector<std::unique_ptr<Action>> actions) {
    if (actions.empty()) return;
    parallels_->Append(std::move(actions));
    ESP_LOGD(TAG, "SpawnActions: parallels 现有动作已追加");
}

void Pipeline::Reparallels(std::vector<std::unique_ptr<Action>> actions) {
    parallels_ = std::make_unique<ActionGroup>(std::move(actions), false);
    ESP_LOGD(TAG, "Reparallels: parallels 已替换");
}

ActionStatus Pipeline::Run(int tick, CellsMap& cells_map) {
    ActionStatus parallel_status = parallels_->Update(tick, cells_map);
    ActionStatus schedule_status = schedule_->Update(tick, cells_map);

    ActionStatus combined_status = ActionStatus::kDone;
    if (parallel_status == ActionStatus::kRunning ||
        schedule_status == ActionStatus::kRunning) {
        combined_status = ActionStatus::kRunning;
    } else if (parallel_status == ActionStatus::kWaiting ||
               schedule_status == ActionStatus::kWaiting) {
        combined_status = ActionStatus::kWaiting;
    }

    return combined_status;
}

void Pipeline::Clear() {
    schedule_ = std::make_unique<ActionSequence>(
        std::vector<std::unique_ptr<Action>>());
    parallels_ = std::make_unique<ActionGroup>(
        std::vector<std::unique_ptr<Action>>());

    ESP_LOGD(TAG, "Clear: Pipeline 已重置");
}

bool Pipeline::IsDone() const {
    return schedule_->IsDone() && parallels_->IsDone();
}
