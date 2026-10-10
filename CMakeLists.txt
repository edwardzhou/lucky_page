# lucky-page（幸运翻页机）— 单机抽奖翻页固件
# 基线：page-player（已升级 ESP-IDF 6.1.x），仅支持 ESP32-S3（N16R8）
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lucky-page VERSION 0.1.0)

# dw_printer 打印机组件（从 tft_displaye 搬运的 raw_printer 路线，设计 D6）
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
