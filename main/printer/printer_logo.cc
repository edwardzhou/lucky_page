#include "printer/printer_logo.h"

#include <cstring>

LogoInfo PrinterLogo::Validate(const uint8_t* buf, size_t len) {
    LogoInfo info;
    if (buf == nullptr || len < 8) {
        info.error = "数据不足 8 字节（缺头部）";
        return info;
    }
    if (memcmp(buf, "LGO1", 4) != 0) {
        info.error = "magic 不是 LGO1";
        return info;
    }
    // 逐字节 LE 解析（禁止 reinterpret_cast：对齐 + 字节序，m-8）
    uint16_t w = (uint16_t)(buf[4] | ((uint16_t)buf[5] << 8));
    uint16_t h = (uint16_t)(buf[6] | ((uint16_t)buf[7] << 8));
    if (w == 0 || (w % 8) != 0) {
        info.error = "宽度必须是 8 的倍数";
        return info;
    }
    if (w > kMaxLogoWidthPx) {
        info.error = "宽度超过 576（80mm 满宽）";
        return info;
    }
    if (h == 0 || h > kMaxLogoHeight) {
        info.error = "高度必须在 1..256";
        return info;
    }
    size_t expect = 8 + (size_t)(w / 8) * h;
    if (len < expect) {
        info.error = "数据被截断（长度不足）";
        return info;
    }
    if (len > expect) {
        info.error = "数据比声明多（长度不精确）";
        return info;
    }
    info.valid = true;
    info.width_dots = w;
    info.height_dots = h;
    info.data_bytes = (int)((w / 8) * h);
    return info;
}

std::vector<std::string> PrinterLogo::BuildRasterCommands(const LogoInfo& info,
                                                          const uint8_t* data, int max_rows) {
    std::vector<std::string> cmds;
    if (!info.valid || data == nullptr || max_rows <= 0) return cmds;

    const int bytes_per_row = info.width_dots / 8;
    const uint8_t xL = (uint8_t)(bytes_per_row & 0xFF);
    const uint8_t xH = (uint8_t)(bytes_per_row >> 8);

    for (int row = 0; row < info.height_dots; row += max_rows) {
        int rows = info.height_dots - row;
        if (rows > max_rows) rows = max_rows;
        const uint8_t yL = (uint8_t)(rows & 0xFF);
        const uint8_t yH = (uint8_t)(rows >> 8);

        std::string cmd;
        cmd.reserve((size_t)(8 + bytes_per_row * rows));
        cmd.push_back((char)0x1D);
        cmd.push_back((char)0x76);
        cmd.push_back((char)0x30);   // GS v 0
        cmd.push_back((char)0x00);   // m = 0 普通模式
        cmd.push_back((char)xL);
        cmd.push_back((char)xH);
        cmd.push_back((char)yL);
        cmd.push_back((char)yH);
        cmd.append((const char*)(data + (size_t)row * bytes_per_row),
                   (size_t)bytes_per_row * rows);
        cmds.push_back(std::move(cmd));
    }
    return cmds;
}

std::string PrinterLogo::CenterOn() {
    return std::string("\x1B\x61\x01", 3);
}

std::string PrinterLogo::CenterOff() {
    return std::string("\x1B\x61\x00", 3);
}
