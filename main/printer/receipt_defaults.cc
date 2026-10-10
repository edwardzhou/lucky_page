#include "printer/receipt_defaults.h"

#include <algorithm>
#include <ctime>

#include "esp_log.h"
#include "lottery/time_source.h"
#include "settings.h"

#define TAG "ReceiptDef"

/// 读取**当前生效模式**的模板（设计 §7.3d 按模式分槽）。
/// 槽为空时回退到该模式的内置版式，因此三种模式永远都有可用模板。
ReceiptTemplateV3 LoadTemplateFromSettings() {
    auto& s = Settings::GetInstance();
    const int mode = s.active_tmpl_mode();
    const std::string& json = s.tmpl_slot(mode);
    if (!json.empty()) {
        std::string err;
        auto t = ReceiptTemplateV3::FromJson(json, &err);
        if (err.empty() && !t.blocks.empty()) return t;
        ESP_LOGW(TAG, "模式 %d 的模板解析失败（%s），回退该模式内置版式", mode, err.c_str());
    }
    // 槽为空（首次使用该模式）→ 用该模式的内置版式
    switch (mode) {
        case 1: return BuiltinTemplate(5);   // canvas：水印/边框叠加版式
        case 2: return BuiltinTemplate(6);   // banner：上下花边版式
        default: break;
    }
    // flow：沿用旧的迁移路径（老设备升级不丢配置）
    auto t = MigrateFromLegacy(s.receipt_header(), s.receipt_footer(), s.paper_cols());
    if (!s.qr_enabled()) {
        t.blocks.erase(std::remove_if(t.blocks.begin(), t.blocks.end(),
                                      [](const ReceiptBlock& b) { return b.type == BlockType::kQr; }),
                       t.blocks.end());
    }
    return t;
}

/// 保存模板到**它自己的模式槽**（并按需切换生效模式）——设计 §7.3d
bool SaveTemplateToSettings(const ReceiptTemplateV3& t, bool make_active) {
    auto& s = Settings::GetInstance();
    const int mode = (int)t.EffectiveMode();
    s.set_tmpl_slot(mode, t.ToJson());
    if (make_active) s.set_active_tmpl_mode(mode);
    return s.SaveSync();
}

/// 首次升级迁移：把旧的单槽模板归入**它所属模式**的槽（避免用户配置"搬家"丢失）
void MigrateTemplateSlots() {
    auto& s = Settings::GetInstance();
    if (s.receipt_tmpl().empty()) return;
    std::string err;
    auto t = ReceiptTemplateV3::FromJson(s.receipt_tmpl(), &err);
    if (!err.empty() || t.blocks.empty()) return;
    const int mode = (int)t.EffectiveMode();
    if (mode == 0) return;                       // 本来就是 flow 槽，无需搬
    if (!s.tmpl_slot(mode).empty()) return;      // 该槽已有内容，不覆盖
    s.set_tmpl_slot(mode, s.receipt_tmpl());
    s.set_active_tmpl_mode(mode);
    s.set_receipt_tmpl(std::string());           // 清空旧槽，避免下次重复搬迁
    s.SaveSync();
    ESP_LOGI(TAG, "模板槽迁移：旧模板 → 模式 %d 的槽", mode);
}

ReceiptParams MakeParamsFromSettings() {
    auto& s = Settings::GetInstance();
    ReceiptParams p;
    p.shop = s.shop_name();
    p.win_title = s.win_title();
    p.lose_title = s.lose_title();
    p.epoch = TimeSource::GetInstance().NowEpoch();
    return p;
}

ReceiptParams MakeSampleParams() {
    ReceiptParams p = MakeParamsFromSettings();
    p.ticket = "TEST-0000";
    p.check = "0000";
    p.prize = "一等奖";
    p.note = "打印链路自检";
    p.page = 0;
    p.is_lose = false;
    p.validity = (Settings::GetInstance().validity_days() > 0) ? "2026-10-06" : "";
    return p;
}

ReceiptTemplateV3 BuiltinTemplate(int variant) {
    if (variant == 2) {
        // 简洁：无分隔线、日期时间居中、奖项大字
        ReceiptTemplateV3 t;
        t.paper_cols = 32;
        t.blocks = {
            {BlockType::kLine, "{shop}", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kLine, "{date} {time}", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kFeed, "", Align::kLeft, 1, false, '-', 6, 49, 1},
            {BlockType::kLine, "{prize}", Align::kCenter, 2, true, '-', 6, 49, 4},
            {BlockType::kLine, "{ticket}", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kQr, "{ticket}{check}", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kLine, "有效期至 {validity}", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kFeed, "", Align::kLeft, 1, false, '-', 6, 49, 4},
        };
        return t;
    }
    if (variant == 3) {
        // 促销：Logo + 双倍大字标题 + 促销文案 + 星期
        ReceiptTemplateV3 t;
        t.paper_cols = 32;
        t.blocks = {
            {BlockType::kLogo, "", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kLine, "{shop}", Align::kCenter, 2, true, '-', 6, 49, 4},
            {BlockType::kLine, "幸运大抽奖", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kSeparator, "", Align::kLeft, 1, false, '=', 6, 49, 4},
            {BlockType::kLine, "{title}", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kLine, "{prize}", Align::kCenter, 2, true, '-', 6, 49, 4},
            {BlockType::kLine, "{note}", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kSeparator, "", Align::kLeft, 1, false, '-', 6, 49, 4},
            {BlockType::kLine, "票号：{ticket}  P{page}", Align::kLeft, 1, false, '-', 6, 49, 4},
            {BlockType::kLine, "{date} {time} {weekday}", Align::kLeft, 1, false, '-', 6, 49, 4},
            {BlockType::kLine, "本券有效期至 {validity}", Align::kLeft, 1, false, '-', 6, 49, 4},
            {BlockType::kQr, "https://example.com/v?t={ticket}", Align::kCenter, 1, false, '-', 7, 49, 4},
            {BlockType::kLine, "扫码核销 · 每券一次", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kFeed, "", Align::kLeft, 1, false, '-', 6, 49, 4},
        };
        return t;
    }
    if (variant == 4) {
        // 并排版（画布 row/col 的示例）：**同一行**左右各放一个元素
        ReceiptTemplateV3 t;
        t.paper_cols = 32;
        auto line = [](const char* text, Align a, int col, int row, int size = 1) {
            ReceiptBlock b;
            b.type = BlockType::kLine;
            b.text = text;
            b.align = a;
            b.col = col;
            b.row = row;
            b.size = size;
            return b;
        };
        t.blocks = {
            line("{shop}", Align::kLeft, 0, 0),          // ┐ 同一行（row=0）
            line("{date}", Align::kRight, 16, 0),        // ┘ 左店名 + 右日期
            line("票号", Align::kLeft, 0, 1),             // ┐ 同一行（row=1）
            line("{ticket}", Align::kRight, 8, 1),       // ┘
            line("校验", Align::kLeft, 0, 2),             // ┐ 同一行（row=2）
            line("{check}", Align::kRight, 8, 2),        // ┘
            line("{time}", Align::kRight, 0, 3),
        };
        ReceiptBlock sep;
        sep.type = BlockType::kSeparator;
        sep.sep_char = '=';
        sep.row = 4;
        t.blocks.push_back(sep);
        t.blocks.push_back(line("{prize}", Align::kCenter, 0, 5, 2));
        ReceiptBlock qr;
        qr.type = BlockType::kQr;
        qr.text = "{ticket}{check}";
        qr.row = 6;
        t.blocks.push_back(qr);
        ReceiptBlock feed;
        feed.type = BlockType::kFeed;
        feed.feed_lines = 3;
        t.blocks.push_back(feed);
        return t;
    }
    if (variant == 5) {
        // canvas 出厂预设：与 672 点（28 行）边框配套的"文字压在花纹带上"版式
        ReceiptTemplateV3 t;
        t.paper_cols = 32;
        t.mode_raw = "canvas";
        auto line = [](const char* text, int row, Align a, int size = 1, bool bold = false,
                       int col = 0) {
            ReceiptBlock b;
            b.type = BlockType::kLine;
            b.text = text;
            b.align = a;
            b.row = row;
            b.col = col;
            b.size = size;
            b.bold = bold;
            return b;
        };
        ReceiptBlock mark;
        mark.type = BlockType::kLogo;
        mark.row = 0;
        mark.bg = true;                 // ← 背景层：文字可压在其上
        ReceiptBlock qr;
        qr.type = BlockType::kQr;
        qr.text = "{ticket}{check}";
        qr.row = 28;
        qr.col = 10;
        qr.qr_size = 4;
        qr.qr_ecc = 49;
        ReceiptBlock feed;
        feed.type = BlockType::kFeed;
        feed.feed_lines = 4;
        t.blocks = {
            mark,
            line("幸运翻页机", 1, Align::kCenter, 2, true),
            line("LUCKY PAGE PLAYER", 3, Align::kCenter),
            line("{shop}", 4, Align::kCenter),
            line("票号 {ticket}", 6, Align::kLeft, 1, false, 2),
            line("时间 {datetime}", 7, Align::kLeft, 1, false, 2),
            line("{prize}", 9, Align::kCenter, 2, true),
            line("{title}", 11, Align::kCenter),
            line("请凭此票到服务台领奖", 13, Align::kCenter),
            line("有效期至 {validity}", 14, Align::kCenter),
            line("客服电话 400-000-0000", 16, Align::kCenter),
            line("地址 门店地址示例", 17, Align::kCenter),
            line("关注公众号 参与更多活动", 19, Align::kCenter),
            line("本票据当日有效 请妥善保管", 21, Align::kCenter),
            line("扫码可查询中奖记录", 23, Align::kCenter),
            line("最终解释权归本店所有", 25, Align::kCenter),
            qr,
            feed,
        };
        return t;
    }
    if (variant == 6) {
        // 花边内置版式（banner）：上下两段花边 + 中间文字，行数不限
        ReceiptTemplateV3 t;
        t.paper_cols = 32;
        t.mode_raw = "banner";
        t.banner_top_rows = 2;
        t.banner_bottom_rows = 3;
        auto line = [](const char* text, Align a, int row, int size = 1, bool bold = false) {
            ReceiptBlock b;
            b.type = BlockType::kLine;
            b.text = text;
            b.align = a;
            b.row = row;
            b.size = size;
            b.bold = bold;
            return b;
        };
        ReceiptBlock qr;
        qr.type = BlockType::kQr;
        qr.text = "{ticket}{check}";
        qr.row = 9;
        qr.col = 10;
        qr.qr_size = 4;
        ReceiptBlock feed;
        feed.type = BlockType::kFeed;
        feed.feed_lines = 4;
        t.blocks = {
            line("{shop}", Align::kCenter, 0, 2, true),
            line("{title}", Align::kCenter, 1),
            line("{prize}", Align::kCenter, 2, 2, true),
            line("票号 {ticket}", Align::kLeft, 4),
            line("时间 {datetime}", Align::kLeft, 5),
            line("有效期至 {validity}", Align::kLeft, 6),
            line("请凭此票到服务台领奖", Align::kCenter, 7),
            qr,
            feed,
        };
        return t;
    }
    return ReceiptTemplateV3::Default();
}


/// 重置为出厂预设：mode = 0/1/2 重置单个，-1 = 三个都重置（设计 §7.3e）
bool ResetTemplatesToPreset(int mode) {
    auto& s = Settings::GetInstance();
    auto preset_of = [](int m) {
        switch (m) {
            case 1: return BuiltinTemplate(5);   // canvas：边框叠加
            case 2: return BuiltinTemplate(6);   // banner：上下花边
            default: return BuiltinTemplate(1);  // flow：经典
        }
    };
    if (mode == -1) {
        for (int m = 0; m < 3; ++m) s.set_tmpl_slot(m, preset_of(m).ToJson());
        s.set_active_tmpl_mode(0);
        ESP_LOGI(TAG, "三种版式已全部重置为出厂预设");
    } else if (mode >= 0 && mode <= 2) {
        s.set_tmpl_slot(mode, preset_of(mode).ToJson());
        s.set_active_tmpl_mode(mode);
        ESP_LOGI(TAG, "版式 %d 已重置为出厂预设", mode);
    } else {
        return false;
    }
    return s.SaveSync();
}
