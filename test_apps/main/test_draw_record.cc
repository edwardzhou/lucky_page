// test_draw_record.cc — 票号 + 当日流水 + CRC16（Task 2.3，TDD；epoch 注入）
#include "lucky_test.h"
#include <string>
#include "lottery/draw_record.h"
#include "lottery/time_source.h"
#include "settings.h"

// 2026-09-29 14:32:07 CST（UTC+8）= 1790663527（python 实算，勿手算）
static constexpr time_t kT0929 = 1790663527;

TEST(DrawRecord, FormatTimeCst) {
    // 0 → 占位
    EXPECT_STREQ("----", DrawRecord::FormatTime(0).c_str());
    // 1790663527 = 2026-09-29 06:32:07 UTC = 14:32:07 CST（python 实算）
    EXPECT_STREQ("2026-09-29 14:32:07", DrawRecord::FormatTime(1790663527).c_str());
}

TEST(DrawRecord, FormatDateCst) {
    EXPECT_STREQ("20260929", DrawRecord::FormatDate(1790663527).c_str());
}

TEST(DrawRecord, SameDayIncrements) {
    DrawRecord dr;
    // 首次调用即触发跨日归零（注入日期与 NVS 存储的不同）→ 0001 起
    EXPECT_STREQ("20261006-0001", dr.NextTicket(kT0929 + 86400 * 7).c_str());
    EXPECT_STREQ("20261006-0002", dr.NextTicket(kT0929 + 86400 * 7 + 60).c_str());
    EXPECT_STREQ("20261006-0003", dr.NextTicket(kT0929 + 86400 * 7 + 3600).c_str());
    EXPECT_EQ(3, dr.TodayCount());
}

TEST(DrawRecord, CrossDayResets) {
    DrawRecord dr;
    dr.NextTicket(kT0929 + 86400 * 14);                  // 10-13 第 1 张
    dr.NextTicket(kT0929 + 86400 * 14 + 7200);           // 10-13 第 2 张
    std::string t = dr.NextTicket(kT0929 + 86400 * 15);  // 次日 10-14 → 归零 0001
    EXPECT_STREQ("20261014-0001", t.c_str());
}

TEST(DrawRecord, TimeBackwardDoesNotDuplicate) {
    // NTP 把时间校回昨天：不重复流水、不破坏当日计数
    DrawRecord dr;
    dr.NextTicket(kT0929 + 86400 * 21);              // 10-20 0001
    dr.NextTicket(kT0929 + 86400 * 21 + 60);         // 10-20 0002
    // 校回 10-19（新日期字符串）→ 归零重开 0001（不重复 10-20 的流水）
    std::string t = dr.NextTicket(kT0929 + 86400 * 20);
    EXPECT_STREQ("20261019-0001", t.c_str());
}

TEST(DrawRecord, TicketPrefixApplied) {
    auto& s = Settings::GetInstance();
    std::string saved = s.ticket_prefix();
    s.set_ticket_prefix("SZ");
    DrawRecord dr;
    EXPECT_STREQ("SZ20261027-0001", dr.NextTicket(kT0929 + 86400 * 28).c_str());
    s.set_ticket_prefix(saved);
}

TEST(DrawRecord, Crc16StableAndFourHexDigits) {
    // 同输入必同输出（4 位大写十六进制）
    EXPECT_STREQ(DrawRecord::Crc16(128).c_str(), DrawRecord::Crc16(128).c_str());
    EXPECT_EQ(4u, DrawRecord::Crc16(128).size());
    // 不同输入不同 CRC（抽查）
    EXPECT_TRUE(DrawRecord::Crc16(1) != DrawRecord::Crc16(2));
}

// ============================================================================
// TimeSource（Task 2.4：SNTP > NVS 回退 > 无来源；无网络环境下测回退路径）
// ============================================================================
TEST(TimeSource, NowEpochNeverGarbage) {
    // 分支无关的不变量：系统钟可信时返回系统钟；否则回退 last_known+uptime。
    // 两者都保证 now ∈ [last_known, 2100年]，绝不返回 0/垃圾值。
    auto& s = Settings::GetInstance();
    int64_t saved = s.last_known_epoch();
    s.set_last_known_epoch(1789965127);
    TimeSource::GetInstance().Init();   // 幂等；SNTP 无网静默失败
    time_t now = TimeSource::GetInstance().NowEpoch();
    EXPECT_TRUE(now >= 1789965127);                       // ≥ 持久化基准（两分支都满足）
    EXPECT_TRUE(now < 4102444800);                        // < 2100-01-01（非垃圾）
    s.set_last_known_epoch(saved);
    TimeSource::GetInstance().PersistNow();               // 持久化路径不崩溃
}
