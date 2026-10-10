#include "lottery/lottery_controller.h"

#include <algorithm>

#include "esp_log.h"
#include "esp_random.h"

#include "lottery/time_source.h"
#include "motion/action_delay.h"
#include "motion/action_group.h"
#include "motion/action_paging.h"
#include "motion/action_sequence.h"
#include "page_adjust.h"
#include "page_mapping.h"
#include "printer/receipt_defaults.h"
#include "printer/receipt_params.h"
#include "printer/receipt_template.h"
#include "printer/receipt_layout.h"
#include "settings.h"

#define TAG "LuckyCtrl"

LotteryController::LotteryController(MotionEngine& engine, PrinterService& printer,
                                     PrizePool& pool)
    : engine_(engine), printer_(printer), pool_(pool) {}

const char* LotteryController::StateName() const {
    switch (state_.load()) {
        case LotteryState::kBoot: return "boot";
        case LotteryState::kHoming: return "homing";
        case LotteryState::kHomed: return "homed";
        case LotteryState::kHomingFailed: return "homing_failed";
        case LotteryState::kIdle: return "idle";
        case LotteryState::kSpinning: return "spinning";
        case LotteryState::kPrinting: return "printing";
        case LotteryState::kDone: return "done";
        case LotteryState::kOutOfPrizes: return "out_of_prizes";
    }
    return "?";
}

// ============================================================================
// 前置检查（D21：互异 + 最小间隔 + 行程范围 —— 取代旧的 "min != max" 弱断言）
// ⚠️ PageMapping::GetMapping() 是惰性加载；LoadMapping() 在文件缺失时会隐式调
//    SetDefaultMapping（i*255/N 均匀公式）。因此 Start() 之前 Application 必须
//    先按 stroke_min/max 显式 SetMapping（Task 2.1d Step 1b），否则行程跨度被静默忽略。
// ============================================================================
bool LotteryController::CheckPrerequisites() {
    auto& settings = Settings::GetInstance();
    const int pages = settings.total_pages();
    const auto& mapping = PageMapping::GetInstance().GetMapping();

    if ((int)mapping.size() < pages) {
        ESP_LOGE(TAG, "前置检查失败：映射长度 %zu < 总页数 %d（先显式 SetMapping，Task 2.1d）",
                 mapping.size(), pages);
        return false;
    }
    // 公共纯函数校验（与 Web PUT /api/page-mapping 同一套规则）
    std::string err = ValidatePageMapping(
        std::vector<uint8_t>(mapping.begin(), mapping.begin() + pages), pages,
        settings.min_page_gap_dmx(), settings.stroke_min_dmx(), settings.stroke_max_dmx());
    if (!err.empty()) {
        ESP_LOGE(TAG, "前置检查失败：%s", err.c_str());
        return false;
    }

    auto vr = pool_.Validate();
    if (!vr.ok) {
        ESP_LOGE(TAG, "前置检查失败：奖池无效：%s", vr.error.c_str());
        return false;
    }
    return true;
}

// ============================================================================
// 归零（§5.5；时序照抄 page-player paging_controller.cc:112-153，10 秒不可改）
// ============================================================================
std::vector<std::unique_ptr<Action>> LotteryController::BuildHomingActions(int total_pages,
                                                                           uint8_t speed) {
    const uint8_t adj = PageAdjust::GetInstance().GetAdjustedValue(0, 0);

    // 第 1 段：ch07=150（重置通道）+ 等机械完全释放 10s
    std::vector<std::unique_ptr<Action>> step1;
    step1.push_back(std::make_unique<ActionPaging>(1, 0, adj, 0, speed, 0, 0, 0, 150));
    step1.push_back(std::make_unique<ActionDelay>(kHomingSettleMs));
    auto g1 = std::make_unique<ActionGroup>(std::move(step1), false);

    // 第 2 段：收掉重置通道
    std::vector<std::unique_ptr<Action>> step2;
    step2.push_back(std::make_unique<ActionPaging>(1, 0, adj, 0, speed, 0, 0, 0, 0));
    auto g2 = std::make_unique<ActionGroup>(std::move(step2), false);

    // 稳定期 500ms 放在序列末尾（§5.5 规则 3：唯一来源，不在回调里再等——m2）
    std::vector<std::unique_ptr<Action>> step3;
    step3.push_back(std::make_unique<ActionDelay>(kHomingStableMs));
    auto g3 = std::make_unique<ActionGroup>(std::move(step3), false);

    std::vector<std::unique_ptr<Action>> seq_steps;
    seq_steps.push_back(std::move(g1));
    seq_steps.push_back(std::move(g2));
    seq_steps.push_back(std::move(g3));

    std::vector<std::unique_ptr<Action>> actions;
    actions.push_back(std::make_unique<ActionSequence>(std::move(seq_steps), false));
    return actions;
}

void LotteryController::BeginHoming() {
    auto& settings = Settings::GetInstance();
    state_ = LotteryState::kHoming;
    engine_.Reschedule(BuildHomingActions(settings.total_pages(),
                                          (uint8_t)settings.step_speed()));
    ESP_LOGI(TAG, "归零序列已下发（10s 机械等待 + 稳定期，预计 %.1fs）",
             (kHomingSettleMs + kHomingStableMs + 2000) / 1000.0);

    // 超时守护（含重试）
    if (homing_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            // esp_timer 任务上下文：只置标志，实际处理由主线程的下次交互完成。
            // 这里直接调用 OnHomingTimeout 前置 state 检查是安全的（原子读）。
            static_cast<LotteryController*>(arg)->OnHomingTimeout();
        };
        args.arg = this;
        args.name = "homing_to";
        esp_timer_create(&args, &homing_timer_);
    }
    esp_timer_stop(homing_timer_);
    esp_timer_start_once(homing_timer_, (int64_t)kHomingTimeoutMs * 1000);
    UpdateSnapshot();
}

void LotteryController::OnHomingTimeout() {
    if (!alive_.load()) return;   // 析构后回调立即返回
    // esp_timer 任务上下文；只做原子状态判断 + 由引擎侧观察恢复。
    // 主线程安全：homing_retry_ 只在主线程写，这里读；状态用 atomic。
    if (state_.load() != LotteryState::kHoming) return;
    if (homing_retry_ >= kHomingMaxRetry) {
        state_ = LotteryState::kHomingFailed;   // 终态：宁可不开奖，不可开错奖
        ESP_LOGE(TAG, "归零重试 %d 次仍超时 → kHomingFailed（人工排查机械/接线）",
                 homing_retry_);
        UpdateSnapshot();
        return;
    }
    homing_retry_++;
    ESP_LOGW(TAG, "归零超时，第 %d 次重试", homing_retry_);
    // 重发归零序列（MotionEngine::Reschedule 线程安全，可在 timer 上下文调用）
    auto& settings = Settings::GetInstance();
    engine_.Reschedule(BuildHomingActions(settings.total_pages(),
                                          (uint8_t)settings.step_speed()));
    esp_timer_start_once(homing_timer_, (int64_t)kHomingTimeoutMs * 1000);
}

// ============================================================================
// Start：kBoot → 前置检查 → kHoming
// ============================================================================
LotteryController::~LotteryController() {
    alive_.store(false);   // 先断回调，再删定时器（否则回调打到已释放的 this）
    if (homing_timer_ != nullptr) {
        esp_timer_stop(homing_timer_);
        esp_timer_delete(homing_timer_);
        homing_timer_ = nullptr;
    }
    if (done_timer_ != nullptr) {
        esp_timer_stop(done_timer_);
        esp_timer_delete(done_timer_);
        done_timer_ = nullptr;
    }
}

void LotteryController::Start() {
    if (state_.load() != LotteryState::kBoot) return;

#if CONFIG_LUCKY_SKIP_HOMING_ON_BOOT
    // 调试便利（R10）：跳过 10s 等待；出厂必须为 n（sdkconfig.defaults 显式关闭）
    if (!CheckPrerequisites()) {
        ESP_LOGE(TAG, "前置检查失败，即使 SKIP_HOMING 也不能开奖");
        state_ = LotteryState::kHomingFailed;
        UpdateSnapshot();
        return;
    }
    ESP_LOGW(TAG, "WARN: homing skipped（调试模式，出厂必须关闭）");
    auto& settings = Settings::GetInstance();
    settings.set_current_page(0);
    settings.RequestSave();
    state_ = LotteryState::kIdle;
    UpdateSnapshot();
    return;
#endif

    if (!CheckPrerequisites()) {
        ESP_LOGE(TAG, "前置检查未通过，留在 kBoot（由 Application 重试或人工介入）");
        return;
    }
    BeginHoming();
}

// ============================================================================
// 管线空回调（归零完成 / 减速序列完成共用；靠状态区分）
// ============================================================================
void LotteryController::OnScheduleDone() {
    LotteryState s = state_.load();

    if (s == LotteryState::kHoming) {
        // ⚠️ 守卫（m2）：Reschedule 会把 was_schedule_done_ 置回 false，
        // 一次归零可能触发两次回调——只处理第一次
        esp_timer_stop(homing_timer_);
        state_ = LotteryState::kHomed;
        auto& settings = Settings::GetInstance();
        settings.set_current_page(0);
        settings.RequestSave();   // current_page 属可容忍丢失的高频类（D22）
        state_ = LotteryState::kIdle;
        ESP_LOGI(TAG, "归零完成，可以开奖");
        UpdateSnapshot();
        return;
    }

    if (s == LotteryState::kSpinning) {
        // 减速序列结束 → 组装小票并打印。
        // 注：二次定位（R5）已静态包含在序列末尾（弹跳段最后一跳 == final_page
        // 且带 400ms 稳定等待，等效于"补发一次幂等的 ActionPaging"）。
        auto& settings = Settings::GetInstance();

        {
            int64_t actual_ms = (esp_timer_get_time() - spin_start_us_) / 1000;
            ESP_LOGI(TAG, "水果机实测 %lld ms（模型预计 %d ms，差 %lld ms）",
                     (long long)actual_ms, last_.expected_ms,
                     (long long)(actual_ms - last_.expected_ms));
        }

        ReceiptTemplateV3 tmpl = LoadTemplateFromSettings();   // 单一来源，与自检页共用

        // 参数取值（设计 §7.3b）：票号/奖项来自本次开奖，日期时间由 {epoch} 决定
        ReceiptParams p = MakeParamsFromSettings();
        p.ticket = last_.ticket;
        p.prize = last_.prize_name;
        p.check = DrawRecord::Crc16(draw_id_);
        p.page = last_.page;
        p.epoch = TimeSource::GetInstance().NowEpoch();
        if (last_.valid && last_.level >= 1 && last_.level <= (int)pool_.tiers().size()) {
            const auto& tier = pool_.tiers()[last_.level - 1];
            p.note = tier.note;
            p.is_lose = tier.quantity == -1 && tier.name.find("谢谢") != std::string::npos;
        }
        p.validity = (settings.validity_days() > 0)
                         ? DrawRecord::FormatDate(p.epoch + (time_t)settings.validity_days() * 86400)
                         : "";

        if (!printer_.Enqueue(tmpl, p)) {
            ESP_LOGE(TAG, "打印队列满（上一张未完成），跳过打印");
            last_.print = PrintOutcome::kSkipped;
            EnterDone();
            return;
        }
        state_ = LotteryState::kPrinting;
        ESP_LOGI(TAG, "小票已入队（票号 %s）", p.ticket.c_str());
        UpdateSnapshot();
        return;
    }
    // 其他状态：忽略（陈旧回调）
}

// ============================================================================
// Trigger：抽奖主流程（D2 先抽奖 → 再翻到该页；D4 单飞）
// ============================================================================
bool LotteryController::Trigger() {
    if (!IsDrawable()) {
        ESP_LOGW(TAG, "状态 %s，忽略触发（单飞 D4）", StateName());
        return false;
    }

    auto outcome = pool_.DrawAndConsume(esp_random());
    if (outcome.status == DrawStatus::kRefuse || outcome.status == DrawStatus::kAllDepleted) {
        state_ = LotteryState::kOutOfPrizes;
        ESP_LOGW(TAG, "无法开奖：%s → out_of_prizes（等待补货或重置库存）",
                 outcome.error.c_str());
        UpdateSnapshot();
        return false;
    }
    if (outcome.status != DrawStatus::kOk) {
        ESP_LOGE(TAG, "抽奖失败：%s", outcome.error.c_str());
        return false;
    }

    // 累计开奖次数 + 库存一起落盘（D22：同步写，扣减即持久化）
    Settings::GetInstance().set_total_draws(Settings::GetInstance().total_draws() + 1);
    Settings::GetInstance().SaveSync();

    // 组装结果（票号/时间在此刻生成——落定时刻）
    draw_id_++;
    auto& settings = Settings::GetInstance();
    time_t now = TimeSource::GetInstance().NowEpoch();
    last_.valid = true;
    last_.ticket = record_.NextTicket(now);
    last_.prize_name = pool_.tiers()[outcome.result.tier_index].name;
    last_.page = outcome.result.page;
    last_.level = outcome.result.level;
    last_.time_str = DrawRecord::FormatTime(now);
    last_.print = PrintOutcome::kSkipped;
    settings.RequestSave();   // 流水号已由上面的 SaveSync 之后变更 → 再补一次异步写

    // 水果机序列（D2：翻到 outcome.result.page）
    SlotMachineParams params;
    params.total_pages = settings.total_pages();
    std::vector<uint32_t> randoms;
    randoms.reserve(64);
    for (int i = 0; i < 64; ++i) randoms.push_back(esp_random());
    current_plan_ = SlotMachineEffect::Plan(outcome.result.page, params, randoms);
    last_.expected_ms = current_plan_.expected_duration_ms;   // 实测对比基线
    engine_.Reschedule(SlotMachineEffect::Build(current_plan_,
                                                (uint8_t)settings.step_speed(),
                                                settings.paging_channel_count()));

    state_ = LotteryState::kSpinning;
    spin_start_us_ = esp_timer_get_time();   // 实测起点（须在 Reschedule 之后、日志之前）
    ESP_LOGI(TAG, "开奖：第 %u 号 → 「%s」停在第 %d 页 | 水果机 %zu 跳，预计 %d ms",
             draw_id_, last_.prize_name.c_str(), outcome.result.page + 1,
             current_plan_.pages.size(), current_plan_.expected_duration_ms);
    UpdateSnapshot();
    return true;
}

// ============================================================================
// 打印完成 → kDone →（auto_return_ms）→ kIdle
// ============================================================================
void LotteryController::OnPrintDone(bool ok) {
    if (state_.load() != LotteryState::kPrinting) return;
    last_.print = ok ? PrintOutcome::kOk : PrintOutcome::kFailed;
    EnterDone();
}

void LotteryController::EnterDone() {
    state_ = LotteryState::kDone;
    auto& settings = Settings::GetInstance();

    if (done_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<LotteryController*>(arg);
            if (!self->alive_.load()) return;   // 析构后立即返回
            if (self->state_.load() == LotteryState::kDone) {
                self->state_ = LotteryState::kIdle;
                self->UpdateSnapshot();
            }
        };
        args.arg = this;
        args.name = "auto_return";
        esp_timer_create(&args, &done_timer_);
    }
    esp_timer_stop(done_timer_);
    esp_timer_start_once(done_timer_, (int64_t)settings.auto_return_ms() * 1000);
    UpdateSnapshot();
}

// ============================================================================
// 快照（D23：seqlock；仅主线程写）
// ============================================================================
void LotteryController::UpdateSnapshot() {
    snap_.state = (uint8_t)state_.load();
    snap_.draw_id = draw_id_;
    snap_.page = (uint8_t)last_.page;
    snap_.print_outcome = (uint8_t)last_.print;
    snap_.printer_fault = printer_.Fault() ? 1 : 0;
    snap_.time_synced = TimeSource::GetInstance().IsSynced() ? 1 : 0;
    // 累计用持久化值（原用 draw_id_ 会话计数，重启归零 → 会出现"今日 > 累计"的荒唐显示）
    snap_.total_draws = (uint32_t)Settings::GetInstance().total_draws();
    snap_.today_draws = (uint32_t)record_.TodayCount();
    auto copy_str = [](char* dst, size_t cap, const std::string& s) {
        size_t n = std::min(s.size(), cap - 1);
        memcpy(dst, s.data(), n);
        dst[n] = '\0';
    };
    copy_str(snap_.ticket, sizeof(snap_.ticket), last_.ticket);
    copy_str(snap_.prize_name, sizeof(snap_.prize_name), last_.prize_name);
    copy_str(snap_.time_str, sizeof(snap_.time_str), last_.time_str);
    status_pub_.Publish(snap_);
}
