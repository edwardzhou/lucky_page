#include "boards/common/button.h"

#include "button_gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#define TAG "Button"

Button::Button(gpio_num_t pin, uint8_t active_level, uint16_t long_press_ms) {
    button_config_t button_cfg = {};
    button_cfg.long_press_time = long_press_ms;
    button_cfg.short_press_time = 0;  // 0 使用组件默认短按时间

    button_gpio_config_t gpio_cfg = {};
    gpio_cfg.gpio_num = pin;
    gpio_cfg.active_level = active_level;
    gpio_cfg.enable_power_save = false;
    gpio_cfg.disable_pull = false;  // 按 active_level 自动配置上拉/下拉

    esp_err_t err = iot_button_new_gpio_device(&button_cfg, &gpio_cfg, &handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iot_button 创建失败, GPIO %d, err=0x%x", pin, err);
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "按钮初始化完成, GPIO %d, active_level=%u, 当前电平=%d", pin, active_level,
             gpio_get_level(pin));
}

Button::Button(button_driver_t* driver, uint16_t long_press_ms) {
    button_config_t button_cfg = {};
    button_cfg.long_press_time = long_press_ms;
    button_cfg.short_press_time = 0;  // 0 使用组件默认短按时间

    esp_err_t err = iot_button_create(&button_cfg, driver, &handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iot_button 自定义驱动创建失败, err=0x%x", err);
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "按钮初始化完成(注入驱动), long_press_ms=%u", long_press_ms);
}

Button::~Button() {
    if (handle_ != nullptr) {
        iot_button_delete(handle_);
        handle_ = nullptr;
    }
}

void Button::RegisterEvent(button_event_t event, std::function<void()>& slot,
                           std::function<void()> callback) {
    if (handle_ == nullptr) {
        return;
    }
    // 覆盖语义：先注销该事件已有的全部回调，避免重复注册导致双触发
    // （仅在已有回调时注销，避免首次注册时产生 INVALID_STATE 错误日志）
    if (slot) {
        iot_button_unregister_cb(handle_, event, nullptr);
    }
    slot = std::move(callback);
    if (!slot) {
        return;
    }
    button_cb_t trampoline = nullptr;
    switch (event) {
        case BUTTON_PRESS_DOWN:
            trampoline = &Button::OnPressEvent;
            break;
        case BUTTON_PRESS_UP:
            trampoline = &Button::OnReleaseEvent;
            break;
        case BUTTON_SINGLE_CLICK:
            trampoline = &Button::OnClickEvent;
            break;
        case BUTTON_DOUBLE_CLICK:
            trampoline = &Button::OnDoubleClickEvent;
            break;
        case BUTTON_LONG_PRESS_START:
            trampoline = &Button::OnLongPressEvent;
            break;
        default:
            ESP_LOGW(TAG, "不支持的按钮事件 %d，已忽略", static_cast<int>(event));
            slot = nullptr;
            return;
    }
    esp_err_t err = iot_button_register_cb(handle_, event, nullptr, trampoline, this);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册按钮事件 %d 失败, err=0x%x", static_cast<int>(event), err);
        slot = nullptr;
    }
}

void Button::OnPressEvent(void* /*handle*/, void* usr_data) {
    auto* btn = static_cast<Button*>(usr_data);
    if (btn->on_press_) {
        btn->on_press_();
    }
}

void Button::OnReleaseEvent(void* /*handle*/, void* usr_data) {
    auto* btn = static_cast<Button*>(usr_data);
    if (btn->on_release_) {
        btn->on_release_();
    }
}

void Button::OnClickEvent(void* /*handle*/, void* usr_data) {
    auto* btn = static_cast<Button*>(usr_data);
    if (btn->on_click_) {
        btn->on_click_();
    }
}

void Button::OnDoubleClickEvent(void* /*handle*/, void* usr_data) {
    auto* btn = static_cast<Button*>(usr_data);
    if (btn->on_double_click_) {
        btn->on_double_click_();
    }
}

void Button::OnLongPressEvent(void* /*handle*/, void* usr_data) {
    auto* btn = static_cast<Button*>(usr_data);
    if (btn->on_long_press_) {
        btn->on_long_press_();
    }
}

void Button::OnPress(std::function<void()> callback) {
    RegisterEvent(BUTTON_PRESS_DOWN, on_press_, std::move(callback));
}

void Button::OnRelease(std::function<void()> callback) {
    RegisterEvent(BUTTON_PRESS_UP, on_release_, std::move(callback));
}

void Button::OnClick(std::function<void()> callback) {
    RegisterEvent(BUTTON_SINGLE_CLICK, on_click_, std::move(callback));
}

void Button::OnDoubleClick(std::function<void()> callback) {
    RegisterEvent(BUTTON_DOUBLE_CLICK, on_double_click_, std::move(callback));
}

void Button::OnLongPress(std::function<void()> callback) {
    RegisterEvent(BUTTON_LONG_PRESS_START, on_long_press_, std::move(callback));
}
