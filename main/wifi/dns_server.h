#ifndef _DNS_SERVER_H_
#define _DNS_SERVER_H_

#include "esp_err.h"

/// 捕获门户 DNS（设计 §8.1b）：
/// 对所有 A 查询应答本机 AP IP，使手机连上热点后 CNA 探测被劫持到配置页。
///
/// 学习自 page-player `main/wifi/dns_server.cc`（就地改造查询包），并补两处加固：
///   - 追加应答前做缓冲区边界检查（其原实现直接往 512B 缓冲尾部写，理论可越界）
///   - 每查询日志降为 DEBUG（手机一次探测会发多个查询，INFO 会刷屏并拖慢控制台任务）
esp_err_t dns_server_start(const char* ap_ip);
void dns_server_stop();
bool dns_server_is_running();

#endif  // _DNS_SERVER_H_
