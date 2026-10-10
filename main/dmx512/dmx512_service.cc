#include "dmx512/dmx512_service.h"

#include <cstring>

#include "esp_log.h"

#include "dmx512/dmx_output.h"
#include "dmx512/motion_engine_source.h"
#include "dmx512/uart_dmx_output.h"
#include "motion/motion_engine.h"

#define TAG "Dmx512"

Dmx512Service::Dmx512Service(MotionEngine* engine)
    : motion_engine_(engine),
      motion_source_(std::make_unique<MotionEngineSource>(engine)) {}

Dmx512Service::~Dmx512Service() {
    if (running_.exchange(false)) {
        vTaskDelay(pdMS_TO_TICKS(60));  // 等任务退出
    }
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
}

bool Dmx512Service::Initialize() {
    output_ = std::make_unique<UartDmxOutput>();
    if (!output_->Initialize(DMX512_UART_COUNT)) {
        ESP_LOGE(TAG, "DMX 输出初始化失败");
        return false;
    }
    ESP_LOGI(TAG, "DMX512 初始化完成: %d 路 UART 输出", DMX512_UART_COUNT);

    running_ = true;
    if (xTaskCreate(SendTaskEntry, "dmx_send", 4096, this, 5, &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "创建发送任务失败");
        running_ = false;
        return false;
    }
    return true;
}

void Dmx512Service::GetCurrentFrame(uint8_t out[512]) const {
    memcpy(out, last_buf_, 512);
}

void Dmx512Service::SendTask() {
    uint8_t bufs[DMX512_UART_COUNT][512];
    int channel_count = 0;
    while (running_) {
        bool has_new = motion_source_->GetFrame(bufs, &channel_count);
        if (has_new) {
            memcpy(last_buf_, bufs[0], 512);
        }
        output_->SendBreak();
        output_->SendMulti(bufs, 1, DMX512_UART_COUNT);
        vTaskDelay(pdMS_TO_TICKS(kFrameIntervalMs));
    }
    vTaskDelete(nullptr);
}

void Dmx512Service::SendTaskEntry(void* arg) {
    static_cast<Dmx512Service*>(arg)->SendTask();
}
