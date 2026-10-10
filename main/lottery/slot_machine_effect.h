#ifndef _SLOT_MACHINE_EFFECT_H_
#define _SLOT_MACHINE_EFFECT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "motion/action.h"

/// 水果机翻页效果参数（设计 §5.3；max_hops 默认收紧到 20，N-7 修复）
struct SlotMachineParams {
    int total_pages = 30;
    int min_hop_ms = 40;    // 名义起始间隔（真实耗时 80ms，见设计"时序模型"）
    int max_hop_ms = 400;   // 名义末段间隔
    int min_hops = 18;
    int max_hops = 20;
    int settle_bounces = 2; // 停稳前"过冲再回弹"次数（每次 = 2 跳）
};

/// 跳页计划（纯逻辑产物，可重放/可审计）
struct HopPlan {
    std::vector<int> pages;                       // 每次跳到的页（含弹跳段）
    std::vector<int> delays;                      // 对应名义等待 ms（均为 40 的倍数）
    int final_page = 0;
    /// 预期真实耗时（ms）= 40*(Σceil(delays[i]/40) - (延迟个数-1))。
    /// 减项来自引擎事实：ActionSequence 在同一 tick 内推进到下一步，每个 delay 白吃 1 tick。
    /// 真机实测（n=20）：预计 4400ms，实测 4363~4382ms（误差 <1 tick）。
    int expected_duration_ms = 0;
};

class SlotMachineEffect {
public:
    /// 生成跳页计划。randoms 为注入的随机数序列（生产用 esp_random，测试用定值）。
    static HopPlan Plan(int final_page, const SlotMachineParams& p,
                        const std::vector<uint32_t>& randoms);

    /// 把计划转为动作序列：每步 = ActionGroup{ActionPaging(1, page, adj), ActionDelay(d)}。
    /// adj 取自 PageAdjust::GetAdjustedValue(0, page)（cell 恒为 0）。
    static std::vector<std::unique_ptr<Action>> Build(const HopPlan& plan, uint8_t speed,
                                                      int paging_channel_count);
};

#endif  // _SLOT_MACHINE_EFFECT_H_
