// lucky-page（幸运翻页机）— 程序入口
#include "esp_log.h"

#include "application.h"

#define TAG "lucky-page"

extern "C" void console_cmd_start();

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "==== lucky-page 幸运翻页机 ====");
    ESP_LOGI(TAG, "板型: ESP32-S3-N16R8 (16MB Flash + 8MB Octal PSRAM)");
    ESP_LOGI(TAG, "打印机: UART2 TX=G9 RX=G10 CTS=G11（G43/44 禁用）");
    Application::GetInstance().Start();
    console_cmd_start();
}
