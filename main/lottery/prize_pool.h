#ifndef _PRIZE_POOL_H_
#define _PRIZE_POOL_H_

#include <string>
#include <vector>

#include "lottery/prize.h"

struct ValidationResult {
    bool ok = false;
    std::string error;   // 中文原因（Web 400 直接回传）
};

/// 分级奖池：加权抽取 + 库存（设计 §5.1/5.2，纯内存逻辑）。
///
/// ⚠️ 本类不直连 NVS（SRP/可测性）：库存落盘由 LotteryController 在
/// DrawAndConsume 成功后调 Settings::SaveSync()（D22 同步路径）。
class PrizePool {
public:
    PrizePool(std::vector<PrizeTier> tiers, int total_pages, DepletionPolicy policy);

    /// 配置校验（设计 §7.2 的 8 条自检）
    ValidationResult Validate() const;

    /// 纯函数抽取判定：不修改库存（预览/审计/单测用）
    DrawOutcome Decide(uint32_t random_value) const;

    /// 抽取 + 扣减库存（落盘由调用方负责）
    DrawOutcome DrawAndConsume(uint32_t random_value);

    /// 各级实时理论概率（Web 显示用；耗尽/停用级为 0，其余合计 ≈ 1.0）
    std::vector<double> DisplayProbabilities() const;

    const std::vector<PrizeTier>& tiers() const { return tiers_; }
    std::vector<PrizeTier>& tiers_mutable() { return tiers_; }
    int total_pages() const { return total_pages_; }
    DepletionPolicy policy() const { return policy_; }

    /// 重置所有限量级的 remaining 回 quantity（Web「重置库存」用）
    void ResetStock();

    // ---- JSON（设计 §7.2 格式，version 2） ----
    std::string ToJson() const;
    /// 解析失败返回空池 + err；total_pages/policy 也从 JSON 读取
    static PrizePool FromJson(const std::string& json, std::string* err);

    // ---- 三级兜底链（Task 2.1c / D9）：NVS 主 → NVS 备份 → 编译内建 ----
    /// 返回实际使用的来源："nvs" / "backup" / "builtin"
    static std::string LoadChain(PrizePool* out);

    /// 编译内建默认奖池（30 页 + 4 级；Task 2.1b）
    static PrizePool BuiltinDefault();

private:
    std::vector<PrizeTier> tiers_;
    int total_pages_ = 30;
    DepletionPolicy policy_ = DepletionPolicy::kFallbackToLose;
};

#endif  // _PRIZE_POOL_H_
