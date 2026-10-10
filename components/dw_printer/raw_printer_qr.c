/*
 * raw_printer_qr.c - ESC/POS QR (GS ( k) command builder.
 * 纯字节拼接，无状态；序列与设计 §6.3 的表格逐字节一致（单测锁死）。
 */
#include "raw_printer_qr.h"

#include <string.h>

printer_status_t raw_printer_qr_build(const char *text, uint8_t module_sz, uint8_t ecc,
                                      uint8_t *out, size_t out_cap, size_t *out_len) {
    if (text == NULL || out == NULL || out_len == NULL) {
        return PRINTER_ERR_ARG;
    }
    size_t k = strlen(text);
    if (k == 0 || k > 124) {          /* pL 是单字节：k+3 必须 <= 255；留裕量 124 */
        return PRINTER_ERR_ARG;
    }
    if (module_sz < 1 || module_sz > 16) {
        return PRINTER_ERR_ARG;
    }
    if (ecc < 48 || ecc > 51) {
        return PRINTER_ERR_ARG;
    }

    /* 五条指令的总长 = 9 + 8 + 8 + (8 + k) + 8 */
    const size_t total = 9 + 8 + 8 + (8 + k) + 8;
    if (out_cap < total) {
        return PRINTER_ERR_NO_SPACE;
    }

    size_t p = 0;

    /* 1) 设模型：GS ( k pL=04 pH=00 cn=31 fn=41 n1=32(model2) n2=00 —— 9 字节 */
    out[p++] = 0x1D; out[p++] = 0x28; out[p++] = 0x6B;
    out[p++] = 0x04; out[p++] = 0x00;
    out[p++] = 0x31; out[p++] = 0x41;
    out[p++] = 0x32; out[p++] = 0x00;

    /* 2) 设模块尺寸：pL=03 —— 8 字节 */
    out[p++] = 0x1D; out[p++] = 0x28; out[p++] = 0x6B;
    out[p++] = 0x03; out[p++] = 0x00;
    out[p++] = 0x31; out[p++] = 0x43;
    out[p++] = module_sz;

    /* 3) 设纠错级别：pL=03 —— 8 字节 */
    out[p++] = 0x1D; out[p++] = 0x28; out[p++] = 0x6B;
    out[p++] = 0x03; out[p++] = 0x00;
    out[p++] = 0x31; out[p++] = 0x45;
    out[p++] = ecc;

    /* 4) 存数据：pL = k + 3（cn/fn/m 三字节计入）—— 8 + k 字节 */
    const uint16_t pl = (uint16_t)(k + 3);
    out[p++] = 0x1D; out[p++] = 0x28; out[p++] = 0x6B;
    out[p++] = (uint8_t)(pl & 0xFF);
    out[p++] = (uint8_t)(pl >> 8);
    out[p++] = 0x31; out[p++] = 0x50; out[p++] = 0x30;
    memcpy(&out[p], text, k);
    p += k;

    /* 5) 打印：pL=03 —— 8 字节 */
    out[p++] = 0x1D; out[p++] = 0x28; out[p++] = 0x6B;
    out[p++] = 0x03; out[p++] = 0x00;
    out[p++] = 0x31; out[p++] = 0x51; out[p++] = 0x30;

    *out_len = p;
    return PRINTER_OK;
}

printer_status_t raw_printer_qr_stage(raw_printer_t *printer, const char *text,
                                      uint8_t module_sz, uint8_t ecc) {
    if (printer == NULL) {
        return PRINTER_ERR_ARG;
    }
    uint8_t buf[160];   /* 最长 124+40，160 足够 */
    size_t len = 0;
    printer_status_t st = raw_printer_qr_build(text, module_sz, ecc, buf, sizeof(buf), &len);
    if (st != PRINTER_OK) {
        return st;
    }
    return raw_printer_raw_write(printer, buf, (uint16_t)len);
}

printer_status_t raw_printer_qr_print(raw_printer_t *printer, uint8_t feed_lines,
                                      uint32_t timeout_ms) {
    if (printer == NULL) {
        return PRINTER_ERR_ARG;
    }
    /* stage() 已暂存"打印"指令；这里只补走纸 + flush */
    printer_status_t st = PRINTER_OK;
    (void)st;
    if (feed_lines > 0) {
        st = raw_printer_text_feed_lines(printer, feed_lines);
        if (st != PRINTER_OK) {
            return st;
        }
    }
    return raw_printer_flush(printer, timeout_ms);
}
