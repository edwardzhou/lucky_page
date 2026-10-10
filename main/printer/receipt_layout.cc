#include "printer/receipt_layout.h"

#include <algorithm>
#include <vector>

#include "printer/gbk_encoder.h"
#include "raw_printer_qr.h"

// ----------------------------------------------------------------------------
// DisplayWidth：按 UTF-8 解码，ASCII=1 列、其余（按 GBK 双字节）= 2 列
// ----------------------------------------------------------------------------
size_t ReceiptLayout::DisplayWidth(const std::string& utf8) {
    size_t width = 0, i = 0;
    while (i < utf8.size()) {
        unsigned char c = (unsigned char)utf8[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > utf8.size()) len = 1;  // 截断的非法序列按 1 字节
        width += (c < 0x80) ? 1 : 2;
        i += len;
    }
    return width;
}

namespace {

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

/// 按显示宽度安全折行（不切断多字节字符；§7.3 规则 2）
}  // namespace

std::vector<std::string> ReceiptLayout::WrapTo(const std::string& line, size_t cols) {
    std::vector<std::string> out;
    if (ReceiptLayout::DisplayWidth(line) <= cols) {
        out.push_back(line);
        return out;
    }
    std::string cur;
    size_t cur_w = 0, i = 0;
    while (i < line.size()) {
        unsigned char c = (unsigned char)line[i];
        size_t len = 1, w = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > line.size()) len = 1;
        w = (c < 0x80) ? 1 : 2;
        if (cur_w + w > cols) {
            out.push_back(cur);
            cur.clear();
            cur_w = 0;
        }
        cur += line.substr(i, len);
        cur_w += w;
        i += len;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/// 居中填充（按显示宽度，§7.3 规则 1）
std::string ReceiptLayout::Centered(const std::string& line, size_t cols) {
    size_t w = ReceiptLayout::DisplayWidth(line);
    if (w >= cols) return line;
    size_t total = cols - w;
    size_t left = total / 2;
    size_t right = total - left;
    return std::string(left, ' ') + line + std::string(right, ' ');
}

std::string ReceiptLayout::ToGbk(const std::string& utf8) {
    auto r = GbkEncoder::Encode(utf8);
    return std::string(r.bytes.begin(), r.bytes.end());
}

std::string ReceiptLayout::Aligned(const std::string& line, size_t cols, int align) {
    size_t w = DisplayWidth(line);
    if (w >= cols) return line;
    size_t pad = cols - w;
    if (align == 1) {                       // 居中
        size_t left = pad / 2;
        return std::string(left, ' ') + line + std::string(pad - left, ' ');
    }
    if (align == 2) return std::string(pad, ' ') + line;   // 右对齐
    return line + std::string(pad, ' ');                    // 左对齐
}

