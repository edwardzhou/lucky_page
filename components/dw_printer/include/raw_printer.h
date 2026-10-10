/*
 * raw_printer.h - minimal raw ESP/POS core for the Dongwei thermal printer
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS (replaces the vendor core archive on the ESP32-P4)
 * ---------------------------------------------------------------------------
 * dw-printer-mcu-esp32-idf-sdk-1.0.1/sdk/lib/esp32p/dwPrinterSDK_V1.0.1.a is
 * built with the RISC-V "Zbb" bit-manipulation extension. The ESP32-P4 does not
 * implement Zbb, so the first call that reaches one of those instructions traps
 * with "Illegal instruction" and reboots the chip. Measured on this project:
 *
 *   $ objdump -d sdk/lib/esp32p/dwPrinterSDK_V1.0.1.a | grep -cE 'zext\.h'
 *   72 zext.h, 78 zext.b, 21 minu, 17 max, 13 sh2add, 12 maxu, 5 min,
 *   3 sext.b, 1 rev8        -> 222 Zbb instructions in the archive
 *
 * The very first print path hits one:
 *   printer_write_direct+0x74:  0809c9b3  zext.h  s3,s3
 * and the P4 reports MCAUSE=2 (illegal instruction), MTVAL=0x0809c9b3.
 * (The esp32c and esp32h archives are affected too - 88 zext.b each - so this
 * is an SDK build-flag problem, not a one-off.)
 *
 * This file is therefore a small, self-contained replacement for the parts of
 * the SDK this application uses - text, raw bytes and the transmit buffer -
 * written from the vendor headers and validated byte-for-byte against the
 * vendor implementation by disassembling the archive:
 *
 *   printer_text_align     -> 1B 61 <0|1|2>          ESC a  (confirmed 0x61)
 *   printer_text_bold      -> 1B 45 <0|1>            ESC E
 *   printer_text_encoding  -> 1B 74 <0..43>          ESC t  (43 = CP949)
 *   printer_text_font_size -> 1D 21 <w-1> <h-1>      GS !   (1..8)
 *   newline / feed_lines   -> 0A x N
 *   raw_send({1B,'@'})     -> 1B 40                  ESC @
 *
 * Not implemented here (they need the vendor core): barcode, QR code, chart,
 * receipt, setting and listener. They return PRINTER_ERR_UNSUPPORTED so a
 * future switch back to the vendor archive is a configuration change, not a
 * rewrite. See PRINTER.md for the defect report to send to the vendor.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Status codes - identical values to the vendor printer_status_t      */
/* ------------------------------------------------------------------ */
typedef enum {
    PRINTER_OK = 0,
    PRINTER_ERR_ARG = -1,
    PRINTER_ERR_STATE = -2,
    PRINTER_ERR_NO_SPACE = -3,
    PRINTER_ERR_IO = -4,
    PRINTER_ERR_TIMEOUT = -5,
    PRINTER_ERR_UNSUPPORTED = -6,
} printer_status_t;

typedef enum {
    PRINTER_DISABLE = 0,
    PRINTER_ENABLE = 1,
} printer_switch_t;

typedef enum {
    ALIGN_LEFT = 0,
    ALIGN_CENTER = 1,
    ALIGN_RIGHT = 2,
} align_type_t;

/** Code pages accepted by ESC t (same numbering as the vendor header). */
typedef enum {
    ENCODING_CP936 = 0,
    /* ... vendor values 1..40 omitted, see sdk/include/printer_types.h ... */
    ENCODING_CP950 = 41,
    ENCODING_CP932 = 42,
    ENCODING_CP949 = 43,
} encoding_type_t;

/* ------------------------------------------------------------------ */
/* Link callbacks - identical signatures to the vendor printer_config_t, so the
 * same linker functions work with either core. The user pointer is passed
 * through unchanged.                                                       */
/* ------------------------------------------------------------------ */
/** @brief Send bytes. Returns the number written, or a negative value. */
typedef int (*raw_printer_write_fn)(const uint8_t *data, uint16_t size,
                                    uint32_t timeout_ms, void *user);
/** @brief Read bytes. Returns the count read, or a negative value. */
typedef int (*raw_printer_read_fn)(uint8_t *data, uint16_t size,
                                   uint32_t timeout_ms, void *user);
/** @brief Millisecond delay. */
typedef void (*raw_printer_delay_fn)(uint32_t ms, void *user);

/**
 * @brief Runtime configuration. buffer/buffer_size/write are required.
 *
 * The application owns every buffer; this core never allocates.
 */
typedef struct {
    uint8_t *buffer;
    uint16_t buffer_size;
    raw_printer_write_fn write;
    raw_printer_read_fn read;
    raw_printer_delay_fn delay_ms;
    void *user;
} raw_printer_config_t;

/** @brief Printer context - allocate one per physical printer. */
typedef struct raw_printer {
    raw_printer_config_t config;
    uint16_t write_pos;          /* bytes staged in config.buffer */
    printer_status_t last_error;
    uint32_t text_encoding;
} raw_printer_t;

/* ------------------------------------------------------------------ */
/* Core                                                                */
/* ------------------------------------------------------------------ */

/** @brief Initialize a context. buffer, buffer_size and write are required. */
printer_status_t raw_printer_init(raw_printer_t *printer,
                                  const raw_printer_config_t *config);

/** @brief Send everything staged in the transmit buffer. */
printer_status_t raw_printer_flush(raw_printer_t *printer, uint32_t timeout_ms);

/** @brief Most recent error recorded on this context. */
printer_status_t raw_printer_last_error(const raw_printer_t *printer);

/** @brief Clear the recorded error. */
void raw_printer_clear_error(raw_printer_t *printer);

/* ------------------------------------------------------------------ */
/* Raw bytes                                                           */
/* ------------------------------------------------------------------ */

/** @brief Stage raw command bytes (does not transmit). */
printer_status_t raw_printer_raw_write(raw_printer_t *printer,
                                       const uint8_t *data, uint16_t size);

/** @brief Send raw command bytes immediately, bypassing the buffer. */
printer_status_t raw_printer_raw_send(raw_printer_t *printer,
                                      const uint8_t *data, uint16_t size,
                                      uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* Transmit buffer                                                     */
/* ------------------------------------------------------------------ */

/** @brief Discard all staged bytes. */
printer_status_t raw_printer_buffer_clear(raw_printer_t *printer);

/** @brief Bytes currently staged. */
uint16_t raw_printer_buffer_used(const raw_printer_t *printer);

/** @brief Bytes that can still be staged. */
uint16_t raw_printer_buffer_available(const raw_printer_t *printer);

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

/** @brief Horizontal alignment: ESC a n. */
printer_status_t raw_printer_text_align(raw_printer_t *printer, align_type_t align);

/** @brief Bold on/off: ESC E n. */
printer_status_t raw_printer_text_bold(raw_printer_t *printer, printer_switch_t mode);

/**
 * @brief Character size: GS ! (width-1, height-1).
 *
 * @param width  Horizontal multiplier, 1..8.
 * @param height Vertical multiplier, 1..8.
 */
printer_status_t raw_printer_text_font_size(raw_printer_t *printer,
                                            uint8_t width, uint8_t height);

/** @brief Code page: ESC t n (character set used for the bytes that follow). */
printer_status_t raw_printer_text_encoding(raw_printer_t *printer,
                                           encoding_type_t encoding);

/** @brief Line spacing in dots: ESC 3 n. */
printer_status_t raw_printer_text_line_space(raw_printer_t *printer, uint8_t dots);

/** @brief Append a UTF-8/ASCII string. Bytes are passed through unchanged. */
printer_status_t raw_printer_text_utf8(raw_printer_t *printer, const char *text);

/** @brief Append raw bytes inside the text stream. */
printer_status_t raw_printer_text_raw(raw_printer_t *printer,
                                      const uint8_t *data, uint16_t size);

/** @brief Append a newline. */
printer_status_t raw_printer_text_newline(raw_printer_t *printer);

/** @brief Feed paper by text lines (one 0x0A per line). */
printer_status_t raw_printer_text_feed_lines(raw_printer_t *printer, uint8_t lines);

/** @brief Feed paper by dots: ESC J n. */
printer_status_t raw_printer_text_feed_dots(raw_printer_t *printer, uint8_t dots);

/** @brief Flush the staged text to the printer. */
printer_status_t raw_printer_text_print(raw_printer_t *printer, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
