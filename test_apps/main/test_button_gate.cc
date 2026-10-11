// ButtonGate 单测：按钮去抖 + 触发限流（用户要求"1 秒内只能触发一次"）
//
// 这些边界（抖动串、999ms vs 1000ms、被吞的事件不计入限流）在真机上很难复现，
// 所以把逻辑抽成纯类在宿主上逐条锁定。
#include "button_gate.h"
#include "lucky_test.h"

TEST(ButtonGate, FirstPressAccepted) {
    ButtonGate g(30, 1000);
    EXPECT_TRUE(g.OnPress(0));
    EXPECT_EQ(1, g.accepted());
    EXPECT_EQ(0, g.rejected_debounce());
    EXPECT_EQ(0, g.rejected_cooldown());
    EXPECT_EQ(0, g.last_trigger_ms());
}

TEST(ButtonGate, BounceBurstProducesSingleTrigger) {
    ButtonGate g(30, 1000);
    // 一次机械按下常见的抖动串：0/5/12/20/28 ms
    EXPECT_TRUE(g.OnPress(0));
    EXPECT_FALSE(g.OnPress(5));
    EXPECT_FALSE(g.OnPress(12));
    EXPECT_FALSE(g.OnPress(20));
    EXPECT_FALSE(g.OnPress(28));
    EXPECT_EQ(1, g.accepted());
    EXPECT_EQ(4, g.rejected_debounce());
    // ⚠ 被去抖吞掉的事件**不应**计入限流拒绝（否则统计会误导现场排障）
    EXPECT_EQ(0, g.rejected_cooldown());
}

TEST(ButtonGate, DebounceWindowSlidesWhileJitterContinues) {
    // 去抖是"等安静"语义：每次抖动都把窗口顺延，因此持续抖动期间不会漏出触发。
    ButtonGate g(30, 1000);
    EXPECT_TRUE(g.OnPress(0));
    for (int t = 20; t <= 200; t += 20) {    // 每 20 ms 一次，始终落在 30 ms 窗口内
        EXPECT_FALSE(g.OnPress(t));
    }
    EXPECT_EQ(1, g.accepted());
    EXPECT_EQ(10, g.rejected_debounce());    // 10 次抖动全被吞（0 ms 那次已接受）
    EXPECT_EQ(0, g.rejected_cooldown());
    // 220 ms 距上一次事件(200 ms)只有 20 ms → **仍然算抖动**（不是限流拒绝）
    EXPECT_FALSE(g.OnPress(220));
    EXPECT_EQ(11, g.rejected_debounce());
    EXPECT_EQ(0, g.rejected_cooldown());
    // 250 ms 距上次事件(220 ms) = 30 ms，恰好出窗口 → 这次才轮到限流判定
    EXPECT_FALSE(g.OnPress(250));            // 距首次接受 250 ms < 1000 → 限流
    EXPECT_EQ(1, g.rejected_cooldown());
    EXPECT_EQ(1, g.accepted());
    // 出窗口 + 超过 1 秒 → 接受（证明"被吞的抖动不会永久卡住按钮"）
    EXPECT_TRUE(g.OnPress(1000));
    EXPECT_EQ(2, g.accepted());
}

TEST(ButtonGate, CooldownRejectsWithin1sAndAllowsExactlyAt1s) {
    // ① 1 秒内拒绝（999 ms）：此时距上次事件很远，走的确实是限流分支而非去抖
    {
        ButtonGate g(30, 1000);
        EXPECT_TRUE(g.OnPress(0));
        EXPECT_FALSE(g.OnPress(999));
        EXPECT_EQ(1, g.accepted());
        EXPECT_EQ(1, g.rejected_cooldown());
        EXPECT_EQ(0, g.rejected_debounce());
    }
    // ② 恰好 1 秒接受（1000 ms，中间没有其它事件 → 不混入去抖判定）
    {
        ButtonGate g(30, 1000);
        EXPECT_TRUE(g.OnPress(0));
        EXPECT_TRUE(g.OnPress(1000));
        EXPECT_EQ(2, g.accepted());
        EXPECT_EQ(1000, g.last_trigger_ms());
        EXPECT_EQ(0, g.rejected_debounce());
        EXPECT_EQ(0, g.rejected_cooldown());
    }
}

TEST(ButtonGate, PressRightAfterCooldownRejectionIsTreatedAsBounce) {
    // 真机语义：999 ms 被限流拒绝后，1000 ms 又来一次 —— 距上一次**事件**仅 1 ms，
    // 物理上就是同一次抖动的尾巴 → 应当吞掉（而不是"因为到 1 秒了就放行"）。
    ButtonGate g(30, 1000);
    EXPECT_TRUE(g.OnPress(0));
    EXPECT_FALSE(g.OnPress(999));            // 限流
    EXPECT_FALSE(g.OnPress(1000));           // 去抖吞掉（1 ms < 30 ms）
    EXPECT_EQ(1, g.accepted());
    EXPECT_EQ(1, g.rejected_debounce());
    EXPECT_EQ(1, g.rejected_cooldown());
    // 松开后再按（1030 ms：距上次事件 30 ms、距首次接受 1030 ms）→ 正常接受
    EXPECT_TRUE(g.OnPress(1030));
    EXPECT_EQ(2, g.accepted());
}

TEST(ButtonGate, DebounceDoesNotSwallowNextLegitimatePress) {
    ButtonGate g(30, 1000);
    EXPECT_TRUE(g.OnPress(0));
    EXPECT_FALSE(g.OnPress(10));             // 抖动
    EXPECT_TRUE(g.OnPress(1000));            // 抖动后、超过冷却 → 正常接受
    EXPECT_EQ(2, g.accepted());
}

TEST(ButtonGate, SeparateGatesDoNotStarveEachOther) {
    // 抽奖与"打自检页"各用一个闸门：长按不应因抽奖刚触发而被限流挡掉
    ButtonGate draw(30, 1000), testpage(30, 1000);
    EXPECT_TRUE(draw.OnPress(0));
    EXPECT_TRUE(testpage.OnPress(1000));     // 同一时刻的另一闸门不受影响
    EXPECT_EQ(1, draw.accepted());
    EXPECT_EQ(1, testpage.accepted());
}

TEST(ButtonGate, InvalidParamsFallBackToSafeMinimum) {
    ButtonGate g(0, 0);                      // 非法参数不应导致"每次都被吞"或除零
    EXPECT_EQ(1, g.debounce_ms());
    EXPECT_EQ(1, g.cooldown_ms());
    EXPECT_TRUE(g.OnPress(0));
    EXPECT_FALSE(g.OnPress(0));              // 0 ms 间隔 < 1 ms 去抖 → 吞掉
    EXPECT_TRUE(g.OnPress(1));
}
