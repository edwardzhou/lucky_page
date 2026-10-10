#ifndef _MOTION_ENGINE_H_
#define _MOTION_ENGINE_H_

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <memory>

#include "motion/action.h"
#include "motion/pipeline.h"

/// DMX512 动作引擎核心：以固定 tick 间隔驱动 Pipeline 执行。
///
/// 对应 Elixir ActionManager。引擎在独立的 FreeRTOS 任务中运行，
/// 每 40ms 执行一次 Pipeline::Run()，将输出的 CellsMap 维护在内部状态中，
/// 并转换为 DMX512 原始 buffer 供 Dmx512Service 直接发送。
///
/// 引擎由 Start/Stop 显式控制生命周期，Pipeline 完成后引擎继续空跑。
///
/// 线程安全：
/// - GetCellsMap()、GetDmxData() 和 SpawnAction() 使用 mutex 保护
/// - running_ 使用 std::atomic，无需加锁
/// - Start/Stop 会等待任务结束，调用方不需要额外同步
class MotionEngine {
public:
    /// tick 间隔 40ms，每秒 25 帧。
    static constexpr int kTickIntervalMs = 40;

    /// DMX512 通道总数。
    static constexpr int kDmxChannelCount = 512;

    /// @param cell_count 预创建的 cell 数量（cell_id 1..cell_count）
    /// @param paging_channel_count 每台翻页机占用的 DMX 通道数（3 或 7）
    explicit MotionEngine(int cell_count, int paging_channel_count = 7);
    ~MotionEngine();

    /// 设置串行队列完成回调。当 schedule 从有动作变为空闲时触发一次。
    void SetScheduleDoneCallback(std::function<void()> callback);

    /// 启动引擎。pipeline 非空时替换默认 pipeline，为空时使用内置 pipeline。
    /// 不清空 cells_map，保留已有状态。
    void Start(std::unique_ptr<Pipeline> pipeline = nullptr);

    /// 停止引擎，等待 FreeRTOS 任务结束。保留 cells_map 和 dmx_data_。
    void Stop();

    /// 热更新引擎参数（cell_count、paging_channel_count），不清空 Pipeline。
    /// 线程安全：在 mutex 内重建 cells_map，不影响运行中的 Tick。
    void Reconfigure(int cell_count, int paging_channel_count);

    /// 查询引擎是否正在运行。
    bool IsRunning() const;

    /// 获取当前 CellsMap 快照（加锁拷贝，线程安全）。
    CellsMap GetCellsMap() const;

    /// 获取 DMX512 原始 buffer（加锁拷贝到目标 buffer，线程安全）。
    /// @param out 目标 buffer，至少 kDmxChannelCount 字节
    void GetDmxData(uint8_t* out) const;

    /// 获取 DMX512 原始 buffer 的可写指针（仅限 MotionEngine 任务内部使用）。
    uint8_t* GetDmxDataBuffer() { return dmx_data_; }

    /// 设置回放活跃状态。回放时跳过 UpdateDmxData()，由 ActionReplay 管理。
    void SetReplayActive(bool active) { replay_active_ = active; }

    /// 强制从 cells_map_ 刷新 dmx_data_（线程安全）。
    /// 用于回放停止后立即恢复 DMX 输出，同时防御性清除 replay_active_ 标志。
    void ForceUpdateDmxData();

    /// 向当前 Pipeline 的串行队列追加动作（线程安全）。
    void AddActions(std::vector<std::unique_ptr<Action>> actions);

    /// 替换当前 Pipeline 的串行队列（线程安全），丢弃未执行的动作。
    void Reschedule(std::vector<std::unique_ptr<Action>> actions);

    /// 向当前 Pipeline 的并行组添加动作（用于 ActionSpawn 派发）。
    void SpawnAction(std::unique_ptr<Action> action);

private:
    /// 执行一次 Pipeline tick（在 FreeRTOS 任务中调用）。
    void Tick();

    /// 根据 cells_map_ 填充 dmx_data_ buffer。
    void UpdateDmxData();

    /// FreeRTOS 任务入口函数。
    static void TaskEntry(void* arg);

    int cell_count_;                                 // cell 数量
    int paging_channel_count_;                       // 每台占用的 DMX 通道数（3 或 7）
    std::unique_ptr<Pipeline> pipeline_;             // 当前 pipeline（构造时默认创建）
    CellsMap cells_map_;                             // 当前 DMX 通道状态
    uint8_t dmx_data_[kDmxChannelCount] = {};        // DMX512 原始 buffer
    mutable SemaphoreHandle_t mutex_;                 // 保护内部状态
    TaskHandle_t task_handle_ = nullptr;              // FreeRTOS 任务句柄
    std::atomic<bool> running_{false};                // 引擎运行标志
    bool replay_active_ = false;                        // 回放活跃标志
    std::function<void()> schedule_done_callback_;          // 串行队列完成回调
    bool was_schedule_done_ = false;                    // 上一帧 schedule 是否已完成
};

#endif  // _MOTION_ENGINE_H_
