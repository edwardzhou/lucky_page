#include "web/web_server.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>

#include "application.h"
#include "lottery/draw_record.h"
#include "lottery/lottery_controller.h"
#include "lottery/prize_pool.h"
#include "lottery/time_source.h"
#include "page_adjust.h"
#include "page_mapping.h"
#include "printer/logo_store.h"
#include "printer/printer_logo.h"
#include "printer/receipt_defaults.h"
#include "printer/receipt_layout.h"
#include "printer/receipt_render.h"
#include "printer/receipt_template.h"
#include "settings.h"
#include "wifi/wifi_config.h"
#include "wifi/wifi_manager.h"

#define TAG "WebServer"

static constexpr int kSyncTimeoutMs = 2000;

// URI handler 上限。
// ⚠ 真机事故（2026-10-10）：ESP-IDF 的 httpd 在注册数超过 max_uri_handlers 时是
// **静默失败**（只打一条 ESP_LOGE），表现为"最后注册的通配静态路由 `/*` 没生效 →
// 所有页面 404 Nothing matches the given URI"，而 API 全部正常 —— 极易误判成 SPIFFS 坏了。
// 本次因新增 3 条 /api/adjust 路由，把 31+2=33 顶破了原来是 32 的上限。
// 对策：① 留足余量；② 编译期 static_assert（见 Start() 内），让超限在编译期就暴露。
static constexpr size_t kMaxUriHandlers = 40;

// MainSync：把任务投到主线程并同步取回 string 结果（D23"重读"路径）
// 单层类型擦除，勿嵌套泛型 lambda（曾把此文件写到 79 万行）
static std::string MainSync(std::function<std::string()> fn, int timeout_ms = kSyncTimeoutMs) {
    auto shared = std::make_shared<std::string>();
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    auto task = std::make_shared<std::function<std::string()>>(std::move(fn));
    Application::GetInstance().Schedule([shared, done, task]() {
        *shared = (*task)();
        xSemaphoreGive(done);
    });
    std::string result;
    if (xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        result = *shared;
    }
    vSemaphoreDelete(done);
    return result;
}

static std::string JsonDump(cJSON* root) {
    char* s = cJSON_PrintUnformatted(root);
    std::string out(s ? s : "{}");
    cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

// ============================ 帮助器 ============================
std::string WebServer::ReadBody(httpd_req_t* req) {
    size_t total = req->content_len;
    std::string body(total, '\0');
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, &body[got], (int)(total - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    body.resize(got);
    return body;
}

esp_err_t WebServer::SendJson(httpd_req_t* req, const std::string& json, int status) {
    if (status != 200) {
        char hdr[16];
        snprintf(hdr, sizeof(hdr), "%d", status);
        httpd_resp_set_status(req, hdr);
    }
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    return httpd_resp_send(req, json.c_str(), (ssize_t)json.size());
}

esp_err_t WebServer::SendError(httpd_req_t* req, const std::string& msg, int status) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg.c_str());
    return SendJson(req, JsonDump(root), status);
}

esp_err_t WebServer::SendFile(httpd_req_t* req, const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_FAIL;
    }
    const char* type = "text/html";
    if (strstr(path, ".css")) type = "text/css";
    else if (strstr(path, ".js")) type = "application/javascript";
    else if (strstr(path, ".png")) type = "image/png";
    httpd_resp_set_type(req, type);
    // 严禁缓存：前端资源改动后必须立刻生效（真机踩坑：无缓存头 → 浏览器
    // 启发式缓存旧页面 → 用户点按钮没反应，看着像功能坏了）
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ============================ 抽奖 / 状态 ============================
esp_err_t WebServer::DrawHandler(httpd_req_t* req) {
    struct Ctx {
        std::string state;
        bool started = false;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx]() -> std::string {
        auto& ctl = Application::GetInstance().controller();
        ctx->started = ctl.Trigger();
        ctx->state = ctl.StateName();
        return "ok";
    });
    if (ctx->started) return SendJson(req, "{\"status\":\"started\"}");
    return SendJson(req, "{\"status\":\"busy\",\"state\":\"" + ctx->state + "\"}", 409);
}

esp_err_t WebServer::LotteryStatusHandler(httpd_req_t* req) {
    // 高频轻读：直接读 seqlock 快照（D23，不过主线程）
    auto snap = Application::GetInstance().controller().Status().Read();
    static const char* kStates[] = {"boot",     "homing",   "homed",   "homing_failed",
                                    "idle",     "spinning", "printing", "done",
                                    "out_of_prizes"};
    static const char* kPrints[] = {"ok", "failed", "skipped"};

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", snap.state < 9 ? kStates[snap.state] : "unknown");
    cJSON_AddNumberToObject(root, "draw_id", snap.draw_id);
    cJSON_AddNumberToObject(root, "current_page", snap.current_page);
    cJSON* last = cJSON_AddObjectToObject(root, "last");
    cJSON_AddStringToObject(last, "ticket", snap.ticket);
    cJSON_AddStringToObject(last, "prize_name", snap.prize_name);
    cJSON_AddStringToObject(last, "prize_note", snap.prize_note);
    cJSON_AddStringToObject(last, "time", snap.time_str);
    cJSON_AddNumberToObject(last, "page", snap.page);
    cJSON_AddStringToObject(last, "print",
                            snap.print_outcome < 3 ? kPrints[snap.print_outcome] : "?");
    cJSON* stats = cJSON_AddObjectToObject(root, "stats");
    cJSON_AddNumberToObject(stats, "total_draws", snap.total_draws);
    cJSON_AddNumberToObject(stats, "today_draws", snap.today_draws);
    cJSON_AddBoolToObject(root, "printer_fault", snap.printer_fault != 0);
    cJSON_AddBoolToObject(root, "time_synced", snap.time_synced != 0);
    return SendJson(req, JsonDump(root));
}

esp_err_t WebServer::StatusHandler(httpd_req_t* req) {
    auto json = MainSync([]() -> std::string {
        auto& app = Application::GetInstance();
        auto& s = Settings::GetInstance();
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "device", "lucky-page");
        cJSON_AddNumberToObject(root, "total_pages", s.total_pages());
        cJSON_AddNumberToObject(root, "cell_count", s.cell_count());
        cJSON_AddNumberToObject(root, "dmx_start_addr", s.dmx_start_addr());
        cJSON_AddNumberToObject(root, "printer_baud", s.printer_baud());
        cJSON_AddNumberToObject(root, "printer_tx_pin", s.printer_tx_pin());
        cJSON_AddNumberToObject(root, "printer_rx_pin", s.printer_rx_pin());
        cJSON_AddBoolToObject(root, "printer_paper_ok",
                              !Application::GetInstance().printer().paper_out());
        cJSON_AddNumberToObject(root, "printer_skipped",
                                Application::GetInstance().printer().skipped_count());
        cJSON_AddBoolToObject(root, "printer_ready", app.printer().IsReady());
        cJSON_AddBoolToObject(root, "printer_fault", app.printer().Fault());
        cJSON_AddBoolToObject(root, "wifi_up", wifi_manager::IsUp());
        cJSON_AddBoolToObject(root, "time_synced", TimeSource::GetInstance().IsSynced());
        cJSON_AddStringToObject(
            root, "now", DrawRecord::FormatTime(TimeSource::GetInstance().NowEpoch()).c_str());
        return JsonDump(root);
    });
    if (json.empty()) return SendError(req, "主线程繁忙", 503);
    return SendJson(req, json);
}

esp_err_t WebServer::PrinterTestHandler(httpd_req_t* req) {
    // 带模板 body → **打印 body 里的模板**（所见即所打：画布上没保存的改动也照打）；
    // 不带 body → 打印当前保存的生效模板（原行为）。
    // 真机问题：原来只打"上次保存的"，用户切了类型/改了版式后打测试页看到的还是旧的。
    std::string body = ReadBody(req);
    if (!body.empty()) {
        std::string err;
        auto t = ReceiptTemplateV3::FromJson(body, &err);
        if (!err.empty()) return SendError(req, err);
        struct Ctx {
            bool ok = false;
        };
        auto ctx = std::make_shared<Ctx>();
        MainSync([t, ctx]() -> std::string {
            ctx->ok = Application::GetInstance().PrintTemplateForTest(t);
            return ctx->ok ? "ok" : "busy";
        });
        if (!ctx->ok) return SendError(req, "打印机忙，请稍后重试", 503);
        return SendJson(req, "{\"status\":\"queued\",\"source\":\"editor\"}");
    }
    MainSync([]() -> std::string {
        Application::GetInstance().PrintTestTicket();
        return "ok";
    });
    return SendJson(req, "{\"status\":\"queued\",\"source\":\"saved\"}");
}

// ============================ 奖池 ============================
esp_err_t WebServer::PrizesGetHandler(httpd_req_t* req) {
    auto json = MainSync([]() -> std::string {
        auto& pool = Application::GetInstance().pool();
        auto probs = pool.DisplayProbabilities();
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", 2);
        cJSON_AddNumberToObject(root, "total_pages", pool.total_pages());
        cJSON_AddNumberToObject(root, "depletion_policy", (int)pool.policy());
        cJSON* arr = cJSON_AddArrayToObject(root, "tiers");
        for (size_t i = 0; i < pool.tiers().size(); ++i) {
            const auto& t = pool.tiers()[i];
            cJSON* o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "level", t.level);
            cJSON_AddNumberToObject(o, "page", t.page);
            cJSON_AddNumberToObject(o, "weight", (double)t.weight);
            cJSON_AddNumberToObject(o, "probability", probs[i]);
            cJSON_AddNumberToObject(o, "quantity", t.quantity);
            cJSON_AddNumberToObject(o, "remaining", t.remaining);
            cJSON_AddBoolToObject(o, "depleted", t.quantity > 0 && t.remaining == 0);
            cJSON_AddStringToObject(o, "name", t.name.c_str());
            cJSON_AddStringToObject(o, "note", t.note.c_str());
            cJSON_AddItemToArray(arr, o);
        }
        return JsonDump(root);
    });
    if (json.empty()) return SendError(req, "主线程繁忙", 503);
    return SendJson(req, json);
}

esp_err_t WebServer::PrizesPutHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    // 纯函数校验（HTTP 任务内；通过才进主线程 —— D23 写路径）
    std::string err;
    PrizePool candidate = PrizePool::FromJson(body, &err);
    if (!err.empty()) return SendError(req, err);
    {
        int pages = Settings::GetInstance().total_pages();
        PrizePool adjusted(candidate.tiers(), pages, candidate.policy());
        auto vr = adjusted.Validate();
        if (!vr.ok) return SendError(req, vr.error);
    }
    MainSync([body]() -> std::string {
        auto& app = Application::GetInstance();
        auto& s = Settings::GetInstance();
        s.set_prize_pool_backup(s.prize_pool());   // 先备份再写主（Task 2.1c）
        s.set_prize_pool(body);
        s.SaveSync();
        std::string err2;
        app.pool() = PrizePool::FromJson(body, &err2);
        return "ok";
    });
    return SendJson(req, "{\"status\":\"saved\"}");
}

esp_err_t WebServer::PrizesResetStockHandler(httpd_req_t* req) {
    MainSync([]() -> std::string {
        auto& app = Application::GetInstance();
        auto& s = Settings::GetInstance();
        app.pool().ResetStock();
        s.set_prize_pool_backup(s.prize_pool());
        s.set_prize_pool(app.pool().ToJson());
        s.SaveSync();
        return "ok";
    });
    return SendJson(req, "{\"status\":\"reset\"}");
}

// ============================ 页数 / 标定 ============================
esp_err_t WebServer::PagesGetHandler(httpd_req_t* req) {
    auto json = MainSync([]() -> std::string {
        auto& s = Settings::GetInstance();
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "total_pages", s.total_pages());
        cJSON_AddNumberToObject(root, "stroke_min_dmx", s.stroke_min_dmx());
        cJSON_AddNumberToObject(root, "stroke_max_dmx", s.stroke_max_dmx());
        cJSON_AddNumberToObject(root, "min_page_gap_dmx", s.min_page_gap_dmx());
        const auto& mapping = PageMapping::GetInstance().GetMapping();
        cJSON* vals = cJSON_AddArrayToObject(root, "values");
        for (int i = 0; i < s.total_pages() && i < (int)mapping.size(); ++i)
            cJSON_AddItemToArray(vals, cJSON_CreateNumber(mapping[i]));
        return JsonDump(root);
    });
    if (json.empty()) return SendError(req, "主线程繁忙", 503);
    return SendJson(req, json);
}

esp_err_t WebServer::PagesPutHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "JSON 解析失败");
    cJSON* tp = cJSON_GetObjectItem(root, "total_pages");
    if (!cJSON_IsNumber(tp) || tp->valueint < 2 || tp->valueint > 128) {
        cJSON_Delete(root);
        return SendError(req, "total_pages 必须在 2..128");
    }
    int new_pages = tp->valueint;
    cJSON_Delete(root);

    struct Ctx {
        std::string conflict;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx, new_pages]() -> std::string {
        for (const auto& t : Application::GetInstance().pool().tiers()) {
            if (t.page >= new_pages) {
                ctx->conflict = "页码 " + std::to_string(t.page) + "（「" + t.name +
                                "」）超出新页数 " + std::to_string(new_pages) + "，请先改奖池";
                return "conflict";
            }
        }
        auto& s = Settings::GetInstance();
        s.set_total_pages(new_pages);
        s.SaveSync();
        return "ok";
    });
    if (!ctx->conflict.empty()) return SendError(req, ctx->conflict);
    return SendJson(req, "{\"status\":\"saved\"}");
}

esp_err_t WebServer::PageMappingPutHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "JSON 解析失败");

    cJSON* values = cJSON_GetObjectItem(root, "values");
    cJSON* names = cJSON_GetObjectItem(root, "names");
    if (!cJSON_IsArray(values)) {
        cJSON_Delete(root);
        return SendError(req, "缺少 values 数组");
    }
    auto& s = Settings::GetInstance();
    const int pages = s.total_pages();

    std::vector<uint8_t> vals;
    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, values) {
        if (!cJSON_IsNumber(it)) {
            cJSON_Delete(root);
            return SendError(req, "values 含非数字项");
        }
        vals.push_back((uint8_t)it->valueint);
    }
    std::vector<std::string> nm;
    if (cJSON_IsArray(names)) {
        cJSON* nit = nullptr;
        cJSON_ArrayForEach(nit, names) {
            nm.push_back(cJSON_IsString(nit) && nit->valuestring ? nit->valuestring : "");
        }
    }
    cJSON_Delete(root);

    // 公共纯函数校验（与开机断言同一套规则 —— Task 2.1d Step 5）
    std::string err = ValidatePageMapping(vals, pages, s.min_page_gap_dmx(), s.stroke_min_dmx(),
                                          s.stroke_max_dmx());
    if (!err.empty()) return SendError(req, err);

    MainSync([vals, nm, pages]() -> std::string {
        PageMapping::GetInstance().SetMapping(vals, nm);
        PageAdjust::GetInstance().Resize(1, pages);
        PageAdjust::GetInstance().SaveAdjust();
        return "ok";
    });
    return SendJson(req, "{\"status\":\"saved\"}");
}

esp_err_t WebServer::PageGotoHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "JSON 解析失败");
    cJSON* page = cJSON_GetObjectItem(root, "page");
    if (!cJSON_IsNumber(page)) {
        cJSON_Delete(root);
        return SendError(req, "缺少 page");
    }
    int p = page->valueint;
    cJSON_Delete(root);

    struct Ctx {
        bool ok = false;
        std::string state;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx, p]() -> std::string {
        ctx->ok = Application::GetInstance().DirectDrive(p);
        ctx->state = Application::GetInstance().controller().StateName();
        return "ok";
    });
    if (!ctx->ok) return SendError(req, "状态 " + ctx->state + " 不允许点动，或页码越界", 409);
    return SendJson(req, "{\"status\":\"started\"}");
}

// ==================== 行程偏移（标定「页码调整模式」）====================
// 参考 page-player 的行程微调：基础映射（PageMapping）不动，逐页叠加 int8 偏移
// （PageAdjust），实际 DMX 值 = clamp(base + offset, 0, 255)。
// 点动（DirectDrive）已叠加偏移，故"微调 → 点动 → 眼睛确认"构成闭环。

esp_err_t WebServer::AdjustGetHandler(httpd_req_t* req) {
    auto json = MainSync([]() -> std::string {
        auto& s = Settings::GetInstance();
        auto& adj = PageAdjust::GetInstance();
        const auto& mapping = PageMapping::GetInstance().GetMapping();
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "cell_count", 1);   // 单机模式固定 1 路
        cJSON_AddNumberToObject(root, "total_pages", s.total_pages());
        // offsets 保持二维形状（与 page-player 一致，便于将来多机扩展）
        cJSON* offs = cJSON_AddArrayToObject(root, "offsets");
        cJSON* cell0 = cJSON_CreateArray();
        for (int i = 0; i < s.total_pages(); ++i) {
            cJSON_AddItemToArray(cell0, cJSON_CreateNumber(adj.GetOffset(0, i)));
        }
        cJSON_AddItemToArray(offs, cell0);
        cJSON* map = cJSON_AddArrayToObject(root, "mapping");
        for (int i = 0; i < s.total_pages() && i < (int)mapping.size(); ++i) {
            cJSON_AddItemToArray(map, cJSON_CreateNumber(mapping[i]));
        }
        return JsonDump(root);
    });
    if (json.empty()) return SendError(req, "主线程繁忙", 503);
    return SendJson(req, json);
}

esp_err_t WebServer::AdjustPutHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "JSON 解析失败");

    cJSON* jcell = cJSON_GetObjectItem(root, "cell");
    cJSON* jpage = cJSON_GetObjectItem(root, "page");
    cJSON* joff = cJSON_GetObjectItem(root, "offset");
    if (!cJSON_IsNumber(jpage) || !cJSON_IsNumber(joff)) {
        cJSON_Delete(root);
        return SendError(req, "缺少 page 或 offset");
    }
    const int cell = cJSON_IsNumber(jcell) ? jcell->valueint : 0;
    const int page = jpage->valueint;
    const int offset = joff->valueint;
    cJSON_Delete(root);

    // ⚠ 必须显式校验：PageAdjust::SetOffset 对越界是**静默忽略**，
    // 不校验的话前端会以为改成功了其实没改（本项目踩过的同类坑）
    if (cell != 0) return SendError(req, "cell 必须为 0（单机模式）");
    auto& settings = Settings::GetInstance();
    const int pages = settings.total_pages();
    if (page < 0 || page >= pages) return SendError(req, "page 越界（0..total_pages-1）");
    if (offset < -128 || offset > 127) return SendError(req, "offset 必须在 -128..127");

    struct Ctx {
        int base = 0;
        int value = 0;
        bool ok = false;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx, page, offset]() -> std::string {
        PageAdjust::GetInstance().SetOffset(0, page, (int8_t)offset);
        const auto& mapping = PageMapping::GetInstance().GetMapping();
        ctx->base = (page < (int)mapping.size()) ? mapping[page] : 0;
        ctx->value = PageAdjust::GetInstance().GetAdjustedValue(0, page);
        ctx->ok = true;
        return "ok";
    });
    if (!ctx->ok) return SendError(req, "主线程繁忙", 503);

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "cell", 0);
    cJSON_AddNumberToObject(out, "page", page);
    cJSON_AddNumberToObject(out, "offset", offset);
    cJSON_AddNumberToObject(out, "base", ctx->base);
    cJSON_AddNumberToObject(out, "value", ctx->value);
    return SendJson(req, JsonDump(out));
}

esp_err_t WebServer::AdjustResetHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "JSON 解析失败");

    cJSON* jcell = cJSON_GetObjectItem(root, "cell");
    cJSON* jpage = cJSON_GetObjectItem(root, "page");
    const int cell = cJSON_IsNumber(jcell) ? jcell->valueint : 0;
    const int page = cJSON_IsNumber(jpage) ? jpage->valueint : -1;   // 缺省 = 清零全部
    cJSON_Delete(root);

    if (cell != 0 && cell != -1) return SendError(req, "cell 必须为 0 或 -1");
    auto& settings = Settings::GetInstance();
    const int pages = settings.total_pages();
    if (page != -1 && (page < 0 || page >= pages)) {
        return SendError(req, "page 越界（-1=全部，0..total_pages-1）");
    }

    struct Ctx {
        int cleared = 0;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx, page, pages]() -> std::string {
        if (page == -1) {
            PageAdjust::GetInstance().ResetAll();
            ctx->cleared = pages;
        } else {
            PageAdjust::GetInstance().SetOffset(0, page, 0);
            ctx->cleared = 1;
        }
        return "ok";
    });
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "cleared", ctx->cleared);
    return SendJson(req, JsonDump(out));
}

// ============================ 文案 / 预览 ============================
esp_err_t WebServer::ReceiptGetHandler(httpd_req_t* req) {
    auto json = MainSync([]() -> std::string {
        auto& s = Settings::GetInstance();
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "shop_name", s.shop_name().c_str());
        cJSON_AddStringToObject(root, "win_title", s.win_title().c_str());
        cJSON_AddStringToObject(root, "lose_title", s.lose_title().c_str());
        cJSON_AddStringToObject(root, "header", s.receipt_header().c_str());
        cJSON_AddStringToObject(root, "footer", s.receipt_footer().c_str());
        cJSON_AddNumberToObject(root, "validity_days", s.validity_days());
        cJSON_AddNumberToObject(root, "paper_cols", s.paper_cols());
        cJSON_AddBoolToObject(root, "qr_enabled", s.qr_enabled());
        cJSON_AddBoolToObject(root, "logo_enabled", s.logo_enabled());
        return JsonDump(root);
    });
    if (json.empty()) return SendError(req, "主线程繁忙", 503);
    return SendJson(req, json);
}

esp_err_t WebServer::ReceiptPutHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* probe = cJSON_Parse(body.c_str());
    if (probe == nullptr) return SendError(req, "JSON 解析失败");
    cJSON_Delete(probe);

    struct Ctx {
        bool ok = false;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([body, ctx]() -> std::string {
        auto& s = Settings::GetInstance();
        cJSON* r = cJSON_Parse(body.c_str());
        if (r == nullptr) return "bad";
        cJSON* f;
        if ((f = cJSON_GetObjectItem(r, "shop_name")) && cJSON_IsString(f))
            s.set_shop_name(f->valuestring);
        if ((f = cJSON_GetObjectItem(r, "win_title")) && cJSON_IsString(f))
            s.set_win_title(f->valuestring);
        if ((f = cJSON_GetObjectItem(r, "lose_title")) && cJSON_IsString(f))
            s.set_lose_title(f->valuestring);
        if ((f = cJSON_GetObjectItem(r, "header")) && cJSON_IsString(f))
            s.set_receipt_header(f->valuestring);
        if ((f = cJSON_GetObjectItem(r, "footer")) && cJSON_IsString(f))
            s.set_receipt_footer(f->valuestring);
        if ((f = cJSON_GetObjectItem(r, "validity_days")) && cJSON_IsNumber(f))
            s.set_validity_days(f->valueint);
        if ((f = cJSON_GetObjectItem(r, "paper_cols")) && cJSON_IsNumber(f))
            s.set_paper_cols(f->valueint == 48 ? 48 : 32);
        if ((f = cJSON_GetObjectItem(r, "qr_enabled")) && cJSON_IsBool(f))
            s.set_qr_enabled(cJSON_IsTrue(f));
        if ((f = cJSON_GetObjectItem(r, "logo_enabled")) && cJSON_IsBool(f))
            s.set_logo_enabled(cJSON_IsTrue(f));
        cJSON_Delete(r);
        ctx->ok = s.SaveSync();
        return ctx->ok ? "ok" : "save_failed";
    });
    if (!ctx->ok) return SendError(req, "保存失败", 500);
    return SendJson(req, "{\"status\":\"saved\"}");
}

esp_err_t WebServer::ReceiptPreviewHandler(httpd_req_t* req) {
    // 用「当前生效模板」渲染预览（v3 路径，与模板编辑页一致）
    auto json = MainSync([]() -> std::string {
        auto t = LoadTemplateFromSettings();
        ReceiptParams p = MakeSampleParams();
        int unknown = 0;
        std::string text = ReceiptRender::RenderPreviewText(t, p, &unknown);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "text", text.c_str());
        cJSON_AddNumberToObject(root, "unknown_params", unknown);
        cJSON_AddNumberToObject(root, "blocks", (int)t.blocks.size());
        cJSON_AddNumberToObject(root, "paper_cols", t.paper_cols);
        return JsonDump(root);
    });
    if (json.empty()) return SendError(req, "主线程繁忙", 503);
    return SendJson(req, json);
}

// ============================ 校时（浏览器时间）============================
esp_err_t WebServer::TimeSetHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "JSON 解析失败");
    cJSON* ep = cJSON_GetObjectItem(root, "epoch");
    if (!cJSON_IsNumber(ep)) {
        cJSON_Delete(root);
        return SendError(req, "缺少 epoch（秒）");
    }
    double val = ep->valuedouble;
    cJSON_Delete(root);
    if (val < 1577836800.0 || val >= 4102444800.0) {
        return SendError(req, "epoch 不在合理区间（2020..2100）");
    }
    struct Ctx {
        bool ok = false;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx, val]() -> std::string {
        ctx->ok = TimeSource::GetInstance().SetFromWeb((time_t)val);
        return ctx->ok ? "ok" : "fail";
    });
    if (!ctx->ok) return SendError(req, "校时失败", 500);
    return SendJson(req, "{\"status\":\"ok\"}");
}

// ============================ WiFi STA（设计 §8.1b）============================
esp_err_t WebServer::WifiGetHandler(httpd_req_t* req) {
    // 状态由纯函数生成，**保证不含密码字段**（有单测锁定）；
    // 已保存网络列表同样只回 SSID + 是否有密码
    auto snap = wifi_manager::Status();
    std::string json = WifiStatusToJson(snap);
    json.pop_back();   // 去掉末尾 '}'
    json += ",\"saved\":" + WifiSavedListToJson(wifi_manager::SavedNetworks());
    // 学习 artnet 的进阶项：IP 模式与静态配置（展示用，非机密）
    auto& s = Settings::GetInstance();
    cJSON* extra = cJSON_CreateObject();
    cJSON_AddNumberToObject(extra, "ip_mode", s.ip_mode());
    cJSON_AddStringToObject(extra, "static_ip", s.static_ip().c_str());
    cJSON_AddStringToObject(extra, "static_mask", s.static_mask().c_str());
    cJSON_AddStringToObject(extra, "static_gw", s.static_gw().c_str());
    cJSON_AddBoolToObject(extra, "ap_ssid_custom", !s.ap_ssid().empty());
    // 密码绝不回显：只告知"是否自定义"（强制规则 9）
    cJSON_AddBoolToObject(extra, "ap_pass_custom", !s.ap_pass().empty());
    cJSON_AddBoolToObject(extra, "user_disconnected", wifi_manager::IsUserDisconnected());
    // 网络复位证据（次数 + 时间），便于排查"没人按却被复位"
    cJSON_AddNumberToObject(extra, "net_reset_count", s.net_reset_count());
    cJSON_AddNumberToObject(extra, "net_reset_epoch", (double)s.net_reset_epoch());
    char* extra_str = cJSON_PrintUnformatted(extra);
    cJSON_Delete(extra);
    if (extra_str != nullptr) {
        std::string tail(extra_str);
        free(extra_str);
        if (tail.size() > 1) json += "," + tail.substr(1, tail.size() - 2);
    }
    json += "}";
    return SendJson(req, json);
}

esp_err_t WebServer::WifiScanHandler(httpd_req_t* req) {
    // 学习 artnet：**改为异步启动**（原来阻塞 2~4s，手机上像卡死）。
    // 结果由 GET /api/wifi/scan 轮询取。
    std::string err;
    if (!wifi_manager::StartAsyncScan(&err)) return SendError(req, err, 409);
    return SendJson(req, "{\"status\":\"started\"}");
}

esp_err_t WebServer::WifiScanPollHandler(httpd_req_t* req) {
    std::vector<WifiScanEntry> list;
    bool running = false;
    std::string err;
    if (!wifi_manager::PollScan(&list, &running, &err)) {
        // 还没发起过扫描（界面不会走到这里，防御性返回 200 + idle 便于前端统一处理）
        return SendJson(req, "{\"running\":false,\"done\":false}");
    }
    if (running) return SendJson(req, "{\"running\":true,\"done\":false}");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", false);
    cJSON_AddBoolToObject(root, "done", true);
    cJSON* aps = cJSON_AddArrayToObject(root, "aps");
    for (const auto& e : list) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", e.ssid.c_str());
        cJSON_AddNumberToObject(o, "rssi", e.rssi);
        cJSON_AddBoolToObject(o, "secure", e.secure);
        cJSON_AddNumberToObject(o, "bars", WifiRssiToBars(e.rssi));
        cJSON_AddItemToArray(aps, o);
    }
    cJSON_AddStringToObject(root, "error", err.c_str());
    return SendJson(req, JsonDump(root));
}

// PUT /api/wifi/ap —— 改热点名 / 热点密码（学习 artnet 的"AP 可改" + 用户要求改密码）
// 请求体支持 ap_ssid 和/或 ap_pass，至少给一个；各自独立校验（任一失败 400 带原因）。
// 空串语义：ap_ssid:"" = 复原默认热点名；ap_pass:"" = **复原默认密码**（不是设成开放网络）。
esp_err_t WebServer::WifiApPutHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "JSON 解析失败");
    cJSON* jssid = cJSON_GetObjectItem(root, "ap_ssid");
    cJSON* jpass = cJSON_GetObjectItem(root, "ap_pass");
    const bool has_ssid = cJSON_IsString(jssid) && jssid->valuestring != nullptr;
    const bool has_pass = cJSON_IsString(jpass) && jpass->valuestring != nullptr;
    std::string ap_ssid = has_ssid ? jssid->valuestring : "";
    std::string ap_pass = has_pass ? jpass->valuestring : "";
    cJSON_Delete(root);
    if (!has_ssid && !has_pass) return SendError(req, "缺少 ap_ssid 或 ap_pass");

    struct Ctx {
        bool ok = false;
        std::string err;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx, has_ssid, has_pass, ap_ssid, ap_pass]() -> std::string {
        // ⚠ 不能写成 "先 ssid 再 if(ctx->ok) pass"：只传 ap_pass 时 ok 初值为 false，
        // 密码那步会被跳过并整体判失败（真机测试抓到过）。这里的语义是"全部成功才 ok"。
        if (has_ssid && !wifi_manager::SetApSsid(ap_ssid, &ctx->err)) return "ok";
        if (has_pass && !wifi_manager::SetApPassword(ap_pass, &ctx->err)) return "ok";
        ctx->ok = true;
        return "ok";
    });
    if (!ctx->ok) return SendError(req, ctx->err.empty() ? "热点设置失败" : ctx->err);
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    // ⚠ 绝不回显密码本身（只回"是否自定义"）
    if (has_ssid) cJSON_AddStringToObject(out, "ap_ssid", ap_ssid.c_str());
    if (has_pass) cJSON_AddBoolToObject(out, "ap_pass_custom", !ap_pass.empty());
    return SendJson(req, JsonDump(out));
}

// PUT /api/wifi/ip —— 切静态 IP / DHCP（学习 artnet 的静态 IP 配置）
esp_err_t WebServer::WifiIpPutHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "JSON 解析失败");
    cJSON* jdhcp = cJSON_GetObjectItem(root, "dhcp");
    const bool dhcp = cJSON_IsTrue(jdhcp);
    cJSON* jip = cJSON_GetObjectItem(root, "ip");
    cJSON* jmask = cJSON_GetObjectItem(root, "netmask");
    cJSON* jgw = cJSON_GetObjectItem(root, "gateway");
    std::string ip = cJSON_IsString(jip) && jip->valuestring ? jip->valuestring : "";
    std::string mask = cJSON_IsString(jmask) && jmask->valuestring ? jmask->valuestring : "";
    std::string gw = cJSON_IsString(jgw) && jgw->valuestring ? jgw->valuestring : "";
    cJSON_Delete(root);

    if (!dhcp && (ip.empty() || mask.empty() || gw.empty())) {
        return SendError(req, "静态 IP 需要 ip / netmask / gateway 三个字段（或 dhcp:true）");
    }
    struct Ctx {
        bool ok = false;
        std::string err;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx, dhcp, ip, mask, gw]() -> std::string {
        ctx->ok = dhcp ? wifi_manager::UseDhcp(&ctx->err)
                       : wifi_manager::SetStaticIp(ip, mask, gw, &ctx->err);
        return "ok";
    });
    if (!ctx->ok) return SendError(req, ctx->err.empty() ? "设置失败" : ctx->err);
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "ip_mode", dhcp ? 0 : 1);
    cJSON_AddStringToObject(out, "ip", dhcp ? "" : ip.c_str());
    cJSON_AddStringToObject(out, "netmask", dhcp ? "" : mask.c_str());
    cJSON_AddStringToObject(out, "gateway", dhcp ? "" : gw.c_str());
    return SendJson(req, JsonDump(out));
}

// POST /api/wifi/reconnect —— 用**已保存凭据**重连（断开后界面能自己恢复，
// 否则密码不回显 → 前端无法重连 → "断开"成了单向门，只能靠串口救）
esp_err_t WebServer::WifiReconnectHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    if (!body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) return SendError(req, "JSON 解析失败");
        cJSON_Delete(root);
    }
    struct Ctx {
        bool ok = false;
        std::string err;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx]() -> std::string {
        ctx->ok = wifi_manager::ReconnectSta(&ctx->err);
        return "ok";
    });
    if (!ctx->ok) return SendError(req, ctx->err.empty() ? "重连失败" : ctx->err, 400);
    return SendJson(req, "{\"status\":\"connecting\"}");
}

// POST /api/wifi/disconnect —— 断开 STA 但保留凭据（学习 artnet）
esp_err_t WebServer::WifiDisconnectHandler(httpd_req_t* req) {
    // ⚠ 先校验请求体：本接口**会真的断开网络**；若带着坏 JSON 也照做，既危险
    // （调用方以为参数错会被拒）又与全站"坏 JSON → 400"不一致。
    // 允许空体（正常调用）；非空则必须是合法 JSON。
    std::string body = ReadBody(req);
    if (!body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) return SendError(req, "JSON 解析失败");
        cJSON_Delete(root);
    }
    struct Ctx {
        bool ok = false;
        std::string err;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx]() -> std::string {
        ctx->ok = wifi_manager::DisconnectSta(&ctx->err);
        return "ok";
    });
    if (!ctx->ok) return SendError(req, ctx->err.empty() ? "断开失败" : ctx->err);
    return SendJson(req, "{\"status\":\"disconnected\"}");
}

esp_err_t WebServer::WifiPutHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "JSON 解析失败");
    cJSON* ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON* pass = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring == nullptr) {
        cJSON_Delete(root);
        return SendError(req, "缺少 ssid");
    }
    std::string s_ssid = ssid->valuestring;
    std::string s_pass = (cJSON_IsString(pass) && pass->valuestring) ? pass->valuestring : "";
    cJSON_Delete(root);

    // 纯函数校验（HTTP 任务内）：非法凭据 400 且不落库
    std::string verr = ValidateWifiCredentials(s_ssid, s_pass);
    if (!verr.empty()) return SendError(req, verr);

    // 保存 + 发起连接在主线程（Settings 归主线程所有）
    struct Ctx {
        bool ok = false;
        std::string err;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx, s_ssid, s_pass]() -> std::string {
        ctx->ok = wifi_manager::ApplySta(s_ssid, s_pass, &ctx->err);
        return ctx->ok ? "ok" : "fail";
    });
    if (!ctx->ok) return SendError(req, ctx->err.empty() ? "连接发起失败" : ctx->err, 400);
    return SendJson(req, "{\"status\":\"connecting\"}");
}

esp_err_t WebServer::WifiDeleteHandler(httpd_req_t* req) {
    // 语义（fail-safe）：body {"index":n} 删除单个；body {"all":true} 才清空全部。
    // 空 body / 无 index 一律**不做任何删除** —— 避免一次误请求把网络配置清光
    std::string body = ReadBody(req);
    int index = -1;
    bool wipe_all = false;
    if (!body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* idx = cJSON_GetObjectItem(root, "index");
            if (cJSON_IsNumber(idx)) index = idx->valueint;
            cJSON* all = cJSON_GetObjectItem(root, "all");
            if (cJSON_IsBool(all) && cJSON_IsTrue(all)) wipe_all = true;
            cJSON_Delete(root);
        }
    }
    if (index < 0 && !wipe_all) return SendError(req, "需要提供 index（删除单个）或 all=true（清空）");
    struct Ctx {
        bool ok = false;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx, index, wipe_all]() -> std::string {
        ctx->ok = wipe_all ? wifi_manager::ForgetSta() : wifi_manager::RemoveSaved(index);
        return ctx->ok ? "ok" : "fail";
    });
    if (!ctx->ok) return SendError(req, "删除失败（索引越界？）", 400);
    return SendJson(req, "{\"status\":\"ok\"}");
}

// ============================ 小票块模板 v3（设计 §7.3b）============================
esp_err_t WebServer::ReceiptTemplateGetHandler(httpd_req_t* req) {
    // ?mode=0|1|2 → 读**指定模式**的模板（切换下拉时预览该模式，不改变生效模式）
    int want_mode = -1;
    {
        char q[64] = {0};
        if (httpd_req_get_url_query_len(req) > 0 &&
            httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            char v[8] = {0};
            if (httpd_query_key_value(q, "mode", v, sizeof(v)) == ESP_OK) want_mode = atoi(v);
        }
    }
    auto json = MainSync([want_mode]() -> std::string {
        // 指定模式时按该模式的内置/已存模板返回（§7.3d 按模式分槽）
        ReceiptTemplateV3 t;
        if (want_mode >= 0 && want_mode <= 2 && want_mode != Settings::GetInstance().active_tmpl_mode()) {
            const std::string& slot = Settings::GetInstance().tmpl_slot(want_mode);
            std::string e2;
            if (!slot.empty()) t = ReceiptTemplateV3::FromJson(slot, &e2);
            if (slot.empty() || !e2.empty() || t.blocks.empty()) t = BuiltinTemplate(want_mode == 1 ? 5 : (want_mode == 2 ? 6 : 1));
        } else {
            t = LoadTemplateFromSettings();   // 当前生效模板（含旧 header/footer 迁移结果）
        }
        cJSON* root = cJSON_Parse(t.ToJson().c_str());
        if (root == nullptr) return "{}";
        cJSON_AddStringToObject(root, "shop_name", Settings::GetInstance().shop_name().c_str());
        cJSON_AddBoolToObject(root, "logo_enabled", Settings::GetInstance().logo_enabled());
        return JsonDump(root);
    });
    if (json.empty()) return SendError(req, "主线程繁忙", 503);
    return SendJson(req, json);
}

esp_err_t WebServer::ReceiptTemplatePutHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    // 纯函数校验（HTTP 任务内）：畸形模板必须 400，绝不能进主线程/落库
    std::string err;
    auto t = ReceiptTemplateV3::FromJson(body, &err);
    if (!err.empty()) return SendError(req, err);

    struct Ctx { bool ok = false; };
    auto ctx = std::make_shared<Ctx>();
    MainSync([t, ctx]() -> std::string {
        // §7.3d：存到模板**自己的模式槽**并设为生效模式（切换类型即切换模板）
        if (SaveTemplateToSettings(t, true)) {
            ctx->ok = true;
            return "ok";
        }
        ctx->ok = false;
        return "save_failed";
    });
    if (!ctx->ok) return SendError(req, "保存失败", 500);
    return SendJson(req, "{\"status\":\"saved\"}");
}

esp_err_t WebServer::ReceiptParamsHandler(httpd_req_t* req) {
    // 参数清单 + 示例值：UI 的「插入参数」直接渲染它，避免前后端两套清单漂移
    auto json = MainSync([]() -> std::string {
        ReceiptParams sample = MakeSampleParams();
        cJSON* root = cJSON_CreateObject();
        cJSON* arr = cJSON_AddArrayToObject(root, "params");
        for (const auto& kv : ReceiptParams::List()) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "key", kv.first.c_str());
            cJSON_AddStringToObject(o, "desc", kv.second.c_str());
            std::string val;
            sample.Get(kv.first, &val);
            cJSON_AddStringToObject(o, "sample", val.c_str());
            cJSON_AddItemToArray(arr, o);
        }
        ReceiptBlock def;
        cJSON_AddNumberToObject(root, "default_feed_lines", def.feed_lines);
        cJSON_AddNumberToObject(root, "default_qr_size", def.qr_size);
        cJSON_AddNumberToObject(root, "default_qr_ecc", def.qr_ecc);
        cJSON_AddItemToObject(root, "default_template",
                              cJSON_Parse(ReceiptTemplateV3::Default().ToJson().c_str()));
        return JsonDump(root);
    });
    if (json.empty()) return SendError(req, "主线程繁忙", 503);
    return SendJson(req, json);
}

esp_err_t WebServer::ReceiptTemplateResetHandler(httpd_req_t* req) {
    // body {"mode": 0|1|2} 重置单个；{"mode": -1} 或空 body = 三种全部重置为出厂预设
    std::string body = ReadBody(req);
    int mode = -1;
    if (!body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* m = cJSON_GetObjectItem(root, "mode");
            if (cJSON_IsNumber(m)) mode = m->valueint;
            cJSON_Delete(root);
        }
    }
    if (mode < -1 || mode > 2) return SendError(req, "mode 必须是 -1(全部)/0/1/2");
    struct Ctx {
        bool ok = false;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([mode, ctx]() -> std::string {
        ctx->ok = ResetTemplatesToPreset(mode);
        return ctx->ok ? "ok" : "fail";
    });
    if (!ctx->ok) return SendError(req, "重置失败", 500);
    return SendJson(req, mode == -1 ? "{\"status\":\"reset\",\"mode\":\"all\"}"
                                    : "{\"status\":\"reset\"}");
}

esp_err_t WebServer::ReceiptTemplatePreviewHandler(httpd_req_t* req) {
    // 保存前预览：body = 模板 JSON（可选 "lose":true 看未中奖版式）
    std::string body = ReadBody(req);
    std::string err;
    auto t = ReceiptTemplateV3::FromJson(body, &err);
    if (!err.empty()) return SendError(req, err);

    cJSON* root_in = cJSON_Parse(body.c_str());
    bool lose = false;
    if (root_in != nullptr) {
        cJSON* l = cJSON_GetObjectItem(root_in, "lose");
        if (cJSON_IsBool(l)) lose = cJSON_IsTrue(l);
        cJSON_Delete(root_in);
    }

    auto json = MainSync([t, lose]() -> std::string {
        ReceiptParams p = MakeSampleParams();
        p.is_lose = lose;
        if (lose) p.prize = p.lose_title;
        int unknown = 0;
        std::string text = ReceiptRender::RenderPreviewText(t, p, &unknown);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "text", text.c_str());
        cJSON_AddNumberToObject(root, "unknown_params", unknown);
        cJSON_AddNumberToObject(root, "blocks", (int)t.blocks.size());
        cJSON_AddNumberToObject(root, "paper_cols", t.paper_cols);
        return JsonDump(root);
    });
    if (json.empty()) return SendError(req, "主线程繁忙", 503);
    return SendJson(req, json);
}

// ============================ Logo ============================
esp_err_t WebServer::LogoPostHandler(httpd_req_t* req) {
    std::string body = ReadBody(req);
    LogoInfo info = PrinterLogo::Validate((const uint8_t*)body.data(), body.size());
    if (!info.valid) return SendError(req, info.error ? info.error : "logo 数据非法");

    struct Ctx {
        bool ok = false;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([body, ctx]() -> std::string {
        ctx->ok = LogoStore::Save((const uint8_t*)body.data(), body.size());
        return ctx->ok ? "ok" : "fail";
    });
    if (!ctx->ok) return SendError(req, "logo 写入失败", 500);
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"status\":\"saved\",\"width\":%d,\"height\":%d,\"bytes\":%d}",
             info.width_dots, info.height_dots, info.data_bytes + 8);
    return SendJson(req, msg);
}

esp_err_t WebServer::LogoRawHandler(httpd_req_t* req) {
    // 返回 LGO1 原始字节（前端画缩略图用：让用户看到"现在用的是哪张图"）
    static uint8_t raw[PrinterLogo::kMaxLogoBytes];
    size_t len = 0;
    struct Ctx {
        bool ok = false;
        size_t len = 0;
    };
    auto ctx = std::make_shared<Ctx>();
    MainSync([ctx]() -> std::string {
        ctx->ok = LogoStore::Load(raw, sizeof(raw), &ctx->len);
        return ctx->ok ? "ok" : "fail";
    });
    if (!ctx->ok) return SendJson(req, "{\"error\":\"no logo\"}", 404);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char*)raw, (ssize_t)ctx->len);
}

esp_err_t WebServer::LogoGetHandler(httpd_req_t* req) {
    LogoInfo info = LogoStore::ReadInfo();
    if (!info.valid) return SendJson(req, "{\"exists\":false}", 404);
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"exists\":true,\"width\":%d,\"height\":%d,\"bytes\":%d}",
             info.width_dots, info.height_dots, info.data_bytes + 8);
    return SendJson(req, msg);
}

esp_err_t WebServer::LogoDeleteHandler(httpd_req_t* req) {
    MainSync([]() -> std::string {
        LogoStore::Remove();
        return "ok";
    });
    return SendJson(req, "{\"status\":\"deleted\"}");
}

// ============================ 静态文件 ============================
esp_err_t WebServer::StaticHandler(httpd_req_t* req) {
    const std::string clean = WebPathClean(req->uri);
    if (clean == "/" || clean == "/index.html") return SendFile(req, "/spiffs/index.html");

    std::string path;
    bool exists = false;
    // 路径穿越防护：含 ".." 的请求直接走捕获跳转，绝不 fopen（有单测锁定）
    if (WebPathIsSafe(clean)) {
        path = (clean.rfind("/spiffs/", 0) == 0) ? clean : ("/spiffs" + clean);
        FILE* f = fopen(path.c_str(), "rb");
        if (f != nullptr) {
            fclose(f);
            exists = true;
        }
    }
    if (WebStaticDecide(req->uri, exists) == StaticRouteAction::kServeFile) {
        return SendFile(req, path.c_str());
    }
    // 未知/不安全路径 → 302 跳配置页。捕获门户关键：手机探测的
    // /generate_204（Android）、/hotspot-detect.html（iOS）必须被明确重定向，
    // 系统才会弹「登录网络」面板（返回 200 + 网页有时不触发）
    ESP_LOGI(TAG, "捕获门户：%s → 302 配置页", clean.c_str());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "<html><body>Redirecting…</body></html>", HTTPD_RESP_USE_STRLEN);
}

// ============================ 启动 ============================
bool WebServer::Start() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = kMaxUriHandlers;
    // 浏览器在编辑模板时会连发请求（每敲一键一次预览）。默认 max_open_sockets=7，
    // 连接一多就直接失败；LRU 清理让服务端优先回收最久未用的连接而不是拒绝新连接。
    config.max_open_sockets = 12;
    config.lru_purge_enable = true;
    config.backlog_conn = 12;
    // ⚠️ 关键：ESP-IDF 默认只做「精确匹配」，通配 URI（/*）必须显式启用匹配函数，
    // 否则 /* 被当作字面量 → 所有静态页面 404 "Nothing matches the given URI"（真机踩坑）
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 10240;
    config.lru_purge_enable = true;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败");
        return false;
    }

    // httpd_uri_t 字段顺序：{uri, method, handler, user_ctx}
    static const httpd_uri_t handlers[] = {
        {"/api/draw", HTTP_POST, DrawHandler, nullptr},
        {"/api/lottery/status", HTTP_GET, LotteryStatusHandler, nullptr},
        {"/api/status", HTTP_GET, StatusHandler, nullptr},
        {"/api/printer/test", HTTP_POST, PrinterTestHandler, nullptr},
        {"/api/time", HTTP_POST, TimeSetHandler, nullptr},
        {"/api/wifi", HTTP_GET, WifiGetHandler, nullptr},
        {"/api/wifi", HTTP_PUT, WifiPutHandler, nullptr},
        {"/api/wifi", HTTP_DELETE, WifiDeleteHandler, nullptr},
        {"/api/wifi/scan", HTTP_POST, WifiScanHandler, nullptr},
        {"/api/wifi/scan", HTTP_GET, WifiScanPollHandler, nullptr},
        {"/api/wifi/ap", HTTP_PUT, WifiApPutHandler, nullptr},
        {"/api/wifi/ip", HTTP_PUT, WifiIpPutHandler, nullptr},
        {"/api/wifi/disconnect", HTTP_POST, WifiDisconnectHandler, nullptr},
        {"/api/wifi/reconnect", HTTP_POST, WifiReconnectHandler, nullptr},
        {"/api/prizes", HTTP_GET, PrizesGetHandler, nullptr},
        {"/api/prizes", HTTP_PUT, PrizesPutHandler, nullptr},
        {"/api/prizes/reset-stock", HTTP_POST, PrizesResetStockHandler, nullptr},
        {"/api/pages", HTTP_GET, PagesGetHandler, nullptr},
        {"/api/pages", HTTP_PUT, PagesPutHandler, nullptr},
        {"/api/page-mapping", HTTP_PUT, PageMappingPutHandler, nullptr},
        {"/api/page/goto", HTTP_POST, PageGotoHandler, nullptr},
        {"/api/adjust", HTTP_GET, AdjustGetHandler, nullptr},
        {"/api/adjust", HTTP_PUT, AdjustPutHandler, nullptr},
        {"/api/adjust/reset", HTTP_POST, AdjustResetHandler, nullptr},
        {"/api/receipt", HTTP_GET, ReceiptGetHandler, nullptr},
        {"/api/receipt", HTTP_PUT, ReceiptPutHandler, nullptr},
        {"/api/receipt/preview", HTTP_GET, ReceiptPreviewHandler, nullptr},
        {"/api/receipt/template", HTTP_GET, ReceiptTemplateGetHandler, nullptr},
        {"/api/receipt/template", HTTP_PUT, ReceiptTemplatePutHandler, nullptr},
        {"/api/receipt/template/reset", HTTP_POST, ReceiptTemplateResetHandler, nullptr},
        {"/api/receipt/template/preview", HTTP_POST, ReceiptTemplatePreviewHandler, nullptr},
        {"/api/receipt/params", HTTP_GET, ReceiptParamsHandler, nullptr},
        {"/api/logo", HTTP_POST, LogoPostHandler, nullptr},
        {"/api/logo", HTTP_GET, LogoGetHandler, nullptr},
        {"/api/logo/raw", HTTP_GET, LogoRawHandler, nullptr},
        {"/api/logo", HTTP_DELETE, LogoDeleteHandler, nullptr},
    };
    int registered = 0;
    for (const auto& h : handlers) {
        httpd_uri_t uri = h;
        esp_err_t e = httpd_register_uri_handler(server_, &uri);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败: %s", h.uri, esp_err_to_name(e));
        } else {
            ++registered;
        }
    }
    // 编译期护栏：路由数（API + 静态 / 与 /*）不得超过 max_uri_handlers。
    // 超限时 httpd 只会静默注册失败 → 静态页面全 404，故宁可编译不过。
    constexpr size_t kApiRouteCount = sizeof(handlers) / sizeof(handlers[0]);
    static_assert(kApiRouteCount + 2 <= kMaxUriHandlers,
                  "路由数超过 kMaxUriHandlers：请调大该常量（超限会导致静态通配 /* 注册失败、"
                  "所有页面 404）");
    httpd_uri_t stat_root = {"/", HTTP_GET, StaticHandler, nullptr};
    if (httpd_register_uri_handler(server_, &stat_root) == ESP_OK) ++registered;
    else ESP_LOGE(TAG, "注册 / 失败");
    httpd_uri_t stat_any = {"/*", HTTP_GET, StaticHandler, nullptr};
    if (httpd_register_uri_handler(server_, &stat_any) == ESP_OK) ++registered;
    else ESP_LOGE(TAG, "注册 /* 失败");
    constexpr size_t kExpectedRoutes = kApiRouteCount + 2;
    if (static_cast<size_t>(registered) != kExpectedRoutes) {
        ESP_LOGE(TAG, "路由注册不完整：成功 %d / 应有 %zu —— **静态页面会全部 404**，"
                      "请调大 kMaxUriHandlers（当前 %zu）", registered, kExpectedRoutes,
                 kMaxUriHandlers);
    } else {
        ESP_LOGI(TAG, "路由注册完成：%d/%zu 条", registered, kExpectedRoutes);
    }

    ESP_LOGI(TAG, "WebServer 就绪: http://192.168.4.1/（%zu 个 API 端点，通配匹配已启用）",
             kApiRouteCount);
    return true;
}

WebServer& WebServer::GetInstance() {
    static WebServer instance;
    return instance;
}
