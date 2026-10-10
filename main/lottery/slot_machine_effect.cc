#include "lottery/slot_machine_effect.h"

#include <cmath>

#include "esp_log.h"
#include "motion/action_delay.h"
#include "motion/action_group.h"
#include "motion/action_paging.h"
#include "motion/action_sequence.h"
#include "page_adjust.h"
#include "settings.h"

#define TAG "SlotMachine"

namespace {

// 引擎 tick 周期（motion_engine.cc kTickIntervalMs）
static constexpr int kTickMs = 40;

/// 名义 ms → 真实耗时（ActionDelay 语义：elapsed += tick; >= millis 即完成，
/// 故名义 m 的延时实际花费 40*ceil(m/40) ms —— 设计 §5.3 时序模型）
inline int RealMs(int nominal_ms) {
    return 40 * ((nominal_ms + 39) / 40);
}

/// 对齐到 40 的倍数且不超过 cap
inline int Align40(int v, int cap) {
    int a = ((v + 39) / 40) * 40;
    return a > cap ? cap : a;
}

/// 顺序消耗的随机源
struct RandSource {
    const std::vector<uint32_t>& v;
    size_t i = 0;
    uint32_t Next() { return (i < v.size()) ? v[i++] : (uint32_t)(i * 2654435761u); }
};

}  // namespace

HopPlan SlotMachineEffect::Plan(int final_page, const SlotMachineParams& p,
                                const std::vector<uint32_t>& randoms) {
    HopPlan plan;
    plan.final_page = final_page;
    RandSource rnd{randoms};

    const int N = p.total_pages;
    if (N < 2 || p.max_hops < 2) {
        // 退化场景：直接落定
        plan.pages.push_back(final_page);
        plan.delays.push_back(p.max_hop_ms);
        plan.expected_duration_ms = RealMs(p.max_hop_ms);
        return plan;
    }

    // ---- 跳数 n ∈ [min_hops, max_hops] ----
    int span = p.max_hops - p.min_hops;
    int n = p.min_hops + (span > 0 ? (int)(rnd.Next() % (uint32_t)(span + 1)) : 0);
    if (n < 4) n = 4;   // 保证"末 3 跳邻域 + 最后一跳"结构成立

    // ---- delays：ease-out（i/(n-1))^2 映射到 [min_hop_ms, max_hop_ms]，对齐 40 ----
    plan.delays.resize(n);
    for (int i = 0; i < n; ++i) {
        double t = (n > 1) ? (double)i / (n - 1) : 1.0;
        double m = p.min_hop_ms + (p.max_hop_ms - p.min_hop_ms) * t * t;
        plan.delays[i] = Align40((int)(m + 0.5), p.max_hop_ms);
    }

    // ---- pages：干净的三段式选页（评审 M2 复审后重写） ----
    // 普通区 [0, n-5]：随机页，禁 final、禁与上一跳相同
    // 擦边区 [n-4, n-2]（末 3 跳）：仅 {final-1, final+1}，禁与上一跳相同
    //   （不含 final —— NeverLandsEarly 要求落定前绝不等于 final）
    // 最后一跳 [n-1]：final
    plan.pages.resize(n);
    int prev = -1;
    auto pick_from = [&](const int* opts, int opt_count) -> int {
        // 从候选中过滤 prev 后随机取；全被过滤则顺序兜底
        int filtered[8];
        int fc = 0;
        for (int i = 0; i < opt_count && fc < 8; ++i)
            if (opts[i] != prev) filtered[fc++] = opts[i];
        if (fc == 0) return opts[0];   // N 极小时的理论兜底
        return filtered[rnd.Next() % (uint32_t)fc];
    };
    for (int i = 0; i < n; ++i) {
        int page;
        if (i == n - 1) {
            page = final_page;                          // 最后一跳落定
        } else if (i >= n - 4) {
            int opts[2] = {(final_page - 1 + N) % N,    // 擦边区：邻域但不含 final
                           (final_page + 1) % N};
            page = pick_from(opts, 2);
        } else {
            int c = (int)(rnd.Next() % (uint32_t)N);    // 普通区
            int opts[2] = {(c != final_page) ? c : (c + 1) % N, (c + 2) % N};
            if (opts[0] == final_page || opts[0] == prev) opts[0] = opts[1];
            if (opts[0] == final_page || opts[0] == prev) opts[0] = (opts[0] + 1) % N;
            page = opts[0];
        }
        // 终极兜底：普通区绝不能等于 final 或 prev（擦边区上面已保证）
        if (i < n - 4) {
            int guard = 0;
            while ((page == final_page || page == prev) && guard++ < 2 * N)
                page = (page + 1) % N;
        }
        plan.pages[i] = page;
        prev = page;
    }
    plan.pages[n - 1] = final_page;   // 最后一跳必为 final

    // ---- 弹跳段：settle_bounces 次 final → 邻页 → final（每次 2 跳，各 400ms） ----
    for (int b = 0; b < p.settle_bounces; ++b) {
        int neighbor = (final_page + 1) % N;
        if (neighbor == final_page) neighbor = (final_page + N - 1) % N;
        plan.pages.push_back(neighbor);
        plan.delays.push_back(p.max_hop_ms);
        plan.pages.push_back(final_page);
        plan.delays.push_back(p.max_hop_ms);
    }

    // ---- 预期真实耗时（唯一权威口径） ----
    // ---- 时长模型（必须与引擎真实语义一致，真机实测校正）----
    // 引擎事实：ActionSequence::Update 用 while 循环在同一 tick 内推进到下一步，
    // 因此每个 ActionDelay 的首个 tick 与上一跳的完成 tick 重叠 —— 白吃 40ms。
    // 实测：n=20 时预计 5320ms，实际 ~4390ms（差 960ms = 24×40，完全吻合）。
    // 正确模型：总 tick 数 = Σceil(d/40) - (延迟个数 - 1)
    int sum_ticks = 0;
    for (int d : plan.delays) {
        sum_ticks += (d + kTickMs - 1) / kTickMs;
    }
    const int delay_count = (int)plan.delays.size();
    plan.expected_duration_ms =
        kTickMs * (sum_ticks - (delay_count > 0 ? delay_count - 1 : 0));

    return plan;
}

std::vector<std::unique_ptr<Action>> SlotMachineEffect::Build(const HopPlan& plan,
                                                              uint8_t speed,
                                                              int paging_channel_count) {
    std::vector<std::unique_ptr<Action>> actions;
    actions.reserve(plan.pages.size());

    for (size_t i = 0; i < plan.pages.size(); ++i) {
        int page = plan.pages[i];
        uint8_t adj = PageAdjust::GetInstance().GetAdjustedValue(0, page);

        std::vector<std::unique_ptr<Action>> children;
        children.push_back(std::make_unique<ActionPaging>(
            /*cell_id=*/1, page, adj, /*ch02=*/0, /*ch03(speed)=*/speed,
            /*ch04..06=*/0, 0, 0, /*ch07=*/0, paging_channel_count));
        children.push_back(std::make_unique<ActionDelay>(plan.delays[i]));

        actions.push_back(std::make_unique<ActionGroup>(std::move(children), false));
    }

    // 整体作为一个串行序列（一次性 Reschedule 给引擎）
    std::vector<std::unique_ptr<Action>> seq;
    seq.push_back(std::make_unique<ActionSequence>(std::move(actions), false));
    return seq;
}
