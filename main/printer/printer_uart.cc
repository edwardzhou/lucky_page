/*
 * printer_uart.cc - UART2 bring-up + raw_printer 链路回调（板级硬件层）
 *
 * 由 tft_displaye/components/dw_printer/dw_printer_uart.c 裁剪：
 *   保留：write/read/delay_ms 回调、"尊重超时"的 uart_tx_chars 轮询发送
 *   裁剪：flow-control 调试、控制台命令、ploop 自检
 *
 * 关键语义：发送用 uart_tx_chars() 轮询 FIFO 而非阻塞 uart_write_bytes()，
 * 超时按错误返回——打印机掉线不会卡死打印任务。这正是 raw_printer 所有
 * append 硬编码 timeout_ms=1000 能生效的原因（设计 §6.4 / M-2）。
 */
#include "printer/printer_uart.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "boards/esp32s3_lucky/config.h"

#define TAG "printer_uart"

static uint8_t s_tx_buffer[512];   // 512B，对齐 tft_displaye/main/printer_app.c:31
static raw_printer_t s_printer;    // 打印机上下文（缓冲由本文件持有）

/* 当前生效的接线（pswap/pflow 会改，pstatus 会显示） */
static int s_tx_pin = -1;
static int s_rx_pin = -1;
static int s_flow_pin = -1;
static printer_flow_t s_flow_mode = PRINTER_FLOW_OFF;

static uart_hw_flowcontrol_t FlowToHal(printer_flow_t m) {
    switch (m) {
        case PRINTER_FLOW_CTS: return UART_HW_FLOWCTRL_CTS;
        case PRINTER_FLOW_RTS: return UART_HW_FLOWCTRL_RTS;
        case PRINTER_FLOW_CTS_RTS: return UART_HW_FLOWCTRL_CTS_RTS;
        default: return UART_HW_FLOWCTRL_DISABLE;
    }
}

void printer_uart_get_config(int* tx_pin, int* rx_pin, int* flow_pin, printer_flow_t* mode) {
    if (tx_pin) *tx_pin = s_tx_pin;
    if (rx_pin) *rx_pin = s_rx_pin;
    if (flow_pin) *flow_pin = s_flow_pin;
    if (mode) *mode = s_flow_mode;
}

esp_err_t printer_uart_set_flow(int flow_pin, printer_flow_t mode) {
    s_flow_pin = flow_pin;
    s_flow_mode = mode;
    const bool need_rts = (mode == PRINTER_FLOW_RTS || mode == PRINTER_FLOW_CTS_RTS);
    const bool need_cts = (mode == PRINTER_FLOW_CTS || mode == PRINTER_FLOW_CTS_RTS);
    esp_err_t err = uart_set_pin(PRINTER_UART_NUM, s_tx_pin, s_rx_pin,
                                 need_rts ? flow_pin : UART_PIN_NO_CHANGE,
                                 need_cts ? flow_pin : UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_set_hw_flow_ctrl(PRINTER_UART_NUM, FlowToHal(mode), 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "流控=%d（0关/1CTS/2RTS/3双向）脚=%d", (int)mode, flow_pin);
    }
    return err;
}

esp_err_t printer_uart_set_pins(int tx_pin, int rx_pin) {
    s_tx_pin = tx_pin;
    s_rx_pin = rx_pin;
    const bool need_rts = (s_flow_mode == PRINTER_FLOW_RTS || s_flow_mode == PRINTER_FLOW_CTS_RTS);
    const bool need_cts = (s_flow_mode == PRINTER_FLOW_CTS || s_flow_mode == PRINTER_FLOW_CTS_RTS);
    esp_err_t err = uart_set_pin(PRINTER_UART_NUM, tx_pin, rx_pin,
                                 need_rts ? s_flow_pin : UART_PIN_NO_CHANGE,
                                 need_cts ? s_flow_pin : UART_PIN_NO_CHANGE);
    if (err == ESP_OK) ESP_LOGI(TAG, "接线已改: TX=%d RX=%d", tx_pin, rx_pin);
    return err;
}

/* 最近一次发送的字节抓取（pdump 排障用） */
static uint8_t s_last_tx[1024];   // 抓完整一帧（小票 ~424B，原 128B 抓不全）
static size_t s_last_tx_len = 0;

static int prn_write(const uint8_t* data, uint16_t size, uint32_t timeout_ms, void* user) {
    (void)user;
    if (size == 0) return 0;

    size_t keep = size < sizeof(s_last_tx) ? size : sizeof(s_last_tx);
    memcpy(s_last_tx, data + (size - keep), keep);
    s_last_tx_len = keep;

    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t sent = 0;
    // 主动限速：UART 在 115200 下可做到 ~11.5 字节/ms，但热敏机**烧点速度**跟不上
    // （密集位图更慢），一口气灌进去会冲爆打印机输入缓冲 → 丢字节 → 解析错位 → 乱码。
    // 每 512 字节等 FIFO 排空后再停 18ms，把有效速率压到 ~8 字节/ms（配合按帧大小
    // 计算的超时，23KB 帧约 2.9s 发完，超时上限 6.8s，余量充足）。
    constexpr size_t kPaceChunk = 512;
    constexpr uint32_t kPaceDelayMs = 18;
    size_t pace_left = kPaceChunk;
    while (sent < size) {
        int n = uart_tx_chars(PRINTER_UART_NUM, (const char*)data + sent, size - sent);
        if (n > 0) {
            sent += (size_t)n;
            pace_left = (pace_left > (size_t)n) ? (pace_left - (size_t)n) : 0;
            if (pace_left == 0) {
                uart_wait_tx_done(PRINTER_UART_NUM, pdMS_TO_TICKS(300));
                vTaskDelay(pdMS_TO_TICKS(kPaceDelayMs));
                pace_left = kPaceChunk;
            }
            continue;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "UART%d 发送超时：已发 %zu/%u", (int)PRINTER_UART_NUM, sent,
                     (unsigned)size);
            return (sent > 0) ? (int)sent : PRINTER_ERR_IO;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return (int)sent;
}

static int prn_read(uint8_t* data, uint16_t size, uint32_t timeout_ms, void* user) {
    (void)user;
    int n = uart_read_bytes(PRINTER_UART_NUM, data, size, pdMS_TO_TICKS(timeout_ms));
    return (n < 0) ? PRINTER_ERR_IO : n;
}

static void prn_delay_ms(uint32_t ms, void* user) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

const uint8_t* printer_uart_last_tx(size_t* len) {
    if (len) *len = s_last_tx_len;
    return s_last_tx;
}

raw_printer_t* printer_uart_ctx(void) { return &s_printer; }

esp_err_t printer_uart_init(int tx_pin, int rx_pin, int baud) {
    static bool inited = false;
    if (!inited) {
        const uart_config_t cfg = {
            .baud_rate = baud,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            // ESP-IDF v6 新增字段（须按结构体声明顺序）：显式置 0 与原
            // "指定初始化器缺省即 0"完全等价（0 = RTS 阈值不启用、关闭 RX
            // 毛刺滤波、无特殊 flags，见 v6.1 uart.c 对 0 值的处理）
            .rx_flow_ctrl_thresh = 0,
            .rx_glitch_filt_thresh = 0,
            .source_clk = UART_SCLK_DEFAULT,
            .flags = {},
        };
        esp_err_t err = uart_param_config(PRINTER_UART_NUM, &cfg);
        if (err != ESP_OK) return err;
        s_tx_pin = tx_pin;
        s_rx_pin = rx_pin;
        err = uart_set_pin(PRINTER_UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
        if (err != ESP_OK) return err;
        err = uart_driver_install(PRINTER_UART_NUM, 256, 0, 0, NULL, 0);
        if (err != ESP_OK) return err;
        inited = true;
        ESP_LOGI(TAG, "打印机 UART%d 初始化: TX=%d RX=%d %d 8N1", (int)PRINTER_UART_NUM,
                 tx_pin, rx_pin, baud);
    }

    raw_printer_config_t conf = {
        .buffer = s_tx_buffer,
        .buffer_size = sizeof(s_tx_buffer),
        .write = prn_write,
        .read = prn_read,
        .delay_ms = prn_delay_ms,
        .user = NULL,
    };
    if (raw_printer_init(&s_printer, &conf) != PRINTER_OK) {
        return ESP_FAIL;
    }

    /* ESC @ 复位打印机 */
    static const uint8_t kReset[] = {0x1B, '@'};
    raw_printer_raw_send(&s_printer, kReset, sizeof(kReset), 300);
    return ESP_OK;
}


bool PrinterPaperEndFromStatus(uint8_t gs_r1) {
    // GS r 1 应答：bit2..3 = 纸尽传感器（00=有纸，11=纸尽）
    return ((gs_r1 >> 2) & 0x03) == 0x03;
}

bool printer_uart_paper_end() {
    static const uint8_t kGsR1[] = {0x1D, 0x72, 0x01};
    uint8_t buf[8];
    while (uart_read_bytes(PRINTER_UART_NUM, buf, sizeof(buf), 0) > 0) {
        // 清掉残留（上次探测/噪声），避免误判
    }
    uart_write_bytes(PRINTER_UART_NUM, (const char*)kGsR1, sizeof(kGsR1));
    uart_wait_tx_done(PRINTER_UART_NUM, pdMS_TO_TICKS(200));
    const int got = uart_read_bytes(PRINTER_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(300));
    if (got != 1) return false;   // 不支持/无应答 → 不阻断出票
    return PrinterPaperEndFromStatus(buf[0]);
}
