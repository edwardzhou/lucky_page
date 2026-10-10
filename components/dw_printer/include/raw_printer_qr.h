/*
 * raw_printer_qr.h - ESC/POS QR code commands (GS ( k) for the Dongwei
 * thermal printer, layered on raw_printer.
 *
 * 字节序列已按 Epson ESC/POS 参考手册 GS ( k Function 165/167/169/180/181
 * 逐字节核对（设计 §6.3）：
 *   设模型     1D 28 6B 04 00 31 41 32 00     (9 字节；n2=00 不可省)
 *   设模块尺寸 1D 28 6B 03 00 31 43 <n>       (n = 1..16)
 *   设纠错级别 1D 28 6B 03 00 31 45 <n>       (48=L 49=M 50=Q 51=H)
 *   存数据     1D 28 6B <pL> <pH> 31 50 30 <d1..dk>   (pL = k + 3)
 *   打印       1D 28 6B 03 00 31 51 30
 *
 * ⚠️ 易错点（已用单测锁死）：设模型指令必须 9 字节含 n2=00；漏掉 n2 会让
 * 打印机把下一条指令的首字节当 n2 吞掉，后续指令整体错位。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raw_printer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** QR 纠错级别（ESC t 同款编号）。 */
enum qr_ecc {
    QR_ECC_L = 48,  /* 7%  */
    QR_ECC_M = 49,  /* 15% */
    QR_ECC_Q = 50,  /* 25% */
    QR_ECC_H = 51,  /* 30% */
};

/**
 * @brief 把五条 QR 指令拼进 caller 提供的缓冲区（纯函数，不碰打印机）。
 *
 * @param text       QR 内容（ASCII；长度须 < 124，超长返回 PARAM 错）
 * @param module_sz  模块尺寸 1..16（本设计默认 6）
 * @param ecc        qr_ecc 枚举值
 * @param out        输出缓冲
 * @param out_cap    缓冲容量；不足返回 NO_SPACE
 * @param out_len    [out] 实际产出字节数
 * @return PRINTER_OK 或负错误码
 */
printer_status_t raw_printer_qr_build(const char *text, uint8_t module_sz, uint8_t ecc,
                                      uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * @brief 生成【全部五条】QR 指令（含末尾的"打印"指令）并暂存到发送缓冲。
 *        发送需再调 raw_printer_flush() 或 raw_printer_qr_print()。
 */
printer_status_t raw_printer_qr_stage(raw_printer_t *printer, const char *text,
                                      uint8_t module_sz, uint8_t ecc);

/**
 * @brief flush 暂存的 QR 指令并走纸（打印指令已由 stage 暂存，这里不重发）。
 */
printer_status_t raw_printer_qr_print(raw_printer_t *printer, uint8_t feed_lines,
                                      uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
