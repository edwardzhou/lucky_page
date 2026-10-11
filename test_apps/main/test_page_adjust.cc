// test_page_adjust.cc — 行程偏移（标定「页码调整模式」的数据模型）
//
// 背景：标定页从"填绝对行程值表格"改为 page-player 式**调整模式** ——
// 基础映射（PageMapping）保持不变，逐页叠加 int8 偏移（PageAdjust），
// 实际值 = clamp(base + offset, 0, 255)，偏移 0 表示未调整。
//
// 这里锁定该模型的关键语义（不依赖 SPIFFS：测试宿主未挂载，保存失败只记日志）：
//   · 偏移默认 0、实际值等于基础值
//   · 改偏移后实际值随之变化、可往返读回
//   · 实际值必须钳位到 0..255（否则 DMX 会收到越界值）
//   · 越界页/cell 必须安全返回（不崩、不读脏数据）
//   · Resize（改总页数）**保留**已有偏移，新页补 0
//   · ResetAll 全清零；单页清零不得影响其它页（接口层「清零本页」用它实现）
#include "lucky_test.h"

#include "page_adjust.h"
#include "page_mapping.h"
#include "settings.h"

namespace {

/// 统一准备：写入已知基础映射（0/50/100/150/200）+ 固定规模，
/// 避免依赖设备/文件里的历史数据。
/// ⚠ 顺序：先 SetMapping（置 loaded_，防止 GetMapping 再回读文件），
///   再 LoadAdjust（置 loaded_，防止 SetOffset 触发 Load 覆盖规模），最后显式 Resize。
void Prepare(int pages = 5) {
    std::vector<uint8_t> base;
    for (int i = 0; i < pages; i++) base.push_back(static_cast<uint8_t>(i * 50));
    PageMapping::GetInstance().SetMapping(base);
    PageAdjust::GetInstance().LoadAdjust();
    PageAdjust::GetInstance().Resize(1, pages);
    PageAdjust::GetInstance().ResetAll();
}

}  // namespace

TEST(PageAdjust, OffsetDefaultsToZeroAndValueEqualsBase) {
    Prepare();
    EXPECT_EQ(0, PageAdjust::GetInstance().GetOffset(0, 2));
    EXPECT_EQ(100, PageAdjust::GetInstance().GetAdjustedValue(0, 2));
}

TEST(PageAdjust, SetOffsetAppliesAndRoundTrips) {
    Prepare();
    PageAdjust::GetInstance().SetOffset(0, 2, 7);
    EXPECT_EQ(7, PageAdjust::GetInstance().GetOffset(0, 2));
    EXPECT_EQ(107, PageAdjust::GetInstance().GetAdjustedValue(0, 2));
}

TEST(PageAdjust, AdjustedValueClampsToUint8Range) {
    Prepare();
    PageAdjust::GetInstance().SetOffset(0, 0, -30);   // 0 - 30 < 0 → 钳到 0
    EXPECT_EQ(0, PageAdjust::GetInstance().GetAdjustedValue(0, 0));
    PageAdjust::GetInstance().SetOffset(0, 4, 100);   // 200 + 100 > 255 → 钳到 255
    EXPECT_EQ(255, PageAdjust::GetInstance().GetAdjustedValue(0, 4));
}

TEST(PageAdjust, OutOfRangePageAndCellAreSafe) {
    Prepare();
    EXPECT_EQ(0, PageAdjust::GetInstance().GetOffset(0, 99));            // 页越界 → 0
    EXPECT_EQ(0, PageAdjust::GetInstance().GetAdjustedValue(0, 99));     // 基础值也按 0 处理
    EXPECT_EQ(0, PageAdjust::GetInstance().GetOffset(3, 0));             // cell 越界 → 0
    PageAdjust::GetInstance().SetOffset(0, 99, 5);                       // 越界写入必须被忽略
    EXPECT_EQ(0, PageAdjust::GetInstance().GetOffset(0, 99));
}

TEST(PageAdjust, ResizeKeepsExistingOffsetsAndZeroFillsNewPages) {
    Prepare(5);
    PageAdjust::GetInstance().SetOffset(0, 4, -6);
    PageAdjust::GetInstance().Resize(1, 8);              // 扩页
    EXPECT_EQ(-6, PageAdjust::GetInstance().GetOffset(0, 4));   // 已有偏移保留
    EXPECT_EQ(0, PageAdjust::GetInstance().GetOffset(0, 7));    // 新页补 0
    PageAdjust::GetInstance().Resize(1, 5);              // 缩页
    EXPECT_EQ(-6, PageAdjust::GetInstance().GetOffset(0, 4));
}

TEST(PageAdjust, ResetAllZeroesEveryPage) {
    Prepare(5);
    for (int p = 0; p < 5; p++) PageAdjust::GetInstance().SetOffset(0, p, (int8_t)(p + 1));
    PageAdjust::GetInstance().ResetAll();
    for (int p = 0; p < 5; p++) EXPECT_EQ(0, PageAdjust::GetInstance().GetOffset(0, p));
}

TEST(PageAdjust, ClearSinglePageLeavesOthersIntact) {
    Prepare(5);
    PageAdjust::GetInstance().SetOffset(0, 1, 3);
    PageAdjust::GetInstance().SetOffset(0, 2, 4);
    PageAdjust::GetInstance().SetOffset(0, 2, 0);        // 接口层「清零本页」即此调用
    EXPECT_EQ(3, PageAdjust::GetInstance().GetOffset(0, 1));
    EXPECT_EQ(0, PageAdjust::GetInstance().GetOffset(0, 2));
}
