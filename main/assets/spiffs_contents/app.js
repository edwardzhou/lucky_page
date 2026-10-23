/* lucky-page 共享前端脚本（6 页共用，避免各页各写一份而漂移）
 *
 * 提供三件事：
 *   1. **按 URL 自动高亮顶部导航**（并补 aria-current="page"）。
 *      为什么用脚本：手写 class="on" 会漏、会漂移 —— v1 实测 6 页只有 2 页有高亮规则，
 *      同一条"小票"链接出现 3 种文案（用户反馈"顶部 menu 选中风格不一致"）。
 *   2. **toast(text, kind)**：短暂成功/失败反馈（2.6s 自动消失）。
 *      为什么需要：v1 保存成功只是卡片底部一行 12px 小字，手机上一滑就看不见 → 用户重复点击。
 *   3. **showMsg(id, kind, text) / hideMsg(id)**：内联消息条（左侧色条 + 语义色）。
 *
 * 外观全部在 app.css（`.msg` / `#toast` / `nav a.on`），本文件只管行为。
 */
(function () {
  'use strict';

  /* ---------- 1. 导航高亮 ---------- */
  var path = location.pathname;
  if (path === '/' || path === '') path = '/index.html';   // 根路径等价于首页
  var links = document.querySelectorAll('nav a');
  for (var i = 0; i < links.length; i++) {
    if (links[i].getAttribute('href') === path) {
      links[i].classList.add('on');
      links[i].setAttribute('aria-current', 'page');
    }
  }

  /* ---------- 2. toast ---------- */
  var toastEl = null;
  var toastTimer = null;
  window.toast = function (text, kind) {
    if (!toastEl) {
      toastEl = document.createElement('div');
      toastEl.id = 'toast';
      toastEl.setAttribute('role', 'status');
      toastEl.setAttribute('aria-live', 'polite');
      document.body.appendChild(toastEl);
    }
    toastEl.className = kind === 'err' ? 'show err' : kind === 'warn' ? 'show warn' : 'show';
    toastEl.textContent = text;
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { toastEl.className = ''; }, 2600);
  };

  /* ---------- 3. 内联消息条 ---------- */
  function resolve(idOrEl) {
    return typeof idOrEl === 'string' ? document.getElementById(idOrEl.replace(/^#/, '')) : idOrEl;
  }
  window.showMsg = function (idOrEl, kind, text) {
    var el = resolve(idOrEl);
    if (!el) return;
    el.className = 'msg show ' + (kind === 'err' ? 'err' : kind === 'warn' ? 'warn' : 'ok');
    el.textContent = text;
  };
  window.hideMsg = function (idOrEl) {
    var el = resolve(idOrEl);
    if (el) el.className = 'msg';
  };
})();
