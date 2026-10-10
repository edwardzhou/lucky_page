#include "printer/printer_service.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"

#include "printer/printer_uart.h"
#include "printer/receipt_defaults.h"
#include "printer/receipt_render.h"
#include "printer/logo_store.h"
#include "settings.h"
#include "printer/printer_logo.h"
#include "raw_printer.h"

#define TAG "PrinterSvc"

static constexpr int kTaskPrio = 5;
static constexpr int kTaskStack = 4096;
static constexpr int kMaxRetry = 2;          // 失败自动重试（R8）
static constexpr int kRetryDelayMs = 300;
static constexpr int kFaultThreshold = 3;    // 连续失败 ≥3 → printer_fault

bool PrinterService::Init(int tx_pin, int rx_pin, int baud) {
    if (printer_uart_init(tx_pin, rx_pin, baud) != ESP_OK) {
        ESP_LOGE(TAG, "打印机 UART 初始化失败 (TX=%d RX=%d %d)", tx_pin, rx_pin, baud);
        return false;
    }
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) return false;
    if (xTaskCreate(TaskEntry, "printer_svc", kTaskStack, this, kTaskPrio, &task_) != pdPASS) {
        ESP_LOGE(TAG, "创建打印任务失败");
        return false;
    }
    ready_ = true;
    ESP_LOGI(TAG, "打印机服务就绪 (UART2 TX=%d RX=%d %d 8N1)", tx_pin, rx_pin, baud);
    return true;
}

bool PrinterService::Enqueue(const ReceiptTemplateV3& tmpl, const ReceiptParams& params) {
    if (mutex_ == nullptr) return false;
    if (xSemaphoreTake(mutex_, 0) != pdTRUE) return false;   // 忙即拒（单飞 D4）
    bool accepted = false;
    if (!has_pending_) {
        pending_.tmpl = tmpl;        // 真正的 C++ 拷贝构造，不是字节 memcpy
        pending_.params = params;
        has_pending_ = true;
        accepted = true;
    }
    xSemaphoreGive(mutex_);
    if (accepted && task_ != nullptr) {
        xTaskNotify(task_, kNotifyNewJob, eSetBits);
    }
    return accepted;
}

bool PrinterService::InitDeliveryForTest() {
    if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
    return mutex_ != nullptr;
}

bool PrinterService::TakePendingForTest(ReceiptTemplateV3* tmpl, ReceiptParams* params) {
    if (mutex_ == nullptr) return false;
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) return false;
    bool has = has_pending_;
    if (has) {
        if (tmpl) *tmpl = pending_.tmpl;
        if (params) *params = pending_.params;
        has_pending_ = false;   // Take = 消费语义（与 PrintTask 取走一致）
    }
    xSemaphoreGive(mutex_);
    return has;
}

bool PrinterService::EnqueueTestTicket() {
    // 用当前配置的模板 + 固定样例数据：自检页必须能反映商家名/票头票尾/纸宽/二维码开关
    return Enqueue(LoadTemplateFromSettings(), MakeSampleParams());
}

bool PrinterService::SendOne(const PrintJob& job) {
    raw_printer_t* prn = printer_uart_ctx();
    if (prn == nullptr) return false;

    // 出票前探测纸尽：缺纸就**跳过本张**并告警（抽奖主流程不受影响）。
    // 为什么必须探测：写路径是"只写不读"，缺纸时 UART 写入照样成功 → 会静默打出空票/半张票
    // （真机事故：32KB 的边框票打到一半没纸，系统全程显示 ok=1）
    if (printer_uart_paper_end()) {
        paper_out_.store(true);
        skipped_count_.fetch_add(1);
        ESP_LOGE(TAG, "打印机缺纸 → 本张跳过（抽奖照常，换纸后可重打；累计跳过 %d 张）",
                 skipped_count_.load());
        return false;
    }

    // Logo 按需加载为渲染信息；由模板里的 kLogo 块决定位置（v3 设计 §7.3b）。
    // 任何异常都只跳过 Logo、不阻断打票（D15）。
    LogoRenderInfo logo_info;
    // 缓冲放 PSRAM：高度上限提到 672 点后需 ~47KB，静态 DRAM 太贵。
    // 只分配一次（首次打印时），失败则本轮不出 logo（不影响打票，D15）。
    static uint8_t* logo_buf = nullptr;
    if (logo_buf == nullptr) {
        logo_buf = (uint8_t*)heap_caps_malloc(PrinterLogo::kMaxLogoBytes, MALLOC_CAP_SPIRAM);
        if (logo_buf == nullptr) {
            ESP_LOGW(TAG, "PSRAM 分配 logo 缓冲失败（%d 字节），本轮不出 logo",
                     (int)PrinterLogo::kMaxLogoBytes);
        }
    }
    // v3 起**模板是唯一真相**：模板里有 kLogo 块就出 logo，没有就不出。
    // 原先还叠了一个 v2 遗留的全局开关 logo_enabled，导致"模板里明明放了 logo 块，
    // 打出来却没有图"（真机踩坑：水印版式打出来没有水印）。
    if (LogoStore::Exists()) {
        LogoInfo info = LogoStore::ReadInfo();
        size_t logo_len = 0;
        if (logo_buf != nullptr && info.valid &&
            LogoStore::Load(logo_buf, PrinterLogo::kMaxLogoBytes, &logo_len)) {
            logo_info.data = logo_buf;
            logo_info.len = logo_len;
            logo_info.width_dots = info.width_dots;
            logo_info.height_dots = info.height_dots;
            logo_info.valid = true;
        } else {
            ESP_LOGW(TAG, "logo 不可用（%s），跳过（不阻断打票，D15）",
                     info.error ? info.error : "读取失败");
        }
    }

    std::string bytes = ReceiptRender::Render(job.tmpl, job.params,
                                              logo_info.valid ? &logo_info : nullptr);
    // 打印整帧字节数与 logo 是否参与 —— 排障关键（pdump 只留最后 1024B，看不到帧头的大图）
    ESP_LOGI(TAG, "整帧 %u 字节（logo %s）", (unsigned)bytes.size(),
             logo_info.valid ? "已包含" : "未包含");
    // ⚠ 必须用 raw_send 并给足超时：
    //   raw_printer_raw_write 内部是 append(..., 1000) —— 硬编码 1000ms 超时；
    //   帧超过 512B 暂存缓冲时会直发，而 115200 下 15KB 需 ~1.3s，
    //   于是半途超时掐断 → 打印机收到残缺字节流 → **整张小票乱码**（真机事故根因）。
    // 115200 baud ≈ 11.5 字节/ms，给 3 倍余量 + 500ms 固定开销。
    const uint32_t tx_timeout_ms = 500 + (uint32_t)((uint64_t)bytes.size() * 3 / 11);
    printer_status_t st = raw_printer_raw_send(prn, (const uint8_t*)bytes.data(),
                                               (uint16_t)bytes.size(), tx_timeout_ms);
    if (st == PRINTER_OK) {
        st = raw_printer_flush(prn, tx_timeout_ms);
    }
    // 打印后再探一次：捕捉"打到一半纸用尽"（这张可能不完整，必须让商家知道）
    if (st == PRINTER_OK) {
        if (printer_uart_paper_end()) {
            paper_out_.store(true);
            ESP_LOGW(TAG, "打印过程中纸用尽 —— 本张可能不完整，请换纸后重打");
        } else {
            paper_out_.store(false);
        }
    }
    return st == PRINTER_OK;
}


void PrinterService::PrintTask() {
    int consecutive_fail = 0;
    while (true) {
        // 等新任务通知（无超时阻塞，不占 CPU）
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, kNotifyNewJob, &bits, portMAX_DELAY) != pdTRUE) continue;

        PrintJob job;
        {
            if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) continue;
            job = pending_;          // C++ 拷贝构造（安全）
            pending_ = PrintJob{};   // 释放服务持有的副本
            has_pending_ = false;
            xSemaphoreGive(mutex_);
        }

        bool ok = false;
        for (int attempt = 0; attempt <= kMaxRetry && !ok; ++attempt) {
            ok = SendOne(job);
            if (!ok && attempt < kMaxRetry) {
                ESP_LOGW(TAG, "打印失败（第 %d 次），%d ms 后重试", attempt + 1, kRetryDelayMs);
                vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            }
        }

        if (ok) {
            ok_count_++;
            consecutive_fail = 0;
            if (printer_fault_.exchange(false)) {
                ESP_LOGI(TAG, "打印恢复，清除故障标志");
            }
            ESP_LOGI(TAG, "打印完成 (ok=%d fail=%d)", ok_count_.load(), fail_count_.load());
        } else {
            fail_count_++;
            if (++consecutive_fail >= kFaultThreshold) {
                printer_fault_ = true;
                ESP_LOGE(TAG, "连续 %d 次失败，置 printer_fault（检查纸张/连线）",
                         consecutive_fail);
            }
        }

        if (done_cb_) done_cb_(ok);   // 由 Application 注入 Schedule → 主线程
    }
}

void PrinterService::TaskEntry(void* arg) {
    static_cast<PrinterService*>(arg)->PrintTask();
}
