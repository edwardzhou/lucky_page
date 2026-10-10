#ifndef _PRIZE_H_
#define _PRIZE_H_

#include <cstdint>
#include <string>

/// 奖品等级（设计 §5.1 / D12：概率与数量分开建模）
struct PrizeTier {
    int level = 0;          // 等级序号 1..N（1 = 一等奖），仅用于展示与排序
    int page = 0;           // 对应翻页机页码，0-based ∈ [0, total_pages)
    uint32_t weight = 0;    // 相对权重（出现概率）；0 = 停用该级
    int quantity = 0;       // 总投放数量；-1 = 不限量
    int remaining = 0;      // 剩余数量（运行时递减；quantity==-1 时忽略）
    std::string name;       // 等级名，如 "一等奖"
    std::string note;       // 小票副标题，如 "奖品：扫地机器人"
};

static constexpr int kMaxTiers = 16;   // 最多 16 级

/// 奖品耗尽策略（设计 D13）
enum class DepletionPolicy {
    kFallbackToLose = 0,   // 默认：回退到不限量兜底级（保证机器不冷场）
    kRefuse = 1,           // 拒绝开奖（宁可不发，不发错的）
};

/// 抽取结果状态（B-3/M-8：DrawResult 无错误通道，必须用 Outcome 表达失败）
enum class DrawStatus {
    kOk,              // 正常抽出（含回退到兜底级的情况）
    kAllDepleted,     // 无任何可抽级（配置异常边缘）
    kRefuse,          // 全部限量级耗尽且 policy == kRefuse
    kInvalidConfig,   // 配置非法（应先 Validate）
};

struct DrawResult {
    int tier_index = -1;     // 在 tiers 中的下标
    int page = 0;
    int level = 0;
    uint32_t roll = 0;       // 抽中的随机数（审计用）
    uint32_t total_weight = 0;
    bool depleted_fallback = false;   // 本次抽取时全部限量级已耗尽
};

struct DrawOutcome {
    DrawStatus status = DrawStatus::kInvalidConfig;
    DrawResult result{};
    std::string error;       // status != kOk 时为中文原因
};

#endif  // _PRIZE_H_
