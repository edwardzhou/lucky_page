#include "dmx512/uart_dmx_output.h"

#include "boards/esp32s3_lucky/config.h"

#include "esp_log.h"

#include <cstring>

#define TAG "UartDmxOutput"

UartDmxOutput::UartDmxOutput() {
    memset(uarts_, 0, sizeof(uarts_));

    // lucky-page：单路 UART1 + TTL485（多路/P4 分支已剥离）
    uart_count_ = 1;
    uarts_[0] = {DMX512_UART_NUM, DMX512_TX_PIN, DMX512_DE_PIN};
}

bool UartDmxOutput::Initialize(int uart_count) {
    if (initialized_) {
        return true;
    }

    if (uart_count > kMaxUartCount) {
        ESP_LOGE(TAG, "请求 UART 数量 %d 超过最大值 %d", uart_count, kMaxUartCount);
        return false;
    }
    uart_count_ = uart_count;

    uart_config_t uart_config = {};
    uart_config.baud_rate = 250000;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_2;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    for (int i = 0; i < uart_count_; i++) {
        auto& u = uarts_[i];

        esp_err_t ret = uart_param_config(u.port, &uart_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "UART%d 参数配置失败: %s", u.port, esp_err_to_name(ret));
            return false;
        }

        ret = uart_set_pin(u.port, u.tx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                            UART_PIN_NO_CHANGE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "UART%d 引脚配置失败: %s", u.port, esp_err_to_name(ret));
            return false;
        }

        ret = uart_driver_install(u.port, 256, 2048, 0, nullptr, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "UART%d 驱动安装失败: %s", u.port, esp_err_to_name(ret));
            return false;
        }

#if !defined(CONFIG_IDF_TARGET_ESP32P4)
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << u.de_pin);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        ret = gpio_config(&io_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "UART%d DE GPIO 配置失败: %s", u.port, esp_err_to_name(ret));
            return false;
        }
        gpio_set_level(u.de_pin, 1);
#endif

#if defined(CONFIG_IDF_TARGET_ESP32P4)
        ESP_LOGI(TAG, "UART%d 初始化完成 (TX=GPIO%d)", u.port, u.tx_pin);
#else
        ESP_LOGI(TAG, "UART%d 初始化完成 (TX=GPIO%d, DE=GPIO%d)", u.port, u.tx_pin, u.de_pin);
#endif
    }

    initialized_ = true;
    ESP_LOGI(TAG, "DMX512 输出初始化完成, %d 路 UART", uart_count_);
    return true;
}

void UartDmxOutput::SendBreak() {
    for (int i = 0; i < uart_count_; i++) {
        uart_set_line_inverse(uarts_[i].port, UART_SIGNAL_TXD_INV);
    }
    esp_rom_delay_us(176);

    for (int i = 0; i < uart_count_; i++) {
        uart_set_line_inverse(uarts_[i].port, 0);
    }
    esp_rom_delay_us(12);
}

void UartDmxOutput::SendMulti(const uint8_t bufs[][512], int channel_count, int uart_count) {
    uint8_t start_code = 0x00;
    uint8_t zero_buf[512] = {0};
    for (int i = 0; i < uart_count; i++) {
        auto& u = uarts_[i];
        uart_write_bytes(u.port, &start_code, 1);
        const uint8_t* data = (i < channel_count) ? bufs[i] : zero_buf;
        uart_write_bytes(u.port, (const char*)data, DMX512_CHANNEL_COUNT);
    }
}

UartDmxOutput::~UartDmxOutput() {
    if (!initialized_) {
        return;
    }
    for (int i = 0; i < uart_count_; i++) {
        uart_driver_delete(uarts_[i].port);
    }
    initialized_ = false;
    ESP_LOGI(TAG, "UART 驱动已清理");
}
