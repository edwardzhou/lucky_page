#include "printer/receipt_params.h"

#include <cstdio>
#include <cstring>

namespace {

/// CST-8 时间拆解（与票号 FormatTime 同一口径：epoch + 8h 后按 UTC 解释）
struct Cst8 {
    int year, month, day, hour, minute, second, weekday;   // weekday: 0=周日
    bool valid;
};

Cst8 SplitCst8(time_t epoch) {
    Cst8 r{0, 0, 0, 0, 0, 0, 0, false};
    if (epoch <= 0) return r;
    time_t shifted = epoch + 8 * 3600;
    struct tm tmv {};
    if (gmtime_r(&shifted, &tmv) == nullptr) return r;
    r.year = tmv.tm_year + 1900;
    r.month = tmv.tm_mon + 1;
    r.day = tmv.tm_mday;
    r.hour = tmv.tm_hour;
    r.minute = tmv.tm_min;
    r.second = tmv.tm_sec;
    r.weekday = tmv.tm_wday;
    r.valid = true;
    return r;
}

const char* kWeekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

std::string Fmt2(int v) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02d", v);
    return buf;
}

}  // namespace

bool ReceiptParams::Get(const std::string& key, std::string* out) const {
    if (key == "shop") {
        *out = shop;
    } else if (key == "title") {
        *out = is_lose ? lose_title : win_title;
    } else if (key == "win_title") {
        *out = win_title;
    } else if (key == "lose_title") {
        *out = lose_title;
    } else if (key == "prize") {
        *out = prize;
    } else if (key == "note") {
        *out = note;
    } else if (key == "ticket") {
        *out = ticket;
    } else if (key == "check") {
        *out = check;
    } else if (key == "validity") {
        *out = validity;
    } else if (key == "page") {
        *out = std::to_string(page + 1);
    } else if (key == "nl") {
        *out = "\n";
    } else if (key == "date" || key == "time" || key == "datetime" || key == "year" ||
               key == "month" || key == "day" || key == "hour" || key == "minute" ||
               key == "second" || key == "weekday") {
        Cst8 t = SplitCst8(epoch);
        if (!t.valid) {
            // 时间未同步：给可读占位，绝不打 1970（真机/现场可辨识）
            if (key == "date") *out = "----";
            else if (key == "time") *out = "--:--:--";
            else if (key == "datetime") *out = "---- --:--:--";
            else if (key == "weekday") *out = "---";
            else *out = "--";
            return true;
        }
        if (key == "date") {
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.year, t.month, t.day);
            *out = buf;
        } else if (key == "time") {
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.minute, t.second);
            *out = buf;
        } else if (key == "datetime") {
            char buf[24];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day,
                     t.hour, t.minute, t.second);
            *out = buf;
        } else if (key == "year") {
            *out = std::to_string(t.year);
        } else if (key == "month") {
            *out = Fmt2(t.month);
        } else if (key == "day") {
            *out = Fmt2(t.day);
        } else if (key == "hour") {
            *out = Fmt2(t.hour);
        } else if (key == "minute") {
            *out = Fmt2(t.minute);
        } else if (key == "second") {
            *out = Fmt2(t.second);
        } else {
            *out = kWeekdays[t.weekday % 7];
        }
    } else {
        return false;
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> ReceiptParams::List() {
    return {
        {"shop", "商家名"},        {"title", "标题（中奖/未中奖自动切换）"},
        {"win_title", "中奖标题"}, {"lose_title", "未中奖标题"},
        {"prize", "奖项名"},       {"note", "奖项副标题"},
        {"ticket", "票号"},        {"check", "校验码"},
        {"page", "页码"},          {"validity", "有效期"},
        {"datetime", "当前日期时间"}, {"date", "当前日期"},
        {"time", "当前时间"},      {"year", "年"},
        {"month", "月"},           {"day", "日"},
        {"hour", "时"},            {"minute", "分"},
        {"second", "秒"},          {"weekday", "星期"},
        {"nl", "换行"},
    };
}

std::string SubstituteParams(const std::string& tmpl, const ReceiptParams& p, int* unknown) {
    int unknown_count = 0;
    std::string out;
    out.reserve(tmpl.size() + 32);

    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] != '{') {
            out.push_back(tmpl[i++]);
            continue;
        }
        size_t close = tmpl.find('}', i + 1);
        if (close == std::string::npos) {
            out.push_back(tmpl[i++]);   // 孤立 '{' 原样输出，不计未知
            continue;
        }
        std::string key = tmpl.substr(i + 1, close - i - 1);
        if (key.empty()) {
            out.append("{}");           // 空花括号原样保留，不计未知
            i = close + 1;
            continue;
        }
        std::string val;
        if (p.Get(key, &val)) {
            out.append(val);
        } else if (key.find(' ') != std::string::npos || key.find('\t') != std::string::npos) {
            out.append(tmpl, i, close - i + 1);   // 含空格视为普通文本
        } else {
            out.append(tmpl, i, close - i + 1);   // 未知参数原样保留
            ++unknown_count;
        }
        i = close + 1;
    }
    if (unknown) *unknown = unknown_count;
    return out;
}
