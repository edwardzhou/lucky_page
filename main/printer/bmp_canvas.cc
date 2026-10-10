#include "printer/bmp_canvas.h"

#include <cstring>

#include "esp_log.h"

#define TAG "BmpCanvas"

// 由 CMake EMBED_FILES("assets/font24.bin") 生成的符号
extern const uint8_t font24_bin_start[] asm("_binary_font24_bin_start");
extern const uint8_t font24_bin_end[] asm("_binary_font24_bin_end");

namespace {

struct GlyphEntry {
    uint32_t code;
    uint16_t w;
    uint16_t h;
    uint32_t offset;
};

const uint8_t* g_data = nullptr;      // 点阵数据区
int g_count = 0;
const uint8_t* g_index = nullptr;     // 索引区起点

bool InitFont() {
    static bool tried = false;
    static bool ok = false;
    if (tried) return ok;
    tried = true;
    const uint8_t* p = font24_bin_start;
    const size_t total = (size_t)(font24_bin_end - font24_bin_start);
    if (total < 8 || memcmp(p, "F24A", 4) != 0) {
        ESP_LOGE(TAG, "字库 magic 非法（%zu 字节）", total);
        return false;
    }
    uint32_t count = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) |
                     ((uint32_t)p[7] << 24);
    const size_t index_bytes = (size_t)count * 12;
    if (8 + index_bytes > total) {
        ESP_LOGE(TAG, "字库索引越界（count=%lu）", (unsigned long)count);
        return false;
    }
    g_count = (int)count;
    g_index = p + 8;
    g_data = p + 8 + index_bytes;
    ok = true;
    ESP_LOGI(TAG, "点阵字库就绪：%d 个字形，%zu 字节", g_count, total);
    return ok;
}

const GlyphEntry* FindGlyph(uint32_t code) {
    if (!InitFont()) return nullptr;
    int lo = 0, hi = g_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint8_t* e = g_index + (size_t)mid * 12;
        const uint32_t c = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16) |
                           ((uint32_t)e[3] << 24);
        if (c == code) {
            static GlyphEntry out;
            out.code = c;
            out.w = (uint16_t)(e[4] | (e[5] << 8));
            out.h = (uint16_t)(e[6] | (e[7] << 8));
            out.offset = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) |
                         ((uint32_t)e[11] << 24);
            return &out;
        }
        if (c < code) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

}  // namespace

int Utf8Next(const std::string& s, size_t pos, uint32_t* code) {
    if (pos >= s.size()) return 0;
    const uint8_t c0 = (uint8_t)s[pos];
    if (c0 < 0x80) { *code = c0; return 1; }
    int n = 0;
    uint32_t v = 0;
    if ((c0 & 0xE0) == 0xC0) { n = 2; v = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { n = 3; v = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { n = 4; v = c0 & 0x07; }
    else { *code = 0xFFFD; return 1; }
    if (pos + (size_t)n > s.size()) { *code = 0xFFFD; return 1; }
    for (int i = 1; i < n; ++i) {
        const uint8_t ci = (uint8_t)s[pos + (size_t)i];
        if ((ci & 0xC0) != 0x80) { *code = 0xFFFD; return 1; }
        v = (v << 6) | (ci & 0x3F);
    }
    *code = v;
    return n;
}

bool BmpCanvas::FontAvailable() { return InitFont(); }
int BmpCanvas::FontGlyphCount() { return InitFont() ? g_count : 0; }

bool BmpCanvas::HasGlyph(uint32_t code) {
    if (code == ' ') return true;                 // 空格不需要字形
    return FindGlyph(code) != nullptr;
}

std::string BmpCanvas::MissingGlyphs(const std::string& utf8) {
    std::string miss;
    size_t pos = 0;
    while (pos < utf8.size()) {
        uint32_t code = 0;
        const int n = Utf8Next(utf8, pos, &code);
        if (n == 0) break;
        pos += (size_t)n;
        if (!HasGlyph(code)) {
            // 用 UTF-8 原字节回填，便于日志直接看
            miss.append(utf8, pos - (size_t)n, (size_t)n);
        }
    }
    return miss;
}

BmpCanvas::BmpCanvas(int width_dots, int height_dots)
    : width_(width_dots), height_(height_dots), stride_((width_dots + 7) / 8) {
    if (width_ < 1) width_ = 1;
    if (height_ < 1) height_ = 1;
    buf_.assign((size_t)stride_ * (size_t)height_, 0);
}

void BmpCanvas::SetPixel(int x, int y, bool black) {
    if (!black || x < 0 || y < 0 || x >= width_ || y >= height_) return;
    buf_[(size_t)y * (size_t)stride_ + (size_t)(x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

bool BmpCanvas::GetPixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return false;
    return (buf_[(size_t)y * (size_t)stride_ + (size_t)(x >> 3)] & (0x80 >> (x & 7))) != 0;
}

int BmpCanvas::DrawText(int x, int y, const std::string& utf8, int size, std::string* missing) {
    if (size < 1) size = 1;
    int cx = x;
    size_t pos = 0;
    while (pos < utf8.size()) {
        uint32_t code = 0;
        const int n = Utf8Next(utf8, pos, &code);
        if (n == 0) break;
        pos += (size_t)n;
        if (code == ' ') {
            cx += 12 * size;
            continue;
        }
        const GlyphEntry* g = FindGlyph(code);
        if (g == nullptr) {
            if (missing) missing->append(utf8, pos - (size_t)n, (size_t)n);
            cx += 12 * size;              // 缺字形占半宽，保持后续文字位置可预期
            continue;
        }
        const uint8_t* bits = g_data + g->offset;
        const int gs = (g->w + 7) / 8;
        for (int gy = 0; gy < g->h; ++gy) {
            for (int gx = 0; gx < g->w; ++gx) {
                if ((bits[(size_t)gy * gs + (size_t)(gx >> 3)] & (0x80 >> (gx & 7))) == 0) continue;
                // size 倍放大（最近邻）
                for (int sy = 0; sy < size; ++sy) {
                    for (int sx = 0; sx < size; ++sx) {
                        SetPixel(cx + gx * size + sx, y + gy * size + sy, true);
                    }
                }
            }
        }
        cx += g->w * size;
    }
    return cx;
}

void BmpCanvas::DrawImage1bpp(int x, int y, const uint8_t* data, int src_w, int src_h,
                              int max_width_dots) {
    if (data == nullptr || src_w <= 0 || src_h <= 0) return;
    int dst_w = src_w;
    int dst_h = src_h;
    if (max_width_dots > 0 && dst_w > max_width_dots) {      // 等比缩小（丢弃列）
        dst_h = src_h * max_width_dots / src_w;
        dst_w = max_width_dots;
    }
    const int src_stride = (src_w + 7) / 8;
    for (int dy = 0; dy < dst_h; ++dy) {
        const int sy = src_h > 0 ? dy * src_h / dst_h : 0;
        for (int dx = 0; dx < dst_w; ++dx) {
            const int sx = src_w > 0 ? dx * src_w / dst_w : 0;
            const uint8_t b = data[(size_t)sy * src_stride + (size_t)(sx >> 3)];
            if (b & (0x80 >> (sx & 7))) SetPixel(x + dx, y + dy, true);
        }
    }
}

std::string BmpCanvas::ToRasterCommands(int max_rows) const {
    std::string out;
    if (max_rows <= 0) max_rows = 24;
    for (int row = 0; row < height_; row += max_rows) {
        int rows = height_ - row;
        if (rows > max_rows) rows = max_rows;
        const uint8_t xL = (uint8_t)(stride_ & 0xFF);
        const uint8_t xH = (uint8_t)(stride_ >> 8);
        const uint8_t yL = (uint8_t)(rows & 0xFF);
        const uint8_t yH = (uint8_t)(rows >> 8);
        const uint8_t hdr[] = {0x1D, 0x76, 0x30, 0x00, xL, xH, yL, yH};
        out.append((const char*)hdr, sizeof(hdr));
        out.append((const char*)&buf_[(size_t)row * (size_t)stride_],
                   (size_t)stride_ * (size_t)rows);
    }
    return out;
}
