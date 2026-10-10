#ifndef _UART_DMX_OUTPUT_H_
#define _UART_DMX_OUTPUT_H_

#include "dmx512/dmx_output.h"

#include <driver/gpio.h>
#include <driver/uart.h>

// DMX512 UART 描述（从 Dmx512Service.h 中搬出，独立于业务逻辑）
struct Dmx512Uart {
    uart_port_t port;
    gpio_num_t tx_pin;
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
    gpio_num_t de_pin;
#endif
};

// 基于 UART 硬件的 DMX512 输出实现
// 封装 Break 信号产生和多路并行数据发送，与 Dmx512Service 业务逻辑解耦
class UartDmxOutput : public DmxOutput {
public:
    UartDmxOutput();
    ~UartDmxOutput() override;

    bool Initialize(int uart_count) override;
    void SendBreak() override;
    void SendMulti(const uint8_t bufs[][512], int channel_count, int uart_count) override;

    int GetUartCount() const { return uart_count_; }

private:
    static constexpr int kMaxUartCount = 4;

    Dmx512Uart uarts_[kMaxUartCount];
    int uart_count_ = 0;
    bool initialized_ = false;
};

#endif  // _UART_DMX_OUTPUT_H_
