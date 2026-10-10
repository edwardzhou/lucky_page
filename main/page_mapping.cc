#define TAG "PageMapping"

#include "page_mapping.h"

#include <algorithm>

#include <cJSON.h>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "settings.h"

PageMapping& PageMapping::GetInstance() {
    static PageMapping instance;
    return instance;
}

const std::vector<uint8_t>& PageMapping::GetMapping() {
    if (!loaded_) {
        LoadMapping();
        loaded_ = true;
    }
    return mapping_;
}

const std::vector<std::string>& PageMapping::GetPageNames() {
    if (!loaded_) {
        LoadMapping();
        loaded_ = true;
    }
    return page_names_;
}

void PageMapping::SetMapping(const std::vector<uint8_t>& values,
                             const std::vector<std::string>& names) {
    mapping_ = values;
    if (!names.empty()) {
        page_names_ = names;
    }
    loaded_ = true;
    SaveMapping();
}

void PageMapping::SetPageNames(const std::vector<std::string>& names) {
    page_names_ = names;
    SaveMapping();
}

int PageMapping::GetMappingByName(const std::string& name) {
    if (!loaded_) {
        LoadMapping();
        loaded_ = true;
    }
    for (int i = 0; i < static_cast<int>(page_names_.size()); i++) {
        if (page_names_[i] == name) {
            return i;
        }
    }
    return -1;
}

void PageMapping::SetDefaultMapping(int total_pages) {
    mapping_.clear();
    if (total_pages <= 1) {
        mapping_.push_back(0);
    } else {
        for (int i = 0; i < total_pages; i++) {
            mapping_.push_back(static_cast<uint8_t>(i * 255 / total_pages));
        }
    }
    GenerateDefaultNames(total_pages);
    loaded_ = true;
    SaveMapping();
}

void PageMapping::GenerateDefaultNames(int total_pages) {
    page_names_.clear();
    for (int i = 0; i < total_pages; i++) {
        page_names_.push_back("P" + std::to_string(i + 1));
    }
}

void PageMapping::LoadMapping() {
    FILE* f = fopen("/spiffs/page_mapping.json", "r");
    if (!f) {
        ESP_LOGW(TAG, "映射文件不存在，使用默认值");
        SetDefaultMapping(Settings::GetInstance().total_pages());
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = new (std::nothrow) char[size + 1];
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "分配内存失败，使用默认值");
        SetDefaultMapping(Settings::GetInstance().total_pages());
        return;
    }

    size_t read = fread(buf, 1, size, f);
    buf[read] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(buf);
    delete[] buf;

    if (!root) {
        ESP_LOGW(TAG, "映射文件解析失败，使用默认值");
        SetDefaultMapping(Settings::GetInstance().total_pages());
        return;
    }

    if (cJSON_IsArray(root)) {
        // 旧格式：纯数组
        mapping_.clear();
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            if (cJSON_IsNumber(item)) {
                mapping_.push_back(static_cast<uint8_t>(item->valueint));
            }
        }
        cJSON_Delete(root);
        GenerateDefaultNames(mapping_.size());
        loaded_ = true;
        ESP_LOGI(TAG, "已加载 %d 条页码映射（旧格式）", mapping_.size());
        return;
    }

    if (cJSON_IsObject(root)) {
        // 新格式：{"mapping": [...], "names": [...]}
        cJSON* mapping_arr = cJSON_GetObjectItemCaseSensitive(root, "mapping");
        cJSON* names_arr = cJSON_GetObjectItemCaseSensitive(root, "names");

        mapping_.clear();
        if (cJSON_IsArray(mapping_arr)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, mapping_arr) {
                if (cJSON_IsNumber(item)) {
                    mapping_.push_back(static_cast<uint8_t>(item->valueint));
                }
            }
        }

        page_names_.clear();
        if (cJSON_IsArray(names_arr)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, names_arr) {
                if (cJSON_IsString(item)) {
                    page_names_.push_back(item->valuestring);
                }
            }
        }

        cJSON_Delete(root);

        if (page_names_.empty()) {
            GenerateDefaultNames(mapping_.size());
        }
        loaded_ = true;
        ESP_LOGI(TAG, "已加载 %d 条页码映射", mapping_.size());
        return;
    }

    ESP_LOGW(TAG, "映射文件格式错误，使用默认值");
    cJSON_Delete(root);
    SetDefaultMapping(Settings::GetInstance().total_pages());
}

void PageMapping::SaveMapping() {
    cJSON* root = cJSON_CreateObject();

    cJSON* mapping_arr = cJSON_CreateArray();
    for (auto val : mapping_) {
        cJSON_AddItemToArray(mapping_arr, cJSON_CreateNumber(val));
    }
    cJSON_AddItemToObject(root, "mapping", mapping_arr);

    cJSON* names_arr = cJSON_CreateArray();
    for (const auto& name : page_names_) {
        cJSON_AddItemToArray(names_arr, cJSON_CreateString(name.c_str()));
    }
    cJSON_AddItemToObject(root, "names", names_arr);

    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);

    FILE* f = fopen("/spiffs/page_mapping.json", "w");
    if (!f) {
        ESP_LOGE(TAG, "无法打开映射文件写入");
        free(json_str);
        return;
    }

    fputs(json_str, f);
    fclose(f);
    free(json_str);

    ESP_LOGI(TAG, "页码映射已保存");
}

// ============================================================================
// 行程表校验（纯函数；web_server 的 PUT /api/page-mapping 与
// LotteryController::CheckPrerequisites 共用，单测直接覆盖 —— Task 2.1d Step 5）
// ============================================================================
std::string ValidatePageMapping(const std::vector<uint8_t>& values, int total_pages,
                                int min_gap, int stroke_min, int stroke_max) {
    if ((int)values.size() != total_pages) {
        return "行程值长度 " + std::to_string(values.size()) + " 必须等于总页数 " +
               std::to_string(total_pages);
    }
    std::vector<uint8_t> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i] == sorted[i - 1]) {
            return "行程值退化：值 " + std::to_string(sorted[i]) + " 出现多次";
        }
        if (sorted[i] - sorted[i - 1] < min_gap) {
            return "相邻行程差 " + std::to_string(sorted[i] - sorted[i - 1]) +
                   " < 最小间隔 " + std::to_string(min_gap);
        }
    }
    if (!values.empty() &&
        (sorted.front() < stroke_min || sorted.back() > stroke_max)) {
        return "行程 [" + std::to_string(sorted.front()) + "," + std::to_string(sorted.back()) +
               "] 超出跨度 [" + std::to_string(stroke_min) + "," + std::to_string(stroke_max) + "]";
    }
    return "";
}
