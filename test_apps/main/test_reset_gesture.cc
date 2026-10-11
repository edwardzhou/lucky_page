// ResetGesture 单测：恢复按钮"长按 5 秒"手势的可信性判定
//
// 动机（真机现象 2026-10-11）：无人接触时设备跑了一次网络复位，排除重启/脚本/串口路径后，
// 剩下的嫌疑是**驱动层误报长按事件**。这三条闸门用来让幽灵长按无法通过。
#include "lucky_test.h"
#include "reset_gesture.h"

namespace {
constexpr int64_t kLong = 5000;
constexpr int64_t kGuard = 30000;

ResetGestureInput Ok() {
    ResetGestureInput in;
    in.boot_ms = 60000;        // 已过守卫
    in.press_down_ms = 55000;  // 5 秒前按下
    in.all_samples_low = true;
    in.sample_count = 8;
    return in;
}
}  // namespace

TEST(ResetGesture, AcceptsRealLongPressAfterGuard) {
    EXPECT_EQ(static_cast<int>(ResetGestureDecision::kAccept),
              static_cast<int>(DecideResetGesture(Ok(), kLong, kGuard)));
}

TEST(ResetGesture, RejectsDuringBootGuard) {
    ResetGestureInput in = Ok();
    in.boot_ms = 20000;        // 开机 20 秒（守卫 30 秒内）
    in.press_down_ms = 15000;
    EXPECT_EQ(static_cast<int>(ResetGestureDecision::kRejectTooEarly),
              static_cast<int>(DecideResetGesture(in, kLong, kGuard)));
    // 边界：恰好到守卫时间应当放行（其余条件满足）
    in.boot_ms = 30000;
    in.press_down_ms = 25000;
    EXPECT_EQ(static_cast<int>(ResetGestureDecision::kAccept),
              static_cast<int>(DecideResetGesture(in, kLong, kGuard)));
}

TEST(ResetGesture, RejectsPhantomLongPressWithoutPressDown) {
    // 幽灵长按：驱动直接报长按，我们从没见过按下事件
    ResetGestureInput in = Ok();
    in.press_down_ms = -1;
    EXPECT_EQ(static_cast<int>(ResetGestureDecision::kRejectNoPressDown),
              static_cast<int>(DecideResetGesture(in, kLong, kGuard)));
}

TEST(ResetGesture, RejectsWhenPressDownTooRecentForARealLongPress) {
    // 长按事件前一瞬才"按下"（例如 100 ms）→ 不可能是持续 5 秒的按压
    ResetGestureInput in = Ok();
    in.press_down_ms = in.boot_ms - 100;
    EXPECT_EQ(static_cast<int>(ResetGestureDecision::kRejectNoPressDown),
              static_cast<int>(DecideResetGesture(in, kLong, kGuard)));
    // 松弛量边界：恰好 long-1000 ms 前按下 → 通过
    in.press_down_ms = in.boot_ms - (kLong - 1000);
    EXPECT_EQ(static_cast<int>(ResetGestureDecision::kAccept),
              static_cast<int>(DecideResetGesture(in, kLong, kGuard)));
}

TEST(ResetGesture, RejectsWhenPinIsNotLowOrSamplesIncomplete) {
    ResetGestureInput in = Ok();
    in.all_samples_low = false;   // 有一次采样读到高电平
    EXPECT_EQ(static_cast<int>(ResetGestureDecision::kRejectPinNotLow),
              static_cast<int>(DecideResetGesture(in, kLong, kGuard)));
    in = Ok();
    in.sample_count = 0;          // 根本没采样成功
    EXPECT_EQ(static_cast<int>(ResetGestureDecision::kRejectPinNotLow),
              static_cast<int>(DecideResetGesture(in, kLong, kGuard)));
}

TEST(ResetGesture, DecisionTextIsNeverEmpty) {
    const ResetGestureDecision all[] = {
        ResetGestureDecision::kAccept, ResetGestureDecision::kRejectTooEarly,
        ResetGestureDecision::kRejectNoPressDown, ResetGestureDecision::kRejectPinNotLow};
    for (auto d : all) {
        const char* t = ResetGestureDecisionText(d);
        EXPECT_TRUE(t != nullptr && t[0] != '\0');
    }
}
