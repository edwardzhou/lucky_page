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

// ===== 抽奖按钮（**低电平触发**：内部上拉 + 按下接地）=====
// 用户确认接线为低电平触发；早期注释写的 active_high 是错的（已更正）。
#define BUTTON_MODULE_GPIO           GPIO_NUM_8
#define BUTTON_MODULE_ACTIVE_LEVEL   0      // 0 = 按下为低（上拉/接地）
#define BUTTON_MODULE_DEBOUNCE_MS    30     // 软件去抖窗口：窗口内的抖动全部吞掉
#define BUTTON_MODULE_COOLDOWN_MS    1000   // 触发限流：**1 秒内只接受一次**（用户要求）

static_assert(BUTTON_MODULE_ACTIVE_LEVEL == 0 || BUTTON_MODULE_ACTIVE_LEVEL == 1,
              "按钮触发电平只能是 0 或 1");
static_assert(BUTTON_MODULE_DEBOUNCE_MS >= 1 && BUTTON_MODULE_DEBOUNCE_MS < BUTTON_MODULE_COOLDOWN_MS,
              "去抖窗口必须 ≥1ms 且小于限流间隔");
static_assert(BUTTON_MODULE_COOLDOWN_MS == 1000, "抽奖按钮限流按需求固定为 1 秒");

// ===== 红外接收（保留，RMT） =====
#define IR_RECEIVER_GPIO     GPIO_NUM_3

// ===== 恢复默认配置按钮（2026-10-10）=====
// 上拉输入、按下为低电平、**长按 5 秒**触发"恢复网络默认配置"（AP 名/AP 密码/已保存网络/IP 模式）。
// 选 GPIO5 的理由：非 strapping（避开 G0/G3/G45/G46）、无板上外设、无 USB/Flash/PSRAM 复用
// （N16R8 的 Flash/PSRAM 占 26..37）。
// ⚠ 曾考虑 GPIO4，但它已是 DMX512 的 DE（发送使能）—— 不可复用。
#define RESET_BUTTON_GPIO      GPIO_NUM_5
#define RESET_BUTTON_LONG_MS   5000
// 开机守卫：复位会清掉已保存 WiFi 凭据，代价高 → 开机后这段时间内忽略该按键
// （防止上电瞬间电平不稳被驱动判定为"持续按住"而误清配置）
#define RESET_BUTTON_BOOT_GUARD_MS  30000

static_assert(RESET_BUTTON_GPIO != BUTTON_MODULE_GPIO, "恢复按钮不能与抽奖按钮同脚");
static_assert(RESET_BUTTON_GPIO != IR_RECEIVER_GPIO, "恢复按钮不能与红外接收同脚");
static_assert(RESET_BUTTON_GPIO != PRINTER_TX_PIN && RESET_BUTTON_GPIO != PRINTER_RX_PIN &&
              RESET_BUTTON_GPIO != PRINTER_FLOW_PIN, "恢复按钮不能与打印机三脚冲突");
static_assert(RESET_BUTTON_GPIO != DMX512_TX_PIN && RESET_BUTTON_GPIO != DMX512_RX_PIN &&
              RESET_BUTTON_GPIO != DMX512_DE_PIN, "恢复按钮不能与 DMX512（含 DE=G4）冲突");
static_assert(RESET_BUTTON_GPIO != GPIO_NUM_19 && RESET_BUTTON_GPIO != GPIO_NUM_20,
              "恢复按钮禁止占用 G19/G20（USB D-/D+）");
static_assert(RESET_BUTTON_GPIO != GPIO_NUM_43 && RESET_BUTTON_GPIO != GPIO_NUM_44,
              "恢复按钮禁止占用 G43/G44（板载 USB-UART 桥）");
static_assert(RESET_BUTTON_LONG_MS == 5000, "恢复按钮长按阈值按需求固定为 5 秒");

// （SD 卡引脚块已删除：lucky-page 从未实现 SD，logo/奖池走 SPIFFS；
//   且原 10/11/9/12 与现在的打印机 9/10/11 冲突）

// ===== 欢迎音喇叭（I2S → MAX98357A 数字功放，2026-10-25）=====
// 引脚对齐 xiaozhi-esp32 小智 S3（bread-compact-wifi）喇叭配置（specs/2026-10-25）：
//   ESP32 BCLK=G15 → 功放 BCLK；LRCK=G16 → 功放 LRC；DOUT=G7 → 功放 DIN
//   功放 SD 脚悬空 = (L+R)/2 单声道输出，3V3/GND 供电
// 格式：16kHz / 16bit / 单声道（素材 main/assets/welcome.wav，tools/make_welcome_wav.sh）
#define SPEAKER_I2S_BCLK_PIN   GPIO_NUM_15
#define SPEAKER_I2S_LRCK_PIN   GPIO_NUM_16
#define SPEAKER_I2S_DOUT_PIN   GPIO_NUM_7
#define SPEAKER_SAMPLE_RATE    24000

// 喇叭三脚冲突护栏（新增功能脚必须核对全表，规则见 §1.5/§1.7）
#define SPEAKER_PINS_OK(p) \
    ((p) != PRINTER_TX_PIN && (p) != PRINTER_RX_PIN && (p) != PRINTER_FLOW_PIN && \
     (p) != DMX512_TX_PIN && (p) != DMX512_RX_PIN && (p) != DMX512_DE_PIN && \
     (p) != BUTTON_MODULE_GPIO && (p) != RESET_BUTTON_GPIO && (p) != IR_RECEIVER_GPIO && \
     (p) != GPIO_NUM_19 && (p) != GPIO_NUM_20 && (p) != GPIO_NUM_43 && (p) != GPIO_NUM_44)
static_assert(SPEAKER_PINS_OK(SPEAKER_I2S_BCLK_PIN), "喇叭 BCLK 与现有外设/禁用脚冲突");
static_assert(SPEAKER_PINS_OK(SPEAKER_I2S_LRCK_PIN), "喇叭 LRCK 与现有外设/禁用脚冲突");
static_assert(SPEAKER_PINS_OK(SPEAKER_I2S_DOUT_PIN), "喇叭 DOUT 与现有外设/禁用脚冲突");
static_assert(SPEAKER_I2S_BCLK_PIN != SPEAKER_I2S_LRCK_PIN &&
              SPEAKER_I2S_BCLK_PIN != SPEAKER_I2S_DOUT_PIN &&
              SPEAKER_I2S_LRCK_PIN != SPEAKER_I2S_DOUT_PIN,
              "喇叭三个 I2S 脚不能互相同脚");
static_assert(SPEAKER_SAMPLE_RATE == 24000, "采样率须与 welcome.wav 素材一致（24kHz，对齐小智）");


#endif  // _ESP32S3_LUCKY_CONFIG_H_
