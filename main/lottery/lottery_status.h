#ifndef _LOTTERY_STATUS_H_
#define _LOTTERY_STATUS_H_

#include <atomic>
#include <cstdint>

// ============================================================================
// LotteryStatusSnapshot — 高频轮询的只读快照（设计 §8.1 / D23，seqlock 双缓冲）
//
// ⚠️ 约束（防 use-after-free，不是脏读）：
// - 全部字段为定长 POD，禁止 std::string/std::vector/指针/任何需要析构的成员
//   （std::atomic<T> 要求可平凡复制，装不了含 string 的结构）
// - 写方（仅主线程）：写非活动槽 → seq+1 发布
// - 读方（HTTP 任务，无锁）：seq 前后一致才采用；不一致重读
// ============================================================================
struct LotteryStatusSnapshot {
    uint8_t state;                // LotteryState 底层值
    uint8_t print_outcome;        // 0=ok 1=failed 2=skipped
    uint8_t printer_fault;        // R8 红色状态灯
    uint8_t time_synced;
    uint32_t seq;                 // 写序号（偶=稳定；实现上另用独立原子发布）
    uint32_t draw_id;
    uint8_t page;
    uint8_t current_page;         // 当前设备页（UI 工作台显示）
    uint32_t total_draws;
    uint32_t today_draws;
    char ticket[20];              // 定长，'\0' 结尾
    char prize_name[32];
    char prize_note[32];          // 奖项副标题（UI 上次开奖卡片）
    char time_str[24];            // "2026-09-29 14:32:07"
};
static_assert(sizeof(LotteryStatusSnapshot) <= 160, "快照必须保持小且定长");

/// 双缓冲 + 原子序号的发布器（写=主线程；读=任意线程，无锁）
class LotteryStatusPublisher {
public:
    /// 用 pending 的内容发布新快照（主线程调用）
    void Publish(const LotteryStatusSnapshot& s) {
        LotteryStatusSnapshot& slot = buf_[1 - active_.load(std::memory_order_relaxed)];
        slot = s;
        slot.seq = next_seq_++;
        active_.store(1 - (active_.load(std::memory_order_relaxed)), std::memory_order_release);
    }

    /// 无锁读取；读取期间发生写入则重读（最多 8 次）
    LotteryStatusSnapshot Read() const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            int a = active_.load(std::memory_order_acquire);
            LotteryStatusSnapshot copy = buf_[a];
            if (active_.load(std::memory_order_acquire) == a) {
                return copy;
            }
        }
        LotteryStatusSnapshot fallback{};
        fallback.state = 0xFF;   // 读取竞争失败（极端情况）
        return fallback;
    }

private:
    LotteryStatusSnapshot buf_[2] = {};
    std::atomic<int> active_{0};
    uint32_t next_seq_ = 1;
};

#endif  // _LOTTERY_STATUS_H_
