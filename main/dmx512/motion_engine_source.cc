#include "dmx512/motion_engine_source.h"

#include <cstring>

#include "esp_log.h"
#include "motion/motion_engine.h"

#define TAG "MotionSrc"

bool MotionEngineSource::GetFrame(uint8_t bufs[][512], int* channel_count) {
    if (engine_ == nullptr || bufs == nullptr) {
        return false;
    }
    engine_->GetDmxData(bufs[0]);
    if (channel_count != nullptr) {   // 防御：曾因调用方传 nullptr 触发 StoreProhibited
        *channel_count = 1;
    }
    static int diag = 0;
    if (++diag % 100 == 0) {
        ESP_LOGD(TAG, "GetFrame running=%d CH01-08: %02x %02x %02x %02x %02x %02x %02x %02x",
                 engine_->IsRunning(),
                 bufs[0][0], bufs[0][1], bufs[0][2], bufs[0][3],
                 bufs[0][4], bufs[0][5], bufs[0][6], bufs[0][7]);
    }
    return true;
}
