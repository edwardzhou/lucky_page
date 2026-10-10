# AGENTS.md - Lucky Page（幸运翻页机）

本文件为 AI 编码助手提供项目上下文、**强制规则**，以及本项目在真机上反复验证过的
**硬事实与踩坑铁律**。与工作区其它项目（page-player 等）保持同一套规则体系。

> 详细设计/评审在 `docs/superpowers/`：设计 `specs/2026-09-29-lucky-page-design.md`、
> 计划 `plans/2026-09-29-lucky-page.md`、评审与真机事故记录 `reviews/2026-09-29-review.md`（**34 节，排障先查它**）。

## 项目概述

基于 ESP32-S3 的**抽奖翻页机**：按按钮触发抽奖 → 水果机随机翻页效果 → 命中奖品页 →
TTL 热敏打印机出票；Web 页面配置奖池/小票版式/Logo/标定/WiFi。

- **框架**：ESP-IDF v6.1.0（`~/.espressif/v6.1/esp-idf`；2026-10-10 自 v5.5.4 升级，
  记录见 `docs/superpowers/reviews/2026-10-10-idf61-upgrade.md`。v6 要点：cJSON 走
  组件管理器 `espressif/cjson`、UART/GPIO 组件依赖须显式 `esp_driver_uart`/`esp_driver_gpio`、
  手工导出环境须带 `ESP_IDF_VERSION` 变量）
- **目标**：`esp32s3`；**板型**：Goouuu ESP32-S3-N16R8（16MB Flash + 8MB Octal PSRAM）
- **分区**：`partitions/16m.csv`（nvs 0x9000 / ota_0 3MB / assets(SPIFFS) 3.44MB / replay / **logo 0xD00000**）

## 强制规则

1. **不要自主简化需求，或给偷懒实现**；有疑问先问，不要猜着做一半。
2. **用中文思考和对话**；代码注释、日志、文档一律中文。
3. **设计先行**：新需求先写设计到 `docs/superpowers/specs/`，再写计划到 `plans/`，最后才动代码。
4. **自动评审**：每次代码修改后**自动落一条评审记录**到 `docs/superpowers/reviews/`，
   含"现象 / 根因 / 修法 / 验证证据"，未评审不得进入下一步。
5. **SOLID + 分层**：`main/` 业务不依赖具体硬件；硬件访问走 `printer/`、`dmx512/`、`motion/`；
   **纯逻辑下沉为纯函数**（可上单测宿主），驱动层薄。
6. **TDD**：先写会失败的测试 → 再写最小实现 → 再重构。禁止删测试来"修复"失败，禁止无断言测试。
7. **降级不 abort（本项目最高优先级原则）**：
   **网络、WiFi、SPIFFS、打印机、素材任何异常都只降级，绝不阻断抽奖、绝不让设备变砖。**
   例：WiFi 连不上照常抽奖；缺 SPIFFS 只是 Web 素材不可用；打印机缺纸跳过本张；
   素材图损坏只是不出图。**禁止**在初始化路径用 `ESP_ERROR_CHECK` 让失败变成重启循环。
8. **数据安全**：奖品库存扣减即落盘（`SaveSync`）；用户素材写入失败必须保留旧数据（D15）。
9. **密码/密钥绝不回显**：接口与日志中 WiFi 密码只出现 `has_password` 之类的布尔。

---

## 一、硬件与现场事实（**不要重新试错**）

### 1.1 串口与控制台

| 项 | 值 |
|---|---|
| 控制台 | **USB-Serial-JTAG** `/dev/cu.usbmodem1433101`（**会重枚举**，换口后先 `ls /dev/cu.*`） |
| 板载 USB-UART 桥 | `/dev/cu.usbserial-*`（接 G43/G44，**禁止占用**） |
| 串口读不到日志时 | 先 `python -m esptool --chip esp32s3 -p <PORT> run` 硬复位，再**立刻**读（启动日志只在前 1 秒） |

### 1.2 打印机（UART2）

| 项 | 值 |
|---|---|
| 接线 | **TX=G9 → 打印机 RX**, **RX=G10 ← 打印机 TX**, **CTS=G11**（打印机→ESP32） |
| 参数 | 115200 8N1；`ppins` 判读：G10 应"外部驱动=高"、G11 应"外部驱动=低" |
| 现场命令 | `pstatus ppins pprobe pscan pswap pflow off\|cts\|rts\|both pdump psample` |
| 流控 | **默认 `off`**；`cts` 在部分机型会让写入超时（实测），另有主动限速兜底 |

### 1.3 打印机能力边界（**真机实测，别再假设**）

1. **不支持 ESC/POS 页面模式**：`ESC L / ESC W / ESC FF / ESC S` 发出后机器忽略、仍按行式输出
   → **"文字压在图上"只能靠设备端位图合成**（见下方 canvas 模式）。
2. **状态查询可用**（按位解码，`pprobe` 已实现）：
   - `GS r 1`：bit2..3 = `11` → **纸尽**（`0x00` 有纸 / `0x0F` 纸尽）
   - `DLE EOT 1`：bit3=1 离线、bit5=1 纸尽、bit6=1 出错（`0x16` 在线有纸）
3. **写路径是"只写不读"**：正常出票不查传感器。因此**出票前会探测纸尽**，缺纸则**跳过本张**并告警
   （`PrinterSvc: 打印机缺纸 → 本张跳过`），打印后再复探以捕捉"打一半没纸"。
4. **大帧有两个独立瓶颈，必须同时治**：
   - **发送侧**：`raw_printer_raw_write` 内部硬编码 **1000ms** 超时 → 超过 ~6KB 会被**半途掐断**，
     打印机收到残缺字节流 → **整张小票乱码**。故走 `raw_printer_raw_send`，超时 `500 + size*3/11` ms。
   - **接收侧**：热敏机烧点速度跟不上 UART（115200 ≈ 11.5 字节/ms）→ 需**主动限速**
     （每 512B 等 FIFO 排空 + 18ms ≈ 8 字节/ms）。
5. **乱码 = 数据没完整到**（超时/丢字节），**不是编码问题**。定位手段：看帧大小 vs 超时量纲。

### 1.4 尺寸与耗时基准（版式设计的硬约束）

| 版式模式 | 帧大小 | 实测耗时 |
|---|---|---|
| `flow` 纯文字 | ~0.5 KB | ~1 s |
| `banner` 上下花边 | ~6 KB | ~0.9 s |
| `canvas` 整框位图 | ~32 KB | ~8 s |

- **素材图上限**：宽 ≤576 点（8 的倍数）、**高 ≤672 点**（=28 行）；`kMaxLogoBytes` **由宽高推导**
  （曾两常量各写一份导致"图在尺寸内却被拒收"）。
- 位图带与文字行对齐靠 **`ESC 3 24`（行距=24 点）**；**2 倍字号占 2 行**；
  **二维码占 `ceil(29×模块尺寸/24)` 行**（三者都必须跳/占够行，否则重叠或大片空白）。

### 1.5 翻页机构 / DMX 引脚（同样别再试错）

| 项 | 值 |
|---|---|
| DMX512 | UART1：**TX=G17 / RX=G18 / DE=G4** |
| 按钮 | **GPIO8**（active_high，短按抽奖 / 长按打自检页） |
| IR 接收 | GPIO3（预留） |
| **禁用引脚** | **G43/G44**（板上 TX0/RX0，接 USB-UART 桥）、**G19/G20**（USB D±） |
| 默认机械参数 | 30 页、`step_speed=60`、`min_page_gap_dmx=8`、行程跨度 0..255 |
| 抽奖时长模型 | `expected = 40 × (Σ⌈d/40⌉ − (延迟数−1))` ms（引擎同 tick 内推进，每个延迟少 1 tick）；n=18→4040ms / n=20→4400ms |
| 奖池默认 | 一等×1 / 二等×5 / 三等×30 / 谢谢参与不限，权重 1:5:20:74，耗尽策略 `kFallbackToLose`（落空） |

> 尚未完成真机验证的一环：**DMX 翻页落页是否与票面一致**、30 页标定可用性判据
> （需接上执行器后用「标定」页逐页点动确认）。

---

## 二、代码地图（改哪里）

```
main/
  application.{h,cc}        启动编排、主线程调度（Schedule）、PrintTemplateForTest
  console_cmd.cc            串口命令（真机排障主力）
  settings.{h,cc}           数据驱动字段表（新增字段：加成员 + 字段表加一行）
  printer/
    printer_uart.{h,cc}     UART 收发、纸尽探测、pdump 抓帧（**只保留最后 1024B**）
    printer_service.{h,cc}  打印任务（互斥+任务通知）、缺纸跳过、整帧日志
    receipt_template.{h,cc} v3 块模板模型（三种模式、JSON 读写、校验）
    receipt_render.{h,cc}   渲染核心：BuildCanvas 排版内核 + flow/hybrid/banner 三条路径
    receipt_defaults.{h,cc} 出厂预设（1 经典 / 5 canvas / 6 banner）+ 分槽加载/保存/重置
    bmp_canvas.{h,cc}       位图画布（图形模式的"文字压图"能力，含点阵字库解析）
    logo_store.{h,cc}       素材独立分区读写（CRC 校验 + 从 SPIFFS 一次性迁移）
    printer_logo.cc         LGO1 校验、GS v 0 分块
  web/web_server.{h,cc}     26+ 接口；`MainSync` 单层类型擦除投递主线程
  assets/spiffs_contents/   6 个页面（index/prizes/receipt/logo/calibrate/wifi）
  assets/font24.bin         点阵字库（EMBED 进固件，7542 字形/≈630KB）
test_apps/                  单测宿主（独立分区表！见 §四-6）
tools/gen_border.py         花纹边框素材生成
tools/gen_font.py           点阵字库生成（`--gb2312` 全覆盖）
scripts/api_verify.py       HTTP 接口验收（可复跑）
scripts/web_smoke.py        前端浏览器冒烟（playwright）
```

---

## 三、小票版式体系（三种模式，各槽独立）

| 模式 | 渲染 | 特点 |
|---|---|---|
| `flow` | 全部走打印机自带文字 | 最快最省，无图 |
| `canvas` | **整票位图合成**（`BuildCanvas` → `BmpCanvas` → `GS v 0`） | 文字可与图**逐像素叠加**、四边完整边框；慢、行数受图高限制 |
| `banner` | 上下两段花边位图 + 中间文字 | 快、**行数不限**；代价是画不出左右竖线 |

- **模式是推导值**（`EffectiveMode()`）：`mode_raw` 为空时"有 bg 块→canvas，否则 flow"，
  **不要**依赖存储字段（曾因"代码构造模板忘设 mode"被自己校验拒收）。
- **分槽存储**：`receipt_tmpl`(flow) / `tmpl_canvas` / `tmpl_banner` + `tmpl_mode`；
  `GET /api/receipt/template?mode=0|1|2` 读指定槽（不改生效模式）；
  `PUT` 存进**模板自己的槽并设为生效**；`POST /api/receipt/template/reset {"mode":-1}` 全部重置为预设。
- **页面**：「打测试页」= **所见即所打**（把当前画布内容发给打印机，未保存也照打）；
  「服务端预览」不打印、只显示真实渲染文本。

---

## 四、踩坑铁律（**本会话血泪，违反必再踩**）

1. **批量/正则改写源码后必须核对实际结果**（grep 或实测），`str.replace` 会**静默不匹配**。
   本会话多次"以为改了其实没改"：`open(h,...)` 把内容当路径、正则只替到第一个 `}`、
   补丁锚点不匹配。**改完立即验证**，别相信"脚本没报错"。
2. **新增块字段必须同时改三处**：`ToJson`/`FromJson`、编辑器 `fromServer` 与 `payload`。
   漏一处 → **画布保存一次就丢字段**（曾丢 `bg` → 背景层退化成普通图 → 整票版式崩坏）。
3. **"纯函数规则 + 单测通过" ≠ "规则生效"**：必须接进**调用点**并在集成路径验一次
   （曾写了 `WifiStaCanScan` 却没接到 `Scan()`，已连接时扫描把连接打断）。
4. **排障工具的自身缺陷会伪装成产品缺陷**：
   `pdump` 只留尾部 1024B（帧头的大图看不到）、硬编码 1000ms 超时、测试没滚动导致"拖动失败"。
   → **归因前先做量纲估算**（例：15KB ÷ 11.5B/ms = 1.3s > 1.0s 超时），并换一个独立证据交叉验证。
5. **48KB 级缓冲绝不放栈上**（走 PSRAM 暂存）。曾因栈上 48KB 数组溢出，**把 USB 外设一起拖死、串口都不出字**。
6. **测试宿主必须用独立 NVS 分区**（`test_apps/partitions/test_app.csv`，NVS 在 0xC00000）。
   曾与主固件共用 0x9000 → **跑一次测试就抹掉设备真实配置**（WiFi/奖池/票号全丢）。
7. **前端改动必须整包烧**（HTML 在 SPIFFS 里，`appflash` 不动 SPIFFS）；只改固件用 `appflash`
   （现为 app+分区表）。**素材已独立分区，不再受烧录影响**。
8. **静态响应必须带 `Cache-Control: no-store`**：否则浏览器缓存旧页面 → "改了没生效"。
9. **HTTP 层必须有端到端验收**：后端单测覆盖不到前端 JS。曾出现"所有页面 404"
   （ESP-IDF 默认精确匹配，`/*` 通配必须显式设 `uri_match_fn = httpd_uri_match_wildcard`）。
10. **前端要自诊断**：`window.onerror`/`unhandledrejection` 显示顶部错误条（现场排障不必连电脑）。
11. **高频编辑要防抖 + 单飞**：曾每敲一键发一次预览请求（15 字符 = 16 次）→ 打爆设备 HTTP。
12. **路径穿越防护**：静态文件拼接必须拒绝含 `..` 的请求（已加纯函数 + 单测）。
13. **编辑器元素尺寸用 `minHeight`**：写死高度会裁字（大字要占 2 行、二维码按实际高度占位）。
14. **SPIFFS 的 `rename` 不覆盖已存在目标** → 覆盖写必须先 `remove` 再 `rename`（否则"换个 logo 必失败"）。

---

## 五、验证纪律（**做完必须跑，通过了才能说"完成"**）

| 层级 | 命令 | 通过标准 |
|---|---|---|
| 编译 | `bash scripts/build.sh s3` | `Project build complete` |
| 片上单测 | `cd test_apps && idf.py -B build -p $PORT flash` + 读串口 | **全 PASS / 0 FAIL**，且**单次启动无重启** |
| HTTP 验收 | `python3 scripts/api_verify.py <设备IP>` | 全通过（含负例：坏 JSON→400、路径穿越→302） |
| 前端冒烟 | `python3 scripts/web_smoke.py <设备IP>` | 6 页面无 JS 异常 + 画布交互全通过 |
| 真机打印 | 串口 `psample` | `整帧 N 字节（logo 已包含）` + `打印完成 (ok=1 fail=0)` |
| 纸面 | **人眼确认** | 版式/字迹/二维码正确（自动化验证不到的只有这一项） |

> **跑完片上单测必须把主固件整包烧回**（测试宿主的分区表没有 assets 分区，
> 否则主固件会因找不到 SPIFFS 重启——虽然代码已改成降级不 abort，仍应烧回）。

---

## 六、常用命令速查

```bash
# 构建/烧录
bash scripts/build.sh s3                 # 只编译
bash scripts/build.sh s3 flash           # 整包（含分区表 + SPIFFS 页面）
bash scripts/build.sh s3 appflash        # 只 app + 分区表（**不动 SPIFFS、素材也不受影响**）
bash scripts/build.sh s3 monitor
bash scripts/build.sh s3 merge           # 合并出厂 bin

# 验收
python3 scripts/api_verify.py 192.168.5.72 [--draw]
python3 scripts/web_smoke.py 192.168.5.72
python3 test_apps/run_tests.py           # 片上单测（端口可用 LUCKY_PORT 覆盖）

# 素材生成
python3 tools/gen_border.py              # 花纹边框（384×672）
python3 tools/gen_font.py --gb2312       # 点阵字库（改了要重新 build 才会 EMBED）

# 串口现场命令（真机排障主力）
pstatus      # 打印统计 + 纸状态/缺纸跳过 + 接线
pprobe       # 打印机实时状态（按位解码：在线/纸尽/出错）
ppins pscan pswap pflow off|cts|rts|both  # 引脚/找脚/对调/流控
pdump        # 最近一帧十六进制（**只保留最后 1024B**，大图看帧头要另想办法）
psample      # 按**当前保存的版式**打一张
pdemo 1..6   # 装演示版式（1 经典 2 简洁 3 促销 4 并排 5 水印 6 花边）
ptmpl <json> # 直接装模板（注意：串口行缓冲小，长 JSON 会被截断）
preceipt ptime prizes gbtest wifi [scan|set|off] httptest <path>
```

---

## 七、已知边界与待办

- 本机不支持页面模式 → "文字压图"用 canvas 位图合成实现（慢、帧大是固有代价）
- 边距/密度调整：改 `tools/gen_border.py` 参数重画；行位置在画布上拖
- 纸尽探测使每张票 +约 0.4s（可接受；如需更快可只在连续多张后抽检）
- `pdump` 只留尾部 1024B —— 需要看帧头大图时应借助"整帧字节数"日志或另加分段抓取
