#include "wifi/dns_server.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define TAG "CaptiveDns"

namespace {
int s_sock = -1;
bool s_running = false;
uint32_t s_answer_ip = 0;   // 网络序

void DnsTask(void*) {
    uint8_t buf[512];
    while (s_running) {
        struct sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &from_len);
        if (n < 12) {           // 比 DNS 头还短 → 丢弃
            if (!s_running) break;
            continue;
        }
        // 计算问题段长度：QNAME（含结尾 0）+ QTYPE(2) + QCLASS(2)
        size_t q = 12;
        while (q < (size_t)n && buf[q] != 0) q += (size_t)buf[q] + 1;
        q += 1 + 4;
        if (q > (size_t)n) continue;          // 包被截断 → 丢弃

        const uint16_t qtype = (uint16_t)((buf[q - 4] << 8) | buf[q - 3]);
        // 只对 A(1) / ANY(255) 给 A 记录；其余类型回「无应答」，避免客户端拿到错误类型
        const bool with_answer = (qtype == 1 || qtype == 255);
        const size_t need = with_answer ? 16 : 0;
        if ((size_t)n + need > sizeof(buf)) continue;   // 边界检查（原实现缺这一步）

        buf[2] = 0x81;   // QR=1（响应）RD=1
        buf[3] = 0x80;   // RA=1
        buf[6] = 0;      // ANCOUNT 高字节
        buf[7] = with_answer ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;   // NS/AR = 0
        size_t len = q;
        if (with_answer) {
            static const uint8_t kAnsHead[] = {0xC0, 0x0C,              // 名称指针→问题段
                                               0x00, 0x01,              // TYPE=A
                                               0x00, 0x01,              // CLASS=IN
                                               0x00, 0x00, 0x00, 0x3C,  // TTL=60
                                               0x00, 0x04};             // RDLENGTH=4
            memcpy(buf + len, kAnsHead, sizeof(kAnsHead));
            len += sizeof(kAnsHead);
            memcpy(buf + len, &s_answer_ip, 4);
            len += 4;
        }
        ESP_LOGD(TAG, "DNS %s qtype=%u → %s", "query", (unsigned)qtype,
                 with_answer ? "应答本机" : "空应答");
        sendto(s_sock, buf, len, 0, (struct sockaddr*)&from, from_len);
    }
    s_sock = -1;
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t dns_server_start(const char* ap_ip) {
    if (s_running) return ESP_OK;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "创建 socket 失败");
        return ESP_FAIL;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(s_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "绑定 53 端口失败（手机不会自动弹配置页）");
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    inet_aton(ap_ip, (struct in_addr*)&s_answer_ip);
    s_running = true;
    if (xTaskCreate(DnsTask, "captive_dns", 3072, nullptr, 5, nullptr) != pdPASS) {
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "捕获门户 DNS 已启动：所有域名 → %s", ap_ip);
    return ESP_OK;
}

void dns_server_stop() {
    if (!s_running) return;
    s_running = false;
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
    }
}

bool dns_server_is_running() { return s_running; }
