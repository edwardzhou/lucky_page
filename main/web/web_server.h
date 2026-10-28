#ifndef _WEB_SERVER_H_
#define _WEB_SERVER_H_

#include <memory>

#include <esp_http_server.h>

/// WebServer（设计 §8.1；约 16 个端点 + SPIFFS 静态托管）
///
/// 线程模型（D23）：
/// - 写操作：HTTP 任务解析 JSON → 【纯函数校验】→ 通过才 Schedule 到主线程落库
/// - 重读（需业务对象）：ScheduleSyncResult 到主线程，值类型（JSON 串）回填
/// - 高频轻读（/api/lottery/status）：直接读 atomic 快照，不过主线程
class WebServer {
public:
    static WebServer& GetInstance();

    bool Start();

private:
    WebServer() = default;

    httpd_handle_t server_ = nullptr;

    // ---- 帮助器 ----
    static std::string ReadBody(httpd_req_t* req);
    static esp_err_t SendJson(httpd_req_t* req, const std::string& json, int status = 200);
    static esp_err_t SendError(httpd_req_t* req, const std::string& msg, int status = 400);
    static esp_err_t SendFile(httpd_req_t* req, const char* path);

    // ---- 抽奖/状态 ----
    static esp_err_t DrawHandler(httpd_req_t* req);
    static esp_err_t LotteryStatusHandler(httpd_req_t* req);
    static esp_err_t StatusHandler(httpd_req_t* req);
    static esp_err_t PrinterTestHandler(httpd_req_t* req);
    static esp_err_t TimeSetHandler(httpd_req_t* req);   // 浏览器校时（AP-only 现场无 NTP）
    // ---- WiFi STA（设计 §8.1b）----
    static esp_err_t WifiGetHandler(httpd_req_t* req);
    static esp_err_t WifiScanHandler(httpd_req_t* req);
    // 学习 artnet：异步扫描轮询 / 改热点名 / 静态 IP / 显式断开
    static esp_err_t WifiScanPollHandler(httpd_req_t* req);
    static esp_err_t WifiApPutHandler(httpd_req_t* req);
    static esp_err_t WifiIpPutHandler(httpd_req_t* req);
    static esp_err_t WifiDisconnectHandler(httpd_req_t* req);
    static esp_err_t WifiReconnectHandler(httpd_req_t* req);
    static esp_err_t WifiPutHandler(httpd_req_t* req);
    static esp_err_t WifiDeleteHandler(httpd_req_t* req);

    // ---- 奖池/页数/标定 ----
    static esp_err_t PrizesGetHandler(httpd_req_t* req);
    static esp_err_t PrizesPutHandler(httpd_req_t* req);
    static esp_err_t PrizesResetStockHandler(httpd_req_t* req);
    static esp_err_t PagesGetHandler(httpd_req_t* req);
    static esp_err_t PagesPutHandler(httpd_req_t* req);
    static esp_err_t PageMappingPutHandler(httpd_req_t* req);
    static esp_err_t PageGotoHandler(httpd_req_t* req);
    // 标定「页码调整模式」：基础映射 + 每页偏移（参考 page-player 的行程微调）
    static esp_err_t AdjustGetHandler(httpd_req_t* req);
    static esp_err_t AdjustPutHandler(httpd_req_t* req);
    static esp_err_t AdjustResetHandler(httpd_req_t* req);

    // ---- 文案/预览 ----
    static esp_err_t ReceiptGetHandler(httpd_req_t* req);
    static esp_err_t ReceiptPutHandler(httpd_req_t* req);
    static esp_err_t ReceiptPreviewHandler(httpd_req_t* req);
    // ---- 块模板 v3（设计 §7.3b）----
    static esp_err_t ReceiptTemplateGetHandler(httpd_req_t* req);
    static esp_err_t ReceiptTemplatePutHandler(httpd_req_t* req);
    static esp_err_t ReceiptParamsHandler(httpd_req_t* req);
    static esp_err_t ReceiptTemplateResetHandler(httpd_req_t* req);
    static esp_err_t ReceiptTemplatePreviewHandler(httpd_req_t* req);

    // ---- Logo ----
    static esp_err_t LogoPostHandler(httpd_req_t* req);
    static esp_err_t LogoGetHandler(httpd_req_t* req);
    static esp_err_t LogoRawHandler(httpd_req_t* req);   // LGO1 原始字节（画布缩略图用）
    static esp_err_t LogoDeleteHandler(httpd_req_t* req);
    static esp_err_t SdStatusHandler(httpd_req_t* req);
    static esp_err_t SdListHandler(httpd_req_t* req);
    static esp_err_t SdUploadHandler(httpd_req_t* req);
    static esp_err_t SdDownloadHandler(httpd_req_t* req);
    static esp_err_t SdDeleteHandler(httpd_req_t* req);
    static esp_err_t SdRenameHandler(httpd_req_t* req);
    static esp_err_t SdMkdirHandler(httpd_req_t* req);
    static esp_err_t SdMountHandler(httpd_req_t* req);
    static esp_err_t SdUnmountHandler(httpd_req_t* req);
    static esp_err_t SoundsGetHandler(httpd_req_t* req);
    static esp_err_t SoundsPostHandler(httpd_req_t* req);
    static esp_err_t SoundsDeleteHandler(httpd_req_t* req);
    static esp_err_t SoundsRawHandler(httpd_req_t* req);
    static esp_err_t SoundsPlayHandler(httpd_req_t* req);
    static esp_err_t AssetsGetHandler(httpd_req_t* req);
    static esp_err_t AssetsPostHandler(httpd_req_t* req);
    static esp_err_t AssetsDeleteHandler(httpd_req_t* req);
    static esp_err_t AssetsRawHandler(httpd_req_t* req);
    static esp_err_t PageContentGetHandler(httpd_req_t* req);
    static esp_err_t PageContentPutHandler(httpd_req_t* req);

    // ---- 静态/捕获门户 ----
    static esp_err_t StaticHandler(httpd_req_t* req);
};

#endif  // _WEB_SERVER_H_
