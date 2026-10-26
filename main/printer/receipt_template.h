#ifndef _RECEIPT_TEMPLATE_H_
#define _RECEIPT_TEMPLATE_H_

#include <string>
#include <vector>

/// 小票块类型（设计 §7.3b）
enum class BlockType {
    kLine,       ///< 一行文字（可含参数；按显示宽度折行）
    kSeparator,  ///< 用 sep_char 填满纸宽
    kQr,         ///< 二维码（text = 内容模板）
    kLogo,       ///< 插入已上传 Logo（未上传则跳过，D15）
    kFeed,       ///< 走纸 N 行
    kPageText,   ///< 当页文字（内容来自「页面内容」；空则跳过）
    kPageImage,  ///< 当页图片（仅 canvas 生效；flow/banner 跳过）
};

/// 水平对齐
enum class Align { kLeft, kCenter, kRight };

/// 模版模式（设计 §7.3c）——三种排版方式本质不同，由用户选择
enum class TemplateMode {
    kFlow = 0,   ///< 纯文字：全走打印机文字（帧 ~0.5KB，最快，行数不限）
    kCanvas,     ///< 画布：整票位图合成，文字与图**逐像素叠加**（帧 ~32KB，慢，四边完整边框）
    kBanner,     ///< 花边：上下两段花边位图 + 中间文字（帧 ~3~4KB，行数不限，无左右竖线）
};

const char* TemplateModeName(TemplateMode m);
bool TemplateModeFromString(const std::string& s, TemplateMode* out);

/// 单个块
struct ReceiptBlock {
    BlockType type = BlockType::kLine;
    std::string text;        ///< line 的文案 / qr 的内容（都支持 {参数}）
    Align align = Align::kLeft;
    int size = 1;            ///< 字符倍宽高：1 | 2
    bool bold = false;
    char sep_char = '-';     ///< separator 填充字符
    int qr_size = 6;         ///< QR 模块尺寸 1..16
    int qr_ecc = 49;         ///< QR 纠错等级 48|49|50|51（49=M，容错好）
    int feed_lines = 4;      ///< feed 走纸行数 0..10
    int col = 0;             ///< 画布列偏移（0 = 纸最左）。横向拖动即改它。
    int row = -1;            ///< 画布行位置：-1 = 自动（跟随流式顺序）；
                             ///< ≥0 = 绝对行（纵向拖动即钉住）。同一 row 上按 col 从左到右
                             ///< 排布 —— 这才是"左 Logo 右店名"并排的完整支持（设计 §7.3b）
    bool bg = false;         ///< 是否作为**背景层**（仅 logo/qr 有意义）：
                             ///< 背景块不占流式行，文字可叠加在上面（页模式实现，设计 §7.3c）

};

/// 小票块模板 v3（存 NVS `receipt_tmpl`）
struct ReceiptTemplateV3 {
    int paper_cols = 32;
    std::string mode_raw;                 ///< JSON 里的原始 mode（空 = 未指定，按规则推断）
    TemplateMode mode = TemplateMode::kFlow;   ///< 生效模式
    int banner_top_rows = 2;              ///< 仅 banner：顶部花边占几行（1 行 = 24 点）
    int banner_bottom_rows = 3;           ///< 仅 banner：底部花边占几行     ///< 32(58mm) | 48(80mm)
    std::vector<ReceiptBlock> blocks;

    static constexpr int kMaxBlocks = 64;
    static constexpr int kMaxTextBytes = 512;
    static constexpr int kMaxRows = 200;   ///< 画布最大行数（防呆）

    /// 出厂默认模板（等价于 v2 默认版式）
    static ReceiptTemplateV3 Default();

    /// 序列化为 JSON
    std::string ToJson() const;

    /// 解析 JSON；失败时返回空 blocks 并写 *error（中文原因）
    static ReceiptTemplateV3 FromJson(const std::string& json, std::string* error);

    /// 校验；返回空串 = 通过，否则中文原因（Web 400 直接回传）
    std::string Validate() const;

    /// **生效模式（推导，非存储）**：mode_raw 为空时按规则推断
    ///   · 有 bg 块 → canvas（与升级前行为一致）
    ///   · 无 bg 块 → flow
    /// 推导而非存储 —— 避免"代码里构造模板忘了设 mode"（真回归教训）
    TemplateMode EffectiveMode() const;

    /// 兼容保留：把推导结果写回 mode 字段（仅用于 JSON 输出）
    void ResolveMode();
};

/// v2（header/footer 文本）→ v3 块模板。
/// 保证迁移后**可见文本与 v2 版式一致**（自动化测试锁定）。
ReceiptTemplateV3 MigrateFromLegacy(const std::string& header, const std::string& footer,
                                    int paper_cols);

/// 块类型 ↔ 字符串（JSON 用；未知返回 false 且不改 *out）
bool BlockTypeFromString(const std::string& s, BlockType* out);
const char* BlockTypeToString(BlockType t);

/// 对齐 ↔ 字符串
bool AlignFromString(const std::string& s, Align* out);
const char* AlignToString(Align a);

#endif  // _RECEIPT_TEMPLATE_H_
