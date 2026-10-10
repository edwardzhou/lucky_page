#ifndef _LOTTERY_CONTROLLER_H_
#define _LOTTERY_CONTROLLER_H_

#include <atomic>
#include <memory>

#include <esp_timer.h>

#include "lottery/draw_record.h"
#include "lottery/lottery_status.h"
#include "lottery/prize_pool.h"
#include "lottery/slot_machine_effect.h"
#include "motion/motion_engine.h"
#include "printer/printer_service.h"

enum class PrintOutcome { kOk = 0, kFailed = 1, kSkipped = 2 };

/// 最近一次开奖结果（值拷贝；对 Web 暴露时必须转 atomic 快照，D23）
struct LastDrawResult {
    bool valid = false;
    std::string ticket;
    std::string prize_name;
    int page = 0;
    int level = 0;
    std::string time_str;
    int expected_ms = 0;   // HopPlan 模型的预计时长（与实测对比）
    PrintOutcome print = PrintOutcome::kSkipped;
};

/// 唯一状态机（设计 §5.4/5.5 合并，M-5：抽奖与归零在同一条时间轴上）
enum class LotteryState {
    kBoot,          // 加载 PageMapping/PageAdjust/奖池 + 前置断言
    kHoming,        // 归零中，拒绝一切触发
    kHomed,         // 落 current_page=0（瞬态）
    kHomingFailed,  // 终态：需人工排查机械/接线后重启
    kIdle,          // 可开奖
    kSpinning,      // 水果机序列执行中
    kPrinting,      // 打印任务进行中
    kDone,          // 本次开奖完成，短暂停留
    kOutOfPrizes,   // 全部限量奖级耗尽且 policy=kRefuse（正常业务终态）
};

/// 抽奖控制器（单一状态机；所有方法仅在 Application 主线程调用，
/// 回调由 Application 注入 Schedule 到主线程后再调）
class LotteryController {
public:
    /// 析构必须停掉并删除所有 esp_timer —— 否则回调会打到已释放的 this
    /// （真机/片上测试踩坑：堆元数据被破坏，tlsf remove_free_block assert）
    ~LotteryController();

    LotteryController(MotionEngine& engine, PrinterService& printer, PrizePool& pool);

    /// 由 Application 初始化序列末尾调用：前置检查 → kHoming →（成功）kIdle
    void Start();

    LotteryState GetState() const { return state_; }
    bool IsDrawable() const {
        return state_ == LotteryState::kIdle || state_ == LotteryState::kDone;
    }
    const char* StateName() const;

    /// 触发一次抽奖。非 IsDrawable 时返回 false（单飞，D4）。
    bool Trigger();

    /// MotionEngine 管线空回调（Application schedule 到主线程后调用）
    void OnScheduleDone();

    /// PrinterService 完成回调（主线程）
    void OnPrintDone(bool ok);

    const LastDrawResult& GetLastResult() const { return last_; }
    const LotteryStatusPublisher& Status() const { return status_pub_; }
    PrizePool& Pool() { return pool_; }
    DrawRecord& Record() { return record_; }

    /// 归零失败的重试次数（诊断用）
    int homing_retry_count() const { return homing_retry_; }

private:
    // ---- 归零（§5.5） ----
    bool CheckPrerequisites();   // 映射覆盖全页 + 互异 + 最小间隔 + 行程范围（D21）
    void BeginHoming();
    void OnHomingTimeout();
    std::vector<std::unique_ptr<Action>> BuildHomingActions(int total_pages, uint8_t speed);

    friend struct LotteryControllerTester;  // 测试桩（Task 3.0 Step 6）

    // ---- 抽奖流程 ----
    void EnterDone();
    void UpdateSnapshot();

    MotionEngine& engine_;
    PrinterService& printer_;
    PrizePool& pool_;
    DrawRecord record_;

    std::atomic<LotteryState> state_{LotteryState::kBoot};
    LastDrawResult last_;
    uint32_t draw_id_ = 0;
    HopPlan current_plan_;
    int homing_retry_ = 0;
    int64_t spin_start_us_ = 0;   // 本轮翻页起点（实测时长用）
    std::atomic<bool> alive_{true};  // 析构后回调立即返回
    esp_timer_handle_t homing_timer_ = nullptr;
    esp_timer_handle_t done_timer_ = nullptr;

    LotteryStatusPublisher status_pub_;
    LotteryStatusSnapshot snap_{};

    static constexpr int kHomingSettleMs = 10000;    // 机械释放等待（照抄 page-player，不可改）
    static constexpr int kHomingStableMs = 500;      // 归零序列末尾稳定期（含在序列内）
    static constexpr int kHomingTimeoutMs = 14000;   // 10s + 行程 + 余量
    static constexpr int kHomingMaxRetry = 3;
};

#endif  // _LOTTERY_CONTROLLER_H_
