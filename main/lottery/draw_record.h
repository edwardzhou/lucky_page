#ifndef _DRAW_RECORD_H_
#define _DRAW_RECORD_H_

#include <ctime>
#include <string>

/// 票号 + 当日流水（Task 2.3，纯逻辑；N-6：不负责取时间——epoch 由外部注入，
/// 不 include 任何 SNTP/网络头文件，单测无需网络桩）
class DrawRecord {
public:
    /// 生成本次票号（YYYYMMDD-NNNN，可带 ticket_prefix）。
    /// 内部处理跨日归零（对比注入日期与 Settings::serial_date）。
    /// ⚠️ 不落盘：调用方（LotteryController）成功抽奖后 SaveSync（D22）。
    std::string NextTicket(time_t now_epoch);

    /// 今日已开奖次数
    int TodayCount() const { return count_; }
    void SetCount(int c) { count_ = c; }

    /// CRC16-CCITT（防手改票；输入 draw_id 等审计值）
    static std::string Crc16(uint32_t value);

    /// epoch → "YYYY-MM-DD HH:MM:SS"（CST-8 固定偏移，无 DST）
    static std::string FormatTime(time_t epoch);

    /// epoch → "YYYYMMDD"（票号日期段）
    static std::string FormatDate(time_t epoch);

private:
    int count_ = 0;
};

#endif  // _DRAW_RECORD_H_
