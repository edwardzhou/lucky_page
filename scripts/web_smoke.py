#!/usr/bin/env python3
"""web_smoke.py — 新版 UI（Lucky Studio）真机冒烟（2026-10-26 重写）

旧脚本断言绑定旧 DOM（画布拖动等），新设计结构不同，故按新 UI 重建。
原则同旧版：快照 → 测试 → 整体还原 → 复载复核；绝不触发物理动作
（不抽奖、不出票、不改 WiFi）。奖品名称编辑是唯一写操作，原样还原。

用法：python3 scripts/web_smoke.py 192.168.5.71
"""
import json
import sys
import urllib.request

from playwright.sync_api import sync_playwright

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.5.71"
BASE = f"http://{IP}"
PAGES = ["index", "prizes", "receipt", "logo", "calibrate", "wifi"]
passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("  PASS " if cond else "  FAIL ") + name + ("" if cond else f" -- {detail}"))


def api(path, method="GET", body=None, retries=3):
    data = json.dumps(body).encode() if body is not None else None
    last = None
    for _ in range(retries):
        try:
            req = urllib.request.Request(BASE + path, data=data, method=method,
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=20) as r:
                raw = r.read().decode()
                return r.status, (json.loads(raw) if raw else {})
        except Exception as e:      # 真机偶发 408（主线程/NVS 写忙）→ 重试
            last = e
            import time as _t
            _t.sleep(1.0)
    raise last


def compact_pool(p):
    """只保留设备接受的字段（GET 里的 probability/depleted 是派生值）。"""
    return {"version": p.get("version", 2), "total_pages": p["total_pages"],
            "depletion_policy": p.get("depletion_policy", 0),
            "tiers": [{k: t[k] for k in ("level", "page", "weight", "quantity",
                                         "remaining", "name", "note") if k in t}
                      for t in p.get("tiers", [])]}


def main():
    print(f"== 快照（{IP}）==")
    _, prizes0 = api("/api/prizes")
    tmpl0 = {m: api(f"/api/receipt/template?mode={m}")[1] for m in (0, 1, 2)}
    _, adjust0 = api("/api/adjust")
    _, status0 = api("/api/lottery/status")
    print(f"  奖池 {len(prizes0.get('tiers', []))} 级；页数 {adjust0.get('total_pages')}；"
          f"状态 {status0.get('state')}")

    with sync_playwright() as p:
        browser = p.chromium.launch()
        ctx = browser.new_context(viewport={"width": 1440, "height": 900})

        print("\n== 页面加载 ==")
        page_objs = {}
        for name in PAGES:
            pg = ctx.new_page()
            errs, bad = [], []
            pg.on("pageerror", lambda e: errs.append(str(e)))
            pg.on("console", lambda m: errs.append(m.text) if m.type == "error" else None)
            pg.on("response", lambda r: bad.append(f"{r.status} {r.url}")
                  if "/api/" in r.url and r.status >= 400 else None)
            pg.goto(f"{BASE}/{name}.html", wait_until="domcontentloaded", timeout=30000)
            pg.wait_for_timeout(2500)
            check(f"{name}.html 无 JS 异常", not errs, str(errs[:2]))
            check(f"{name}.html API 无 4xx/5xx", not bad, str(bad[:3]))
            page_objs[name] = pg

        print("\n== 工作台 ==")
        idx = page_objs["index"]
        check("设备状态为就绪",
              idx.inner_text("#drawState").strip() in ("准备就绪", "已完成"),
              idx.inner_text("#drawState"))
        check("累计开奖数为数字",
              idx.inner_text("#totalDraws").strip().replace(",", "").isdigit(),
              idx.inner_text("#totalDraws"))
        check("设备当前页显示（current_page 生效）",
              "第" in idx.inner_text("#devicePage"), idx.inner_text("#devicePage"))
        check("抽奖按钮存在且未误触发",
              idx.locator("#startDraw").count() == 1 and
              idx.locator("#startDraw").inner_text().strip() in ("开始抽奖", "正在处理…"))

        print("\n== 奖池编辑/还原 ==")
        pz = page_objs["prizes"]
        rows = pz.locator("#tiers tr:not(.hdr)")
        check("奖池行数与设备一致", rows.count() == len(prizes0.get("tiers", [])),
              f"{rows.count()} vs {len(prizes0.get('tiers', []))}")
        first_name = pz.locator('#tiers tr:not(.hdr)').first.locator('input[data-f="name"]')
        original_name = first_name.input_value()
        new_name = original_name + "*"
        first_name.fill(new_name)
        pz.wait_for_timeout(300)
        check("编辑后出现未保存标记",
              "show" in (pz.get_attribute("#unsavedFlag", "class") or ""),
              pz.get_attribute("#unsavedFlag", "class"))
        pz.get_by_role("button", name="保存奖池").first.click()
        pz.wait_for_timeout(1500)
        _, prizes1 = api("/api/prizes")
        got = next((t["name"] for t in prizes1.get("tiers", [])
                    if t.get("level") == 1), None)
        check("保存后设备回读为新名称", got == new_name, f"{got!r} != {new_name!r}")

        api("/api/prizes", "PUT", compact_pool(prizes0))
        _, prizes2 = api("/api/prizes")
        check("奖池整体还原成功",
              [t.get("name") for t in prizes2.get("tiers", [])] ==
              [t.get("name") for t in prizes0.get("tiers", [])])

        print("\n== 标定微调 ==")
        cal = page_objs["calibrate"]
        _, adj_a = api("/api/adjust")
        page_now = 0
        off_a = adj_a["offsets"][0][page_now]
        cal.locator("#adj_p5").click()
        cal.wait_for_timeout(800)
        _, adj_b = api("/api/adjust")
        off_b = adj_b["offsets"][0][page_now]
        check("点击 +5 后偏移 +5", off_b == off_a + 5, f"{off_a} → {off_b}")
        cal.locator("#adj_m5").click()
        cal.wait_for_timeout(800)
        _, adj_c = api("/api/adjust")
        check("点击 -5 后偏移还原", adj_c["offsets"][0][page_now] == off_a,
              f"{adj_c['offsets'][0][page_now]} != {off_a}")
        check("标定未改变页码映射", adj_c["mapping"] == adj_a["mapping"])

        print("\n== 小票模板 ==")
        rc = page_objs["receipt"]
        check("画布已渲染", rc.locator("#canvas").count() == 1)
        blocks_before = api("/api/receipt/template?mode=0")[1].get("blocks", [])
        rc.select_option("#mode", "canvas")
        rc.wait_for_timeout(1200)
        canvas_tmpl = api("/api/receipt/template?mode=1")[1]
        check("切到画布模式后加载到画布槽模板",
              len(canvas_tmpl.get("blocks", [])) > 0,
              f"canvas blocks={len(canvas_tmpl.get('blocks', []))}")
        rc.get_by_role("button", name="服务端预览").first.click()
        rc.wait_for_timeout(1500)
        prev = rc.inner_text("#preview")
        check("服务端预览返回文本", len(prev.strip()) > 10, prev[:60])
        _, tmpl_after = api("/api/receipt/template?mode=0")
        check("预览/切模式未改动生效模板", tmpl_after.get("blocks") == blocks_before)

        print("\n== 网络页 ==")
        wf = page_objs["wifi"]
        ap = wf.inner_text("#apName").strip()
        check("热点名显示", bool(ap) and ap != "—", ap)
        check("已保存网络区块存在", wf.locator("#savedCard").count() == 1)

        print("\n== 移动视口 ==")
        mctx = browser.new_context(viewport={"width": 390, "height": 844},
                                   is_mobile=True, has_touch=True)
        m = mctx.new_page()
        m.goto(f"{BASE}/index.html", wait_until="domcontentloaded", timeout=30000)
        m.wait_for_timeout(1500)
        ov = m.evaluate("document.documentElement.scrollWidth > "
                        "document.documentElement.clientWidth + 1")
        check("手机视口无横向溢出", not ov)
        check("手机端显示底部导航",
              m.evaluate("getComputedStyle(document.querySelector('.lp-mobile-nav')).display") == "grid")
        mctx.close()
        browser.close()

    print("\n== 还原复核 ==")
    _, prizes_end = api("/api/prizes")
    check("最终奖池与快照一致",
          [(t["name"], t["quantity"], t["weight"], t["page"]) for t in prizes_end.get("tiers", [])] ==
          [(t["name"], t["quantity"], t["weight"], t["page"]) for t in prizes0.get("tiers", [])])
    _, tmpl_end = api("/api/receipt/template?mode=0")
    check("最终模板与快照一致", tmpl_end.get("blocks") == tmpl0[0].get("blocks"))
    _, adj_end = api("/api/adjust")
    check("最终偏移与快照一致", adj_end["offsets"] == adjust0["offsets"])

    print(f"\n{'=' * 60}\n通过 {len(passed)} / 失败 {len(failed)}\n{'=' * 60}")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
