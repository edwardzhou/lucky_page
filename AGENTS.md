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
- **分区**：`partitions/16m.csv`（nvs 0x9000 / ota_0 3MB / assets(SPIFFS) 3.44MB / replay /
  **logo 0xD00000(128KB)** / **media 0xD20000(2.875MB，素材库 48 槽 × 48KiB)**）

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
| **控制台逐键输入** | 2026-10-26 起控制台按"累积到换行"执行（曾因 USB-JTAG VFS 非阻塞把半行当命令 → 每敲一键回「未知命令」，评审 38）。脚本发命令用 `python3 scripts/serial_cmd.py <命令>` 最稳（自动处理打开端口复位/首行污染） |
| **设备"不可达"先查两件事** | ① **当前烧的是哪套固件**：跑完 `run_tests.py`（片上单测宿主）若不烧回主固件，设备**没有 Web、没有 STA 重连、控制台也不认 `wifi/rstbtn`**（日志里只有 `test_*.cc:PASS` + `Returned from app_main`）→ 表现就是"完全不可达"。② **开发机是否还在同一 WiFi**：本机曾漫游到别的网络（192.168.3.x）却一直在 ping 192.168.0.252。③ 打开串口若未显式 `dtr=False; rts=False`，会因拉了 EN/GPIO0 把芯片**带进下载模式**（应用停机、串口全静默）——`esptool run` 可复位回来 |

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
| 抽奖按钮 | **GPIO8**（**active_low/按下=0**：内部上拉 + 按下接地）；**软件去抖 30 ms** + **触发限流 1000 ms**（`ButtonGate` 纯逻辑，8 项单测）；短按抽奖 / 长按打自检页（各自一个闸门）；串口 `btbtn` 自检 |
| IR 接收 | GPIO3（预留） |
| **恢复默认配置按钮** | **GPIO5**（上拉输入、**按下为低**、**长按 5 秒**）→ 复位**网络**配置：热点名/密码(=12345678)/已保存网络/IP=DHCP；**开机 30 s 内忽略**；奖池/模板/标定**不动** |
| **禁用引脚** | **G43/G44**（板上 TX0/RX0，接 USB-UART 桥）、**G19/G20**（USB D±） |
| 默认机械参数 | 30 页、`step_speed=60`、`min_page_gap_dmx=8`、行程跨度 0..255 |
| 抽奖时长模型 | `expected = 40 × (Σ⌈d/40⌉ − (延迟数−1))` ms（引擎同 tick 内推进，每个延迟少 1 tick）；n=18→4040ms / n=20→4400ms |
| 奖池默认 | 一等×1 / 二等×5 / 三等×30 / 谢谢参与不限，权重 1:5:20:74，耗尽策略 `kFallbackToLose`（落空） |

> 尚未完成真机验证的一环：**DMX 翻页落页是否与票面一致**、30 页标定可用性判据
> （需接上执行器后用「标定」页逐页点动确认）。

### 1.6 标定 = 「页码调整模式」（学习 page-player）

| 项 | 值 |
|---|---|
| 数据模型 | **基础映射**（`PageMapping`，理论基准）+ **每页偏移**（`PageAdjust`，int8）；实际 DMX = `clamp(基础+偏移, 0, 255)` |
| 持久化 | 偏移存 `/spiffs/page_adjust.json`；每次改动**立即落盘** |
| 接口 | `GET /api/adjust`（offsets 二维 + mapping）、`PUT /api/adjust`（`{cell:0,page,offset}` → 返回 `{offset,base,value}`）、`POST /api/adjust/reset`（`{cell:0,page}` 清本页 / `{cell:-1}` 清全部）|
| 界面 | `calibrate.html`：状态卡（当前页/基础值/偏移/实际值）+ ±10/±5/±1 微调（**只改偏移不翻页**）+ 上页/下页/首页/末页/跳页 + 清零；「高级：基础映射」折叠区保留整表编辑能力 |
| 单机 | `cell_count` 恒为 1；接口保留 `cell` 字段与二维 `offsets` 形状，便于将来多机扩展 |
| **点动已叠加偏移** | `Application::DirectDrive()` 用 `GetAdjustedValue(0,page)` —— 所以「微调 → 点动 → 眼睛确认」闭环成立 |
| **`current_page` 不可用于标定** | `set_current_page` 只在**归零**路径写，`DirectDrive` 点动**不更新**它 → 标定界面的"当前页"必须用**界面本地游标**（加载时读一次作初值） |
| 校验纪律 | `PUT /api/adjust` 必须显式校验（cell≠0 / page 越界 / offset 超出 -128..127 → 400）：`PageAdjust::SetOffset` 对越界是**静默忽略** |

### 1.7 WiFi 进阶能力（学习 artnet-controller，2026-10-10）

| 项 | 值 |
|---|---|
| 接口 | `PUT /api/wifi/ap`（改热点名 **和/或** 热点密码 `ap_pass`，空串=复原默认）· `PUT /api/wifi/ip`（`{dhcp:true}` 或 `{ip,netmask,gateway}`）· `POST /api/wifi/disconnect`（断开**保留凭据**）· `POST /api/wifi/reconnect`（用已保存凭据重连）· `POST /api/wifi/scan`（**启动异步扫描**）· `GET /api/wifi/scan`（轮询 `{running}` / `{done,aps[]}`） |
| 持久化 | `ap_ssid`（空=默认 `lucky-page-XXXX`）· `ip_mode`（0=DHCP/1=静态）· `st_ip`/`st_mask`/`st_gw` |
| 安全校验 | 掩码必须**连续 1**、网关必须与 IP **同网段**、网关≠本机 IP；热点名 1..31 字节且无控制字符/首尾空格（都是"配错就失联"的现场事故点）|
| **断开要真的断** | `s_user_disconnected` 置位后退避任务**不再自动重连**，直到 `ApplySta`/`ReconnectSta` 或重启（否则 1 秒后就连回来，功能是假的）|
| **扫描白名单不放松** | 异步扫描沿用 `WifiStaCanScan`：已连接时仍拒绝（会打断连接），提示语不变 |
| **密码不回显 → 必须有 reconnect 接口** | 前端拿不到已保存密码，若无 `POST /api/wifi/reconnect`，"断开"在界面上就是**单向门**（只能靠串口救）|
| **`esp_netif_ip_info_t` 是 lwIP 内部序** | 必须 `IP4_ADDR(&info.ip, o1,o2,o3,o4)`；直接赋 `ParseIpv4` 的"大端数值"会**字节反序**（真机事故：设完显示 252.0.168.192，若非热点兜底即失联）|
| 串口兜底 | `wifi disc`/`wifi on`/`wifi ascan`/`wifi ap <名>`/`wifi ip <ip> <mask> <gw>`/`wifi dhcp`；`httptest [GET\|POST\|PUT\|DELETE] <path>`（**断开后只有本机 AP 可达，这是唯一的验证/救援通道**）|
| **AP 密码可改**（用户要求）| 页面状态行「AP 密码」+「修改」内联编辑；`wifi appass <密码>` 串口改；校验 **8..63 字节**、无控制字符；**空串 = 复原默认 12345678**（不是开放网络，`authmode` 恒 WPA2）；立即生效且重启仍生效 |
| **密码绝不回显**（强制规则 9）| HTTP 只回 `ap_pass_custom` 布尔、页面只显示"默认 12345678 / 已自定义（不显示）"、**启动日志不打明文**；三处断言锁定"响应/页面文本不含明文"；**串口 `wifi` 打印真实密码**（物理接触才是救援通道，页面文案里明示这条出路）|
| 串口读**驱动**而非 Settings | `ApPasswordForConsole()` 用 `esp_wifi_get_config(WIFI_IF_AP)` 读当前生效值 —— "Settings 里有值" ≠ "驱动接受了"，只有读驱动才能验证真的应用上（改名/静态 IP 同理，验证时优先读设备实际状态）|
| **G4 是 DMX DE，勿复用** | 恢复按钮原本要求用 G4，但它已是 `DMX512_DE_PIN`（输出、初始化拉高）→ 改用 **GPIO5**；新增功能脚必须先核对 §1.5 引脚表并加编译期 static_assert |
| 网络复位范围 | `Settings::ResetNetworkConfig()` 只清 9 个网络字段；**用户数据（奖池/三模板/标定/计数/票号）有哨兵单测锁死不许碰**；按钮与 `wifi resetnet` 同一函数 |
| **复位是真操作，别把自己的验证当事故** | 复位会按设计清掉网络配置；**验证复位功能的脚本必须自己负责恢复**（页面上重填最稳；串口连续 `wifi set` 会撞 `ESP_ERR_WIFI_STATE`，且可能让 STA 卡在"扫描返回 0 个 AP"，**重启即恢复**）。曾因为"脚本自己复位 + 恢复失败"而一路追查一个不存在的"神秘触发"——**先复盘自己的操作时间线，再怀疑硬件** |
| **GPIO5 有常驻"引脚黑匣子"** | 10 ms 周期采样，**任何一次电平跳变都打 `W App` 日志**（含开机毫秒与累计低电平），`rstbtn` 可读 `累计跳变/累计低电平/最近跳变/当前电平`。作用：把"没人按却被复位"从"只能推断"变成"可直接取证"——跳变=0 即证明引脚从未被拉低（问题在软件侧）；低电平≈5000 ms 即证明电气上真的被拉低了 |
| **复位必须过"可信性闸门"**（2026-10-11 血泪）| 真机在**无人接触**时跑过一次复位（`net_rst_cnt 0→1`，NVS 有时间戳；脚本/重启/串口/HTTP/用户操作/空闲噪声**逐条排除**，只剩"驱动误报长按"）。现在复位前必须同时满足：**① 我们亲眼见过 `PRESS_DOWN`**（幽灵事件不会先发按下）**② 引脚连续 8×10ms 采样全为低 ③ 已过开机 30s 守卫**；不满足则拒绝并打 `E App` 显眼日志 + 累加 `幽灵长按拒绝次数`（`rstbtn` 可读）。复位日志带**来源标记**（按钮/串口）。逻辑在纯函数 `main/reset_gesture.{h,cc}`，6 项单测 |
| **复位有持久化证据** | `network_reset.cc` 会写 `net_rst_cnt`/`net_rst_epoch`，WiFi 页显示"曾被恢复默认 N 次 + 最近时间"；配合**开机 30 s 守卫**与**按下/松开/长按日志**，以及串口 `rstbtn`/`rstmon [秒]` 引脚监视 —— 用于排查"没人按却被复位"这类无日志现象（曾有一次被我误判为神秘触发，实为**我自己的验证脚本**所清；现已有计数/时间/按键日志/引脚监视四重证据，5 分钟可定性）|
| 凭据被清也能救回 | NVS 是**日志结构**：字段被覆盖前旧条目仍在。真机事故中就是用 `esptool read_flash 0x9000 0x4000` 读 NVS、提取明文 JSON 捞回两台已保存网络的 SSID 与明文密码（恢复时**低优先网络先 set**，保持原优先级顺序）。**凭据本身不写进仓库**：只记录方法与命令，真值留在设备 NVS，需要时按上述 `read_flash` 流程现场读取 |
| **页面布局 = artnet 风格**（用户指定）| 页标题 + 卡片标题带下边框 + 状态行（标签左/加粗值右 + 「修改」小按钮）+ **通栏主操作按钮**（`.action-btn.primary/danger`）+ 居中空状态提示 + 内联编辑区（`.ip-edit-area`）；**保留本项目菜单与配色**，不采用 artnet 的 bottom tab 与配色 |
| 页面内容 | 2026-10-26 起新增第 7 页 `pages.html`：每页文字 + 图片（从素材库选）；模板块 `page_text`/`page_image`；图片仅 canvas 生效；接口 `/api/assets`、`/api/pages/content` |
| 宽度 | **6 页统一 `--w-page:760px`**；唯一例外是小票编辑页 `body.wide`（要与属性栏并排）。WiFi 页曾短暂用 480px 窄列（artnet 观感），用户要求"与其他页一致"后已取消 → **宽度例外必须显式枚举进断言**（`mobile_layout_check.py` 的 `WANT` 表 + `web_smoke.py` 真机复核），不能悄悄放宽 |
| 风险操作验证纪律 | "真的断开一次""真的设静态 IP"这类会改变可达性的验证**走串口**，HTTP 只测校验负例与可逆项（改热点名只影响 AP 接口，安全）|

---

## 二、代码地图（改哪里）

```
main/
  application.{h,cc}        启动编排、主线程调度（Schedule）、PrintTemplateForTest
  console_cmd.cc            串口命令（真机排障主力）
  settings.{h,cc}           数据驱动字段表（新增字段：加成员 + 字段表加一行）
  wifi/wifi_manager.{h,cc}  AP+STA 管理、退避重连、异步扫描、断开/重连、热点改名、静态 IP
  wifi/wifi_config.{h,cc}   纯函数层（凭据/热点名/IPv4 与静态 IP 校验、状态白名单、退避、JSON）
  printer/
    printer_uart.{h,cc}     UART 收发、纸尽探测、pdump 抓帧（**只保留最后 1024B**）
    printer_service.{h,cc}  打印任务（互斥+任务通知）、缺纸跳过、整帧日志
    receipt_template.{h,cc} v3 块模板模型（三种模式、JSON 读写、校验）
    receipt_render.{h,cc}   渲染核心：BuildCanvas 排版内核 + flow/hybrid/banner 三条路径
    receipt_defaults.{h,cc} 出厂预设（1 经典 / 5 canvas / 6 banner）+ 分槽加载/保存/重置
    bmp_canvas.{h,cc}       位图画布（图形模式的"文字压图"能力，含点阵字库解析）
    logo_store.{h,cc}       素材独立分区读写（CRC 校验 + 从 SPIFFS 一次性迁移）
    printer_logo.cc         LGO1 校验、GS v 0 分块
  web/web_server.{h,cc}     38 个接口（含标定 `/api/adjust` 与 WiFi 进阶 `/api/wifi/ap|ip|
                            disconnect|reconnect|scan`）；
                            `MainSync` 单层类型擦除投递主线程；`kMaxUriHandlers` 有 static_assert 护栏
  page_mapping.{h,cc}       页码→DMX 基础映射（理论基准）
  page_adjust.{h,cc}        每页 int8 行程**偏移**（实际值 = clamp(基础+偏移, 0, 255)）
  assets/spiffs_contents/   7 个页面（index/prizes/receipt/logo/calibrate/wifi/pages）
                            **UI v2「Lucky Studio」**（2026-10-26 起）：侧栏+顶栏+工作台
                            卡片布局，app.css 39KB 设计系统 + workspace/prizes/demo/
                            page-enhancements 四个脚本；demo.js 仅 ?demo=1 生效。
                            旧 760px 单列布局已废弃，布局断言见 mobile_layout_check.py
  assets/spiffs_contents/app.css  设计系统 v2（6 页唯一来源）：
                            §1 设计令牌（**语义色分离**：brand/ok/warn/danger/info + 表面/尺度）
                            §2 基础层（body/排印/按钮/表单/表格/旧提示类兼容）
                            §3 骨架组件（.page-head / .card-head / .kv / .metrics / .chip /
                                      .msg / #toast / .zone-danger / .empty / .field / .prob-bar / .seg）
                            §4 导航（品牌 + tab，选中态由 app.js 按 URL 判定并打 aria-current）
                            §5 底栏 → §6 手机层（≤640px：44px 触控/16px 输入/安全区/抽屉/宽表转卡片）
                            ⚠ 被 mobile_layout_check 锁定的量纲：760px · 卡片 16/12 ·
                            按钮 8×16·14·8 · 输入 8×10·14·8 · h3 15px —— 改前先看断言
  assets/spiffs_contents/app.js   共享脚本（按 URL 自动高亮当前页菜单，避免各页手写漂移）
  assets/font24.bin         点阵字库（EMBED 进固件，7542 字形/≈630KB）
test_apps/                  单测宿主（独立分区表！见 §四-6）
tools/gen_border.py         花纹边框素材生成
tools/gen_font.py           点阵字库生成（`--gb2312` 全覆盖）
scripts/api_verify.py       HTTP 接口验收（可复跑）
scripts/web_smoke.py        前端浏览器冒烟（playwright，含手机视口段）
scripts/mobile_layout_check.py  手机布局离线校验（本地 mock，不需真机，出截图）
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
15. **前端改动必须跑手机视口校验**：`python3 scripts/mobile_layout_check.py`
    （本地静态服务 + playwright mock `/api/**`，**不需要真机**）—— PC 布局肉眼看不出的
    手机问题（横向溢出/触控太小/输入框触发 iOS 缩放）只有它能拦住；截图输出在 `/tmp/lucky-mobile/`。
16. **手机端约定（改前端必须遵守，勿退回 PC 布局）**：
    - **组件样式一律走 `app.css` 基础层，页面内联样式只写"页面特有"部分**。
      曾因 6 页各写一份 `:root/body/.card/button/input` → PC 实测内容宽度 480/560/620/720/760/全宽、
      卡片内距 6 种、按钮内距 6 种（用户反馈"各页风格不一致"）。
      改组件外观 = 改 `app.css` 的令牌或基础层，**不要**在页面里重定义。
      内容宽度统一 `--w-page:760px`；**唯一例外**是小票编辑页 `body.wide`（要与属性栏并排）。
      例外必须**显式枚举进断言**（`mobile_layout_check.py` 的 `WANT` 表 + `web_smoke.py` 真机复核），
      不能悄悄放宽；曾给 WiFi 页开过 480px 窄列，用户要求"与其他页一致"后取消。
    - **一致性由探针断言锁定**：`mobile_layout_check.py` 往每个页面注入
      `.card/h3/button/input` 探针并比对跨页计算样式 —— 不要改成"取页面第一个按钮"，
      抽奖大圆按钮/标定微调大按钮/WiFi `.mini` 都是**有意的变体**，那样会误判。
    - 6 个页面都在内联 `<style>` **之后**引入共享 `app.css`；移动端规则**全部**写在
      `@media(max-width:640px)` 内 → 桌面端零影响。
    - 页面**自身**的 `@media` 块必须放在该页样式表**末尾**（媒体查询不提升优先级，
      放前面会被后面的基础规则盖掉 —— 本项目踩过：`#ruler` 隐藏失效、固定栏不生效）。
    - 小票编辑页手机端：画布**默认适应宽度**（`fitCanvas()` 按可用宽度反算列宽）、
      属性面板是**底部抽屉** `#panels`（点选元素滑出）、主操作是**底部固定栏** `.card.actions`
      （复用同一批按钮，**不许复制 DOM** —— 否则冒烟测试的文本选择器会命中多个）。
    - **抽屉绝不能盖住正在编辑的元素**（真机反馈过）：底部固定抽屉弹出后必须把选中元素
      滚到抽屉上方可见带（`ensureSelectedVisible()`），且**只有轻点选中才弹抽屉、拖动时不弹**
      （`pointerdown` 就弹会在手指下盖一层，连拖动都受影响）；抽屉打开时 `.layout`
      追加 `padding-bottom:52vh`，否则最底部元素滚不上去。不变量已由两个校验脚本锁定。
    - **"轻点"必须容忍真手指抖动**：真手指点一下必然带 1~3px 位移并产生 `pointermove`，
      若把任何指针移动都当拖动，真机轻点就永远弹不出面板
      （真机事故：面板点不出来）。判定阈值 `TAP_SLOP=6`，位移以按下点为基准。
      **测试必须用 `jittery_tap()`（down→move 2px→up），`touchscreen.tap()` 不产生中间 move，
      会绕过这条路径而假绿**。底栏另有「属性」按钮作保底入口，不依赖手势识别。
    - **顶部菜单只有一个来源**：外观在共享 `app.css` 的 nav 段，**哪一项高亮由 `app.js`
      按 URL 自动判定**（不在页面里手写 `class="on"`）。曾因 6 页各写一份 → 只有 2 页有高亮、
      同一条链接 3 种文案（真机反馈"选中风格不一致"）。改菜单只需改 `app.css`/`app.js`。
    - 桌面端 `colw=14/lineh=22` **不可自动改**：`web_smoke.py` 的拖动断言按
      `28px=2 列 / 22px=1 行` 写死。
    - 量纲基准用 `document.documentElement.clientWidth/clientHeight`，
      **不要用 `window.innerWidth/innerHeight`**：手机模拟下会被 shrink-to-fit 放大
      （实测 598×1295 vs 真实 390×844），会把"已溢出"判成正常。
17. **回归脚本的清理必须"整体快照 → 整体还原"，禁止用 pop/猜测哪个是测试数据**。
    `web_smoke.py` 曾在真机上**删掉真实模板块**：脚本中途切到 canvas 槽后 `blocks`
    已换成 canvas 模板，此时 `blocks.pop()` 删的是真实块并把生效模式留在 canvas。
    现在测试前快照 3 个槽 + 生效模式，结束时整体还原（生效槽最后写）再复载复核。
18. **测试要能在真实数据上工作，离线 mock 必须包含"重叠元素"这类真实特征**：
    `web_smoke.py` 的 `query_selector_all(".el")[0].click()` 在真机模板上超时——
    元素重叠被上层拦截；离线 mock 模板太简单，掩盖了它。
    现用 `clickable_el_index()`（`document.elementFromPoint` 找真正可点元素）。
    **轻点类断言一律用 `jittery_tap()`**（见 §四-16）；`touchscreen.tap()` 只用于
    需要"干净触摸事件"的场合，不能单独代表真手指轻点。
19. **设备数据不是测试夹具：回归前确认没人在编辑，还原前先问**。
    回归脚本会"快照 → 测试 → 还原"模板，**与用户手机上的手工编辑并发时会互相覆盖**
    （真实事故：曾把"模板与备份不一致"误判成脚本写坏，用备份还原，
    可能覆盖了用户当时的编辑；实际差异是用户自己改的 —— 两次差异内容各不相同、
    都是语义明确的人工编辑，而逐步隔离复测证明代码路径零写入）。
    另外：只比"块数/元素数"会漏掉字段级漂移，故 `web_smoke.py` 结束时对
    **三个槽逐块**与**本次运行开始时**的快照比对（语义是"本次运行期间不得漂移"，
    不是"设备必须等于我的备份"）。
20. **新增 URI 路由必须核对"注册条数 vs `max_uri_handlers`"**。
    httpd 超限时是**静默注册失败**：后注册的通配静态路由 `/*` 不生效 →
    **所有静态页面 404 "Nothing matches the given URI"**，而 API 全部正常
    （极易误判成 SPIFFS 坏了。真机事故：加 3 条 `/api/adjust` 后 31+2=33 > 32 顶破）。
    现已有 `kMaxUriHandlers` + **编译期 static_assert**（再加路由会直接编译不过），
    启动日志也会打 `路由注册完成：N/M 条`，不完整时 `ESP_LOGE` 明确提示。
    → 加路由后若页面 404，**先看启动日志这两行**，别急着怀疑 SPIFFS。
21. **新增测试文件后必须 `idf.py -B build reconfigure`**。
    `test_apps/main/CMakeLists.txt` 用 `file(GLOB ...)`，CMake 只在 configure 时求值 →
    只 `build` 不会收集新测试文件（真机事故：加了 7 个 PageAdjust 单测后仍显示 240 PASSED，
    与改造前一样）。**测试数与预期不符时必须停下核对**，不能把"数字没变"当通过。

22. **会改变"可达性"的验证必须走串口兜底通道，且先准备好救援手段**。
    断开 WiFi / 改静态 IP 这类操作会让设备**离开你能访问它的那条链路**
    （真机实测：`POST /api/wifi/disconnect` 后主机立刻不可达）。
    因此：① 先实现并验证救援命令（`wifi on`、`httptest [METHOD] <path>` 走本机 AP 192.168.4.1）；
    ② 风险操作只在串口做；③ HTTP 侧只测**校验负例**与**可逆项**；
    ④ 每次验证完必须核对设备已复原（热点名/IP 模式/连接状态/已保存网络/模板/偏移）。
23. **`esp_netif_ip_info_t` 是 lwIP 内部字节序**：必须 `IP4_ADDR(&info.ip, o1,o2,o3,o4)`。
    把 `ParseIpv4` 解析出的"大端数值"直接赋给 `.addr` 会**字节反序**
    （真机事故：静态 IP 显示成 252.0.168.192）。同类"看起来对、实际反序"的坑，
    靠**真机读回显示值**才能发现 —— 单测覆盖不到（纯函数是对的，错在赋值方式）。

24. **多字段可选参数的 handler 不能用"前一步的 ok"做后一步的门禁**。
    真机事故：`PUT /api/wifi/ap` 支持 `ap_ssid` 与 `ap_pass` 二选一，写成
    `if (has_ssid) ok = SetApSsid(...); if (ok && has_pass) ok = SetApPassword(...);` →
    **只传密码时 `ok` 保持初值 false，密码那步被跳过并整体返回 400**。
    正确写法：`if (has_ssid && !A(...)) return; if (has_pass && !B(...)) return; ok = true;`
    —— `ok` 的语义是"**所有被请求的操作都成功**"，不是"前面成功了"。


25. **异步事件处理器不得抢走同步路径的驱动资源**。
    真机事故：为"异步扫描"新增的 `WIFI_EVENT_SCAN_DONE` 处理器**无条件**调用
    `esp_wifi_scan_get_ap_records()`；而**同步** `Scan()`（`esp_wifi_scan_start(blocking=true)`）
    也会收到同一事件 → 事件任务先把结果取走，同步路径只剩 0 个网络
    （串口 `wifi scan` 从此"永远扫描不到任何 AP"，看起来像硬件坏了）。
    修法：加"这次是不是**我自己**发起的异步操作"标志（`s_scan_async_pending`），
    只有自己发起时才在事件里消费结果；别人的事件只复位状态。
    → 通用原则：**共享的驱动资源（扫描结果、DMA 缓冲、事件队列）在事件处理器里必须先判断
    "这事件是不是我触发的"再消费。**

26. **测试期望值必须先用"真实轨迹"确定，不能凭直觉写**。
    真机事故（2026-10-11，按钮去抖）：我先凭直觉写了两条断言 —— "220 ms 那次算限流拒绝"、
    "999 ms 被拒后 1000 ms 必接受" —— 连续两次跑挂。把纯逻辑模块单独用 g++ 在主机上跑出
    **真实轨迹**才看清：去抖是"等安静"语义（220 ms 距上次事件仅 20 ms 仍属抖动）；
    被限流拒绝的那次**也是事件**、会顺延去抖窗口（1000 ms 距它 1 ms = 同一次抖动的尾巴）。
    → 对"时序/边界"类逻辑：**先把序列在主机上跑出来，再照实测值写断言**；
    断言失败时先怀疑自己的期望，再怀疑实现。
    另注：本项目 `EXPECT_EQ(a,b)` 内部按 `(b,a)` 调 Unity，报错 "Expected X Was Y" 里的 X 是**实际值**。


---

## 五、验证纪律（**做完必须跑，通过了才能说"完成"**）

| 层级 | 命令 | 通过标准 |
|---|---|---|
| 编译 | `bash scripts/build.sh s3` | `Project build complete` |
| 片上单测 | `cd test_apps && idf.py -B build -p $PORT flash` + 读串口 | **全 PASS / 0 FAIL**，且**单次启动无重启** |
| HTTP 验收 | `python3 scripts/api_verify.py <设备IP>` | 全通过（含负例：坏 JSON→400、路径穿越→302）；**默认不出纸** |
| 前端冒烟 | `python3 scripts/web_smoke.py <设备IP>` | 6 页面无 JS 异常 + 画布交互全通过 |
| 真机打印 | 串口 `psample` | `整帧 N 字节（logo 已包含）` + `打印完成 (ok=1 fail=0)` |
| 纸面 | **人眼确认** | 版式/字迹/二维码正确（自动化验证不到的只有这一项） |

> **默认不跑会真出纸的用例**（用户要求：除非明确要求，否则排除实际打印）：
> `api_verify.py` 里的 `POST /api/printer/test`（真出纸）需显式 `--with-print`，
> 抽奖全链路 `POST /api/draw`（会转页 + 出票）需显式 `--draw`；默认只做**只读**的
> `GET /api/status` 打印机状态检查。脚本结束会打印「本次跳过」清单，避免"以为验了其实没验"。
> 新增任何会物理出纸/出票的用例，一律走"默认跳过 + 参数显式开启"，不要直接塞进默认路径。

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
preceipt ptime prizes gbtest
wifi [scan|ascan|set <ssid> <pass>|off|disc|on|ap <名>|appass <密码>|ip ...|dhcp]
btbtn        # 抽奖按钮自检（触发电平/去抖/限流/当前电平/统计）
rstbtn       # 恢复按钮自检（长按阈值/当前电平/幽灵长按拒绝次数/可信性要求）
httptest [GET|POST|PUT|DELETE] <path>   # 板内自测；断开后唯一通道
```

---

## 七、已知边界与待办

- 本机不支持页面模式 → "文字压图"用 canvas 位图合成实现（慢、帧大是固有代价）
- 边距/密度调整：改 `tools/gen_border.py` 参数重画；行位置在画布上拖
- 纸尽探测使每张票 +约 0.4s（可接受；如需更快可只在连续多张后抽检）
- `pdump` 只留尾部 1024B —— 需要看帧头大图时应借助"整帧字节数"日志或另加分段抓取
