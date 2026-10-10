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
#include "printer/receipt_defaults.h"
#include "printer/receipt_render.h"
#include "printer/receipt_template.h"
#include "settings.h"

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
    // 无参数 = 打印状态
    auto s = wifi_manager::Status();
    printf("WiFi 状态: 模式=%s 已配置=%d STA已连=%d 信号=%d(%u格) IP=%s 时间已同步=%d AP客户端=%u\n",
           s.mode_is_apsta ? "APSTA" : "AP", s.configured, s.sta_connected, (int)s.rssi,
           (unsigned)WifiRssiToBars(s.rssi), s.ip[0] ? s.ip : "-", s.time_synced,
           (unsigned)s.ap_clients);
    printf("  本机热点: SSID=%s 密码=12345678 IP=192.168.4.1 信道=%u（%s）\n", s.ap_ssid,
           (unsigned)s.ap_channel, s.ap_up ? "广播中" : "未启动");
    auto saved = wifi_manager::SavedNetworks();
    printf("  已存网络(%zu 个, 按优先级):", saved.size());
    for (size_t i = 0; i < saved.size(); ++i) {
        printf(" %s%s", saved[i].ssid.c_str(), saved[i].password.empty() ? "(开放)" : "");
    }
    printf("%s  后台自动重连\n", saved.empty() ? "（无 → 仅热点模式）" : "");
    printf("  用法: wifi scan | wifi set <ssid> <pass> | wifi off\n");
}

// httptest [path] —— 设备自己向本机 WebServer 发 HTTP 请求（无需外部网络，
// 用于验证静态页面/captive 跳转/接口是否真的可用；真机排障关键工具）
static void CmdHttpTest(const char* arg) {
    const char* path = (arg && arg[0]) ? arg : "/";
    char url[128];
    snprintf(url, sizeof(url), "http://192.168.4.1%s", path);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 5000;
    cfg.disable_auto_redirect = true;   // 要看到 302 本身，不自动跟随
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == nullptr) {
        printf("HTTP 客户端创建失败\n");
        return;
    }
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        printf("GET %s 连接失败: %s\n", url, esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return;
    }
    int hdr = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    printf("GET %s → HTTP %d (content_length=%d)\n", url, status, hdr);
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

static void CmdGbtest(const char* text) {
    auto r = GbkEncoder::Encode(text);
    printf("UTF-8(%zu B) → GBK(%zu B): ", strlen(text), r.bytes.size());
    for (unsigned char c : r.bytes) printf("%02X ", c);
    printf("%s(replaced=%zu fallback=%zu)\n", (r.replaced || r.fallback) ? " ⚠" : "", r.replaced,
           r.fallback);
}

// ============================ 任务与分发 ============================
static void ConsoleTask(void*) {
    char line[1024];   // 长模板 JSON 用
    while (true) {
        // 控制台是 USB-Serial-JTAG 时 fgets 无输入会立刻返回 NULL，必须让出 CPU
        if (fgets(line, sizeof(line), stdin) == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (line[0] == '\0' || line[0] == '\n') continue;
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "help") == 0) {
            printf("draw|ptest|pstatus|pdump|preceipt [win|lose|<页码>]|prizes|gbtest <中文>\n"
                   "pflow off|cts|rts|both  pswap  pprobe  ppins  pscan\n"
                   "ptmpl <json>  pdemo 1|2|3  ptime <epoch>  wifi [scan|set|off]\n"
                   "httptest [path]  （板内自测 WebServer）\n");
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
        else if (strncmp(line, "httptest", 8) == 0) CmdHttpTest(line[8] ? line + 9 : "");
        else if (strncmp(line, "pbg", 3) == 0) CmdBgTest(line[3] ? line + 4 : "");
        else if (strcmp(line, "prizes") == 0) CmdPrizes();
        else if (strncmp(line, "gbtest ", 7) == 0) CmdGbtest(line + 7);
        else printf("未知命令（help 查看）\n");
        fflush(stdout);
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
