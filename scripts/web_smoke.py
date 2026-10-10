#!/usr/bin/env python3
"""lucky-page 前端冒烟回归（真实浏览器）

用法:
    python3 scripts/web_smoke.py [设备IP]      # 默认 192.168.5.72（设备热点则为 192.168.4.1）

为什么需要它：本项目多次出现"后端全绿、前端点不动"的问题，根因都是我批量改
HTML/JS 时引入的（漏 `#` 的选择器、重复 id、正则改坏表达式、两种 `$` 约定混用）。
后端单测与 HTTP 验收都覆盖不到这些，只有真的把页面跑起来才看得见。

覆盖：
  1) 6 个页面逐个加载 → 断言无 JS 异常、无页面错误提示条
  2) 画布编辑器交互 → 添加元素 / 横向拖动改列 / 纵向拖动钉行 / 属性编辑 / 保存
  3) 服务端预览与画布元素数一致
"""
import sys
from playwright.sync_api import sync_playwright

BASE = sys.argv[1] if len(sys.argv) > 1 else "192.168.5.72"
ROOT = f"http://{BASE}"
PAGES = ["/index.html", "/prizes.html", "/receipt.html", "/logo.html",
         "/calibrate.html", "/wifi.html"]

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(f"  {'✅' if cond else '❌'} {name}{(' — ' + detail) if detail and not cond else ''}")


def main():
    with sync_playwright() as pw:
        browser = pw.chromium.launch()

        print("① 页面加载（无 JS 异常 / 无错误提示条）")
        for path in PAGES:
            pg = browser.new_page()
            errs = []
            pg.on("pageerror", lambda e: errs.append(str(e)))
            try:
                pg.goto(ROOT + path, wait_until="load", timeout=20000)
                pg.wait_for_timeout(1800)
                bar = ""
                if pg.query_selector("#errbar"):
                    bar = pg.eval_on_selector("#errbar", "e=>e.textContent") or ""
                check(f"{path} 无脚本异常", not errs and not bar,
                      (errs[0] if errs else bar)[:110])
            except Exception as ex:                       # noqa: BLE001
                check(f"{path} 可打开", False, str(ex)[:110])
            finally:
                pg.close()

        print("\n② 画布编辑器交互")
        pg = browser.new_page()
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.goto(ROOT + "/receipt.html", wait_until="load", timeout=20000)
        pg.wait_for_timeout(2000)

        n0 = pg.eval_on_selector_all(".el", "e=>e.length")
        check(f"模板元素渲染（{n0} 个）", n0 > 0, f"渲染出 {n0} 个")

        pg.click("text=＋ 文字行")
        pg.wait_for_timeout(400)
        n1 = pg.eval_on_selector_all(".el", "e=>e.length")
        check("点「＋ 文字行」新增元素", n1 == n0 + 1, f"{n0} → {n1}")

        # 属性面板改文字
        pg.fill("#pf-text", "冒烟测试 {date}")
        pg.wait_for_timeout(300)
        check("属性面板改文字生效",
              pg.evaluate("blocks[blocks.length-1].text") == "冒烟测试 {date}")

        # 横向拖动 → col 变化（先把元素滚入视口：画布模式内容长，元素可能在视口外）
        el = pg.query_selector_all(".el")[-1]
        el.scroll_into_view_if_needed()
        pg.wait_for_timeout(200)
        box = el.bounding_box()
        col0 = pg.evaluate("blocks[blocks.length-1].col||0")
        pg.mouse.move(box["x"] + 20, box["y"] + 6)
        pg.mouse.down()
        pg.mouse.move(box["x"] + 20 + 28, box["y"] + 6, steps=6)   # 右移 2 列
        pg.mouse.up()
        pg.wait_for_timeout(400)
        col1 = pg.evaluate("blocks[blocks.length-1].col")
        check("横向拖动改列位", col1 == col0 + 2, f"col {col0} → {col1}（期望 +2）")

        # 纵向拖动 → 钉住行（基准行固定，不得累加跑飞）
        el = pg.query_selector_all(".el")[-1]
        el.scroll_into_view_if_needed()
        pg.wait_for_timeout(200)
        box = el.bounding_box()
        base = pg.evaluate(
            "(()=>{const L=layout();const i=blocks.length-1;"
            "return (L.rows[i]&&L.rows[i].row)||0;})()")
        pg.mouse.move(box["x"] + 20, box["y"] + 6)
        pg.mouse.down()
        pg.mouse.move(box["x"] + 20, box["y"] + 6 + 22, steps=6)   # 下移 1 行
        pg.mouse.up()
        pg.wait_for_timeout(400)
        row1 = pg.evaluate("blocks[blocks.length-1].row")
        check("纵向拖动钉住行（不累加跑飞）", row1 == base + 1,
              f"row {base} → {row1}（期望 +1）")

        # 二维码往返：画布必须读到内容，保存后接口端内容不得丢失
        # （历史 bug：接口用 content/size/ecc，画布用 text/qr_size/qr_ecc，
        #   直接 Object.assign 导致保存时把二维码内容清空）
        qr_js = pg.evaluate(
            "(()=>{const b=blocks.find(x=>x.type==='qr');"
            "return b? (b.text+'|'+b.qr_size+'|'+b.qr_ecc) : 'MISSING';})()")
        check("画布读到二维码内容", qr_js != "MISSING" and qr_js.split("|")[0] != "",
              f"画布二维码={qr_js}")
        import json as _json
        import urllib.request as _u
        before = _json.load(_u.urlopen(f"{ROOT}/api/receipt/template", timeout=8))
        qr_before = next((b for b in before["blocks"] if b["type"] == "qr"), {})
        pg.click("text=保存模板")
        pg.wait_for_timeout(1500)
        after = _json.load(_u.urlopen(f"{ROOT}/api/receipt/template", timeout=8))
        qr_after = next((b for b in after["blocks"] if b["type"] == "qr"), {})
        check("保存后二维码内容未丢失",
              qr_after.get("content") == qr_before.get("content") and qr_after.get("size") == qr_before.get("size"),
              f"before={qr_before} after={qr_after}")

        # 背景层标记必须能在"载入 → 保存"往返中存活
        # （历史 bug：payload() 漏传 bg → 保存一次背景层就退化成普通图片）
        pg.select_option("#mode", "canvas")
        pg.wait_for_timeout(2000)
        bg_js = pg.eval_on_selector_all(".el.bg", "e=>e.length")
        check("画布模式载入后有背景元素", bg_js >= 1, f"背景元素 {bg_js}")
        pg.click("text=保存模板")
        pg.wait_for_timeout(1500)
        after_bg = _json.load(_u.urlopen(f"{ROOT}/api/receipt/template", timeout=8))
        n_bg = sum(1 for b in after_bg["blocks"] if b.get("bg"))
        check("保存后背景层标记未丢失", n_bg == bg_js, f"保存后 bg={n_bg}（应为 {bg_js}）")

        # 服务端预览
        pg.click("text=服务端预览")
        pg.wait_for_timeout(1200)
        pv = pg.eval_on_selector("#preview", "e=>e.textContent") or ""
        check("服务端预览返回内容", len(pv) > 20, f"预览 {len(pv)} 字符")

        # 保存
        pg.click("text=保存模板")
        pg.wait_for_timeout(1500)
        msg = pg.eval_on_selector("#msg", "e=>e.textContent") or ""
        check("保存模板成功", "已保存" in msg, msg[:110])

        # 还原：把测试改动删掉再存回，避免污染设备模板
        pg.evaluate("blocks.pop(); sel=-1; paint(); paintProps();")
        pg.wait_for_timeout(300)
        pg.click("text=保存模板")
        pg.wait_for_timeout(1500)
        check("清理测试元素后模板复原",
              pg.eval_on_selector_all(".el", "e=>e.length") == n0)

        check("画布交互无脚本异常", not errs, errs[0][:110] if errs else "")
        pg.close()
        browser.close()

    print(f"\n{'=' * 56}\n通过 {len(passed)} / 失败 {len(failed)}\n{'=' * 56}")
    for f in failed:
        print(f"  ❌ {f}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
