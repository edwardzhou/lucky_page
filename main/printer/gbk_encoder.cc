#include "printer/gbk_encoder.h"

#include <algorithm>
#include <vector>

#include "esp_log.h"

#define TAG "GbkEncoder"

static_assert(sizeof(GbkReverseEntry) == 4, "反查条目必须 4 字节");

namespace {

/// 解析一个 UTF-8 字符，返回码点与字节数；非法序列返回 {0, 1}（按 1 字节跳过）
struct Utf8Char {
    uint32_t cp;
    size_t len;
};

Utf8Char DecodeUtf8(const std::string& s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return {c, 1};                       // ASCII
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        return {((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F), 2};
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        return {((uint32_t)(c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) |
                    ((unsigned char)s[i + 2] & 0x3F),
                3};
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        return {((uint32_t)(c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12) |
                    (((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F),
                4};
    }
    return {0, 1};  // 非法/截断：按 1 字节跳过，交由上层替换 '?'
}

}  // namespace

uint16_t GbkEncoder::LookupGbk(uint32_t codepoint) {
    if (codepoint < 0x80) {
        return (uint16_t)codepoint;  // GBK 单字节区即 ASCII
    }
    if (codepoint > 0xFFFF) {
        return 0;  // BMP 之外（Emoji 等），表中不存在
    }
    const GbkReverseEntry* table = GbkTableReverse();
    size_t n = GbkTableReverseLen();
    const GbkReverseEntry key{(uint16_t)codepoint, 0};
    auto it = std::lower_bound(table, table + n, key,
                               [](const GbkReverseEntry& a, const GbkReverseEntry& b) {
                                   return a.cp < b.cp;
                               });
    if (it != table + n && it->cp == (uint16_t)codepoint) {
        return it->gbk;
    }
    return 0;
}

// 装饰字符降级表：这些字符在 GBK 中无码位（属 GB18030 扩展或纯符号），
// 直接送 '?' 会让商家粘贴的花边变成一串问号（真机踩坑：默认模板的 ═ ─ 全变 ?）。
// 映射到视觉等价的 ASCII，保证小票永远可读、可对齐。
struct DecoFallback {
    uint32_t cp;
    const char* ascii;
};
static const DecoFallback kDecoFallbacks[] = {
    {0x2500, "-"}, {0x2501, "-"}, {0x2550, "="}, {0x2551, "|"}, {0x2502, "|"},
    {0x250C, "+"}, {0x2510, "+"}, {0x2514, "+"}, {0x2518, "+"}, {0x251C, "+"},
    {0x2524, "+"}, {0x252C, "+"}, {0x2534, "+"}, {0x253C, "+"}, {0x2560, "+"},
    {0x2563, "+"}, {0x2566, "+"}, {0x2569, "+"}, {0x256C, "+"}, {0x2605, "*"},
    {0x2606, "*"}, {0x25CF, "*"}, {0x25A0, "*"}, {0x25AA, "*"}, {0x25B2, "^"},
    {0x25BC, "v"}, {0x2192, "->"}, {0x2190, "<-"}, {0x2022, "*"}, {0x3010, "["},
    {0x3011, "]"}, {0x300A, "<"}, {0x300B, ">"},
};
static const char* DecoFallbackFor(uint32_t cp) {
    for (const auto& f : kDecoFallbacks) {
        if (f.cp == cp) return f.ascii;
    }
    return nullptr;
}

GbkEncoder::Result GbkEncoder::Encode(const std::string& utf8) {
    Result r;
    r.bytes.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        Utf8Char uc = DecodeUtf8(utf8, i);
        if (uc.cp == 0) {
            r.bytes.push_back('?');
            ++r.replaced;
            i += uc.len;
            continue;
        }
        if (uc.cp < 0x80) {
            r.bytes.push_back((char)uc.cp);           // ASCII 透传
        } else {
            uint16_t gbk = LookupGbk(uc.cp);
            if (gbk == 0) {
                // 先试装饰降级（═→= ─→- ★→* 等），仅真正无解的才降为 '?'
                if (const char* alt = DecoFallbackFor(uc.cp)) {
                    for (const char* p = alt; *p; ++p) r.bytes.push_back(*p);
                    ++r.fallback;
                } else {
                    r.bytes.push_back('?');
                    ++r.replaced;
                }
            } else {
                r.bytes.push_back((char)(gbk >> 8));  // GBK 高字节在前
                r.bytes.push_back((char)(gbk & 0xFF));
            }
        }
        i += uc.len;
    }
    if (r.replaced > 0 || r.fallback > 0) {
        ESP_LOGW(TAG, "GBK 转码：%zu 字符降级为 ASCII 等价符，%zu 字符无解替换为 '?'",
                 r.fallback, r.replaced);
    }
    return r;
}
