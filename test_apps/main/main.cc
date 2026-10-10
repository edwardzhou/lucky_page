// lucky-page 单测宿主入口（片上 unity）
#include "esp_log.h"

#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "unity.h"

extern "C" void app_main() {
    // 部分 argv 风格的 unity 入口需要；防止优化掉
    Settings::GetInstance().Load();   // 走真实加载路径（否则全是默认值）
    ESP_LOGI("test_main", "lucky-page 单测宿主启动，3s 后开始（等待上位机打开串口）");
    vTaskDelay(pdMS_TO_TICKS(3000));
    // NVS 初始化（Settings 字段表持久化路径在片上真实可用）
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    unity_run_all_tests();
}
