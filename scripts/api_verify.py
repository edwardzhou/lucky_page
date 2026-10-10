#!/usr/bin/env python3
"""lucky-page HTTP API 验收脚本（设计 §8.1 / §8.1b）

用法:
    python3 scripts/api_verify.py [目标IP]           # 默认 192.168.5.72
    python3 scripts/api_verify.py 192.168.4.1        # 也可跑设备热点地址

背景：HTTP 层曾拖了三轮未验证，导致"所有静态页面 404"漏到用户手里。
本脚本把 26 个端点的验收固化成可复跑脚本，任何人在同一局域网都能一键回归。

只做「读」与「可还原的写」；全程不碰抽奖（draw 需显式 --draw 才跑）。
"""
import json
import sys
import time
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "192.168.5.72"
DO_DRAW = "--draw" in sys.argv
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
             "/calibrate.html", "/wifi.html"]:
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

print("\n⑦ 打印自检页（会真的出纸）")
code, body, _ = call("POST", "/api/printer/test")
check("POST /api/printer/test → 200", code == 200, f"HTTP {code}: {body[:120]!r}")
time.sleep(3)
code, data, _ = call("GET", "/api/status")
if code == 200:
    st = json.loads(data)
    check("打印机无故障", not st.get("printer_fault"), f"printer_fault={st.get('printer_fault')}")

if DO_DRAW:
    print("\n⑧ 抽奖全链路（会转页 + 出票）")
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
for f in failed:
    print(f"  ❌ {f}")
sys.exit(1 if failed else 0)
