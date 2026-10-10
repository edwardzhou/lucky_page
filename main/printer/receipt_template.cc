#include "printer/receipt_template.h"

#include <cJSON.h>

namespace {

const char* kBlockTypes[] = {"line", "separator", "qr", "logo", "feed"};
const char* kAligns[] = {"left", "center", "right"};

int TypeIndex(BlockType t) {
    switch (t) {
        case BlockType::kLine: return 0;
        case BlockType::kSeparator: return 1;
        case BlockType::kQr: return 2;
        case BlockType::kLogo: return 3;
        case BlockType::kFeed: return 4;
    }
    return 0;
}

int AlignIndex(Align a) {
    switch (a) {
        case Align::kLeft: return 0;
        case Align::kCenter: return 1;
        case Align::kRight: return 2;
    }
    return 0;
}

/// 把一段可能含 {nl} 的旧文本拆成多行（迁移用）
std::vector<std::string> SplitLegacyLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 4, "{nl}") == 0) {
            out.push_back(cur);
            cur.clear();
            i += 4;
            continue;
        }
        if (s[i] == '\n') {
            out.push_back(cur);
            cur.clear();
            ++i;
            continue;
        }
        cur.push_back(s[i]);
        ++i;
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}

ReceiptBlock MakeLine(const std::string& text, Align a) {
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = text;
    b.align = a;
    return b;
}

}  // namespace

bool BlockTypeFromString(const std::string& s, BlockType* out) {
    for (int i = 0; i < 5; ++i) {
        if (s == kBlockTypes[i]) {
            if (out) *out = (BlockType)i;
            return true;
        }
    }
    return false;
}

const char* BlockTypeToString(BlockType t) { return kBlockTypes[TypeIndex(t)]; }

bool AlignFromString(const std::string& s, Align* out) {
    for (int i = 0; i < 3; ++i) {
        if (s == kAligns[i]) {
            if (out) *out = (Align)i;
            return true;
        }
    }
    return false;
}

const char* AlignToString(Align a) { return kAligns[AlignIndex(a)]; }

ReceiptTemplateV3 ReceiptTemplateV3::Default() {
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    t.blocks = {
        MakeLine("{shop}", Align::kCenter),
        MakeLine("==================", Align::kCenter),
        MakeLine("{title}", Align::kCenter),
        MakeLine("{prize}", Align::kCenter),
        MakeLine("{note}", Align::kCenter),
        {BlockType::kSeparator, "", Align::kLeft, 1, false, '-', 6, 49, 4},
        MakeLine("票号：{ticket}", Align::kLeft),
        MakeLine("时间：{datetime}", Align::kLeft),
        MakeLine("页码：P{page}", Align::kLeft),
        MakeLine("校验：{check}", Align::kLeft),
        {BlockType::kSeparator, "", Align::kLeft, 1, false, '-', 6, 49, 4},
        MakeLine("请凭此票到服务台领奖", Align::kLeft),
        MakeLine("有效期至：{validity}", Align::kLeft),
        {BlockType::kQr, "{ticket}{check}", Align::kCenter, 1, false, '-', 6, 49, 4},
        {BlockType::kFeed, "", Align::kLeft, 1, false, '-', 6, 49, 4},
    };
    return t;
}

std::string ReceiptTemplateV3::ToJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddNumberToObject(root, "paper_cols", paper_cols);
    // mode 始终写出（生效模式），便于前端显示；banner 行数仅该模式下写出
    const TemplateMode eff_mode = EffectiveMode();
    cJSON_AddStringToObject(root, "mode", TemplateModeName(eff_mode));
    if (eff_mode == TemplateMode::kBanner) {
        cJSON_AddNumberToObject(root, "banner_top_rows", banner_top_rows);
        cJSON_AddNumberToObject(root, "banner_bottom_rows", banner_bottom_rows);
    }
    cJSON* arr = cJSON_AddArrayToObject(root, "blocks");
    for (const auto& b : blocks) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", BlockTypeToString(b.type));
        if (b.col != 0) cJSON_AddNumberToObject(o, "col", b.col);
        if (b.row >= 0) cJSON_AddNumberToObject(o, "row", b.row);
        if (b.bg) cJSON_AddBoolToObject(o, "bg", true);
        if (b.type == BlockType::kLine) {
            cJSON_AddStringToObject(o, "text", b.text.c_str());
            cJSON_AddStringToObject(o, "align", AlignToString(b.align));
            cJSON_AddNumberToObject(o, "size", b.size);
            cJSON_AddBoolToObject(o, "bold", b.bold);
        } else if (b.type == BlockType::kSeparator) {
            char s[2] = {b.sep_char, 0};
            cJSON_AddStringToObject(o, "char", s);
        } else if (b.type == BlockType::kQr) {
            cJSON_AddStringToObject(o, "content", b.text.c_str());
            cJSON_AddNumberToObject(o, "size", b.qr_size);
            cJSON_AddNumberToObject(o, "ecc", b.qr_ecc);
        } else if (b.type == BlockType::kFeed) {
            cJSON_AddNumberToObject(o, "lines", b.feed_lines);
        }
        cJSON_AddItemToArray(arr, o);
    }
    char* s = cJSON_PrintUnformatted(root);
    std::string out(s ? s : "{}");
    cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

ReceiptTemplateV3 ReceiptTemplateV3::FromJson(const std::string& json, std::string* error) {
    ReceiptTemplateV3 t;
    t.blocks.clear();

    // ⚠️ 错误信息必须在 cJSON_Delete 之前拼好！
    // 曾写成 `cJSON_Delete(root); return fail("..." + std::string(type->valuestring));`
    // → use-after-free：读已释放节点的字符串 → std::string(nullptr) 抛异常 → abort。
    // 后果是「浏览器发一段畸形模板 JSON 就能把设备打崩」（Web API 远程 DoS）。
    // 本 lambda 的 msg 参数在调用前求值（此时树仍存活），函数体内才释放。
    auto fail = [&](cJSON* root, const std::string& msg) {
        if (root != nullptr) cJSON_Delete(root);
        t.blocks.clear();
        if (error) *error = msg;
        t.ResolveMode();   // 回填缓存模式（渲染/校验用推导值 EffectiveMode，此处仅为 JSON 输出一致）
    return t;
    };

    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) return fail(nullptr, "JSON 解析失败");

    cJSON* ver = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(ver) && ver->valueint != 3) {
        int v = ver->valueint;   // 先取值，避免释放后读取
        return fail(root, "模板 version 必须为 3（当前 " + std::to_string(v) + "）");
    }

    cJSON* md = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(md) && md->valuestring) t.mode_raw = md->valuestring;
    cJSON* btr = cJSON_GetObjectItem(root, "banner_top_rows");
    if (cJSON_IsNumber(btr)) t.banner_top_rows = btr->valueint;
    cJSON* bbr = cJSON_GetObjectItem(root, "banner_bottom_rows");
    if (cJSON_IsNumber(bbr)) t.banner_bottom_rows = bbr->valueint;
    cJSON* pc = cJSON_GetObjectItem(root, "paper_cols");
    if (cJSON_IsNumber(pc)) t.paper_cols = pc->valueint;

    cJSON* arr = cJSON_GetObjectItem(root, "blocks");
    if (!cJSON_IsArray(arr)) return fail(root, "缺少 blocks 数组");
    const int arr_size = cJSON_GetArraySize(arr);
    if (arr_size == 0) return fail(root, "blocks 不能为空（至少要有 1 个块）");
    if (arr_size > kMaxBlocks) {
        return fail(root, "blocks 超过上限 " + std::to_string(kMaxBlocks) + " 块");
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) return fail(root, "blocks 含非对象项");

        cJSON* type = cJSON_GetObjectItem(item, "type");
        if (!cJSON_IsString(type) || type->valuestring == nullptr) {
            return fail(root, "块缺少 type 字段");
        }
        ReceiptBlock b;
        cJSON* col = cJSON_GetObjectItem(item, "col");
        if (cJSON_IsNumber(col)) b.col = col->valueint;
        cJSON* row = cJSON_GetObjectItem(item, "row");
        if (cJSON_IsNumber(row)) b.row = row->valueint;
        cJSON* bg = cJSON_GetObjectItem(item, "bg");
        if (cJSON_IsBool(bg)) b.bg = cJSON_IsTrue(bg);
        if (!BlockTypeFromString(type->valuestring, &b.type)) {
            std::string bad = type->valuestring;   // 释放前拷出来
            return fail(root, "未知块 type: " + bad);
        }

        if (b.type == BlockType::kLine) {
            cJSON* text = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(text) && text->valuestring) b.text = text->valuestring;
            cJSON* align = cJSON_GetObjectItem(item, "align");
            if (cJSON_IsString(align) && align->valuestring) {
                std::string bad_align = align->valuestring;
                if (!AlignFromString(bad_align, &b.align)) {
                    return fail(root, "line 的 align 非法: " + bad_align +
                                          "（应为 left/center/right）");
                }
            }
            cJSON* size = cJSON_GetObjectItem(item, "size");
            if (cJSON_IsNumber(size)) b.size = size->valueint;
            cJSON* bold = cJSON_GetObjectItem(item, "bold");
            if (cJSON_IsBool(bold)) b.bold = cJSON_IsTrue(bold);
        } else if (b.type == BlockType::kSeparator) {
            cJSON* ch = cJSON_GetObjectItem(item, "char");
            if (cJSON_IsString(ch) && ch->valuestring && ch->valuestring[0] != '\0') {
                b.sep_char = ch->valuestring[0];
            }
        } else if (b.type == BlockType::kQr) {
            cJSON* content = cJSON_GetObjectItem(item, "content");
            if (cJSON_IsString(content) && content->valuestring) b.text = content->valuestring;
            cJSON* size = cJSON_GetObjectItem(item, "size");
            if (cJSON_IsNumber(size)) b.qr_size = size->valueint;
            cJSON* ecc = cJSON_GetObjectItem(item, "ecc");
            if (cJSON_IsNumber(ecc)) b.qr_ecc = ecc->valueint;
        } else if (b.type == BlockType::kFeed) {
            cJSON* lines = cJSON_GetObjectItem(item, "lines");
            if (cJSON_IsNumber(lines)) b.feed_lines = lines->valueint;
        }
        t.blocks.push_back(b);
    }
    cJSON_Delete(root);

    std::string err = t.Validate();
    if (!err.empty()) {
        t.blocks.clear();
        if (error) *error = err;
        t.ResolveMode();   // 回填缓存模式（渲染/校验用推导值 EffectiveMode，此处仅为 JSON 输出一致）
    return t;
    }
    if (error) error->clear();
    t.ResolveMode();   // 回填缓存模式（渲染/校验用推导值 EffectiveMode，此处仅为 JSON 输出一致）
    return t;
}

std::string ReceiptTemplateV3::Validate() const {
    if (paper_cols != 32 && paper_cols != 48) {
        return "paper_cols 必须是 32 或 48（当前 " + std::to_string(paper_cols) + "）";
    }
    if (blocks.empty()) return "blocks 不能为空（至少要有 1 个块）";
    if ((int)blocks.size() > kMaxBlocks) {
        return "blocks 超过上限 " + std::to_string(kMaxBlocks) + " 块";
    }
    // ---- 模式相关校验（设计 §7.3c）----
    if (!mode_raw.empty()) {
        TemplateMode tmp = TemplateMode::kFlow;
        if (!TemplateModeFromString(mode_raw, &tmp)) {
            return "mode 必须是 flow / canvas / banner（当前 \"" + mode_raw + "\"）";
        }
    }
    const TemplateMode eff = EffectiveMode();
    if (eff == TemplateMode::kBanner) {
        if (banner_top_rows < 0 || banner_top_rows > 10) return "banner_top_rows 必须在 0..10";
        if (banner_bottom_rows < 0 || banner_bottom_rows > 10) return "banner_bottom_rows 必须在 0..10";
        if (banner_top_rows + banner_bottom_rows == 0) return "花边模式至少要有一段花边行数 > 0";
    }
    bool any_bg = false;
    for (const auto& b : blocks) {
        if (b.bg) { any_bg = true; break; }
    }
    // 仅当**显式**写了 flow 却又带背景块时报错；未指定 mode 时按 canvas 推断（兼容旧模板）
    if (any_bg && mode_raw == "flow") {
        return "含有背景层元素，mode 不能是 flow（请用 canvas 或 banner）";
    }

    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& b = blocks[i];
        const std::string at = "第 " + std::to_string(i + 1) + " 块：";
        if (b.row < -1 || b.row > kMaxRows) {
            return at + "row 必须在 -1（自动）.." + std::to_string(kMaxRows) +
                   "（当前 " + std::to_string(b.row) + "）";
        }
        if (b.bg && b.type != BlockType::kLogo) {
            // 二维码不做背景：文字压在码点上会遮挡模块、降低扫码识别率（现场确认的策略）
            return at + "只有 logo 可以作为背景水印（二维码做背景会遮挡码点、影响扫码）";
        }
        if (b.text.size() > (size_t)kMaxTextBytes) {
            return at + "文本超过 512 字节上限";
        }
        switch (b.type) {
            case BlockType::kLine:
                if (b.col < 0 || b.col >= paper_cols) {
                    return at + "col 必须在 0.." + std::to_string(paper_cols - 1) +
                           "（当前 " + std::to_string(b.col) + "）";
                }
                if (b.size != 1 && b.size != 2) {
                    return at + "size 必须是 1 或 2（当前 " + std::to_string(b.size) + "）";
                }
                break;
            case BlockType::kSeparator:
                break;
            case BlockType::kQr: {
                if (b.col < 0 || b.col >= paper_cols) {
                    return at + "col 必须在 0.." + std::to_string(paper_cols - 1) + "（当前 " +
                           std::to_string(b.col) + "）";
                }
                if (b.qr_size < 1 || b.qr_size > 16) {
                    return at + "二维码 size 必须在 1..16（当前 " +
                           std::to_string(b.qr_size) + "）";
                }
                if (b.qr_ecc != 48 && b.qr_ecc != 49 && b.qr_ecc != 50 && b.qr_ecc != 51) {
                    return at + "二维码 ecc 必须是 48/49/50/51（当前 " +
                           std::to_string(b.qr_ecc) + "）";
                }
                // 空内容二维码打不出任何东西 —— 这是配置错误，必须当场拒绝而不是静默跳过
                // （真机问题：画布保存时字段名不匹配把内容清空 → 小票上没有二维码）
                if (b.text.empty()) {
                    return at + "二维码内容不能为空（例如 {ticket}{check}）";
                }
                break;
            }
            case BlockType::kLogo:
                if (b.col < 0 || b.col >= paper_cols) {
                    return at + "col 必须在 0.." + std::to_string(paper_cols - 1) + "（当前 " +
                           std::to_string(b.col) + "）";
                }
                break;
            case BlockType::kFeed:
                if (b.feed_lines < 0 || b.feed_lines > 10) {
                    return at + "feed lines 必须在 0..10（当前 " +
                           std::to_string(b.feed_lines) + "）";
                }
                break;
        }
    }
    // 同一绝对行上的块不得水平重叠（否则打出来互相覆盖）
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].row < 0) continue;
        for (size_t j = i + 1; j < blocks.size(); ++j) {
            if (blocks[j].row != blocks[i].row) continue;
            // 背景层与前景层**允许重叠**（这正是"文字叠在 logo/二维码上"的用途）
            if (blocks[i].bg || blocks[j].bg) continue;
            const int a0 = blocks[i].col;
            const int b0 = blocks[j].col;
            if (a0 == b0) {
                return "第 " + std::to_string(i + 1) + " 与第 " + std::to_string(j + 1) +
                       " 块在同一行同一列（会互相覆盖）";
            }
            // 2 倍字号每字符占 2 列，与 1 倍混排时列坐标不同量纲 → 拒绝，避免打出错位
            if (blocks[i].type == BlockType::kLine && blocks[j].type == BlockType::kLine &&
                blocks[i].size != blocks[j].size) {
                return "第 " + std::to_string(i + 1) + " 与第 " + std::to_string(j + 1) +
                       " 块在同一行混用了 1 倍与 2 倍字号（列坐标无法对齐）";
            }
        }
    }
    return "";
}

ReceiptTemplateV3 MigrateFromLegacy(const std::string& header, const std::string& footer,
                                    int paper_cols) {
    ReceiptTemplateV3 t;
    t.paper_cols = (paper_cols == 48) ? 48 : 32;

    if (!header.empty()) {
        for (auto& l : SplitLegacyLines(header)) t.blocks.push_back(MakeLine(l, Align::kCenter));
    }
    // 中间固定版式（等价 v2 的硬编码区）
    t.blocks.push_back(MakeLine("{title}", Align::kCenter));
    t.blocks.push_back(MakeLine("{prize}", Align::kCenter));
    t.blocks.push_back(MakeLine("{note}", Align::kCenter));
    t.blocks.push_back({BlockType::kSeparator, "", Align::kLeft, 1, false, '-', 6, 49, 4});
    t.blocks.push_back(MakeLine("票号：{ticket}", Align::kLeft));
    t.blocks.push_back(MakeLine("时间：{datetime}", Align::kLeft));
    t.blocks.push_back(MakeLine("页码：P{page}", Align::kLeft));
    t.blocks.push_back(MakeLine("校验：{check}", Align::kLeft));

    if (!footer.empty()) {
        t.blocks.push_back({BlockType::kSeparator, "", Align::kLeft, 1, false, '-', 6, 49, 4});
        for (auto& l : SplitLegacyLines(footer)) t.blocks.push_back(MakeLine(l, Align::kLeft));
    } else {
        t.blocks.push_back({BlockType::kSeparator, "", Align::kLeft, 1, false, '-', 6, 49, 4});
        t.blocks.push_back(MakeLine("请凭此票到服务台领奖", Align::kLeft));
        t.blocks.push_back(MakeLine("有效期至：{validity}", Align::kLeft));
    }

    t.blocks.push_back({BlockType::kQr, "{ticket}{check}", Align::kCenter, 1, false, '-', 6, 49, 4});
    t.blocks.push_back({BlockType::kFeed, "", Align::kLeft, 1, false, '-', 6, 49, 4});
    return t;
}


const char* TemplateModeName(TemplateMode m) {
    switch (m) {
        case TemplateMode::kFlow: return "flow";
        case TemplateMode::kCanvas: return "canvas";
        case TemplateMode::kBanner: return "banner";
    }
    return "?";
}

bool TemplateModeFromString(const std::string& s, TemplateMode* out) {
    if (s == "flow") { if (out) *out = TemplateMode::kFlow; return true; }
    if (s == "canvas") { if (out) *out = TemplateMode::kCanvas; return true; }
    if (s == "banner") { if (out) *out = TemplateMode::kBanner; return true; }
    return false;
}

/// 按规则推断生效模式（设计 §7.3c 兼容规则）：
///   mode 缺省 → 有 bg 块 = canvas（与升级前行为一致）；无 bg 块 = flow
TemplateMode ReceiptTemplateV3::EffectiveMode() const {
    if (!mode_raw.empty()) {
        TemplateMode m = TemplateMode::kFlow;
        if (TemplateModeFromString(mode_raw, &m)) return m;
    }
    for (const auto& b : blocks) {
        if (b.bg) return TemplateMode::kCanvas;
    }
    return TemplateMode::kFlow;
}

void ReceiptTemplateV3::ResolveMode() { mode = EffectiveMode(); }
