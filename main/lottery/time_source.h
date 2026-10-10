#ifndef _TIME_SOURCE_H_
#define _TIME_SOURCE_H_

#include <ctime>

/// 时间基准（Task 2.4 / R7；独立于 DrawRecord，N-6）。
/// 优先级：SNTP 已同步 > 持久化最后已知时间 + 开机时长 > 编译时间。
/// 对外只暴露 NowEpoch() / IsSynced()——业务侧（DrawRecord/小票）只消费时间，不关心来源。
class TimeSource {
public:
    static TimeSource& GetInstance();

    /// 启动 SNTP（时区固定 CST-8）；同步成功会更新 Settings::last_known_epoch
    void Init();

    /// 网络就绪后立即重新发起 SNTP 校时。
    /// 为什么必须显式踢一次：SNTP 在开机（无网络）时就初始化了，首轮请求必然失败，
    /// 而 IDF 的重试间隔默认是 **1 小时**（CONFIG_LWIP_SNTP_UPDATE_DELAY=3600000），
    /// 不踢就要等一小时才校时（真机踩坑）。
    void KickSntp();

    /// SNTP/Web 校时成功时标记（回调上下文安全：只写原子标志）
    void MarkSynced(time_t epoch);

    /// 当前时间（见类注释的优先级）。无任何可用来源时返回 0（调用方显示占位）。
    time_t NowEpoch() const;

    /// 外部校时（Web 配置页用浏览器时间校准，解决 AP-only 现场无 NTP 的问题）。
    /// 仅接受合理区间（2020-01-01 .. 2100-01-01）；成功则设置系统时钟并持久化。
    /// @return false = 时间不合理或设置失败
    bool SetFromWeb(time_t epoch);

    bool IsSynced() const { return synced_; }

    /// 持久化当前时间到 Settings（低频调用：NTP 同步回调 / 定期 / 关机前）
    void PersistNow();

private:
    TimeSource() = default;
    static TimeSource* instance_;

    bool sntp_started_ = false;
    bool synced_ = false;
    time_t boot_wall_ = 0;     // 启动时的"回退钟摆"基准（last_known_epoch + 0）
    int64_t boot_uptime_us_ = 0;
};

#endif  // _TIME_SOURCE_H_
