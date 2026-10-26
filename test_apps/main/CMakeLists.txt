# lucky-page 单测宿主 —— 被测模块编译清单
#
# ⚠️ 每新增一个可单测模块，必须同步加进这里（否则测试链接失败——M-7 教训）。
# ⚠️ 插入位置：所有 set(...) 之后、idf_component_register(...) 之前（list(APPEND) 在 set 前会被覆盖，N-4 教训）。
#
# 模块到达时间表（按 Phase）：
#   Phase 0: motion / dmx512 / settings / page_mapping / page_adjust / nvs_saver ←（本文件已含）
#   Phase 1: lottery/printer 的 gbk_encoder.cc、gbk_table.cc、receipt_layout.cc、printer_logo.cc
#   Phase 2: lottery/prize_pool.cc、slot_machine_effect.cc、draw_record.cc、time_source.cc

set(MOTION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main/motion")
set(DMX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main/dmx512")
set(LOTTERY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main/lottery")
set(PRINTER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main/printer")
set(WIFI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main/wifi")
set(MAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main")

file(GLOB MOTION_SOURCES "${MOTION_DIR}/*.cc")
file(GLOB DMX_SOURCES "${DMX_DIR}/*.cc")
file(GLOB LOTTERY_SOURCES "${LOTTERY_DIR}/*.cc")    # Phase 2 起有内容
file(GLOB PRINTER_SOURCES "${PRINTER_DIR}/*.cc")    # Phase 1 起有内容
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

# WiFi 只取纯逻辑层 wifi_config.cc（驱动层 wifi_manager.cc 依赖 esp_wifi/esp_netif 事件，
# 不上测试宿主——设计 §8.1b 的"可测部分下沉为纯函数"）
set(WIFI_SOURCES "${WIFI_DIR}/wifi_config.cc")

# dw_printer 组件源（QR/打印机核心；Task 1.1 起接入）
set(DWPRINTER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/dw_printer")
set(DWPRINTER_SOURCES
    "${DWPRINTER_DIR}/raw_printer.c"
    "${DWPRINTER_DIR}/raw_printer_qr.c"
)

# 喇叭纯逻辑层（wav_data.cc 解析 EMBED 的 WAV；speaker_service 是 I2S 驱动层不上宿主）
set(SPEAKER_SOURCES
    "${MAIN_DIR}/speaker/wav_data.cc"
    "${MAIN_DIR}/speaker/win_sound.cc")

set(MAIN_SOURCES
    "${MAIN_DIR}/button_gate.cc"
    "${MAIN_DIR}/reset_gesture.cc"
    "${MAIN_DIR}/settings.cc"
    "${MAIN_DIR}/page_mapping.cc"
    "${MAIN_DIR}/page_adjust.cc"
    "${MAIN_DIR}/nvs_saver.cc"
)

idf_component_register(
    SRCS ${MOTION_SOURCES} ${DMX_SOURCES}
         ${LOTTERY_SOURCES}
        ${WIFI_SOURCES} ${PRINTER_SOURCES}
         ${TEST_SOURCES} ${MAIN_SOURCES} ${SPEAKER_SOURCES} ${DWPRINTER_SOURCES}
    # 点阵字库（图形模式）：测试宿主也要嵌入，否则链接时找不到 _binary_font24_bin_start
    EMBED_FILES
        "../../main/assets/font24.bin"

    INCLUDE_DIRS "." "${CMAKE_CURRENT_SOURCE_DIR}/../../main" "${DWPRINTER_DIR}/include"
    # ESP-IDF v6：json 组件移除（cJSON 由组件管理器的 espressif/cjson 提供，见 idf_component.yml）
    PRIV_REQUIRES unity esp_driver_uart esp_driver_gpio esp_timer nvs_flash spiffs esp_netif lwip
    WHOLE_ARCHIVE
)
