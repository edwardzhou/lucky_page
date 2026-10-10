#include <cstring>
// test_printer_service.cc — 打印任务投递层（真机回归：std::string 被队列字节 memcpy）
#include "lucky_test.h"

#include <string>

#include "printer/gbk_encoder.h"
#include "printer/printer_service.h"
#include "printer/printer_uart.h"
#include "printer/receipt_defaults.h"
#include "printer/receipt_render.h"
#include "printer/receipt_template.h"
#include "settings.h"

// 构造一个"长串 + 全块类型"的模板，专门压投递层的深拷贝正确性
static ReceiptTemplateV3 LongTemplate() {
    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    t.blocks = {
        {BlockType::kLogo, "", Align::kCenter, 1, false, '-', 6, 49, 4},
        {BlockType::kLine, "梅州市梅江区老街口幸运翻页机旗舰店", Align::kCenter, 1, false, '-', 6, 49, 4},
        {BlockType::kLine, "==================", Align::kCenter, 1, false, '-', 6, 49, 4},
        {BlockType::kLine, "{title}", Align::kCenter, 2, true, '-', 6, 49, 4},
        {BlockType::kLine, "{prize}", Align::kCenter, 2, true, '-', 6, 49, 4},
        {BlockType::kLine, "时间：{datetime}（{weekday}）", Align::kLeft, 1, false, '-', 6, 49, 4},
        {BlockType::kSeparator, "", Align::kLeft, 1, false, '=', 6, 49, 4},
        {BlockType::kLine, "票号：{ticket}", Align::kLeft, 1, false, '-', 6, 49, 4},
        {BlockType::kQr, "{ticket}{check}", Align::kCenter, 1, false, '-', 8, 50, 4},
        {BlockType::kFeed, "", Align::kLeft, 1, false, '-', 6, 49, 3},
    };
    return t;
}

static ReceiptParams LongParams() {
    ReceiptParams p;
    p.shop = "梅州市梅江区老街口幸运翻页机旗舰店";   // 51B，远超 SSO
    p.win_title = "恭喜您中奖啦";
    p.lose_title = "感谢参与下次再来";
    p.prize = "特等奖";
    p.note = "奖品：扫地机器人一台（含安装服务）";
    p.ticket = "SZ20260929-1234";
    p.check = "3F2A";
    p.page = 28;
    p.validity = "2026-10-06";
    p.epoch = 1790663527;
    return p;
}

TEST(PrinterService, LongStringsSurviveDelivery) {
    // 真机踩坑：曾用 xQueueSend 传含 std::string 的 PrintJob → 按字节 memcpy，
    // 长串（>SSO 15B）的堆指针在临时 job 析构后悬空 → 打出乱码。
    PrinterService svc;
    ASSERT_TRUE(svc.InitDeliveryForTest());

    auto t = LongTemplate();
    auto p = LongParams();
    EXPECT_TRUE(svc.Enqueue(t, p));

    ReceiptTemplateV3 t2;
    ReceiptParams p2;
    ASSERT_TRUE(svc.TakePendingForTest(&t2, &p2));

    EXPECT_EQ(t.blocks.size(), t2.blocks.size());
    for (size_t i = 0; i < t.blocks.size(); ++i) {
        EXPECT_TRUE(t.blocks[i].type == t2.blocks[i].type);
        EXPECT_STREQ(t.blocks[i].text.c_str(), t2.blocks[i].text.c_str());
        EXPECT_TRUE(t.blocks[i].align == t2.blocks[i].align);
        EXPECT_EQ(t.blocks[i].size, t2.blocks[i].size);
        EXPECT_EQ(t.blocks[i].bold, t2.blocks[i].bold);
        EXPECT_EQ(t.blocks[i].sep_char, t2.blocks[i].sep_char);
        EXPECT_EQ(t.blocks[i].qr_size, t2.blocks[i].qr_size);
        EXPECT_EQ(t.blocks[i].qr_ecc, t2.blocks[i].qr_ecc);
        EXPECT_EQ(t.blocks[i].feed_lines, t2.blocks[i].feed_lines);
    }
    EXPECT_STREQ(p.ticket.c_str(), p2.ticket.c_str());
    EXPECT_STREQ(p.prize.c_str(), p2.prize.c_str());
    EXPECT_STREQ(p.note.c_str(), p2.note.c_str());
    EXPECT_EQ(p.page, p2.page);
    EXPECT_EQ((long)p.epoch, (long)p2.epoch);
    EXPECT_EQ(t.paper_cols, t2.paper_cols);
}

TEST(PrinterService, DeliveredJobRendersWithoutMojibake) {
    // 端到端：投递后的任务渲染出的字节必须与直接渲染完全一致（乱码会被这条抓住）
    PrinterService svc;
    ASSERT_TRUE(svc.InitDeliveryForTest());
    auto t = LongTemplate();
    auto p = LongParams();

    auto direct = ReceiptRender::Render(t, p, nullptr);
    ASSERT_TRUE(svc.Enqueue(t, p));
    ReceiptTemplateV3 t2;
    ReceiptParams p2;
    ASSERT_TRUE(svc.TakePendingForTest(&t2, &p2));
    auto viaService = ReceiptRender::Render(t2, p2, nullptr);

    EXPECT_EQ(direct.size(), viaService.size());
    EXPECT_TRUE(direct == viaService);          // 逐字节相同
    std::string s(viaService.begin(), viaService.end());
    EXPECT_EQ(std::string::npos, s.find('?'));  // 不含替换符（乱码会有）
}

TEST(PrinterService, SingleFlightRejectsSecondWhilePending) {
    PrinterService svc;
    ASSERT_TRUE(svc.InitDeliveryForTest());
    ReceiptTemplateV3 t;
    ReceiptParams p;
    EXPECT_TRUE(svc.Enqueue(t, p));
    EXPECT_FALSE(svc.Enqueue(t, p));     // 单飞（D4）：未取走前拒绝
    ReceiptTemplateV3 t2;
    ReceiptParams p2;
    EXPECT_TRUE(svc.TakePendingForTest(&t2, &p2));
    EXPECT_TRUE(svc.Enqueue(t, p));      // 取走后可再投
}

TEST(PrinterService, SampleTicketUsesCurrentSettings) {
    // 自检页必须反映当前配置（曾用空模板，打不出商家名/票头票尾，无法验证配置）。
    // ⚠️ 测试不得留副作用：NVS 与主固件共享，改了必须还原。
    auto& s = Settings::GetInstance();
    const std::string saved_name = s.shop_name();
    const std::string saved_tmpl = s.receipt_tmpl();
    const bool saved_logo = s.logo_enabled();

    s.set_shop_name("自检店");
    s.set_receipt_tmpl(ReceiptTemplateV3::Default().ToJson());
    auto t = LoadTemplateFromSettings();
    EXPECT_STREQ("", t.Validate().c_str());

    ReceiptParams p = MakeSampleParams();
    EXPECT_STREQ("自检店", p.shop.c_str());
    EXPECT_STREQ("TEST-0000", p.ticket.c_str());
    EXPECT_FALSE(p.prize.empty());

    s.set_shop_name(saved_name);
    s.set_receipt_tmpl(saved_tmpl);
    s.set_logo_enabled(saved_logo);
    EXPECT_TRUE(s.SaveSync());
    EXPECT_STREQ(saved_name.c_str(), s.shop_name().c_str());
}

TEST(PrinterService, LoadTemplateFallsBackWhenStoredJsonBroken) {
    // 存进 NVS 的模板若损坏（掉电/手改），必须回退迁移旧模板而不是打不出票
    auto& s = Settings::GetInstance();
    const std::string saved = s.receipt_tmpl();
    s.set_receipt_tmpl("{broken json");
    auto t = LoadTemplateFromSettings();
    EXPECT_STREQ("", t.Validate().c_str());     // 仍是可用模板
    EXPECT_FALSE(t.blocks.empty());
    s.set_receipt_tmpl(saved);
    EXPECT_TRUE(s.SaveSync());
}

TEST(PrinterService, MigrationRespectsLegacyQrSwitch) {
    // 旧 qr_enabled=0 的机器迁移后不应突然多出二维码
    auto& s = Settings::GetInstance();
    const std::string saved_tmpl = s.receipt_tmpl();
    const int saved_qr = s.qr_enabled();
    s.set_receipt_tmpl("");            // 强制走迁移路径
    s.set_qr_enabled(0);
    auto t = LoadTemplateFromSettings();
    for (const auto& b : t.blocks) EXPECT_FALSE(b.type == BlockType::kQr);
    s.set_qr_enabled(1);
    auto t2 = LoadTemplateFromSettings();
    int qr = 0;
    for (const auto& b : t2.blocks) if (b.type == BlockType::kQr) ++qr;
    EXPECT_EQ(1, qr);
    s.set_qr_enabled(saved_qr);
    s.set_receipt_tmpl(saved_tmpl);
    EXPECT_TRUE(s.SaveSync());
}

TEST(PrinterService, LogoBlockEmitsRasterRegardlessOfLegacyFlag) {
    // 回归：模板里有 logo 块时，必须真的把光栅发出去。
    // 曾因 v2 遗留全局开关 logo_enabled 默认为 0，导致"模板放了 logo 却打不出图"。
    auto& s = Settings::GetInstance();
    const bool saved_flag = s.logo_enabled();
    s.set_logo_enabled(false);          // 故意把遗留开关关掉

    ReceiptTemplateV3 t;
    t.paper_cols = 32;
    ReceiptBlock logo;
    logo.type = BlockType::kLogo;
    ReceiptBlock txt;
    txt.type = BlockType::kLine;
    txt.text = "x";
    t.blocks = {logo, txt};

    // 传入有效 logo 数据 → 渲染结果必须含 GS v 0 光栅指令
    std::vector<uint8_t> fake;
    fake.resize(8 + 8 * 8, 0);          // 64×8 点
    memcpy(fake.data(), "LGO1", 4);
    fake[4] = 64; fake[5] = 0; fake[6] = 8; fake[7] = 0;
    LogoRenderInfo info;
    info.valid = true;
    info.data = fake.data();
    info.len = fake.size();
    info.width_dots = 64;
    info.height_dots = 8;

    auto bytes = ReceiptRender::Render(t, MakeSampleParams(), &info);
    std::string s_out(bytes.begin(), bytes.end());
    EXPECT_NE(std::string::npos, s_out.find(std::string("\x1d\x76\x30", 3)));   // GS v 0

    s.set_logo_enabled(saved_flag);
}

// ============================================================================
// 纸尽判定（纯函数）：GS r 1 应答的 bit2..3 == 11 表示纸尽
// ============================================================================
TEST(PrinterPaper, PaperEndFromStatusBits) {
    EXPECT_FALSE(PrinterPaperEndFromStatus(0x00));   // 有纸（真机实测值）
    EXPECT_TRUE(PrinterPaperEndFromStatus(0x0F));    // 纸尽（真机实测值）
    EXPECT_TRUE(PrinterPaperEndFromStatus(0x0C));    // bit2..3 = 11
    EXPECT_FALSE(PrinterPaperEndFromStatus(0x03));   // 仅"纸将尽"，不算纸尽
    EXPECT_FALSE(PrinterPaperEndFromStatus(0x08));   // bit3 单独置位不算
}
