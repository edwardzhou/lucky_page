#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <deque>
#include <functional>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "boards/common/button.h"
#include "button_gate.h"
#include "reset_gesture.h"
#include "dmx512/dmx512_service.h"
#include "lottery/lottery_controller.h"
#include "lottery/prize_pool.h"
#include "lottery/time_source.h"
#include "motion/motion_engine.h"
#include "printer/printer_service.h"

/// lucky-page 应用主类（单例调度中心，裁剪自 page-player）。
///
/// 主线程模型：Application 拥有主循环任务；Schedule() 把任意线程的闭包
/// 投递到主线程执行（MotionEngine 回调 / HTTP handler / 按钮事件都经此汇聚）。
class Application {
public:
    static Application& GetInstance();

    /// 按钮闸门统计快照（串口 btbtn 自检用；public 以便控制台读取）
    struct ButtonStats {
        int draw_accepted = 0, draw_debounce = 0, draw_cooldown = 0;
        int page_accepted = 0, page_debounce = 0, page_cooldown = 0;
        int64_t draw_last_ms = -1;
    };
    ButtonStats ButtonStatsSnapshot() const;

    /// 复位手势被判为不可信（幽灵长按）而拒绝的次数 —— 排查无接触复位用
    int ResetPhantomRejects() const { return reset_phantom_rejects_; }

    /// GPIO5（恢复按钮）引脚黑匣子：常驻 10 ms 采样，记录每次电平跳变与累计低电平时长。
    /// 用途：下次再出现"无人接触却复位"时，能直接回答"引脚究竟有没有被拉低过"。
    struct ResetPinTrace {
        int edges = 0;            ///< 电平跳变次数（上电以来）
        int64_t low_ms_total = 0; ///< 累计处于低电平的毫秒数
        int64_t last_edge_ms = -1;///< 最近一次跳变时刻（-1=从未）
        int last_level = 1;       ///< 当前/最近一次采样电平
    };
    ResetPinTrace ResetPinTraceSnapshot() const;

    void Start();

    /// 跨线程调度到主线程（线程安全）
    void Schedule(std::function<void()> fn);

    LotteryController& controller() { return *controller_; }
    PrizePool& pool() { return *pool_; }
    PrinterService& printer() { return *printer_service_; }
    Dmx512Service& dmx() { return *dmx_service_; }

    /// 打印一张测试票（串口命令 / Web / 按钮长按共用）
    void PrintTestTicket();

    /// SPIFFS 是否可用（缺分区时降级：Web 页面不可用，但抽奖/打印照常）
    bool web_assets_available() const { return web_assets_available_; }

    /// 用给定模板打一张样张（背景叠加等画布功能的现场验证用）
    bool PrintTemplateForTest(const struct ReceiptTemplateV3& tmpl);   // 返回是否成功入队

    /// 标定点动：驱动到指定页（仅 idle/done 态；Web calibrate.html 用）
    bool DirectDrive(int page);

private:
    bool web_assets_available_ = false;
    Application() = default;

    void InitNvs();
    void InitFilesystems();      // SPIFFS（logo/prizes/mapping）
    void InitMappings();         // 显式 SetMapping（D21 Step 1b）→ PageAdjust
    void InitMotion();           // MotionEngine + Dmx512Service + 回调接线
    void InitPrinter();
    void InitButton();
    void MainLoop();

    std::unique_ptr<MotionEngine> engine_;
    std::unique_ptr<Dmx512Service> dmx_service_;
    std::unique_ptr<PrinterService> printer_service_;
    std::unique_ptr<PrizePool> pool_;
    std::unique_ptr<LotteryController> controller_;
    std::unique_ptr<Button> module_button_;
    std::unique_ptr<Button> reset_button_;   // GPIO5 长按 5 秒 → 复位网络配置
    // 按钮触发闸门：软件去抖 + 限流（抽奖与打自检页各一个，互不饿死）
    ButtonGate draw_gate_{BUTTON_MODULE_DEBOUNCE_MS, BUTTON_MODULE_COOLDOWN_MS};
    ButtonGate testpage_gate_{BUTTON_MODULE_DEBOUNCE_MS, BUTTON_MODULE_COOLDOWN_MS};

    // 恢复按钮（GPIO5）：只有「我们亲眼见过按下事件 + 引脚多点采样确为低电平」才允许复位。
    // 真机现象（2026-10-11）：无人接触时跑过一次网络复位，故加这两道证据闸门。
    int64_t reset_press_down_ms_ = -1;   ///< 最近一次按下事件时刻（-1=从未）
    int reset_phantom_rejects_ = 0;      ///< 被判为幽灵长按而拒绝的次数

    // GPIO5 引脚黑匣子（10 ms 周期采样；采样函数在 esp_timer 上下文，只做读+计数）
    static void ResetPinSample(void* arg);
    esp_timer_handle_t reset_pin_timer_ = nullptr;
    int reset_pin_last_level_ = 1;
    int reset_pin_edges_ = 0;
    int64_t reset_pin_low_ms_ = 0;
    int64_t reset_pin_last_edge_ms_ = -1;

    // ---- Schedule 机制（mutex + deque + event group） ----
    std::deque<std::function<void()>> schedule_queue_;
    SemaphoreHandle_t schedule_mutex_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t main_task_ = nullptr;

    static constexpr int kMainTaskStack = 8192;
    static constexpr int kMainTaskPrio = 4;
    static void MainTaskEntry(void* arg);
};

#endif  // _APPLICATION_H_
