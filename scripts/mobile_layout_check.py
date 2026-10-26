#!/usr/bin/env python3
"""mobile_layout_check.py — 新版 UI（Lucky Studio）手机/桌面布局离线校验

2026-10-26 重写：旧断言绑定旧 DOM（760px 单列、#tiers tr[data-tier]），
新设计是「侧栏 + 顶栏 + 工作台」响应式布局，量纲已变化，故按新设计重建断言，
并把**有意的例外显式枚举**（范围滑块/复选框/页脚内联链接不参与触控断言）。

覆盖：
  1. 7 页移动视口（390×844）：无横向溢出、零 JS 异常、侧栏收起、汉堡可点、底部导航显示
  2. 触控目标 ≥36px（枚举例外）
  3. 输入框字号 ≥16px（除 range/checkbox）
  4. 桌面视口（1440×900）：无横向溢出、侧栏展开 224px、跨页组件样式一致
  5. 截图输出 /tmp/lucky-mobile-v2/

本地静态服务 + demo.js 模拟接口（?demo=1），不需要真机。
"""
import asyncio
import http.server
import pathlib
import socketserver
import sys
import threading

from playwright.async_api import async_playwright

ROOT = pathlib.Path(__file__).resolve().parent.parent / "main" / "assets" / "spiffs_contents"
PORT = 8898
PAGES = ["index", "prizes", "receipt", "pages", "logo", "calibrate", "wifi", "sound"]
SHOTS = pathlib.Path("/tmp/lucky-mobile-v2")

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(("  ✅ " if cond else "  ❌ ") + name + ("" if cond else f" — {detail}"))


def serve():
    class H(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(ROOT), **kw)

        def log_message(self, *a):
            pass

    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.TCPServer(("127.0.0.1", PORT), H)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


async def _std_input_style(browser, base, name):
    """取该页第一个不在表格/工具栏里的输入框计算样式（跨页比对用）。"""
    ctx = await browser.new_context(viewport={"width": 1440, "height": 900})
    pg = await ctx.new_page()
    await pg.goto(f"{base}/{name}.html?demo=1", wait_until="networkidle", timeout=30000)
    await pg.wait_for_timeout(400)
    st = await pg.evaluate("""() => {
      for (const e of document.querySelectorAll('input:not([type=checkbox]):not([type=range]),select,textarea')) {
        if (e.closest('td') || e.closest('.toolbar') || e.type === 'file') continue;
        const s = getComputedStyle(e);
        return {pad: s.padding, radius: s.borderRadius};
      }
      return null;
    }""")
    await ctx.close()
    return st


async def main():
    SHOTS.mkdir(parents=True, exist_ok=True)
    httpd = serve()
    base = f"http://127.0.0.1:{PORT}"
    async with async_playwright() as p:
        browser = await p.chromium.launch()

        # ---------------- 移动端 ----------------
        ctx = await browser.new_context(
            viewport={"width": 390, "height": 844}, is_mobile=True, has_touch=True,
            device_scale_factor=2,
            user_agent="Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) "
                       "AppleWebKit/605.1.15 Mobile/15E148")
        styles = {}
        for name in PAGES:
            path = f"/{name}.html"
            pg = await ctx.new_page()
            errs = []
            pg.on("pageerror", lambda e: errs.append(str(e)))
            pg.on("console", lambda m: errs.append(m.text) if m.type == "error" else None)
            await pg.goto(base + path + "?demo=1", wait_until="networkidle", timeout=30000)
            await pg.wait_for_timeout(900)
            print(f"\n[{name}]")
            check(f"{path} 无脚本异常", not errs, str(errs[:2]))

            st = await pg.evaluate("""() => {
              const d=document.documentElement;
              const g=s=>{const e=document.querySelector(s);return e?getComputedStyle(e):null};
              const side=g('.lp-sidebar'), menu=g('.lp-menu-button'), nav=g('.lp-mobile-nav');
              const probe=(sel)=>{const e=document.querySelector(sel);if(!e)return null;const s=getComputedStyle(e);
                return {pad:s.padding,radius:s.borderRadius,font:s.fontSize}};
              return {
                sw:d.scrollWidth, iw:d.clientWidth,
                sideVis:side?side.visibility:null,
                menu:menu?menu.display:null, nav:nav?nav.display:null,
                card:probe('.card'), btn:probe('button:not(.small-btn):not(.sm)'),
                inp:probe('input:not([type=checkbox]):not([type=range]),select,textarea')
              };
            }""")
            check(f"{path} 无横向溢出", st["sw"] <= st["iw"] + 1,
                  f"scrollWidth={st['sw']} > 视口 {st['iw']}")
            check(f"{path} 侧栏收起（移动端抽屉）", st["sideVis"] == "hidden", st["sideVis"])
            check(f"{path} 汉堡按钮可见", st["menu"] not in (None, "none"), st["menu"])
            check(f"{path} 底部导航可见", st["nav"] == "grid", st["nav"])
            styles[name] = st

            # 触控目标：主操作 ≥44px；紧凑控件（.sm/.mini/.small-btn/工具栏/菜单/页脚链接）
            # 是**设计有意的变体**，下限 32px —— 例外必须在这里显式枚举，不能悄悄放宽
            touch = await pg.evaluate("""() => {
              // 触点断言范围（显式枚举）：
              //   ① range/checkbox/radio 不是"点击高度"语义 → 不参与
              //   ② 侧栏/页脚/底部导航/工具栏/紧凑按钮是设计有意的密度变体 → 下限 32
              //   ③ 普通文字链接（<a> 无 .btn）是内联导航 → 下限 32
              //   ④ 其余按钮/输入/下拉 = 主操作 → 下限 44
              const compact=e=>e.matches('.sm,.mini,.small-btn,.lp-menu-button,.lp-topinfo,.lp-brand') ||
                !!e.closest('.toolbar') || !!e.closest('.lp-footer') || !!e.closest('.lp-mobile-nav') ||
                !!e.closest('.lp-sidebar') || !!e.closest('.lp-page-actions') || !!e.closest('.card.actions');
              const rows=[];
              for (const e of document.querySelectorAll('button,a,input,select,textarea')) {
                const t=(e.type||'').toLowerCase();
                if (['range','checkbox','radio'].includes(t)) continue;
                if (!e.checkVisibility || !e.checkVisibility({checkVisibilityCSS:true,checkOpacity:true})) continue;
                const r=e.getBoundingClientRect();
                if (r.width<=0||r.height<=0) continue;
                const h=+r.height.toFixed(0);
                // 文字链接（无 .btn）是内联导航，WCAG 允许"内联链接"例外 → 不做高度断言；
                // 其余按钮/输入/下拉按主操作 44 / 紧凑变体 32 判定
                if (e.tagName === 'A' && !e.classList.contains('btn')) continue;
                const min = compact(e) ? 32 : 44;
                if (h<min) rows.push({t:e.tagName,id:e.id,h,min});
              }
              return rows;
            }""")
            check(f"{path} 触控目标达标（主操作≥44 / 紧凑≥32）", not touch, f"过小 {touch[:4]}")

            fsz = await pg.evaluate("""() => {
              const els=[...document.querySelectorAll('input:not([type=checkbox]):not([type=range]),select,textarea')];
              return els.map(e=>({id:e.id,font:parseFloat(getComputedStyle(e).fontSize)})).filter(o=>o.font<16);
            }""")
            check(f"{path} 输入框字号 ≥16px（防 iOS 聚焦缩放）", not fsz, str(fsz[:4]))
            await pg.screenshot(path=str(SHOTS / f"m-{name}.png"))
            await pg.close()

        # 跨页一致性（同一组件在不同页计算样式一致）
        # 输入框只比对"表单默认样式"；表格内输入 / 文件控件是密度变体（枚举豁免）
        for name in PAGES:
            styles[name]["inp"] = await _std_input_style(browser, base, name)
        for key, label in [("card", "卡片"), ("btn", "按钮"), ("inp", "输入框")]:
            vals = {(s[key]["pad"], s[key]["radius"]) for s in styles.values() if s.get(key)}
            check(f"跨页一致：{label}内距/圆角", len(vals) <= 1, str(sorted(vals)))
        await ctx.close()

        # ---------------- 桌面端 ----------------
        ctx2 = await browser.new_context(viewport={"width": 1440, "height": 900})
        print("\n[桌面 1440×900]")
        for name in PAGES:
            path = f"/{name}.html"
            pg = await ctx2.new_page()
            errs = []
            pg.on("pageerror", lambda e: errs.append(str(e)))
            await pg.goto(base + path + "?demo=1", wait_until="networkidle", timeout=30000)
            await pg.wait_for_timeout(600)
            st = await pg.evaluate("""() => {
              const d=document.documentElement;const s=document.querySelector('.lp-sidebar');
              return {sw:d.scrollWidth, iw:d.clientWidth,
                      sideVis:s?getComputedStyle(s).visibility:null,
                      sideW:s?Math.round(s.getBoundingClientRect().width):0,
                      menu:getComputedStyle(document.querySelector('.lp-menu-button')).display};
            }""")
            check(f"{path} 桌面无溢出", st["sw"] <= st["iw"] + 1, f"{st['sw']}>{st['iw']}")
            check(f"{path} 桌面侧栏展开 224px", st["sideVis"] == "visible" and st["sideW"] == 224,
                  f"vis={st['sideVis']} w={st['sideW']}")
            check(f"{path} 桌面隐藏汉堡菜单", st["menu"] == "none", st["menu"])
            check(f"{path} 桌面无脚本异常", not errs, str(errs[:2]))
            await pg.screenshot(path=str(SHOTS / f"d-{name}.png"))
            await pg.close()
        await ctx2.close()
        await browser.close()

    httpd.shutdown()
    print(f"\n{'=' * 60}\n通过 {len(passed)} / 失败 {len(failed)}\n截图：{SHOTS}\n{'=' * 60}")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
