#ifndef _BUTTON_H_
#define _BUTTON_H_

#include "driver/gpio.h"
#include "iot_button.h"

#include <cstdint>
#include <functional>

/**
 * @brief esp-iot-solution iot_button 的 RAII C++ 薄包装（板级硬件抽象层）
 *
 * 事件回调运行在 esp_timer 任务上下文，回调内禁止阻塞或耗时操作。
 * 重复调用同一 OnXxx() 为覆盖语义（先注销旧回调再注册新回调）。
 *
 * 并发约束：OnXxx() 仅允许在回调可能触发之前（初始化阶段）调用，
 * 禁止运行期重注册（与 esp_timer 上下文中正在执行的回调存在数据竞争）。
 * Button 实例的销毁同样仅允许在不再有回调触发时进行（现有用法均为
 * 静态单例或应用生命周期持有，满足该约束）。
 */
class Button {
public:
    /**
     * @param pin           GPIO 编号
     * @param active_level  按下时的电平（0：上拉/按下接地；1：下拉/按下接高）
     * @param long_press_ms 长按触发阈值（ms），0 使用组件默认值
     */
    Button(gpio_num_t pin, uint8_t active_level = 0, uint16_t long_press_ms = 1000);

    /**
     * @brief 自定义驱动注入构造（依赖倒置，供测试模拟按键电平）
     *
     * @param driver        iot_button 驱动描述（get_key_level 返回 1=按下），
     *                      指针生命周期由调用方保证，须不早于本对象销毁
     * @param long_press_ms 长按触发阈值（ms），0 使用组件默认值
     */
    Button(button_driver_t* driver, uint16_t long_press_ms = 1000);
    ~Button();

    Button(const Button&) = delete;
    Button& operator=(const Button&) = delete;

    void OnPress(std::function<void()> callback);        // BUTTON_PRESS_DOWN
    void OnRelease(std::function<void()> callback);      // BUTTON_PRESS_UP
    void OnClick(std::function<void()> callback);        // BUTTON_SINGLE_CLICK
    void OnDoubleClick(std::function<void()> callback);  // BUTTON_DOUBLE_CLICK
    void OnLongPress(std::function<void()> callback);    // BUTTON_LONG_PRESS_START

private:
    void RegisterEvent(button_event_t event, std::function<void()>& slot,
                       std::function<void()> callback);

    static void OnPressEvent(void* handle, void* usr_data);
    static void OnReleaseEvent(void* handle, void* usr_data);
    static void OnClickEvent(void* handle, void* usr_data);
    static void OnDoubleClickEvent(void* handle, void* usr_data);
    static void OnLongPressEvent(void* handle, void* usr_data);

    button_handle_t handle_ = nullptr;
    std::function<void()> on_press_;
    std::function<void()> on_release_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::function<void()> on_long_press_;
};

#endif  // _BUTTON_H_
