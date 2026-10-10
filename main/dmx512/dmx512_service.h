#ifndef _DMX512_SERVICE_H_
#define _DMX512_SERVICE_H_

#include <atomic>
#include <cstdint>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "boards/esp32s3_lucky/config.h"

class DmxOutput;
class MotionEngine;
class MotionEngineSource;

/// lucky-page 的 DMX512 输出服务（单路纯输出，简化版）。
///
/// 职责（SRP）：以固定间隔从 MotionEngine 取帧 → 经 DmxOutput 发到 UART1+TTL485。
/// 已按设计裁剪原则剥离：ArtNet 输入、CH432T/SPI 输出、录制回放、多路切换、
/// License、Pause/Resume（page-player 的对应能力）。
class Dmx512Service {
public:
    explicit Dmx512Service(MotionEngine* engine);
    ~Dmx512Service();

    bool Initialize();

    /// 获取当前帧快照（Web /api/status 用；只读不加锁拷贝）
    void GetCurrentFrame(uint8_t out[512]) const;

private:
    void SendTask();
    static void SendTaskEntry(void* arg);

    MotionEngine* motion_engine_;
    std::unique_ptr<MotionEngineSource> motion_source_;
    std::unique_ptr<DmxOutput> output_;

    TaskHandle_t task_handle_ = nullptr;
    std::atomic<bool> running_{false};
    uint8_t last_buf_[512] = {};

    static constexpr int kFrameIntervalMs = 40;   // 与 MotionEngine tick 对齐
};

#endif  // _DMX512_SERVICE_H_
