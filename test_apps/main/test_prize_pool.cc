// test_prize_pool.cc — 分级奖池：加权抽取 + 库存 + 耗尽策略 + 校验 + JSON（Task 2.1，TDD）
#include "lucky_test.h"
#include <string>
#include "lottery/prize_pool.h"

static std::vector<PrizeTier> Make4Tiers() {
    return {
        {1, 0, 1, 1, 1, "一等奖", "奖品：扫地机器人"},
        {2, 1, 5, 5, 5, "二等奖", "奖品：蓝牙音箱"},
        {3, 2, 20, 30, 30, "三等奖", "奖品：定制水杯"},
        {4, 3, 74, -1, 0, "谢谢参与", "欢迎下次再来"},
    };
}
static constexpr int kPages = 30;

// ---------- 权重分布 ----------
TEST(PrizePool, WeightedDistribution) {
    PrizePool pool(Make4Tiers(), kPages, DepletionPolicy::kFallbackToLose);
    ASSERT_TRUE(pool.Validate().ok);
    const uint32_t total = 1 + 5 + 20 + 74;
    // 20000 次（片上实测量级；Decide 已零堆分配）。容差按二项 3σ 取 0.01：
    // max σ_p = sqrt(0.74*0.26/20000) ≈ 0.0031 → 0.01 ≈ 3.2σ（可靠不抖动）
    const uint32_t kIters = 20000;
    int hits[4] = {0, 0, 0, 0};
    for (uint32_t r = 0; r < kIters; ++r) {
        auto out = pool.Decide(r % total);
        ASSERT_EQ(DrawStatus::kOk, out.status);
        ASSERT_TRUE(out.result.tier_index >= 0 && out.result.tier_index < 4);
        hits[out.result.tier_index]++;
    }
    const double expect[4] = {0.01, 0.05, 0.20, 0.74};
    for (int i = 0; i < 4; ++i) {
        double got = (double)hits[i] / kIters;
        EXPECT_NEAR(got, expect[i], 0.01);
    }
}

TEST(PrizePool, SingleTierAlwaysWins) {
    PrizePool pool({{1, 7, 10, -1, 0, "唯一奖", ""}}, kPages,
                   DepletionPolicy::kFallbackToLose);
    ASSERT_TRUE(pool.Validate().ok);
    for (uint32_t r : {0u, 1u, 9u, 100u, 12345u}) {
        auto out = pool.Decide(r);
        ASSERT_EQ(DrawStatus::kOk, out.status);
        EXPECT_EQ(0, out.result.tier_index);
        EXPECT_EQ(7, out.result.page);
    }
}

// ---------- 库存（核心） ----------
TEST(PrizePool, QuantityIsRespectedExactly) {
    // 级1 有限量 3（权重极大，未耗尽时必被抽中）；级2 不限量
    PrizePool pool({{1, 0, 1000, 3, 3, "限量", ""},
                    {2, 1, 1, -1, 0, "兜底", ""}},
                   kPages, DepletionPolicy::kFallbackToLose);
    ASSERT_TRUE(pool.Validate().ok);
    int tier1_hits = 0;
    for (int i = 0; i < 10; ++i) {
        auto out = pool.DrawAndConsume(0);   // r=0 → 前缀和落在权重最大的候选级
        ASSERT_EQ(DrawStatus::kOk, out.status);
        if (out.result.tier_index == 0) tier1_hits++;
    }
    EXPECT_EQ(3, tier1_hits);   // 恰好 3 次，不多不少
    // 第 4 次起候选集不含级1 → 必落级2
    auto after = pool.DrawAndConsume(0);
    ASSERT_EQ(DrawStatus::kOk, after.status);
    EXPECT_EQ(1, after.result.tier_index);
}

TEST(PrizePool, DepletedTierRenormalizesOthers) {
    PrizePool pool({{1, 0, 50, 1, 1, "A", ""},
                    {2, 1, 50, -1, 0, "B", ""}},
                   kPages, DepletionPolicy::kFallbackToLose);
    ASSERT_TRUE(pool.Validate().ok);
    auto first = pool.DrawAndConsume(0);   // r=0 → 前缀 [0,50) = A
    ASSERT_EQ(DrawStatus::kOk, first.status);
    EXPECT_EQ(0, first.result.tier_index);
    // A 已耗尽 → Σweight 只剩 50，r∈[0,50) 必须 100% 落 B，且分母重算为 50
    for (uint32_t r = 0; r < 50; ++r) {
        auto out = pool.Decide(r);
        ASSERT_EQ(DrawStatus::kOk, out.status);
        EXPECT_EQ(1, out.result.tier_index);   // 不能再抽到已耗尽的 A
        EXPECT_EQ(50u, out.result.total_weight);
    }
}

TEST(PrizePool, UnlimitedTierNeverDepletes) {
    PrizePool pool({{1, 0, 10, -1, 0, "不限量", ""}}, kPages,
                   DepletionPolicy::kFallbackToLose);
    ASSERT_TRUE(pool.Validate().ok);
    for (int i = 0; i < 1000; ++i) {
        auto out = pool.DrawAndConsume(0);
        ASSERT_EQ(DrawStatus::kOk, out.status);
    }
    EXPECT_EQ(0, pool.tiers()[0].remaining);   // quantity==-1 时 remaining 不被修改
    auto still = pool.Decide(0);
    EXPECT_EQ(DrawStatus::kOk, still.status);  // 仍可抽中
}

TEST(PrizePool, DecideDoesNotMutate) {
    PrizePool pool(Make4Tiers(), kPages, DepletionPolicy::kFallbackToLose);
    const int before = pool.tiers()[0].remaining;
    for (int i = 0; i < 100; ++i) (void)pool.Decide(0);
    EXPECT_EQ(before, pool.tiers()[0].remaining);   // 纯函数不改库存
}

// ---------- 耗尽策略（D13） ----------
TEST(PrizePool, FallbackToLoseWhenAllDepleted) {
    PrizePool pool({{1, 0, 100, 1, 1, "一等奖", ""},
                    {2, 1, 1, -1, 0, "谢谢参与", ""}},
                   kPages, DepletionPolicy::kFallbackToLose);
    ASSERT_TRUE(pool.Validate().ok);
    (void)pool.DrawAndConsume(0);   // 耗尽一等奖
    auto out = pool.DrawAndConsume(0);
    ASSERT_EQ(DrawStatus::kOk, out.status);
    EXPECT_EQ(1, out.result.tier_index);
    EXPECT_TRUE(out.result.depleted_fallback);
}

TEST(PrizePool, RefuseWhenAllDepletedAndPolicyRefuse) {
    PrizePool pool({{1, 0, 100, 1, 1, "一等奖", ""},
                    {2, 1, 1, -1, 0, "谢谢参与", ""}},
                   kPages, DepletionPolicy::kRefuse);
    ASSERT_TRUE(pool.Validate().ok);
    (void)pool.DrawAndConsume(0);   // 耗尽一等奖
    auto out = pool.DrawAndConsume(0);
    // 失败必须可表达（DrawOutcome.status），而不是靠哨兵值猜
    EXPECT_EQ(DrawStatus::kRefuse, out.status);
    EXPECT_FALSE(out.error.empty());
}

// ---------- 配置校验 ----------
static ValidationResult ValidateTiers(std::vector<PrizeTier> v, DepletionPolicy p) {
    PrizePool pool(std::move(v), kPages, p);
    return pool.Validate();
}

TEST(PrizePool, RejectsDuplicatePage) {
    auto r = ValidateTiers({{1, 2, 10, 1, 1, "A", ""}, {2, 2, 10, 1, 1, "B", ""}},
                           DepletionPolicy::kFallbackToLose);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("页码"));
}
TEST(PrizePool, RejectsDuplicateLevel) {
    auto r = ValidateTiers({{1, 0, 10, 1, 1, "A", ""}, {1, 1, 10, 1, 1, "B", ""}},
                           DepletionPolicy::kFallbackToLose);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("等级"));
}
TEST(PrizePool, RejectsNonContiguousLevels) {
    auto r = ValidateTiers({{1, 0, 10, 1, 1, "A", ""}, {3, 1, 10, 1, 1, "B", ""}},
                           DepletionPolicy::kFallbackToLose);
    EXPECT_FALSE(r.ok);
}
TEST(PrizePool, RejectsOutOfRangePage) {
    // kPages = 30 → 合法 0..29；page 30 越界
    auto r = ValidateTiers({{1, 30, 10, 1, 1, "A", ""}, {2, 1, 10, -1, 0, "B", ""}},
                           DepletionPolicy::kFallbackToLose);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("30"));
}
TEST(PrizePool, RejectsZeroQuantity) {
    auto r = ValidateTiers({{1, 0, 10, 0, 0, "A", ""}}, DepletionPolicy::kRefuse);
    EXPECT_FALSE(r.ok);
}
TEST(PrizePool, RejectsRemainingOverQuantity) {
    auto r = ValidateTiers({{1, 0, 10, 3, 40, "A", ""}}, DepletionPolicy::kRefuse);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("剩余"));
}
TEST(PrizePool, RejectsTooManyTiers) {
    std::vector<PrizeTier> v;
    for (int i = 0; i < kMaxTiers + 1; ++i)   // 17 级 > 16
        v.push_back({i + 1, i % kPages, 1, 1, 1, "x", ""});
    auto r = ValidateTiers(std::move(v), DepletionPolicy::kRefuse);
    EXPECT_FALSE(r.ok);
}
TEST(PrizePool, RejectsNoDrawableTier) {
    auto r = ValidateTiers({{1, 0, 0, 1, 1, "A", ""}, {2, 1, 0, 1, 1, "B", ""}},
                           DepletionPolicy::kRefuse);
    EXPECT_FALSE(r.ok);
}
TEST(PrizePool, RejectsMissingFallbackWhenPolicyFallback) {
    auto r = ValidateTiers({{1, 0, 10, 1, 1, "A", ""}, {2, 1, 10, 5, 5, "B", ""}},
                           DepletionPolicy::kFallbackToLose);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string::npos, r.error.find("兜底"));
}

// ---------- JSON 往返 ----------
TEST(PrizePool, JsonRoundTrip) {
    PrizePool src(Make4Tiers(), kPages, DepletionPolicy::kFallbackToLose);
    src.tiers_mutable()[2].remaining = 17;   // 一个非默认值
    std::string json = src.ToJson();
    std::string err;
    PrizePool dst = PrizePool::FromJson(json, &err);
    EXPECT_TRUE(err.empty());
    ASSERT_EQ(4u, dst.tiers().size());
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(src.tiers()[i].level, dst.tiers()[i].level);
        EXPECT_EQ(src.tiers()[i].page, dst.tiers()[i].page);
        EXPECT_EQ(src.tiers()[i].weight, dst.tiers()[i].weight);
        EXPECT_EQ(src.tiers()[i].quantity, dst.tiers()[i].quantity);
        EXPECT_EQ(src.tiers()[i].remaining, dst.tiers()[i].remaining);   // 必须往返保留
        EXPECT_EQ(src.tiers()[i].name, dst.tiers()[i].name);
    }
    EXPECT_EQ(17, dst.tiers()[2].remaining);
    EXPECT_TRUE(dst.Validate().ok);
}
TEST(PrizePool, RejectsMalformedJson) {
    std::string err;
    auto p = PrizePool::FromJson("{not json", &err);
    EXPECT_FALSE(err.empty());   // 失败且不崩溃
}
TEST(PrizePool, RejectsMissingField) {
    std::string err;
    (void)PrizePool::FromJson("{\"version\":2,\"total_pages\":30}", &err);   // 缺 tiers
    EXPECT_FALSE(err.empty());
}
TEST(PrizePool, RejectsVersionMismatch) {
    std::string err;
    (void)PrizePool::FromJson("{\"version\":1,\"tiers\":[]}", &err);   // 旧版 1
    EXPECT_FALSE(err.empty());
}

// ---------- 实时概率（Web 显示用） ----------
TEST(PrizePool, ComputesDisplayProbability) {
    PrizePool pool(Make4Tiers(), kPages, DepletionPolicy::kFallbackToLose);
    ASSERT_TRUE(pool.Validate().ok);
    auto probs = pool.DisplayProbabilities();
    ASSERT_EQ(4u, probs.size());
    double sum = 0;
    for (double x : probs) sum += x;
    EXPECT_NEAR(sum, 1.0, 1e-9);
    EXPECT_NEAR(probs[0], 1.0 / 100, 1e-9);
    EXPECT_NEAR(probs[3], 74.0 / 100, 1e-9);

    // 耗尽一级后重算：耗尽级为 0，其余上升（分母变小）
    pool.tiers_mutable()[0].remaining = 0;
    auto p2 = pool.DisplayProbabilities();
    EXPECT_DOUBLE_EQ(0.0, p2[0]);
    EXPECT_NEAR(p2[1] + p2[2] + p2[3], 1.0, 1e-9);
    EXPECT_TRUE(p2[1] > probs[1]);   // 99 分母 → 概率上升
}

// ---------- 内建默认 + 兜底链 ----------
TEST(PrizePool, BuiltinDefaultValidates) {
    auto def = PrizePool::BuiltinDefault();
    auto r = def.Validate();
    EXPECT_TRUE(r.ok);
    auto probs = def.DisplayProbabilities();
    double sum = 0;
    for (double x : probs) sum += x;
    EXPECT_NEAR(sum, 1.0, 1e-9);
    EXPECT_EQ(30, def.total_pages());
}
TEST(PrizePool, ResetStockRestoresQuantities) {
    PrizePool pool({{1, 0, 10, 5, 1, "A", ""}, {2, 1, 10, -1, 0, "B", ""}}, kPages,
                   DepletionPolicy::kFallbackToLose);
    pool.ResetStock();
    EXPECT_EQ(5, pool.tiers()[0].remaining);
    EXPECT_EQ(0, pool.tiers()[1].remaining);   // 不限量级不受影响
}
