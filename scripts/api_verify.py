#!/usr/bin/env python3
"""lucky-page HTTP API 验收脚本（设计 §8.1 / §8.1b）

用法:
    python3 scripts/api_verify.py [目标IP]           # 默认 192.168.5.72
    python3 scripts/api_verify.py 192.168.4.1        # 也可跑设备热点地址

背景：HTTP 层曾拖了三轮未验证，导致"所有静态页面 404"漏到用户手里。
本脚本把 26 个端点的验收固化成可复跑脚本，任何人在同一局域网都能一键回归。

只做「读」与「可还原的写」；**默认不跑任何实际打印/出票的用例**：

    --with-print   才会执行 POST /api/printer/test（真的出纸）
    --draw         才会执行抽奖全链路（会转页 + 出票）

默认只做只读的打印机状态检查（GET /api/status），不出纸。
"""
import json
import sys
import time
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "192.168.5.72"
DO_DRAW = "--draw" in sys.argv
# ⚠ 会**真的出纸**的用例默认不跑（用户要求：除非明确要求，否则排除实际打印）
WITH_PRINT = "--with-print" in sys.argv
SKIPPED = []
ROOT = f"http://{BASE}"

passed, failed = [], []


def call(method, path, body=None, timeout=8, allow_redirect=True):
    url = ROOT + path
    data = None
    headers = {}
    if body is not None:
        data = body.encode() if isinstance(body, str) else body
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    opener = urllib.request.build_opener()
    if not allow_redirect:
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *a, **k):
                return None
        opener = urllib.request.build_opener(NoRedirect)
    try:
        with opener.open(req, timeout=timeout) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)
    except Exception as e:                                   # 连接失败
        return 0, str(e).encode(), {}


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(f"  {'✅' if cond else '❌'} {name}{(' — ' + detail) if detail and not cond else ''}")


def expect_status(name, method, path, want, body=None, timeout=8):
    code, data, _ = call(method, path, body, timeout)
    check(f"{method} {path} → {want}", code == want, f"实际 {code}: {data[:120]!r}")
    return code, data


def expect_json_has(name, path, key):
    code, data, _ = call("GET", path)
    ok = code == 200
    obj = None
    if ok:
        try:
            obj = json.loads(data)
        except Exception:
            ok = False
    check(f"GET {path} 含 {key}", ok and obj is not None, f"HTTP {code}, body={data[:120]!r}")
    return obj


print(f"=== lucky-page HTTP API 验收 @ {ROOT} ===\n")

print("① 静态页面（通配匹配修复的回归项）")
for page in ["/", "/index.html", "/prizes.html", "/receipt.html", "/logo.html",
             "/calibrate.html", "/wifi.html", "/pages.html", "/sound.html"]:
    code, data, _ = call("GET", page)
    check(f"GET {page} → 200 且是 HTML", code == 200 and b"<html" in data.lower(),
          f"HTTP {code}")

print("\n② 只读接口")
for path, key in [
    ("/api/status", "device"),
    ("/api/lottery/status", "state"),
    ("/api/prizes", "tiers"),
    ("/api/pages", "total_pages"),
    ("/api/receipt", "shop_name"),
    ("/api/receipt/preview", "text"),
    ("/api/receipt/template", "blocks"),
    ("/api/receipt/params", "params"),
    ("/api/wifi", "ap_ssid"),
]:
    expect_json_has(f"GET {path}", path, key)

code, data, _ = call("GET", "/api/logo")
check("GET /api/logo → 200/404（未上传时 404）", code in (200, 404), f"HTTP {code}")

print("\n③ 状态一致性（WiFi / 时间 / AP 信息）")
code, data, _ = call("GET", "/api/wifi")
wifi = json.loads(data) if code == 200 else {}
wifi_raw = data.decode(errors="replace") if isinstance(data, bytes) else str(data)
check("WiFi JSON 含 ap_up/ap_ssid/ap_channel", all(k in wifi for k in ("ap_up", "ap_ssid", "ap_channel")))
# 精确检查 JSON 键，而非子串（"has_password" 里就含 "password"，曾误报）
check("WiFi JSON 不含 password 键", '"password"' not in wifi_raw and '"passwd"' not in wifi_raw,
      wifi_raw[:160])
saved_raw = json.dumps(wifi.get("saved", []))
check("已保存网络列表只回 ssid/has_password",
      "saved" in wifi and '"password"' not in saved_raw and "has_password" in saved_raw,
      saved_raw[:160])
check("时间已同步（WiFi 联通后 SNTP 生效）", wifi.get("time_synced") is True,
      f"time_synced={wifi.get('time_synced')}")
check("STA 已连接", wifi.get("sta_connected") is True, f"sta_connected={wifi.get('sta_connected')}")
check("无遗留断开原因", wifi.get("last_reason") == "none", f"last_reason={wifi.get('last_reason')}")
lap = None
code, data, _ = call("GET", "/api/lottery/status")
if code == 200:
    lap = json.loads(data)
    check("lottery/status 有 state 与 stats", "state" in lap and "stats" in lap)
    st = lap.get("stats", {})
    # 回归（H19）：原 total_draws 是会话计数，重启归零 → 界面出现"今日 > 累计"
    check("累计开奖 ≥ 今日开奖",
          st.get("total_draws", 0) >= st.get("today_draws", 0),
          f"total={st.get('total_draws')} today={st.get('today_draws')}")

print("\n④ 负例（校验必须拒绝且不落库）")
expect_status("坏 JSON → 400", "PUT", "/api/receipt/template", 400, "{not json")
expect_status("坏模板（未知块）→ 400", "PUT", "/api/receipt/template", 400,
              json.dumps({"version": 3, "blocks": [{"type": "image"}]}))
expect_status("空 SSID → 400", "PUT", "/api/wifi", 400, json.dumps({"ssid": "", "password": "12345678"}))
expect_status("密码过短 → 400", "PUT", "/api/wifi", 400, json.dumps({"ssid": "x", "password": "123"}))
expect_status("SSID 超 32 字节 → 400", "PUT", "/api/wifi", 400,
              json.dumps({"ssid": "a" * 33, "password": "12345678"}))
expect_status("非法 epoch → 400", "POST", "/api/time", 400, json.dumps({"epoch": 0}))
expect_status("删除越界网络索引 → 400", "DELETE", "/api/wifi", 400, json.dumps({"index": 99}))
expect_status("错误方法（DELETE 到只读接口）→ 405", "DELETE", "/api/status", 405)
code, data, hdr = call("GET", "/generate_204", allow_redirect=False)
check("未知路径 → 302 捕获跳转", code == 302 and "Location" in hdr, f"HTTP {code}")
code, data, hdr = call("GET", "/../etc/passwd", allow_redirect=False)
check("路径穿越 → 302（不读文件系统）", code == 302, f"HTTP {code}")
expect_status("坏 logo → 400", "POST", "/api/logo", 400, "NOTALOGO")

print("\n⑤ 可还原的写操作")
code, data, _ = call("GET", "/api/receipt/template")
orig_tmpl = data.decode() if code == 200 else None
if orig_tmpl:
    mut = json.loads(orig_tmpl)
    mut["paper_cols"] = 48 if mut.get("paper_cols") == 32 else 32
    code, body, _ = call("PUT", "/api/receipt/template", json.dumps(mut))
    check("PUT 模板（改纸宽）→ 200", code == 200, f"HTTP {code}: {body[:120]!r}")
    code, body, _ = call("PUT", "/api/receipt/template", orig_tmpl)
    check("还原原始模板 → 200", code == 200, f"HTTP {code}")
    code, data2, _ = call("GET", "/api/receipt/template")
    check("还原后与原始一致", data2.decode() == orig_tmpl if code == 200 else False)

code, data, _ = call("GET", "/api/pages")
if code == 200:
    pages = json.loads(data)
    code, body, _ = call("PUT", "/api/pages", json.dumps({"total_pages": pages["total_pages"]}))
    check("PUT 页数（原值）→ 200", code == 200, f"HTTP {code}: {body[:120]!r}")
    expect_status("PUT 页数越界 → 400", "PUT", "/api/pages", 400, json.dumps({"total_pages": 999}))

code, body, _ = call("POST", "/api/time", json.dumps({"epoch": int(time.time())}))
check("POST /api/time（本机校时）→ 200", code == 200, f"HTTP {code}: {body[:120]!r}")
if code == 200:
    lap = json.loads(call("GET", "/api/lottery/status")[1])
    code2, data2, _ = call("GET", "/api/status")
    check("校时后 /api/status time_synced 或 now 合理", code2 == 200 and b"now" in data2)

print("\n⑤b 行程偏移调整（标定「页码调整模式」；改完必须还原）")
code, data, _ = call("GET", "/api/adjust")
adj0 = None
if code == 200:
    adj0 = json.loads(data)
    ok_shape = (adj0.get("cell_count") == 1
                and isinstance(adj0.get("offsets"), list) and len(adj0["offsets"]) == 1
                and len(adj0["offsets"][0]) == adj0.get("total_pages")
                and len(adj0.get("mapping", [])) >= 1)
    check("GET /api/adjust 结构（cell_count/offsets/mapping）", ok_shape,
          f"cell_count={adj0.get('cell_count')} pages={adj0.get('total_pages')} "
          f"offsets0={len(adj0.get('offsets', [[]])[0])}")
else:
    check("GET /api/adjust → 200", False, f"HTTP {code}")

if adj0:
    page = 1 if adj0["total_pages"] > 1 else 0
    orig_off = adj0["offsets"][0][page]
    probe = orig_off + 3 if orig_off <= 120 else orig_off - 3
    code, body, _ = call("PUT", "/api/adjust",
                         json.dumps({"cell": 0, "page": page, "offset": probe}))
    ok, got = code == 200, None
    if ok:
        got = json.loads(body)
        ok = (got.get("offset") == probe
              and got.get("value") == min(255, max(0, adj0["mapping"][page] + probe)))
    check("PUT /api/adjust 改偏移 → 200 且 value=clamp(base+offset)", ok,
          f"HTTP {code}: {body[:140]!r}")
    # 读回确认真的落盘了
    code, data2, _ = call("GET", "/api/adjust")
    check("GET 读回偏移与写入一致",
          code == 200 and json.loads(data2)["offsets"][0][page] == probe)
    # 还原（关键：不能污染设备的标定数据）
    code, _, _ = call("PUT", "/api/adjust",
                      json.dumps({"cell": 0, "page": page, "offset": orig_off}))
    code2, data3, _ = call("GET", "/api/adjust")
    check("还原原始偏移",
          code == 200 and code2 == 200
          and json.loads(data3)["offsets"][0][page] == orig_off,
          f"应为 {orig_off}，实为 {json.loads(data3)['offsets'][0][page] if code2 == 200 else '?'}")

    expect_status("PUT /api/adjust 坏 JSON → 400", "PUT", "/api/adjust", 400, "{not json")
    expect_status("PUT /api/adjust 页越界 → 400", "PUT", "/api/adjust", 400,
                  json.dumps({"cell": 0, "page": 999, "offset": 1}))
    expect_status("PUT /api/adjust 偏移越界 → 400", "PUT", "/api/adjust", 400,
                  json.dumps({"cell": 0, "page": page, "offset": 999}))
    expect_status("PUT /api/adjust cell 非 0 → 400", "PUT", "/api/adjust", 400,
                  json.dumps({"cell": 5, "page": page, "offset": 1}))
    expect_status("POST /api/adjust/reset 页越界 → 400", "POST", "/api/adjust/reset", 400,
                  json.dumps({"cell": 0, "page": 999}))
    # 清零本页 + 验证只影响该页，然后还原
    code, _, _ = call("POST", "/api/adjust/reset", json.dumps({"cell": 0, "page": page}))
    code2, data4, _ = call("GET", "/api/adjust")
    check("POST /api/adjust/reset 清零本页",
          code == 200 and code2 == 200 and json.loads(data4)["offsets"][0][page] == 0)
    call("PUT", "/api/adjust", json.dumps({"cell": 0, "page": page, "offset": orig_off}))
    code3, data5, _ = call("GET", "/api/adjust")
    check("清零后再次还原成功",
          code3 == 200 and json.loads(data5)["offsets"][0][page] == orig_off)

print("\n⑤c WiFi 进阶（学习 artnet：改热点名 / 静态 IP / 断开；均不危及连通性）")
code, data, _ = call("GET", "/api/wifi")
w0 = json.loads(data) if code == 200 else {}
check("GET /api/wifi 含进阶字段（ip_mode/ap_ssid_custom/user_disconnected）",
      all(k in w0 for k in ("ip_mode", "ap_ssid_custom", "user_disconnected")),
      f"keys={sorted(w0.keys())[:12]}")

# —— 改热点名：只影响 AP 接口，主机走 STA，安全；改完必须复原 ——
if w0.get("ap_ssid"):
    orig_ap = w0["ap_ssid"]
    probe_ap = "lucky-page-VERIFY"
    code, body, _ = call("PUT", "/api/wifi/ap", json.dumps({"ap_ssid": probe_ap}))
    ok = code == 200 and json.loads(body).get("ap_ssid") == probe_ap
    check("PUT /api/wifi/ap 改热点名 → 200", ok, f"HTTP {code}: {body[:120]!r}")
    code, data2, _ = call("GET", "/api/wifi")
    check("读回热点名已变", code == 200 and json.loads(data2).get("ap_ssid") == probe_ap)
    check("ap_ssid_custom 标记为真",
          code == 200 and json.loads(data2).get("ap_ssid_custom") is True)
    # 复原：空串 = 恢复默认名（MAC 后缀）
    code, body, _ = call("PUT", "/api/wifi/ap", json.dumps({"ap_ssid": ""}))
    code2, data3, _ = call("GET", "/api/wifi")
    restored = code2 == 200 and json.loads(data3).get("ap_ssid") == orig_ap
    check("复原默认热点名", code == 200 and restored,
          f"HTTP {code}；现在={json.loads(data3).get('ap_ssid') if code2==200 else '?'} 期望={orig_ap}")
    # —— AP 密码可改（用户要求）；**绝不回显**是最关键的断言 ——
    probe_pass = "verify-pass-2026"
    code, body, _ = call("PUT", "/api/wifi/ap", json.dumps({"ap_pass": probe_pass}))
    check("PUT /api/wifi/ap 改热点密码 → 200", code == 200, f"HTTP {code}: {body[:120]!r}")
    code, data4, _ = call("GET", "/api/wifi")
    g = json.loads(data4) if code == 200 else {}
    check("ap_pass_custom 标记为真", g.get("ap_pass_custom") is True, f"{g.get('ap_pass_custom')}")
    raw4 = data4.decode("utf-8", "replace") if isinstance(data4, bytes) else data4
    check("⭐ GET /api/wifi 响应全文不含明文密码（绝不回显）",
          probe_pass not in raw4, "响应里出现了明文密码！")
    check("⭐ GET /api/wifi 不含 ap_pass 字段",
          "ap_pass" not in g or g.get("ap_pass") is None,
          f"字段={[k for k in g if 'pass' in k]}")
    # 复原默认密码
    code, body, _ = call("PUT", "/api/wifi/ap", json.dumps({"ap_pass": ""}))
    code2, data5, _ = call("GET", "/api/wifi")
    back = json.loads(data5).get("ap_pass_custom") if code2 == 200 else None
    check("复原默认热点密码", code == 200 and back is False, f"HTTP {code}；ap_pass_custom={back}")
    expect_status("PUT /api/wifi/ap 密码 7 位 → 400", "PUT", "/api/wifi/ap", 400,
                  json.dumps({"ap_pass": "1234567"}))
    expect_status("PUT /api/wifi/ap 密码 64 位 → 400", "PUT", "/api/wifi/ap", 400,
                  json.dumps({"ap_pass": "x" * 64}))
    expect_status("PUT /api/wifi/ap 密码含控制字符 → 400", "PUT", "/api/wifi/ap", 400,
                  json.dumps({"ap_pass": "abcd\u0001efgh"}))
    expect_status("PUT /api/wifi/ap 空对象 → 400", "PUT", "/api/wifi/ap", 400, "{}")
    expect_status("PUT /api/wifi/ap 坏 JSON → 400", "PUT", "/api/wifi/ap", 400, "{not json")
    expect_status("PUT /api/wifi/ap 超长名 → 400", "PUT", "/api/wifi/ap", 400,
                  json.dumps({"ap_ssid": "x" * 32}))
    expect_status("PUT /api/wifi/ap 含控制字符 → 400", "PUT", "/api/wifi/ap", 400,
                  json.dumps({"ap_ssid": "bad\u0001name"}))

# —— 静态 IP：只测校验负例（正例应用会改 IP，改由串口验证，避免自失联）——
expect_status("PUT /api/wifi/ip 坏 IP → 400", "PUT", "/api/wifi/ip", 400,
              json.dumps({"ip": "192.168.1", "netmask": "255.255.255.0", "gateway": "192.168.1.1"}))
expect_status("PUT /api/wifi/ip 掩码不连续 → 400", "PUT", "/api/wifi/ip", 400,
              json.dumps({"ip": "192.168.1.100", "netmask": "255.0.255.0", "gateway": "192.168.1.1"}))
expect_status("PUT /api/wifi/ip 网关跨网段 → 400", "PUT", "/api/wifi/ip", 400,
              json.dumps({"ip": "192.168.1.100", "netmask": "255.255.255.0", "gateway": "192.168.2.1"}))
expect_status("PUT /api/wifi/ip 缺字段 → 400", "PUT", "/api/wifi/ip", 400,
              json.dumps({"ip": "192.168.1.100"}))
expect_status("PUT /api/wifi/ip 坏 JSON → 400", "PUT", "/api/wifi/ip", 400, "{not json")

# —— 断开接口：只测坏 JSON（真断开会让设备离开本网段，改由串口验证）——
expect_status("POST /api/wifi/disconnect 带坏 JSON → 400", "POST", "/api/wifi/disconnect", 400,
              "{not json")
expect_status("POST /api/wifi/reconnect 带坏 JSON → 400", "POST", "/api/wifi/reconnect", 400,
              "{not json")

print("\n⑥ 扫描（阻塞 2~4s；已连接路由器时应被状态白名单拒绝）")
wifi_state = call("GET", "/api/wifi")[1]
sta_state = json.loads(wifi_state).get("sta_state") if wifi_state else "?"
code, body, _ = call("POST", "/api/wifi/scan", timeout=15)
if code == 200:
    nets = json.loads(body)
    check(f"未连接时扫描返回网络列表（{len(nets)} 个）", isinstance(nets, list))
    if nets:
        check("扫描结果含 rssi/bars/secure", all(k in nets[0] for k in ("ssid", "rssi", "bars", "secure")))
elif code in (409, 503):
    # 已连接时拒绝扫描是正确的（否则会打断 STA 连接 —— 真机踩坑）
    check(f"已连接({sta_state})时扫描被拒绝且给出原因", b"\u" in body or len(body) > 10, body[:160])
else:
    check("扫描返回 200 或 409", False, f"HTTP {code}")
time.sleep(2)
code, data, _ = call("GET", "/api/wifi")
check("扫描后 WiFi 仍可达（连接未被扫描打断）", code == 200,
      f"HTTP {code}")

print("\n⑦ 打印机状态（只读检查，**不出纸**）")
code, data, _ = call("GET", "/api/status")
if code == 200:
    st = json.loads(data)
    check("GET /api/status 含打印机状态", "printer_fault" in st, f"keys={sorted(st)[:8]}")
    check("打印机无故障（只读检查，不出纸）", not st.get("printer_fault"),
          f"printer_fault={st.get('printer_fault')}")
else:
    check("GET /api/status → 200", False, f"HTTP {code}")

if WITH_PRINT:
    print("\n⑦b 打印自检页（会真的出纸，已显式 --with-print）")
    code, body, _ = call("POST", "/api/printer/test")
    check("POST /api/printer/test → 200", code == 200, f"HTTP {code}: {body[:120]!r}")
    time.sleep(3)
    code, data, _ = call("GET", "/api/status")
    if code == 200:
        st = json.loads(data)
        check("打印后打印机无故障", not st.get("printer_fault"),
              f"printer_fault={st.get('printer_fault')}")
else:
    SKIPPED.append("⑦b 打印自检页 POST /api/printer/test（真出纸）")
    print("   ⏭ 已跳过：POST /api/printer/test（真出纸）—— 需要时加 --with-print")

if not DO_DRAW:
    SKIPPED.append("⑧ 抽奖全链路 POST /api/draw（会转页 + 出票）")
    print("\n⑧ 已跳过抽奖全链路（会出票）—— 需要时加 --draw")
if DO_DRAW:
    print("\n⑧ 抽奖全链路（会转页 + 出票，已显式 --draw）")
    code, body, _ = call("POST", "/api/draw")
    check("POST /api/draw → 200/409（状态允许则开奖）", code in (200, 409), f"HTTP {code}: {body[:120]!r}")
    for _ in range(12):
        time.sleep(1)
        lap = json.loads(call("GET", "/api/lottery/status")[1])
        if lap.get("state") in ("done", "idle"):
            break
    check("抽奖回到 done/idle", lap.get("state") in ("done", "idle"), f"state={lap.get('state')}")
    check("产出票号", bool(lap.get("last", {}).get("ticket")), f"last={lap.get('last')}")

print(f"\n{'=' * 56}\n通过 {len(passed)} / 失败 {len(failed)}\n{'=' * 56}")
if SKIPPED:
    print("⏭ 本次跳过（会出纸/出票，需显式开启）：")
    for x in SKIPPED:
        print("   -", x)
for f in failed:
    print(f"  ❌ {f}")
sys.exit(1 if failed else 0)
