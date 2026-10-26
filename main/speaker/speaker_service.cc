// speaker_service.cc — 欢迎音喇叭服务实现（I2S 新驱动 esp_driver_i2s，2026-10-25）
#include "speaker/speaker_service.h"

#include <cstring>
#include "esp_heap_caps.h"
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
// ESP-IDF v6：I2S 从老 driver 拆分为独立组件 esp_driver_i2s（头文件路径仍为 driver/*）
#include "driver/i2s_std.h"

// 板级引脚（boards/esp32s3_lucky/config.h，含冲突 static_assert 护栏）
#include "boards/esp32s3_lucky/config.h"

// EMBED_FILES 产物符号（main/assets/welcome.wav）
extern "C" const uint8_t welcome_wav_start[] asm("_binary_welcome_wav_start");
extern "C" const uint8_t welcome_wav_end[] asm("_binary_welcome_wav_end");

#include "speaker/wav_data.h"
#include "speaker/win_sound.h"
#include "speaker/sound_store.h"

#define TAG "SpeakerSvc"

// 播放任务优先级：12 —— 需高于常规任务（console=3），否则播放中被长抢占
// → DMA 欠载重播残段（真机"电音"事故 2026-10-25）；仍低于 WiFi(23) 等系统任务
static constexpr int      kPlayTaskPrio  = 12;
static constexpr uint32_t kPlayTaskStack = 4096;
// 每块写入量：320 样本 = 20ms@16kHz，DMA 之外最多缓存这么多，避免长阻塞
static constexpr size_t   kChunkBytes    = 640;
// 软件播放音量（百分比，平方律，运行时可调，定义见 SetVolume）：MAX98357A 增益 12dB
// （用户模块 GAIN 焊 GND），素材峰值 -1.2dB 近满幅，不衰减会削顶爆破（真机事故 2026-10-25）。
// 默认 70% 平方律 ≈ -6.2dB，对齐小智 audio_codec.h 默认 output_volume_=70。


namespace speaker {

// 播放请求（值类型经队列投递给常驻播放任务；串行播放，互不打断）
struct PlayRequest {
    enum class Mode { kWav, kTone, kChime, kLose, kCustom };
    const uint8_t* pcm = nullptr;   // kWav：16bit PCM（welcome.wav data 区，固件嵌入数据指针稳定）
    size_t pcm_bytes = 0;
    uint32_t tone_hz = 0;           // kTone：正弦频率
    uint32_t tone_ms = 0;           // kTone：正弦时长
    int volume_pct = 100;           // 入队时的音量快照（1..100）
    int role = -1;                  // kCustom：sound_store 角色（welcome/win/lose）
    Mode mode = Mode::kWav;
};
// 队列深度：开机欢迎音 + 连续中奖的余量；满了丢弃新请求（保住"正在播放不被打断"）
static constexpr int kPlayQueueDepth = 4;

SpeakerService& SpeakerService::GetInstance() {
    static SpeakerService inst;
    return inst;
}

void SpeakerService::SetVolume(int percent) {
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    volume_percent_ = percent;
    ESP_LOGI(TAG, "播放音量设为 %d%%（平方律 %.2f）", percent,
             (double)percent * percent / 10000.0);
}

bool SpeakerService::Init() {
    if (ready_) return true;
    if (init_failed_) return false;   // 之前失败过，不再反复试探（规则 7：只降级）
    // 并发护栏：开机欢迎音（主任务）与串口 sndplay（console 任务）可能同时
    // 首次触发 Init——双通道双 worker 会同时驱动同一组引脚 = 真重叠。
    // ⚠ 临界区只做"谁去初始化"的标志翻转，重活（I2S 创建/日志）必须在外面：
    //   曾把 EnsureChannel 整个塞进临界区，其 ESP_LOGI 拿 stdio 锁 → 临界区内
    //   阻塞 → 启动即 abort 循环（真机事故，backtrace: lock_acquire_recursive）
    static portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
    static bool initializing = false;
    bool do_init = false;
    portENTER_CRITICAL(&init_mux);
    if (!ready_ && !init_failed_ && !initializing) {
        initializing = true;
        do_init = true;
    }
    portEXIT_CRITICAL(&init_mux);
    if (!do_init) return false;   // 别的任务正在初始化：本轮放弃（下次调用即 ready）
    bool ok = EnsureChannel();
    portENTER_CRITICAL(&init_mux);
    initializing = false;
    portEXIT_CRITICAL(&init_mux);
    return ok;
}

bool SpeakerService::EnsureChannel() {
    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = I2S_NUM_AUTO;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;   // 对齐小智 AUDIO_CODEC_DMA_FRAME_NUM
    // 双向自动清零：欠载时 DMA 无数据可发则发零——否则可能重播描述符里
    // 上一次的残留数据碎片（真机表现为素材尾部静音区出现"电音"，2026-10-25）
    chan_cfg.auto_clear = true;          // = auto_clear_after_cb
    chan_cfg.auto_clear_before_cb = true;
    i2s_chan_handle_t tx = nullptr;
    if (i2s_new_channel(&chan_cfg, &tx, nullptr) != ESP_OK || tx == nullptr) {
        ESP_LOGW(TAG, "I2S 通道申请失败 → 喇叭不可用（不影响其他功能）");
        init_failed_ = true;
        return false;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE);
    // 格式对齐模块官方配套例程（~/Downloads/MAX98357_TEST/esp32_MusicPlayer-master：
    // 老驱动 16bit + I2S_CHANNEL_FMT_RIGHT_LEFT + Philips——该代码在此模块上验证正常）。
    // 新驱动等效写法：16bit 槽 / 立体声 / 双槽全发 / Philips；BCLK=fs×16×2。
    // （曾试 16bit-mono 与 32bit-mono 均沙哑失真，20% 音量仍失真 → 与幅度无关，
    //   差异在槽结构：此模块要求两槽都有数据，SD=VCC 的 (L+R)/2 输出模式下
    //   mono 单槽 → 半幅且实测失真。真机记录 2026-10-25。）
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_STEREO);
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;   // MAX98357A 不需要 MCLK
    std_cfg.gpio_cfg.bclk = SPEAKER_I2S_BCLK_PIN;
    std_cfg.gpio_cfg.ws = SPEAKER_I2S_LRCK_PIN;
    std_cfg.gpio_cfg.dout = SPEAKER_I2S_DOUT_PIN;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    esp_err_t err = i2s_channel_init_std_mode(tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S 初始化失败(%s) → 喇叭不可用", esp_err_to_name(err));
        i2s_del_channel(tx);
        init_failed_ = true;
        return false;
    }
    // 注意：此处不 enable——通道由 PlayOne 按需使能/播完禁用（静默期时钟停止）
    // 常驻播放任务 + 队列（串行播放模型，2026-10-25）：任何失败只降级
    play_queue_ = xQueueCreate(kPlayQueueDepth, sizeof(PlayRequest));
    if (play_queue_ == nullptr ||
        xTaskCreate(PlayWorkerEntry, "spk_play", kPlayTaskStack, this, kPlayTaskPrio,
                    nullptr) != pdPASS) {
        ESP_LOGW(TAG, "播放队列/任务创建失败 → 喇叭不可用");
        if (play_queue_) {
            vQueueDelete(play_queue_);
            play_queue_ = nullptr;
        }
        i2s_del_channel(tx);
        init_failed_ = true;
        return false;
    }
    tx_ = tx;
    ready_ = true;
    ESP_LOGI(TAG, "喇叭就绪：I2S BCLK=G%d LRCK=G%d DOUT=G%d（24kHz/16bit/立体声双槽，串行播放）",
             SPEAKER_I2S_BCLK_PIN, SPEAKER_I2S_LRCK_PIN, SPEAKER_I2S_DOUT_PIN);
    return true;
}

bool SpeakerService::Enqueue(const PlayRequest& req) {
    if (play_queue_ == nullptr) return false;
    if (xQueueSend(play_queue_, &req, 0) != pdTRUE) {
        // 队满：丢新保旧——正在播放/排队的音效绝不被打断（用户要求 2026-10-25）
        ESP_LOGW(TAG, "播放队列已满（%d），丢弃本次请求", kPlayQueueDepth);
        return false;
    }
    return true;
}

bool SpeakerService::PlayWelcome() {
    if (!Init()) return false;
    // 自定义欢迎音优先（2026-10-26）
    if (sound_store::ReadInfo(sound_store::kWelcome).custom) {
        PlayRequest req;
        req.mode = PlayRequest::Mode::kCustom;
        req.role = sound_store::kWelcome;
        req.volume_pct = volume_percent_;
        return Enqueue(req);
    }

    // 素材自检（嵌入数据损坏只降级，不进播放任务）
    size_t wav_len = (size_t)(welcome_wav_end - welcome_wav_start);
    wav::WavInfo info = wav::ParseWav(welcome_wav_start, wav_len);
    if (!info.ok || info.sample_rate != SPEAKER_SAMPLE_RATE) {
        ESP_LOGW(TAG, "welcome.wav 校验失败（ok=%d rate=%u）→ 跳过播放",
                 (int)info.ok, info.sample_rate);
        return false;
    }

    PlayRequest req;
    req.mode = PlayRequest::Mode::kWav;
    req.pcm = welcome_wav_start + info.data_offset;
    req.pcm_bytes = info.data_size;
    req.volume_pct = volume_percent_;
    return Enqueue(req);
}

bool SpeakerService::PlayTone(uint32_t freq_hz, uint32_t duration_ms) {
    if (!Init()) return false;
    if (freq_hz == 0 || freq_hz >= SPEAKER_SAMPLE_RATE / 2) {
        ESP_LOGW(TAG, "测试音频率须在 1..%u Hz", SPEAKER_SAMPLE_RATE / 2 - 1);
        return false;
    }
    PlayRequest req;
    req.mode = PlayRequest::Mode::kTone;
    req.tone_hz = freq_hz;
    req.tone_ms = duration_ms;
    req.volume_pct = volume_percent_;
    return Enqueue(req);
}

bool SpeakerService::PlayChime() {
    if (!Init()) return false;
    if (sound_store::ReadInfo(sound_store::kWin).custom) {
        PlayRequest req;
        req.mode = PlayRequest::Mode::kCustom;
        req.role = sound_store::kWin;
        req.volume_pct = volume_percent_;
        return Enqueue(req);
    }
    // 合成在播放任务里做（≈47KB@24kHz，绝不放栈上，规则 §四-5）
    PlayRequest req;
    req.mode = PlayRequest::Mode::kChime;
    req.volume_pct = volume_percent_;
    return Enqueue(req);
}

bool SpeakerService::PlayLose() {
    if (!Init()) return false;
    if (sound_store::ReadInfo(sound_store::kLose).custom) {
        PlayRequest req;
        req.mode = PlayRequest::Mode::kCustom;
        req.role = sound_store::kLose;
        req.volume_pct = volume_percent_;
        return Enqueue(req);
    }
    PlayRequest req;
    req.mode = PlayRequest::Mode::kLose;
    req.volume_pct = volume_percent_;
    return Enqueue(req);
}

void SpeakerService::PlayWorkerEntry(void* arg) {
    SpeakerService* self = static_cast<SpeakerService*>(arg);
    // 常驻串行播放：一次取一个请求，播完才取下一个（正在播放的绝不被打断）
    while (true) {
        PlayRequest a;
        if (xQueueReceive(self->play_queue_, &a, portMAX_DELAY) != pdTRUE) continue;
        self->PlayOne(a);
    }
}

void SpeakerService::PlayOne(const PlayRequest& a) {
    // 播放前使能通道（结尾会禁用；见下）
    (void)i2s_channel_enable(tx_);

    // 正弦/中奖音模式：软件生成（幅度即素材峰值量级）
    std::vector<int16_t> tone;
    const int16_t* src = nullptr;
    if (a.mode == PlayRequest::Mode::kCustom) {
        static uint8_t* snd = nullptr;
        if (snd == nullptr) snd = (uint8_t*)heap_caps_malloc(sound_store::kSlotBytes,
                                                             MALLOC_CAP_SPIRAM);
        size_t got = 0;
        if (snd != nullptr &&
            sound_store::Load(a.role, snd, sound_store::kSlotBytes, &got) && got >= 2) {
            tone.resize(got / 2);
            memcpy(tone.data(), snd, got);
            src = tone.data();
        } else {
            ESP_LOGW(TAG, "自定义音效读取失败（role=%d）→ 本次跳过", a.role);
        }
    } else if (a.mode == PlayRequest::Mode::kChime) {
        tone = SynthWinChime(SPEAKER_SAMPLE_RATE);
        src = tone.data();
    } else if (a.mode == PlayRequest::Mode::kLose) {
        tone = SynthLoseChime(SPEAKER_SAMPLE_RATE);
        src = tone.data();
    } else if (a.mode == PlayRequest::Mode::kTone) {
        size_t samples = (size_t)a.tone_ms * SPEAKER_SAMPLE_RATE / 1000;
        tone.resize(samples);
        // 整数相位累积：phase 单位 = 1/65536 圈，每样本步进 freq*65536/fs
        uint32_t step = (uint32_t)((uint64_t)a.tone_hz * 65536 / SPEAKER_SAMPLE_RATE);
        uint32_t phase = 0;
        for (size_t i = 0; i < samples; ++i) {
            // 2π 相位 → 正弦查表（256 点，线性够诊断用）；幅度 0.9 满幅
            unsigned idx = (phase >> 8) & 0xFF;
            static const int32_t kSinTable[256] = {
                0,   1769,  3535,  5297,  7053,  8801, 10540, 12266, 13977, 15669,
                17338, 18982, 20598, 22183, 23735, 25251, 26728, 28164, 29557, 30904,
                32203, 33452, 34648, 35789, 36874, 37900, 38865, 39768, 40606, 41377,
                42079, 42711, 43270, 43756, 44167, 44502, 44760, 44940, 45042, 45063,
                45005, 44866, 44646, 44345, 43962, 43499, 42955, 42330, 41624, 40838,
                39972, 39026, 38000, 36896, 35713, 34452, 33114, 31700, 30211, 28648,
                27012, 25304, 23525, 21676, 19759, 17775, 15725, 13612, 11436, 9200,
                6906, 4557, 2174,  -2174, -4557, -6906, -9200, -11436, -13612, -15725,
                -17775, -19759, -21676, -23525, -25304, -27012, -28648, -30211, -31700,
                -33114, -34452, -35713, -36896, -38000, -39026, -39972, -40838, -41624,
                -42330, -42955, -43499, -43962, -44345, -44646, -44866, -45005, -45063,
                -45042, -44940, -44760, -44502, -44167, -43756, -43270, -42711, -42079,
                -41377, -40606, -39768, -38865, -37900, -36874, -35789, -34648, -33452,
                -32203, -30904, -29557, -28164, -26728, -25251, -23735, -22183, -20598,
                -18982, -17338, -15669, -13977, -12266, -10540, -8801, -7053, -5297,
                -3535, -1769, 0, 1769, 3535, 5297, 7053, 8801, 10540, 12266, 13977,
                15669, 17338, 18982, 20598, 22183, 23735, 25251, 26728, 28164, 29557,
                30904, 32203, 33452, 34648, 35789, 36874, 37900, 38865, 39768, 40606,
                41377, 42079, 42711, 43270, 43756, 44167, 44502, 44760, 44940, 45042,
                45063, 45005, 44866, 44646, 44345, 43962, 43499, 42955, 42330, 41624,
                40838, 39972, 39026, 38000, 36896, 35713, 34452, 33114, 31700, 30211,
                28648, 27012, 25304, 23525, 21676, 19759, 17775, 15725, 13612, 11436,
                9200, 6906, 4557, 2174,
            };
            // 末尾 10ms（240 样本）线性淡出：整毫秒截断点相位任意，
            // 不淡出会以最高 ~90% 峰值突然归零 → 结尾"啪"爆音（真机反馈 2026-10-25）
            int32_t amp = (int32_t)kSinTable[idx] * 9 / 10;
            if (samples - i <= SPEAKER_SAMPLE_RATE / 100) {
                amp = (int32_t)((int64_t)amp * (int64_t)(samples - i) /
                                (SPEAKER_SAMPLE_RATE / 100));
            }
            tone[i] = (int16_t)amp;
            phase += step;
        }
        src = tone.data();
    }

    // mono 16bit 源 → 立体声交错 16bit（L=R=样本），DMA 每帧 2×16bit=4 字节。
    // 缓冲 320 帧 = 640 个 int16（≈13ms@24kHz），DMA 满时由 100ms 超时让出 CPU
    constexpr size_t kFrames = kChunkBytes / 2;   // 每块源样本数（16bit 计）
    static_assert(kFrames > 0, "chunk 至少 1 个样本");
    int16_t* buf = new int16_t[kFrames * 2];

    const int16_t* pcm_src = src ? src : reinterpret_cast<const int16_t*>(a.pcm);
    size_t pcm_total = src ? tone.size() * 2 : a.pcm_bytes;

    // 平方律音量（int16 域内缩放，右移 16 收尾）
    const int32_t vol_q16 = (int32_t)a.volume_pct * a.volume_pct * 65536 / 10000;

    size_t off = 0;
    while (off < pcm_total) {
        size_t samples = (pcm_total - off) / 2;
        if (samples > kFrames) samples = kFrames;
        if (samples == 0) break;
        for (size_t i = 0; i < samples; ++i) {
            int16_t v = (int16_t)(((int32_t)pcm_src[off / 2 + i] * vol_q16) >> 16);
            buf[i * 2] = v;      // 左槽
            buf[i * 2 + 1] = v;  // 右槽（L=R → (L+R)/2 = 原样本）
        }
        size_t written = 0;
        if (i2s_channel_write(tx_, buf, samples * 2 * sizeof(int16_t), &written,
                              pdMS_TO_TICKS(100)) != ESP_OK) {
            break;   // 通道异常（如正在被回收）→ 直接结束，不刷日志噪音
        }
        off += written / sizeof(int16_t);   // 源是 mono：每 2 个输出 int16 = 1 个源
    }
    delete[] buf;
    // ===== 段间隔断（2026-10-25 第六轮：gap 加长到 ~560ms）=====
    // ① 写 60ms 静音帧（= DMA 全缓冲深度 6×240 帧，把旧数据完整冲刷出去）
    // ② 等 500ms：排空（60ms）+ 功放/喇叭瞬态彻底衰减 + 静默期时钟已停
    //    （播放结束即 disable 通道），下一段才开始
    {
        constexpr size_t kSilenceFrames = 1440;          // 60ms × 24kHz
        static int16_t silence[kSilenceFrames * 2];      // 立体声双槽，零初始化
        size_t written = 0;
        (void)i2s_channel_write(tx_, silence, sizeof(silence), &written,
                                pdMS_TO_TICKS(200));
        vTaskDelay(pdMS_TO_TICKS(500));   // 排空 + 静默（用户要求加长，2026-10-25）
    }
    // ===== 禁用通道：静默期 BCLK/LRCK 时钟完全停止 =====
    // 真机"每段末尾电音"的真根因（2026-10-25 第六轮）：I2S 时钟即使数据全零也
    // 持续开关，经杜邦线串扰进 MAX98357A 模拟输出 = 段尾"滋滋"电音。禁用通道
    // 时钟即停，串扰源消失；下段播放前再使能（通道配置保持不变）。
    (void)i2s_channel_disable(tx_);
    switch (a.mode) {
    case PlayRequest::Mode::kWav:
        ESP_LOGI(TAG, "欢迎音播放完成（%zu 字节 PCM）", a.pcm_bytes);
        break;
    case PlayRequest::Mode::kTone:
        ESP_LOGI(TAG, "正弦测试音完成（%uHz %ums）", (unsigned)a.tone_hz,
                 (unsigned)a.tone_ms);
        break;
    case PlayRequest::Mode::kChime:
        ESP_LOGI(TAG, "中奖音效播放完成");
        break;
    case PlayRequest::Mode::kLose:
        ESP_LOGI(TAG, "落空音效播放完成");
        break;
    case PlayRequest::Mode::kCustom:
        ESP_LOGI(TAG, "自定义音效播放完成（role=%d，%zu 字节 PCM）", a.role, pcm_total);
        break;
    }
}

}  // namespace speaker
