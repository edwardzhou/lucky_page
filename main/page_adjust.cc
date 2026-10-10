#define TAG "PageAdjust"

#include "page_adjust.h"

#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "esp_log.h"
#include "page_mapping.h"
#include "settings.h"

PageAdjust& PageAdjust::GetInstance() {
    static PageAdjust instance;
    return instance;
}

void PageAdjust::LoadAdjust() {
    FILE* f = fopen("/spiffs/page_adjust.json", "r");
    if (!f) {
        ESP_LOGI(TAG, "微调文件不存在，初始化默认值");
        int cell_count = Settings::GetInstance().cell_count();
        int total_pages = Settings::GetInstance().total_pages();
        Resize(cell_count, total_pages);
        loaded_ = true;
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = new (std::nothrow) char[size + 1];
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "分配内存失败，使用默认值");
        Resize(Settings::GetInstance().cell_count(),
               Settings::GetInstance().total_pages());
        loaded_ = true;
        return;
    }

    size_t read = fread(buf, 1, size, f);
    buf[read] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(buf);
    delete[] buf;

    if (!root) {
        ESP_LOGW(TAG, "微调文件格式错误，使用默认值");
        Resize(Settings::GetInstance().cell_count(),
               Settings::GetInstance().total_pages());
        loaded_ = true;
        return;
    }

    cJSON* offsets_arr = cJSON_GetObjectItem(root, "offsets");
    if (!cJSON_IsArray(offsets_arr)) {
        ESP_LOGW(TAG, "offsets 字段缺失或非数组");
        cJSON_Delete(root);
        Resize(Settings::GetInstance().cell_count(),
               Settings::GetInstance().total_pages());
        loaded_ = true;
        return;
    }

    offsets_.clear();
    int cell_idx = 0;
    cJSON* cell_arr = nullptr;
    cJSON_ArrayForEach(cell_arr, offsets_arr) {
        if (!cJSON_IsArray(cell_arr)) continue;
        std::vector<int8_t> page_offsets;
        cJSON* val = nullptr;
        cJSON_ArrayForEach(val, cell_arr) {
            if (cJSON_IsNumber(val)) {
                int v = val->valueint;
                if (v > 127) v = 127;
                if (v < -128) v = -128;
                page_offsets.push_back(static_cast<int8_t>(v));
            } else {
                page_offsets.push_back(0);
            }
        }
        offsets_.push_back(std::move(page_offsets));
        cell_idx++;
    }
    cJSON_Delete(root);
    loaded_ = true;

    ESP_LOGI(TAG, "已加载 %d 个 Cell 的微调偏移", offsets_.size());
}

void PageAdjust::SaveAdjust() {
    cJSON* root = cJSON_CreateObject();
    cJSON* offsets_arr = cJSON_CreateArray();

    for (const auto& cell_offsets : offsets_) {
        cJSON* cell_arr = cJSON_CreateArray();
        for (auto val : cell_offsets) {
            cJSON_AddItemToArray(cell_arr, cJSON_CreateNumber(val));
        }
        cJSON_AddItemToArray(offsets_arr, cell_arr);
    }
    cJSON_AddItemToObject(root, "offsets", offsets_arr);

    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);

    FILE* f = fopen("/spiffs/page_adjust.json", "w");
    if (!f) {
        ESP_LOGE(TAG, "无法打开微调文件写入");
        free(json_str);
        return;
    }

    fputs(json_str, f);
    fclose(f);
    free(json_str);

    ESP_LOGI(TAG, "微调偏移已保存");
}

int8_t PageAdjust::GetOffset(int cell, int page) const {
    if (cell < 0 || cell >= (int)offsets_.size()) return 0;
    if (page < 0 || page >= (int)offsets_[cell].size()) return 0;
    return offsets_[cell][page];
}

void PageAdjust::SetOffset(int cell, int page, int8_t offset) {
    if (!loaded_) LoadAdjust();
    if (cell < 0 || cell >= (int)offsets_.size()) return;
    if (page < 0 || page >= (int)offsets_[cell].size()) return;
    offsets_[cell][page] = offset;
    SaveAdjust();
}

uint8_t PageAdjust::GetAdjustedValue(int cell, int page) const {
    auto& mapping = PageMapping::GetInstance().GetMapping();
    int base = (page >= 0 && page < (int)mapping.size()) ? mapping[page] : 0;
    int offset = GetOffset(cell, page);
    int result = base + offset;
    return static_cast<uint8_t>(std::clamp(result, 0, 255));
}

void PageAdjust::ResetAll() {
    for (auto& cell_offsets : offsets_) {
        std::fill(cell_offsets.begin(), cell_offsets.end(), 0);
    }
    SaveAdjust();
    ESP_LOGI(TAG, "所有微调偏移已清零");
}

void PageAdjust::Resize(int cell_count, int total_pages) {
    offsets_.resize(cell_count);
    for (auto& cell_offsets : offsets_) {
        cell_offsets.resize(total_pages, 0);
    }
}

const std::vector<std::vector<int8_t>>& PageAdjust::GetOffsets() const {
    return offsets_;
}
