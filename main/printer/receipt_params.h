#ifndef _RECEIPT_PARAMS_H_
#define _RECEIPT_PARAMS_H_

#include <ctime>
#include <string>
#include <utility>
#include <vector>

/// 小票参数取值来源（设计 §7.3b 参数表）
struct ReceiptParams {
    std::string shop;        ///< {shop}
    std::string win_title;   ///< {win_title}
    std::string lose_title;  ///< {lose_title}
    std::string prize;       ///< {prize}
    std::string note;        ///< {note}
    std::string ticket;      ///< {ticket}
    std::string check;       ///< {check}
    std::string validity;    ///< {validity}
    int page = 0;            ///< 0-based；{page} 输出 +1
    bool is_lose = false;    ///< {title} 取 lose_title
    time_t epoch = 0;        ///< 日期时间参数来源（0 = 未同步 → 占位）

    /// 取单个参数值；返回 false = 未知参数
    bool Get(const std::string& key, std::string* out) const;

    /// Web UI 用：全部可用参数（key, 中文说明），顺序即界面展示顺序
    static std::vector<std::pair<std::string, std::string>> List();
};

/// 占位符替换：{key} → 值。未知参数原样保留并计入 *unknown（可为 nullptr）。
/// `{}`、孤立 `{`/`}`、`{ 带空格 }` 不算未知参数（原样保留且不计数）。
std::string SubstituteParams(const std::string& tmpl, const ReceiptParams& p, int* unknown);

#endif  // _RECEIPT_PARAMS_H_
