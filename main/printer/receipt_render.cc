#include "printer/receipt_render.h"

#include <algorithm>
#include <climits>
#include <vector>

#include "printer/printer_logo.h"
#include "esp_log.h"

#include "printer/bmp_canvas.h"
#include "printer/receipt_layout.h"
#include "settings.h"

namespace {

void Push(std::string* out, const uint8_t* data, size_t len) {
    out->append(reinterpret_cast<const char*>(data), len);
}

// ESC/POS 指令（与 receipt_layout.cc 同一套）
const uint8_t kReset[] = {0x1B, 0x40};                 // ESC @
const uint8_t kCodePage[] = {0x1B, 0x74, 0x00};        // ESC t 0（CP437/GBK 变体）
const uint8_t kSize1x[] = {0x1D, 0x21, 0x00};          // GS ! 0
const uint8_t kSize2x[] = {0x1D, 0x21, 0x11};          // GS ! 17（2 倍宽高）
const uint8_t kBoldOn[] = {0x1B, 0x45, 0x01};          // ESC E 1
const uint8_t kBoldOff[] = {0x1B, 0x45, 0x00};         // ESC E 0
const uint8_t kLf = 0x0A;

/// 画布布局：把块映射到"绝对行 → 该行上按 col 排序的块列表"。
/// row < 0 的块走流式（跟随游标）；row >= 0 的块钉在指定行。
/// 同一行的多个块输出时用空格补齐到各自 col —— 这就是"左 Logo 右店名"。
int AlignIndex(Align a) {
    switch (a) {
        case Align::kCenter: return 1;
        case Align::kRight: return 2;
        default: return 0;
    }
}

/// 画布排版内核：把模板排成「行 → 该行上的片段（按 col 从左到右）」。
///
/// 这是**打印与预览共用的唯一布局实现** —— 两条路径各写一份是 bug 之源
/// （曾经：预览只取折行后的第一行、二维码预览超出纸宽 69 列）。
///   - row >= 0 的块钉在绝对行；row < 0 的块走流式游标
///   - 折行产生的续行占用 base_row+1、+2 …
///   - feed 块占用 feed_lines 个空行
struct CanvasPiece {
    int col = 0;
    std::string text;              ///< line/separator 的文本；qr/logo 仅供预览展示
    const ReceiptBlock* block = nullptr;
};

struct Canvas {
    std::vector<std::vector<CanvasPiece>> rows;
    int unknown_params = 0;
};

Canvas BuildCanvas(const ReceiptTemplateV3& t, const ReceiptParams& p,
                   const PageRenderInfo* page) {
    Canvas cv;
    int cursor = 0;   // 流式游标
    auto ensure = [&](int r) {
        while ((int)cv.rows.size() <= r) cv.rows.emplace_back();
    };
    for (const auto& b : t.blocks) {
        const int col = (b.col < 0 || b.col >= t.paper_cols) ? 0 : b.col;
        const size_t avail = (size_t)(t.paper_cols - col);
        std::vector<std::string> lines;
        // 行跨度：2 倍字号每行占 2 行纸；二维码按其实际高度占行
        // （必须与编辑器一致，否则画布位置与纸面不符 —— 真机问题：大字被裁、二维码压住后续行）
        int extra_rows_per_line = 1;
        int u = 0;
        switch (b.type) {
            case BlockType::kPageText: {
                const size_t box = (b.size == 2) ? (avail / 2) : avail;
                std::string text = page != nullptr ? page->text : std::string();
                if (text.empty()) continue;          // 当页没写字 → 整块跳过
                for (auto& w : ReceiptLayout::WrapTo(SubstituteParams(text, p, &u),
                                                     box == 0 ? 1 : box)) {
                    lines.push_back(ReceiptLayout::Aligned(w, box == 0 ? 1 : box,
                                                           AlignIndex(b.align)));
                }
                if (lines.empty()) lines.push_back("");
                break;
            }
            case BlockType::kPageImage:
                if (page == nullptr || !page->has_image()) continue;   // 无图 → 跳过
                lines.push_back("[页面图片]");
                break;
            case BlockType::kLine: {
                const size_t box = (b.size == 2) ? (avail / 2) : avail;
                std::string text = SubstituteParams(b.text, p, &u);
                for (auto& w : ReceiptLayout::WrapTo(text, box == 0 ? 1 : box)) {
                    lines.push_back(ReceiptLayout::Aligned(w, box == 0 ? 1 : box,
                                                           AlignIndex(b.align)));
                }
                if (lines.empty()) lines.push_back("");
                break;
            }
            case BlockType::kSeparator:
                lines.push_back(std::string(avail, b.sep_char));
                break;
            case BlockType::kQr: {
                std::string text = "▩ " + SubstituteParams(b.text, p, &u);
                // 必须按剩余宽度折行，否则预览会超出纸宽（真机踩坑：69 > 32）
                for (auto& w : ReceiptLayout::WrapTo(text, avail == 0 ? 1 : avail)) {
                    lines.push_back(w);
                }
                if (lines.empty()) lines.push_back("▩");
                break;
            }
            case BlockType::kLogo:
                lines.push_back("[Logo]");
                break;
            case BlockType::kFeed: {
                const int n = std::max(1, b.feed_lines);
                int base = (b.row >= 0) ? b.row : cursor;
                ensure(base + n - 1);
                cursor = base + n;
                if (b.row >= 0) cv.unknown_params += 0;
                continue;
            }
        }
        cv.unknown_params += u;
        if (b.type == BlockType::kLine && b.size == 2) extra_rows_per_line = 2;
        int span = (int)lines.size() * extra_rows_per_line;
        if (b.type == BlockType::kPageImage && page != nullptr && page->has_image()) {
            span = std::max(1, ((int)page->height_dots + 23) / 24);
        }
        if (b.type == BlockType::kQr) {
            // 二维码实际高度 ≈ 29 模块 × 模块尺寸（与渲染器 r += qr_rows 的预留一致）
            const int qr_sz = (b.qr_size >= 1 && b.qr_size <= 16) ? b.qr_size : 6;
            span = std::max(1, (29 * qr_sz + 23) / 24);
        }
        int base = (b.row >= 0) ? b.row : cursor;
        ensure(base + span - 1);
        for (size_t i = 0; i < lines.size(); ++i) {
            cv.rows[base + (int)(i * extra_rows_per_line)].push_back({col, lines[i], &b});
            // 2 倍字号的续行也放一个**空片段**（同一块）：一是让预览知道该行宽度按半幅算，
            // 二是让行式渲染在续行补一个换行 —— 2 倍字要占 2 行纸
            if (extra_rows_per_line == 2) {
                cv.rows[base + (int)(i * 2) + 1].push_back({col, std::string(), &b});
            }
        }
        cursor = base + span;
    }
    return cv;
}

/// 把一行上的片段拼成整行文本（按 col 补空格；不足纸宽补满）
///
/// ⚠️ 2 倍字号的字符在纸上占 2 列，所以含 2 倍行的**逻辑宽度只有纸宽的一半**；
/// 若按整纸宽补空格，打印时会变成 64 列而溢出（真机踩坑）。
std::string ComposeRow(const std::vector<CanvasPiece>& pieces, int paper_cols) {
    std::vector<CanvasPiece> sorted = pieces;
    std::sort(sorted.begin(), sorted.end(),
              [](const CanvasPiece& a, const CanvasPiece& b) { return a.col < b.col; });
    std::string line;
    int cursor_col = 0;
    for (const auto& pc : sorted) {
        if (pc.col > cursor_col) {
            line.append((size_t)(pc.col - cursor_col), ' ');
            cursor_col = pc.col;
        }
        line.append(pc.text);
        cursor_col += (int)ReceiptLayout::DisplayWidth(pc.text);
    }
    int target = paper_cols;
    for (const auto& pc : sorted) {
        if (pc.block != nullptr && pc.block->type == BlockType::kLine &&
            pc.block->size == 2) {
            target = paper_cols / 2;
            break;
        }
    }
    const size_t w = ReceiptLayout::DisplayWidth(line);
    if ((int)w < target) line.append((size_t)target - w, ' ');
    return line;
}


/// 生成 line 块折行后的每行文本。
/// 画布语义（设计 §7.3b）：
///   - `col` 为左边距，先在行首补 col 个空格
///   - 可用宽度 = 纸宽 - col（size=2 时再减半，因为每字符占 2 列）
///   - `align` 在**可用宽度内**对齐（不再是整张纸）
std::vector<std::string> LineRows(const ReceiptBlock& b, const ReceiptParams& p, int paper_cols,
                                  int* unknown) {
    const int col = (b.col < 0 || b.col >= paper_cols) ? 0 : b.col;
    const size_t avail = (size_t)(paper_cols - col);
    const size_t box = (b.size == 2) ? (avail / 2) : avail;   // 2 倍宽：每字符占 2 列
    if (box == 0) return {};
    std::string text = SubstituteParams(b.text, p, unknown);
    std::vector<std::string> rows;
    for (auto& w : ReceiptLayout::WrapTo(text, box)) {
        rows.push_back(std::string((size_t)col, ' ') + ReceiptLayout::Aligned(w, box,
                                                                             AlignIndex(b.align)));
    }
    if (rows.empty()) rows.push_back(std::string((size_t)col, ' '));
    return rows;
}

}  // namespace

/// 输出一行的**文字部分**（跳过背景块与图块，它们由调用方处理）
void AppendTextRow(std::string* out, const std::vector<CanvasPiece>& pieces, int paper_cols,
                   int* cursor_col_out) {
    static const uint8_t kBoldOnL[] = {0x1B, 0x45, 0x01};
    static const uint8_t kBoldOffL[] = {0x1B, 0x45, 0x00};
    static const uint8_t kSize2xL[] = {0x1D, 0x21, 0x11};
    static const uint8_t kSize1xL[] = {0x1D, 0x21, 0x00};
    std::vector<CanvasPiece> sorted = pieces;
    std::sort(sorted.begin(), sorted.end(),
              [](const CanvasPiece& a, const CanvasPiece& b) { return a.col < b.col; });
    int cursor_col = 0;
    for (const auto& pc : sorted) {
        const ReceiptBlock& b = *pc.block;
        if (b.bg || b.type == BlockType::kQr || b.type == BlockType::kLogo ||
            b.type == BlockType::kPageImage) continue;
        if (pc.col > cursor_col) {
            out->append(ReceiptLayout::ToGbk(std::string((size_t)(pc.col - cursor_col), ' ')));
            cursor_col = pc.col;
        }
        if (b.type == BlockType::kLine && b.bold) out->append((const char*)kBoldOnL, 3);
        if (b.type == BlockType::kLine && b.size == 2) out->append((const char*)kSize2xL, 3);
        out->append(ReceiptLayout::ToGbk(pc.text));
        if (b.type == BlockType::kLine && b.size == 2) out->append((const char*)kSize1xL, 3);
        if (b.type == BlockType::kLine && b.bold) out->append((const char*)kBoldOffL, 3);
        cursor_col += (int)ReceiptLayout::DisplayWidth(pc.text);
    }
    if (cursor_col_out) *cursor_col_out = cursor_col;
}

/// 混合模式：**只有与背景图重叠的那些行**走位图，其余仍走打印机文字。
/// 为什么：整幅位图 15KB 会把打印机输入缓冲冲爆（真机事故：整张小票乱码）；
/// 而混合模式通常只有 3~6KB，落在已知可用区间，且叠加效果不受影响。
/// ⚠ 必须显式设行距 ESC 3 24，使"1 行文字 = 24 点"与位图行高一致，否则错位。
std::string RenderHybrid(const ReceiptTemplateV3& t, const ReceiptParams& p,
                         const LogoRenderInfo* logo, std::string* missing,
                         const PageRenderInfo* page) {
    const Canvas cv = BuildCanvas(t, p, page);
    const int rows = (int)cv.rows.size();

    // ① 计算图形区：[gfx_first, gfx_last]，由所有背景块覆盖的行决定
    int gfx_first = INT32_MAX;
    int gfx_last = -1;
    for (int r = 0; r < rows; ++r) {
        for (const auto& pc : cv.rows[r]) {
            if (!pc.block->bg && pc.block->type != BlockType::kPageImage) continue;
            int span = 1;
            if (pc.block->type == BlockType::kLogo && logo != nullptr && logo->valid) {
                span = std::max(1, ((int)logo->height_dots + 23) / 24);
            }
            if (pc.block->type == BlockType::kPageImage && page != nullptr && page->has_image()) {
                span = std::max(1, ((int)page->height_dots + 23) / 24);
            }
            if (r < gfx_first) gfx_first = r;
            if (r + span - 1 > gfx_last) gfx_last = r + span - 1;
        }
    }
    if (gfx_last < 0) return std::string();          // 无背景块（不该走到这里）

    std::string out;
    static const uint8_t kInit[] = {0x1B, 0x40};                 // ESC @
    static const uint8_t kCodePageZ[] = {0x1B, 0x74, 0x00};      // ESC t 0
    static const uint8_t kLineSpacing[] = {0x1B, 0x33, 24};      // ESC 3 24：1 行 = 24 点
    static const uint8_t kLineSpacingDefault[] = {0x1B, 0x32};   // ESC 2 恢复默认行距
    static const uint8_t kAlignLeft[] = {0x1B, 0x61, 0x00};
    out.append((const char*)kInit, sizeof(kInit));
    out.append((const char*)kCodePageZ, sizeof(kCodePageZ));
    out.append((const char*)kLineSpacing, sizeof(kLineSpacing));
    out.append((const char*)kAlignLeft, sizeof(kAlignLeft));

    // feed 行统一留到最后输出（**必须在二维码之后**，否则裁剪空行会跑到码上面，
    // 真机现象："二维码与边框空了太多间隔"）
    int feed_total = 0;
    for (const auto& b : t.blocks) {
        if (b.type == BlockType::kFeed) feed_total += std::max(0, b.feed_lines);
    }

    int r = 0;
    while (r < rows) {
        // ① 该行有二维码 → **就地**打印（不再拖到最后），并跳过它实际占用的行数，
        //    否则码与上方内容之间会凭空多出若干空行。
        const ReceiptBlock* qr_here = nullptr;
        for (const auto& pc : cv.rows[r]) {
            if (pc.block->type == BlockType::kQr && !pc.block->text.empty()) {
                qr_here = pc.block;
                break;
            }
        }
        if (qr_here != nullptr) {
            const int col = (qr_here->col < 0 || qr_here->col >= t.paper_cols) ? 0 : qr_here->col;
            std::string content = SubstituteParams(qr_here->text, p, nullptr);
            if (col > 0) {
                const uint8_t pos[] = {0x1B, 0x24, (uint8_t)(col * 12), 0x00};
                out.append((const char*)pos, sizeof(pos));
            }
            static const uint8_t kModel[] = {0x1D, 0x28, 0x6B, 0x04, 0x00, 0x31, 0x41, 0x32, 0x00};
            out.append((const char*)kModel, sizeof(kModel));
            const uint8_t size_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43,
                                        (uint8_t)qr_here->qr_size};
            out.append((const char*)size_cmd, sizeof(size_cmd));
            const uint8_t ecc_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45,
                                       (uint8_t)qr_here->qr_ecc};
            out.append((const char*)ecc_cmd, sizeof(ecc_cmd));
            const size_t pL = content.size() + 3;
            const uint8_t store[] = {0x1D, 0x28, 0x6B, (uint8_t)(pL & 0xFF),
                                     (uint8_t)((pL >> 8) & 0xFF), 0x31, 0x50, 0x30};
            out.append((const char*)store, sizeof(store));
            out.append(content);
            static const uint8_t kPrint[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30};
            out.append((const char*)kPrint, sizeof(kPrint));
            // 二维码实际高度 ≈ 29 模块 × 模块尺寸；按行数跳过，避免多余空行
            const int qr_rows = std::max(1, (29 * (int)qr_here->qr_size + 23) / 24);
            r += qr_rows;
            continue;
        }
        // ② feed 行：跳过（最后统一输出）
        bool only_feed = !cv.rows[r].empty();
        for (const auto& pc : cv.rows[r]) {
            if (pc.block->type != BlockType::kFeed) { only_feed = false; break; }
        }
        if (only_feed) { ++r; continue; }

        if (r < gfx_first || r > gfx_last) {
            AppendTextRow(&out, cv.rows[r], t.paper_cols, nullptr);
            out.push_back('\n');
            ++r;
            continue;
        }
        // ② 图形区：合成一张位图带
        const int band_rows = gfx_last - r + 1;
        BmpCanvas band(t.paper_cols * 12, band_rows * 24);
        for (int rr = r; rr <= gfx_last && rr < rows; ++rr) {
            const int y = (rr - r) * 24;
            for (const auto& pc : cv.rows[rr]) {
                const ReceiptBlock& b = *pc.block;
                const int x = pc.col * 12;
                if (b.type == BlockType::kQr) continue;          // 二维码另用指令打印
                if (b.type == BlockType::kPageImage) {
                    if (page != nullptr && page->has_image()) {
                        int x0 = x;
                        const int maxw = t.paper_cols * 12 - x;
                        if (b.align == Align::kCenter) {
                            x0 = x + std::max(0, (maxw - page->width_dots) / 2);
                        }
                        band.DrawImage1bpp(x0, y, page->image + 8, page->width_dots,
                                           page->height_dots, maxw);
                    }
                    continue;
                }
                if (b.type == BlockType::kLogo) {
                    if (b.bg && logo != nullptr && logo->valid && logo->data != nullptr &&
                        logo->len >= 8) {
                        int x0 = x;
                        const int maxw = t.paper_cols * 12 - x;
                        if (b.align == Align::kCenter) {
                            x0 = x + std::max(0, (maxw - (int)logo->width_dots) / 2);
                        }
                        band.DrawImage1bpp(x0, y, logo->data + 8, (int)logo->width_dots,
                                           (int)logo->height_dots, maxw);
                    }
                    continue;
                }
                if (b.type == BlockType::kSeparator) {
                    for (int dx = 0; dx < t.paper_cols * 12 - x; dx += 3) {
                        band.SetPixel(x + dx, y + 21, true);
                    }
                    continue;
                }
                if (b.type == BlockType::kFeed) continue;
                band.DrawText(x, y, pc.text, b.size == 2 ? 2 : 1, missing);
            }
        }
        out.append(band.ToRasterCommands(24));
        r = gfx_last + 1;
    }
    out.append((const char*)kLineSpacingDefault, sizeof(kLineSpacingDefault));

    // ③ 末尾补走纸空行（裁剪用）—— 必须在二维码之后
    for (int i = 0; i < feed_total; ++i) out.push_back('\n');
    return out;
}

/// 把 1-bit 位图的连续若干行直接裁出来输出（花边模式用）。
/// 行在源数据里是连续的，因此切片是 O(1) 偏移 + 分块 ≤24 行（GS v 0 单块上限友好）。
std::string RasterRows(const uint8_t* data, int width_dots, int row0, int row_count) {
    std::string out;
    if (data == nullptr || width_dots <= 0 || row_count <= 0) return out;
    const int stride = width_dots / 8;
    for (int r = 0; r < row_count; r += 24) {
        const int rows = std::min(24, row_count - r);
        const uint8_t xL = (uint8_t)(stride & 0xFF);
        const uint8_t xH = (uint8_t)(stride >> 8);
        const uint8_t yL = (uint8_t)(rows & 0xFF);
        const uint8_t yH = (uint8_t)(rows >> 8);
        const uint8_t hdr[] = {0x1D, 0x76, 0x30, 0x00, xL, xH, yL, yH};
        out.append((const char*)hdr, sizeof(hdr));
        out.append((const char*)(data + (size_t)(row0 + r) * (size_t)stride),
                   (size_t)stride * (size_t)rows);
    }
    return out;
}

/// 花边模式（设计 §7.3c）：顶部花边 + 中间不限行数文字 + 底部花边。
/// 帧小（约 3~4KB）、打印快（约 1.5s），代价是画不出左右竖线——
/// 需要四边完整边框或文字压图时请用画布模式。
std::string RenderBanner(const ReceiptTemplateV3& t, const ReceiptParams& p,
                         const LogoRenderInfo* logo, std::string* missing,
                         const PageRenderInfo* page) {
    const int top_dots = std::max(0, t.banner_top_rows) * 24;
    const int bottom_dots = std::max(0, t.banner_bottom_rows) * 24;
    // 数据长度也要校验：按 width/height 声明的数据不足时切片会越界（防御）
    const bool have_img = logo != nullptr && logo->valid && logo->data != nullptr &&
                          logo->width_dots > 0 && (logo->width_dots % 8) == 0 &&
                          logo->len >= 8 + (size_t)(logo->width_dots / 8) * logo->height_dots &&
                          logo->height_dots >= top_dots + bottom_dots;
    if (!have_img) {
        // 图缺失/过矮：退化为纯文字（花边不打印），绝不打乱版式
        ESP_LOGW("ReceiptRender", "花边模式：图缺失或高度不足（需 %d 点），本张不打印花边",
                 top_dots + bottom_dots);
    }

    static const uint8_t kInit[] = {0x1B, 0x40};
    static const uint8_t kCodePageZ[] = {0x1B, 0x74, 0x00};
    static const uint8_t kLineSpacing[] = {0x1B, 0x33, 24};   // 行距 = 24 点，保证与花边对齐
    static const uint8_t kAlignLeft[] = {0x1B, 0x61, 0x00};
    std::string out;
    out.append((const char*)kInit, sizeof(kInit));
    out.append((const char*)kCodePageZ, sizeof(kCodePageZ));
    out.append((const char*)kLineSpacing, sizeof(kLineSpacing));
    out.append((const char*)kAlignLeft, sizeof(kAlignLeft));

    const int img_w = have_img ? (int)logo->width_dots : 0;
    if (have_img && top_dots > 0) {
        out += RasterRows(logo->data + 8, img_w, 0, top_dots);
    }

    const Canvas cv = BuildCanvas(t, p, page);
    int feed_total = 0;
    for (const auto& b : t.blocks) {
        if (b.type == BlockType::kFeed) feed_total += std::max(0, b.feed_lines);
    }
    const int rows = (int)cv.rows.size();
    int r = 0;
    while (r < rows) {
        // 二维码就地打印（同画布模式：不留多余空行）
        const ReceiptBlock* qr_here = nullptr;
        for (const auto& pc : cv.rows[r]) {
            if (pc.block->type == BlockType::kQr && !pc.block->text.empty()) {
                qr_here = pc.block;
                break;
            }
        }
        if (qr_here != nullptr) {
            const int col = (qr_here->col < 0 || qr_here->col >= t.paper_cols) ? 0 : qr_here->col;
            std::string content = SubstituteParams(qr_here->text, p, nullptr);
            if (col > 0) {
                const uint8_t pos[] = {0x1B, 0x24, (uint8_t)(col * 12), 0x00};
                out.append((const char*)pos, sizeof(pos));
            }
            static const uint8_t kModel[] = {0x1D, 0x28, 0x6B, 0x04, 0x00, 0x31, 0x41, 0x32, 0x00};
            out.append((const char*)kModel, sizeof(kModel));
            const uint8_t size_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43,
                                        (uint8_t)qr_here->qr_size};
            out.append((const char*)size_cmd, sizeof(size_cmd));
            const uint8_t ecc_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45,
                                       (uint8_t)qr_here->qr_ecc};
            out.append((const char*)ecc_cmd, sizeof(ecc_cmd));
            const size_t pL = content.size() + 3;
            const uint8_t store[] = {0x1D, 0x28, 0x6B, (uint8_t)(pL & 0xFF),
                                     (uint8_t)((pL >> 8) & 0xFF), 0x31, 0x50, 0x30};
            out.append((const char*)store, sizeof(store));
            out.append(content);
            static const uint8_t kPrint[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30};
            out.append((const char*)kPrint, sizeof(kPrint));
            r += std::max(1, (29 * (int)qr_here->qr_size + 23) / 24);
            continue;
        }
        // feed 行留到最后
        bool only_feed = !cv.rows[r].empty();
        for (const auto& pc : cv.rows[r]) {
            if (pc.block->type != BlockType::kFeed) { only_feed = false; break; }
        }
        if (only_feed) { ++r; continue; }
        AppendTextRow(&out, cv.rows[r], t.paper_cols, nullptr);
        out.push_back('\n');
        ++r;
    }

    if (have_img && bottom_dots > 0) {
        out += RasterRows(logo->data + 8, img_w, (int)logo->height_dots - bottom_dots, bottom_dots);
    }
    for (int i = 0; i < feed_total; ++i) out.push_back('\n');
    if (missing) missing->clear();
    return out;
}

/// 页面模式（ESC/POS page mode）拼接：把背景图与文字按坐标放到同一页，
/// 重叠处的点由打印机做"或"运算 —— 这是行式热敏机上**唯一**能真正叠加的方式。
///
/// 关键指令：
///   ESC L                     进入页面模式
///   ESC W xL xH yL yH dxL dxH dyL dyH   设定页面打印区（宽×高，单位点）
///   ESC $ nL nH               绝对水平位置（点，自左边距）
///   GS  $ nL nH               绝对垂直位置（页面模式专用）
///   ESC FF                    打印页面区内容     ESC S  返回标准模式
///
/// 页面模式并非所有机型都支持（不支持时可能整页错乱），因此由设置
/// `printer_pagemode` 控制，默认关闭，现场可用 `pbg` 命令打样张验证。
namespace {
constexpr int kColDots = 12;    // 1 个字符列 = 12 点（FONT A 12×24）
constexpr int kRowDots = 24;    // 1 行文字 = 24 点

void PushPos(uint8_t* out, size_t* n, int dots, bool vertical) {
    if (vertical) {
        out[(*n)++] = 0x1D; out[(*n)++] = 0x24;   // GS $
    } else {
        out[(*n)++] = 0x1B; out[(*n)++] = 0x24;   // ESC $
    }
    out[(*n)++] = (uint8_t)(dots & 0xFF);
    out[(*n)++] = (uint8_t)((dots >> 8) & 0xFF);
}

std::string RenderPageMode(const ReceiptTemplateV3& t, const ReceiptParams& p,
                           const LogoRenderInfo* logo, const PageRenderInfo* page) {
    std::string out;
    // 先算总行数（背景不占流式行，只影响页面高度）
    int total_rows = 0;
    int max_bg_row = 0;
    {
        int cursor = 0;
        for (const auto& b : t.blocks) {
            int span = 1;
            if (b.type == BlockType::kLine) {
                const int bcol = (b.col < 0 || b.col >= t.paper_cols) ? 0 : b.col;
                const size_t avail = (size_t)(t.paper_cols - bcol);
                const size_t box = (b.size == 2) ? (avail / 2) : avail;
                span = (int)std::max<size_t>(
                    1, ReceiptLayout::WrapTo(SubstituteParams(b.text, p, nullptr), box == 0 ? 1 : box)
                           .size());
            } else if (b.type == BlockType::kFeed) {
                span = std::max(1, b.feed_lines);
            }
            const int r = (b.row >= 0) ? b.row : cursor;
            if (b.bg) {
                max_bg_row = std::max(max_bg_row, r + 6);   // 二维码约 6 行高
                cursor = std::max(cursor, r);
            } else {
                cursor = r + span;
            }
        }
        total_rows = std::max(cursor, max_bg_row) + 1;
    }
    const int page_w = t.paper_cols * kColDots;
    const int page_h = total_rows * kRowDots;

    uint8_t buf[16];
    size_t n = 0;
    Push(&out, kReset, sizeof(kReset));
    Push(&out, kCodePage, sizeof(kCodePage));
    buf[n++] = 0x1B; buf[n++] = 0x4C;                       // ESC L 进入页面模式
    buf[n++] = 0x1B; buf[n++] = 0x57;                       // ESC W 打印区
    buf[n++] = 0x00; buf[n++] = 0x00;                       // x = 0
    buf[n++] = 0x00; buf[n++] = 0x00;                       // y = 0
    buf[n++] = (uint8_t)(page_w & 0xFF); buf[n++] = (uint8_t)((page_w >> 8) & 0xFF);
    buf[n++] = (uint8_t)(page_h & 0xFF); buf[n++] = (uint8_t)((page_h >> 8) & 0xFF);
    Push(&out, buf, n);

    // ① 背景层先放（logo 位图 / 二维码）
    for (const auto& b : t.blocks) {
        if (!b.bg) continue;
        const int col = (b.col < 0 || b.col >= t.paper_cols) ? 0 : b.col;
        const int row = std::max(0, b.row);
        n = 0;
        PushPos(buf, &n, col * kColDots, false);
        PushPos(buf, &n, row * kRowDots, true);
        Push(&out, buf, n);
        if (b.type == BlockType::kLogo && logo != nullptr && logo->valid &&
            logo->data != nullptr && logo->len >= 8) {
            LogoInfo info;
            info.valid = true;
            info.width_dots = (uint16_t)logo->width_dots;
            info.height_dots = (uint16_t)logo->height_dots;
            info.data_bytes = (int)logo->len - 8;
            for (const auto& cmd : PrinterLogo::BuildRasterCommands(info, logo->data + 8)) {
                out.append(cmd);
            }
        } else if (b.type == BlockType::kQr) {
            std::string content = SubstituteParams(b.text, p, nullptr);
            if (!content.empty()) {
                static const uint8_t kModel[] = {0x1D, 0x28, 0x6B, 0x04, 0x00, 0x31, 0x41, 0x32, 0x00};
                Push(&out, kModel, sizeof(kModel));
                const uint8_t size_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43, (uint8_t)b.qr_size};
                Push(&out, size_cmd, sizeof(size_cmd));
                const uint8_t ecc_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45, (uint8_t)b.qr_ecc};
                Push(&out, ecc_cmd, sizeof(ecc_cmd));
                const size_t pL = content.size() + 3;
                const uint8_t store[] = {0x1D, 0x28, 0x6B, (uint8_t)(pL & 0xFF),
                                         (uint8_t)((pL >> 8) & 0xFF), 0x31, 0x50, 0x30};
                Push(&out, store, sizeof(store));
                out.append(content);
                static const uint8_t kPrint[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30};
                Push(&out, kPrint, sizeof(kPrint));
            }
        }
    }

    // ② 前景层：文字/分隔线，按行列坐标定位后输出
    const Canvas cv = BuildCanvas(t, p, page);
    for (size_t r = 0; r < cv.rows.size(); ++r) {
        for (const auto& pc : cv.rows[r]) {
            if (pc.block->bg) continue;
            const ReceiptBlock& b = *pc.block;
            n = 0;
            PushPos(buf, &n, pc.col * kColDots, false);
            PushPos(buf, &n, (int)r * kRowDots, true);
            Push(&out, buf, n);
            if (b.type == BlockType::kQr || b.type == BlockType::kLogo) {
                continue;   // 前景图不重复输出（图已由背景层或普通路径处理）
            }
            if (b.bold) Push(&out, kBoldOn, sizeof(kBoldOn));
            if (b.size == 2) Push(&out, kSize2x, sizeof(kSize2x));
            out.append(ReceiptLayout::ToGbk(pc.text));
            if (b.size == 2) Push(&out, kSize1x, sizeof(kSize1x));
            if (b.bold) Push(&out, kBoldOff, sizeof(kBoldOff));
        }
    }
    buf[0] = 0x1B; buf[1] = 0x0C;   // ESC FF 打印页面
    Push(&out, buf, 2);
    buf[0] = 0x1B; buf[1] = 0x53;   // ESC S 返回标准模式
    Push(&out, buf, 2);
    return out;
}

bool HasBackground(const ReceiptTemplateV3& t) {
    for (const auto& b : t.blocks) {
        if (b.bg) return true;
    }
    return false;
}

}  // namespace

/// 图形模式：把整张小票（文字 + 图）合成到**同一张位图**再整幅输出（GS v 0）。
/// 这是本机唯一能实现"文字真正压在图上"的方式（页面模式实测不支持，评审 §19）。
/// 版式与文字模式完全一致：字形尺寸对齐 FONT A（ASCII 12×24、汉字 24×24）。
std::string RenderGraphics(const ReceiptTemplateV3& t, const ReceiptParams& p,
                           const LogoRenderInfo* logo, std::string* missing,
                           const PageRenderInfo* page) {
    const Canvas cv = BuildCanvas(t, p, page);
    const int rows = std::max<int>(1, (int)cv.rows.size());
    BmpCanvas canvas(t.paper_cols * 12, rows * 24);
    bool any_qr = false;
    for (size_t r = 0; r < cv.rows.size(); ++r) {
        for (const auto& pc : cv.rows[r]) {
            const ReceiptBlock& b = *pc.block;
            const int x = pc.col * 12;
            const int y = (int)r * 24;
            if (b.type == BlockType::kQr) { any_qr = true; continue; }   // 二维码走打印机指令
            if (b.type == BlockType::kPageImage) {
                if (page != nullptr && page->has_image()) {
                    int x0 = x;
                    int maxw = t.paper_cols * 12 - x;
                    if (b.align == Align::kCenter) {
                        const int w = std::min<int>(page->width_dots, maxw);
                        x0 = x + std::max(0, (maxw - w) / 2);
                    }
                    canvas.DrawImage1bpp(x0, y, page->image + 8, page->width_dots,
                                         page->height_dots, maxw);
                }
                continue;
            }
            if (b.type == BlockType::kLogo) {
                if (logo != nullptr && logo->valid && logo->data != nullptr && logo->len >= 8) {
                    int x0 = x;
                    int maxw = t.paper_cols * 12 - x;
                    if (b.align == Align::kCenter) {
                        const int w = std::min<int>((int)logo->width_dots, maxw);
                        x0 = x + std::max(0, (maxw - w) / 2);
                    }
                    canvas.DrawImage1bpp(x0, y, logo->data + 8, (int)logo->width_dots,
                                         (int)logo->height_dots, maxw);
                }
                continue;
            }
            if (b.type == BlockType::kFeed || b.type == BlockType::kSeparator) {
                if (b.type == BlockType::kSeparator) {
                    canvas.DrawText(x, y, std::string(pc.text.size() / 1, ' '), 1, nullptr);
                    // 分隔线用位图画：逐点画横线更直观
                    for (int dx = 0; dx < t.paper_cols * 12 - x; dx += 3) {
                        canvas.SetPixel(x + dx, y + 21, true);
                    }
                }
                continue;
            }
            // 文字：按列对齐绘制（与文字模式同一套 col/align 计算）
            canvas.DrawText(x, y, pc.text, b.size == 2 ? 2 : 1, missing);
        }
    }
    std::string out;
    out.append("\x1B\x40", 2);          // ESC @
    out.append("\x1B\x74\x00", 3);     // ESC t 0
    out.append(canvas.ToRasterCommands(24));
    // 二维码：位图模式下不参与叠加，统一排在图形区之后（用户已确认二维码不做背景）
    if (any_qr) {
        for (const auto& b : t.blocks) {
            if (b.type != BlockType::kQr) continue;
            std::string content = SubstituteParams(b.text, p, nullptr);
            if (content.empty()) continue;
            const int col = (b.col < 0 || b.col >= t.paper_cols) ? 0 : b.col;
            if (col > 0) {
                const uint8_t pos[] = {0x1B, 0x24, (uint8_t)(col * 12), 0x00};
                out.append((const char*)pos, sizeof(pos));
            }
            static const uint8_t kModel[] = {0x1D, 0x28, 0x6B, 0x04, 0x00, 0x31, 0x41, 0x32, 0x00};
            out.append((const char*)kModel, sizeof(kModel));
            const uint8_t size_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43, (uint8_t)b.qr_size};
            out.append((const char*)size_cmd, sizeof(size_cmd));
            const uint8_t ecc_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45, (uint8_t)b.qr_ecc};
            out.append((const char*)ecc_cmd, sizeof(ecc_cmd));
            const size_t pL = content.size() + 3;
            const uint8_t store[] = {0x1D, 0x28, 0x6B, (uint8_t)(pL & 0xFF),
                                     (uint8_t)((pL >> 8) & 0xFF), 0x31, 0x50, 0x30};
            out.append((const char*)store, sizeof(store));
            out.append(content);
            static const uint8_t kPrint[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30};
            out.append((const char*)kPrint, sizeof(kPrint));
        }
        out.append("\x0A\x0A", 2);
    }
    return out;
}

std::string ReceiptRender::Render(const ReceiptTemplateV3& t, const ReceiptParams& p,
                                  const LogoRenderInfo* logo, const PageRenderInfo* page) {
    // 有背景层元素时走页面模式（真叠加）；打印机不支持时可用设置退回标准模式
    // ---- 按模版模式分派（设计 §7.3c）----
    const TemplateMode emode = t.EffectiveMode();
    if (emode == TemplateMode::kBanner) {
        std::string missing;
        std::string banner = RenderBanner(t, p, logo, &missing, page);
        ESP_LOGI("ReceiptRender", "花边模式合成完成（%u 字节：上 %d 行 / 下 %d 行花边）",
                 (unsigned)banner.size(), t.banner_top_rows, t.banner_bottom_rows);
        return banner;
    }
    if (emode == TemplateMode::kFlow && HasBackground(t)) {
        // 校验已拦，这里兜底：flow 模式忽略背景块（不因配置怪异而崩）
        ESP_LOGW("ReceiptRender", "flow 模式含背景块，已忽略背景");
    }

    if (emode == TemplateMode::kCanvas && HasBackground(t)) {
        // 有背景水印块 → 需要叠加能力，按优先级选择实现：
        //   ① 页面模式（由打印机做"或"叠加；本机实测不支持，见评审 §19）
        //   ② 图形模式（我们自己把整票合成成位图 —— 本机走这条）
        //   ③ 都不可用 → 退回文字模式（图与字上下分层，绝不乱码）
        if (Settings::GetInstance().printer_pagemode()) {
            return RenderPageMode(t, p, logo, page);
        }
        if (BmpCanvas::FontAvailable()) {
            std::string missing;
            std::string gfx = RenderHybrid(t, p, logo, &missing, page);
            if (!gfx.empty() && missing.empty()) {
                ESP_LOGI("ReceiptRender", "混合模式合成完成（%u 字节，仅重叠行走位图）",
                         (unsigned)gfx.size());
                return gfx;
            }
            // 缺字形 → 退回文字模式，并明确告知缺了哪些字（可重新生成字库补齐）
            ESP_LOGW("ReceiptRender", "字库缺少 %u 个字符「%s」→ 本张退回文字模式（分层不叠加）",
                     (unsigned)missing.size(), missing.c_str());
        } else {
            ESP_LOGW("ReceiptRender", "点阵字库不可用 → 退回文字模式");
        }
    }
    std::string out;
    out.reserve(1024);
    Push(&out, kReset, sizeof(kReset));
    Push(&out, kCodePage, sizeof(kCodePage));

    const Canvas cv = BuildCanvas(t, p, page);
    for (size_t row_idx = 0; row_idx < cv.rows.size(); ++row_idx) {
        const auto& pieces = cv.rows[row_idx];
        if (pieces.empty()) {
            Push(&out, &kLf, 1);   // 空行（含 feed）
            continue;
        }
        std::vector<CanvasPiece> sorted = pieces;
        std::sort(sorted.begin(), sorted.end(),
                  [](const CanvasPiece& a, const CanvasPiece& b) { return a.col < b.col; });
        int cursor_col = 0;
        for (const auto& pc : sorted) {
            const ReceiptBlock& b = *pc.block;
            if (pc.col > cursor_col) {
                out.append(ReceiptLayout::ToGbk(std::string((size_t)(pc.col - cursor_col), ' ')));
                cursor_col = pc.col;
            }
            if (b.type == BlockType::kQr) {
                std::string content = SubstituteParams(b.text, p, nullptr);
                if (!content.empty()) {
                    static const uint8_t kModel[] = {0x1D, 0x28, 0x6B, 0x04, 0x00, 0x31,
                                                     0x41, 0x32, 0x00};
                    Push(&out, kModel, sizeof(kModel));
                    const uint8_t size_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43,
                                                (uint8_t)b.qr_size};
                    Push(&out, size_cmd, sizeof(size_cmd));
                    const uint8_t ecc_cmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45,
                                               (uint8_t)b.qr_ecc};
                    Push(&out, ecc_cmd, sizeof(ecc_cmd));
                    const size_t pL = content.size() + 3;
                    const uint8_t store_hdr[] = {0x1D, 0x28, 0x6B, (uint8_t)(pL & 0xFF),
                                                 (uint8_t)((pL >> 8) & 0xFF), 0x31, 0x50, 0x30};
                    Push(&out, store_hdr, sizeof(store_hdr));
                    out.append(content);
                    static const uint8_t kPrint[] = {0x1D, 0x28, 0x6B, 0x03, 0x00,
                                                     0x31, 0x51, 0x30};
                    Push(&out, kPrint, sizeof(kPrint));
                }
                cursor_col = t.paper_cols;
                // 二维码实际高约 ceil(29×模块尺寸/24) 行 —— 必须**跳过**这些预留行，
                // 否则它们会被当作空行打出来（真机问题：二维码之后空白太多）
                row_idx += (size_t)(std::max(1, (29 * (int)b.qr_size + 23) / 24) - 1);
            } else if (b.type == BlockType::kPageImage) {
                cursor_col = t.paper_cols;   // 图片在文字模式不打印（仅 canvas 支持），不占文字位
            } else if (b.type == BlockType::kLogo) {
                if (logo != nullptr && logo->valid && logo->data != nullptr && logo->len >= 8) {
                    LogoInfo info;
                    info.valid = true;
                    info.width_dots = (uint16_t)logo->width_dots;
                    info.height_dots = (uint16_t)logo->height_dots;
                    info.data_bytes = (int)logo->len - 8;
                    if (b.align == Align::kCenter && pc.col == 0) {
                        out.append(PrinterLogo::CenterOn());
                    }
                    for (const auto& cmd : PrinterLogo::BuildRasterCommands(info, logo->data + 8)) {
                        out.append(cmd);
                    }
                    if (b.align == Align::kCenter && pc.col == 0) {
                        out.append(PrinterLogo::CenterOff());
                    }
                }
                cursor_col = t.paper_cols;
            } else {
                // line / separator：文本（line 的 2 倍宽高与加粗按块属性切换）
                if (b.type == BlockType::kLine && b.bold) Push(&out, kBoldOn, sizeof(kBoldOn));
                if (b.type == BlockType::kLine && b.size == 2) Push(&out, kSize2x, sizeof(kSize2x));
                out.append(ReceiptLayout::ToGbk(pc.text));
                if (b.type == BlockType::kLine && b.size == 2) Push(&out, kSize1x, sizeof(kSize1x));
                if (b.type == BlockType::kLine && b.bold) Push(&out, kBoldOff, sizeof(kBoldOff));
                cursor_col += (int)ReceiptLayout::DisplayWidth(pc.text);
            }
        }
        Push(&out, &kLf, 1);
    }
    return out;
}

std::string ReceiptRender::RenderPreviewText(const ReceiptTemplateV3& t, const ReceiptParams& p) {
    return RenderPreviewText(t, p, nullptr);
}

std::string ReceiptRender::RenderPreviewText(const ReceiptTemplateV3& t, const ReceiptParams& p,
                                             int* unknown_params, const PageRenderInfo* page) {
    const Canvas cv = BuildCanvas(t, p, page);
    std::string out;
    bool first = true;
    for (size_t r = 0; r < cv.rows.size(); ++r) {
        // 二维码按实际高度占行：预览也要**跳过**这些预留行，
        // 否则预览会比纸面多出几行空白（预览必须与打印一致）
        int qr_skip = 0;
        for (const auto& pc : cv.rows[r]) {
            if (pc.block != nullptr && pc.block->type == BlockType::kQr) {
                const int sz = (pc.block->qr_size >= 1 && pc.block->qr_size <= 16)
                                   ? pc.block->qr_size : 6;
                qr_skip = std::max(1, (29 * sz + 23) / 24) - 1;
                break;
            }
        }
        if (!first) out.push_back('\n');
        first = false;
        // 页面图片：仅在 canvas 模式会真的打印；其余模式预览也跳过占位，
        // 保持"预览 = 纸面"（flow/banner 没有位图能力）
        std::vector<CanvasPiece> shown;
        const bool canvas_mode = t.EffectiveMode() == TemplateMode::kCanvas;
        for (const auto& pc : cv.rows[r]) {
            if (pc.block != nullptr && pc.block->type == BlockType::kPageImage && !canvas_mode)
                continue;
            shown.push_back(pc);
        }
        out.append(ComposeRow(shown, t.paper_cols));
        r += (size_t)qr_skip;
    }
    if (unknown_params) *unknown_params = cv.unknown_params;
    return out;
}
