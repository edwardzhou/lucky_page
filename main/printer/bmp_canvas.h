#ifndef LUCKY_BMP_CANVAS_H_
#define LUCKY_BMP_CANVAS_H_

#include <cstdint>
#include <string>
#include <vector>

/// 位图画布（图形模式）：把文字/图像画成像素，再整幅输出 GS v 0。
///
/// 为什么需要：文字要**真正压在图上**，只能由我们把整张小票合成成一张点阵图。
/// 打印机的页面模式本机实测不支持（评审 §19），所以走这条路。
///
/// 字形尺寸与打印机 FONT A 对齐（ASCII 12×24、汉字 24×24），
/// 因此图形模式与文字模式的**行宽完全一致**，版式不变。
class BmpCanvas {
public:
    BmpCanvas(int width_dots, int height_dots);

    int width() const { return width_; }
    int height() const { return height_; }

    void SetPixel(int x, int y, bool black);
    bool GetPixel(int x, int y) const;

    /// 画一段 UTF-8 文字；size=2 时 2 倍宽高。返回绘制结束后的 x。
    /// missing 非空时，把字库里没有的字符追加进去（供调用方告警/回退）。
    int DrawText(int x, int y, const std::string& utf8, int size, std::string* missing);

    /// 画 1-bit 图像（LGO1 数据体：逐行 MSB 优先）。按 max_width_dots 等比缩放。
    void DrawImage1bpp(int x, int y, const uint8_t* data, int src_w, int src_h, int max_width_dots);

    /// 输出为 GS v 0 指令序列（每块 ≤ max_rows 行，避免单条指令过大）
    std::string ToRasterCommands(int max_rows = 24) const;

    // ---- 字库 ----
    static bool FontAvailable();
    static int FontGlyphCount();
    /// 该码点是否有字形
    static bool HasGlyph(uint32_t code);
    /// 文本里缺字形的字符（UTF-8），空表示齐全
    static std::string MissingGlyphs(const std::string& utf8);

private:
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    std::vector<uint8_t> buf_;
};

/// UTF-8 解码为一个码点，返回消耗的字节数（0 = 非法）
int Utf8Next(const std::string& s, size_t pos, uint32_t* code);

#endif  // LUCKY_BMP_CANVAS_H_
