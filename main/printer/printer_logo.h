#ifndef _PRINTER_LOGO_H_
#define _PRINTER_LOGO_H_

#include <cstdint>
#include <string>
#include <vector>

/// Logo 位图头部（8 字节，LE；设计 §6.4）。
/// ⚠️ 解析必须逐字节（buf[4] | buf[5]<<8），禁止 reinterpret_cast——
///    HTTP body 指针不保证对齐，且字节序必须显式（m-8）。
struct LogoInfo {
    bool valid = false;
    uint16_t width_dots = 0;
    uint16_t height_dots = 0;
    int data_bytes = 0;
    const char* error = nullptr;   // 中文错误原因
};

/// Logo 纯逻辑校验 + GS v 0 指令分包（设计 §6.4 / D14；不碰硬件与文件系统）
class PrinterLogo {
public:
    // 背景图高度上限。原为 256（≈10 行文字），用户反馈"框内空间不足" →
// 抬到 512（≈21 行文字，约 72mm），代价是 logo_buf 静态缓冲增大到 36KB DRAM。
    static constexpr int kMaxLogoHeight = 672;   // 28 行文字（约 100mm）：框内可放 24 行
    static constexpr int kMaxLogoWidthPx = 576;              // 80mm 满宽
    // ⚠ 必须由宽高**推导**，不能各写一份常量（曾漂移：高度抬到 512 而字节数仍按 256 算，
    // 导致"图明明在尺寸内却被拒收"）
    static constexpr int kMaxLogoBytes = 8 + (kMaxLogoWidthPx / 8) * kMaxLogoHeight;

    /// 校验 8 字节头 + 数据长度精确匹配（防截断/溢出）
    static LogoInfo Validate(const uint8_t* buf, size_t len);

    /// 生成 GS v 0 栅格指令，按 max_rows 行分包（默认 24 行/包）。
    /// 每条指令自带完整头（1D 76 30 00 xL xH yL yH + data），互相独立。
    /// 分包理由 = 超时（§6.4）：raw_printer 所有 append 硬编码 timeout_ms=1000，
    /// 12288B@115200≈1067ms 必然超时；1152B≈100ms 留 ~10 倍余量。
    static std::vector<std::string> BuildRasterCommands(const LogoInfo& info,
                                                        const uint8_t* data,
                                                        int max_rows = 24);

    /// 居中前缀/恢复（ESC a 1 / ESC a 0）
    static std::string CenterOn();    // "1B 61 01"
    static std::string CenterOff();   // "1B 61 00"
};

#endif  // _PRINTER_LOGO_H_
