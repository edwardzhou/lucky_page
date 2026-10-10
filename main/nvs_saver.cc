#include "nvs_saver.h"

#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define TAG "NvsSaver"

NvsSaver& NvsSaver::GetInstance() {
    static NvsSaver instance;
    return instance;
}

void NvsSaver::Initialize() {
    if (queue_ != nullptr) {
        return;
    }
    queue_ = xQueueCreate(kQueueDepth, sizeof(uint8_t));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "创建保存队列失败");
        return;
    }

    BaseType_t ret = xTaskCreate(TaskEntry, "nvs_saver", kStackSize, this,
                                  tskIDLE_PRIORITY + 1, &task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建保存任务失败");
        vQueueDelete(queue_);
        queue_ = nullptr;
        return;
    }

    ESP_LOGI(TAG, "异步保存器已启动");
}

void NvsSaver::RequestSave() {
    if (queue_ == nullptr) {
        return;
    }
    uint8_t signal = 1;
    xQueueSend(queue_, &signal, 0);  // 非阻塞，满了就丢弃
}

void NvsSaver::SaveSync() {
    DoSave();
}

void NvsSaver::Stop() {
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    if (queue_ != nullptr) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    ESP_LOGI(TAG, "异步保存器已停止");
}

void NvsSaver::TaskEntry(void* arg) {
    auto* saver = static_cast<NvsSaver*>(arg);
    while (true) {
        uint8_t signal;
        if (xQueueReceive(saver->queue_, &signal, portMAX_DELAY) != pdTRUE) {
            break;
        }
        // 清空队列：丢弃所有重复信号，只保存最新内存数据
        xQueueReset(saver->queue_);
        saver->DoSave();
    }
    saver->task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void NvsSaver::DoSave() {
    // lucky-page（D22 修复）：不再硬编码字段清单——page-player 的做法导致
    // 新增字段永不落盘（评审 BLOCKER B3）。改为调用 Settings::SaveAll()，
    // 由字段描述表统一驱动，新增字段自动被覆盖。
    Settings::GetInstance().SaveAll();
}
