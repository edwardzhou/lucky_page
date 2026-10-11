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
import json
import sys
import urllib.request
from playwright.sync_api import sync_playwright

BASE = sys.argv[1] if len(sys.argv) > 1 else "192.168.5.72"
ROOT = f"http://{BASE}"
PAGES = ["/index.html", "/prizes.html", "/receipt.html", "/logo.html",
         "/calibrate.html", "/wifi.html"]

passed, failed = [], []


def check(name, cond, detail=""):
    (passed if cond else failed).append(name)
    print(f"  {'✅' if cond else '❌'} {name}{(' — ' + detail) if detail and not cond else ''}")


def api_get(path):
    """直接读设备接口（用于测试前后快照/比对，不经浏览器）。"""
    import json as _json
    import urllib.request as _u
    with _u.urlopen(f"{ROOT}{path}", timeout=10) as r:
        return _json.load(r)


def api_get_raw(path):
    """读设备接口的**原始响应文本**（密码泄露这类断言必须看原文，不能只看解析后的字段）。"""
    import urllib.request as _u
    with _u.urlopen(f"{ROOT}{path}", timeout=10) as r:
        return r.read().decode("utf-8", "replace")


def api_put_template(obj):
    """把一份模板写回它自己的槽（API 语义：PUT 存进模板自己的槽并设为生效）。"""
    import json as _json
    import urllib.request as _u
    req = _u.Request(f"{ROOT}/api/receipt/template",
                     data=_json.dumps(obj).encode(), method="PUT")
    with _u.urlopen(req, timeout=15) as r:
        return r.status


def clickable_el_index(pg):
    """找一个"中心点确实能点到"的元素下标。

    为什么需要：真实模板里元素会**重叠**（例如钉在同一行的两个块），
    playwright 默认点击会因"被其它元素拦截"而超时；
    离线 mock 模板太简单，暴露不出这个问题 —— 真机回归才发现。
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

    ⚠ 真实踩坑：`touchscreen.tap()` 不产生中间 move，于是"任何 pointermove 都算拖动"
    的实现能通过自动化测试，却让真机轻点永远弹不出属性抽屉（用户反馈的正是这个）。
    """
    pg.mouse.move(x, y)
    pg.mouse.down()
    pg.mouse.move(x + dx, y + dy, steps=2)
    pg.mouse.up()


def main():
    with sync_playwright() as pw:
        browser = pw.chromium.launch()

        print("① 页面加载（无 JS 异常 / 无错误提示条 / 菜单高亮一致）")
        nav_labels, nav_on_bg = [], []
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
                # 顶部菜单一致性（用户反馈"选中风格不一致"）：恰好 1 项高亮且指向本页
                navinfo = pg.evaluate("""()=>{
                    const as=[...document.querySelectorAll('nav a')];
                    const on=as.filter(a=>a.classList.contains('on'));
                    return {labels:as.map(a=>a.textContent.trim()),
                            onHrefs:on.map(a=>a.getAttribute('href')), onCount:on.length,
                            onBg:on[0]?getComputedStyle(on[0]).backgroundColor:null,
                            offBg:as[0]&&!as[0].classList.contains('on')
                                  ?getComputedStyle(as[0]).backgroundColor:null};
                }""")
                check(f"{path} 菜单高亮指向本页",
                      navinfo["onCount"] == 1 and navinfo["onHrefs"][0] == path,
                      f"高亮数={navinfo['onCount']} 指向={navinfo['onHrefs']}")
                nav_labels.append(tuple(navinfo["labels"]))
                nav_on_bg.append(navinfo["onBg"])
            except Exception as ex:                       # noqa: BLE001
                check(f"{path} 可打开", False, str(ex)[:110])
            finally:
                pg.close()

        check("6 页菜单文案序列一致", len(set(nav_labels)) == 1,
              f"{len(set(nav_labels))} 种：{sorted(set(nav_labels))}")
        check("6 页菜单选中底色一致", len(set(nav_on_bg)) == 1,
              f"{len(set(nav_on_bg))} 种：{sorted(set(nav_on_bg))}")

        print("\n② 画布编辑器交互")
        pg = browser.new_page()
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.goto(ROOT + "/receipt.html", wait_until="load", timeout=20000)
        pg.wait_for_timeout(2000)

        # 测试前快照：三个版式槽 + 生效模式。
        # ⚠ 必须整体快照/还原，不能用"pop 一个再存"来清理：脚本中途会 select #mode 切到
        # canvas（blocks 变成 canvas 模板），那时 pop 删掉的是**真实块**并保存 ——
        # 本脚本曾在真机上把 canvas 模板从 18 块删成 17 块、并把生效模式留在 canvas。
        snapshot = {m: api_get(f"/api/receipt/template?mode={m}") for m in (0, 1, 2)}
        active_mode = api_get("/api/receipt/template").get("mode", "flow")
        active_idx = {"flow": 0, "canvas": 1, "banner": 2}.get(active_mode, 0)

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

        # 安全清理：把三个版式槽**整体还原**成测试前快照（生效槽最后写，保证生效模式正确）。
        # 绝不用"pop 掉最后一个块再存"——中途切过 canvas 后 blocks 已是 canvas 模板，
        # 那样会删掉真实块（真机踩过：canvas 18→17 块，生效模式被留成 canvas）。
        put_ok = True
        for m in (0, 1, 2):
            if m == active_idx:
                continue
            try:
                api_put_template(snapshot[m])
            except Exception:                                  # noqa: BLE001
                put_ok = False
        try:
            api_put_template(snapshot[active_idx])
        except Exception:                                      # noqa: BLE001
            put_ok = False
        check("三个版式槽已还原为测试前快照", put_ok)

        # 端到端复核：重新加载页面，元素数应与测试前一致
        pg.reload(wait_until="load")
        pg.wait_for_timeout(2500)
        n_after = pg.eval_on_selector_all(".el", "e=>e.length")
        check("重新加载后模板复原（元素数一致）", n_after == n0, f"{n0} → {n_after}")
        restored = api_get("/api/receipt/template")
        check("生效模式已复原", restored.get("mode") == active_mode,
              f"{active_mode} → {restored.get('mode')}")

        # ⚠ 逐块校验**三个槽**与测试前快照完全一致（不只是块数与生效槽）：
        # 曾经只比对生效槽的块数，掩盖了"其它槽被改/某块字段被改"的漂移
        # （实测发现过 flow 槽 block0 多出 row:0、block10 的 col/row 被改）。
        drift = []
        for m in (0, 1, 2):
            got = api_get(f"/api/receipt/template?mode={m}")
            if got.get("blocks") != snapshot[m].get("blocks"):
                drift.append(f"槽{m}({snapshot[m].get('mode')})")
        check("三个版式槽逐块与快照一致（无字段漂移）", not drift,
              f"发生漂移：{', '.join(drift)}")

        check("画布交互无脚本异常", not errs, errs[0][:110] if errs else "")
        pg.close()

        # ---------------- ③ 手机视口（移动端布局） ----------------
        # 为什么单独跑一遍：手机布局靠 @media(max-width:640px) + 底部抽屉/固定栏，
        # 桌面视口完全看不到这些分支 —— 只在桌面跑就会"改坏了也不知道"。
        # ⚠ 本段**只读不写**（不点保存），不改变设备上的模板。
        print("\n③ 手机视口（390×844，触屏模拟）")
        mctx = browser.new_context(viewport={"width": 390, "height": 844},
                                   has_touch=True, is_mobile=True, device_scale_factor=2)
        mpg = mctx.new_page()
        merrs = []
        mpg.on("pageerror", lambda e: merrs.append(str(e)))
        mpg.goto(ROOT + "/receipt.html", wait_until="load", timeout=20000)
        mpg.wait_for_timeout(2500)

        # 溢出量纲用 documentElement.clientWidth：is_mobile 下 innerWidth 会被
        # shrink-to-fit 放大（实测 598 vs 真实 390），拿它当基准会把溢出判成正常
        geo = mpg.evaluate("""()=>{
            const cv=document.querySelector('#canvas'), ru=document.querySelector('#ruler');
            return {sw:document.documentElement.scrollWidth,
                    iw:document.documentElement.clientWidth,
                    vh:document.documentElement.clientHeight,
                    cv:cv.getBoundingClientRect().width,
                    ruler:ru?getComputedStyle(ru).display:'none',
                    colw:colw};
        }""")
        check("手机视口无横向溢出", geo["sw"] <= geo["iw"] + 1,
              f"scrollWidth={geo['sw']} > 视口 {geo['iw']}")
        check("画布已适配屏幕宽度（无需横向滚动）", geo["cv"] <= geo["iw"] + 1,
              f"canvas={geo['cv']:.0f}px > 视口 {geo['iw']}px")
        check("手机端隐藏标尺", geo["ruler"] == "none", f"display={geo['ruler']}")

        # 触屏点选元素 → 底部抽屉滑出，且属性可编辑
        # 用 touchscreen.tap（真实触摸事件）而非鼠标 —— 手机上是触摸路径
        idx = clickable_el_index(mpg)
        els = mpg.query_selector_all(".el")
        tapped = False
        if idx is not None and idx >= 0 and els:
            bx = els[idx].bounding_box()
            if bx:
                mpg.touchscreen.tap(bx["x"] + min(20, bx["width"] / 2), bx["y"] + bx["height"] / 2)
                tapped = True
        if not tapped and els:
            els[0].click(force=True)          # 找不到可点位置时退化（仍算通过，但标注）
            tapped = True
        check("触屏点选画布元素", tapped,
              "没有可点选的元素" if not tapped else "")
        mpg.wait_for_timeout(600)
        sheet = mpg.evaluate("""()=>{
            const p=document.querySelector('#panels'), r=p.getBoundingClientRect();
            return {open:document.body.classList.contains('sheet-open'),
                    top:Math.round(r.top), vh:document.documentElement.clientHeight};
        }""")
        check("点选元素后属性抽屉展开", sheet["open"] and sheet["top"] < sheet["vh"],
              f"open={sheet['open']} top={sheet['top']} 视口高={sheet['vh']}")
        fs = mpg.eval_on_selector("#pf-text", "e=>parseFloat(getComputedStyle(e).fontSize)")
        check("抽屉输入框字号 ≥16px（防 iOS 聚焦缩放）", fs >= 16, f"font-size={fs}px")

        # ⚠ 真机反馈的核心问题：抽屉从底部弹出会**盖住画布编辑区**，用户看不到自己在编辑
        # 哪个元素（实测抽屉顶 y=339、画布元素从 y=507 起 → 整个画布被盖住）。
        # 修法是弹出后把选中元素滚到抽屉上方；这里锁定该不变量。
        ov = mpg.evaluate("""()=>{
            const el=document.querySelector('.el.sel'), p=document.querySelector('#panels');
            if(!el||!p) return null;
            const er=el.getBoundingClientRect();
            const b=parseFloat(getComputedStyle(p).bottom)||0;
            const sheetTop=document.documentElement.clientHeight-b-p.offsetHeight;
            return {selTop:Math.round(er.top), selBottom:Math.round(er.bottom),
                    sheetTop:Math.round(sheetTop),
                    open:document.body.classList.contains('sheet-open')};
        }""")
        check("选中元素不被属性抽屉遮挡（编辑区可见）",
              bool(ov and ov.get("open") and ov.get("selTop", -1) >= 0
                   and ov.get("selBottom", 1e9) <= ov.get("sheetTop", -1) + 2),
              f"元素 {ov and ov.get('selTop')}~{ov and ov.get('selBottom')} "
              f"抽屉顶 {ov and ov.get('sheetTop')}（被遮挡）")

        # ⚠ 真手指轻点必然带 1~3px 抖动：必须仍然弹出抽屉
        # （用户真机反馈"点击元素没看到属性面板"—— 抖动被误判成拖动）
        mpg.evaluate("sel=-1; paint(); paintProps(); closeSheet();")
        mpg.wait_for_timeout(400)
        els3 = mpg.query_selector_all(".el")
        idx3 = clickable_el_index(mpg)
        if idx3 is not None and idx3 >= 0 and els3:
            bx3 = els3[idx3].bounding_box()
            jittery_tap(mpg, bx3["x"] + min(20, bx3["width"] / 2), bx3["y"] + bx3["height"] / 2)
            mpg.wait_for_timeout(900)
        jit = mpg.evaluate("""()=>({open:document.body.classList.contains('sheet-open'),
                                    sel:!!document.querySelector('.el.sel')})""")
        check("带抖动的真实轻点仍弹出属性抽屉",
              bool(jit.get("open") and jit.get("sel")),
              f"open={jit.get('open')} 选中={jit.get('sel')}")

        # 底栏「属性」保底入口：不依赖手势识别也能打开面板
        if jit.get("open"):
            mpg.click("#propsBtn")
            mpg.wait_for_timeout(500)
            closed = mpg.evaluate("!document.body.classList.contains('sheet-open')")
            mpg.click("#propsBtn")
            mpg.wait_for_timeout(500)
            reopened = mpg.evaluate("document.body.classList.contains('sheet-open')")
            check("底栏「属性」按钮可收起/再打开面板", bool(closed and reopened),
                  f"收起={closed} 再打开={reopened}")

        # 主操作（保存）固定在视口内，不用滚动就能点到
        box = mpg.eval_on_selector("text=保存模板", """e=>{const r=e.getBoundingClientRect();
            return {top:r.top,bottom:r.bottom,h:document.documentElement.clientHeight};}""")
        check("「保存模板」固定在视口内", 0 <= box["top"] and box["bottom"] <= box["h"] + 1,
              f"top={box['top']:.0f} bottom={box['bottom']:.0f} 视口高={box['h']}")

        ta = mpg.eval_on_selector(".el", "e=>getComputedStyle(e).touchAction")
        check("元素禁用浏览器手势（触控拖动前提）", ta == "none", f"touch-action={ta}")

        small = mpg.evaluate("""()=>{
            const sel='nav a, button, input:not([type=checkbox]):not([type=radio]), select, .psel div';
            const bad=[];
            document.querySelectorAll(sel).forEach(e=>{
                const r=e.getBoundingClientRect();
                if(r.width<1||r.height<1) return;
                if(r.height<40) bad.push(e.tagName+(e.id?'#'+e.id:'')+':'+Math.round(r.height));
            });
            return bad.slice(0,5);
        }""")
        check("手机端触控控件高度 ≥40px", not small, f"偏小：{small}")
        check("手机视口无脚本异常", not merrs, merrs[0][:110] if merrs else "")
        mctx.close()

        # ---------------- ③b WiFi 页：学习 artnet 的进阶能力（安全项）----------------
        # ⚠ 只做**不危及连通性**的检查：改热点名只影响 AP 接口、静态 IP 只测校验负例、
        #    真实断开与真实静态 IP 应用改由串口验证（见 reviews/2026-10-10-wifi-artnet.md）。
        print("\n③b WiFi 页：进阶控件与「已连接时扫描被拒」白名单")
        wctx = browser.new_context(viewport={"width": 390, "height": 844},
                                   has_touch=True, is_mobile=True, device_scale_factor=2)
        wpg = wctx.new_page()
        werrs = []
        wpg.on("pageerror", lambda e: werrs.append(str(e)))
        wpg.goto(ROOT + "/wifi.html", wait_until="load", timeout=20000)
        wpg.wait_for_timeout(2000)
        wctl = wpg.evaluate("""()=>{
            const ids=['apEditBtn','apPassEditBtn','apPass','apPassSave','apPassReset','ipEditBtn','discBtn','apSsid','stIp','stMask','stGw','scanBtn','ipModeTag'];
            const miss=ids.filter(i=>!document.getElementById(i));
            const w=document.getElementById('apName');
            const row=document.querySelector('.status-row');
            const lab=row&&row.querySelector('.status-label'), val=row&&row.querySelector('.status-value');
            const rb=row?row.getBoundingClientRect():null;
            const scan=document.getElementById('scanBtn'), card=document.querySelector('.card');
            const sb=scan?scan.getBoundingClientRect():null, cb=card?card.getBoundingClientRect():null;
            return {miss:miss, apText:w?w.textContent.trim().slice(0,40):'',
                    modeTag:(document.getElementById('ipModeTag')||{}).textContent||'',
                    labelLeft: (lab&&rb)? (lab.getBoundingClientRect().left-rb.left) : -1,
                    valueRight: (val&&rb)? (rb.right-val.getBoundingClientRect().right) : -1,
                    valWeight: val? getComputedStyle(val).fontWeight : '',
                    scanFull: sb?sb.width:0, cardW: cb?cb.width:0,
                    bodyMaxW: getComputedStyle(document.body).maxWidth,
                    titleBorder: (()=>{const t=document.querySelector('.card-title');
                                       return t?getComputedStyle(t).borderBottomWidth:'';})()};
        }""")
        check("WiFi 页进阶控件齐全（改名/IP/断开/扫描）", not wctl["miss"], f"缺失：{wctl['miss']}")
        check("WiFi 页显示本机热点名", len(wctl["apText"]) > 3, f"apName={wctl['apText']!r}")
        # AP 密码：可改 + **绝不回显**（接口只回布尔）
        dev_raw = api_get_raw("/api/wifi")
        dev_wifi = json.loads(dev_raw)
        pass_keys = sorted(k for k in dev_wifi if "pass" in k)
        check("⭐ /api/wifi 只回 ap_pass_custom，不含任何密码字段",
              pass_keys == ["ap_pass_custom"], f"含密码相关字段={pass_keys}")
        check("ap_pass_custom 是布尔", isinstance(dev_wifi.get("ap_pass_custom"), bool),
              f"{dev_wifi.get('ap_pass_custom')!r}")
        # 界面显示必须与设备真实状态一致：未自定义 → 显示默认密码；已自定义 → 只显示"已自定义"
        pstate = wpg.eval_on_selector("#apPassState", "e=>e.textContent.trim()") or ""
        custom = bool(dev_wifi.get("ap_pass_custom"))
        check("AP 密码状态行与设备一致（默认/已自定义）",
              ("12345678" in pstate) if not custom else ("已自定义" in pstate),
              f"页面={pstate!r} 设备自定义={custom}")
        check("⭐ 已自定义时不显示明文密码",
              (not custom) or ("12345678" not in pstate and "pass" not in pstate.lower()),
              f"页面={pstate!r}")
        check("IP 模式标签有内容（DHCP/静态）", wctl["modeTag"] in ("DHCP", "静态"), f"{wctl['modeTag']!r}")
        # artnet 布局不变量（真机复核）
        check("真机 WiFi 页宽度与其它页一致（760px）", wctl["bodyMaxW"] == "760px", f"{wctl['bodyMaxW']}")
        check("真机状态行「标签左 / 值右 + 值加粗」",
              wctl["labelLeft"] < 4 and wctl["valueRight"] < 4 and int(wctl["valWeight"] or 0) >= 600,
              f"left={wctl['labelLeft']:.0f} right={wctl['valueRight']:.0f} weight={wctl['valWeight']}")
        check("真机卡片标题带下边框分隔", wctl["titleBorder"] not in ("0px", ""), f"{wctl['titleBorder']}")
        check("真机主操作按钮通栏",
              abs(wctl["scanFull"] - (wctl["cardW"] - 32)) < 3,
              f"按钮宽={wctl['scanFull']:.0f} 卡片内容宽={wctl['cardW']-32:.0f}")

        # 已连接路由器时，扫描必须被状态白名单拒绝并给出原因（既有保护不能因异步化丢失）
        wpg.click("#scanBtn")
        wpg.wait_for_timeout(2500)
        scmsg = (wpg.eval_on_selector("#scmsg", "e=>e.textContent") or "").strip()
        dev = api_get("/api/wifi")
        if dev.get("sta_connected"):
            check("已连接时扫描被拒绝并给出原因", "已连接" in scmsg or "中断" in scmsg, f"提示={scmsg!r}")
        else:
            check("未连接时扫描可用出结果", "找到" in scmsg or "扫描" in scmsg, f"提示={scmsg!r}")
        check("WiFi 页无脚本异常", not werrs, werrs[0][:110] if werrs else "")
        wctx.close()

        # ---------------- ④ 标定页：页码调整模式 ----------------
        # 参考 page-player 的行程微调：改偏移不翻页、状态卡实时显示、可清零。
        # ⚠ 本段会**写设备偏移**，故：先快照 → 改 → 校验 → 还原 → 再核对；
        #    任何异常都走 finally 兜底还原（设备数据不是测试夹具，见 AGENTS §四-19）。
        print("\n④ 标定页：页码调整模式（改 → 校验 → 还原）")
        adj_ctx = browser.new_context(viewport={"width": 390, "height": 844},
                                      has_touch=True, is_mobile=True, device_scale_factor=2)
        cpg = adj_ctx.new_page()
        cerrs = []
        cpg.on("pageerror", lambda e: cerrs.append(str(e)))
        orig_off, cur_page = None, 0
        try:
            cpg.goto(ROOT + "/calibrate.html", wait_until="load", timeout=20000)
            cpg.wait_for_timeout(2500)

            cur_page = int(cpg.evaluate("()=>cur") or 0)
            adj0 = api_get("/api/adjust")
            orig_off = adj0["offsets"][0][cur_page]
            check("标定页状态卡与设备偏移一致",
                  int(cpg.eval_on_selector("#stOff", "e=>parseInt(e.textContent,10)||0")) == orig_off,
                  f"界面={cpg.eval_on_selector('#stOff', 'e=>e.textContent')} 设备={orig_off}")

            cpg.click("#adj_p5")                       # 只改偏移，不翻页
            cpg.wait_for_timeout(1200)
            ui_off = int(cpg.eval_on_selector("#stOff", "e=>parseInt(e.textContent,10)||0"))
            ui_act = int(cpg.eval_on_selector("#stAct", "e=>parseInt(e.textContent,10)||0"))
            ui_base = int(cpg.eval_on_selector("#stBase", "e=>parseInt(e.textContent,10)||0"))
            dev_off = api_get("/api/adjust")["offsets"][0][cur_page]
            check("点「+5」→ 设备偏移 +5 且界面同步",
                  ui_off == orig_off + 5 and dev_off == orig_off + 5,
                  f"界面 {orig_off}→{ui_off}，设备 {orig_off}→{dev_off}")
            check("实际值 = clamp(基础值+偏移)",
                  ui_act == max(0, min(255, ui_base + ui_off)),
                  f"基础={ui_base} 偏移={ui_off} 实际={ui_act}")

            # 「清零本页」应只清当前页，且界面归零
            cpg.click("#clrPage")
            cpg.wait_for_timeout(1000)
            check("「清零本页偏移」生效",
                  api_get("/api/adjust")["offsets"][0][cur_page] == 0
                  and int(cpg.eval_on_selector("#stOff", "e=>parseInt(e.textContent,10)||0")) == 0)

            # 上页/下页：本地游标必须跟着走（设备 current_page 不参与判定）
            before_cur = cur_page
            cpg.click("#navNext" if cur_page + 1 < adj0["total_pages"] else "#navPrev")
            cpg.wait_for_timeout(1500)
            after_cur = int(cpg.evaluate("()=>cur") or 0)
            check("上页/下页更新界面当前页",
                  after_cur != before_cur and int(cpg.eval_on_selector("#stCur", "e=>parseInt(e.textContent,10)||0")) == after_cur + 1,
                  f"{before_cur} → {after_cur}")
            check("标定页无脚本异常", not cerrs, cerrs[0][:110] if cerrs else "")
            cpg.screenshot(path="/tmp/lucky-mobile/device-calibrate.png", full_page=False)
        finally:
            # 兜底还原：把当前页偏移写回测试前的值（不清零 —— 那是用户数据）
            if orig_off is not None:
                try:
                    req = urllib.request.Request(
                        f"{ROOT}/api/adjust",
                        data=json.dumps({"cell": 0, "page": cur_page, "offset": orig_off}).encode(),
                        method="PUT")
                    urllib.request.urlopen(req, timeout=10).read()
                except Exception:                              # noqa: BLE001
                    pass
            adj_ctx.close()

        if orig_off is not None:
            final = api_get("/api/adjust")["offsets"][0][cur_page]
            check("偏移已还原为测试前值（不污染设备）", final == orig_off,
                  f"应为 {orig_off}，实为 {final}")

        browser.close()

    print(f"\n{'=' * 56}\n通过 {len(passed)} / 失败 {len(failed)}\n{'=' * 56}")
    for f in failed:
        print(f"  ❌ {f}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
