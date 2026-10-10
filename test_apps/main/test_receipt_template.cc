#include <cstring>

#include "printer/bmp_canvas.h"
#include "printer/receipt_defaults.h"
// test_receipt_template.cc — 小票块模板 v3（设计 §7.3b）
// 覆盖：JSON 往返 / 校验 / 参数替换 / 排版 / 字节指令 / 金字节 / v2→v3 迁移
#include "lucky_test.h"

#include <algorithm>
#include <string>
#include <vector>

#include "lottery/draw_record.h"
#include "printer/gbk_encoder.h"
#include "lottery/time_source.h"
#include "settings.h"
#include "printer/receipt_layout.h"
#include "printer/receipt_params.h"
#include "printer/receipt_render.h"
#include "printer/receipt_template.h"

// 2026-09-29 14:32:07 CST = 1790663527（python 实算）
static constexpr time_t kT = 1790663527;

static ReceiptParams SampleParams() {
    ReceiptParams p;
    p.shop = "幸运翻页机";
    p.win_title = "恭喜中奖";
    p.lose_title = "感谢参与";
    p.is_lose = false;
    p.prize = "一等奖";
    p.note = "奖品：扫地机器人";
    p.ticket = "20260929-0007";
    p.check = "3F2A";
    p.page = 2;   // → 渲染 P3
    p.validity = "2026-10-06";
    p.epoch = kT;
    return p;
}

// ============================================================================
// JSON 往返
// ============================================================================
TEST(ReceiptTemplateV3, DefaultIsValid) {
    auto t = ReceiptTemplateV3::Default();
    EXPECT_STREQ("", t.Validate().c_str());
    EXPECT_TRUE(t.blocks.size() >= 5);
}

TEST(ReceiptTemplateV3, JsonRoundTripAllBlockTypes) {
    ReceiptTemplateV3 t;
    t.paper_cols = 48;
    t.blocks = {
        {BlockType::kLogo, "", Align::kCenter, 1, false, '-', 6, 49, 4},
        {BlockType::kLine, "{shop}", Align::kCenter, 2, true, '-', 6, 49, 4},
        {BlockType::kSeparator, "", Align::kLeft, 1, false, '=', 6, 49, 4},
        {BlockType::kQr, "{ticket}{check}", Align::kCenter, 1, false, '-', 8, 50, 4},
        {BlockType::kFeed, "", Align::kLeft, 1, false, '-', 6, 49, 3},
    };
    std::string json = t.ToJson();
    std::string err;
    auto back = ReceiptTemplateV3::FromJson(json, &err);
    EXPECT_STREQ("", err.c_str());
    EXPECT_EQ(48, back.paper_cols);
    ASSERT_EQ(t.blocks.size(), back.blocks.size());
    EXPECT_TRUE(t.blocks[0].type == back.blocks[0].type);
    EXPECT_STREQ("{shop}", back.blocks[1].text.c_str());
    EXPECT_EQ(2, back.blocks[1].size);
    EXPECT_TRUE(back.blocks[1].bold);
    EXPECT_TRUE(back.blocks[1].align == Align::kCenter);
    EXPECT_EQ('=', back.blocks[2].sep_char);
    EXPECT_STREQ("{ticket}{check}", back.blocks[3].text.c_str());
    EXPECT_EQ(8, back.blocks[3].qr_size);
    EXPECT_EQ(50, back.blocks[3].qr_ecc);
    EXPECT_EQ(3, back.blocks[4].feed_lines);
}

TEST(ReceiptTemplateV3, BadJsonRejected) {
    std::string err;
    auto t = ReceiptTemplateV3::FromJson("{not json", &err);
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(t.blocks.empty());
}

TEST(ReceiptTemplateV3, MissingFieldsUseDefaults) {
    std::string err;
    auto t = ReceiptTemplateV3::FromJson(R"({"version":3,"blocks":[{"type":"line","text":"hi"}]})",
                                         &err);
    EXPECT_STREQ("", err.c_str());
    ASSERT_EQ(1u, t.blocks.size());
    EXPECT_TRUE(t.blocks[0].align == Align::kLeft);   // 默认左对齐
    EXPECT_EQ(1, t.blocks[0].size);
    EXPECT_FALSE(t.blocks[0].bold);
    EXPECT_EQ(32, t.paper_cols);                      // 默认纸宽
}

// ============================================================================
// 校验（每条规则都要有测试）
// ============================================================================
TEST(ReceiptTemplateV3, RejectsUnknownBlockType) {
    std::string err;
    auto t = ReceiptTemplateV3::FromJson(
        R"({"version":3,"blocks":[{"type":"image","text":"x"}]})", &err);
    EXPECT_NE(std::string::npos, err.find("type"));
    EXPECT_TRUE(t.blocks.empty());
}

TEST(ReceiptTemplateV3, RejectsEmptyBlocks) {
    std::string err;
    auto t = ReceiptTemplateV3::FromJson(R"({"version":3,"blocks":[]})", &err);
    EXPECT_NE(std::string::npos, err.find("块"));
    EXPECT_TRUE(t.blocks.empty());
}

TEST(ReceiptTemplateV3, RejectsTooManyBlocks) {
    auto t = ReceiptTemplateV3::Default();
    t.blocks.clear();
    for (int i = 0; i < 65; ++i) {
        ReceiptBlock b;
        b.type = BlockType::kLine;
        b.text = "x";
        t.blocks.push_back(b);
    }
    EXPECT_NE(std::string::npos, t.Validate().find("块"));
}

TEST(ReceiptTemplateV3, RejectsBadAlignFromJson) {
    std::string err;
    auto t = ReceiptTemplateV3::FromJson(
        R"({"version":3,"blocks":[{"type":"line","text":"x","align":"middle"}]})", &err);
    EXPECT_NE(std::string::npos, err.find("align"));
}

TEST(ReceiptTemplateV3, RejectsSizeOutOfRange) {
    auto t = ReceiptTemplateV3::Default();
    t.blocks[0].size = 3;
    EXPECT_NE(std::string::npos, t.Validate().find("size"));
}

TEST(ReceiptTemplateV3, RejectsBadQrEcc) {
    auto t = ReceiptTemplateV3::Default();
    ReceiptBlock q;
    q.type = BlockType::kQr;
    q.text = "x";
    q.qr_ecc = 47;
    t.blocks = {q};
    EXPECT_NE(std::string::npos, t.Validate().find("ecc"));
}

TEST(ReceiptTemplateV3, RejectsQrSizeOutOfRange) {
    auto t = ReceiptTemplateV3::Default();
    ReceiptBlock q;
    q.type = BlockType::kQr;
    q.text = "x";
    q.qr_size = 17;
    t.blocks = {q};
    EXPECT_NE(std::string::npos, t.Validate().find("size"));
}

TEST(ReceiptTemplateV3, RejectsFeedOutOfRange) {
    auto t = ReceiptTemplateV3::Default();
    ReceiptBlock f;
    f.type = BlockType::kFeed;
    f.feed_lines = 11;
    t.blocks = {f};
    EXPECT_NE(std::string::npos, t.Validate().find("lines"));
}

TEST(ReceiptTemplateV3, RejectsOverlongText) {
    auto t = ReceiptTemplateV3::Default();
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = std::string(513, 'x');
    t.blocks = {b};
    EXPECT_NE(std::string::npos, t.Validate().find("512"));
}

TEST(ReceiptTemplateV3, RejectsBadPaperCols) {
    auto t = ReceiptTemplateV3::Default();
    t.paper_cols = 20;
    EXPECT_NE(std::string::npos, t.Validate().find("paper_cols"));
}

TEST(ReceiptTemplateV3, EmptyLineTextIsAllowed) {
    // 空行是合法的排版手段（制造空行），不应被拒
    auto t = ReceiptTemplateV3::Default();
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = "";
    t.blocks = {b};
    EXPECT_STREQ("", t.Validate().c_str());
}

// ============================================================================
// 参数替换
// ============================================================================
TEST(ReceiptParams, AllPlaceholdersResolve) {
    auto p = SampleParams();
    int unknown = 0;
    EXPECT_STREQ("幸运翻页机", SubstituteParams("{shop}", p, &unknown).c_str());
    EXPECT_STREQ("恭喜中奖", SubstituteParams("{title}", p, &unknown).c_str());
    EXPECT_STREQ("一等奖", SubstituteParams("{prize}", p, &unknown).c_str());
    EXPECT_STREQ("奖品：扫地机器人", SubstituteParams("{note}", p, &unknown).c_str());
    EXPECT_STREQ("20260929-0007", SubstituteParams("{ticket}", p, &unknown).c_str());
    EXPECT_STREQ("3F2A", SubstituteParams("{check}", p, &unknown).c_str());
    EXPECT_STREQ("P3", SubstituteParams("P{page}", p, &unknown).c_str());
    EXPECT_STREQ("2026-10-06", SubstituteParams("{validity}", p, &unknown).c_str());
    // 日期时间（CST-8）
    EXPECT_STREQ("2026-09-29", SubstituteParams("{date}", p, &unknown).c_str());
    EXPECT_STREQ("14:32:07", SubstituteParams("{time}", p, &unknown).c_str());
    EXPECT_STREQ("2026-09-29 14:32:07", SubstituteParams("{datetime}", p, &unknown).c_str());
    EXPECT_STREQ("2026", SubstituteParams("{year}", p, &unknown).c_str());
    EXPECT_STREQ("09", SubstituteParams("{month}", p, &unknown).c_str());
    EXPECT_STREQ("29", SubstituteParams("{day}", p, &unknown).c_str());
    EXPECT_STREQ("14", SubstituteParams("{hour}", p, &unknown).c_str());
    EXPECT_STREQ("32", SubstituteParams("{minute}", p, &unknown).c_str());
    EXPECT_STREQ("07", SubstituteParams("{second}", p, &unknown).c_str());
    EXPECT_EQ(0, unknown);
}

TEST(ReceiptParams, WeekdayIsChinese) {
    auto p = SampleParams();
    int unknown = 0;
    // 2026-09-29 是星期二
    EXPECT_STREQ("星期二", SubstituteParams("{weekday}", p, &unknown).c_str());
}

TEST(ReceiptParams, LoseTicketPicksLoseTitle) {
    auto p = SampleParams();
    p.is_lose = true;
    int unknown = 0;
    EXPECT_STREQ("感谢参与", SubstituteParams("{title}", p, &unknown).c_str());
    EXPECT_STREQ("感谢参与", SubstituteParams("{lose_title}", p, &unknown).c_str());
}

TEST(ReceiptParams, UnknownPlaceholderKeptAndCounted) {
    auto p = SampleParams();
    int unknown = 0;
    EXPECT_STREQ("A{foo}B", SubstituteParams("A{foo}B", p, &unknown).c_str());
    EXPECT_EQ(1, unknown);
}

TEST(ReceiptParams, BraceEdgeCases) {
    auto p = SampleParams();
    int unknown = 0;
    EXPECT_STREQ("{}", SubstituteParams("{}", p, &unknown).c_str());
    EXPECT_STREQ("{", SubstituteParams("{", p, &unknown).c_str());
    EXPECT_STREQ("}", SubstituteParams("}", p, &unknown).c_str());
    EXPECT_STREQ("{ shop }", SubstituteParams("{ shop }", p, &unknown).c_str());  // 带空格不识别
    EXPECT_STREQ("恭喜中奖恭喜中奖", SubstituteParams("{title}{title}", p, &unknown).c_str());
    EXPECT_EQ(0, unknown);   // `{}` 与孤立花括号不算未知参数
}

TEST(ReceiptParams, NlBecomesNewline) {
    auto p = SampleParams();
    int unknown = 0;
    EXPECT_STREQ("a\nb", SubstituteParams("a{nl}b", p, &unknown).c_str());
    EXPECT_EQ(0, unknown);
}

TEST(ReceiptParams, ZeroEpochGivesPlaceholderDates) {
    // 时间未同步（epoch=0）时不能打出 1970，应给出可读占位
    auto p = SampleParams();
    p.epoch = 0;
    int unknown = 0;
    EXPECT_STREQ("----", SubstituteParams("{date}", p, &unknown).c_str());
    EXPECT_STREQ("--:--:--", SubstituteParams("{time}", p, &unknown).c_str());
}

TEST(ReceiptParams, ParamListCoversAllSubstitutable) {
    // Web UI 的参数清单必须覆盖所有能替换的键（防止"文档里有、界面里没有"）
    auto list = ReceiptParams::List();
    ReceiptParams p = SampleParams();
    for (const auto& kv : list) {
        int unknown = 0;
        SubstituteParams("{" + kv.first + "}", p, &unknown);
        EXPECT_EQ(0, unknown);   // 清单里的每个键都必须能真的替换
    }
    EXPECT_TRUE(list.size() >= 18);
}

// ============================================================================
// 排版与字节
// ============================================================================
static ReceiptTemplateV3 OneBlock(ReceiptBlock b, int cols = 32) {
    ReceiptTemplateV3 t;
    t.paper_cols = cols;
    t.blocks = {b};
    return t;
}

TEST(ReceiptRender, CenterAlignPadsBothSides) {
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = "AB";
    b.align = Align::kCenter;
    auto p = SampleParams();
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b), p);
    // 32 列居中 "AB" → 15 空格 + AB + 15 空格
    EXPECT_STREQ(std::string(15, ' ').append("AB").append(15, ' ').c_str(), text.c_str());
}

TEST(ReceiptRender, RightAlignPadsLeft) {
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = "AB";
    b.align = Align::kRight;
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b), SampleParams());
    EXPECT_STREQ(std::string(30, ' ').append("AB").c_str(), text.c_str());
}

TEST(ReceiptRender, SeparatorFillsPaperWidth) {
    ReceiptBlock b;
    b.type = BlockType::kSeparator;
    b.sep_char = '-';
    EXPECT_STREQ(std::string(32, '-').c_str(),
                 ReceiptRender::RenderPreviewText(OneBlock(b, 32), SampleParams()).c_str());
    EXPECT_STREQ(std::string(48, '-').c_str(),
                 ReceiptRender::RenderPreviewText(OneBlock(b, 48), SampleParams()).c_str());
}

TEST(ReceiptRender, LongLineWrapsWithinPaperWidth) {
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = std::string(100, 'x');
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b), SampleParams());
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        size_t len = (nl == std::string::npos ? text.size() : nl) - start;
        EXPECT_LE(len, (size_t)32);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

TEST(ReceiptRender, Size2HalvesWrapWidth) {
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = std::string(23, 'x');   // 23 > 16 → 必须在 2 倍宽下折成两行
    b.size = 2;
    b.align = Align::kLeft;
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b), SampleParams());
    EXPECT_NE(std::string::npos, text.find('\n'));
    // 逐行检查：2 倍字号每行占 **2 行纸**（第二行是续行，用于走纸），所有行宽度都必须 ≤ 半幅
    size_t pos = 0;
    int rows = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string row = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        EXPECT_LE(row.size(), (size_t)16);
        ++rows;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    EXPECT_GE(rows, 3);   // 折成 2 段 + 1 个续行
}

TEST(ReceiptRender, EmitsSizeAndBoldCodes) {
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = "A";
    b.size = 2;
    b.bold = true;
    auto bytes = ReceiptRender::Render(OneBlock(b), SampleParams(), nullptr);
    std::string s(bytes.begin(), bytes.end());
    EXPECT_NE(std::string::npos, s.find(std::string("\x1B\x45\x01", 3)));   // 粗体开
    EXPECT_NE(std::string::npos, s.find(std::string("\x1D\x21\x11", 3)));   // 2 倍宽高
    EXPECT_NE(std::string::npos, s.find(std::string("\x1D\x21\x00", 3)));   // 复位 1 倍
    EXPECT_NE(std::string::npos, s.find(std::string("\x1B\x45\x00", 3)));   // 粗体关
}

TEST(ReceiptRender, FeedEmitsExactLfCount) {
    ReceiptBlock b;
    b.type = BlockType::kFeed;
    b.feed_lines = 3;
    auto bytes = ReceiptRender::Render(OneBlock(b), SampleParams(), nullptr);
    int lf = 0;
    for (unsigned char c : bytes) {
        if (c == 0x0A) ++lf;
    }
    EXPECT_EQ(3, lf);
}

TEST(ReceiptRender, QrEmitsFiveCommands) {
    ReceiptBlock b;
    b.type = BlockType::kQr;
    b.text = "AB";
    b.qr_size = 6;
    b.qr_ecc = 49;
    auto bytes = ReceiptRender::Render(OneBlock(b), SampleParams(), nullptr);
    std::string s(bytes.begin(), bytes.end());
    // 1) 设模型  2) 设尺寸  3) 设纠错  4) 存数据  5) 打印
    EXPECT_NE(std::string::npos, s.find(std::string("\x1D\x28\x6B\x04\x00\x31\x41\x32\x00", 9)));
    EXPECT_NE(std::string::npos, s.find(std::string("\x1D\x28\x6B\x03\x00\x31\x43\x06", 8)));
    EXPECT_NE(std::string::npos, s.find(std::string("\x1D\x28\x6B\x03\x00\x31\x45\x31", 8)));
    EXPECT_NE(std::string::npos, s.find(std::string("\x1D\x28\x6B\x05\x00\x31\x50\x30" "AB", 10)));
    EXPECT_NE(std::string::npos, s.find(std::string("\x1D\x28\x6B\x03\x00\x31\x51\x30", 8)));
}

TEST(ReceiptRender, QrSkippedWhenContentEmpty) {
    ReceiptBlock b;
    b.type = BlockType::kQr;
    b.text = "";   // 未中奖时 content 可能解析为空
    auto bytes = ReceiptRender::Render(OneBlock(b), SampleParams(), nullptr);
    std::string s(bytes.begin(), bytes.end());
    EXPECT_EQ(std::string::npos, s.find(std::string("\x1D\x28\x6B", 3)));
}

TEST(ReceiptRender, LogoBlockSkippedWhenAbsent) {
    // 无 logo：跳过但其余块照常（D15）
    ReceiptTemplateV3 t;
    t.blocks = {{BlockType::kLogo, "", Align::kLeft, 1, false, '-', 6, 49, 4},
                {BlockType::kLine, "OK", Align::kLeft, 1, false, '-', 6, 49, 4}};
    auto bytes = ReceiptRender::Render(t, SampleParams(), nullptr);
    std::string s(bytes.begin(), bytes.end());
    EXPECT_EQ(std::string::npos, s.find(std::string("\x1D\x76\x30", 3)));   // 无光栅指令
    EXPECT_NE(std::string::npos, s.find("OK"));
}

TEST(ReceiptRender, LogoBlockEmitsRasterWhenPresent) {
    // 384×8 点 = 48 字节数据
    std::vector<uint8_t> logo(8 + 48, 0);
    logo[0] = 0x4C; logo[1] = 0x47; logo[2] = 0x4F; logo[3] = 0x31;   // LGO1
    logo[4] = 0x80; logo[5] = 0x01;   // width=384
    logo[6] = 0x08; logo[7] = 0x00;   // height=8
    LogoRenderInfo info{logo.data(), logo.size(), 384, 8, true};
    ReceiptTemplateV3 t;
    t.blocks = {{BlockType::kLogo, "", Align::kCenter, 1, false, '-', 6, 49, 4}};
    auto bytes = ReceiptRender::Render(t, SampleParams(), &info);
    std::string s(bytes.begin(), bytes.end());
    EXPECT_NE(std::string::npos, s.find(std::string("\x1D\x76\x30", 3)));   // GS v 0
}

// ============================================================================
// 金字节（锁死回归）
// ============================================================================
TEST(ReceiptRender, GoldenBytesForDefaultTemplate) {
    auto t = ReceiptTemplateV3::Default();
    auto bytes = ReceiptRender::Render(t, SampleParams(), nullptr);
    std::string s(bytes.begin(), bytes.end());
    // 必须包含这些关键片段（顺序敏感）；中文按 GBK 比对
    auto gbk = [](const std::string& u) {
        auto r = GbkEncoder::Encode(u);
        return std::string(r.bytes.begin(), r.bytes.end());
    };
    size_t i_shop = s.find(gbk("幸运翻页机"));
    size_t i_title = s.find(gbk("恭喜中奖"));
    size_t i_prize = s.find(gbk("一等奖"));
    size_t i_ticket = s.find(gbk("票号："));
    size_t i_qr = s.find(std::string("\x1D\x28\x6B", 3));
    EXPECT_NE(std::string::npos, i_shop);
    EXPECT_NE(std::string::npos, i_title);
    EXPECT_NE(std::string::npos, i_prize);
    EXPECT_NE(std::string::npos, i_ticket);
    EXPECT_NE(std::string::npos, i_qr);
    EXPECT_TRUE(i_shop < i_title);
    EXPECT_TRUE(i_title < i_prize);
    EXPECT_TRUE(i_prize < i_ticket);
    // 不得有 GBK 替换符（乱码）
    EXPECT_EQ(std::string::npos, s.find('?'));
}

// ============================================================================
// v2 → v3 迁移（必须无损）
// ============================================================================
TEST(ReceiptMigrate, LegacyHeaderFooterBecomeBlocks) {
    auto t = MigrateFromLegacy("{shop}{nl}===={nl}", "----{nl}谢谢惠顾{nl}", 32);
    EXPECT_STREQ("", t.Validate().c_str());
    auto text = ReceiptRender::RenderPreviewText(t, SampleParams());
    EXPECT_NE(std::string::npos, text.find("幸运翻页机"));
    EXPECT_NE(std::string::npos, text.find("===="));
    EXPECT_NE(std::string::npos, text.find("谢谢惠顾"));
    // 旧模板里的 {nl} 必须变成真正的换行，而不是字面 "{nl}"
    EXPECT_EQ(std::string::npos, text.find("{nl}"));
}

TEST(ReceiptMigrate, MigrationKeepsTitlePrizeInfoLines) {
    auto t = MigrateFromLegacy("{shop}{nl}", "{validity}", 32);
    auto text = ReceiptRender::RenderPreviewText(t, SampleParams());
    EXPECT_NE(std::string::npos, text.find("恭喜中奖"));       // 标题
    EXPECT_NE(std::string::npos, text.find("一等奖"));         // 奖项
    EXPECT_NE(std::string::npos, text.find("20260929-0007"));  // 票号
    EXPECT_NE(std::string::npos, text.find("2026-10-06"));     // 有效期
}

TEST(ReceiptMigrate, EmptyLegacyStillGivesUsableTemplate) {
    auto t = MigrateFromLegacy("", "", 32);
    EXPECT_STREQ("", t.Validate().c_str());
    EXPECT_TRUE(t.blocks.size() >= 3);
}

// ============================================================================
// 真机回归：畸形模板绝不能让设备崩（Web API 可远程提交模板 JSON）
// 曾因 "先 cJSON_Delete 再读节点字符串" 的 use-after-free →
// std::string(nullptr) 抛异常 → abort 重启，实测 39 次重启循环。
// ============================================================================
TEST(ReceiptTemplateV3, MalformedInputsNeverCrash) {
    const char* bad[] = {
        "{not json",
        "",
        "[]",
        "{}",
        R"({"version":3})",
        R"({"version":3,"blocks":[]})",
        R"({"version":2,"blocks":[{"type":"line","text":"x"}]})",
        R"({"version":3,"blocks":[{"type":"image"}]})",
        R"({"version":3,"blocks":[{"type":"line","text":"x","align":"middle"}]})",
        R"({"version":3,"blocks":[{"type":"line","size":3}]})",
        R"({"version":3,"blocks":[{"type":"qr","content":"x","ecc":47}]})",
        R"({"version":3,"blocks":[{"type":"qr","content":"x","size":99}]})",
        R"({"version":3,"blocks":[{"type":"feed","lines":99}]})",
        R"({"version":3,"paper_cols":7,"blocks":[{"type":"line","text":"x"}]})",
        R"({"version":3,"blocks":[1,2,3]})",
        R"({"version":3,"blocks":[{"type":null}]})",
    };
    for (const char* j : bad) {
        std::string err;
        auto t = ReceiptTemplateV3::FromJson(j, &err);
        EXPECT_FALSE(err.empty());     // 必须给出中文原因
        EXPECT_TRUE(t.blocks.empty());  // 失败时不产出可用模板
    }
    // 通过后再解析一次合法模板，确认解析器状态未被污染
    std::string err2;
    auto ok = ReceiptTemplateV3::FromJson(ReceiptTemplateV3::Default().ToJson(), &err2);
    EXPECT_STREQ("", err2.c_str());
    EXPECT_FALSE(ok.blocks.empty());
}

TEST(ReceiptTemplateV3, LegalEdgeCasesAreAccepted) {
    // 与 MalformedInputsNeverCrash 成对：这些看似"缺字段"的输入其实合法，
    // 不能被误判为错误（否则商家清空一行文字就保存不了）
    struct Case { const char* json; const char* why; };
    const Case ok[] = {
        {R"({"version":3,"blocks":[{"type":"line","text":null}]})", "空行（合法排版手段）"},
        {R"({"version":3,"blocks":[{"type":"line"}]})", "缺 text = 空行"},
        {R"({"version":3,"blocks":[{"type":"separator","char":""}]})", "空 char = 用默认 -"},
        {R"({"version":3,"blocks":[{"type":"logo"}]})", "logo 块无字段"},
        {R"({"version":3,"blocks":[{"type":"feed"}]})", "feed 默认 4 行"},
        {R"({"version":3,"blocks":[{"type":"qr","content":"x"}]})", "qr 默认尺寸/纠错"},
    };
    for (const auto& c : ok) {
        std::string err;
        auto t = ReceiptTemplateV3::FromJson(c.json, &err);
        EXPECT_STREQ("", err.c_str());
        EXPECT_FALSE(t.blocks.empty());
    }
}

// ============================================================================
// 参数化模板端到端：固定 epoch 下整张票的可见文本（含日期时间参数）
// ============================================================================
TEST(ReceiptRender, ParameterizedTemplateRendersRealDateTime) {
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    t.blocks = {
        {BlockType::kLine, "{shop}", Align::kCenter, 2, true, '-', 6, 49, 4},
        {BlockType::kSeparator, "", Align::kLeft, 1, false, '=', 6, 49, 4},
        {BlockType::kLine, "开奖时间：{date} {time}", Align::kLeft, 1, false, '-', 6, 49, 4},
        {BlockType::kLine, "{weekday}", Align::kRight, 1, false, '-', 6, 49, 4},
        {BlockType::kLine, "票号 {ticket}", Align::kLeft, 1, false, '-', 6, 49, 4},
        {BlockType::kQr, "{ticket}", Align::kCenter, 1, false, '-', 6, 49, 4},
    };
    auto text = ReceiptRender::RenderPreviewText(t, SampleParams());
    EXPECT_NE(std::string::npos, text.find("开奖时间：2026-09-29 14:32:07"));
    EXPECT_NE(std::string::npos, text.find("星期二"));
    EXPECT_NE(std::string::npos, text.find("票号 20260929-0007"));
    // 2 倍宽下 32 列纸只有 16 列可用：每行显示宽度不得超
    for (auto& line : {std::string("x")}) {
        (void)line;
    }
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string row = text.substr(start, (nl == std::string::npos ? text.size() : nl) - start);
        EXPECT_LE(ReceiptLayout::DisplayWidth(row), (size_t)32);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

// ============================================================================
// 校时（Web 浏览器时间）：合理性校验 + 拒坏值
// ============================================================================
TEST(TimeSource, SetFromWebRejectsImplausibleEpoch) {
    auto& ts = TimeSource::GetInstance();
    EXPECT_FALSE(ts.SetFromWeb(0));
    EXPECT_FALSE(ts.SetFromWeb(1));
    EXPECT_FALSE(ts.SetFromWeb(1000000000));      // 2001 年，太早
    EXPECT_FALSE(ts.SetFromWeb(4102444800LL));    // 2100-01-01，越上界
}

TEST(TimeSource, SetFromWebAcceptsPlausibleAndPersists) {
    auto& ts = TimeSource::GetInstance();
    auto& s = Settings::GetInstance();
    const int64_t saved = s.last_known_epoch();

    EXPECT_TRUE(ts.SetFromWeb(kT));
    EXPECT_EQ((long)kT, (long)ts.NowEpoch());        // 立即可用
    EXPECT_EQ((long)kT, (long)s.last_known_epoch()); // 已持久化（掉电可推算）

    s.set_last_known_epoch(saved);
    s.SaveSync();
}

TEST(TimeSource, SetFromWebMakesDateParamsReal) {
    auto& ts = TimeSource::GetInstance();
    EXPECT_TRUE(ts.SetFromWeb(kT));
    ReceiptParams p = SampleParams();
    p.epoch = ts.NowEpoch();
    int unknown = 0;
    EXPECT_STREQ("2026-09-29", SubstituteParams("{date}", p, &unknown).c_str());
    EXPECT_STREQ("14:32:07", SubstituteParams("{time}", p, &unknown).c_str());
}

TEST(ReceiptRender, PreviewNeverExceedsPaperWidthForAnyBlock) {
    // 全块类型 + 超长文本 + 长 URL 二维码：预览每一行的显示宽度都必须 ≤ 纸宽
    for (int cols : {32, 48}) {
        ReceiptTemplateV3 t;
        t.paper_cols = cols;
        t.blocks = {
            {BlockType::kLogo, "", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kLine, "{shop}", Align::kCenter, 2, true, '-', 6, 49, 4},
            {BlockType::kLine, std::string(100, 'x'), Align::kLeft, 1, false, '-', 6, 49, 4},
            {BlockType::kLine, "一等奖一等奖一等奖一等奖一等奖一等奖", Align::kCenter, 2, true, '-', 6, 49, 4},
            {BlockType::kSeparator, "", Align::kLeft, 1, false, '=', 6, 49, 4},
            {BlockType::kQr, "https://example.com/very/long/path?ticket={ticket}&check={check}", Align::kCenter, 1, false, '-', 6, 49, 4},
            {BlockType::kFeed, "", Align::kLeft, 1, false, '-', 6, 49, 4},
        };
        auto text = ReceiptRender::RenderPreviewText(t, SampleParams());
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl = text.find('\n', start);
            std::string row = text.substr(start, (nl == std::string::npos ? text.size() : nl) - start);
            EXPECT_LE(ReceiptLayout::DisplayWidth(row), (size_t)cols);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
}

// ============================================================================
// 画布列偏移 col（设计 §7.3b：横向拖动 → 列吸附；支持同行并排）
// ============================================================================
TEST(ReceiptCanvas, ColPadsLeadingSpaces) {
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = "AB";
    b.align = Align::kLeft;
    b.col = 10;
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b, 32), SampleParams());
    // 10 空格 + AB + 补到剩余宽度（32-10=22 → AB 后 20 空格）
    EXPECT_STREQ(std::string(10, ' ').append("AB").append(20, ' ').c_str(), text.c_str());
}

TEST(ReceiptCanvas, ColShrinksWrapWidth) {
    // col=20 → 可用宽度 12；20 个 ASCII 必须折成 12 + 8 两行
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = std::string(20, 'x');
    b.align = Align::kLeft;
    b.col = 20;
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b, 32), SampleParams());
    size_t nl = text.find('\n');
    ASSERT_NE(std::string::npos, nl);
    EXPECT_EQ((size_t)32, nl);                 // 第一行：20 空格 + 12 个 x
    EXPECT_EQ((size_t)32, text.size() - nl - 1);   // 第二行：20 空格 + 8 个 x + 4 空格
}

TEST(ReceiptCanvas, Size2HalvesBoxAfterCol) {
    // col=8, size=2 → 可用 24 列 → 2 倍宽下 box=12 字符
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = "ABCDEFGHIJKLM";   // 13 > 12 → 折行
    b.size = 2;
    b.col = 8;
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b, 32), SampleParams());
    EXPECT_NE(std::string::npos, text.find('\n'));
    EXPECT_EQ(8u, (size_t)text.find_first_not_of(' '));   // 首行 8 空格起
}

TEST(ReceiptCanvas, CenterAlignsWithinRemainingBox) {
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = "AB";
    b.align = Align::kCenter;
    b.col = 12;                 // 可用 20 → (20-2)/2 = 9 空格
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b, 32), SampleParams());
    EXPECT_STREQ(std::string(12 + 9, ' ').append("AB").append(9, ' ').c_str(), text.c_str());
}

TEST(ReceiptCanvas, SeparatorFillsFromColToEdge) {
    ReceiptBlock b;
    b.type = BlockType::kSeparator;
    b.sep_char = '-';
    b.col = 6;
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b, 32), SampleParams());
    EXPECT_EQ((size_t)32, text.size());
    EXPECT_EQ(6u, (size_t)text.find_first_not_of(' '));
    EXPECT_EQ(std::string::npos, text.find(' ' , 6));   // 从第 6 列起全是分隔符
}

TEST(ReceiptCanvas, SideBySideTwoBlocksRenderOnSeparateLinesByFlow) {
    // 流式模型下 col 只影响行内水平位置；同一行并排要靠"对齐+col"组合表达
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    ReceiptBlock left;
    left.type = BlockType::kLine; left.text = "P1"; left.col = 0;
    ReceiptBlock right;
    right.type = BlockType::kLine; right.text = "3F2A"; right.align = Align::kRight; right.col = 8;
    t.blocks = {left, right};
    auto text = ReceiptRender::RenderPreviewText(t, SampleParams());
    // 两行：第一行 P1，第二行在 col=8 起右对齐到 32
    EXPECT_NE(std::string::npos, text.find("P1"));
    EXPECT_NE(std::string::npos, text.find("3F2A"));
}

TEST(ReceiptCanvas, JsonRoundTripsCol) {
    ReceiptTemplateV3 t;
    t.blocks = {{BlockType::kLine, "x", Align::kLeft, 1, false, '-', 6, 49, 4, 17}};
    std::string err;
    auto back = ReceiptTemplateV3::FromJson(t.ToJson(), &err);
    EXPECT_STREQ("", err.c_str());
    ASSERT_EQ(1u, back.blocks.size());
    EXPECT_EQ(17, back.blocks[0].col);
}

TEST(ReceiptCanvas, RejectsColOutOfPaper) {
    auto t = ReceiptTemplateV3::Default();
    t.paper_cols = 32;
    t.blocks[0].col = 32;      // 越界（0..31）
    EXPECT_NE(std::string::npos, t.Validate().find("col"));
    t.blocks[0].col = 31;
    EXPECT_STREQ("", t.Validate().c_str());
}

TEST(ReceiptCanvas, DefaultColZeroKeepsClassicLayout) {
    // 回归：col 默认 0 时，渲染结果与加 col 之前完全一致（不能改变既有版式）
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    ReceiptBlock b;
    b.type = BlockType::kLine;
    b.text = "AB";
    b.align = Align::kCenter;
    auto text = ReceiptRender::RenderPreviewText(OneBlock(b, 32), SampleParams());
    EXPECT_STREQ(std::string(15, ' ').append("AB").append(15, ' ').c_str(), text.c_str());
}

// ============================================================================
// 画布绝对行 row（真正的"同一行并排"：左 Logo/文字 + 右文字）
// ============================================================================
TEST(ReceiptCanvas, SameRowBlocksShareOneLine) {
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    ReceiptBlock left;
    left.type = BlockType::kLine; left.text = "店名"; left.col = 0; left.row = 0;
    ReceiptBlock right;
    right.type = BlockType::kLine; right.text = "2026-09-30"; right.col = 12; right.row = 0;
    t.blocks = {left, right};
    auto text = ReceiptRender::RenderPreviewText(t, SampleParams());
    ASSERT_EQ(std::string::npos, text.find('\n'));      // 只有一行
    EXPECT_NE(std::string::npos, text.find("店名"));
    EXPECT_NE(std::string::npos, text.find("2026-09-30"));
    // 右侧内容必须落在 col=12 之后
    EXPECT_GE(text.find("2026-09-30"), (size_t)12);
}

TEST(ReceiptCanvas, SameRowEscPosRendersOneLine) {
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    ReceiptBlock a; a.type = BlockType::kLine; a.text = "A"; a.col = 0; a.row = 0;
    ReceiptBlock b; b.type = BlockType::kLine; b.text = "B"; b.col = 10; b.row = 0;
    t.blocks = {a, b};
    auto bytes = ReceiptRender::Render(t, SampleParams(), nullptr);
    std::string s(bytes.begin(), bytes.end());
    int lf = 0; for (char ch : s) if (ch == 0x0A) ++lf;
    EXPECT_EQ(1, lf);                                    // 一行（1 个换行）
    EXPECT_NE(std::string::npos, s.find("A"));
    EXPECT_NE(std::string::npos, s.find("B"));
}

TEST(ReceiptCanvas, AbsoluteRowLeavesGapRows) {
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    ReceiptBlock a; a.type = BlockType::kLine; a.text = "top"; a.row = 0;
    ReceiptBlock b; b.type = BlockType::kLine; b.text = "bottom"; b.row = 3;
    t.blocks = {a, b};
    auto text = ReceiptRender::RenderPreviewText(t, SampleParams());
    auto nl = std::count(text.begin(), text.end(), '\n');
    EXPECT_EQ(3, nl);                                    // 行 0、1、2 空、行 3
    EXPECT_LT(text.find("top"), text.find("bottom"));
}

TEST(ReceiptCanvas, MixedFlowAndAbsolute) {
    // 流式块（row=-1）与绝对行块混用：流式从 0 开始，绝对行钉在指定位置
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    ReceiptBlock flow; flow.type = BlockType::kLine; flow.text = "flow";   // row=-1
    ReceiptBlock abs;  abs.type = BlockType::kLine; abs.text = "abs"; abs.row = 2;
    t.blocks = {flow, abs};
    auto text = ReceiptRender::RenderPreviewText(t, SampleParams());
    EXPECT_LT(text.find("flow"), text.find("abs"));
    EXPECT_EQ(2, std::count(text.begin(), text.end(), '\n'));
}

TEST(ReceiptCanvas, RejectsOverlapOnSameRow) {
    ReceiptTemplateV3 t;
    t.blocks = {
        {BlockType::kLine, "A", Align::kLeft, 1, false, '-', 6, 49, 4, 5, 0},
        {BlockType::kLine, "B", Align::kLeft, 1, false, '-', 6, 49, 4, 5, 0},   // 同行同列
    };
    EXPECT_NE(std::string::npos, t.Validate().find("覆盖"));
}

TEST(ReceiptCanvas, RejectsRowOutOfRange) {
    auto t = ReceiptTemplateV3::Default();
    t.blocks[0].row = ReceiptTemplateV3::kMaxRows + 1;
    EXPECT_NE(std::string::npos, t.Validate().find("row"));
}

TEST(ReceiptCanvas, JsonRoundTripsRow) {
    ReceiptTemplateV3 t;
    t.blocks = {{BlockType::kLine, "x", Align::kLeft, 1, false, '-', 6, 49, 4, 3, 7}};
    std::string err;
    auto back = ReceiptTemplateV3::FromJson(t.ToJson(), &err);
    EXPECT_STREQ("", err.c_str());
    ASSERT_EQ(1u, back.blocks.size());
    EXPECT_EQ(7, back.blocks[0].row);
    EXPECT_EQ(3, back.blocks[0].col);
}

TEST(ReceiptCanvas, DefaultRowMinusOneKeepsFlowLayout) {
    // 回归：row 默认 -1 时保持流式版式（既有模板不受影响）
    auto t = ReceiptTemplateV3::Default();
    auto text = ReceiptRender::RenderPreviewText(t, SampleParams());
    EXPECT_LT(text.find("幸运翻页机"), text.find("恭喜中奖"));
    EXPECT_NE(std::string::npos, text.find("票号"));
}

TEST(ReceiptCanvas, RejectsEmptyQrContent) {
    // 回归：画布保存曾因字段名不匹配把二维码内容清空 → 小票上没有二维码，
    // 而且没有任何报错。现在必须在保存前就被拒绝。
    ReceiptTemplateV3 t;
    ReceiptBlock qr;
    qr.type = BlockType::kQr;
    qr.text = "";
    t.blocks = {qr};
    EXPECT_NE(std::string::npos, t.Validate().find("不能为空"));

    qr.text = "{ticket}{check}";
    t.blocks = {qr};
    EXPECT_STREQ("", t.Validate().c_str());
}

// ============================================================================
// 背景层叠加（页模式 ESC/POS page mode，设计 §7.3c）
// ============================================================================
namespace {
ReceiptTemplateV3 BgTemplate() {
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    ReceiptBlock qr;              // 二维码只做前景
    qr.type = BlockType::kQr;
    qr.text = "{ticket}";
    qr.row = 2;
    qr.col = 10;
    ReceiptBlock bglogo;          // logo 水印做背景
    bglogo.type = BlockType::kLogo;
    bglogo.row = 2;
    bglogo.bg = true;
    ReceiptBlock txt;
    txt.type = BlockType::kLine;
    txt.text = "OVERLAY";
    txt.row = 3;                  // 与背景同一区域
    txt.col = 2;
    ReceiptBlock txt2;
    txt2.type = BlockType::kLine;
    txt2.text = "BOTTOM";
    txt2.row = 9;
    t.blocks = {bglogo, qr, txt, txt2};
    return t;
}
}  // namespace

TEST(ReceiptBg, BackgroundAllowsOverlapWithText) {
    auto t = BgTemplate();
    // 背景与前景重叠是**允许**的（这正是叠加的用途）
    EXPECT_STREQ("", t.Validate().c_str());
}

TEST(ReceiptBg, OnlyLogoCanBeBackground) {
    // 文字不能做背景
    ReceiptTemplateV3 t;
    ReceiptBlock line;
    line.type = BlockType::kLine;
    line.text = "x";
    line.bg = true;
    t.blocks = {line};
    EXPECT_NE(std::string::npos, t.Validate().find("背景"));

    // 二维码也不能做背景（会遮挡码点、影响扫码 —— 现场确认的策略）
    ReceiptBlock qr;
    qr.type = BlockType::kQr;
    qr.text = "{ticket}";
    qr.bg = true;
    t.blocks = {qr};
    EXPECT_NE(std::string::npos, t.Validate().find("扫码"));
}

TEST(ReceiptBg, JsonRoundTripsBg) {
    auto t = BgTemplate();
    std::string err;
    auto back = ReceiptTemplateV3::FromJson(t.ToJson(), &err);
    EXPECT_STREQ("", err.c_str());
    ASSERT_EQ(4u, back.blocks.size());
    EXPECT_TRUE(back.blocks[0].bg);      // logo 背景
    EXPECT_FALSE(back.blocks[1].bg);     // 二维码前景
}

TEST(ReceiptBg, PageModeEmittedOnlyWhenSettingEnabled) {
    auto t = BgTemplate();
    const bool saved = Settings::GetInstance().printer_pagemode();

    Settings::GetInstance().set_printer_pagemode(false);
    auto std_bytes = ReceiptRender::Render(t, SampleParams(), nullptr);
    std::string s_std(std_bytes.begin(), std_bytes.end());
    EXPECT_EQ(std::string::npos, s_std.find(std::string("\x1b\x4c", 2)));   // 无 ESC L

    Settings::GetInstance().set_printer_pagemode(true);
    auto pg_bytes = ReceiptRender::Render(t, SampleParams(), nullptr);
    std::string s_pg(pg_bytes.begin(), pg_bytes.end());
    EXPECT_NE(std::string::npos, s_pg.find(std::string("\x1b\x4c", 2)));   // ESC L 进页面模式
    EXPECT_NE(std::string::npos, s_pg.find(std::string("\x1b\x57", 2)));   // ESC W 页面区
    EXPECT_NE(std::string::npos, s_pg.find(std::string("\x1b\x0c", 2)));   // ESC FF 打印页面
    EXPECT_NE(std::string::npos, s_pg.find(std::string("\x1b\x53", 2)));   // ESC S 回标准模式
    EXPECT_NE(std::string::npos, s_pg.find(std::string("\x1d\x24", 2)));   // GS $ 垂直定位
    EXPECT_NE(std::string::npos, s_pg.find("OVERLAY"));                      // 前景文字
    // 页模式下先输出背景图、再输出文字
    EXPECT_LT(s_pg.find(std::string("\x1b\x4c", 2)), s_pg.find("OVERLAY"));

    Settings::GetInstance().set_printer_pagemode(saved);
}

TEST(ReceiptBg, PageModeNotTriggeredWithoutBackground) {
    // 没有背景元素时保持原有行式路径（不得改变既有小票）
    auto t = ReceiptTemplateV3::Default();
    const bool saved = Settings::GetInstance().printer_pagemode();
    Settings::GetInstance().set_printer_pagemode(true);
    auto bytes = ReceiptRender::Render(t, SampleParams(), nullptr);
    std::string s(bytes.begin(), bytes.end());
    EXPECT_EQ(std::string::npos, s.find(std::string("\x1b\x4c", 2)));
    Settings::GetInstance().set_printer_pagemode(saved);
}

// ============================================================================
// 位图画布（图形模式：文字真正压在图上）
// ============================================================================
TEST(BmpCanvas, FontIsEmbeddedAndHasCommonGlyphs) {
    EXPECT_TRUE(BmpCanvas::FontAvailable());
    EXPECT_GT(BmpCanvas::FontGlyphCount(), 100);
    EXPECT_TRUE(BmpCanvas::HasGlyph('A'));
    EXPECT_TRUE(BmpCanvas::HasGlyph(0x4E2D));   // 中
    EXPECT_TRUE(BmpCanvas::HasGlyph(0x7968));   // 票
    EXPECT_TRUE(BmpCanvas::HasGlyph(' '));      // 空格不需要字形
}

TEST(BmpCanvas, MissingGlyphsReported) {
    EXPECT_STREQ("", BmpCanvas::MissingGlyphs("票号 ABC 123").c_str());
    // 一个几乎不可能收录的生僻字 → 必须被报出来（用于回退文字模式）
    std::string rare = "\u9FA5";  // 龥
    EXPECT_FALSE(BmpCanvas::MissingGlyphs(rare).empty());
}

TEST(BmpCanvas, Utf8Decoding) {
    uint32_t code = 0;
    EXPECT_EQ(1, Utf8Next("A", 0, &code));
    EXPECT_EQ('A', code);
    EXPECT_EQ(3, Utf8Next("中", 0, &code));
    EXPECT_EQ(0x4E2Du, code);
}

TEST(BmpCanvas, DrawTextSetsPixelsAndAdvances) {
    BmpCanvas c(384, 48);
    std::string miss;
    const int end_x = c.DrawText(0, 0, "A", 1, &miss);
    EXPECT_EQ(12, end_x);                       // ASCII 半宽 12 点
    EXPECT_STREQ("", miss.c_str());
    int black = 0;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 12; ++x) if (c.GetPixel(x, y)) ++black;
    EXPECT_GT(black, 5);                        // 确实画上了

    BmpCanvas c2(384, 48);
    EXPECT_EQ(24, c2.DrawText(0, 0, "中", 1, &miss));   // 汉字全宽 24 点
}

TEST(BmpCanvas, Size2DoublesAdvance) {
    BmpCanvas c(384, 96);
    std::string miss;
    EXPECT_EQ(24, c.DrawText(0, 0, "A", 2, &miss));     // 2 倍宽 = 24 点
}

TEST(BmpCanvas, RasterCommandsChunkedWithGsv0) {
    BmpCanvas c(384, 60);                       // 60 行 → 24+24+12 三块
    c.SetPixel(0, 0, true);
    auto s = c.ToRasterCommands(24);
    int chunks = 0;
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if ((uint8_t)s[i] == 0x1D && (uint8_t)s[i+1] == 0x76 && (uint8_t)s[i+2] == 0x30) ++chunks;
    }
    EXPECT_EQ(3, chunks);
    // 每块头部 xL/xH 应为 384/8 = 48 字节
    EXPECT_EQ(48, (uint8_t)s[4]);
}

TEST(BmpCanvas, OverlayTwoDrawsOnSamePixels) {
    // 叠加的本质：后画的像素落在**已画内容之上**。
    // 用"空心方框 + 框内画字"来证明：框内原本是白的，画字后必须出现黑点。
    BmpCanvas c(64, 32);
    // 24×24 的空心方框（只描边）
    uint8_t box[3 * 24];
    memset(box, 0, sizeof(box));
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 24; ++x) {
            const bool edge = (x == 0 || x == 23 || y == 0 || y == 23);
            if (edge) box[y * 3 + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
        }
    }
    c.DrawImage1bpp(0, 0, box, 24, 24, 64);

    int inside_before = 0;
    for (int y = 2; y < 22; ++y)
        for (int x = 2; x < 22; ++x) if (c.GetPixel(x, y)) ++inside_before;
    EXPECT_EQ(0, inside_before);                 // 框内本来是空的

    std::string miss;
    c.DrawText(0, 0, "中", 1, &miss);             // 文字画在框内
    int inside_after = 0;
    for (int y = 2; y < 22; ++y)
        for (int x = 2; x < 22; ++x) if (c.GetPixel(x, y)) ++inside_after;
    EXPECT_GT(inside_after, 20);                 // 字确实压在框内 → 叠加成立
    EXPECT_STREQ("", miss.c_str());
    // 边框也还在（两者共存）
    EXPECT_TRUE(c.GetPixel(0, 0));
    EXPECT_TRUE(c.GetPixel(23, 23));
}

TEST(ReceiptGfx, BackgroundTemplateUsesGraphicsModeWhenPageModeOff) {
    // 有背景水印块 + 页面模式关 → 走图形模式：输出应为整幅光栅（GS v 0），而非逐行文字
    auto& s = Settings::GetInstance();
    const bool saved_pm = s.printer_pagemode();
    s.set_printer_pagemode(false);

    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    ReceiptBlock logo;
    logo.type = BlockType::kLogo;
    logo.align = Align::kCenter;
    logo.bg = true;
    ReceiptBlock txt;
    txt.type = BlockType::kLine;
    txt.text = "票号 {ticket}";
    txt.row = 1;
    t.blocks = {logo, txt};

    std::vector<uint8_t> fake;
    fake.resize(8 + 8 * 8, 0);
    memcpy(fake.data(), "LGO1", 4);
    fake[4] = 64; fake[5] = 0; fake[6] = 8; fake[7] = 0;
    LogoRenderInfo info;
    info.valid = true;
    info.data = fake.data();
    info.len = fake.size();
    info.width_dots = 64;
    info.height_dots = 8;

    auto bytes = ReceiptRender::Render(t, MakeSampleParams(), &info);
    std::string out(bytes.begin(), bytes.end());
    EXPECT_NE(std::string::npos, out.find(std::string("\x1d\x76\x30", 3)));   // GS v 0 整幅
    EXPECT_EQ(std::string::npos, out.find(std::string("\x1b\x4c", 2)));        // 无页面模式
    s.set_printer_pagemode(saved_pm);
}

// ============================================================================
// 模版模式体系（设计 §7.3c）：flow / canvas / banner 三模式可选
// ============================================================================
TEST(TemplateModeKinds, NamesAndParsing) {
    EXPECT_STREQ("flow", TemplateModeName(TemplateMode::kFlow));
    EXPECT_STREQ("canvas", TemplateModeName(TemplateMode::kCanvas));
    EXPECT_STREQ("banner", TemplateModeName(TemplateMode::kBanner));
    TemplateMode m = TemplateMode::kFlow;
    EXPECT_TRUE(TemplateModeFromString("banner", &m));
    EXPECT_TRUE(m == TemplateMode::kBanner);
    EXPECT_FALSE(TemplateModeFromString("image", &m));
}

TEST(TemplateModeKinds, LegacyInferenceKeepsBehavior) {
    // 兼容规则：mode 缺省时，有 bg 块 → canvas；无 bg 块 → flow
    ReceiptTemplateV3 plain;
    ReceiptBlock line;
    line.type = BlockType::kLine;
    line.text = "x";
    plain.blocks = {line};
    plain.ResolveMode();
    EXPECT_TRUE(plain.mode == TemplateMode::kFlow);

    ReceiptTemplateV3 with_bg;
    ReceiptBlock bg;
    bg.type = BlockType::kLogo;
    bg.bg = true;
    with_bg.blocks = {bg, line};
    with_bg.ResolveMode();
    EXPECT_TRUE(with_bg.mode == TemplateMode::kCanvas);
}

TEST(TemplateModeKinds, JsonRoundTripsMode) {
    ReceiptTemplateV3 t;
    ReceiptBlock l;
    l.type = BlockType::kLine;
    l.text = "x";
    t.blocks = {l};
    t.mode_raw = "banner";
    t.ResolveMode();
    t.banner_top_rows = 2;
    t.banner_bottom_rows = 4;
    std::string err;
    auto back = ReceiptTemplateV3::FromJson(t.ToJson(), &err);
    EXPECT_STREQ("", err.c_str());
    EXPECT_TRUE(back.EffectiveMode() == TemplateMode::kBanner);   // 模式是推导值
    EXPECT_EQ(2, back.banner_top_rows);
    EXPECT_EQ(4, back.banner_bottom_rows);
}

TEST(TemplateModeKinds, RejectsUnknownModeAndBadRows) {
    ReceiptTemplateV3 t;
    ReceiptBlock l;
    l.type = BlockType::kLine;
    l.text = "x";
    t.mode_raw = "image";
    t.blocks = {l};
    EXPECT_NE(std::string::npos, t.Validate().find("mode"));

    ReceiptTemplateV3 b;
    b.mode_raw = "banner";
    b.blocks = {l};
    b.banner_top_rows = 0;
    b.banner_bottom_rows = 0;
    EXPECT_NE(std::string::npos, b.Validate().find("花边"));

    ReceiptTemplateV3 c;
    c.mode_raw = "banner";
    c.blocks = {l};
    c.banner_top_rows = 99;
    c.banner_bottom_rows = 1;
    EXPECT_NE(std::string::npos, c.Validate().find("banner_top_rows"));
}

TEST(TemplateModeKinds, FlowWithBackgroundIsRejected) {
    ReceiptTemplateV3 t;
    t.mode_raw = "flow";
    ReceiptBlock bg;
    bg.type = BlockType::kLogo;
    bg.bg = true;
    t.blocks = {bg};
    t.ResolveMode();
    EXPECT_NE(std::string::npos, t.Validate().find("flow"));
}

TEST(TemplateModeKinds, BannerEmitsTwoRastersWithTextBetween) {
    ReceiptTemplateV3 t;
    t.mode_raw = "banner";
    t.paper_cols = 32;
    t.banner_top_rows = 1;      // 24 点 = 恰好 1 个光栅块（否则 24 行分块会让"第二块"落在顶花边内部）
    t.banner_bottom_rows = 1;
    ReceiptBlock line;
    line.type = BlockType::kLine;
    line.text = "HELLO";
    t.blocks = {line};
    t.ResolveMode();

    // 造一张 32×72 点的图：上 24 点给顶花边、下 24 点给底花边
    std::vector<uint8_t> fake(8 + 4 * 72, 0xAA);   // 32/8=4 字节/行 × 72 行
    memcpy(fake.data(), "LGO1", 4);
    fake[4] = 32; fake[5] = 0; fake[6] = 72; fake[7] = 0;
    LogoRenderInfo info;
    info.valid = true;
    info.data = fake.data();
    info.len = fake.size();
    info.width_dots = 32;
    info.height_dots = 72;

    auto bytes = ReceiptRender::Render(t, MakeSampleParams(), &info);
    std::string out(bytes.begin(), bytes.end());
    const std::string gsv0 = std::string("\x1d\x76\x30", 3);
    const size_t first = out.find(gsv0);
    ASSERT_NE(std::string::npos, first);
    const size_t second = out.find(gsv0, first + 1);
    EXPECT_NE(std::string::npos, second);          // 两段花边
    const size_t text = out.find("HELLO");
    ASSERT_NE(std::string::npos, text);
    EXPECT_LT(first, text);                        // 顶花边在文字之前
    EXPECT_LT(text, second);                       // 底花边在文字之后
}

TEST(TemplateModeKinds, BannerWithoutImageStillPrintsText) {
    // 图缺失时不得打乱版式：只出文字
    ReceiptTemplateV3 t;
    t.mode_raw = "banner";
    t.banner_top_rows = 2;
    t.banner_bottom_rows = 2;
    ReceiptBlock line;
    line.type = BlockType::kLine;
    line.text = "TEXTONLY";
    t.blocks = {line};
    t.ResolveMode();
    auto bytes = ReceiptRender::Render(t, MakeSampleParams(), nullptr);
    std::string out(bytes.begin(), bytes.end());
    EXPECT_NE(std::string::npos, out.find("TEXTONLY"));
    EXPECT_EQ(std::string::npos, out.find(std::string("\x1d\x76\x30", 3)));
}

// ============================================================================
// 按模式分槽存储（设计 §7.3d）：每种版式类型各有自己的模板
// ============================================================================
TEST(PerModeSlots, EachModeKeepsItsOwnTemplate) {
    auto& s = Settings::GetInstance();
    const std::string f0 = s.receipt_tmpl(), c0 = s.tmpl_canvas(), b0 = s.tmpl_banner();
    const int m0 = s.active_tmpl_mode();

    // 三种模式各存一份互不相同的模板
    ReceiptTemplateV3 tf; tf.mode_raw = "flow";
    ReceiptBlock lf; lf.type = BlockType::kLine; lf.text = "FLOW-TPL";
    tf.blocks = {lf};
    ReceiptTemplateV3 tc; tc.mode_raw = "canvas";
    ReceiptBlock lc; lc.type = BlockType::kLine; lc.text = "CANVAS-TPL";
    tc.blocks = {lc};
    ReceiptTemplateV3 tb; tb.mode_raw = "banner";
    tb.banner_top_rows = 1; tb.banner_bottom_rows = 1;
    ReceiptBlock lb; lb.type = BlockType::kLine; lb.text = "BANNER-TPL";
    tb.blocks = {lb};

    EXPECT_TRUE(SaveTemplateToSettings(tf, false));
    EXPECT_TRUE(SaveTemplateToSettings(tc, false));
    EXPECT_TRUE(SaveTemplateToSettings(tb, false));

    // 三个槽各自独立，且按生效模式取到对应模板
    s.set_active_tmpl_mode(0);
    auto g0 = LoadTemplateFromSettings();
    EXPECT_STREQ("FLOW-TPL", g0.blocks[0].text.c_str());
    s.set_active_tmpl_mode(1);
    auto g1 = LoadTemplateFromSettings();
    EXPECT_STREQ("CANVAS-TPL", g1.blocks[0].text.c_str());
    s.set_active_tmpl_mode(2);
    auto g2 = LoadTemplateFromSettings();
    EXPECT_STREQ("BANNER-TPL", g2.blocks[0].text.c_str());
    EXPECT_EQ(1, g2.banner_top_rows);

    // 还原
    s.set_receipt_tmpl(f0); s.set_tmpl_canvas(c0); s.set_tmpl_banner(b0);
    s.set_active_tmpl_mode(m0);
    EXPECT_TRUE(s.SaveSync());
}

TEST(PerModeSlots, EmptySlotFallsBackToBuiltinOfThatMode) {
    auto& s = Settings::GetInstance();
    const std::string f0 = s.receipt_tmpl(), c0 = s.tmpl_canvas(), b0 = s.tmpl_banner();
    const int m0 = s.active_tmpl_mode();

    s.set_tmpl_canvas("");        // canvas 槽空 → 回退该模式内置版式
    s.set_active_tmpl_mode(1);
    auto t = LoadTemplateFromSettings();
    EXPECT_FALSE(t.blocks.empty());
    EXPECT_TRUE(t.EffectiveMode() == TemplateMode::kCanvas ||
                t.EffectiveMode() == TemplateMode::kFlow);   // 内置水印版式含 bg → canvas

    s.set_tmpl_banner("");
    s.set_active_tmpl_mode(2);
    auto tb = LoadTemplateFromSettings();
    EXPECT_FALSE(tb.blocks.empty());
    EXPECT_TRUE(tb.EffectiveMode() == TemplateMode::kBanner);

    s.set_receipt_tmpl(f0); s.set_tmpl_canvas(c0); s.set_tmpl_banner(b0);
    s.set_active_tmpl_mode(m0);
    EXPECT_TRUE(s.SaveSync());
}

// ============================================================================
// 出厂预设（设计 §7.3e）：三种模式各有一份可直接用的预设
// ============================================================================
TEST(Presets, PresetsAreValidForAllModes) {
    for (int variant = 1; variant <= 6; ++variant) {
        auto t = BuiltinTemplate(variant);
        EXPECT_FALSE(t.blocks.empty());
        EXPECT_STREQ("", t.Validate().c_str());
    }
    // flow / canvas / banner 三种预设的模式必须正确
    EXPECT_TRUE(BuiltinTemplate(1).EffectiveMode() == TemplateMode::kFlow);
    EXPECT_TRUE(BuiltinTemplate(5).EffectiveMode() == TemplateMode::kCanvas);
    EXPECT_TRUE(BuiltinTemplate(6).EffectiveMode() == TemplateMode::kBanner);
    // canvas 预设必须带背景层（否则退化成"图一张、字一张"）
    int bg = 0;
    for (const auto& b : BuiltinTemplate(5).blocks) {
        if (b.bg) ++bg;
    }
    EXPECT_EQ(1, bg);
    // banner 预设必须有花边行数
    EXPECT_GT(BuiltinTemplate(6).banner_top_rows, 0);
}

TEST(Presets, ResetAllWritesAllThreeSlots) {
    auto& s = Settings::GetInstance();
    const std::string f0 = s.receipt_tmpl(), c0 = s.tmpl_canvas(), b0 = s.tmpl_banner();
    const int m0 = s.active_tmpl_mode();

    EXPECT_TRUE(ResetTemplatesToPreset(-1));          // 三个都重置
    EXPECT_FALSE(s.receipt_tmpl().empty());
    EXPECT_FALSE(s.tmpl_canvas().empty());
    EXPECT_FALSE(s.tmpl_banner().empty());
    for (int m = 0; m < 3; ++m) {
        s.set_active_tmpl_mode(m);
        auto t = LoadTemplateFromSettings();
        EXPECT_FALSE(t.blocks.empty());
        EXPECT_STREQ("", t.Validate().c_str());
    }
    // 单模式重置
    EXPECT_TRUE(ResetTemplatesToPreset(2));
    EXPECT_FALSE(ResetTemplatesToPreset(9));          // 非法模式

    s.set_receipt_tmpl(f0); s.set_tmpl_canvas(c0); s.set_tmpl_banner(b0);
    s.set_active_tmpl_mode(m0);
    EXPECT_TRUE(s.SaveSync());
}
