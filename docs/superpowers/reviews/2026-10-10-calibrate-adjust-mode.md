# 评审：标定页改造为「页码调整模式」（学习 page-player）

> 设计 `specs/2026-10-10-calibrate-adjust-mode.md`　|　计划 `plans/2026-10-10-calibrate-adjust-mode.md`
> 参考：`~/esp32_work/page-player`（`tab-adjust.html`、`docs/plans/2026-04-11-req0018-page-adjust-design.md`）

## 现象 / 差距

用户要求"标定学习 page-player 的页码调整模式"。对照参考实现，lucky-page 的差距是：

1. **后端**：`PageAdjust`（每页 int8 偏移，与参考同款模型）**早就存在并已接入**
   （`LoadAdjust`+`Resize(1,pages)` 在启动路径；`DirectDrive` 点动已用 `GetAdjustedValue`
   叠加偏移），`Resize` 也不清已有偏移 —— 但**没有任何 HTTP 接口能读写偏移**
   （只有 `PUT /api/page-mapping` 整表写绝对映射）。
2. **前端**：`calibrate.html` 是"30 行绝对行程值表格"，**没有状态卡、没有 ± 微调、
   没有上页/下页/跳页、没有清零**；且初始值按公式 `lo+i*(hi-lo)/(n-1)` **猜**，
   保存时把猜的值整表覆盖真实映射 —— 而 `GET /api/pages` 其实**早已返回真实 values**，
   注释里"取不到真实值"是过时认知（隐患）。
3. **"当前页"不能用设备值**：`set_current_page` 只在归零路径写，
   `DirectDrive`（点动）**不更新**它 → 调整模式必须用界面本地游标。

## 修法

### 后端：新增 3 个接口（`web_server.{h,cc}`）
| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/adjust` | `{cell_count:1, total_pages, offsets:[[..]], mapping:[..]}`（保持参考的二维形状） |
| PUT | `/api/adjust` | `{cell:0,page,offset}` → 显式校验（cell≠0/page 越界/offset 越界 → 400），返回 `{offset,base,value}` |
| POST | `/api/adjust/reset` | `{cell:0,page}` 清本页；`{cell:-1}` 清全部 |

- 全部经 `MainSync` 走主线程（与既有 PUT 一致），改完立即落盘。
- **显式校验是必须的**：`PageAdjust::SetOffset` 对越界是**静默忽略**，
  不校验则前端会"以为改了其实没改"。
- `PageAdjust` 类**零改动**（"清零本页"= `SetOffset(0,page,0)`，"清零全部"= `ResetAll()`）。

### 前端：`calibrate.html` 重做为调整模式（手机优先）
- **状态卡** 当前页 / 基础值 / 偏移 / 实际值（`实际值 = clamp(基础+偏移, 0, 255)`）
- **微调** `+10/+5/+1` `-1/-5/-10` 大按钮（手机 64px 高）——只改偏移、不翻页
- **翻页** 上页/下页/首页/末页 + 「跳到第 N 页」点动（用户要求新增）
- **清零** 清零本页偏移 / 清零全部偏移
- **高级：基础映射**（`<details>` 默认折叠）保留原表格能力，但**初始值改读真实 `values`**
- `value` 一律以 `PUT /api/adjust` 响应回填，前端不自己推断钳位

## 过程中抓到的两个严重问题（都已修，且加了防复发手段）

### ① 新增路由顶破 `max_uri_handlers` → **所有静态页面 404**
- **现象**：加完 3 条 `/api/adjust` 路由并烧录后，`api_verify` 的 6 个静态页断言**全红**
  （`HTTP 404 Nothing matches the given URI`），而所有 API 正常。
- **根因**：`config.max_uri_handlers = 32`，而路由表 31 条 + 静态 `/` 与 `/*` = **33 条**；
  ESP-IDF 的 httpd 超限时**静默注册失败**（只打一条 ESP_LOGE），于是**最后注册的通配静态路由
  `/*` 没生效** → 静态页面全部 404。设备日志实锤：`E WebServer: 注册 /* 失败` /
  `路由注册完成：32 条（API 31 + 静态 2）`。
- **修法**：① 上限提到 40（`kMaxUriHandlers`）并留余量；
  ② **加编译期断言** `static_assert(kApiRouteCount + 2 <= kMaxUriHandlers, ...)`
  —— 让"再加一条路由就顶破"变成**编译期错误**而不是运行期神秘 404；
  ③ 启动日志改为"成功 N / 应有 M"，不完整时用 `ESP_LOGE` 明确写出"静态页面会全部 404"。
- **验证**：修复后启动日志 `路由注册完成：33/33 条`，6 个页面 `HTTP 200`（含 `app.css`/`app.js` 的 MIME 正确）。

### ② 新增单测文件没被编译 → 测试数没变（**假绿**）
- **现象**：加了 7 个 `PageAdjust` 单测后跑片上单测，仍是 **240 PASSED**（与改造前**一模一样**）。
- **根因**：`test_apps/main/CMakeLists.txt` 用 `file(GLOB TEST_SOURCES ...)`，
  **CMake 只在 configure 时求值**；只跑 `idf.py build` 不会收集新文件。
- **修法/验证**：`idf.py -B build reconfigure` 后构建输出出现
  `Building CXX object ... test_page_adjust.cc.obj`，再跑得 **247 PASSED / 0 FAILED**。
- **教训**：看到"测试数与预期不符"必须停下核对，不能把 240 当通过（本次差点漏过）。

## 验证证据

| 层级 | 命令 | 结果 |
|---|---|---|
| 编译 | `bash scripts/build.sh s3` | `Project build complete`（含 static_assert 护栏） |
| 片上单测 | `test_apps/run_tests.py`（reconfigure 后） | **247 PASSED / 0 FAILED**（新增 7 项 PageAdjust 全过） |
| HTTP 验收 | `api_verify.py 192.168.0.252` | **58/0**（新增 ⑤b 共 11 项：GET 结构、PUT 往返、`value=clamp(base+offset)`、读回一致、还原、4 个负例、清零本页、再次还原） |
| 离线界面校验 | `mobile_layout_check.py` | **73/0**（新增 ②b：状态卡四项、控件齐全、高级表格默认折叠、点 +5 偏移/实际值同步、点 -5 还原） |
| 真机前端冒烟 | `web_smoke.py 192.168.0.252` | **50/0**（新增 ④：状态卡与设备一致、点 +5 设备同步、`clamp` 正确、清零本页、上页/下页游标、无脚本异常、**偏移已还原**） |
| 设备数据 | `GET /api/adjust` 复核 | 30 页偏移**全部为 0**（与测试前一致）；基础映射未被改动 |
| 截图 | `/tmp/lucky-mobile/verify-calibrate.png` | ①状态卡+微调 ②点+5 偏移/实际值实时更新 ③高级表格显示**真实映射值**（P1=0/P2=8/P3=17…） |

## 结论

标定页已具备 page-player 的调整模式闭环（**改偏移 → 翻页 → 眼睛确认 → 清零/还原**），
且基础映射与偏移分离、界面只读真实值、写入立即落盘。回归全绿且未污染设备数据。
