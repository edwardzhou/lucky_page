// console_cmd.cc — 串口排障命令（设计 §8.3）
//   draw / ptest / pstatus / pprobe / ppins / pscan / pdump /
//   preceipt / ptmpl / pflow / pswap / prizes / gbtest
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "driver/gpio.h"
#include "esp_http_client.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
// ESP-IDF v6：esp_vfs_usb_serial_jtag.h 移除，VFS 行结尾设置改用 USB-Serial-JTAG 驱动的新 API
// （esp_vfs_dev_usb_serial_jtag_* → usb_serial_jtag_vfs_*，见迁移指引 6.0/storage）
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "application.h"
#include "lottery/prize_pool.h"
#include "printer/gbk_encoder.h"
#include "printer/printer_uart.h"
#include "lottery/time_source.h"
#include "wifi/wifi_config.h"
#include "wifi/wifi_manager.h"
#include "speaker/speaker_service.h"
#include "printer/receipt_defaults.h"
#include "printer/receipt_render.h"
#include "printer/receipt_template.h"
#include "settings.h"
#include "network_reset.h"

#define TAG "Console"

// ============================ 抽奖/打印 ============================
static void CmdDraw() {
    Application::GetInstance().Schedule([]() {
        if (!Application::GetInstance().controller().Trigger()) {
            printf("busy（状态 %s）\n", Application::GetInstance().controller().StateName());
        } else {
            printf("started\n");
        }
    });
}

// psample —— 用**当前保存的版式**打一张样张（走真实出票链路）
static void CmdPsample() {
    ReceiptTemplateV3 t = LoadTemplateFromSettings();
    std::string err = t.Validate();
    if (!err.empty()) {
        printf("当前版式非法: %s\n", err.c_str());
        return;
    }
    printf("按当前版式打样：%u 块, 纸宽 %d 列, 页面模式=%s\n", (unsigned)t.blocks.size(),
           t.paper_cols, Settings::GetInstance().printer_pagemode() ? "开" : "关");
    Application::GetInstance().Schedule([t]() { Application::GetInstance().PrintTemplateForTest(t); });
    printf("已入队（1 秒后开始打印）\n");
}

static void CmdPtest() {
    Application::GetInstance().Schedule(
        []() { Application::GetInstance().PrintTestTicket(); });
    printf("queued\n");
}

static const char* FlowName(int m) {
    switch (m) {
        case 1: return "cts ";   // 打印机→ESP32（仅状态回读）
        case 2: return "rts ";   // ESP32→打印机（打印方向）
        case 3: return "both";
        default: return "off ";
    }
}

static void CmdPstatus() {
    auto& p = Application::GetInstance().printer();
    int ok = 0, fail = 0;
    p.GetStats(&ok, &fail);
    int tx = -1, rx = -1, fp = -1;
    printer_flow_t mode = PRINTER_FLOW_OFF;
    printer_uart_get_config(&tx, &rx, &fp, &mode);
    printf("PRINTER 纸=%s 缺纸跳过=%d 张\n",
           p.paper_out() ? "⚠ 缺纸" : "有纸", p.skipped_count());
    printf("PRINTER ready=%d fault=%d ok=%d fail=%d\n", p.IsReady(), p.Fault(), ok, fail);
    printf("  接线(实际): TX=G%d  RX=G%d  流控=%s 流控脚=G%d  baud=%d\n", tx, rx,
           FlowName((int)mode), fp, Settings::GetInstance().printer_baud());
    printf("  接线(NVS) : TX=G%d  RX=G%d  mode=%d\n", Settings::GetInstance().printer_tx_pin(),
           Settings::GetInstance().printer_rx_pin(), Settings::GetInstance().printer_flow_mode());
    printf("  提示: TX 是 ESP32 侧输出；若一个字节都不打，先试 pswap，再试 pflow rts\n");
}

// pprobe —— 打印机存在性/极性探测：发 ESC/POS 状态查询并回读
static void CmdPprobe() {
    static const uint8_t kDleEot1[] = {0x10, 0x04, 0x01};
    static const uint8_t kGsR1[] = {0x1D, 0x72, 0x01};
    auto& p = Application::GetInstance().printer();
    if (!p.IsReady()) {
        printf("打印机服务未就绪\n");
        return;
    }
    uint8_t buf[32];
    int tx0 = -1, rx0 = -1, fp0 = -1;
    printer_flow_t m0 = PRINTER_FLOW_OFF;
    printer_uart_get_config(&tx0, &rx0, &fp0, &m0);
    printf("打印机探测（TX=G%d RX=G%d flow=%d）:\n", tx0, rx0, (int)m0);
    // kind: 0=DLE EOT 1（打印机状态） 1=GS r 1（纸传感器）
    auto probe = [&](const char* name, const uint8_t* cmd, size_t n, int kind) {
        while (uart_read_bytes(PRINTER_UART_NUM, buf, sizeof(buf), 0) > 0) {}
        uart_write_bytes(PRINTER_UART_NUM, (const char*)cmd, n);
        uart_wait_tx_done(PRINTER_UART_NUM, pdMS_TO_TICKS(200));
        int got = uart_read_bytes(PRINTER_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(400));
        printf("  %s → 回读 %d 字节", name, got);
        for (int i = 0; i < got; ++i) printf(" %02X", buf[i]);
        // 判定要严：真机状态应答是「1 字节」或很短的定长帧；
        // 悬空 RX 脚会读到大段重复噪声（真机踩坑：曾把噪声判成"打印机在线"）
        const char* verdict;
        if (got == 0) verdict = "  ❌ 无响应（打印机没接/没上电/RX 接错脚）";
        else if (got > 4) verdict = "  ⚠️ 疑似线噪（RX 脚悬空），不是打印机应答";
        else verdict = "  ✅ 有短应答，疑似打印机在线";
        printf("%s\n", verdict);
        // 收到单字节应答时**按位解码**（廉价机型可能不支持，故仅在拿到字节时解读）
        if (got == 1) {
            const uint8_t b = buf[0];
            if (kind == 0) {
                printf("     └ DLE EOT 1 解码: 在线=%s  纸尽=%s  出错=%s\n",
                       (b & 0x08) ? "否(离线)" : "是", (b & 0x20) ? "⚠ 是" : "否(有纸)",
                       (b & 0x40) ? "⚠ 是" : "否");
            } else {
                const int end_bits = (b >> 2) & 0x03;   // bit2..3
                printf("     └ GS r 1 解码: 纸传感器=%s  纸将尽=%s\n",
                       end_bits == 0x03 ? "⚠ 纸尽" : "有纸", (b & 0x03) ? "⚠ 是" : "否");
            }
        }
    };
    probe("DLE EOT 1", kDleEot1, sizeof(kDleEot1), 0);
    probe("GS r 1   ", kGsR1, sizeof(kGsR1), 1);
    printf("提示: 先跑 ppins 确认三个脚有外部驱动（悬空=没接上）。\n"
           "      再把 TX/RX 对调试 pswap，流控方向试 pflow rts / pflow cts。\n");
}

// ppins —— 决定性电平测量：判断打印机是否真的接在这三个脚上
static void CmdPpins() {
    const int pins[] = {9, 10, 11};
    const char* names[] = {"G9 (ESP32 TX)", "G10 (ESP32 RX)", "G11 (打印机 CTS)"};
    printf("电平测量（H=高 L=低；「外部驱动」= 抗住内部上下拉）:\n");
    printf("  %-20s %-6s %-6s %-6s %s\n", "引脚", "悬空", "上拉", "下拉", "判读");
    for (int i = 0; i < 3; ++i) {
        int lv[3];
        const gpio_pull_mode_t modes[3] = {GPIO_FLOATING, GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY};
        for (int m = 0; m < 3; ++m) {
            gpio_reset_pin((gpio_num_t)pins[i]);
            gpio_set_direction((gpio_num_t)pins[i], GPIO_MODE_INPUT);
            gpio_set_pull_mode((gpio_num_t)pins[i], modes[m]);
            vTaskDelay(pdMS_TO_TICKS(20));
            lv[m] = gpio_get_level((gpio_num_t)pins[i]);
        }
        const char* verdict = "悬空（无外部驱动）";
        if (lv[2] == 1) verdict = "外部驱动=高（有器件接着）";
        else if (lv[1] == 0) verdict = "外部驱动=低（有器件接着）";
        printf("  %-20s %-6s %-6s %-6s %s\n", names[i], lv[0] ? "H" : "L", lv[1] ? "H" : "L",
               lv[2] ? "H" : "L", verdict);
    }
    auto& s = Settings::GetInstance();
    printer_uart_set_pins(s.printer_tx_pin(), s.printer_rx_pin());
    printf("说明: 打印机 TX 脚应见「外部驱动=高」(UART 空闲高)。\n");
}

// pscan —— 扫描全部安全空闲脚，找出打印机实际接在哪
static void CmdPscan() {
    static const int kCands[] = {1, 2, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 21, 38, 39, 40, 41, 42, 47, 48};
    const int n = sizeof(kCands) / sizeof(kCands[0]);
    printf("扫描空闲脚（找外部驱动；在用: DMX=4/17/18 按钮=8 控制台=43/44 USB=19/20）:\n");
    int found = 0;
    for (int i = 0; i < n; ++i) {
        const int pin = kCands[i];
        int lv[3];
        const gpio_pull_mode_t modes[3] = {GPIO_FLOATING, GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY};
        for (int m = 0; m < 3; ++m) {
            gpio_reset_pin((gpio_num_t)pin);
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
            gpio_set_pull_mode((gpio_num_t)pin, modes[m]);
            vTaskDelay(pdMS_TO_TICKS(10));
            lv[m] = gpio_get_level((gpio_num_t)pin);
        }
        const char* verdict = nullptr;
        if (lv[2] == 1) verdict = "外部驱动=高 ← 有器件接着";
        else if (lv[1] == 0) verdict = "外部驱动=低 ← 有器件接着";
        if (verdict) {
            printf("  G%-2d  悬空=%s 上拉=%s 下拉=%s  %s\n", pin, lv[0] ? "H" : "L",
                   lv[1] ? "H" : "L", lv[2] ? "H" : "L", verdict);
            ++found;
        }
        gpio_reset_pin((gpio_num_t)pin);
    }
    if (found == 0) {
        printf("  （无任何空闲脚检测到外部驱动 → 打印机没接上，或没上电/没共地）\n");
    } else {
        printf("  共 %d 个脚有外部驱动。打印机 TX 应为「外部驱动=高」\n", found);
    }
    auto& s = Settings::GetInstance();
    printer_uart_set_pins(s.printer_tx_pin(), s.printer_rx_pin());
}

static void CmdPdump() {
    size_t len = 0;
    const uint8_t* tx = printer_uart_last_tx(&len);
    printf("PTX %zu bytes:\n", len);
    for (size_t i = 0; i < len; ++i) {
        printf("%02X ", tx[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16 != 0) printf("\n");
}

// preceipt [win|lose] [page]：在板上渲染样例小票并全量 hex 输出（无需打印机）
static void CmdPreceipt(int page, bool lose) {
    ReceiptTemplateV3 t = LoadTemplateFromSettings();
    ReceiptParams p = MakeSampleParams();
    p.page = page;
    p.is_lose = lose;
    if (lose) p.prize = p.lose_title;
    int unknown = 0;
    std::string preview = ReceiptRender::RenderPreviewText(t, p, &unknown);
    auto bytes = ReceiptRender::Render(t, p, nullptr);
    printf("RECEIPT %zu bytes (page=%d lose=%d unknown=%d):\n", bytes.size(), page + 1,
           (int)lose, unknown);
    for (size_t i = 0; i < bytes.size(); ++i) {
        printf("%02X ", (unsigned char)bytes[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (bytes.size() % 16 != 0) printf("\n");
    printf("--- 预览文本 ---\n%s\n", preview.c_str());
}

// ptmpl <json>：用给定模板 JSON 渲染预览（不落库），用于离线核对模板
static void CmdPtmpl(const char* json) {
    std::string err;
    auto t = ReceiptTemplateV3::FromJson(json, &err);
    if (!err.empty()) {
        printf("模板非法: %s\n", err.c_str());
        return;
    }
    int unknown = 0;
    printf("--- 模板预览（%zu 块）---\n%s\n", t.blocks.size(),
           ReceiptRender::RenderPreviewText(t, MakeSampleParams(), &unknown).c_str());
    if (unknown > 0) printf("⚠ %d 个未知参数（已原样保留）\n", unknown);
}

// pflow off|cts|rts|both —— 现场试流控方向（持久化，免重烧）
static void CmdPflow(const char* arg) {
    int mode = -1;
    if (strcmp(arg, "off") == 0) mode = 0;
    else if (strcmp(arg, "cts") == 0) mode = 1;
    else if (strcmp(arg, "rts") == 0) mode = 2;
    else if (strcmp(arg, "both") == 0) mode = 3;
    if (mode < 0) {
        printf("用法: pflow off|cts|rts|both\n"
               "  rts  = ESP32 RTS 输出 → 打印机 CTS（打印方向，优先试）\n"
               "  cts  = 打印机 RTS 输出 → ESP32 CTS（仅状态回读）\n");
        return;
    }
    auto& s = Settings::GetInstance();
    esp_err_t err = printer_uart_set_flow(s.printer_flow_pin(), (printer_flow_t)mode);
    if (err == ESP_OK) {
        s.set_printer_flow_mode(mode);
        s.SaveSync();
        printf("流控已设为 %s（脚 G%d）并持久化\n", FlowName(mode), s.printer_flow_pin());
    } else {
        printf("设置失败: %s\n", esp_err_to_name(err));
    }
}

// pswap —— 交换 TX/RX（接线接反的最快补救，持久化）
static void CmdPswap() {
    auto& s = Settings::GetInstance();
    int tx = s.printer_tx_pin();
    int rx = s.printer_rx_pin();
    esp_err_t err = printer_uart_set_pins(rx, tx);
    if (err == ESP_OK) {
        s.set_printer_tx_pin(rx);
        s.set_printer_rx_pin(tx);
        s.SaveSync();
        printf("TX/RX 已交换: TX=G%d RX=G%d（已持久化）\n", rx, tx);
    } else {
        printf("交换失败: %s\n", esp_err_to_name(err));
    }
}

// pdemo <1|2|3> —— 安装内置演示模板（持久化），便于现场快速确认版式
static void CmdPdemo(const char* arg) {
    int n = atoi(arg);
    if (n < 1 || n > 5) {
        printf("用法: pdemo 1|2|3|4|5（1 经典 / 2 简洁 / 3 促销 / 4 并排 / 5 水印）\n");
        return;
    }
    ReceiptTemplateV3 t = BuiltinTemplate(n);
    std::string err = t.Validate();
    if (!err.empty()) {
        printf("内置模板异常（不该发生）: %s\n", err.c_str());
        return;
    }
    auto& s = Settings::GetInstance();
    s.set_receipt_tmpl(t.ToJson());
    bool ok = s.SaveSync();
    printf("已安装演示模板 #%d（%zu 块）%s\n", n, t.blocks.size(), ok ? "" : " ⚠保存失败");
    int unknown = 0;
    printf("--- 预览 ---\n%s\n",
           ReceiptRender::RenderPreviewText(t, MakeSampleParams(), &unknown).c_str());
    if (unknown > 0) printf("⚠ %d 个未知参数\n", unknown);
}

// ptime <epoch> —— 串口校时（现场无网络/无浏览器时用）
static void CmdPtime(const char* arg) {
    long long ep = atoll(arg);
    if (ep <= 0) {
        printf("用法: ptime <epoch秒>（例 1790663527）；查看当前用 pstatus 或 preceipt\n");
        return;
    }
    if (TimeSource::GetInstance().SetFromWeb((time_t)ep)) {
        printf("已校时: %lld\n", ep);
    } else {
        printf("校时被拒（合理区间 2020..2100）\n");
    }
}

// wifi [scan|set <ssid> <pass>|off] —— WiFi STA 状态与配置（设计 §8.1b）
static void CmdWifi(const char* arg) {
    if (strncmp(arg, "scan", 4) == 0) {
        std::vector<WifiScanEntry> list;
        std::string err;
        printf("扫描中…\n");
        if (!wifi_manager::Scan(&list, &err)) {
            printf("扫描失败: %s\n", err.c_str());
            return;
        }
        printf("找到 %zu 个网络:\n", list.size());
        for (auto& e : list) {
            printf("  %-32s %4d dBm  %s\n", e.ssid.c_str(), e.rssi, e.secure ? "🔒" : "  ");
        }
        return;
    }
    if (strncmp(arg, "set ", 4) == 0) {
        char ssid[64] = "", pass[80] = "";
        int n = sscanf(arg + 4, "%63s %79s", ssid, pass);
        if (n < 1) {
            printf("用法: wifi set <ssid> [password]\n");
            return;
        }
        std::string err;
        if (wifi_manager::ApplySta(ssid, n >= 2 ? pass : "", &err)) {
            printf("已保存并连接: %s（用 wifi 查看结果）\n", ssid);
        } else {
            printf("失败: %s\n", err.c_str());
        }
        return;
    }
    if (strncmp(arg, "off", 3) == 0) {
        wifi_manager::ForgetSta();
        printf("已忘记网络，回到 AP-only\n");
        return;
    }
    // wifi disc —— 断开但**保留凭据**（学习 artnet 的「断开连接」）；
    // 断开后 HTTP 不可达，这条命令就是唯一的救援通道（配合 wifi on 恢复）
    if (strncmp(arg, "disc", 4) == 0) {
        std::string err;
        if (wifi_manager::DisconnectSta(&err)) {
            printf("已断开路由器（凭据保留）；用 wifi on 可重新连接\n");
        } else {
            printf("断开失败: %s\n", err.c_str());
        }
        return;
    }
    // wifi on —— 用已保存的最高优先网络重连（解除"用户主动断开"状态）
    if (strncmp(arg, "on", 2) == 0) {
        std::string err;
        if (wifi_manager::ReconnectSta(&err)) {
            printf("已发起重连（用 wifi 查看结果）\n");
        } else {
            printf("重连失败: %s\n", err.c_str());
        }
        return;
    }
    // wifi ascan —— 走**异步扫描**路径（与网页同一条代码：StartAsyncScan + PollScan），
    // 用于在"设备已连 STA、HTTP 会按白名单拒绝扫描"时也能验证该路径
    if (strncmp(arg, "ascan", 5) == 0) {
        std::string err;
        if (!wifi_manager::StartAsyncScan(&err)) {
            printf("异步扫描启动失败: %s\n", err.c_str());
            return;
        }
        std::vector<WifiScanEntry> list;
        bool running = false;
        for (int i = 0; i < 12; ++i) {
            vTaskDelay(pdMS_TO_TICKS(500));
            err.clear();
            if (!wifi_manager::PollScan(&list, &running, &err)) {
                printf("轮询失败: %s\n", err.c_str());
                return;
            }
            if (!running) break;
        }
        if (running) {
            printf("扫描超时（仍在进行）\n");
            return;
        }
        if (!err.empty()) {
            printf("扫描出错: %s\n", err.c_str());
            return;
        }
        printf("异步扫描完成，找到 %zu 个网络:\n", list.size());
        for (auto& e : list) {
            printf("  %-32s %4d dBm  %s\n", e.ssid.c_str(), e.rssi, e.secure ? "🔒" : "  ");
        }
        return;
    }
    // wifi resetnet —— **与硬件恢复按钮同一条代码路径**（按钮长按 5 秒调用它）
    if (strncmp(arg, "resetnet", 8) == 0) {
        ResetNetworkConfigToDefault("串口 wifi resetnet");
        printf("已恢复网络默认配置（热点名/密码 + 已保存网络 + DHCP）；用户数据未改动\n");
        return;
    }
    // wifi appass <密码> —— 改本机热点密码（空 = 复原默认 12345678）
    if (strncmp(arg, "appass", 6) == 0) {
        const char* pass = (arg[6] == ' ') ? arg + 7 : "";
        std::string err;
        if (wifi_manager::SetApPassword(pass, &err)) {
            printf("热点密码已%s；已连接热点的客户端需重连\n", pass[0] ? "更新" : "复原为默认");
            printf("（当前生效密码: %s）\n", wifi_manager::ApPasswordForConsole().c_str());
        } else {
            printf("设置失败: %s\n", err.c_str());
        }
        return;
    }
    // wifi ap <name> —— 改本机热点名（学习 artnet）
    if (strncmp(arg, "ap ", 3) == 0) {
        std::string err;
        if (wifi_manager::SetApSsid(arg + 3, &err)) {
            printf("热点名已改为: %s（已连接客户端需重连）\n", arg + 3);
        } else {
            printf("改名失败: %s\n", err.c_str());
        }
        return;
    }
    // wifi dhcp / wifi ip <ip> <mask> <gw> —— 静态 IP 配置（配错也不会丢热点）
    if (strncmp(arg, "dhcp", 4) == 0) {
        std::string err;
        wifi_manager::UseDhcp(&err);
        printf("已切回 DHCP\n");
        return;
    }
    if (strncmp(arg, "ip ", 3) == 0) {
        char ip[20] = "", mask[20] = "", gw[20] = "";
        if (sscanf(arg + 3, "%19s %19s %19s", ip, mask, gw) < 3) {
            printf("用法: wifi ip <ip> <mask> <gateway>\n");
            return;
        }
        std::string err;
        if (wifi_manager::SetStaticIp(ip, mask, gw, &err)) {
            printf("已设静态 IP %s/%s gw %s\n", ip, mask, gw);
        } else {
            printf("设置失败: %s\n", err.c_str());
        }
        return;
    }
    // 无参数 = 打印状态
    auto s = wifi_manager::Status();
    printf("WiFi 状态: 模式=%s 已配置=%d STA已连=%d 信号=%d(%u格) IP=%s 时间已同步=%d AP客户端=%u\n",
           s.mode_is_apsta ? "APSTA" : "AP", s.configured, s.sta_connected, (int)s.rssi,
           (unsigned)WifiRssiToBars(s.rssi), s.ip[0] ? s.ip : "-", s.time_synced,
           (unsigned)s.ap_clients);
    // 串口是**物理接触**的救援通道：这里打印真实 AP 密码（HTTP 层绝不回显，强制规则 9）
    printf("  本机热点: SSID=%s 密码=%s%s IP=192.168.4.1 信道=%u（%s）\n", s.ap_ssid,
           wifi_manager::ApPasswordForConsole().c_str(),
           Settings::GetInstance().ap_pass().empty() ? "（默认）" : "（自定义）",
           (unsigned)s.ap_channel, s.ap_up ? "广播中" : "未启动");
    auto saved = wifi_manager::SavedNetworks();
    printf("  已存网络(%zu 个, 按优先级):", saved.size());
    for (size_t i = 0; i < saved.size(); ++i) {
        printf(" %s%s", saved[i].ssid.c_str(), saved[i].password.empty() ? "(开放)" : "");
    }
    printf("%s  后台自动重连\n", saved.empty() ? "（无 → 仅热点模式）" : "");
    printf("  用法: wifi scan | wifi set <ssid> <pass> | wifi off\n");
    printf("        wifi disc（断开留凭据）| wifi on（重连）| wifi ascan（异步扫描）\n");
    printf("        wifi ap <名> | wifi appass <密码> | wifi resetnet（= 长按恢复按钮）\n");
    printf("        wifi ip <ip> <mask> <gw> | wifi dhcp     rstbtn | rstmon [秒]（引脚监视）\n");
    printf("        btbtn（抽奖按钮自检：触发电平/去抖/限流/当前电平/统计）\n");
}

// rstmon [秒] —— 监视恢复按钮引脚电平变化（排查"没人按却被触发"）
// 每 50ms 采样一次，只在电平变化时打印时间戳与电平，结束时给出下降沿计数。
static void CmdRstMon(const char* arg) {
    int secs = (arg && arg[0]) ? atoi(arg) : 30;
    if (secs < 1) secs = 1;
    if (secs > 600) secs = 600;
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << RESET_BUTTON_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    printf("监视 GPIO%d %d 秒（每 50ms 采样；只在变化时打印）：\n", (int)RESET_BUTTON_GPIO, secs);
    int last = gpio_get_level(RESET_BUTTON_GPIO);
    int lows = 0, changes = 0;
    int64_t t0 = esp_timer_get_time();
    printf("  t=0ms 初始电平=%d\n", last);
    while ((esp_timer_get_time() - t0) / 1000 < (int64_t)secs * 1000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int lv = gpio_get_level(RESET_BUTTON_GPIO);
        if (lv != last) {
            ++changes;
            if (lv == 0) ++lows;
            printf("  t=%lldms 电平 %d → %d%s\n", (long long)((esp_timer_get_time() - t0) / 1000),
                   last, lv, lv == 0 ? "（按下/被拉低！）" : "（松开）");
            last = lv;
        }
    }
    printf("监视结束：变化 %d 次，下降沿 %d 次（0 = 该引脚全程稳定，没人按也没被拉低）\n", changes,
           lows);
}

// btbtn —— 抽奖按钮自检：触发电平/去抖/限流/**当前电平** + 闸门统计
// 用途：现场确认接线的触发电平（上拉未按下应为 1），并按一下看计数变化
// ——**不需要真的出票**就能确认按钮与去抖/限流是否生效。
static void CmdBtBtn() {
    const Application::ButtonStats st = Application::GetInstance().ButtonStatsSnapshot();
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << BUTTON_MODULE_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = (BUTTON_MODULE_ACTIVE_LEVEL == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    io.pull_down_en = (BUTTON_MODULE_ACTIVE_LEVEL == 0) ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    const int lv = gpio_get_level(BUTTON_MODULE_GPIO);
    printf("抽奖按钮: GPIO%d %s 去抖 %d ms 限流 %d ms\n", (int)BUTTON_MODULE_GPIO,
           BUTTON_MODULE_ACTIVE_LEVEL == 0 ? "active_low（按下=0）" : "active_high（按下=1）",
           BUTTON_MODULE_DEBOUNCE_MS, BUTTON_MODULE_COOLDOWN_MS);
    printf("  当前电平=%d（%s）\n", lv,
           BUTTON_MODULE_ACTIVE_LEVEL == 0 ? (lv ? "未按下/上拉正常" : "正在按下或被拉低")
                                           : (lv ? "正在按下" : "未按下"));
    printf("  闸门统计: 抽奖 接受 %d / 去抖吞 %d / 限流忽略 %d%s", st.draw_accepted,
           st.draw_debounce, st.draw_cooldown,
           st.draw_last_ms >= 0 ? "" : "（尚未触发过）\n");
    if (st.draw_last_ms >= 0) printf("，上次接受于开机 %lld ms\n", (long long)st.draw_last_ms);
    printf("  自检 接受 %d / 去抖吞 %d / 限流忽略 %d\n", st.page_accepted, st.page_debounce,
           st.page_cooldown);
}

// rstbtn —— 恢复按钮自检：引脚号/有效电平/长按阈值/**当前电平**（上拉未按下应为 1）
static void CmdRstBtn() {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << RESET_BUTTON_GPIO);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    printf("恢复按钮: GPIO%d active_low 长按 %d ms\n", (int)RESET_BUTTON_GPIO, RESET_BUTTON_LONG_MS);
    printf("  当前电平=%d（1=未按下/上拉正常，0=正在按下）\n", gpio_get_level(RESET_BUTTON_GPIO));
    printf("  触发动作: 复位网络配置（等价于 wifi resetnet）\n");    {
        auto& app = Application::GetInstance();
        const int phantom = app.ResetPhantomRejects();
        printf("  幽灵长按拒绝次数=%d%s\n", phantom,
               phantom ? "（有驱动误报！看日志里的 E App 行）" : "（无）");
        printf("  可信性要求: 见过按下事件 + 引脚连续 8 次采样全为低 + 已过开机守卫\n");        const auto tr = app.ResetPinTraceSnapshot();
        printf("  引脚黑匣子: 累计跳变=%d 次，累计低电平=%lld ms，最近跳变=%lld ms，当前电平=%d\n",
               tr.edges, (long long)tr.low_ms_total, (long long)tr.last_edge_ms, tr.last_level);
        printf("  （若无按键操作却看到跳变>0，说明引脚被电气干扰拉低过 —— 看日志的 W App 行）\n");
    }
}

// httptest [path] —— 设备自己向本机 WebServer 发 HTTP 请求（无需外部网络，
// 用于验证静态页面/captive 跳转/接口是否真的可用；真机排障关键工具）
static void CmdHttpTest(const char* arg) {
    // 支持可选方法前缀：httptest [GET|POST|PUT|DELETE] <path>（默认 GET）。
    // 为什么需要：设备**断开路由器后**只有本机 AP 可达（192.168.4.1），
    // 这时要验证 POST/PUT 接口（如断开后重连）只能靠设备自测。
    esp_http_client_method_t method = HTTP_METHOD_GET;
    const char* method_name = "GET";
    const char* path = (arg && arg[0]) ? arg : "/";
    if (strncmp(path, "POST ", 5) == 0) {
        method = HTTP_METHOD_POST; method_name = "POST"; path += 5;
    } else if (strncmp(path, "PUT ", 4) == 0) {
        method = HTTP_METHOD_PUT; method_name = "PUT"; path += 4;
    } else if (strncmp(path, "DELETE ", 7) == 0) {
        method = HTTP_METHOD_DELETE; method_name = "DELETE"; path += 7;
    } else if (strncmp(path, "GET ", 4) == 0) {
        path += 4;
    }
    char url[128];
    snprintf(url, sizeof(url), "http://192.168.4.1%s", path);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = method;
    cfg.timeout_ms = 5000;
    cfg.disable_auto_redirect = true;   // 要看到 302 本身，不自动跟随
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == nullptr) {
        printf("HTTP 客户端创建失败\n");
        return;
    }
    // POST/PUT 发一个空 JSON 体（本机接口对空体与 {} 都是合法输入；坏 JSON 才 400）
    const char* payload = (method == HTTP_METHOD_POST || method == HTTP_METHOD_PUT) ? "{}" : nullptr;
    esp_err_t err = esp_http_client_open(cli, payload ? (int)strlen(payload) : 0);
    if (err != ESP_OK) {
        printf("%s %s 连接失败: %s\n", method_name, url, esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return;
    }
    // ⚠ open(cli, len) 只是**声明** Content-Length；body 必须真的 write 出去。
    // 漏掉这一步时 httpd 会一直等 body，最终回 "HTTP 408 Server closed this connection"
    // （真机实测踩到）——这不是接口的问题，而是自测工具没发 body。
    if (payload != nullptr) {
        int written = esp_http_client_write(cli, payload, strlen(payload));
        if (written < 0) {
            printf("发送请求体失败\n");
            esp_http_client_close(cli);
            esp_http_client_cleanup(cli);
            return;
        }
    }
    int hdr = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    printf("%s %s → HTTP %d (content_length=%d)\n", method_name, url, status, hdr);
    char* loc = nullptr;   // esp_http_client_get_header 需要 char**
    if (esp_http_client_get_header(cli, "Location", &loc) == ESP_OK && loc != nullptr) {
        printf("  Location: %s\n", loc);
    }
    char body[161];
    int n = esp_http_client_read(cli, body, 160);
    if (n > 0) {
        body[n] = 0;
        for (int i = 0; i < n; ++i) {
            if (body[i] == '\n') body[i] = ' ';
        }
        printf("  body: %.150s\n", body);
    } else {
        printf("  body: （空）\n");
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
}

// pbg —— 打「背景叠加」样张并切换页面模式开关
// 用法: pbg [on|off]   不带参数 = 打样张
static void CmdBgTest(const char* arg) {
    auto& s = Settings::GetInstance();
    if (arg && strncmp(arg, "on", 2) == 0) {
        s.set_printer_pagemode(true); s.SaveSync();
        printf("页面模式已开启（背景叠加生效）\n");
        return;
    }
    if (arg && strncmp(arg, "off", 3) == 0) {
        s.set_printer_pagemode(false); s.SaveSync();
        printf("页面模式已关闭（退回行式：图与字上下排列）\n");
        return;
    }
    // 样张：二维码作背景 + 三行文字叠加其上 + 一行在下方
    ReceiptTemplateV3 t;
    t.paper_cols = s.paper_cols();
    ReceiptBlock qr;
    qr.type = BlockType::kQr; qr.text = "BG-OVERLAY-{ticket}"; qr.row = 1; qr.col = 8;
    qr.qr_size = 4; qr.qr_ecc = 49; qr.bg = true;
    ReceiptBlock l1;
    l1.type = BlockType::kLine; l1.text = "背景叠加测试"; l1.row = 1; l1.col = 0;
    l1.size = 2; l1.bold = true;
    ReceiptBlock l2;
    l2.type = BlockType::kLine; l2.text = "文字应压在二维码上"; l2.row = 3; l2.col = 0;
    ReceiptBlock l3;
    l3.type = BlockType::kLine; l3.text = "行式打印做不到这种效果"; l3.row = 4; l3.col = 0;
    ReceiptBlock l4;
    l4.type = BlockType::kLine; l4.text = "（下方是普通文字）"; l4.row = 8; l4.col = 0;
    t.blocks = {qr, l1, l2, l3, l4};
    std::string err = t.Validate();
    if (!err.empty()) { printf("样张模板非法: %s\n", err.c_str()); return; }
    printf("页面模式: %s\n", s.printer_pagemode() ? "开" : "关（本张为行式对照）");
    printf("注: 本机（58mm 热敏）实测【不支持】页面模式：ESC L/ESC W/ESC FF 发出后\n"
           "    机器忽略，仍按行式输出 → 真正叠加做不到，只能图带独占若干行、\n"
           "    文字排在其上下方。页面模式代码保留，换支持该指令的机型即可复用。\n");
    Application::GetInstance().Schedule([t]() { Application::GetInstance().PrintTemplateForTest(t); });
    if (true) {
        printf("样张已入队，请看纸面：\n  若支持页面模式 → 文字与二维码重叠在同一片区域\n"
               "  若不支持 → 二维码与文字上下分离，或出现乱码\n"
               "确认支持后用 pbg on 开启；不支持用 pbg off 退回行式\n");
    } else {
        printf("入队失败（打印机忙？）\n");
    }
}

static void CmdPrizes() {
    auto& pool = Application::GetInstance().pool();
    auto probs = pool.DisplayProbabilities();
    printf("奖池（total_pages=%d policy=%d）\n", pool.total_pages(), (int)pool.policy());
    for (size_t i = 0; i < pool.tiers().size(); ++i) {
        const auto& t = pool.tiers()[i];
        printf("  L%d P%-3d w=%-5" PRIu32 " p=%5.1f%% qty=%-4d left=%-4d %s %s\n", t.level,
               t.page + 1, t.weight, probs[i] * 100, t.quantity,
               t.quantity < 0 ? -1 : t.remaining, t.name.c_str(), t.note.c_str());
    }
}

// sndplay —— 重播开机欢迎音（喇叭真机验证用，2026-10-25）
static void CmdSndPlay() {
    auto& svc = speaker::SpeakerService::GetInstance();
    if (!svc.PlayWelcome()) {
        printf("播放失败：I2S 未就绪或素材损坏（看 SpeakerSvc 日志）\n");
        return;
    }
    printf("欢迎音播放中（BCLK=G15 LRCK=G16 DOUT=G7）\n");
}

static void CmdGbtest(const char* text) {
    auto r = GbkEncoder::Encode(text);
    printf("UTF-8(%zu B) → GBK(%zu B): ", strlen(text), r.bytes.size());
    for (unsigned char c : r.bytes) printf("%02X ", c);
    printf("%s(replaced=%zu fallback=%zu)\n", (r.replaced || r.fallback) ? " ⚠" : "", r.replaced,
           r.fallback);
}

// ============================ 任务与分发 ============================
// 处理一整行命令（ConsoleTask 只在收到换行后才调用）
static void ProcessLine(char* line) {
    // 修剪行尾 CR/LF 与空格：不同输入方式（逐键回车 vs 一次性写入 "cmd\n"）
    // 到达这里时可能带换行符，不修剪会被 strcmp 判成未知命令（真机事故 2026-10-26）
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' '))
        line[--n] = '\0';
    if (n == 0) return;
    {

        if (strcmp(line, "help") == 0) {
            printf("draw|ptest|pstatus|pdump|preceipt [win|lose|<页码>]|prizes|gbtest <中文>\n"
                   "pflow off|cts|rts|both  pswap  pprobe  ppins  pscan\n"
                   "ptmpl <json>  pdemo 1|2|3  ptime <epoch>  wifi [scan|set|off]\n"
                   "httptest [GET|POST|PUT|DELETE] [path]  （板内自测 WebServer）\n"
                   "sndplay  sndwin  sndlose  sndtest [Hz] [ms]  sndvol <1-100>\n");
        } else if (strcmp(line, "draw") == 0) CmdDraw();
        else if (strcmp(line, "psample") == 0) CmdPsample();
        else if (strcmp(line, "ptest") == 0) CmdPtest();
        else if (strcmp(line, "pstatus") == 0) CmdPstatus();
        else if (strcmp(line, "pprobe") == 0) CmdPprobe();
        else if (strcmp(line, "ppins") == 0) CmdPpins();
        else if (strcmp(line, "pscan") == 0) CmdPscan();
        else if (strcmp(line, "pdump") == 0) CmdPdump();
        else if (strcmp(line, "pswap") == 0) CmdPswap();
        else if (strncmp(line, "pflow ", 6) == 0) CmdPflow(line + 6);
        else if (strncmp(line, "ptmpl ", 6) == 0) CmdPtmpl(line + 6);
        else if (strncmp(line, "preceipt", 8) == 0) {
            int page = 2;
            bool lose = false;
            char arg[32] = "";
            if (sscanf(line + 8, "%31s", arg) == 1) {
                if (strcmp(arg, "lose") == 0) lose = true;
                else if (strcmp(arg, "win") == 0) lose = false;
                else page = atoi(arg) - 1;
            }
            CmdPreceipt(page, lose);
        } else if (strncmp(line, "pdemo ", 6) == 0) CmdPdemo(line + 6);
        else if (strncmp(line, "ptime ", 6) == 0) CmdPtime(line + 6);
        else if (strncmp(line, "wifi", 4) == 0) CmdWifi(line[4] ? line + 5 : "");
        else if (strncmp(line, "rstbtn", 6) == 0) CmdRstBtn();
        else if (strncmp(line, "btbtn", 5) == 0) CmdBtBtn();
        else if (strncmp(line, "rstmon", 6) == 0) CmdRstMon(line[6] ? line + 7 : "");
        else if (strncmp(line, "httptest", 8) == 0) CmdHttpTest(line[8] ? line + 9 : "");
        else if (strncmp(line, "pbg", 3) == 0) CmdBgTest(line[3] ? line + 4 : "");
        else if (strcmp(line, "prizes") == 0) CmdPrizes();
        else if (strncmp(line, "gbtest ", 7) == 0) CmdGbtest(line + 7);
        else if (strcmp(line, "sndplay") == 0) CmdSndPlay();
        else if (strcmp(line, "sndlose") == 0) {
            if (!speaker::SpeakerService::GetInstance().PlayLose())
                printf("播放失败：I2S 未就绪（看 SpeakerSvc 日志）\n");
            else printf("落空音效播放中\n");
        } else if (strcmp(line, "sndwin") == 0) {
            if (!speaker::SpeakerService::GetInstance().PlayChime())
                printf("播放失败：I2S 未就绪（看 SpeakerSvc 日志）\n");
            else printf("中奖音效播放中\n");
        }
        else if (strncmp(line, "sndvol ", 7) == 0) {
            int v = atoi(line + 7);
            speaker::SpeakerService::GetInstance().SetVolume(v);
            printf("音量 → %d%%（再 sndtest/sndplay 对比失真是否随幅度变化）\n",
                   speaker::SpeakerService::GetInstance().volume_percent());
        } else if (strncmp(line, "sndtest", 7) == 0) {
            unsigned hz = 440, ms = 2000;
            sscanf(line + 7, "%u %u", &hz, &ms);
            speaker::SpeakerService::GetInstance().PlayTone(hz, ms);
            printf("正弦测试音 %uHz %ums 播放中\n", hz, ms);
        }
        else printf("未知命令（help 查看）\n");
        fflush(stdout);
    }
}

// 逐字符累积到换行才执行（2026-10-26 真机事故：终端逐键发送时，USB-Serial-JTAG
// 的 VFS 非阻塞，fgets 读到一个字符后下一次 read 返回 0 被当作 EOF →
// 半个单词就被当成一条命令 → 每敲一个键都回「未知命令」。改为自己累积：
// 只有收到 \r/\n 才分发；EOF/无数据只让出 CPU；支持退格回显。）
static void ConsoleTask(void*) {
    char line[1024];   // 长模板 JSON 用
    size_t len = 0;
    while (true) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0) continue;
            line[len] = '\0';
            ProcessLine(line);
            len = 0;
            continue;
        }
        if (c == 0x7f || c == 0x08) {          // 退格：删一个字符并回显
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;                            // 超长行丢弃，避免状态错乱
            printf("\n命令过长，已丢弃\n");
        }
    }
}

extern "C" void console_cmd_start() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
#else
    // ESP-IDF v6：esp_vfs_dev_uart_port_set_* → uart_vfs_dev_port_set_*（driver/uart_vfs.h）
    uart_vfs_dev_port_set_rx_line_endings(0, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(0, ESP_LINE_ENDINGS_CRLF);
#endif
    xTaskCreate(ConsoleTask, "console", 4096, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "串口命令就绪：draw/ptest/pstatus/pprobe/ppins/pscan/pdump/preceipt/ptmpl/prizes/gbtest");
}
