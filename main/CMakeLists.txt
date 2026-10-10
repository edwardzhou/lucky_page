# lucky-page main 组件
file(GLOB MOTION_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/motion/*.cc")
file(GLOB DMX_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/dmx512/*.cc")
file(GLOB PRINTER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/printer/*.cc")
file(GLOB LOTTERY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/lottery/*.cc")
file(GLOB WIFI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/wifi/*.cc")
file(GLOB WEB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/web/*.cc")

set(BOARD_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/button.cc"
)

idf_component_register(
    SRCS
        "main.cc"
        "application.cc"
        "console_cmd.cc"
        "settings.cc"
        "nvs_saver.cc"
        "page_mapping.cc"
        "page_adjust.cc"
        ${MOTION_SOURCES}
        ${DMX_SOURCES}
        ${PRINTER_SOURCES}
        ${LOTTERY_SOURCES}
        ${WIFI_SOURCES}
        ${WEB_SOURCES}
        ${BOARD_SOURCES}
    # 点阵字库嵌入固件（图形模式用；放固件里而不是 SPIFFS，
    # 因为烧固件/OTA 会重写 SPIFFS 资源分区，用户素材会丢 —— 见评审 G3）
    EMBED_FILES
        "assets/font24.bin"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_timer
        nvs_flash
        spiffs
        esp_partition
        # ESP-IDF v6 移除了内置 json 组件 → 改用组件管理器的 espressif/cjson（见 idf_component.yml）
        # ESP-IDF v6 起 driver 兼容组件只剩 i2c/touch/twai，UART/GPIO 头文件
        # （路径仍为 driver/uart.h、driver/gpio.h）由下列组件提供，须显式声明
        esp_driver_uart
        esp_driver_gpio
        # console_cmd.cc 的 USB-Serial-JTAG VFS 行结尾设置（v6 起为独立驱动组件）
        esp_driver_usb_serial_jtag
        dw_printer
        button
        esp_wifi
        esp_netif
        esp_http_server
        esp_http_client
)

# ---- SPIFFS 镜像打包（Task 0.1 Step 4b，设计 B-5）----
# 分区名必须与 partitions/16m.csv 的 assets 分区一致。
# 漏掉这一步的后果：/spiffs/ 无镜像 → Web 页面 404、prizes.json/logo.bin 无处存。
set(SPIFFS_CONTENTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/spiffs_contents")
if(EXISTS "${SPIFFS_CONTENTS_DIR}")
    spiffs_create_partition_image(assets "${SPIFFS_CONTENTS_DIR}" FLASH_IN_PROJECT)
endif()
