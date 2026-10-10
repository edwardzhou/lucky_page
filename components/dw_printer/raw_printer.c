/*
 * raw_printer.c - minimal raw ESC/POS core (see raw_printer.h for why)
 *
 * Design notes:
 *   - No dynamic memory: the caller owns the staging buffer, exactly like the
 *     vendor SDK.
 *   - Every appender is "write through": bytes are staged into the buffer and
 *     flushed automatically when the buffer cannot hold the whole command, so
 *     a caller can never overflow it and never has to reason about free space.
 *     printer_text_print() is still the explicit flush the vendor examples use.
 *   - Command bytes were verified against the vendor archive:
 *     ESC a / ESC E / ESC t / GS ! / ESC 3 / ESC J / ESC @ / LF.
 *   - Not reentrant, matching the vendor core: one task drives one printer.
 */

#include "raw_printer.h"

#include <string.h>

/* Maximum bytes one appender can need (GS ! and the longest raw block). */
#define CMD_MAX 4

static void set_error(raw_printer_t *printer, printer_status_t status)
{
    printer->last_error = status;
}

static printer_status_t send_direct(raw_printer_t *printer, const uint8_t *data,
                                    uint16_t size, uint32_t timeout_ms)
{
    int written;

    if (printer == NULL || printer->config.write == NULL ||
        (data == NULL && size > 0)) {
        if (printer != NULL) {
            set_error(printer, PRINTER_ERR_ARG);
        }
        return PRINTER_ERR_ARG;
    }
    if (size == 0) {
        return PRINTER_OK;
    }

    written = printer->config.write(data, size, timeout_ms, printer->config.user);
    if (written < 0 || (uint16_t)written != size) {
        set_error(printer, PRINTER_ERR_IO);
        return PRINTER_ERR_IO;
    }
    return PRINTER_OK;
}

static printer_status_t flush_locked(raw_printer_t *printer, uint32_t timeout_ms)
{
    printer_status_t status;

    if (printer->write_pos == 0) {
        return PRINTER_OK;
    }
    status = send_direct(printer, printer->config.buffer, printer->write_pos,
                         timeout_ms);
    if (status != PRINTER_OK) {
        return status;
    }
    printer->write_pos = 0;
    return PRINTER_OK;
}

printer_status_t raw_printer_flush(raw_printer_t *printer, uint32_t timeout_ms)
{
    if (printer == NULL || printer->config.buffer == NULL) {
        return PRINTER_ERR_ARG;
    }
    return flush_locked(printer, timeout_ms);
}

/**
 * @brief Stage bytes, flushing first when they would not fit.
 *
 * @param timeout_ms Timeout for the automatic flush.
 */
static printer_status_t append(raw_printer_t *printer, const uint8_t *data,
                               uint16_t size, uint32_t timeout_ms)
{
    if (printer == NULL || printer->config.buffer == NULL ||
        (data == NULL && size > 0)) {
        if (printer != NULL) {
            set_error(printer, PRINTER_ERR_ARG);
        }
        return PRINTER_ERR_ARG;
    }
    if (size == 0) {
        return PRINTER_OK;
    }
    if (size > printer->config.buffer_size) {
        /* Larger than the whole buffer: send it straight out. */
        return send_direct(printer, data, size, timeout_ms);
    }
    if (printer->write_pos + size > printer->config.buffer_size) {
        printer_status_t status = flush_locked(printer, timeout_ms);
        if (status != PRINTER_OK) {
            return status;
        }
    }
    memcpy(printer->config.buffer + printer->write_pos, data, size);
    printer->write_pos = (uint16_t)(printer->write_pos + size);
    return PRINTER_OK;
}

/* ------------------------------------------------------------------ */
/* Core                                                                */
/* ------------------------------------------------------------------ */

printer_status_t raw_printer_init(raw_printer_t *printer,
                                  const raw_printer_config_t *config)
{
    if (printer == NULL || config == NULL || config->buffer == NULL ||
        config->buffer_size == 0 || config->write == NULL) {
        return PRINTER_ERR_ARG;
    }
    memcpy(&printer->config, config, sizeof(*config));
    printer->write_pos = 0;
    printer->last_error = PRINTER_OK;
    printer->text_encoding = ENCODING_CP936;
    return PRINTER_OK;
}

printer_status_t raw_printer_last_error(const raw_printer_t *printer)
{
    return (printer == NULL) ? PRINTER_ERR_ARG : printer->last_error;
}

void raw_printer_clear_error(raw_printer_t *printer)
{
    if (printer != NULL) {
        printer->last_error = PRINTER_OK;
    }
}

/* ------------------------------------------------------------------ */
/* Raw bytes                                                           */
/* ------------------------------------------------------------------ */

printer_status_t raw_printer_raw_write(raw_printer_t *printer,
                                       const uint8_t *data, uint16_t size)
{
    return append(printer, data, size, 1000);
}

printer_status_t raw_printer_raw_send(raw_printer_t *printer,
                                      const uint8_t *data, uint16_t size,
                                      uint32_t timeout_ms)
{
    /* Send anything already staged first, so ordering is preserved. */
    printer_status_t status = flush_locked(printer, timeout_ms);
    if (status != PRINTER_OK) {
        return status;
    }
    return send_direct(printer, data, size, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* Transmit buffer                                                     */
/* ------------------------------------------------------------------ */

printer_status_t raw_printer_buffer_clear(raw_printer_t *printer)
{
    if (printer == NULL) {
        return PRINTER_ERR_ARG;
    }
    printer->write_pos = 0;
    return PRINTER_OK;
}

uint16_t raw_printer_buffer_used(const raw_printer_t *printer)
{
    return (printer == NULL) ? 0 : printer->write_pos;
}

uint16_t raw_printer_buffer_available(const raw_printer_t *printer)
{
    if (printer == NULL) {
        return 0;
    }
    return (uint16_t)(printer->config.buffer_size - printer->write_pos);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

printer_status_t raw_printer_text_align(raw_printer_t *printer, align_type_t align)
{
    uint8_t cmd[3];
    if (align != ALIGN_LEFT && align != ALIGN_CENTER && align != ALIGN_RIGHT) {
        if (printer != NULL) {
            set_error(printer, PRINTER_ERR_ARG);
        }
        return PRINTER_ERR_ARG;
    }
    cmd[0] = 0x1B; cmd[1] = 'a'; cmd[2] = (uint8_t)align;
    return append(printer, cmd, sizeof(cmd), 1000);
}

printer_status_t raw_printer_text_bold(raw_printer_t *printer, printer_switch_t mode)
{
    uint8_t cmd[3];
    if (mode != PRINTER_DISABLE && mode != PRINTER_ENABLE) {
        if (printer != NULL) {
            set_error(printer, PRINTER_ERR_ARG);
        }
        return PRINTER_ERR_ARG;
    }
    cmd[0] = 0x1B; cmd[1] = 'E'; cmd[2] = (uint8_t)mode;
    return append(printer, cmd, sizeof(cmd), 1000);
}

printer_status_t raw_printer_text_font_size(raw_printer_t *printer,
                                            uint8_t width, uint8_t height)
{
    uint8_t cmd[4];
    if (width < 1 || width > 8 || height < 1 || height > 8) {
        if (printer != NULL) {
            set_error(printer, PRINTER_ERR_ARG);
        }
        return PRINTER_ERR_ARG;
    }
    cmd[0] = 0x1D; cmd[1] = '!';
    cmd[2] = (uint8_t)(width - 1);
    cmd[3] = (uint8_t)(height - 1);
    return append(printer, cmd, sizeof(cmd), 1000);
}

printer_status_t raw_printer_text_encoding(raw_printer_t *printer,
                                           encoding_type_t encoding)
{
    uint8_t cmd[3];
    if (encoding < 0 || encoding > 43) {
        if (printer != NULL) {
            set_error(printer, PRINTER_ERR_ARG);
        }
        return PRINTER_ERR_ARG;
    }
    cmd[0] = 0x1B; cmd[1] = 't'; cmd[2] = (uint8_t)encoding;
    printer->text_encoding = (uint32_t)encoding;
    return append(printer, cmd, sizeof(cmd), 1000);
}

printer_status_t raw_printer_text_line_space(raw_printer_t *printer, uint8_t dots)
{
    uint8_t cmd[3];
    cmd[0] = 0x1B; cmd[1] = '3'; cmd[2] = dots;
    return append(printer, cmd, sizeof(cmd), 1000);
}

printer_status_t raw_printer_text_utf8(raw_printer_t *printer, const char *text)
{
    size_t len;

    if (printer == NULL || text == NULL) {
        if (printer != NULL) {
            set_error(printer, PRINTER_ERR_ARG);
        }
        return PRINTER_ERR_ARG;
    }
    len = strlen(text);
    if (len == 0) {
        return PRINTER_OK;
    }
    /* The bytes are passed through unchanged: ASCII is always safe, and for
     * multi-byte text the printer needs a matching code page (ESC t) and font
     * resource. Long strings are split over several flushes by append(). */
    while (len > 0) {
        const uint16_t chunk = (len > 0xFFFFu) ? 0xFFFFu : (uint16_t)len;
        printer_status_t status = append(printer, (const uint8_t *)text, chunk, 1000);
        if (status != PRINTER_OK) {
            return status;
        }
        text += chunk;
        len -= chunk;
    }
    return PRINTER_OK;
}

printer_status_t raw_printer_text_raw(raw_printer_t *printer,
                                      const uint8_t *data, uint16_t size)
{
    return append(printer, data, size, 1000);
}

printer_status_t raw_printer_text_newline(raw_printer_t *printer)
{
    static const uint8_t lf = 0x0A;
    return append(printer, &lf, 1, 1000);
}

printer_status_t raw_printer_text_feed_lines(raw_printer_t *printer, uint8_t lines)
{
    static const uint8_t lf = 0x0A;
    printer_status_t status = PRINTER_OK;
    while (lines-- > 0) {
        status = append(printer, &lf, 1, 1000);
        if (status != PRINTER_OK) {
            return status;
        }
    }
    return PRINTER_OK;
}

printer_status_t raw_printer_text_feed_dots(raw_printer_t *printer, uint8_t dots)
{
    uint8_t cmd[3];
    cmd[0] = 0x1B; cmd[1] = 'J'; cmd[2] = dots;
    return append(printer, cmd, sizeof(cmd), 1000);
}

printer_status_t raw_printer_text_print(raw_printer_t *printer, uint32_t timeout_ms)
{
    return raw_printer_flush(printer, timeout_ms);
}
