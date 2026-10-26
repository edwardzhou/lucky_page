// page_content.cc — 每页打印内容纯逻辑（JSON/校验/增删改查）
#include "printer/page_content.h"

#include <algorithm>

#include "cJSON.h"

namespace page_content {
namespace {

bool ValidText(const std::string& text) {
    return text.size() <= (size_t)kMaxTextBytes;
}

}  // namespace

std::string ToJson(const std::vector<Item>& items) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON* arr = cJSON_AddArrayToObject(root, "pages");
    std::vector<Item> sorted = items;
    std::sort(sorted.begin(), sorted.end(),
              [](const Item& a, const Item& b) { return a.page < b.page; });
    for (const auto& it : sorted) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "page", it.page);
        if (!it.text.empty()) cJSON_AddStringToObject(o, "text", it.text.c_str());
        if (it.asset >= 0) cJSON_AddNumberToObject(o, "asset", it.asset);
        cJSON_AddItemToArray(arr, o);
    }
    char* s = cJSON_PrintUnformatted(root);
    std::string out(s ? s : "{}");
    cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

bool FromJson(const std::string& json, std::vector<Item>* out, std::string* error) {
    out->clear();
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        if (error) *error = "页面内容 JSON 解析失败";
        return false;
    }
    cJSON* pages = cJSON_GetObjectItem(root, "pages");
    if (!cJSON_IsArray(pages)) {
        cJSON_Delete(root);
        if (error) *error = "缺少 pages 数组";
        return false;
    }
    int n = cJSON_GetArraySize(pages);
    for (int i = 0; i < n; ++i) {
        cJSON* o = cJSON_GetArrayItem(pages, i);
        cJSON* pj = cJSON_GetObjectItem(o, "page");
        if (!cJSON_IsNumber(pj)) continue;
        int page = pj->valueint;
        if (page < 0 || page >= kMaxPages) {
            cJSON_Delete(root);
            if (error) *error = "页码越界（0.." + std::to_string(kMaxPages - 1) + "）";
            return false;
        }
        Item it;
        it.page = page;
        cJSON* tj = cJSON_GetObjectItem(o, "text");
        if (cJSON_IsString(tj) && tj->valuestring != nullptr) it.text = tj->valuestring;
        if (!ValidText(it.text)) {
            cJSON_Delete(root);
            if (error) *error = "页面文字超过 " + std::to_string(kMaxTextBytes) + " 字节";
            return false;
        }
        cJSON* aj = cJSON_GetObjectItem(o, "asset");
        it.asset = cJSON_IsNumber(aj) ? aj->valueint : -1;
        if (it.asset < -1) it.asset = -1;
        // 同页重复时后者覆盖（前端保存/导入都可能出现）
        bool replaced = false;
        for (auto& exist : *out) {
            if (exist.page == page) {
                exist = it;
                replaced = true;
                break;
            }
        }
        if (!replaced) out->push_back(it);
    }
    cJSON_Delete(root);
    return true;
}

bool Upsert(std::vector<Item>* items, int page, const std::string& text, int asset) {
    if (page < 0 || page >= kMaxPages) return false;
    if (!ValidText(text)) return false;
    if (asset < -1) asset = -1;
    for (auto& it : *items) {
        if (it.page == page) {
            it.text = text;
            it.asset = asset;
            return true;
        }
    }
    if (text.empty() && asset < 0) return true;   // 本来就是空
    items->push_back(Item{page, text, asset});
    return true;
}

const Item* Find(const std::vector<Item>& items, int page) {
    for (const auto& it : items) {
        if (it.page == page) return &it;
    }
    return nullptr;
}

}  // namespace page_content
