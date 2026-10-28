// sd_name_codec.cc — SD 文件名 UTF-8 ↔ GBK 转换
#include "storage/sd_name_codec.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "printer/gbk_encoder.h"

namespace sd_name_codec {
namespace {

// GBK → Unicode 反查表（首次使用时按 GBK 码排序建一次；约 7.5k 项）
const std::vector<std::pair<uint16_t, uint16_t>>& Reverse() {
    static std::vector<std::pair<uint16_t, uint16_t>> table;
    if (table.empty()) {
        const GbkReverseEntry* src = GbkTableReverse();
        const size_t n = GbkTableReverseLen();
        table.reserve(n);
        for (size_t i = 0; i < n; ++i) table.push_back({src[i].gbk, src[i].cp});
        std::sort(table.begin(), table.end());
    }
    return table;
}

void AppendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back((char)cp);
    } else if (cp < 0x800) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out->push_back((char)(0xF0 | (cp >> 18)));
        out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::string ToFs(const std::string& utf8) {
    return GbkEncoder::Encode(utf8).bytes;
}

std::string FromFs(const std::string& gbk) {
    const auto& table = Reverse();
    std::string out;
    for (size_t i = 0; i < gbk.size();) {
        const unsigned char b = (unsigned char)gbk[i];
        if (b < 0x80) {
            out.push_back((char)b);
            ++i;
            continue;
        }
        if (i + 1 >= gbk.size()) {
            out.push_back('?');
            break;
        }
        const uint16_t code = (uint16_t)((b << 8) | (unsigned char)gbk[i + 1]);
        auto it = std::lower_bound(table.begin(), table.end(),
                                   std::make_pair(code, (uint16_t)0));
        if (it != table.end() && it->first == code) {
            AppendUtf8(&out, it->second);
        } else {
            out.push_back('?');
        }
        i += 2;
    }
    return out;
}

}  // namespace sd_name_codec
