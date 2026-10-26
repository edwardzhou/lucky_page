# UI 重设计 v2 集成 实施计划

设计：`specs/2026-10-26-ui-redesign-integration.md`

## Step 1 页面资源替换
- [ ] 备份旧 `main/assets/spiffs_contents`（已 cp 到 /tmp）
- [ ] 复制新 `ui-design/spiffs_contents/*`（6 html + app.css/js + 4 个新 js）
- [ ] 确认无外链/CDN、无 demo 默认启用（demo.js 仅 ?demo=1 生效）

## Step 2 固件增量字段
- [ ] snapshot + current_page/prize_note（上限 128→160）
- [ ] UpdateSnapshot 填充（last_ 增加 prize_note）
- [ ] LotteryStatusHandler 输出
- [ ] 编译

## Step 3 真机验证
- [ ] `build.sh s3 flash`（SPIFFS + 固件）
- [ ] 6 页 HTTP 200 + 无 404 资源
- [ ] Playwright：控制台零 JS 异常、关键 API 全 200、桌面+手机截图
- [ ] 修复发现的契约/DOM 问题

## Step 4 回归脚本适配
- [ ] `mobile_layout_check.py` WANT 表按新设计更新
- [ ] `web_smoke.py` 选择器按新 DOM 更新（保留"快照→还原"机制）
- [ ] `api_verify.py` 全过

## Step 5 评审与提交
- [ ] `reviews/2026-10-26-ui-redesign-integration.md`（现象/根因/修法/证据）
- [ ] 提交推送（不含 ui-design 原包？按用户要求一并提交）
