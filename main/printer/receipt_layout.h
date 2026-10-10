#ifndef _RECEIPT_LAYOUT_H_
#define _RECEIPT_LAYOUT_H_

#include <string>
#include <vector>

/// 排版工具集（纯逻辑，不碰硬件）。v3 块渲染器复用这些函数。
class ReceiptLayout {
public:
    /// 显示宽度：ASCII=1 列，GBK 双字节字符=2 列（按 UTF-8 原文计）
    static size_t DisplayWidth(const std::string& utf8);

    /// 按显示宽度安全折行（不切断多字节字符）—— 块渲染器复用
    static std::vector<std::string> WrapTo(const std::string& line, size_t cols);

    /// 居中填充（按显示宽度）—— 块渲染器复用
    static std::string Centered(const std::string& line, size_t cols);

    /// UTF-8 → GBK 字节流（未收录字符由编码器降级/替换）
    static std::string ToGbk(const std::string& utf8);

    /// 按对齐方式填充到 cols 显示宽度
    static std::string Aligned(const std::string& line, size_t cols, int align /*0左1中2右*/);

    // 注：v2 的 Render/Substitute 已由 v3 块渲染器（ReceiptRender）取代，
    // 类内只保留可复用的排版工具函数（设计 §7.3b）。
};

#endif  // _RECEIPT_LAYOUT_H_
