#include "motion/motion_engine.h"

#include <cstring>

#include "esp_log.h"
#include "settings.h"

#define TAG "MotionEngine"

MotionEngine::MotionEngine(int cell_count, int paging_channel_count)
    : cell_count_(cell_count), paging_channel_count_(paging_channel_count) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        ESP_LOGE(TAG, "创建互斥量失败");
    }

    // lucky-page：单机直接模式（cell_count 恒为 1，DMX 起始通道 = dmx_start_addr）
    // （page-player 的 PixelMapping 多机矩阵模式已按设计 D7/裁剪原则剥离）
    auto& settings = Settings::GetInstance();
    int channel = settings.dmx_start_addr();
    Cell cell(channel);
    cell.SetY(1);
    cell.SetX(1);
    cells_map_[1] = cell;
    cell_count_ = 1;
    ESP_LOGI(TAG, "单机模式初始化: cell=1, DMX起始通道=%d, paging_ch=%d",
             channel, paging_channel_count_);

    pipeline_ = std::make_unique<Pipeline>();
}

MotionEngine::~MotionEngine() {
    Stop();
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

void MotionEngine::Start(std::unique_ptr<Pipeline> pipeline) {
    Stop();

    if (mutex_ == nullptr) {
        ESP_LOGE(TAG, "互斥量未初始化，无法启动");
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (pipeline != nullptr) {
        pipeline_ = std::move(pipeline);
    }
    xSemaphoreGive(mutex_);

    running_.store(true);

    BaseType_t ret = xTaskCreate(TaskEntry, "motion_eng", 8192, this,
                                  tskIDLE_PRIORITY + 1, &task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建任务失败");
        running_.store(false);
        return;
    }

    ESP_LOGI(TAG, "引擎已启动，tick=%dms", kTickIntervalMs);
}

void MotionEngine::Stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // 等待任务真正退出（task_handle_ 由 TaskEntry 设为 nullptr）
    int wait_ms = 0;
    while (task_handle_ != nullptr && wait_ms < 500) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }

    ESP_LOGI(TAG, "引擎已停止");
}

void MotionEngine::Reconfigure(int cell_count, int paging_channel_count) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    paging_channel_count_ = paging_channel_count;
    (void)cell_count;  // lucky-page 恒为 1

    CellsMap new_map;
    {
        auto& settings = Settings::GetInstance();
        Cell cell(settings.dmx_start_addr());
        cell.SetY(1);
        cell.SetX(1);
        new_map[1] = cell;
    }
    cell_count_ = 1;

    cells_map_ = std::move(new_map);
    UpdateDmxData();
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "Reconfigure: cell=1, paging_ch=%d", paging_channel_count_);
}

bool MotionEngine::IsRunning() const {
    return running_.load();
}

void MotionEngine::SetScheduleDoneCallback(std::function<void()> callback) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    schedule_done_callback_ = std::move(callback);
    was_schedule_done_ = pipeline_ ? pipeline_->IsDone() : true;
    xSemaphoreGive(mutex_);
}

CellsMap MotionEngine::GetCellsMap() const {
    CellsMap snapshot;
    if (mutex_ == nullptr) {
        return snapshot;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot = cells_map_;
    xSemaphoreGive(mutex_);
    return snapshot;
}

void MotionEngine::GetDmxData(uint8_t* out) const {
    if (mutex_ == nullptr || out == nullptr) {
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    memcpy(out, dmx_data_, kDmxChannelCount);
    xSemaphoreGive(mutex_);
}

void MotionEngine::ForceUpdateDmxData() {
    if (mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    replay_active_ = false;  // 防御性清除
    UpdateDmxData();
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "DMX 数据已强制刷新");
}

void MotionEngine::AddActions(std::vector<std::unique_ptr<Action>> actions) {
    if (mutex_ == nullptr || !running_.load()) {
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (pipeline_) {
        pipeline_->AddActions(std::move(actions));
    }
    xSemaphoreGive(mutex_);
}

void MotionEngine::Reschedule(std::vector<std::unique_ptr<Action>> actions) {
    if (mutex_ == nullptr || !running_.load()) {
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (pipeline_) {
        pipeline_->Reschedule(std::move(actions));
        was_schedule_done_ = false;
    }
    xSemaphoreGive(mutex_);
}

void MotionEngine::SpawnAction(std::unique_ptr<Action> action) {
    if (mutex_ == nullptr || !running_.load()) {
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (pipeline_) {
        std::vector<std::unique_ptr<Action>> actions;
        actions.push_back(std::move(action));
        pipeline_->SpawnActions(std::move(actions));
    }
    xSemaphoreGive(mutex_);
}

void MotionEngine::UpdateDmxData() {
    memset(dmx_data_, 0, sizeof(dmx_data_));
    for (const auto& [id, cell] : cells_map_) {
        int base = cell.ChannelNumber() - 1;
        if (base < 0 || base + paging_channel_count_ > kDmxChannelCount) continue;
        memcpy(dmx_data_ + base, cell.Data(), paging_channel_count_);
    }
}

void MotionEngine::Tick() {
    if (mutex_ == nullptr) {
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (pipeline_ == nullptr) {
        xSemaphoreGive(mutex_);
        return;
    }

    pipeline_->Run(kTickIntervalMs, cells_map_);
    if (!replay_active_) {
        UpdateDmxData();
    }

    bool now_done = pipeline_->IsDone();
    if (!was_schedule_done_ && now_done && schedule_done_callback_) {
        schedule_done_callback_();
    }
    was_schedule_done_ = now_done;

    xSemaphoreGive(mutex_);
}

void MotionEngine::TaskEntry(void* arg) {
    auto* engine = static_cast<MotionEngine*>(arg);
    while (engine->running_.load()) {
        engine->Tick();
        vTaskDelay(pdMS_TO_TICKS(kTickIntervalMs));
    }
    ESP_LOGI(TAG, "tick 任务退出");
    engine->task_handle_ = nullptr;
    vTaskDelete(nullptr);
}
