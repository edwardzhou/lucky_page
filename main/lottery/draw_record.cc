#include "lottery/draw_record.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "settings.h"

// 中国标准时间 = UTC+8（无夏令时，固定偏移即可，避免 TZ 环境依赖）
static constexpr int64_t kCstOffsetSec = 8 * 3600;

std::string DrawRecord::FormatTime(time_t epoch) {
    if (epoch <= 0) return "----";
    time_t cst = epoch + kCstOffsetSec;
    struct tm tm;
    gmtime_r(&cst, &tm);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string DrawRecord::FormatDate(time_t epoch) {
    if (epoch <= 0) return "19700101";
    time_t cst = epoch + kCstOffsetSec;
    struct tm tm;
    gmtime_r(&cst, &tm);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string DrawRecord::NextTicket(time_t now_epoch) {
    auto& settings = Settings::GetInstance();
    std::string today = FormatDate(now_epoch);

    // 跨日归零（NTP 回拨到昨天时不重复流水：只认"前进到新的一天"）
    if (today != settings.serial_date()) {
        settings.set_serial_date(today);
        settings.set_serial_daily(0);
    }
    int serial = settings.serial_daily() + 1;
    settings.set_serial_daily(serial);
    count_ = serial;

    char buf[48];
    if (settings.ticket_prefix().empty()) {
        snprintf(buf, sizeof(buf), "%s-%04d", today.c_str(), serial);
    } else {
        snprintf(buf, sizeof(buf), "%s%s-%04d", settings.ticket_prefix().c_str(),
                 today.c_str(), serial);
    }
    return buf;
}

std::string DrawRecord::Crc16(uint32_t value) {
    // CRC-CCITT (XModem): 初值 0x0000，多项式 0x1021
    uint16_t crc = 0;
    // value 的十进制字符串逐字节
    char digits[16];
    snprintf(digits, sizeof(digits), "%" PRIu32, value);
    for (char* p = digits; *p; ++p) {
        crc ^= (uint16_t)((uint8_t)*p) << 8;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%04X", crc);
    return buf;
}
