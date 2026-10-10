#ifndef _GBK_ENCODER_H_
#define _GBK_ENCODER_H_

#include <cstddef>
#include <cstdint>
#include <string>

/// 反查表条目：Unicode 码点 → GBK 编码（cp 升序，供二分查找）
struct GbkReverseEntry {
    uint16_t cp;
    uint16_t gbk;
};

/// UTF-8 → GBK(CP936) 编码器（设计 §6.2，纯逻辑）。
///
/// 表由 scripts/gen_gbk_table.py 生成（gbk_table.cc，78,044 B flash）：
/// - 反查（cp→GBK）覆盖 GB2312 子集 7,478 字——简体常用字 99.9%+
/// - 未收录字符（生僻字/繁体/假名/Emoji）→ '?' 并计数告警，永不静默乱码
class GbkEncoder {
public:
    struct Result {
        std::string bytes;          // GBK 字节流（可直接发给打印机）
        size_t replaced = 0;   // 无解字符，降为 '?'（>0 时应告警）
        size_t fallback = 0;   // 装饰符降级为 ASCII 等价符（═→= ─→- ★→*）
    };

    static Result Encode(const std::string& utf8);

    /// 单码点 → GBK 编码；未收录返回 0。
    static uint16_t LookupGbk(uint32_t codepoint);
};

// ---- 生成表的访问入口（gbk_table.cc 提供） ----
const uint16_t* GbkTableForward();
size_t GbkTableForwardLen();
const GbkReverseEntry* GbkTableReverse();
size_t GbkTableReverseLen();

#endif  // _GBK_ENCODER_H_
