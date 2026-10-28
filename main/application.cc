#include "application.h"

#include "esp_timer.h"

#include "network_reset.h"

#include "printer/logo_store.h"
#include "printer/receipt_defaults.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "boards/esp32s3_lucky/config.h"
#include "motion/action_delay.h"
#include "motion/action_group.h"
#include "motion/action_paging.h"
#include "printer/printer_uart.h"
#include "speaker/speaker_service.h"
#include "storage/sd_card.h"
#include "page_adjust.h"
#include "page_mapping.h"
#include "settings.h"
#include "web/web_server.h"
#include "wifi/wifi_manager.h"

#define TAG "App"

// 恢复按钮开机守卫（板级宏 → 本文件常量，便于阅读）
static constexpr int64_t kResetButtonBootGuardMs = RESET_BUTTON_BOOT_GUARD_MS;

// 主循环事件位
#define MAIN_EVENT_SCHEDULE (1 << 0)

Application& Application::GetInstance() {
    static Application instance;
    return instance;
}

// ============================================================================
// Schedule：跨线程投递到主线程
// ============================================================================
void Application::Schedule(std::function<void()> fn) {
    if (schedule_mutex_ == nullptr || fn == nullptr) return;
    xSemaphoreTake(schedule_mutex_, portMAX_DELAY);
    schedule_queue_.push_back(std::move(fn));
    xSemaphoreGive(schedule_mutex_);
    if (event_group_ != nullptr) xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// ============================================================================
// 初始化链（顺序不可乱 —— Task 3.2 Step 1）
// ============================================================================
void Application::InitNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    Settings::GetInstance().Load();
    ESP_LOGI(TAG, "[1/7] NVS + Settings 就绪");
}

void Application::InitFilesystems() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "assets",
        .max_files = 6,
        .format_if_mount_failed = true,
    };
    // 绝不因缺文件系统变砖（真机事故：分区表被测试宿主换掉后，
    // 主固件找不到 assets 分区 → ESP_ERROR_CHECK abort → 无限重启）。
    // 原则同网络：外设/存储异常只降级，保抽奖与打印主链路可用。
    esp_err_t fs_err = esp_vfs_spiffs_register(&conf);
    if (fs_err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 挂载失败(%s)：Web 页面与上传素材不可用，"
                      "但抽奖/打印继续工作（不 abort）", esp_err_to_name(fs_err));
        web_assets_available_ = false;
    } else {
        web_assets_available_ = true;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("assets", &total, &used);
    ESP_LOGI(TAG, "[2/7] SPIFFS(assets): %zu B 总量, %zu B 已用", total, used);
}

void Application::InitMappings() {
    auto& settings = Settings::GetInstance();
    const int pages = settings.total_pages();

    // ⚠️ D21 Step 1b（Task 2.1d）：必须先显式 SetMapping，否则行程跨度被静默忽略。
    // GetMapping() 惰性加载在文件缺失时会退回 i*255/N 均匀公式（恰好 8 间隔，
    // 能通过"互异+间隔"断言但不是用户配置的跨度 → 查不出来的静默失效）。
    auto& mapping = PageMapping::GetInstance();
    const auto& current = mapping.GetMapping();   // 触发惰性加载（读已有文件）
    bool needs_regen = (int)current.size() < pages;

    if (needs_regen) {
        // 按 [stroke_min, stroke_max] 生成 N 个值：value_i = min + i*(max-min)/(N-1)
        std::vector<uint8_t> values((size_t)pages);
        int lo = settings.stroke_min_dmx();
        int hi = settings.stroke_max_dmx();
        for (int i = 0; i < pages; ++i) {
            values[i] = (uint8_t)(pages > 1 ? lo + (int64_t)i * (hi - lo) / (pages - 1) : lo);
        }
        std::vector<std::string> names((size_t)pages);
        for (int i = 0; i < pages; ++i) names[i] = "P" + std::to_string(i + 1);
        mapping.SetMapping(values, names);
        ESP_LOGI(TAG, "[3/7] 行程映射已按跨度 [%d,%d] 生成 %d 页（显式 SetMapping，D21）",
                 lo, hi, pages);
    } else {
        ESP_LOGI(TAG, "[3/7] 行程映射已存在（%zu 页，来自标定/出厂文件）", current.size());
    }
    PageAdjust::GetInstance().LoadAdjust();
    PageAdjust::GetInstance().Resize(1, pages);
}

void Application::InitMotion() {
    auto& settings = Settings::GetInstance();
    engine_ = std::make_unique<MotionEngine>(1, settings.paging_channel_count());
    engine_->Start();

    // 管线空回调 → 主线程 → 状态机（归零完成 / 减速完成）
    engine_->SetScheduleDoneCallback([this]() {
        Schedule([this]() {
            if (controller_) controller_->OnScheduleDone();   // 初始化窗口期保护
        });
    });

    dmx_service_ = std::make_unique<Dmx512Service>(engine_.get());
    dmx_service_->Initialize();
    ESP_LOGI(TAG, "[4/7] MotionEngine + DMX512 就绪");
}

void Application::InitPrinter() {
    auto& settings = Settings::GetInstance();
    printer_service_ = std::make_unique<PrinterService>();
    if (!printer_service_->Init(settings.printer_tx_pin(), settings.printer_rx_pin(),
                                settings.printer_baud())) {
        ESP_LOGE(TAG, "[5/7] 打印机初始化失败（继续运行，开奖打印会失败重试）");
    } else {
        printer_service_->SetDoneCallback([this](bool ok) {
            Schedule([this, ok]() { controller_->OnPrintDone(ok); });
        });
        // 流控（默认关；现场用串口 pflow 命令试方向，见 config.h 说明）
        printer_uart_set_flow(settings.printer_flow_pin(),
                              (printer_flow_t)settings.printer_flow_mode());
        ESP_LOGI(TAG, "[5/7] 打印机服务就绪");
    }
}

Application::ResetPinTrace Application::ResetPinTraceSnapshot() const {
    ResetPinTrace tr;
    tr.edges = reset_pin_edges_;
    tr.low_ms_total = reset_pin_low_ms_;
    tr.last_edge_ms = reset_pin_last_edge_ms_;
    tr.last_level = reset_pin_last_level_;
    return tr;
}

void Application::ResetPinSample(void* arg) {
    auto* self = static_cast<Application*>(arg);
    const int lv = gpio_get_level(RESET_BUTTON_GPIO);
    if (lv == 0) {
        self->reset_pin_low_ms_ += 10;   // 周期 10 ms
    }
    if (lv != self->reset_pin_last_level_) {
        ++self->reset_pin_edges_;
        self->reset_pin_last_edge_ms_ = esp_timer_get_time() / 1000;
        // 变低 = 疑似按下（要做复位的正是这种），用 W 级；变高 = 松开，I 级
        ESP_LOGW(TAG, "GPIO%d 电平变化 %d→%d（开机 %lld ms，累计跳变 %d 次，累计低电平 %lld ms）",
                 (int)RESET_BUTTON_GPIO, self->reset_pin_last_level_, lv,
                 (long long)self->reset_pin_last_edge_ms_, self->reset_pin_edges_,
                 (long long)self->reset_pin_low_ms_);
        self->reset_pin_last_level_ = lv;
    }
}

Application::ButtonStats Application::ButtonStatsSnapshot() const {
    ButtonStats st;
    st.draw_accepted = draw_gate_.accepted();
    st.draw_debounce = draw_gate_.rejected_debounce();
    st.draw_cooldown = draw_gate_.rejected_cooldown();
    st.draw_last_ms = draw_gate_.last_trigger_ms();
    st.page_accepted = testpage_gate_.accepted();
    st.page_debounce = testpage_gate_.rejected_debounce();
    st.page_cooldown = testpage_gate_.rejected_cooldown();
    return st;
}

void Application::InitButton() {
    // 抽奖按钮：**低电平触发**（上拉 + 按下接地）；回调在 esp_timer 上下文 → Schedule
    module_button_ = std::make_unique<Button>(BUTTON_MODULE_GPIO, BUTTON_MODULE_ACTIVE_LEVEL);
    // 去抖 + 限流（1 秒一次）在闸门里做：esp_button 4.x 没有可配的去抖参数
    module_button_->OnPress([this]() {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (!draw_gate_.OnPress(now_ms)) {
            ESP_LOGD(TAG, "抽奖按钮忽略（去抖 %d 次 / 限流 %d 次）",
                     draw_gate_.rejected_debounce(), draw_gate_.rejected_cooldown());
            return;
        }
        Schedule([this]() {
            if (!controller_->Trigger()) {
                ESP_LOGW(TAG, "按钮触发被拒绝（状态 %s）", controller_->StateName());
            }
        });
    });
    module_button_->OnLongPress([this]() {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (!testpage_gate_.OnPress(now_ms)) {
            ESP_LOGD(TAG, "长按打自检页忽略（去抖/限流）");
            return;
        }
        Schedule([this]() { PrintTestTicket(); });
    });
    ESP_LOGI(TAG, "[6/7] 抽奖按钮就绪 (GPIO%d, %s, 去抖 %d ms, 限流 %d ms)", BUTTON_MODULE_GPIO,
             BUTTON_MODULE_ACTIVE_LEVEL == 0 ? "active_low/按下=0" : "active_high/按下=1",
             BUTTON_MODULE_DEBOUNCE_MS, BUTTON_MODULE_COOLDOWN_MS);

    // 恢复默认配置按钮：上拉输入、按下为低电平、长按 5 秒 → 复位**网络**配置。
    // 回调在 esp_timer 上下文（禁止阻塞/写 NVS）→ 必须 Schedule 到主线程。
    reset_button_ = std::make_unique<Button>(RESET_BUTTON_GPIO, 0, RESET_BUTTON_LONG_MS);
    // ⚠ 开机守卫：复位会清掉已保存的 WiFi 凭据，代价高，
    // 因此**开机后 30 秒内一律忽略**该按键 —— 万一上电瞬间引脚电平不稳
    // 被驱动当成"一直按住"，也不会在第一分钟内把用户网络配置抹掉。
    // 记录"我们亲眼见过按下事件"的时刻 —— 幽灵长按（驱动误报）不会有这一步
    reset_button_->OnPress([this]() {
        reset_press_down_ms_ = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "恢复按钮按下（需持续 %d ms，开机 %lld ms）", RESET_BUTTON_LONG_MS,
                 (long long)reset_press_down_ms_);
    });
    reset_button_->OnRelease([this]() { ESP_LOGI(TAG, "恢复按钮松开"); });
    reset_button_->OnLongPress([this]() {
        // 多点采样引脚电平：只有**连续全部为低**才算真的按着（挡掉电平回弹/抖动）
        constexpr int kSamples = 8;
        int low = 0;
        for (int i = 0; i < kSamples; ++i) {
            low += (gpio_get_level(RESET_BUTTON_GPIO) == 0) ? 1 : 0;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        ResetGestureInput in;
        in.boot_ms = esp_timer_get_time() / 1000;
        in.press_down_ms = reset_press_down_ms_;
        in.all_samples_low = (low == kSamples);
        in.sample_count = kSamples;
        const ResetGestureDecision d =
            DecideResetGesture(in, RESET_BUTTON_LONG_MS, kResetButtonBootGuardMs);
        if (d != ResetGestureDecision::kAccept) {
            ++reset_phantom_rejects_;
            // 误报要显眼：这类日志是"没人按却被复位"的唯一现场线索
            ESP_LOGE(TAG,
                     "恢复按钮长按**未通过可信性判定**（第 %d 次）→ %s"
                     "［开机 %lld ms，按下事件于 %lld ms，低电平采样 %d/%d］",
                     reset_phantom_rejects_, ResetGestureDecisionText(d), (long long)in.boot_ms,
                     (long long)in.press_down_ms, low, kSamples);
            return;
        }
        ESP_LOGW(TAG, "恢复按钮长按通过判定（开机 %lld ms，按下于 %lld ms，低电平 %d/%d）→ 复位网络配置",
                 (long long)in.boot_ms, (long long)in.press_down_ms, low, kSamples);
        Schedule([this]() { ResetNetworkConfigToDefault("恢复按钮长按 5 秒"); });
    });
    ESP_LOGI(TAG, "[6/7] 恢复按钮就绪 (GPIO%d, active_low, 长按 %d ms)",
             RESET_BUTTON_GPIO, RESET_BUTTON_LONG_MS);

    // GPIO5 黑匣子：常驻采样，任何一次电平跳变都留日志 + 计数（排查无接触复位的关键证据）
    {
        esp_timer_create_args_t args = {};
        args.callback = &Application::ResetPinSample;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "rst_pin";
        args.skip_unhandled_events = false;
        if (esp_timer_create(&args, &reset_pin_timer_) == ESP_OK) {
            esp_timer_start_periodic(reset_pin_timer_, 10 * 1000);   // 10 ms
            ESP_LOGI(TAG, "GPIO%d 引脚黑匣子已启动（10 ms 采样，跳变即留证据）",
                     (int)RESET_BUTTON_GPIO);
        } else {
            ESP_LOGW(TAG, "引脚黑匣子启动失败（仅影响取证，不影响功能）");
        }
    }
}

bool Application::PrintTemplateForTest(const ReceiptTemplateV3& tmpl) {
    bool ok = printer_service_ && printer_service_->Enqueue(tmpl, MakeSampleParams());
    ESP_LOGI(TAG, "样张已入队（%u 块, %s）", (unsigned)tmpl.blocks.size(), ok ? "ok" : "busy");
    return ok;
}

void Application::PrintTestTicket() {
    // 首启迁移：旧单槽模板归入其所属模式的槽（设计 §7.3d）
    MigrateTemplateSlots();
    LogoStore::MigrateFromSpiffsIfNeeded();   // 素材：旧 SPIFFS → 独立分区（一次性）
    if (printer_service_) printer_service_->EnqueueTestTicket();
}

bool Application::DirectDrive(int page) {
    auto& settings = Settings::GetInstance();
    if (page < 0 || page >= settings.total_pages()) return false;
    LotteryState st = controller_->GetState();
    if (st != LotteryState::kIdle && st != LotteryState::kDone) return false;
    uint8_t adj = PageAdjust::GetInstance().GetAdjustedValue(0, page);
    std::vector<std::unique_ptr<Action>> children;
    children.push_back(std::make_unique<ActionPaging>(
        1, page, adj, 0, (uint8_t)settings.step_speed(), 0, 0, 0, 0,
        settings.paging_channel_count()));
    children.push_back(std::make_unique<ActionDelay>(800));
    std::vector<std::unique_ptr<Action>> acts;
    acts.push_back(std::make_unique<ActionGroup>(std::move(children), false));
    engine_->Reschedule(std::move(acts));
    ESP_LOGI(TAG, "点动到第 %d 页（行程 %u）", page + 1, adj);
    return true;
}

// ============================================================================
// Start / MainLoop
// ============================================================================
void Application::Start() {
    schedule_mutex_ = xSemaphoreCreateMutex();
    event_group_ = xEventGroupCreate();

    InitNvs();
    InitFilesystems();
    // SD 卡：延迟 1s 挂载，失败只降级（不影响抽奖/打印/Web，规则 7）
    sd_card::Initialize();
    InitMappings();
    InitMotion();
    InitPrinter();

    // 奖池：三级兜底链（NVS 主 → NVS 备份 → 编译内建，Task 2.1c）
    pool_ = std::make_unique<PrizePool>(PrizePool::BuiltinDefault());
    {
        std::string source = PrizePool::LoadChain(pool_.get());
        (void)source;
    }

    // 时间基准（SNTP，Task 2.4）
    TimeSource::GetInstance().Init();

    // 控制器（含开机归零，D11）
    controller_ =
        std::make_unique<LotteryController>(*engine_, *printer_service_, *pool_);

    // 抽奖音效（2026-10-25）：表现层注入——控制器不依赖发声硬件（规则 5）
    controller_->SetSoundLeadMs(speaker::kSoundLeadMs);
    controller_->SetSoundCallback([](bool win) {
        if (win) {
            speaker::SpeakerService::GetInstance().PlayChime();
        } else {
            speaker::SpeakerService::GetInstance().PlayLose();
        }
    });

    InitButton();

    // 主循环任务
    if (xTaskCreate(MainTaskEntry, "app_main_loop", kMainTaskStack, this, kMainTaskPrio,
                    &main_task_) != pdPASS) {
        ESP_LOGE(TAG, "主循环任务创建失败");
        return;
    }
    // WiFi AP + WebServer（现场直连 192.168.4.1）
    // 网络属"锦上添花"：任一步失败只记日志，绝不 abort（否则抽奖机变砖）
    if (wifi_manager::Start() == ESP_OK) {
        WebServer::GetInstance().Start();
    } else {
        ESP_LOGW(TAG, "WiFi 未启动，跳过 WebServer（串口命令与抽奖不受影响）");
    }

    // 开机欢迎音（I2S 喇叭，2026-10-25）：异步播放不阻塞启动；
    // 喇叭属"锦上添花"：I2S 失败/素材损坏只打日志（SpeakerService 内部降级，规则 7）
    speaker::SpeakerService::GetInstance().PlayWelcome();

    ESP_LOGI(TAG, "[7/7] Application 启动完成");
}

void Application::MainTaskEntry(void* arg) {
    static_cast<Application*>(arg)->MainLoop();
}

void Application::MainLoop() {
    // 主线程上下文里启动状态机（kBoot → kHoming → kIdle）
    controller_->Start();

    // 定期把当前时间持久化（NTP 同步后每 10 分钟一次，Task 2.4）
    int persist_div = 0;
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(60000));
        if (bits & MAIN_EVENT_SCHEDULE) {
            std::deque<std::function<void()>> local;
            xSemaphoreTake(schedule_mutex_, portMAX_DELAY);
            local.swap(schedule_queue_);
            xSemaphoreGive(schedule_mutex_);
            for (auto& fn : local) fn();
        }
        // 心跳 + 时间持久化（超时 1 分钟醒来一次）
        if (++persist_div >= 10) {
            persist_div = 0;
            TimeSource::GetInstance().PersistNow();
        }
    }
}
