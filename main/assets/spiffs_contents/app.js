/* lucky-page 共享前端脚本（6 页共用一个文件，避免各页各写一份而漂移）
 *
 * 目前只做一件事：**按当前 URL 自动高亮顶部导航的对应项**。
 * 为什么用脚本而不是每页手写 class="on"：手写会漏、会漂移 —— 实测 6 个页面里
 * 只有 2 页有高亮规则，且同一条"小票"链接出现 3 种文案（用户反馈"顶部 menu
 * 选中风格不一致"）。按 URL 判定后，任何页面（含以后新增的）都自动一致。
 *
 * 依赖：外观定义在共享 app.css 的 nav 段（`nav a.on`）。
 */
(function () {
  var path = location.pathname;
  if (path === '/' || path === '') path = '/index.html';   // 根路径等价于首页
  var links = document.querySelectorAll('nav a');
  for (var i = 0; i < links.length; i++) {
    if (links[i].getAttribute('href') === path) {
      links[i].classList.add('on');
    }
  }
})();
