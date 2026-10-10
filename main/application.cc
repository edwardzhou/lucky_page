#include "application.h"

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
#include "page_adjust.h"
#include "page_mapping.h"
#include "settings.h"
#include "web/web_server.h"
#include "wifi/wifi_manager.h"

#define TAG "App"

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

void Application::InitButton() {
    // 高电平按钮模块（沿用 page-player：按下=HIGH）；回调在 esp_timer 上下文 → Schedule
    module_button_ = std::make_unique<Button>(BUTTON_MODULE_GPIO, 1);
    module_button_->OnPress([this]() {
        Schedule([this]() {
            if (!controller_->Trigger()) {
                ESP_LOGW(TAG, "按钮触发被拒绝（状态 %s）", controller_->StateName());
            }
        });
    });
    module_button_->OnLongPress([this]() {
        Schedule([this]() { PrintTestTicket(); });
    });
    ESP_LOGI(TAG, "[6/7] 按钮就绪 (GPIO%d, active_high)", BUTTON_MODULE_GPIO);
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
