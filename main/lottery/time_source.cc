#include "lottery/time_source.h"

#include <cstring>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_netif.h"
#include "settings.h"

#define TAG "TimeSource"

TimeSource* TimeSource::instance_ = nullptr;

TimeSource& TimeSource::GetInstance() {
    if (instance_ == nullptr) {
        instance_ = new TimeSource();
    }
    return *instance_;
}

void TimeSource::Init() {
    auto& settings = Settings::GetInstance();

    // 回退基准：last_known_epoch + 开机时长（同步前用）
    boot_wall_ = (time_t)settings.last_known_epoch();
    boot_uptime_us_ = esp_timer_get_time();

    if (!sntp_started_) {
        // 时间同步通知回调：**没有它 synced_ 永远不会置位**，
        // 界面会一直显示"未同步"（真机踩坑：UI 在撒谎）
        esp_sntp_set_time_sync_notification_cb([](struct timeval* tv) {
            TimeSource::GetInstance().MarkSynced((time_t)(tv ? tv->tv_sec : 0));
        });
        // lwip/netif 未初始化时 SNTP 会崩溃（测试宿主/无网环境）——幂等初始化，失败忽略
        esp_netif_init();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        // 公共 NTP 池（中国网络环境可现场换）
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "pool.ntp.org");
        // 未同步期间 60s 重试（默认 1 小时太久，现场等待体验差）
        esp_sntp_set_sync_interval(60 * 1000);
        esp_sntp_init();
        sntp_started_ = true;
        ESP_LOGI(TAG, "SNTP 已启动（ CST-8，回退基准 %lld）", (long long)boot_wall_);
    }
}

time_t TimeSource::NowEpoch() const {
    // 1) SNTP 已同步：系统时钟即为真实时间
    time_t now = time(nullptr);
    if (synced_ || (sntp_started_ && esp_sntp_getreachability(0))) {
        return now;
    }
    // 2) 未同步但系统时间明显已被 SNTP 校准过（> 2020-01-01）也视为可信
    if (now > 1577836800) {
        return now;
    }
    // 3) 回退：last_known_epoch + 开机时长
    if (boot_wall_ > 0) {
        int64_t uptime = (esp_timer_get_time() - boot_uptime_us_) / 1000000;
        return (time_t)(boot_wall_ + uptime);
    }
    // 4) 无任何来源
    return 0;
}

bool TimeSource::SetFromWeb(time_t epoch) {
    // 合理性区间：2020-01-01 ~ 2100-01-01（挡住 0/1970/乱值）
    const time_t kMin = 1577836800;
    const time_t kMax = 4102444800;
    if (epoch < kMin || epoch >= kMax) {
        ESP_LOGW(TAG, "Web 校时被拒：%lld 不在合理区间", (long long)epoch);
        return false;
    }
    struct timeval tv = {};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGE(TAG, "settimeofday 失败");
        return false;
    }
    boot_wall_ = epoch;
    boot_uptime_us_ = esp_timer_get_time();
    Settings::GetInstance().set_last_known_epoch(epoch);
    Settings::GetInstance().SaveSync();   // 掉电后仍能从该基准+开机时长推算
    synced_ = true;   // 浏览器校时同样让时间可信（否则界面显示未同步）
    ESP_LOGI(TAG, "已按 Web 时间校时：%lld", (long long)epoch);
    return true;
}

void TimeSource::KickSntp() {
    if (!sntp_started_) return;
    // 重新发起：stop + init（IDF 无独立 restart API）
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_sync_interval(synced_ ? 3600 * 1000 : 60 * 1000);
    esp_sntp_init();
    ESP_LOGI(TAG, "网络就绪，已重新发起 SNTP 校时");
}

void TimeSource::MarkSynced(time_t epoch) {
    synced_ = true;
    if (epoch > 0) {
        boot_wall_ = epoch;
        boot_uptime_us_ = esp_timer_get_time();
    }
    ESP_LOGI(TAG, "SNTP 校时成功：%lld（小票日期时间自此可信）", (long long)epoch);
}

void TimeSource::PersistNow() {
    time_t now = NowEpoch();
    if (now > 0) {
        auto& settings = Settings::GetInstance();
        settings.set_last_known_epoch((int64_t)now);
        settings.SaveSync();   // 低频路径，同步写（D22）
    }
}
