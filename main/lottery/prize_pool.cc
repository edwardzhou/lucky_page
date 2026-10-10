#include "lottery/prize_pool.h"

#include <algorithm>
#include <set>

#include <cJSON.h>

#include "esp_log.h"
#include "nvs_saver.h"
#include "settings.h"

#define TAG "PrizePool"

PrizePool::PrizePool(std::vector<PrizeTier> tiers, int total_pages, DepletionPolicy policy)
    : tiers_(std::move(tiers)), total_pages_(total_pages), policy_(policy) {}

// ----------------------------------------------------------------------------
// Validate（设计 §7.2 的 8 条自检，返回中文原因）
// ----------------------------------------------------------------------------
ValidationResult PrizePool::Validate() const {
    ValidationResult r;

    if ((int)tiers_.size() > kMaxTiers) {
        r.error = "最多 " + std::to_string(kMaxTiers) + " 个等级，当前 " +
                  std::to_string(tiers_.size()) + " 个";
        return r;
    }
    if (tiers_.empty()) {
        r.error = "奖池为空";
        return r;
    }

    std::set<int> pages_used;
    std::set<int> levels_used;
    bool any_drawable = false;
    bool has_unlimited = false;
    int max_level = 0;

    for (size_t i = 0; i < tiers_.size(); ++i) {
        const auto& t = tiers_[i];

        if (t.page < 0 || t.page >= total_pages_) {
            r.error = "第 " + std::to_string(i + 1) + " 项页码 " + std::to_string(t.page) +
                      " 超出范围（共 " + std::to_string(total_pages_) + " 页）";
            return r;
        }
        if (!pages_used.insert(t.page).second) {
            r.error = "页码 " + std::to_string(t.page) + " 已被「" +
                      tiers_[i].name + "」占用";
            return r;
        }
        if (t.level < 1) {
            r.error = "等级必须 >= 1（当前 " + std::to_string(t.level) + "）";
            return r;
        }
        if (!levels_used.insert(t.level).second) {
            r.error = "等级 " + std::to_string(t.level) + " 重复出现";
            return r;
        }
        max_level = std::max(max_level, t.level);

        if (t.name.empty()) {
            r.error = "第 " + std::to_string(i + 1) + " 项名称为空";
            return r;
        }
        if (t.quantity == 0) {
            r.error = "「" + t.name + "」数量必须为正数或不限量(-1)";
            return r;
        }
        if (t.quantity < -1) {
            r.error = "「" + t.name + "」数量非法（仅 -1 表示不限量）";
            return r;
        }
        if (t.quantity > 0 && (t.remaining < 0 || t.remaining > t.quantity)) {
            r.error = "「" + t.name + "」剩余 " + std::to_string(t.remaining) +
                      " 超过总量 " + std::to_string(t.quantity);
            return r;
        }
        if (t.quantity == -1) {
            has_unlimited = true;
        }
        if (t.weight > 0 && (t.quantity == -1 || t.remaining > 0)) {
            any_drawable = true;
        }
    }

    if ((int)levels_used.size() != (int)tiers_.size() || *levels_used.begin() != 1 ||
        max_level != (int)tiers_.size()) {
        r.error = "等级必须为 1..N 连续不重复（当前最大 " + std::to_string(max_level) +
                  "，共 " + std::to_string(tiers_.size()) + " 级）";
        return r;
    }

    if (!any_drawable) {
        r.error = "所有等级均已耗尽或权重为 0，无法开奖";
        return r;
    }
    if (policy_ == DepletionPolicy::kFallbackToLose && !has_unlimited) {
        r.error = "存在奖品耗尽风险，请设置一个不限量的兜底等级（如「谢谢参与」）";
        return r;
    }

    r.ok = true;
    return r;
}

// ----------------------------------------------------------------------------
// Decide / DrawAndConsume（设计 §5.2 四步算法）
// ----------------------------------------------------------------------------
DrawOutcome PrizePool::Decide(uint32_t random_value) const {
    DrawOutcome out;

    // 栈上定长候选集（kMaxTiers=16）——实时路径零堆分配（AGENTS 嵌入式约束）。
    // 旧实现每次调用 new vector<Cand>，100 万次压测时既是性能灾难也饿死看门狗。
    struct Cand {
        int index;
        uint32_t weight;
    };
    Cand cands[kMaxTiers];
    int cand_count = 0;
    bool limited_drawable = false;   // 是否存在"可抽的限量级"
    uint32_t total = 0;
    const int n_tiers = (int)tiers_.size();
    for (int i = 0; i < n_tiers && cand_count < kMaxTiers; ++i) {
        const auto& t = tiers_[i];
        bool drawable = t.weight > 0 && (t.quantity == -1 || t.remaining > 0);
        if (!drawable) continue;
        cands[cand_count++] = {i, t.weight};
        total += t.weight;
        if (t.quantity != -1) limited_drawable = true;
    }

    // 2. 全部限量级耗尽时的策略（D13）
    if (cand_count == 0 || !limited_drawable) {
        if (cand_count == 0) {
            out.status = DrawStatus::kAllDepleted;
            out.error = "无任何可抽等级（奖品已全部发完）";
            return out;
        }
        if (policy_ == DepletionPolicy::kRefuse) {
            out.status = DrawStatus::kRefuse;
            out.error = "奖品已发完（策略：拒绝开奖）";
            return out;
        }
        // kFallbackToLose：继续抽（只剩不限量级），标记回退
    }

    // 3. 前缀和 + random % total 定位（无分配）
    uint32_t roll = (total > 0) ? (random_value % total) : 0;
    uint32_t acc = 0;
    int selected = cands[cand_count - 1].index;
    for (int i = 0; i < cand_count; ++i) {
        acc += cands[i].weight;
        if (roll < acc) {
            selected = cands[i].index;
            break;
        }
    }

    out.status = DrawStatus::kOk;
    out.result.tier_index = selected;
    out.result.page = tiers_[selected].page;
    out.result.level = tiers_[selected].level;
    out.result.roll = roll;
    out.result.total_weight = total;
    out.result.depleted_fallback = !limited_drawable;
    return out;
}

DrawOutcome PrizePool::DrawAndConsume(uint32_t random_value) {
    DrawOutcome out = Decide(random_value);
    if (out.status != DrawStatus::kOk) return out;

    PrizeTier& t = tiers_[out.result.tier_index];
    if (t.quantity != -1) {
        if (t.remaining > 0) {
            --t.remaining;
            if (t.remaining == 0) {
                ESP_LOGW(TAG, "「%s」已发完（%d/%d）", t.name.c_str(), t.remaining, t.quantity);
            }
        }
    }
    // 库存落盘由调用方（LotteryController → Settings::SaveSync，D22）负责
    return out;
}

std::vector<double> PrizePool::DisplayProbabilities() const {
    uint32_t total = 0;
    std::vector<uint32_t> weights(tiers_.size(), 0);
    for (size_t i = 0; i < tiers_.size(); ++i) {
        const auto& t = tiers_[i];
        if (t.weight > 0 && (t.quantity == -1 || t.remaining > 0)) {
            weights[i] = t.weight;
            total += t.weight;
        }
    }
    std::vector<double> probs(tiers_.size(), 0.0);
    if (total == 0) return probs;
    for (size_t i = 0; i < tiers_.size(); ++i) probs[i] = (double)weights[i] / total;
    return probs;
}

void PrizePool::ResetStock() {
    for (auto& t : tiers_) {
        if (t.quantity > 0) t.remaining = t.quantity;
    }
}

// ----------------------------------------------------------------------------
// JSON（设计 §7.2，version 2）
// ----------------------------------------------------------------------------
std::string PrizePool::ToJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddNumberToObject(root, "total_pages", total_pages_);
    cJSON_AddNumberToObject(root, "depletion_policy", (int)policy_);
    cJSON* arr = cJSON_AddArrayToObject(root, "tiers");
    for (const auto& t : tiers_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "level", t.level);
        cJSON_AddNumberToObject(o, "page", t.page);
        cJSON_AddNumberToObject(o, "weight", (double)t.weight);
        cJSON_AddNumberToObject(o, "quantity", t.quantity);
        cJSON_AddNumberToObject(o, "remaining", t.remaining);
        cJSON_AddStringToObject(o, "name", t.name.c_str());
        cJSON_AddStringToObject(o, "note", t.note.c_str());
        cJSON_AddItemToArray(arr, o);
    }
    char* s = cJSON_PrintUnformatted(root);
    std::string out(s ? s : "");
    cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

PrizePool PrizePool::FromJson(const std::string& json, std::string* err) {
    auto fail = [err](const char* msg) {
        if (err) *err = msg;
        return PrizePool({}, 30, DepletionPolicy::kFallbackToLose);
    };
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) return fail("JSON 解析失败");
    struct Guard {
        cJSON* p;
        ~Guard() { cJSON_Delete(p); }
    } guard{root};

    cJSON* ver = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsNumber(ver) || (int)ver->valuedouble != 2) {
        return fail("version 必须为 2（旧版奖池不兼容）");
    }
    cJSON* tp = cJSON_GetObjectItem(root, "total_pages");
    if (!cJSON_IsNumber(tp) || tp->valueint < 1) return fail("total_pages 非法");
    cJSON* dp = cJSON_GetObjectItem(root, "depletion_policy");
    int policy_val = cJSON_IsNumber(dp) ? dp->valueint : 0;
    if (policy_val != 0 && policy_val != 1) return fail("depletion_policy 必须为 0 或 1");

    cJSON* arr = cJSON_GetObjectItem(root, "tiers");
    if (!cJSON_IsArray(arr)) return fail("缺少 tiers 数组");

    std::vector<PrizeTier> tiers;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        PrizeTier t;
        cJSON* f;
        if ((f = cJSON_GetObjectItem(item, "level"))) t.level = f->valueint;
        if ((f = cJSON_GetObjectItem(item, "page"))) t.page = f->valueint;
        if ((f = cJSON_GetObjectItem(item, "weight"))) t.weight = (uint32_t)f->valuedouble;
        if ((f = cJSON_GetObjectItem(item, "quantity"))) t.quantity = f->valueint;
        if ((f = cJSON_GetObjectItem(item, "remaining"))) t.remaining = f->valueint;
        if ((f = cJSON_GetObjectItem(item, "name"))) t.name = f->valuestring ? f->valuestring : "";
        if ((f = cJSON_GetObjectItem(item, "note"))) t.note = f->valuestring ? f->valuestring : "";
        tiers.push_back(std::move(t));
    }
    return PrizePool(std::move(tiers), tp->valueint,
                     (DepletionPolicy)policy_val);
}

// ----------------------------------------------------------------------------
// 三级兜底链（Task 2.1c / D9 / R5.8）与内建默认（Task 2.1b）
// ----------------------------------------------------------------------------
PrizePool PrizePool::BuiltinDefault() {
    // 30 页 + 4 级（用户确认的默认值，C9）：
    // 一等 1 / 二等 5 / 三等 30 / 谢谢参与不限量；权重 1:5:20:74
    std::vector<PrizeTier> tiers = {
        {1, 0, 1, 1, 1, "一等奖", "奖品：扫地机器人"},
        {2, 1, 5, 5, 5, "二等奖", "奖品：蓝牙音箱"},
        {3, 2, 20, 30, 30, "三等奖", "奖品：定制水杯"},
        {4, 3, 74, -1, 0, "谢谢参与", "欢迎下次再来"},
    };
    return PrizePool(std::move(tiers), 30, DepletionPolicy::kFallbackToLose);
}

std::string PrizePool::LoadChain(PrizePool* out) {
    if (out == nullptr) return "";

    auto& settings = Settings::GetInstance();

    // 第一级：NVS 主配置
    if (!settings.prize_pool().empty()) {
        std::string err;
        *out = FromJson(settings.prize_pool(), &err);
        if (err.empty() && out->Validate().ok) {
            ESP_LOGI(TAG, "奖池来源：NVS 主配置");
            return "nvs";
        }
        ESP_LOGW(TAG, "NVS 主配置无效：%s", err.empty() ? out->Validate().error.c_str() : err.c_str());
    }

    // 第二级：NVS 备份
    if (!settings.prize_pool_backup().empty()) {
        std::string err;
        *out = FromJson(settings.prize_pool_backup(), &err);
        if (err.empty() && out->Validate().ok) {
            ESP_LOGW(TAG, "奖池来源：NVS 备份（主配置已损坏）");
            return "backup";
        }
        ESP_LOGW(TAG, "NVS 备份也无效：%s", err.empty() ? out->Validate().error.c_str() : err.c_str());
    }

    // 第三级：编译内建默认
    *out = BuiltinDefault();
    ESP_LOGW(TAG, "奖池来源：编译内建默认（主配置与备份均不可用）");
    return "builtin";
}
