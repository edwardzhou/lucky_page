cmake_minimum_required(VERSION 3.5)

# lucky-page 单测宿主（片上 unity：烧录 → 串口读取结果）
# ESP-IDF v6 移除了 tools/unit-test-app；unity 已是内置标准组件（components/unity），
# 由 PRIV_REQUIRES unity 直接解析，无需再挂 EXTRA_COMPONENT_DIRS。
set(EXTRA_COMPONENT_DIRS "")
# managed_components（button/cjson 等）由 idf_component.yml 拉取；目录不存在时不加入
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lucky_page_test)
