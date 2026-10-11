#!/usr/bin/env python3
"""lucky-page 手机布局离线校验（不需要真机、不需要设备热点）

用法:
    python3 scripts/mobile_layout_check.py [--port 8899] [--shots /tmp/lucky-mobile]

为什么需要它：
    「PC 布局改手机布局」是纯视觉/交互问题，后端单测与 HTTP 验收都看不见；
    而真机冒烟（web_smoke.py）需要设备在线。本脚本用**本地静态服务 + playwright
    拦截 /api/** 返回固定 JSON**，把 6 个页面在手机视口下跑起来，断言：
      1) 无横向溢出（手机最常见的失败：画布/表格撑出屏幕）
      2) 点选画布元素后属性抽屉在视口内可见且可编辑（底部抽屉方案）
      3) 主操作（保存）固定栏在视口内，不用滚动就能点到
      4) 触控拖动前提成立（.el 有 touch-action:none）
      5) 输入框 font-size >= 16px（否则 iOS 聚焦自动放大）
      6) 桌面视口量纲不变（colw=14/lineh=22）——这是 web_smoke.py 拖动断言的前提
"""
import argparse
import functools
import http.server
import json
import pathlib
import socketserver
import sys
import threading

ROOT = pathlib.Path(__file__).resolve().parent.parent
SPIFFS = ROOT / "main" / "assets" / "spiffs_contents"

MOBILE_VP = {"width": 390, "height": 844}          # iPhone 12/13/14 逻辑分辨率
DESKTOP_VP = {"width": 1280, "height": 720}

# ---- 各接口的固定响应（离线 mock；字段与固件返回保持一致，否则页面逻辑走岔） ----
BLOCKS = [
    {"type": "line", "text": "幸运翻页机 小票", "align": "center", "size": 2, "bold": True, "col": 0, "row": -1},
    {"type": "separator", "char": "=", "col": 0, "row": -1},
    {"type": "line", "text": "票号 {ticket}", "align": "left", "size": 1, "col": 0, "row": -1},
    {"type": "qr", "content": "{ticket}{check}", "size": 6, "ecc": 49, "col": 2, "row": -1},
    {"type": "feed", "lines": 4, "col": 0, "row": -1},
    {"type": "logo", "bg": True, "col": 0, "row": -1},
    {"type": "line", "text": "谢谢参与，欢迎再来", "align": "center", "size": 1, "col": 0, "row": -1},
    # ⚠ 必须让画布足够高：真机模板 15 块、画布范围远超一屏，元素会落在
    # 底部抽屉下方（真机反馈的"抽屉盖住编辑区"正是这样暴露的）。
    # mock 太短就复现不出这类问题。
    {"type": "line", "text": "有效期至 {validity}", "align": "left", "size": 1, "col": 0, "row": -1},
    {"type": "line", "text": "地址：某某路 1 号", "align": "left", "size": 1, "col": 0, "row": -1},
    {"type": "line", "text": "电话：400-000-0000", "align": "left", "size": 1, "col": 0, "row": -1},
    {"type": "line", "text": "最终解释权归本店所有", "align": "center", "size": 1, "col": 0, "row": -1},
]
TEMPLATE = {"version": 3, "mode": "flow", "paper_cols": 32, "banner_top_rows": 2,
            "banner_bottom_rows": 3, "blocks": BLOCKS}
PARAMS = {"params": [{"key": "ticket", "desc": "票号"}, {"key": "date", "desc": "日期"},
                     {"key": "time", "desc": "时间"}, {"key": "check", "desc": "校验码"}],
          "default_template": TEMPLATE}
PRIZES = {"total_pages": 30, "depletion_policy": 0, "tiers": [
    {"level": 1, "page": 0, "weight": 1, "probability": 0.01, "quantity": 1, "remaining": 1,
     "name": "一等奖", "note": "大奖", "depleted": False},
    {"level": 2, "page": 5, "weight": 5, "probability": 0.05, "quantity": 5, "remaining": 3,
     "name": "二等奖", "note": "", "depleted": False},
    {"level": 3, "page": 12, "weight": 20, "probability": 0.20, "quantity": 30, "remaining": 30,
     "name": "三等奖", "note": "小奖", "depleted": False},
    {"level": 4, "page": 20, "weight": 74, "probability": 0.74, "quantity": -1, "remaining": 0,
     "name": "谢谢参与", "note": "", "depleted": False},
]}

MOCK = {
    "/api/receipt/template": TEMPLATE,
    "/api/receipt/params": PARAMS,
    "/api/receipt/template/preview": {"text": "幸运翻页机 小票\n================\n票号 0001\n\n[二维码]\n",
                                      "blocks": len(BLOCKS), "paper_cols": 32, "unknown_params": 0},
    "/api/receipt/template/reset": {"ok": True},
    "/api/logo": {"exists": False},
    "/api/logo/raw": None,                       # 二进制，返回空体
    "/api/prizes": PRIZES,
    "/api/prizes/reset-stock": {"ok": True},
    "/api/pages": {"total_pages": 30, "stroke_min_dmx": 0, "stroke_max_dmx": 255},
    "/api/page-mapping": {"values": [round(i * 255 / 29) for i in range(30)]},
    # 标定「页码调整模式」：每页 int8 偏移（0=未调整）+ 基础映射
    "/api/adjust": {"cell_count": 1, "total_pages": 30, "offsets": [[0] * 30],
                    "mapping": [round(i * 255 / 29) for i in range(30)]},
    "/api/adjust/reset": {"ok": True},
    "/api/page/goto": {"ok": True},
    "/api/lottery/status": {"state": "idle", "last_prize": None,
                            "stats": {"total": 0, "ok": 0, "failed": 0}},
    "/api/draw": {"ok": True, "prize": "谢谢参与"},
    "/api/printer/test": {"ok": True},
    "/api/time": {"ok": True},
    "/api/wifi": {"mode": "APSTA", "ap_ssid": "lucky-page-0E6D", "ap_ip": "192.168.4.1",
                  "sta_state": "disconnected", "sta_ssid": "", "saved": [],
                  "ap_up": True, "ap_channel": 6, "ap_clients": 0,
                  "ip_mode": 0, "static_ip": "", "static_mask": "", "static_gw": "",
                  "ap_ssid_custom": False, "ap_pass_custom": False, "user_disconnected": False,
                  "net_reset_count": 1, "net_reset_epoch": 1790663527,
                  "configured": True, "sta_connected": False, "ip": "", "rssi": 0, "bars": 0,
                  "time_synced": True},
    # 异步扫描：POST 启动 → GET 轮询（有状态 mock，与固件同语义）
    "/api/wifi/ap": {"ok": True, "ap_ssid": "lucky-page-0E6D"},
    "/api/wifi/ip": {"ok": True, "ip_mode": 0},
    "/api/wifi/disconnect": {"status": "disconnected"},
    "/api/wifi/reconnect": {"status": "connecting"},
    "/api/wifi/scan": {"aps": [{"ssid": "test-ap", "rssi": -55, "auth": 3}]},
}

passed, failed, shots = [], [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(f"  {'✅' if cond else '❌'} {name}{(' — ' + detail) if detail and not cond else ''}")


def serve(directory: pathlib.Path, port: int):
    """极简静态服务：页面里的 /xxx.html、app.css 直接映射到 spiffs 目录。"""
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(directory))
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    httpd = socketserver.ThreadingTCPServer(("127.0.0.1", port), handler)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def make_router():
    """playwright 路由：/api/** 一律返回 mock JSON，绝不落到网络上。

    行程调整（/api/adjust）用**有状态** mock：与固件同语义（校验 page/offset/cell、
    value = clamp(base+offset, 0, 255)、PUT 后 GET 能读回），否则"微调→读回→还原"
    这类真实交互在离线阶段根本验不出来（铁律 §四-18：mock 必须含真实特征）。
    """
    offsets = [0] * 30                      # 单机：cell 0 的 30 页偏移
    SCAN = {"started": False}                # 异步扫描状态（POST 启动 / GET 轮询）
    APST = {"pass_custom": False}             # 热点密码是否自定义（有状态 mock，与固件同语义）

    def route(rhandler):
        req = rhandler.request
        path = "/api/" + req.url.split("?")[0].split("/api/", 1)[1]
        try:
            payload = json.loads(req.post_data) if req.post_data else {}
        except Exception:                                     # noqa: BLE001
            payload = None

        if path == "/api/adjust":
            if req.method == "GET":
                body = {"cell_count": 1, "total_pages": 30,
                        "offsets": [list(offsets)],
                        "mapping": MOCK["/api/adjust"]["mapping"]}
            elif payload is None:
                rhandler.fulfill(status=400, content_type="application/json",
                                 body=json.dumps({"error": "JSON 解析失败"}).encode())
                return
            else:
                page, off = payload.get("page"), payload.get("offset")
                cell = payload.get("cell", 0)
                if cell != 0 or not isinstance(page, int) or not 0 <= page < 30:
                    rhandler.fulfill(status=400, content_type="application/json",
                                     body=json.dumps({"error": "cell/page 越界"}).encode())
                    return
                if not isinstance(off, int) or not -128 <= off <= 127:
                    rhandler.fulfill(status=400, content_type="application/json",
                                     body=json.dumps({"error": "offset 越界"}).encode())
                    return
                offsets[page] = off
                base = MOCK["/api/adjust"]["mapping"][page]
                body = {"ok": True, "cell": 0, "page": page, "offset": off, "base": base,
                        "value": max(0, min(255, base + off))}
        elif path == "/api/adjust/reset":
            if payload is None:
                rhandler.fulfill(status=400, content_type="application/json",
                                 body=json.dumps({"error": "JSON 解析失败"}).encode())
                return
            page = payload.get("page", -1)
            if page == -1:
                for i in range(len(offsets)):
                    offsets[i] = 0
                cleared = len(offsets)
            elif isinstance(page, int) and 0 <= page < len(offsets):
                offsets[page] = 0
                cleared = 1
            else:
                rhandler.fulfill(status=400, content_type="application/json",
                                 body=json.dumps({"error": "page 越界"}).encode())
                return
            body = {"ok": True, "cleared": cleared}
        elif path == "/api/wifi/ap":
            # 与固件同语义：ap_pass 空串 = 复原默认；响应绝不回显密码
            if req.method == "PUT":
                sent = payload or {}
                if "ap_pass" in sent:
                    APST["pass_custom"] = bool(sent.get("ap_pass"))
                body = {"ok": True}
                if "ap_ssid" in sent:
                    body["ap_ssid"] = sent["ap_ssid"] or "lucky-page-0E6D"
                if "ap_pass" in sent:
                    body["ap_pass_custom"] = APST["pass_custom"]
            else:
                body = MOCK["/api/wifi/ap"]
        elif path == "/api/wifi/scan":
            # 第一次 POST 返回 started，随后 GET 返回完成结果（模拟异步扫描）
            if req.method == "POST":
                SCAN["started"] = True
                body = {"status": "started"}
            else:
                body = ({"running": False, "done": True,
                         "aps": [{"ssid": "MyShop-5G", "rssi": -52, "secure": True, "bars": 4},
                                 {"ssid": "FreeWiFi", "rssi": -70, "secure": False, "bars": 2}],
                         "error": ""} if SCAN.get("started")
                        else {"running": False, "done": False})
        elif req.method in ("PUT", "POST") and path not in MOCK:
            body = {"ok": True}
        else:
            body = MOCK.get(path, {"ok": True})
            if path == "/api/wifi" and isinstance(body, dict):
                body = dict(body)
                # 有状态：改过密码后 GET 要反映"已自定义"（与固件同语义）
                body["ap_pass_custom"] = APST["pass_custom"]

        if body is None:
            rhandler.fulfill(status=200, content_type="application/octet-stream", body=b"")
            return
        rhandler.fulfill(status=200, content_type="application/json",
                         body=json.dumps(body, ensure_ascii=False).encode())
    return route


def box_in_view(pg, selector):
    """返回元素相对视口的矩形；元素不存在返回 None（缺失即判失败，不抛异常）。

    视口高用 documentElement.clientHeight（=390×844 真实视口），
    不能用 window.innerHeight —— is_mobile 模拟下它会被 shrink-to-fit 放大到 1295。
    """
    try:
        return pg.eval_on_selector(selector, "e=>{const r=e.getBoundingClientRect();"
                                            "return {top:r.top,bottom:r.bottom,"
                                            "h:document.documentElement.clientHeight};}")
    except Exception:
        return None


def try_eval(pg, expr):
    """求值失败返回 None（页面尚未实现该控件时不要炸掉整轮校验）。"""
    try:
        return pg.evaluate(expr)
    except Exception:
        return None


def clickable_el_index(pg):
    """找一个中心点确实能点到的画布元素。

    真实模板里元素会重叠（钉在同一行的两个块），直接点 `.el[0]` 会被
    上层元素拦截 —— 离线 mock 太简单看不出来，真机回归才暴露。
    """
    return pg.evaluate("""()=>{
        const els=[...document.querySelectorAll('.el')];
        for(let i=0;i<els.length;i++){
            const r=els[i].getBoundingClientRect();
            if(r.width<4||r.height<4) continue;
            const x=r.left+Math.min(20,r.width/2), y=r.top+r.height/2;
            const hit=document.elementFromPoint(x,y);
            if(hit===els[i]||els[i].contains(hit)) return i;
        }
        return -1;
    }""")


def jittery_tap(pg, x, y, dx=2, dy=1):
    """模拟真手指轻点：必然带 1~3px 抖动（会产生 pointermove）。

    ⚠ 这是本项目的真实踩坑：`touchscreen.tap()` 不产生中间 move，于是
    "任何 pointermove 都算拖动"的实现能通过自动化测试，却让真机轻点永远弹不出属性抽屉。
    """
    pg.mouse.move(x, y)
    pg.mouse.down()
    pg.mouse.move(x + dx, y + dy, steps=2)
    pg.mouse.up()


def overflow(pg):
    """横向溢出量纲。

    ⚠ 必须用 body.clientWidth 作为基准，不能用 window.innerWidth：
    手机浏览器在内容超宽时会 shrink-to-fit **缩小视觉视口**（innerWidth/innerHeight
    反而变大：实测 598×1295，而真实视口是 390×844），于是"页面已经溢出"却看起来不溢出。
    故一律用 documentElement.clientWidth 作为基准。
    """
    return pg.evaluate("()=>({sw:document.documentElement.scrollWidth,"
                       " iw:document.documentElement.clientWidth})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--shots", default="/tmp/lucky-mobile")
    args = ap.parse_args()
    shots_dir = pathlib.Path(args.shots)
    shots_dir.mkdir(parents=True, exist_ok=True)

    from playwright.sync_api import sync_playwright

    httpd = serve(SPIFFS, args.port)
    base = f"http://127.0.0.1:{args.port}"
    pages = ["/index.html", "/prizes.html", "/receipt.html",
             "/logo.html", "/calibrate.html", "/wifi.html"]

    with sync_playwright() as pw:
        browser = pw.chromium.launch()

        # ---------------- ① 手机视口：6 页均无横向溢出 ----------------
        print("① 手机视口 390×844：无横向溢出（画布/表格不得撑出屏幕）")
        ctx = browser.new_context(viewport=MOBILE_VP, has_touch=True, is_mobile=True,
                                  device_scale_factor=2)
        ctx.route("**/api/**", make_router())
        nav_labels, nav_on_bg = [], []
        for path in pages:
            pg = ctx.new_page()
            perrs = []
            pg.on("pageerror", lambda e: perrs.append(str(e)))
            pg.goto(base + path, wait_until="load", timeout=20000)
            pg.wait_for_timeout(1200)
            # 页面自诊断条（window.onerror 会显示在 #errbar）：有内容=前端出错了
            bar = ""
            if pg.query_selector("#errbar"):
                bar = pg.eval_on_selector("#errbar", "e=>e.textContent") or ""
            check(f"{path} 无脚本异常", not perrs and not bar,
                  (perrs[0] if perrs else bar)[:110])
            o = overflow(pg)
            check(f"{path} 无横向溢出", o["sw"] <= o["iw"] + 1, f"scrollWidth={o['sw']} > 视口 {o['iw']}")
            # 触控尺寸审计：可交互控件的实际高度必须够手指点（≥40px，含 44px 目标留余量）
            small = pg.evaluate("""()=>{
                const sel='nav a, button, input:not([type=checkbox]):not([type=radio]), select, textarea, .ap, .psel div, .tbl-cards td a';
                const bad=[];
                document.querySelectorAll(sel).forEach(e=>{
                    const r=e.getBoundingClientRect();
                    if(r.width<1||r.height<1) return;              // 隐藏元素不算
                    if(getComputedStyle(e).display==='none') return;
                    if(r.height < 40) bad.push({t:e.tagName+(e.id?'#'+e.id:'')+(e.className?'.'+String(e.className).split(' ')[0]:''),
                                                h:Math.round(r.height)});
                });
                return bad.slice(0,6);
            }""")
            check(f"{path} 触控控件高度 ≥40px", not small,
                  f"{len(small)} 个偏小：{small}")

            # 顶部菜单一致性（用户反馈"选中风格不一致"）：
            # 每页必须**恰好一个**菜单项高亮、且指向本页；菜单单行不折行；
            # 高亮底色与未选中底色必须不同（否则"选中"看不出来）。
            navinfo = pg.evaluate("""()=>{
                const as=[...document.querySelectorAll('nav a')];
                const on=as.filter(a=>a.classList.contains('on'));
                const off=as.find(a=>!a.classList.contains('on'));
                const rows=new Set(as.map(a=>Math.round(a.getBoundingClientRect().top))).size;
                return {labels:as.map(a=>a.textContent.trim()),
                        onHrefs:on.map(a=>a.getAttribute('href')), onCount:on.length, rows:rows,
                        onBg:on[0]?getComputedStyle(on[0]).backgroundColor:null,
                        offBg:off?getComputedStyle(off).backgroundColor:null};
            }""")
            check(f"{path} 菜单恰好高亮 1 项且指向本页",
                  navinfo["onCount"] == 1 and navinfo["onHrefs"][0] == path,
                  f"高亮数={navinfo['onCount']} 指向={navinfo['onHrefs']}")
            check(f"{path} 菜单单行不折行", navinfo["rows"] == 1, f"{navinfo['rows']} 行")
            check(f"{path} 高亮与未选中底色不同", navinfo["onBg"] != navinfo["offBg"],
                  f"选中={navinfo['onBg']} 未选中={navinfo['offBg']}")
            nav_labels.append(tuple(navinfo["labels"]))
            nav_on_bg.append(navinfo["onBg"])

            pg.screenshot(path=str(shots_dir / f"mobile{path.replace('/', '-')}.png"), full_page=True)
            shots.append(shots_dir / f"mobile{path.replace('/', '-')}.png")
            pg.close()

        # 跨页比对：标签序列与"选中底色"必须全站一致（这是"风格一致"的核心）
        check("6 页菜单文案序列完全一致", len(set(nav_labels)) == 1,
              f"出现 {len(set(nav_labels))} 种：{sorted(set(nav_labels))}")
        check("6 页菜单选中底色完全一致", len(set(nav_on_bg)) == 1,
              f"出现 {len(set(nav_on_bg))} 种：{sorted(set(nav_on_bg))}")

        # ---------------- ② 手机视口：编辑页抽屉 + 操作栏 ----------------
        print("\n② 手机视口：点选元素 → 属性抽屉可见可编辑；主操作固定可及")
        pg = ctx.new_page()
        pg.goto(base + "/receipt.html", wait_until="load", timeout=20000)
        pg.wait_for_timeout(1800)

        n_el = pg.eval_on_selector_all(".el", "e=>e.length")
        check(f"元素渲染（{n_el} 个）", n_el > 0, "画布没有元素")

        # 画布必须适配视口宽度（不靠横向滚动）；同时网格列不得被内容撑爆
        geo = pg.evaluate("""()=>{
            const cv=document.querySelector('#canvas'), st=document.querySelector('#stage'),
                  ru=document.querySelector('#ruler');
            return {cv:cv.getBoundingClientRect().width, st:st.clientWidth,
                    ru:ru?ru.scrollWidth:0, body:document.documentElement.clientWidth};
        }""")
        check("画布适配屏幕宽度（无需横向滚动）", geo["cv"] <= geo["body"] + 1,
              f"canvas={geo['cv']:.0f}px > 屏幕={geo['body']}px")
        check("画布容器未被内容撑爆", geo["st"] <= geo["body"] + 1,
              f"stage={geo['st']}px > body={geo['body']}px（网格 min-width 撑爆）")
        check("标尺未撑出屏幕", geo["ru"] <= geo["body"] + 1,
              f"ruler={geo['ru']}px > body={geo['body']}px")

        # 点选第一个可点到元素 → 抽屉展开（用触摸点击，贴近手机实际）
        els = pg.query_selector_all(".el")
        idx = clickable_el_index(pg)
        if idx is not None and idx >= 0:
            bx = els[idx].bounding_box()
            pg.touchscreen.tap(bx["x"] + min(20, bx["width"] / 2), bx["y"] + bx["height"] / 2)
        else:
            els[0].click(force=True)
        pg.wait_for_timeout(500)
        vis = pg.eval_on_selector("#pf-text", "e=>{const r=e.getBoundingClientRect();"
                                             "return {top:r.top,bottom:r.bottom,h:window.innerHeight};}")
        check("属性抽屉在视口内可见", 0 <= vis["top"] and vis["bottom"] <= vis["h"] + 1,
              f"#pf-text top={vis['top']:.0f} bottom={vis['bottom']:.0f} 视口高={vis['h']}")
        pg.fill("#pf-text", "手机端编辑测试")
        pg.wait_for_timeout(300)
        check("抽屉内可正常输入",
              pg.evaluate("blocks[0].text") == "手机端编辑测试", "输入未写入 blocks")

        # ⚠ 真机反馈的核心问题：点选元素后，从底部弹出的抽屉**盖住画布编辑区**，
        # 用户看不到自己在编辑哪个元素（实测：抽屉顶边 y=339，而画布元素从 y=507 起 →
        # 整个画布被盖住）。这里锁定不变量：选中元素必须落在抽屉上方的可见带内。
        ov = pg.evaluate("""()=>{
            const el=document.querySelector('.el.sel'), p=document.querySelector('#panels');
            if(!el||!p) return null;
            const er=el.getBoundingClientRect();
            const b=parseFloat(getComputedStyle(p).bottom)||0;
            const sheetTop=document.documentElement.clientHeight-b-p.offsetHeight;
            return {selTop:Math.round(er.top), selBottom:Math.round(er.bottom),
                    sheetTop:Math.round(sheetTop), vh:document.documentElement.clientHeight,
                    scrollY:Math.round(window.scrollY),
                    open:document.body.classList.contains('sheet-open')};
        }""")
        check("选中元素不被属性抽屉遮挡（编辑区可见）",
              bool(ov and ov.get("open") and ov.get("selTop", -1) >= 0
                   and ov.get("selBottom", 1e9) <= ov.get("sheetTop", -1) + 2),
              f"元素 {ov and ov.get('selTop')}~{ov and ov.get('selBottom')} "
              f"抽屉顶 {ov and ov.get('sheetTop')}（被遮挡）")
        pg.screenshot(path=str(shots_dir / "mobile-receipt-sheet.png"), full_page=False)
        shots.append(shots_dir / "mobile-receipt-sheet.png")

        # ⚠ 真手指轻点必然带抖动：必须仍然弹出属性抽屉
        # （真机反馈"点击元素没看到属性面板"就是这个 —— 抖动被误判成拖动）
        pg.evaluate("sel=-1; paint(); paintProps(); closeSheet();")
        pg.wait_for_timeout(400)
        els2 = pg.query_selector_all(".el")
        idx2 = clickable_el_index(pg)
        tapx = tapy = None
        if idx2 is not None and idx2 >= 0:
            bx2 = els2[idx2].bounding_box()
            tapx, tapy = bx2["x"] + min(20, bx2["width"] / 2), bx2["y"] + bx2["height"] / 2
            jittery_tap(pg, tapx, tapy)                       # ← 带 2px 抖动
        pg.wait_for_timeout(900)
        jit = pg.evaluate("""()=>({open:document.body.classList.contains('sheet-open'),
                                   sel:!!document.querySelector('.el.sel')})""")
        check("带抖动的真实轻点仍弹出属性抽屉", bool(jit.get("open") and jit.get("sel")),
              f"open={jit.get('open')} 选中={jit.get('sel')}（抖动被误判成拖动）")

        # 底栏「属性」保底入口：收起后仍能一键打开（不依赖手势识别）
        if jit.get("open"):
            pg.click("#propsBtn")
            pg.wait_for_timeout(500)
            closed = pg.evaluate("!document.body.classList.contains('sheet-open')")
            pg.click("#propsBtn")
            pg.wait_for_timeout(500)
            reopened = pg.evaluate("document.body.classList.contains('sheet-open')")
            check("底栏「属性」按钮可收起/再打开面板", bool(closed and reopened),
                  f"收起={closed} 再打开={reopened}")

        # 保存按钮（主操作）必须在视口内，无需滚动
        box = box_in_view(pg, "text=保存模板")
        check("「保存模板」固定在视口内",
              box is not None and 0 <= box["top"] and box["bottom"] <= box["h"] + 1,
              "找不到按钮" if box is None else
              f"top={box['top']:.0f} bottom={box['bottom']:.0f} 视口高={box['h']}")

        # 触控拖动前提：元素必须禁用浏览器手势
        ta = pg.eval_on_selector(".el", "e=>getComputedStyle(e).touchAction")
        check("元素禁用浏览器手势（touch-action:none）", ta == "none", f"touch-action={ta}")

        # iOS 聚焦不放大
        fs = pg.eval_on_selector("input,textarea", "e=>parseFloat(getComputedStyle(e).fontSize)")
        check("输入框字号 ≥16px（防 iOS 聚焦缩放）", fs >= 16, f"font-size={fs}px")

        # 缩放控件：手机端默认已适应宽度；先放大（允许溢出）再点「适应宽度」收回
        has_zoom = try_eval(pg, "()=>({fit:!!document.querySelector('#fitBtn'),"
                                "zin:!!document.querySelector('#zoomIn')})")
        check("提供缩放控件（适应宽度 / 放大）",
              bool(has_zoom and has_zoom.get("fit") and has_zoom.get("zin")),
              f"控件={has_zoom}")
        if has_zoom and has_zoom.get("fit") and has_zoom.get("zin"):
            colw_fit = pg.evaluate("colw")
            pg.click("#zoomIn")
            pg.wait_for_timeout(300)
            colw_big = pg.evaluate("colw")
            check("「＋」可放大（列宽变大）", colw_big > colw_fit, f"colw {colw_fit} → {colw_big}")
            pg.click("#fitBtn")
            pg.wait_for_timeout(400)
            colw_back = pg.evaluate("colw")
            check("「适应宽度」收回并可容纳", colw_back <= colw_big, f"colw {colw_big} → {colw_back}")
            o = overflow(pg)
            check("适配后无横向溢出", o["sw"] <= o["iw"] + 1, f"scrollWidth={o['sw']} 视口={o['iw']}")
            # 缩放百分比提示与实际一致
            zt = pg.eval_on_selector("#zoomTxt", "e=>e.textContent")
            check("缩放百分比显示正确", zt == f"{round(colw_back / 14 * 100)}%", f"显示 {zt}")
        pg.close()

        # ---------------- ②c WiFi 页：学习 artnet 的进阶能力 ----------------
        print("\n②c WiFi 页：改热点名 / 静态 IP / 断开 / 异步扫描 + 一键连接")
        wpg = ctx.new_page()
        werrs = []
        wpg.on("pageerror", lambda e: werrs.append(str(e)))
        wpg.goto(base + "/wifi.html", wait_until="load", timeout=20000)
        wpg.wait_for_timeout(1500)

        ctl = try_eval(wpg, """()=>{
            const ids=['apEditBtn','ipEditBtn','discBtn','reconBtn','apSsid','apSave','apReset',
                       'stIp','stMask','stGw','ipSave','ipDhcp','scanBtn','ipModeTag'];
            return ids.filter(i=>!document.getElementById(i));
        }""")
        check("WiFi 页进阶控件齐全（改名/IP/断开/扫描）",
              isinstance(ctl, list) and not ctl, f"缺失：{ctl}")

        # 扫描**前**的空状态（artnet 布局特征：居中提示「点击上方按钮扫描附近 WiFi」）
        empty_state = try_eval(wpg, """()=>{
            const el=document.getElementById('aplist');
            return {cls: el.className, txt: el.textContent.trim()};
        }""")
        check("artnet 布局：扫描前为居中空状态提示",
              bool(empty_state and "empty-hint" in empty_state["cls"]
                   and "扫描" in empty_state["txt"]), f"{empty_state}")

        # 异步扫描：点扫描 → POST 启动 → 轮询 GET → 出列表（每行带「连接」按钮）
        wpg.click("#scanBtn")
        wpg.wait_for_timeout(2500)
        scan = try_eval(wpg, """()=>{
            const rows=[...document.querySelectorAll('#aplist .ap')];
            // 「连接」按钮在 .ap-act 里（artnet 布局：名称/信号一行，密码+连接一行）
            const btnTxt=[...document.querySelectorAll('#aplist .ap-act button')]
                          .filter(b=>b.textContent.trim()==='连接').length;
            return {n:rows.length, btns:btnTxt,
                    pws:document.querySelectorAll('#aplist .pw').length,
                    msg:(document.getElementById('scmsg')||{}).textContent||''};
        }""")
        check("异步扫描轮询出结果（列表 + 每行「连接」按钮）",
              bool(scan and scan.get("n", 0) >= 2 and scan.get("btns", 0) >= 2), f"{scan}")
        check("加密网络有内联密码框（一键连接前提）",
              bool(scan and scan.get("pws", 0) >= 1), f"密码框数={scan and scan.get('pws')}")

        # 静态 IP：切到静态（mock 回 ip_mode=1）后模式标签应变化
        wpg.click("#ipEditBtn"); wpg.wait_for_timeout(300)
        wpg.fill("#stIp", "192.168.1.100"); wpg.fill("#stMask", "255.255.255.0")
        wpg.fill("#stGw", "192.168.1.1")
        wpg.wait_for_timeout(200)
        check("静态 IP 编辑区可输入", True)

        # AP 密码：状态行 + 内联编辑 + 保存（mock 有状态：改完 GET 应显示"已自定义"）
        pass_row = try_eval(wpg, """()=>({
            btn: !!document.getElementById('apPassEditBtn'),
            state: (document.getElementById('apPassState')||{}).textContent||'',
            hint: (document.getElementById('apHintSsid')?1:0)
        })""")
        check("AP 密码状态行与「修改」按钮存在", bool(pass_row and pass_row["btn"]), f"{pass_row}")
        check("AP 密码默认显示 12345678（未自定义时）",
              bool(pass_row and "12345678" in pass_row["state"]), f"{pass_row and pass_row['state']!r}")
        wpg.click("#apPassEditBtn"); wpg.wait_for_timeout(300)
        wpg.fill("#apPass", "my-shop-pass-2026"); wpg.wait_for_timeout(200)
        wpg.click("#apPassSave"); wpg.wait_for_timeout(900)
        after = try_eval(wpg, """()=>({
            state: (document.getElementById('apPassState')||{}).textContent||'',
            msg: (document.getElementById('msg')||{}).textContent||'',
            body: document.body.innerText
        })""")
        check("改 AP 密码走通（PUT /api/wifi/ap）",
              bool(after and "已自定义" in after["state"]), f"{after and after['state']!r}")
        check("⭐ 页面不显示自定义密码明文（绝不回显）",
              bool(after and "my-shop-pass-2026" not in after["body"]), "页面上出现了明文密码！")

        # 改热点名：填名字保存 → 提示成功（mock 返回 ap_ssid）
        wpg.click("#apEditBtn"); wpg.wait_for_timeout(300)
        wpg.fill("#apSsid", "lucky-page-TEST"); wpg.wait_for_timeout(200)
        wpg.click("#apSave"); wpg.wait_for_timeout(800)
        msg = wpg.eval_on_selector("#msg", "e=>e.textContent") or ""
        check("改热点名走通（PUT /api/wifi/ap）", "热点名已改" in msg, msg[:90])

        # ---- artnet 布局不变量（用户明确偏好这套布局，用断言锁住，防止回归成"卡片+标签网格"）----
        lay = try_eval(wpg, """()=>{
            const row=document.querySelector('.status-row');
            const lab=row && row.querySelector('.status-label');
            const val=row && row.querySelector('.status-value');
            const rb=row.getBoundingClientRect();
            const card=document.querySelector('.card');
            const title=document.querySelector('.card-title');
            const scan=document.getElementById('scanBtn');
            const sb=scan.getBoundingClientRect();
            const cb=card.getBoundingClientRect();
            const h1=document.querySelector('h1');
            return {hasRow:!!row, rowWrap:!!(lab&&val),
                    labelLeft: lab? (lab.getBoundingClientRect().left - rb.left) : -1,
                    valueRight: val? (rb.right - val.getBoundingClientRect().right) : -1,
                    valWeight: val? getComputedStyle(val).fontWeight : '',
                    labelColor: lab? getComputedStyle(lab).color : '',
                    titleBorder: title? getComputedStyle(title).borderBottomWidth : '',
                    scanFull: sb.width, cardW: cb.width,
                    h1: h1? h1.textContent.trim() : '',
                    emptyHint: (document.getElementById('aplist')||{}).className||''};
        }""")
        check("artnet 布局：状态行「标签左 / 值右」",
              bool(lay and lay["rowWrap"] and lay["labelLeft"] < 4 and lay["valueRight"] < 4), f"{lay}")
        check("artnet 布局：值加粗、标签用次要色",
              bool(lay and int(lay["valWeight"] or 0) >= 600 and lay["labelColor"] not in ("", "rgb(238, 238, 238)")),
              f"weight={lay and lay['valWeight']} color={lay and lay['labelColor']}")
        check("artnet 布局：卡片标题带下边框分隔", bool(lay and lay["titleBorder"] not in ("0px", "")),
              f"borderWidth={lay and lay['titleBorder']}")
        check("artnet 布局：主操作按钮通栏",
              bool(lay and lay["cardW"] and abs(lay["scanFull"] - (lay["cardW"] - 32)) < 3),
              f"按钮宽={lay and lay['scanFull']:.0f} 卡片内容宽={lay and (lay['cardW']-32):.0f}")
        check("artnet 布局：页标题存在", bool(lay and "网络设置" in lay["h1"]), f"{lay and lay['h1']!r}")
        check("artnet 布局：扫描后面板渲染出网络行（不再是提示）",
              bool(lay and "empty-hint" not in lay["emptyHint"]), f"className={lay and lay['emptyHint']!r}")
        check("WiFi 页宽度与其它页一致（760px）",
              wpg.evaluate("getComputedStyle(document.body).maxWidth") == "760px",
              wpg.evaluate("getComputedStyle(document.body).maxWidth"))
        hint_txt = (wpg.eval_on_selector("#resetHint", "e=>e.textContent") or "") if \
            try_eval(wpg, "()=>!!document.getElementById('resetHint')") else ""
        check("WiFi 页说明「长按恢复按钮 5 秒」", "长按" in hint_txt and "5" in hint_txt, f"{hint_txt[:60]!r}")
        check("说明里明确「不会清掉奖池/模板/标定」",
              all(k in hint_txt for k in ("奖池", "模板", "标定")), f"{hint_txt[:90]!r}")
        ev = (wpg.eval_on_selector("#resetEvidence", "e=>e.textContent") or "") if \
            try_eval(wpg, "()=>!!document.getElementById('resetEvidence')") else ""
        check("显示网络复位证据（次数 + 时间）",
              "1 次" in ev and ("2026" in ev or ":" in ev), f"{ev[:80]!r}")
        check("证据里给出排查线索", "排查" in ev or "若非你主动" in ev, f"{ev[:80]!r}")
        check("WiFi 页无脚本异常", not werrs, werrs[0][:110] if werrs else "")
        wpg.screenshot(path=str(shots_dir / "mobile-wifi.html.png"), full_page=True)
        shots.append(shots_dir / "mobile-wifi.html.png")
        wpg.close()

        # ---------------- ②b 标定页：页码调整模式 ----------------
        print("\n②b 标定页：调整模式（状态卡 / ±微调 / 翻页 / 清零 / 折叠高级表格）")
        cpg = ctx.new_page()
        cpg.goto(base + "/calibrate.html", wait_until="load", timeout=20000)
        cpg.wait_for_timeout(1500)

        card = try_eval(cpg, """()=>{
            const q=id=>document.getElementById(id);
            const txt=id=>{const e=q(id); return e?e.textContent.trim():null;};
            return {cur:txt('stCur'), base:txt('stBase'), off:txt('stOff'), act:txt('stAct'),
                    has:!!(q('stCur')&&q('stBase')&&q('stOff')&&q('stAct'))};
        }""")
        check("状态卡四项（当前页/基础值/偏移/实际值）",
              bool(card and card.get("has")), f"{card}")

        btns = try_eval(cpg, """()=>{
            const ids=['adj_p10','adj_p5','adj_p1','adj_m1','adj_m5','adj_m10',
                       'navPrev','navNext','navFirst','navLast','jumpPage','jumpBtn',
                       'clrPage','clrAll'];
            return ids.filter(i=>!document.getElementById(i));
        }""")
        check("微调/翻页/跳页/清零控件齐全", isinstance(btns, list) and not btns,
              f"缺失：{btns}")

        adv = try_eval(cpg, "()=>{const d=document.getElementById('advBase');"
                            "return d?d.open:null;}")
        check("高级基础映射表格默认折叠", adv is False, f"details.open={adv}")

        # 点 +5 → 状态卡偏移与实际值应按服务端返回更新（value = clamp(base+offset)）
        def read_card(p):
            return p.evaluate("""()=>{
                const t=id=>{const e=document.getElementById(id);
                              return e?(parseInt(e.textContent,10)||0):null;};
                return {off:t('stOff'), act:t('stAct'), base:t('stBase')};
            }""")
        before = read_card(cpg)
        if None in (before.get("off"), before.get("act"), before.get("base")):
            check("状态卡数值可读（用于微调断言）", False, f"{before}")
        else:
            cpg.click("#adj_p5")
            cpg.wait_for_timeout(800)
            after = read_card(cpg)
            check("点「+5」后偏移 +5 且实际值 = 基础值+偏移",
                  after["off"] == before["off"] + 5
                  and after["act"] == after["base"] + after["off"],
                  f"{before} → {after}")
            cpg.click("#adj_m5")                       # 还原，避免污染本地 mock 状态
            cpg.wait_for_timeout(600)
            back = read_card(cpg)
            check("点「-5」可还原偏移", back["off"] == before["off"],
                  f"{after['off']} → {back['off']}")

        cpg.screenshot(path=str(shots_dir / "mobile-calibrate.html.png"), full_page=True)
        shots.append(shots_dir / "mobile-calibrate.html.png")
        cpg.close()

        # ---------------- ③ 手机视口：奖池页表格转卡片 ----------------        print("\n③ 手机视口：奖池页宽表转卡片（字段标签可见、输入框可点）")
        pg = ctx.new_page()
        pg.goto(base + "/prizes.html", wait_until="load", timeout=20000)
        pg.wait_for_timeout(1500)
        info = pg.evaluate("""()=>{
            const tr=document.querySelector('#tiers tr[data-tier]');
            if(!tr) return {ok:false,why:'无数据行'};
            const td=tr.querySelector('td');
            const cs=getComputedStyle(tr), bs=getComputedStyle(td,'::before');
            const inp=tr.querySelector('input');
            return {ok:true, display:cs.display, label:bs.content,
                    inpW:inp?inp.getBoundingClientRect().width:0,
                    hdrHidden:getComputedStyle(document.querySelector('#tiers .hdr')).display};
        }""")
        check("表格在手机端已卡片化（数据行 display:block）",
              info.get("ok") and info.get("display") == "block", f"{info}")
        check("卡片字段显示列名标签（td::before=data-label）",
              "等级" in str(info.get("label", "")), f"label={info.get('label')}")
        check("表头行在手机端隐藏", info.get("hdrHidden") == "none", f"display={info.get('hdrHidden')}")
        check("卡片内输入框足够宽（≥120px）", info.get("inpW", 0) >= 120,
              f"输入框宽 {info.get('inpW', 0):.0f}px")
        # 字段仍可编辑（不能因为卡片化丢了 data-f 绑定）
        pg.fill('#tiers tr[data-tier] input[data-f="name"]', "手机改的名字")
        pg.wait_for_timeout(200)
        nm = pg.eval_on_selector('#tiers tr[data-tier] input[data-f="name"]', "e=>e.value")
        check("卡片内字段可编辑", nm == "手机改的名字", f"值={nm}")
        pg.screenshot(path=str(shots_dir / "mobile-prizes-cards.png"), full_page=True)
        shots.append(shots_dir / "mobile-prizes-cards.png")
        pg.close()
        ctx.close()

        # ---------------- ④ 桌面视口：量纲不变 + 风格一致性 ----------------
        print("\n④ 桌面视口 1280×720：布局与拖动量纲保持不变")
        dctx = browser.new_context(viewport=DESKTOP_VP)
        dctx.route("**/api/**", make_router())
        pg = dctx.new_page()
        pg.goto(base + "/receipt.html", wait_until="load", timeout=20000)
        pg.wait_for_timeout(1800)
        dims = pg.evaluate("()=>({colw:colw, lineh:lineh})")
        check("桌面拖动量纲 colw=14 / lineh=22 不变",
              dims["colw"] == 14 and dims["lineh"] == 22, f"colw={dims['colw']} lineh={dims['lineh']}")
        o = overflow(pg)
        check("桌面视口无横向溢出", o["sw"] <= o["iw"] + 1, f"scrollWidth={o['sw']}")
        # 桌面属性面板仍在右栏（非抽屉）
        pos = try_eval(pg, "()=>{const e=document.querySelector('#panels');"
                           "return e?getComputedStyle(e).position:'MISSING';}")
        check("桌面属性面板仍是右栏（非 fixed 抽屉）", pos == "static", f"position={pos}")

        # ---- 复刻 web_smoke.py 的桌面关键断言（离线跑，不依赖设备在线）----
        # 依据：本次改了拖动/选中/保存路径（dropdown 抽屉、dragging 类、pointercancel），
        # 这些正是冒烟测试盯的地方，必须在离线阶段就能锁住。
        n0 = pg.eval_on_selector_all(".el", "e=>e.length")
        pg.click("text=＋ 文字行")
        pg.wait_for_timeout(400)
        n1 = pg.eval_on_selector_all(".el", "e=>e.length")
        check("桌面：点「＋ 文字行」新增元素", n1 == n0 + 1, f"{n0} → {n1}")

        pg.fill("#pf-text", "桌面回归 {date}")
        pg.wait_for_timeout(300)
        check("桌面：属性面板改文字生效",
              pg.evaluate("blocks[blocks.length-1].text") == "桌面回归 {date}")

        el = pg.query_selector_all(".el")[-1]
        el.scroll_into_view_if_needed()
        pg.wait_for_timeout(200)
        box = el.bounding_box()
        col0 = pg.evaluate("blocks[blocks.length-1].col||0")
        pg.mouse.move(box["x"] + 20, box["y"] + 6)
        pg.mouse.down()
        pg.mouse.move(box["x"] + 20 + 28, box["y"] + 6, steps=6)     # 右移 28px = 2 列
        pg.mouse.up()
        pg.wait_for_timeout(400)
        check("桌面：横向拖动 28px 改 2 列（量纲未变）",
              pg.evaluate("blocks[blocks.length-1].col") == col0 + 2,
              f"col {col0} → {pg.evaluate('blocks[blocks.length-1].col')}")

        el = pg.query_selector_all(".el")[-1]
        el.scroll_into_view_if_needed()
        pg.wait_for_timeout(200)
        box = el.bounding_box()
        base = pg.evaluate("(()=>{const L=layout();const i=blocks.length-1;"
                           "return (L.rows[i]&&L.rows[i].row)||0;})()")
        pg.mouse.move(box["x"] + 20, box["y"] + 6)
        pg.mouse.down()
        pg.mouse.move(box["x"] + 20, box["y"] + 6 + 22, steps=6)     # 下移 22px = 1 行
        pg.mouse.up()
        pg.wait_for_timeout(400)
        check("桌面：纵向拖动 22px 钉住行（不累加跑飞）",
              pg.evaluate("blocks[blocks.length-1].row") == base + 1,
              f"row {base} → {pg.evaluate('blocks[blocks.length-1].row')}")

        pg.click("text=保存模板")
        pg.wait_for_timeout(800)
        msg = pg.eval_on_selector("#msg", "e=>e.textContent") or ""
        check("桌面：保存模板走通（PUT /api/receipt/template）", "已保存" in msg, msg[:110])
        pg.screenshot(path=str(shots_dir / "desktop-receipt.png"), full_page=False)
        shots.append(shots_dir / "desktop-receipt.png")
        pg.close()

        # ---- PC 风格一致性：6 页的**设计系统**必须完全一致（用户反馈"各页风格不一致"）----
        # 断言方式：往页面里注入"探针元素"（普通 button / input / .card / h3），读它们的计算样式。
        # 为什么不用"页面上第一个按钮"：抽奖页第一个按钮是大圆按钮、标定页是微调大按钮 ——
        # 那些是**有意的页面变体**；这里要验的是"任何普通元素在各页都长一样"（设计系统一致），
        # 探针法也顺带保证以后再往页面里加普通控件时会自动一致。
        probe = """() => {
            const box = document.createElement('div');
            box.innerHTML = '<div class="card" id="__probeCard"><h3>x</h3>'
                          + '<button id="__probeBtn">x</button>'
                          + '<input id="__probeInput"></div>';
            document.body.appendChild(box);
            const cs = e => getComputedStyle(e);
            const card = cs(document.getElementById('__probeCard'));
            const btn = cs(document.getElementById('__probeBtn'));
            const inp = cs(document.getElementById('__probeInput'));
            const h = cs(box.querySelector('h3'));
            const body = cs(document.body);
            const r = {maxW: body.maxWidth, font: body.fontSize,
                       cardPad: card.padding, cardR: card.borderRadius,
                       btnPad: btn.padding, btnFont: btn.fontSize, btnR: btn.borderRadius,
                       inpPad: inp.padding, inpFont: inp.fontSize, inpR: inp.borderRadius,
                       hFont: h.fontSize};
            box.remove();
            return r;
        }"""
        print("    PC 风格基准：宽度 760（小票除外）/ 卡片 16·12 / 按钮 8×16·14·8 / 输入 8×10·14·8")
        pc_styles = {}
        root_url = f"http://127.0.0.1:{args.port}"      # 注意：上面的拖动断言把 base 复用成了行号
        for path in pages:
            p = dctx.new_page()
            p.goto(root_url + path, wait_until="load", timeout=20000)
            p.wait_for_timeout(1200)
            pc_styles[path] = p.evaluate(probe)
            p.close()
        # 期望值（唯一真源：app.css 的设计令牌）
        EXP = {"font": "14px", "cardPad": "16px", "cardR": "12px",
               "btnPad": "8px 16px", "btnFont": "14px", "btnR": "8px",
               "inpPad": "8px 10px", "inpFont": "14px", "inpR": "8px", "hFont": "15px"}
        for path in pages:
            st = pc_styles[path]
            bad = {k: st.get(k) for k, want in EXP.items() if st.get(k) != want}
            check(f"{path} 设计系统符合统一定义", not bad, f"不符：{bad}")
            # 每页期望宽度：两个有意的例外（小票=并排宽版；WiFi=窄列，学习 artnet 布局），
            # 其余 5 页统一 760px —— 例外必须显式列出，不能悄悄放宽断言
            # 唯一例外：小票编辑页（画布+属性栏并排）。WiFi 页已按用户要求回到 760px
            WANT = {"/receipt.html": "none"}
            want = WANT.get(path, "760px")
            label = {"none": "宽版例外（画布+属性栏并排）"}
            check(f"{path} 内容宽度 {want}" + ("（" + label[want] + "）" if want in label else ""),
                  st["maxW"] == want, f"maxWidth={st['maxW']}")
        # 跨页比对：同一属性在所有页面必须取值相同（这是"风格一致"的直接判据）
        for key in ("font", "cardPad", "cardR", "btnPad", "btnFont", "btnR",
                    "inpPad", "inpFont", "inpR", "hFont"):
            vals = {p: s.get(key) for p, s in pc_styles.items()}
            check(f"跨页一致：{key}", len(set(vals.values())) == 1,
                  f"出现 {sorted(set(vals.values()))}")
        dctx.close()
        browser.close()

    httpd.shutdown()
    print(f"\n{'=' * 60}\n通过 {len(passed)} / 失败 {len(failed)}\n{'=' * 60}")
    for f in failed:
        print(f"  ❌ {f}")
    print(f"截图目录: {shots_dir}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
