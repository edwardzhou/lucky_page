#include "button_gate.h"

namespace {
constexpr int64_t kMinDebounceMs = 1;
constexpr int64_t kMinCooldownMs = 1;
}  // namespace

ButtonGate::ButtonGate(int64_t debounce_ms, int64_t cooldown_ms)
    : debounce_ms_(debounce_ms < kMinDebounceMs ? kMinDebounceMs : debounce_ms),
      cooldown_ms_(cooldown_ms < kMinCooldownMs ? kMinCooldownMs : cooldown_ms) {}

bool ButtonGate::OnPress(int64_t now_ms) {
    // ① 去抖：距上次事件太近 → 视为同一串抖动，吞掉；并把时间戳顺延到本次
    //    （这样"抖多久吞多久"，不会在抖动串中间漏出一个触发）
    if (has_event_ && now_ms - last_event_ms_ < debounce_ms_) {
        last_event_ms_ = now_ms;
        ++rej_debounce_;
        return false;
    }
    last_event_ms_ = now_ms;
    has_event_ = true;

    // ② 限流：距上次被接受的触发不足冷却时间 → 忽略（不改变 last_trigger_ms_）
    if (has_trigger_ && now_ms - last_trigger_ms_ < cooldown_ms_) {
        ++rej_cooldown_;
        return false;
    }

    // ③ 接受
    last_trigger_ms_ = now_ms;
    has_trigger_ = true;
    ++accepted_;
    return true;
}
