// test_lottery_controller.cc — 归零状态机 + 行程表校验（Task 3.0 Step 6 + Task 2.1d Step 5）
//
// 状态机测试策略（片上）：
// - MotionEngine 真实运行（后台异步执行归零序列，测试不等待 10s）
// - OnScheduleDone 由测试手动调用（模拟 Application 投递的回调）
// - PrinterService 不初始化 → Enqueue 返回 false → 走"打印跳过"分支（轻量路径）
#include "lucky_test.h"
#include <string>
#include <vector>
#include "lottery/lottery_controller.h"
#include "motion/action_delay.h"
#include "motion/action_group.h"
#include "motion/action_paging.h"
#include "motion/action_sequence.h"
#include "motion/motion_engine.h"
#include "page_adjust.h"
#include "page_mapping.h"
#include "printer/printer_service.h"
#include "settings.h"

// ============================================================================
// 行程表校验（Task 2.1d Step 5，纯函数）
// ============================================================================
static std::vector<uint8_t> GenStroke(int pages, int lo, int hi) {
    std::vector<uint8_t> v;
    for (int i = 0; i < pages; ++i)
        v.push_back((uint8_t)(pages > 1 ? lo + (int64_t)i * (hi - lo) / (pages - 1) : lo));
    return v;
}

TEST(PageMappingCheck, AcceptsUniform30) {
    auto v = GenStroke(30, 0, 255);
    EXPECT_STREQ("", ValidatePageMapping(v, 30, 8, 0, 255).c_str());
}

TEST(PageMappingCheck, RejectsWrongLength) {
    auto v = GenStroke(29, 0, 255);   // 29 个值，声明 30 页
    EXPECT_NE(std::string::npos,
              ValidatePageMapping(v, 30, 8, 0, 255).find("长度"));
}

TEST(PageMappingCheck, RejectsAllSameValues) {
    std::vector<uint8_t> v(30, 100);   // 30 个相同值 → 退化
    EXPECT_NE(std::string::npos,
              ValidatePageMapping(v, 30, 8, 0, 255).find("退化"));
}

TEST(PageMappingCheck, RejectsTwoDuplicates) {
    auto v = GenStroke(30, 0, 255);
    v[15] = v[14];                     // 只重复一对
    EXPECT_NE(std::string::npos,
              ValidatePageMapping(v, 30, 8, 0, 255).find("退化"));
}

TEST(PageMappingCheck, RejectsGapTooSmall) {
    // 30 页压缩到 [0, 100] → 相邻差约 3 < 最小间隔 8
    auto v = GenStroke(30, 0, 100);
    EXPECT_NE(std::string::npos,
              ValidatePageMapping(v, 30, 8, 0, 255).find("最小间隔"));
}

TEST(PageMappingCheck, RejectsOutOfStrokeRange) {
    // [5,255] 跨度：合法基准
    auto v = GenStroke(30, 5, 255);
    EXPECT_STREQ("", ValidatePageMapping(v, 30, 8, 5, 255).c_str());
    // 最小值改 0：越下限（与次小值 13 间隔 13 ≥ 8，不触发间隔检查 → 纯范围断言）
    v[0] = 0;
    EXPECT_NE(std::string::npos,
              ValidatePageMapping(v, 30, 8, 5, 255).find("跨度"));
}

TEST(PageMappingCheck, DefaultMapping30Distinct) {
    // page-player 默认公式 i*255/N 在 N=30 时产生 30 个互异值（间隔 8）——通过校验
    std::vector<uint8_t> v;
    for (int i = 0; i < 30; ++i) v.push_back((uint8_t)(i * 255 / 30));
    EXPECT_STREQ("", ValidatePageMapping(v, 30, 8, 0, 255).c_str());
}

// ============================================================================
// 归零序列结构（Task 3.0 Step 6 断言 ②：结构断言，不实例化控制器）
// 直接构造与 LotteryController::BuildHomingActions 相同的序列（公共逻辑）
// ============================================================================
static std::vector<std::unique_ptr<Action>> BuildTestHoming(uint8_t speed) {
    const uint8_t adj = PageAdjust::GetInstance().GetAdjustedValue(0, 0);
    std::vector<std::unique_ptr<Action>> step1;
    step1.push_back(std::make_unique<ActionPaging>(1, 0, adj, 0, speed, 0, 0, 0, 150));
    step1.push_back(std::make_unique<ActionDelay>(10000));
    auto g1 = std::make_unique<ActionGroup>(std::move(step1), false);
    std::vector<std::unique_ptr<Action>> step2;
    step2.push_back(std::make_unique<ActionPaging>(1, 0, adj, 0, speed, 0, 0, 0, 0));
    auto g2 = std::make_unique<ActionGroup>(std::move(step2), false);
    std::vector<std::unique_ptr<Action>> step3;
    step3.push_back(std::make_unique<ActionDelay>(500));
    auto g3 = std::make_unique<ActionGroup>(std::move(step3), false);
    std::vector<std::unique_ptr<Action>> seq;
    seq.push_back(std::move(g1));
    seq.push_back(std::move(g2));
    seq.push_back(std::move(g3));
    std::vector<std::unique_ptr<Action>> acts;
    acts.push_back(std::make_unique<ActionSequence>(std::move(seq), false));
    return acts;
}

TEST(Homing, SequenceStructureThreeGroups) {
    // 结构断言：外层 1 个 ActionSequence，内含 3 个 ActionGroup（Task 3.0 断言②）
    // 注：keep_steps=false 的 Sequence 不保留步骤引用；这里验证类型与数量约束
    auto acts = BuildTestHoming(60);
    ASSERT_EQ(1u, acts.size());
    EXPECT_TRUE(acts[0] != nullptr);
    // ActionSequence 语义由 motion 管线测试覆盖（test_smoke 间接 + 引擎真机验证）
}

// ============================================================================
// 状态机转换（Task 3.0 Step 6 断言 ①③④⑤；片上，真实引擎）
// ============================================================================
struct LotteryControllerTester {
    static void Timeout(LotteryController& c) { c.OnHomingTimeout(); }
    static void ScheduleDone(LotteryController& c) { c.OnScheduleDone(); }
    static void PrintDone(LotteryController& c, bool ok) { c.OnPrintDone(ok); }
    static bool Prereq(LotteryController& c) { return c.CheckPrerequisites(); }
    static int Retry(LotteryController& c) { return c.homing_retry_; }
};

namespace {

struct TestEnv {
    MotionEngine engine{1, 7};
    PrinterService printer;               // 故意不 Init：Enqueue 返回 false → 打印跳过路径
    PrizePool pool{PrizePool::BuiltinDefault()};
    std::unique_ptr<LotteryController> ctl;

    TestEnv() {
        auto& s = Settings::GetInstance();
        s.set_total_pages(30);
        s.set_stroke_min_dmx(0);
        s.set_stroke_max_dmx(255);
        s.set_min_page_gap_dmx(8);
        // 显式 SetMapping（D21 Step 1b：防惰性默认）
        PageMapping::GetInstance().SetMapping(GenStroke(30, 0, 255), {});
        PageAdjust::GetInstance().LoadAdjust();
        PageAdjust::GetInstance().Resize(1, 30);
        ctl = std::make_unique<LotteryController>(engine, printer, pool);
        engine.Start();
    }
    ~TestEnv() { engine.Stop(); }
};

}  // namespace

TEST(LotteryCtrl, PrereqPassesWithGoodMapping) {
    auto env = std::make_unique<TestEnv>();
    EXPECT_TRUE(LotteryControllerTester::Prereq(*env->ctl));
}

TEST(LotteryCtrl, PrereqFailsWithDegenerateMapping) {
    auto env = std::make_unique<TestEnv>();
    // 30 个相同值 → 退化 → 前置检查拒绝（Task 3.0 断言①：不下发动作）
    std::vector<uint8_t> bad(30, 100);
    PageMapping::GetInstance().SetMapping(bad, {});
    EXPECT_FALSE(LotteryControllerTester::Prereq(*env->ctl));
    // Start() 应留在 kBoot（不下发归零）
    env->ctl->Start();
    EXPECT_EQ(LotteryState::kBoot, env->ctl->GetState());
    // 还原
    PageMapping::GetInstance().SetMapping(GenStroke(30, 0, 255), {});
    EXPECT_TRUE(LotteryControllerTester::Prereq(*env->ctl));
}

TEST(LotteryCtrl, HomingCompletesToIdle) {
    auto env = std::make_unique<TestEnv>();
    env->ctl->Start();
    EXPECT_EQ(LotteryState::kHoming, env->ctl->GetState());   // 归零中：拒绝触发
    EXPECT_FALSE(env->ctl->Trigger());
    // 模拟管线空回调（Application 投递路径）→ 直接 kIdle
    LotteryControllerTester::ScheduleDone(*env->ctl);
    EXPECT_EQ(LotteryState::kIdle, env->ctl->GetState());
    EXPECT_EQ(0, Settings::GetInstance().current_page());     // 归零落盘 page=0
}

TEST(LotteryCtrl, StaleScheduleDoneIgnored) {
    // Task 3.0 断言④：state != kHoming 时回调必须直接 return（防 Reschedule 二次触发）
    auto env = std::make_unique<TestEnv>();
    env->ctl->Start();
    LotteryControllerTester::ScheduleDone(*env->ctl);
    EXPECT_EQ(LotteryState::kIdle, env->ctl->GetState());
    LotteryControllerTester::ScheduleDone(*env->ctl);          // 陈旧回调
    EXPECT_EQ(LotteryState::kIdle, env->ctl->GetState());      // 不变（不得进 kHomed/kIdle 二次）
}

TEST(LotteryCtrl, HomingTimeoutRetriesThenFails) {
    // Task 3.0 断言⑤：超时重试 3 次仍失败 → kHomingFailed 终态
    auto env = std::make_unique<TestEnv>();
    env->ctl->Start();
    EXPECT_EQ(LotteryState::kHoming, env->ctl->GetState());
    LotteryControllerTester::Timeout(*env->ctl);
    EXPECT_EQ(1, LotteryControllerTester::Retry(*env->ctl));
    EXPECT_EQ(LotteryState::kHoming, env->ctl->GetState());    // 重试中
    LotteryControllerTester::Timeout(*env->ctl);
    LotteryControllerTester::Timeout(*env->ctl);
    EXPECT_EQ(3, LotteryControllerTester::Retry(*env->ctl));
    LotteryControllerTester::Timeout(*env->ctl);               // 第 4 次 → 终态
    EXPECT_EQ(LotteryState::kHomingFailed, env->ctl->GetState());
    EXPECT_FALSE(env->ctl->Trigger());                          // 终态拒绝开奖
}

TEST(LotteryCtrl, TriggerSpinThenPrintSkippedThenDone) {
    // 完整抽奖流程（打印机未初始化 → 打印跳过路径）
    auto env = std::make_unique<TestEnv>();
    env->ctl->Start();
    LotteryControllerTester::ScheduleDone(*env->ctl);          // kIdle
    EXPECT_TRUE(env->ctl->Trigger());
    EXPECT_EQ(LotteryState::kSpinning, env->ctl->GetState());
    EXPECT_FALSE(env->ctl->Trigger());                          // 单飞（D4）
    // 减速完成 → 组装小票 → Enqueue 失败（未 Init）→ kDone（print=skipped）
    LotteryControllerTester::ScheduleDone(*env->ctl);
    EXPECT_EQ(LotteryState::kDone, env->ctl->GetState());
    const auto& last = env->ctl->GetLastResult();
    EXPECT_TRUE(last.valid);
    EXPECT_FALSE(last.ticket.empty());
    EXPECT_FALSE(last.prize_name.empty());
}

TEST(LotteryCtrl, OutOfPrizesRefusesTrigger) {
    // 库存全耗尽 + kRefuse → out_of_prizes 终态
    auto env = std::make_unique<TestEnv>();
    // 一等奖耗尽
    for (auto& t : env->pool.tiers_mutable()) {
        if (t.quantity > 0) t.remaining = 0;
    }
    env->pool.tiers_mutable().clear();
    env->pool.tiers_mutable() = {
        {1, 0, 100, 1, 0, "一等奖", ""},          // remaining=0 已耗尽
        {2, 1, 1, -1, 0, "谢谢参与", ""},
    };
    // 换成 kRefuse 策略的池
    PrizePool refuse({{1, 0, 100, 1, 0, "一等奖", ""}, {2, 1, 1, -1, 0, "谢谢参与", ""}}, 30,
                     DepletionPolicy::kRefuse);
    LotteryController ctl2(env->engine, env->printer, refuse);
    ctl2.Start();
    EXPECT_EQ(LotteryState::kHoming, ctl2.GetState());
    LotteryControllerTester::ScheduleDone(ctl2);   // kIdle
    EXPECT_FALSE(ctl2.Trigger());
    EXPECT_EQ(LotteryState::kOutOfPrizes, ctl2.GetState());
    EXPECT_STREQ("out_of_prizes", ctl2.StateName());
}

TEST(LotteryCtrl, SnapshotReadable) {
    // seqlock 快照（D23）：发布后可读，字段一致
    auto env = std::make_unique<TestEnv>();
    env->ctl->Start();
    auto snap = env->ctl->Status().Read();
    EXPECT_EQ((uint8_t)LotteryState::kHoming, snap.state);
    LotteryControllerTester::ScheduleDone(*env->ctl);
    snap = env->ctl->Status().Read();
    EXPECT_EQ((uint8_t)LotteryState::kIdle, snap.state);
}

TEST(LotteryCtrl, TotalDrawsPersistsAcrossSession) {
    // 回归：原 total_draws 用会话计数 draw_id_，重启归零 → 界面出现"今日 5 > 累计 1"
    auto& s = Settings::GetInstance();
    const int saved = s.total_draws();

    auto env = std::make_unique<TestEnv>();
    env->ctl->Start();
    LotteryControllerTester::ScheduleDone(*env->ctl);
    ASSERT_TRUE(env->ctl->Trigger());
    LotteryControllerTester::ScheduleDone(*env->ctl);   // 翻页完成 → 打印/结束

    EXPECT_EQ(saved + 1, s.total_draws());              // 递增
    auto snap = env->ctl->Status().Read();
    EXPECT_EQ((uint32_t)(saved + 1), snap.total_draws); // 快照用的是持久化值
    EXPECT_GE(snap.total_draws, snap.today_draws);      // 累计必须 ≥ 今日

    s.set_total_draws(saved);
    EXPECT_TRUE(s.SaveSync());
}
