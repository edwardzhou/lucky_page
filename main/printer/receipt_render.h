#ifndef _RECEIPT_RENDER_H_
#define _RECEIPT_RENDER_H_

#include <cstdint>
#include <string>

#include "printer/receipt_params.h"
#include "printer/receipt_template.h"

/// Logo 渲染信息（由 PrinterService 从 LogoStore 加载后注入；
/// 渲染层不依赖存储，便于纯函数测试 —— D15/§6.4）
struct LogoRenderInfo {
    const uint8_t* data = nullptr;   ///< LGO1 头 8B + 1bit 栅格（不含头）
    size_t len = 0;
    int width_dots = 0;
    int height_dots = 0;
    bool valid = false;
};

/// 当页打印内容（由 PrinterService 按 params.page 从「页面内容」+素材库加载）
struct PageRenderInfo {
    std::string text;                ///< 当页文字（可含参数）
    const uint8_t* image = nullptr;  ///< LGO1（8B 头 + 1bpp），无图则 nullptr
    size_t image_len = 0;
    int width_dots = 0;
    int height_dots = 0;
    bool image_valid = false;

    bool has_text() const { return !text.empty(); }
    bool has_image() const {
        return image_valid && image != nullptr && image_len >= 8 && width_dots > 0 &&
               height_dots > 0 && (width_dots % 8) == 0 &&
               image_len >= 8 + (size_t)(width_dots / 8) * height_dots;
    }
};

/// 块模板渲染（纯逻辑，不碰硬件）
class ReceiptRender {
public:
    /// 渲染为 ESC/POS 字节流。
    /// @param logo 可为 nullptr（无 Logo → kLogo 块自动跳过，D15）
    static std::string Render(const ReceiptTemplateV3& t, const ReceiptParams& p,
                              const LogoRenderInfo* logo,
                              const PageRenderInfo* page = nullptr);

    /// 人类可读预览（UTF-8；每行显示宽度 ≤ paper_cols；不含任何指令字节）
    static std::string RenderPreviewText(const ReceiptTemplateV3& t, const ReceiptParams& p);

    /// 预览 + 未知参数计数（Web 预览告警用）
    static std::string RenderPreviewText(const ReceiptTemplateV3& t, const ReceiptParams& p,
                                         int* unknown_params,
                                         const PageRenderInfo* page = nullptr);
};

#endif  // _RECEIPT_RENDER_H_
