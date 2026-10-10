#ifndef _PRINTER_SERVICE_H_
#define _PRINTER_SERVICE_H_

#include <atomic>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "printer/receipt_params.h"
#include "printer/receipt_template.h"

/// 打印机服务：深度 1 的打印队列 + 独立任务阻塞发送（设计 D4 单飞 / §6）。
///
/// - Enqueue 非阻塞；已有待打任务（上一张未打完）返回 false
/// - 打印任务内渲染（ReceiptLayout::Render）→ 插入 logo（若启用且有效）→ 发送
/// - 失败自动重试 2 次（间隔 300ms）；连续失败 ≥3 置 printer_fault（R8 红色状态灯）
/// - 完成回调在 Application 主线程上下文执行（由 Application 注入 Schedule）
class PrinterService {
public:
    static constexpr int kQueueDepth = 1;   // 单飞（D4）

    bool Init(int tx_pin, int rx_pin, int baud);

    /// 非阻塞入队（拷贝模板与数据）。返回 false = 上一张还没打完。
    bool Enqueue(const ReceiptTemplateV3& tmpl, const ReceiptParams& params);

    /// 打一张测试票（与真实开奖同一条渲染路径，R8.2）
    bool EnqueueTestTicket();

    /// 注册完成回调（在 Application::Schedule 注入的主线程上下文执行）
    void SetDoneCallback(std::function<void(bool ok)> cb) { done_cb_ = std::move(cb); }

    /// 是否缺纸（出票前探测 + 打印后复探的结果）
    bool paper_out() const { return paper_out_.load(); }
    /// 因缺纸被跳过的张数
    int skipped_count() const { return skipped_count_.load(); }

    void GetStats(int* ok, int* fail) const {
        if (ok) *ok = ok_count_;
        if (fail) *fail = fail_count_;
    }
    /// 测试专用：只初始化投递层（互斥锁），不碰 UART、不建打印任务
    bool InitDeliveryForTest();

    /// 测试专用：取出待打任务（验证跨线程投递不丢内容 / 长串不悬空）
    bool TakePendingForTest(ReceiptTemplateV3* tmpl, ReceiptParams* params);

    bool IsReady() const { return ready_.load(); }
    bool Fault() const { return printer_fault_.load(); }
    void ClearFault() { printer_fault_ = false; }

private:
    struct PrintJob {
        ReceiptTemplateV3 tmpl;
        ReceiptParams params;
    };

    void PrintTask();
    static void TaskEntry(void* arg);
    bool SendOne(const PrintJob& job);

    // ⚠️ 不能用 FreeRTOS 队列传 PrintJob：xQueueSend 按字节 memcpy，
    // 而 ReceiptTemplate/ReceiptData 含 std::string —— 长串（>SSO 15B）会有堆指针，
    // 临时 job 析构后队列副本即悬空 → 打出乱码（真机踩坑）。
    // 改为「互斥锁 + 任务通知 + 服务自身持有任务」：只传所有权，不做字节拷贝。
    PrintJob pending_;
    bool has_pending_ = false;
    SemaphoreHandle_t mutex_ = nullptr;      // 保护 pending_
    TaskHandle_t task_ = nullptr;
    static constexpr uint32_t kNotifyNewJob = 1;
    std::atomic<bool> ready_{false};
    std::atomic<bool> printer_fault_{false};
    std::atomic<bool> paper_out_{false};   // 缺纸（出票前探测 + 打印后复探）
    std::atomic<int> skipped_count_{0};    // 因缺纸跳过的张数
    std::atomic<int> ok_count_{0};
    std::atomic<int> fail_count_{0};
    std::function<void(bool)> done_cb_;
};

#endif  // _PRINTER_SERVICE_H_
