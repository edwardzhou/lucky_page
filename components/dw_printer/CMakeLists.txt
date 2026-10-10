# lucky-page 打印机组件（设计 D6：raw_printer 路线，不用厂商 .a）
#
# 来源：
#   raw_printer.c / include/raw_printer.h   ← tft_displaye 原样搬运
#   raw_printer_qr.c / include/raw_printer_qr.h ← 新增（ESC/POS GS ( k 二维码）
#   printer_uart.cc（UART2 桥）在 main/printer/ —— 板级硬件层，不进本组件
#
# 不搬运：lib/*.a（厂商核心，RISC-V Zbb 缺陷 + 二进制依赖）、adapter/
idf_component_register(
    SRCS
        "raw_printer.c"
        "raw_printer_qr.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # ESP-IDF v6：driver 组件不再携带 UART/GPIO 头文件，须显式依赖（include 路径不变）
        esp_driver_uart
        esp_driver_gpio
        esp_timer
    PRIV_REQUIRES
        log
)
