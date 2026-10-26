// speaker_service.h — 喇叭服务（I2S → MAX98357A，2026-10-25）
// 唯一的喇叭硬件访问点（规则 5 分层：main/ 业务只调本接口，不碰 I2S）。
// 播放模型：常驻播放任务 + 队列（串行播放）——新请求排队等当前播完，
// 绝不打断正在播放的音效；队列满则丢弃新请求并打日志（降级，规则 7）。
#ifndef LUCKY_PAGE_SPEAKER_SPEAKER_SERVICE_H_
#define LUCKY_PAGE_SPEAKER_SPEAKER_SERVICE_H_

#include <cstdint>

// 前置声明（与 esp_driver_i2s 的 i2s_types.h 定义完全一致，避免头文件污染）
typedef struct i2s_channel_obj_t* i2s_chan_handle_t;
typedef struct QueueDefinition* QueueHandle_t;

namespace speaker {

// 音效先行时长（供控制器"音效播完才打印"的时序用）：
// 最长音效(中奖 970ms) + 段间隔断(60ms 静音 + 500ms 静默) + 裕量
constexpr int kSoundLeadMs = 1600;

class SpeakerService {
public:
    static SpeakerService& GetInstance();

    // 懒初始化 I2S TX 通道 + 播放队列/常驻任务（首次播放时自动完成）。
    // 返回 false 仅代表"本次没初始化成功"（已初始化/失败都不算错误路径）。
    bool Init();

    // 以下三个播放接口均为"入队即返回"：串行等待播放，不打断当前音效。
    // 返回 false = 服务不可用或队列已满（已打日志，不影响业务流程）。

    // 内置欢迎音（main/assets/welcome.wav，EMBED 进固件）
    bool PlayWelcome();

    // 软件生成纯正弦（诊断用：分离"素材问题"与"硬件/链路问题"）
    bool PlayTone(uint32_t freq_hz, uint32_t duration_ms);

    // 中奖音效（win_sound 合成；抽奖控制器在中奖揭晓时经回调触发）
    bool PlayChime();

    // 落空音效（下行双音，2026-10-25；控制器 is_lose 分支触发）
    bool PlayLose();

    // 运行时音量（1..100，平方律；sndvol 现场调，用于"幅度对照"诊断失真来源：
    // 低音量仍失真 → 数字链路；低音量干净 → 模拟侧过载）
    void SetVolume(int percent);
    int volume_percent() const { return volume_percent_; }

    // 是否已完成初始化（诊断/串口命令用）
    bool ready() const { return ready_; }

private:
    SpeakerService() = default;
    bool EnsureChannel();              // 建 I2S 通道 + 队列 + 常驻播放任务
    static void PlayWorkerEntry(void* arg);
    void PlayOne(const struct PlayRequest& a);   // 播一个请求（worker 内调用）
    bool Enqueue(const struct PlayRequest& req);

    int volume_percent_ = 100;     // 用户要求最大音量（2026-10-25；若再出现破音=功放削顶，串口 sndvol 回调）
    i2s_chan_handle_t tx_ = nullptr;
    QueueHandle_t play_queue_ = nullptr;
    bool init_failed_ = false;   // 记住失败，避免每次调用都重复试探 I2S
    bool ready_ = false;
};

}  // namespace speaker

#endif  // LUCKY_PAGE_SPEAKER_SPEAKER_SERVICE_H_
