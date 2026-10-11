#ifndef _RESET_GESTURE_H_
#define _RESET_GESTURE_H_

#include <cstdint>

/// 恢复按钮长按手势的**可信性判定**（纯逻辑，可上单测宿主）
///
/// 背景（真机现象，2026-10-11）：设备在**无人接触**的情况下跑了一次网络复位
/// （`net_rst_cnt 0→1`，NVS 里可查到时间戳），而 GPIO5 空闲电平稳定为 1、
/// 三连硬复位也不再复现，排除了重启/脚本/串口命令等路径。
/// 无法排除的是**驱动层误报长按事件**（电平抖动、组件状态机异常等）。
///
/// 因此复位动作前必须同时满足三个条件（缺一不可）：
///   1. **我们亲眼见过按下事件**，且它发生在约 `long_ms` 之前
///      （幽灵事件不会先发 PRESS_DOWN，因此这条能挡住纯驱动误报）；
///   2. **引脚此刻确实为低**（按下电平），并且最近 `sample_count` 次采样**全部为低**
///      （挡掉"事件为真但电平已回弹"的时序错位）；
///   3. 开机已超过守卫时间（上电瞬间电平不稳不会被当成一直按住）。
enum class ResetGestureDecision {
    kAccept = 0,           ///< 接受，可以执行复位
    kRejectTooEarly,       ///< 开机守卫内
    kRejectNoPressDown,    ///< 没有先见过按下事件（或按下时刻晚得不合理）→ 驱动误报
    kRejectPinNotLow,      ///< 引脚不是"按下"电平（或多点采样里有一次不是低）
};

struct ResetGestureInput {
    int64_t boot_ms = 0;        ///< 开机以来毫秒（事件发生时刻）
    int64_t press_down_ms = -1; ///< 最近一次**我们自己收到**的按下事件时刻；-1 = 从未
    bool all_samples_low = false;  ///< 最近若干次引脚采样是否全为低电平
    int sample_count = 0;          ///< 实际参与判定的采样点数（0 = 没采样 → 不通过）
};

/// @param long_ms 长按阈值（与 Button 构造一致）
/// @param boot_guard_ms 开机守卫时间
ResetGestureDecision DecideResetGesture(const ResetGestureInput& in, int64_t long_ms,
                                       int64_t boot_guard_ms);

/// 判定结果的日志文本（串口/日志统一口径，避免各处手写漂移）
const char* ResetGestureDecisionText(ResetGestureDecision d);

#endif  // _RESET_GESTURE_H_
