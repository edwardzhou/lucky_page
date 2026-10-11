#ifndef _BUTTON_GATE_H_
#define _BUTTON_GATE_H_

#include <cstdint>

/// 按钮触发闸门：**软件去抖 + 触发限流**（纯逻辑，可上单测宿主）
///
/// 为什么需要：
/// - `esp_button` 4.2.1 的 `button_config_t` 只有 `long_press_time`/`short_press_time`，
///   **没有可配的去抖参数**（内部 10 ms 轮询不能替代去抖）；
/// - 机械触点 + 长导线的按钮抖动会在一串毫秒级抖动里产生多个"按下"事件；
/// - 用户要求：**1 秒内只触发一次**（防止连击重复触发抽奖）。
///
/// 语义（判定顺序固定）：
/// 1. **去抖**：距上次"事件"（含被吞掉的抖动）不足 `debounce_ms` → 吞掉，
///    并把时间戳顺延到本次（经典"等安静"语义：抖多久吞多久）；
/// 2. **限流**：距上次**被接受**的触发不足 `cooldown_ms` → 忽略；
/// 3. 否则接受。
///
/// 注意：不要用 `INT64_MIN` 之类做"从未发生"哨兵 —— `now - INT64_MIN` 会溢出，
/// 因此内部用 `has_event_/has_trigger_` 布尔。
class ButtonGate {
public:
    /// @param debounce_ms 去抖窗口（ms）；参数非法时按 1 ms/1000 ms 兜底
    /// @param cooldown_ms 两次"被接受的触发"之间的最小间隔（ms）
    ButtonGate(int64_t debounce_ms, int64_t cooldown_ms);

    /// 由一次**按下事件**驱动（设备上用 `esp_timer_get_time()/1000`；
    /// 单测里传入构造好的时间序列）。
    /// @return true = 接受本次触发；false = 被去抖或限流吞掉
    bool OnPress(int64_t now_ms);

    int accepted() const { return accepted_; }
    int rejected_debounce() const { return rej_debounce_; }
    int rejected_cooldown() const { return rej_cooldown_; }
    /// 上次**被接受**触发的时刻（ms，设备上为开机以来毫秒）；从未触发返回 -1
    int64_t last_trigger_ms() const { return has_trigger_ ? last_trigger_ms_ : -1; }
    int64_t debounce_ms() const { return debounce_ms_; }
    int64_t cooldown_ms() const { return cooldown_ms_; }

private:
    int64_t debounce_ms_;
    int64_t cooldown_ms_;
    int64_t last_event_ms_ = 0;     ///< 上次事件（含抖动）
    int64_t last_trigger_ms_ = 0;   ///< 上次被接受的触发
    bool has_event_ = false;
    bool has_trigger_ = false;
    int accepted_ = 0;
    int rej_debounce_ = 0;
    int rej_cooldown_ = 0;
};

#endif  // _BUTTON_GATE_H_
