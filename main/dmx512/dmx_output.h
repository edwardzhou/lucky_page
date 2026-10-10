#ifndef _DMX_OUTPUT_H_
#define _DMX_OUTPUT_H_

#include <cstddef>
#include <cstdint>

class DmxOutput {
public:
    virtual ~DmxOutput() = default;

    virtual bool Initialize(int uart_count) = 0;

    // 所有路同时产生 DMX512 Break 信号
    virtual void SendBreak() = 0;

    // 向所有路发送数据，uart_count=物理路数(固定4)，
    // channel_count=有效数据路数(Settings 1-4)，超出 channel_count 的路发全零
    virtual void SendMulti(const uint8_t bufs[][512], int channel_count, int uart_count) = 0;
};

#endif  // _DMX_OUTPUT_H_
