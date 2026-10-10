#ifndef _NVS_SAVER_H_
#define _NVS_SAVER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

/// NVS 异步保存器：通过 FreeRTOS Queue 接收保存信号，
/// 在独立低优先级任务中批量写入 NVS，避免阻塞调用者。
///
/// 设计要点：
/// - 纯信号队列（无数据负载），调用者只发"有东西要保存"的通知
/// - 消费端 xQueueReset() 丢弃重复信号，只保存最新内存数据
/// - 批量写入：单次 open/commit/close，14 个字段一次提交
class NvsSaver {
public:
    static NvsSaver& GetInstance();

    /// 初始化：创建 FreeRTOS Queue 和保存任务。在 Settings::Load() 之后调用。
    void Initialize();

    /// 请求保存（非阻塞，线程安全，任意线程可调用）
    void RequestSave();

    /// 同步保存（阻塞等待完成，供关机前使用）
    void SaveSync();

    /// 停止保存任务
    void Stop();

    NvsSaver(const NvsSaver&) = delete;
    NvsSaver& operator=(const NvsSaver&) = delete;

private:
    NvsSaver() = default;

    /// FreeRTOS 任务入口
    static void TaskEntry(void* arg);

    /// 实际执行批量 NVS 写入
    void DoSave();

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    static constexpr int kQueueDepth = 2;
    static constexpr int kStackSize = 4096;
};

#endif  // _NVS_SAVER_H_
