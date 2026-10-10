#ifndef _ESP32S3_LUCKY_CONFIG_H_
#define _ESP32S3_LUCKY_CONFIG_H_

#include <driver/gpio.h>
#include <driver/uart.h>

// ============================================================================
// lucky-page 板级配置（ESP32-S3-N16R8，实物照片 docs/ESP32S3-PIN.png 为准，D19）
//
// ⚠️ 本板 G43/G44 = TX0/RX0，接板载 USB-UART 桥，禁止占用（设计 D18）。
// ⚠️ 19/20 是 USB D−/D+ 系统脚（不是 strapping）；strapping 脚为 0/3/45/46。
// ============================================================================

// ===== DMX512 翻页机输出（UART1 + TTL485） =====
#define DMX512_UART_NUM      UART_NUM_1
#define DMX512_TX_PIN        GPIO_NUM_17
#define DMX512_RX_PIN        GPIO_NUM_18
#define DMX512_DE_PIN        GPIO_NUM_4   // TTL485 DE/RE 方向控制
#define DMX512_CHANNEL_COUNT 512
#define DMX512_UART_COUNT    1            // 单路纯输出

// ===== TTL 热敏打印机（UART2） =====
// 接线（用户实测提供，2026-09-29 变更）：
//   打印机 TX  → GPIO9   （= ESP32 的 RX）
//   打印机 RX  → GPIO10  （= ESP32 的 TX）
//   打印机 CTS → GPIO11  （流控方向见下）
//
// 历史教训：
//   ① G43/G44 = TX0/RX0，接板载 USB-UART 桥，绝对禁止占用。
//   ② 原先照抄 tft_displaye 的 G47/G48 是错的 —— 那个工程是 ESP32-**P4**，
//      在 P4 上只有 47/48 可用；而本板是 S3，G48 多数 S3 板还兼板载 RGB LED。
//   ③ 流控两个方向都叫 "CTS"，接反的表现是「一个字节都不打」：
//        RTS 方向：ESP32 RTS 输出 → 打印机 CTS 输入（打印方向；参考工程推荐）
//        CTS 方向：打印机 RTS 输出 → ESP32 CTS 输入（仅状态回读有意义）
//      故默认关闭流控，用串口命令 `pflow off|cts|rts|both` 现场试出正确方向，
//      避免两个输出端同时驱动一根线。
#define PRINTER_UART_NUM     UART_NUM_2
#define PRINTER_TX_PIN       GPIO_NUM_9
#define PRINTER_RX_PIN       GPIO_NUM_10
#define PRINTER_FLOW_PIN     GPIO_NUM_11   // 打印机 CTS
#define PRINTER_BAUDRATE     115200

// D18 编译期断言：禁占用 USB-UART 桥脚与 USB D±，且 TX/RX 不可同脚
static_assert(PRINTER_TX_PIN != GPIO_NUM_43 && PRINTER_TX_PIN != GPIO_NUM_44,
              "打印机 TX 禁止占用 G43/G44（板载 USB-UART 桥 TX0/RX0）");
static_assert(PRINTER_RX_PIN != GPIO_NUM_43 && PRINTER_RX_PIN != GPIO_NUM_44,
              "打印机 RX 禁止占用 G43/G44（板载 USB-UART 桥 TX0/RX0）");
static_assert(PRINTER_TX_PIN != GPIO_NUM_19 && PRINTER_TX_PIN != GPIO_NUM_20,
              "打印机 TX 禁止占用 G19/G20（USB D-/D+）");
static_assert(PRINTER_RX_PIN != GPIO_NUM_19 && PRINTER_RX_PIN != GPIO_NUM_20,
              "打印机 RX 禁止占用 G19/G20（USB D-/D+）");
static_assert(PRINTER_TX_PIN != PRINTER_RX_PIN, "打印机 TX/RX 不能同脚");

// ===== 抽奖按钮（高电平有效，沿用 page-player） =====
#define BUTTON_MODULE_GPIO   GPIO_NUM_8

// ===== 红外接收（保留，RMT） =====
#define IR_RECEIVER_GPIO     GPIO_NUM_3

// （SD 卡引脚块已删除：lucky-page 从未实现 SD，logo/奖池走 SPIFFS；
//   且原 10/11/9/12 与现在的打印机 9/10/11 冲突）

#endif  // _ESP32S3_LUCKY_CONFIG_H_
