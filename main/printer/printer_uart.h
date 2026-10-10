#ifndef _PRINTER_UART_H_
#define _PRINTER_UART_H_

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "raw_printer.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 硬件流控方向（两个方向都叫 CTS，接反的表现是「一个字节都不打」）
typedef enum {
    PRINTER_FLOW_OFF = 0,      ///< 关闭（默认，避免两个输出端抢一根线）
    PRINTER_FLOW_CTS = 1,      ///< 打印机 RTS 输出 → ESP32 CTS 输入（仅状态回读有意义）
    PRINTER_FLOW_RTS = 2,      ///< ESP32 RTS 输出 → 打印机 CTS 输入（打印方向）
    PRINTER_FLOW_CTS_RTS = 3,  ///< 双向
} printer_flow_t;

/// UART2 初始化（幂等）并返回打印机上下文（缓冲由本模块持有）。
/// 板级硬件层：引脚/波特率参数化（NVS 可配，免重烧 —— R1/D18）。
esp_err_t printer_uart_init(int tx_pin, int rx_pin, int baud);

/// 运行时改 TX/RX（pswap 用；无需重启）
esp_err_t printer_uart_set_pins(int tx_pin, int rx_pin);

/// 运行时设置流控方向（pflow 用；无需重启）
esp_err_t printer_uart_set_flow(int flow_pin, printer_flow_t mode);

/// 读回当前生效配置（pstatus 显示）
void printer_uart_get_config(int* tx_pin, int* rx_pin, int* flow_pin, printer_flow_t* mode);

/// 打印机上下文（raw_printer_* API 用）
raw_printer_t* printer_uart_ctx(void);

/// 最近一次发送的字节尾部（pdump 排障）
const uint8_t* printer_uart_last_tx(size_t* len);

/// 纸尽判定（**纯函数**，可单测）：GS r 1 应答字节的 bit2..3 == 11 表示纸尽
bool PrinterPaperEndFromStatus(uint8_t gs_r1);

/// 向打印机查询纸状态：true = 缺纸。
/// **无应答时一律返回 false**（不确定就不阻断出票 —— 有些廉价机型不支持该指令）
bool printer_uart_paper_end();

#ifdef __cplusplus
}
#endif

#endif  // _PRINTER_UART_H_
