#define TAG "ActionPaging"

#include "motion/action_paging.h"

#include <cstring>
#include <memory>

#include "esp_log.h"
#include "settings.h"

ActionPaging::ActionPaging(int cell_id, int page, uint8_t page_value,
                           uint8_t ch02, uint8_t ch03, uint8_t ch04,
                           uint8_t ch05, uint8_t ch06, uint8_t ch07,
                           int paging_channel_count)
    : cell_id_(cell_id), page_(page), paging_channel_count_(paging_channel_count) {
    channels_[0] = page_value;
    channels_[1] = ch02;
    channels_[2] = ch03;
    channels_[3] = ch04;
    channels_[4] = ch05;
    channels_[5] = ch06;
    channels_[6] = ch07;
}

std::unique_ptr<Action> ActionPaging::Clone() const {
    return std::make_unique<ActionPaging>(cell_id_, page_, channels_[0],
        channels_[1], channels_[2], channels_[3], channels_[4],
        channels_[5], channels_[6], paging_channel_count_);
}

ActionStatus ActionPaging::Update(int tick, CellsMap& cells_map) {
    if (is_done_) {
        return ActionStatus::kDone;
    }

    auto& cell = cells_map[cell_id_];

    if (Settings::GetInstance().paging_channel_count() == 3) {
        cell.SetData(0, channels_[0]);
        cell.SetData(1, channels_[2]);
        cell.SetData(2, channels_[6]);
    } else {
        for (int i = 0; i < 7 && i < Cell::kMaxChannels; ++i) {
            cell.SetData(i, channels_[i]);
        }
    }

    is_done_ = true;
    return ActionStatus::kDone;
}

void ActionPaging::Reset() {
    is_done_ = false;
}

int ActionPaging::WaitForMs() const {
    return 0;
}
