# 计划：UI 重设计 v2「设备控制台」

> 设计：`specs/2026-10-23-ui-redesign.md`　|　评审：`reviews/2026-10-23-ui-review.md`

## Task 1 现状评审
- [x] 通读 6 页 HTML + app.css + app.js，梳理功能与意图
- [x] 输出问题清单（P0 语义冲突/误触、P1 层级一致性、P2 密度、P3 可访问性）
- [x] 盘点既有校验断言（69 处 check，117 项）作为不可破坏契约

## Task 2 设计系统 v2
- [x] 语义色分离（brand/ok/warn/danger/info）+ 表面/文字/间距/圆角令牌
- [x] 骨架组件：page-head / card-head / kv / metrics / chip / msg / toast / zone-danger / empty / field / prob-bar
- [x] 兼容层：保留 v1 全部类名与锁定量纲（760/16·12/8×16·14·8/8×10·14·8/h3 15px）
- [x] app.js：aria-current + toast + showMsg/hideMsg

## Task 3 六页改造
- [x] index：页头状态 chip + 指标条 + 开奖区 + 最近结果空状态
- [x] prizes：基础设置两列 + 概率条 + 发完标签 + 删除改按钮 + 危险区 + 底部保存
- [x] receipt：工具条分组 + 画布 + 右栏 + 底部操作栏（保留拖动量纲/抽屉）
- [x] logo：当前状态 kv + 棋盘格预览 + 滑块 + 危险区（删除加确认）
- [x] calibrate：2×2 指标 + ±语义色微调 + 翻页 + 危险区 + 施工判据 + 高级折叠
- [x] wifi：页头 + 状态 kv（保留 artnet 结构）+ 连接操作 + 危险区 + 恢复说明

## Task 4 验证
- [x] `mobile_layout_check.py` → **117 / 0**
- [x] 截图归档 `docs/superpowers/reviews/2026-10-23-ui/`
- [ ] 真机 `web_smoke.py`（设备 192.168.0.252 当前不可达，待在线补跑）
