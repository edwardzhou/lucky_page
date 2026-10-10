// test_slot_machine_effect.cc — 水果机减速计划（Task 2.2，TDD；含真实断言）
#include "lucky_test.h"
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lottery/slot_machine_effect.h"

// 可预测的伪随机源（LCG），保证测试可重放
static std::vector<uint32_t> MakeRandoms(size_t n, uint32_t seed = 12345) {
    std::vector<uint32_t> v(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        v[i] = (x >> 16) & 0x7FFF;
    }
    return v;
}
static int RealMs(int nominal) { return 40 * ((nominal + 39) / 40); }
// 长循环测试里周期性喂狗（片上 main 任务忙转会饿死 IDLE）
static void FeedWdt() { vTaskDelay(pdMS_TO_TICKS(1)); }
static const SlotMachineParams& DefaultParams() {
    static SlotMachineParams p;   // min_hops=18, max_hops=20, bounces=2
    return p;
}

TEST(SlotMachine, HopsInRange) {
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        if (seed % 50 == 0) FeedWdt();
        auto plan = SlotMachineEffect::Plan(3, DefaultParams(), MakeRandoms(64, seed));
        EXPECT_GE((int)plan.pages.size(), DefaultParams().min_hops);
        EXPECT_LE((int)plan.pages.size(),
                  DefaultParams().max_hops + DefaultParams().settle_bounces * 2);
        EXPECT_EQ(plan.pages.size(), plan.delays.size());
    }
}

TEST(SlotMachine, NeverLandsEarly) {
    // 弹跳段开始前的每一跳都不得等于 final_page（最后一下才落定）
    const auto& p = DefaultParams();
    const int bounce_hops = p.settle_bounces * 2;
    const int fp = 3;
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        if (seed % 50 == 0) FeedWdt();
        auto plan = SlotMachineEffect::Plan(fp, p, MakeRandoms(64, seed));
        int main_hops = (int)plan.pages.size() - bounce_hops;
        ASSERT_GE(main_hops, 1);
        for (int i = 0; i < main_hops - 1; ++i) {
            EXPECT_NE(fp, plan.pages[i]);
        }
    }
}

TEST(SlotMachine, LastHopIsFinal) {
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        const int fp = 7;
        auto plan = SlotMachineEffect::Plan(fp, DefaultParams(), MakeRandoms(64, seed));
        ASSERT_FALSE(plan.pages.empty());
        EXPECT_EQ(fp, plan.pages.back());    // 最后一跳（弹跳段末）必为 final
        EXPECT_EQ(fp, plan.final_page);
    }
}

TEST(SlotMachine, DelaysMonotonicAndAligned) {
    const auto& p = DefaultParams();
    const int bounce_hops = p.settle_bounces * 2;
    for (uint32_t seed = 1; seed <= 100; ++seed) {
        auto plan = SlotMachineEffect::Plan(3, p, MakeRandoms(64, seed));
        int main = (int)plan.delays.size() - bounce_hops;
        ASSERT_GT(main, 1);
        // 全部 delay 是 40 的倍数且在 [min, max]
        for (int d : plan.delays) {
            EXPECT_EQ(0, d % 40);
            EXPECT_GE(d, p.min_hop_ms);
            EXPECT_LE(d, p.max_hop_ms);
        }
        // 减速段（前 main 跳）单调不减
        for (int i = 1; i < main; ++i) {
            EXPECT_GE(plan.delays[i], plan.delays[i - 1]);
        }
        // 弹跳段全部 == max_hop_ms（400）
        for (int i = main; i < (int)plan.delays.size(); ++i) {
            EXPECT_EQ(p.max_hop_ms, plan.delays[i]);
        }
    }
}

TEST(SlotMachine, NoAdjacentRepeat) {
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        if (seed % 50 == 0) FeedWdt();
        auto plan = SlotMachineEffect::Plan(3, DefaultParams(), MakeRandoms(64, seed));
        for (size_t i = 1; i < plan.pages.size(); ++i) {
            EXPECT_NE(plan.pages[i], plan.pages[i - 1]);   // 相邻不得重复（防视觉卡住）
        }
    }
}

TEST(SlotMachine, LateHopsNearFinal) {
    // 弹跳开始前的最后 3 跳必须落在 final 的邻域（擦边紧张感）
    const auto& p = DefaultParams();
    const int bounce_hops = p.settle_bounces * 2;
    const int fp = 5, N = p.total_pages;
    for (uint32_t seed = 1; seed <= 100; ++seed) {
        auto plan = SlotMachineEffect::Plan(fp, p, MakeRandoms(64, seed));
        int main = (int)plan.pages.size() - bounce_hops;
        ASSERT_GE(main, 4);
        for (int i = main - 3; i < main; ++i) {
            int pg = plan.pages[i];
            bool near_final =
                (pg == fp) || (pg == (fp - 1 + N) % N) || (pg == (fp + 1) % N);
            EXPECT_TRUE(near_final);
        }
    }
}

TEST(SlotMachine, RealDurationReasonable) {
    // 引擎真实模型（真机实测校正）：ActionSequence 同 tick 推进 → 每个 delay 白吃 1 tick
    //   总 ms = 40 * (Σceil(d/40) - (delay 数 - 1))
    // 实测：n=20 预计 4400 → 实际 4363~4382（误差 <1 tick）
    int lo = 1 << 30, hi = 0;
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        if (seed % 25 == 0) FeedWdt();
        auto plan = SlotMachineEffect::Plan(3, DefaultParams(), MakeRandoms(64, seed));
        ASSERT_FALSE(plan.pages.empty());
        int sum_ticks = 0;
        for (int d : plan.delays) sum_ticks += (d + 39) / 40;
        int real = 40 * (sum_ticks - ((int)plan.delays.size() - 1));
        EXPECT_EQ(real, plan.expected_duration_ms);   // 自算与实现必须一致
        lo = std::min(lo, real);
        hi = std::max(hi, real);
    }
    // n=18 → 4040ms，n=20 → 4400ms
    EXPECT_GE(lo, 4000);
    EXPECT_LE(hi, 4440);
}

TEST(SlotMachine, BounceIsTwoHopsPerBounce) {
    // 口径防回归：settle_bounces=2 → 弹跳段 4 跳 / 1600ms，不是 8 跳 / 3200ms
    auto plan = SlotMachineEffect::Plan(3, DefaultParams(), MakeRandoms(64, 7));
    int bounce_ms = 0, hops400 = 0;
    // 弹跳段 = 末尾 settle_bounces*2 跳
    int main = (int)plan.delays.size() - DefaultParams().settle_bounces * 2;
    for (int i = main; i < (int)plan.delays.size(); ++i) {
        bounce_ms += RealMs(plan.delays[i]);
        hops400++;
    }
    EXPECT_EQ(4, hops400);
    EXPECT_EQ(1600, bounce_ms);
    // 弹跳段页序：邻页 → final（两次）
    const int N = DefaultParams().total_pages;
    for (int b = 0; b < DefaultParams().settle_bounces; ++b) {
        int neighbor = plan.pages[main + b * 2];
        bool is_neighbor = neighbor == (3 + 1) % N || neighbor == (3 - 1 + N) % N;
        EXPECT_TRUE(is_neighbor);
        EXPECT_EQ(3, plan.pages[main + b * 2 + 1]);   // 回到 final
    }
}

TEST(SlotMachine, PredictsDurationMatchingTicks) {
    // expected_duration_ms 必须等于引擎真实口径（现场核对就靠它）：
    //   40 * (Σceil(d/40) - (延迟个数 - 1))
    // 真机实测校正：旧口径 Σ40*ceil(d/40) 高估 18%（n=20 时 5320 vs 实测 4363~4382）
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        if (seed % 25 == 0) FeedWdt();
        auto plan = SlotMachineEffect::Plan(9, DefaultParams(), MakeRandoms(64, seed));
        ASSERT_FALSE(plan.delays.empty());
        int sum_ticks = 0;
        for (int d : plan.delays) sum_ticks += (d + 39) / 40;
        int expect = 40 * (sum_ticks - ((int)plan.delays.size() - 1));
        EXPECT_EQ(expect, plan.expected_duration_ms);
    }
}

TEST(SlotMachine, DeterministicForSameRandoms) {
    auto r = MakeRandoms(64, 999);
    auto a = SlotMachineEffect::Plan(4, DefaultParams(), r);
    auto b = SlotMachineEffect::Plan(4, DefaultParams(), r);
    EXPECT_TRUE(a.pages == b.pages);
    EXPECT_TRUE(a.delays == b.delays);
    EXPECT_EQ(a.expected_duration_ms, b.expected_duration_ms);
}
