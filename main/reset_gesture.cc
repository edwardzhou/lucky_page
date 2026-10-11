#include "reset_gesture.h"

namespace {
/// 按下事件允许的最早时刻：长按事件理应在按下后 `long_ms` 左右到达；
/// 若"按下"发生在长按事件前一瞬（远小于 long_ms），说明这不是一次真实的长按过程。
constexpr int64_t kPressDownSlackMs = 1000;
}  // namespace

ResetGestureDecision DecideResetGesture(const ResetGestureInput& in, int64_t long_ms,
                                       int64_t boot_guard_ms) {
    // ① 开机守卫：上电瞬间电平不稳不得触发（复位代价高：会清掉 WiFi 凭据）
    if (in.boot_ms < boot_guard_ms) {
        return ResetGestureDecision::kRejectTooEarly;
    }
    // ② 必须先见过按下事件，且它发生在"长按阈值"附近之前（幽灵事件不会先发按下）
    if (in.press_down_ms < 0 || in.boot_ms - in.press_down_ms < long_ms - kPressDownSlackMs) {
        return ResetGestureDecision::kRejectNoPressDown;
    }
    // ③ 引脚此刻必须确实是低电平，且最近多次采样全为低（挡掉电平已回弹的时序错位）
    if (!in.all_samples_low || in.sample_count <= 0) {
        return ResetGestureDecision::kRejectPinNotLow;
    }
    return ResetGestureDecision::kAccept;
}

const char* ResetGestureDecisionText(ResetGestureDecision d) {
    switch (d) {
        case ResetGestureDecision::kAccept:
            return "接受（按下事件 + 引脚确为低电平 + 已过开机守卫）";
        case ResetGestureDecision::kRejectTooEarly:
            return "拒绝：开机守卫时间内（上电电平不稳不得复位）";
        case ResetGestureDecision::kRejectNoPressDown:
            return "拒绝：没有先收到按下事件或按下时刻不合理 → 判定驱动误报（幽灵长按）";
        case ResetGestureDecision::kRejectPinNotLow:
            return "拒绝：引脚不是按下电平（或采样中有高电平）";
    }
    return "拒绝：未知原因";
}
