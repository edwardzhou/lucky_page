// page_content.h — 每页打印内容（页面文字 + 图片素材槽号）纯逻辑（2026-10-26）
// 不依赖 NVS/Flash：JSON 编解码 + 校验 + 增删都在这里，可上单测宿主。
#ifndef LUCKY_PAGE_PRINTER_PAGE_CONTENT_H_
#define LUCKY_PAGE_PRINTER_PAGE_CONTENT_H_

#include <string>
#include <vector>

namespace page_content {

/// 单页内容：page 从 0 开始；text 可为空；asset = 素材槽号，-1 表示无图
struct Item {
    int page = 0;
    std::string text;
    int asset = -1;
};

constexpr int kMaxPages = 128;       // 与 total_pages 上限一致
constexpr int kMaxTextBytes = 128;   // 单页文字上限（UTF-8 字节）

/// 序列化（稳定顺序、按 page 升序）
std::string ToJson(const std::vector<Item>& items);

/// 反序列化；失败返回 false 并写 *error（中文）
bool FromJson(const std::string& json, std::vector<Item>* out, std::string* error);

/// 写入/覆盖某页（page 越界或 text 超长返回 false）；空 text 且 asset<0 表示删除该页
bool Upsert(std::vector<Item>* items, int page, const std::string& text, int asset);

/// 查找（未设置返回 nullptr）
const Item* Find(const std::vector<Item>& items, int page);

}  // namespace page_content

#endif  // LUCKY_PAGE_PRINTER_PAGE_CONTENT_H_
