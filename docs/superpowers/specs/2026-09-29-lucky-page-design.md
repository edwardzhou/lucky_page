# lucky-page 设计文档 —— 单机抽奖翻页机

> 状态：**设计已按两轮 Momus 评审修订（含 44 条空壳测试重写），待第 3 轮定向复审**
> 评审记录：`docs/superpowers/reviews/2026-09-29-review.md`（两轮评审共：方案 5 BLOCKER + 9 MAJOR、计划 8 BLOCKER + 16 MAJOR，**均已修复**）
> 经用户确认：**暂不编写实现代码**；按 `page-player/AGENTS.md` 规则 4 完成评审后再进入实现
> 用户已确认：S3（**N16R8**，实物照片 `docs/ESP32S3-PIN.png`）/ 打印机参考 `tft_displaye` / 奖品等级池 / 默认 30 页可配 / 奖池分级可配（数量+概率）/ 小票文案可编辑 / logo 可上传 /
> **奖池默认值可用**（一等 1、二等 5、三等 30、谢谢参与不限量；权重 1:5:20:74）/ **小票文案默认可用** / **不需要出卡机联动**
> 日期：2026-09-29
> 基线项目：`page-player`（ESP-IDF 5.5.x，Apache/MIT）
> 打印机参考：`tft_displaye/components/dw_printer`（东为热敏打印机 TTL 驱动）

---

## 一、项目名称

**lucky-page**（中文名：**幸运翻页机**）

命名理由：

| 维度 | 说明 |
|---|---|
| 与基线一致 | `page-player` → `lucky-page`，同后缀、同语义域（page） |
| 技术本质 | 用**翻页机的物理翻页**做抽奖（luck），不是屏幕滚动 |
| 检索友好 | `lucky-page` 在 Espressif / GitHub 生态无重名（已查工作区与仓库） |
| 目录/组件名 | 全部 `lucky-page`、任务名 `lucky_draw`、TAG `LuckyDraw` |
| 备选 | `slot-pager`（强调水果机手感）、`fortune-pager`（偏营销）、`page-lottery`（最直白） |

**正式采用 `lucky-page`。** 若你更偏好"水果机"语感，改 `slot-pager` 只需全局改名，不影响任何设计决策。

---

## 二、背景与目标

### 2.1 背景

`page-player` 是通用多机翻页控制固件，支持 1~300+ 台翻页机、巡页、定时、计数器、DMX/ArtNet 输入、Web 配置等。它已经具备本需求所需的**全部底层能力**：

| 已有能力 | 源码位置 | 本需求用途 |
|---|---|---|
| 单机定页翻页 | `PagingController::GotoPage(page, cell_id, replace)` | 落到中奖页 |
| 动作管线 | `MotionEngine::Reschedule/AddActions`、`ActionSequence`、`ActionGroup`、`ActionDelay`、`ActionPaging` | 构造"水果机"减速序列 |
| 按钮 | `boards/common/button.h`（iot_button）、`BUTTON_MODULE_GPIO` | 抽奖触发 |
| 页码→行程映射 | `PageMapping`、`PageAdjust` | 每页对应的 DMX 行程 |
| 配置持久化 | `Settings` + `NvsSaver` | 奖池、参数 |
| Web/REST | `web_server.cc`（约 124 KB）+ SPIFFS 前端 | 奖池配置、手动触发 |
| 管线空回调 | `MotionEngine::SetScheduleDoneCallback` → `PagingController::OnScheduleDone` | 判断"翻页已停稳" |

### 2.2 目标

做一个**只控制一台翻页设备**的独立抽奖机：

1. **按下按钮触发抽奖**（单台翻页机）。
2. **水果机随机翻页效果**：先快速随机跳页 → 逐渐减速 → 微弹跳 → 停在最终中奖页。
3. **结果通过 TTL 热敏打印机打印**（中奖小票）。
4. 输出**项目名称**（本文档第一节）。

### 2.3 非目标（明确不做）

- ❌ 多机/矩阵翻页（`cell_count` 恒为 1）
- ❌ 巡页、定时翻页、计数器、时钟模式、DMX 录制回放、ArtNet 输入
- ❌ 语音交互、音频播放、蓝牙、MCP、License
- ❌ 出卡机联动（`req0033a` 提到的弹卡机构，属于后续需求）
- ❌ OTA / 远程升级门户（保留最小 OTA 例外见 §10.4）

> **裁剪原则**：不复制 `page-player` 的冗余模块，只搬运本需求闭环所需的最小集合。搬运时**保留接口签名**，以便后续反向同步修复。

---

## 三、架构总览

### 3.1 分层（沿用 Clean Architecture 依赖规则）

```
┌──────────────────────────────────────────────────────────────┐
│ 表现层    Display(可选) / WebServer(HTML+JSON) / SerialCmd    │
├──────────────────────────────────────────────────────────────┤
│ 应用层    Application(单例, Schedule 调度中心)                │
│           LotteryController(抽奖状态机)                       │
├──────────────────────────────────────────────────────────────┤
│ 领域层    PrizePool(奖池+加权抽取)                            │
│           SlotMachineEffect(减速序列生成)                     │
│           MotionEngine / Pipeline / Action* (搬运)            │
├──────────────────────────────────────────────────────────────┤
│ 服务层    LotteryController      │ PrinterService(排队+任务)  │
├──────────────────────────────────────────────────────────────┤
│ 驱动层    Dmx512Service(UART1)   │ PrinterUart(UART2)        │
│           GbkEncoder(纯逻辑)     │ QrCode(纯逻辑, ESC/POS)   │
├──────────────────────────────────────────────────────────────┤
│ 硬件层    Board(S3) → GPIO / UART / NVS / SNTP                │
└──────────────────────────────────────────────────────────────┘
```

依赖方向单向向内：`Application` 只依赖抽象（`MotionEngine&`、`PrinterService*`），不直接碰 UART/GPIO。

### 3.2 目录结构

```
lucky-page/
├── CMakeLists.txt                    # project(lucky-page VERSION 0.1.0)
├── sdkconfig.defaults                # target: esp32s3
├── partitions/
│   └── 16m.csv                       # 实物为 16 MB Flash，与 page-player 16m 一致
├── scripts/
│   ├── build.sh                      # 从 page-player/scripts 搬运（s3 only）
│   └── gen_gbk_table.py              # 生成 main/printer/gbk_table.cc（构建期前一次性）
├── components/
│   └── dw_printer/                   # 从 tft_displaye 搬运，见 §9
│       ├── include/raw_printer.h
│       ├── raw_printer.c
│       ├── include/raw_printer_qr.h  # 新增：GS ( k 二维码
│       ├── raw_printer_qr.c          # 新增
│       └── lucky_printer_uart.c      # 由 dw_printer_uart.c 裁剪而来
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── main.cc
│   ├── application.h/.cc             # 裁剪版
│   ├── settings.h/.cc                # 裁剪版 + 奖池 JSON
│   ├── nvs_saver.h/.cc               # ⚠️不可原样搬运：DoSave() 是硬编码字段表，必须扩展（D22/§7.2）
│   ├── lottery/
│   │   ├── prize.h                   # PrizeTier 结构 + 校验常量
│   │   ├── prize_pool.h/.cc          # 分级奖池 + 加权抽取 + 库存扣减（纯逻辑, 可单测）
│   │   ├── slot_machine_effect.h/.cc # 减速/弹跳序列生成（纯逻辑, 可单测）
│   │   ├── lottery_controller.h/.cc  # 抽奖 + 开机归零（合并单一状态机，§5.4/5.5）
│   │   └── draw_record.h/.cc         # 票号 + 当日计数（可单测）
│   ├── motion/                       # 从 page-player 搬运最小集合
│   │   ├── action.h, action_paging.*, action_delay.*,
│   │   ├── action_sequence.*, action_group.*, action_repeat.*
│   │   ├── cell.h, pipeline.*, motion_engine.*
│   │   └── paging_effects.*          # 仅保留单机所需（可裁剪）
│   ├── dmx512/
│   │   └── dmx512_service.h/.cc      # 裁剪为 1 路 UART 纯输出
│   ├── printer/
│   │   ├── gbk_encoder.h/.cc         # UTF-8 → GBK
│   │   ├── gbk_table.cc              # 生成物（78,044 B，见 §6.2）
│   │   ├── receipt_layout.h/.cc      # 小票模板渲染 + 显示宽度排版（纯逻辑, 可单测）
│   │   ├── printer_logo.h/.cc        # LGO1 头部校验 + GS v 0 分包（纯逻辑, 可单测）
│   │   ├── logo_store.h/.cc          # /spiffs/logo.bin 读写（临时文件 + rename）
│   │   ├── printer_service.h/.cc     # 队列 + 打印任务
│   │   └── printer_uart.h/.cc        # UART2 bring-up
│   ├── boards/
│   │   ├── board.h/.cc
│   │   ├── wifi_board.h/.cc
│   │   ├── common/button.h/.cc
│   │   └── esp32s3_lucky/config.h    # 新板级配置
│   ├── web/
│   │   ├── web_server.h/.cc          # 裁剪 + 新增端点（§8.1）
│   │   └── assets/spiffs_contents/
│   │       ├── index.html            # 抽奖控制 + 状态（含归零倒计时）
│   │       ├── prizes.html           # 分级奖池 + 概率 + 库存
│   │       ├── receipt.html          # 文案编辑 + 实时预览
│   │       ├── logo.html             # Logo 上传 + 二值化预览
│   │       └── calibrate.html        # 30 页行程标定（逐页点动）
│   └── wifi/                         # 从 page-player 搬运（STA + captive portal）
├── test_apps/
│   ├── main/
│   │   ├── test_prize_pool.cc
│   │   ├── test_slot_machine_effect.cc
│   │   ├── test_gbk_encoder.cc
│   │   ├── test_receipt_layout.cc
│   │   ├── test_printer_logo.cc
│   │   ├── test_qr_code.cc
│   │   ├── test_gbk_encoder.cc
│   │   ├── test_draw_record.cc
│   │   ├── test_lottery_controller.cc
│   │   └── test_draw_record.cc
│   └── run_tests.py                  # 搬运
├── docs/superpowers/{specs,plans}/   # 本目录
└── reqs/
    └── req0001_lucky-page.md
```

---

## 四、硬件设计

### 4.1 目标芯片

**ESP32-S3**（沿用 `page-player/main/boards/esp32s3_devkit` 已验证配置）。

理由：只控 1 台翻页机 → 1 路 DMX 足够；无语音/矩阵需求；成本最低。

### 4.2 引脚分配（已按实物照片 `docs/ESP32S3-PIN.png` 核对）

实际板子：**Goouuu Tech ESP32-S3-N16R8**（16 MB Flash + 8 MB **Octal** PSRAM，双 USB-C：一个接板载 USB-UART 桥，一个为原生 USB）。

| 功能 | 外设 | 引脚 | 来源 |
|---|---|---|---|
| DMX512 输出 TX | UART1 | `GPIO17` | 沿用 page-player S3 |
| DMX512 输出 RX（不用） | UART1 | `GPIO18` | 沿用（保留） |
| TTL485 DE/RE 方向 | GPIO | `GPIO4` | 沿用 page-player S3 |
| **TTL 打印机 TX** | **UART2** | **`GPIO47`** | **推荐（见下方论证）** |
| **TTL 打印机 RX** | **UART2** | **`GPIO48`** | **推荐**（可选，v1 不读状态） |
| 抽奖按钮 | GPIO | `GPIO8` | 沿用 `BUTTON_MODULE_GPIO` |
| 红外接收（保留） | RMT | `GPIO3` | 沿用 |
| SD 卡（logo/奖池导入，可选） | SPI2 | `10/11/9/12` | 沿用 |

**❌ 明确不用 `GPIO43` / `GPIO44`（推翻我上一轮的推荐）**

实物照片上，右侧排针把这两个脚**直接丝印为 `TX0` / `RX0`**——它们就是 UART0 的下载/日志脚，并且**接到了板载 USB-UART 桥上**（双 USB-C 中那个靠下/靠内的口）。若把打印机接上去：

- 桥芯片会在空闲时**驱动**这两条线 → 与打印机 TX 抢总线；
- 每次 `idf.py flash/monitor`（走 UART0 那个 USB 口）都会把**打印数据当成串口命令**发进去；
- 反过来打印机的数据也会灌进 UART0 → 干扰固件下载。

我上一轮的依据是 `page-player/docs/esp32s3-pinout.md`，它把 43/44 标成了"未使用"——**那份文档与实际板子不符**（它描述的可能是另一块没有板载 UART 桥的 S3 板）。**以实物照片为准。**

**✅ 为什么是 `GPIO47`(TX) / `GPIO48`(RX)**

把实物照片上**两侧排针的完整引出清单**与 `page-player` 已占用脚交叉核对后：

| 侧 | 引出脚（照片丝印） |
|---|---|
| 左 | `3V3 EN G4 G5 G6 G7 G15 G16 G17 G18 G8 G19 G20 G3 G46 G9 G10 G11 G12 3V3` |
| 右 | `GND G1 G2 TX0 RX0 G42 G41 G40 G39 G38 G37 G36 G35 G0 G45 G10 G9 G21 G14 G13 5VIN` |

- **已被 `page-player` 占用**：`4`(DMX DE)、`7/15/16`(I2S SPK)、`3`(IR)、`8`(按键模块)、`9/10/11/12`(SD SPI)、`17/18`(DMX UART1)、`21/40/41/42/45/47`—注意 `47` 在 page-player 是 **LCD MOSI**！
- **必须避开**：Strapping 脚 `0`、`3`、`45`、`46`；**系统脚** `19`/`20`（**USB D−/D+，不是 strapping**）、`43/44`(TX0/RX0)、`EN`。
- **`13`/`14`**：`14` 在 page-player 引脚表里标了 **Strapping**；`13` 与 `14` 相邻，且这两脚在 S3 上兼有 **JTAG**（MTCK/MTMS）复用，接打印机 RX 会被外部驱动 → 有启动/调试风险，避开。

→ 扣除后**真正干净、非 strapping、已引出排针**的只剩 **`47` 和 `48`**，正好是一对，直接给 UART2。

> ⚠️ **`GPIO47` 在 `page-player` 里是 LCD MOSI**——但 **`lucky-page` 不驱动 LCD**（无显示需求），所以该脚在本项目是空闲的。**这一点必须在 `main/boards/esp32s3_lucky/config.h` 里显式注释说明**，避免后人从 page-player 抄配置时踩到这个差异。
>
> ⚠️ `GPIO48` 在本板上还兼有 **RGB LED / 板载 LED**（照片里那颗红色 LED 与右下角的 RGB 元件区域）。若发现该脚被板载 LED 电路拉住导致打印机通信异常，**RX 可以先不接**（v1 只发不收，本来就不需要 RX）。**TX 用 `GPIO47` 是安全的**。

> 💡 **只需接 TX 一根线**（v1 不读打印机状态）。因此**即使 RX 有争议，方案也不受影响**。

引脚已做成 **NVS 可配**（`printer_tx_pin` / `printer_rx_pin`，设计 §7.1），现场不对可改配置，**不用重新烧录**。

### 4.2b 30 页分辨率问题（可行性，必须正面解决）

**问题**：`PageMapping` 默认公式 `i * 255 / total_pages`（`page-player/main/page_mapping.cc:67`）把 0~255 的 DMX 行程**均匀**分给 N 页：

| N | 相邻页步进 | 占比 |
|---|---|---|
| 10 | 25 | 9.8% |
| **30** | **8** | **3.1%** |

30 页时相邻页只差 **8 个 DMX 单位（3.1% 行程）**。翻页机是**机械**机构，其重复精度、回差、DMX 量化误差、执行器死区**只要超过 3% 就会跨页落错**——**这是分辨率决定的，逐页标定（`PageAdjust`）也救不回来**，因为标定只能平移、不能拉开间距。

**设计对策（三项，缺一不可）**：

1. **补"行程跨度"配置**（新增 Settings）：
   - `stroke_min_dmx`（默认 0）、`stroke_max_dmx`（默认 255）
   - 映射生成改为在 `[stroke_min, stroke_max]` 内均匀分布：`value_i = stroke_min + i*(stroke_max-stroke_min)/(N-1)`
   - 这样 30 页可以**铺满执行器全程**，把可用间距从 8 提升到 `(max-min)/29`。若执行器全程只有 255 个 DMX 单位且机械精度 3%，则**物理上就装不下 30 页**——必须由现场用下面的判据确认。
2. **必须走显式 `SetMapping()`，否则行程跨度会被静默忽略（本设计的自我纠正）**：

   `PageMapping::GetMapping()` 是**惰性加载**的——`loaded_` 为 false 时调 `LoadMapping()`；
   **而 `LoadMapping()` 在文件不存在时会隐式调用 `SetDefaultMapping(total_pages)`**，
   那正是写死 `i*255/N` 的均匀公式（`page-player/main/page_mapping.cc:61-73,85-87`）。

   → 若只加 `stroke_min/max` 配置而不改这条路径，**开机时映射仍会被均匀公式填满**，
   `stroke_min/max` **完全不起作用**，而且恰好能通过"互异 + 间隔 ≥ 8"的断言（因为它就是按 8 造的），
   **形成一个查不出来的静默失效**。

   **因此 `lucky-page` 采取以下之一，且必须在代码里写死注释说明**：
   - **推荐**：在 `kBoot` 里**先**按 `stroke_min/max` 算出 N 个值，调 `PageMapping::SetMapping(values, names)`
     显式落盘（同时 `PageAdjust::Resize(1, N)`），**再**做断言与归零。这样 `GetMapping()` 首次调用时
     `loaded_` 已为 true，**不会**触发隐式默认。
   - 或：**打包出厂 `page_mapping.json`**（Task 2.1d Step 2），使 `LoadMapping()` 能读到文件、
     不进入默认分支。
   - ⚠️ **禁止**依赖"文件缺失 → 隐式默认"这条路径来获得映射。

3. **断言升级为「互异性 + 最小间隔」**：
   - 原断言只查 `min != max`（强度不足）；**改为排序后检查两两互异**，并额外检查 `相邻差 >= kMinPageGapDmx`。
   - `kMinPageGapDmx` 为**实测标定值**（默认取 8，即 255/30 的理论值；现场标定后调大）。不满足则拒绝进入可开奖状态。
4. **给出「30 页何时可用」的判据**（写进 README，施工必做）：
   > 逐页点动 30 次，记录机器实际停靠位置。**若出现任意两次落到同一格、或落页与目标不一致 → 该机台不支持 30 页**，必须减小 `total_pages`（建议 10~16）或更换行程更大的执行器。
   **这条必须在现场标定阶段确认，不能等到开奖时才发现。**

> **决策 D21**：`total_pages` 的**默认值仍是 30**（用户确认），但 30 页**必须通过现场标定判据**才允许投用；不通过就下调页数。软件侧通过"行程跨度 + 最小间隔断言"把问题**前移到开机**，而不是留到开奖时随机出错。

### 4.3 打印机接线（关键坑，来自 tft_displaye/PRINTER.md §5.2）

```
   ESP32-S3 (N16R8)                          东为 / MARSTECH 热敏打印机
 ┌─────────────┐                            ┌─────────────────────────┐
 │ G9  TX ────┼────────────────────────────┤ RX 或 TX（见下方警告）  │
 │ G10  RX ────┼────────────────────────────┤ TX 或 RX（可选，v1 不接）│
 │ G11 CTS ───┼────────────────────────────┤ CTS/RTS（方向见下）      │
 │ GND ────────┼────────────────────────────┤ GND                     │
 └─────────────┘                            │ VCC ── 外部 5~12V 2~3A  │
                                            └─────────────────────────┘
     ↑ 排针丝印见 docs/ESP32S3-PIN.png 右列：… G45 G10 G9 G21 G14 G13 5VIN
```

- **必须共地**，否则乱码/无输出。
- **禁止用开发板 3V3 给打印头供电**：打印瞬间 2~3 A 会把 S3 拉复位。
- **现场首坑**：`tft_displaye` 实测的 MARSTECH MB583 **丝印方向与内部实际相反**，按丝印接完全无输出，**对调两根数据线后立刻出票**。若出现"有电、FEED 能走纸、一个字节都不打"，**第一个动作就是对调 RX/TX**。
- 串口参数：**115200 8N1，无流控**（`tft_displaye` 实测；若出乱码用运行时命令降到 9600 逐个试）。
- **G11 流控方向（2026-09-29 新增接线，用户提供）**：两个方向都叫 "CTS"，接反的表现是
  「一个字节都不打」。默认 `printer_flow_mode=0`（关闭），避免两个输出端同时驱动一根线；
  现场用串口命令试方向，免重烧：
  - `pflow rts` — ESP32 RTS 输出 → 打印机 CTS 输入（**打印方向，优先试**）
  - `pflow cts` — 打印机 RTS 输出 → ESP32 CTS 输入（仅状态回读有意义）
- **现场排障命令**（真机新增）：`ppins`（测电平判断是否真接上，悬空=没接/没上电）、
  `pprobe`（发 ESC/POS 状态查询回读，会识别悬空脚噪声，不误报在线）、`pswap`（TX/RX 对调）。

---

## 五、领域设计

### 5.1 Prize / PrizeTier（奖项与等级）

**需求**：总页数可配（**默认 30**）；奖池可配——**几级、每级的数量、每级的出现概率**。

"数量"和"概率"是**两个独立维度**，必须分开存：

- **概率**（`weight`）决定**抽中谁**；
- **数量**（`quantity`）决定**能发几次**，发完（`remaining == 0`）该级自动退出抽奖池。

> 若只按概率抽而不跟踪数量，商家配了"一等奖 1 个、概率 10%"，可能出现发出 5 个一等奖（奖品不够，现场事故）。**库存耗尽是必须建模的业务约束**（决策 D12）。

```cpp
// main/lottery/prize.h
struct PrizeTier {
    int      level;          // 等级序号 1..N（1 = 一等奖），仅用于展示与排序
    int      page;           // 该级对应翻页机页码，0-based，范围 [0, total_pages)
    uint32_t weight;         // 相对权重（出现概率），0 = 停用该级
    int      quantity;       // 总投放数量；-1 = 不限量
    int      remaining;      // 剩余数量（运行时递减，落 NVS）
    char     name[32];       // 等级名（UTF-8），如 "一等奖"
    char     note[48];       // 小票副标题（UTF-8），如 "奖品：扫地机器人"
};

static constexpr int kMaxTiers = 16;   // 最多 16 级
```

**约束（`PrizePool::Validate()` 强制校验，非法配置拒绝生效并返回中文错误原因）**：

1. 至少 1 级 `weight > 0` 且未被耗尽；
2. `0 <= page < total_pages`，且**每个 `page` 至多被一级占用**（否则"停在某页"含义不唯一）；
3. `name` 非空；`level` 在 `1..N` 内且不重复；
4. `quantity >= 1` 或 `quantity == -1`（不限量）；`0 <= remaining <= quantity`（`quantity == -1` 时 `remaining` 忽略）；
5. 级数 ≤ `kMaxTiers = 16`；
6. **概率的可解释性**：`weight` 是**相对权重**，不是绝对百分比。实际概率 = `weight_i / Σ(未耗尽级的 weight)`。

> **页码即结果（关键设计决策 D2）**：抽奖结果与"翻页机实际停在第几页"**必须一致**——顾客看着机器。因此先抽 `PrizeTier`，再驱动机器翻到该级的 `page`。**禁止**先随机翻页再读页码反查奖项。

### 5.2 加权抽取 + 库存（`PrizePool`）

```cpp
struct DrawResult {
    int      tier_index;      // 在 tiers 中的下标
    int      page;            // = tiers[tier_index].page
    int      level;
    uint32_t roll;            // 抽中的随机数（审计用）
    uint32_t total_weight;    // 本次参与抽取的权重合计
    bool     depleted_fallback;  // 是否因"全部耗尽"而回退到兜底级
};

/// 失败语义（`DrawResult` 是值类型、无错误通道，无法表达 kRefuse —— 必须用 Outcome 包裹）
enum class DrawStatus { kOk, kAllDepleted, kRefuse, kInvalidConfig };

struct DrawOutcome {
    DrawStatus  status = DrawStatus::kInvalidConfig;
    DrawResult  result{};        // status != kOk 时无效
    const char* error = nullptr; // 中文原因，供 Web 400 回传
};

/// 纯函数：不修改库存，只做抽取判定（便于单测）
DrawResult PrizePool::Decide(uint32_t random_value) const;

/// 非纯：Decide() + 扣减 remaining + 落 NVS。库存从 1 → 0 时记 ESP_LOGW。
DrawResult PrizePool::DrawAndConsume(uint32_t random_value);
```

**抽取算法**：

1. 构造**候选集** = 所有 `weight > 0 && (quantity == -1 || remaining > 0)` 的级。
2. 若候选集**非空**：前缀和 + `random_value % Σweight` 定位中选级。
3. 若候选集**为空**（奖品全发完）：按 `depletion_policy` 处理（**决策 D13**）：
   - `kFallbackToLose`（**默认**）：中「谢谢参与」级（必须存在一个 `page` 且 `quantity == -1` 的兜底级，否则 `Validate()` 直接拒绝配置）；
   - `kRefuse`：拒绝开奖，`/api/lottery/status` 报 `state:"out_of_prizes"`，**宁可不发也不发错的**。
4. 扣减：仅当 `quantity != -1` 时 `remaining--`，并 **`NvsSaver::SaveSync()`（同步写盘，不能用 `RequestSave()`）**——理由见 §7.2 / 决策 D22。

> **权重不是硬性百分比**：某级耗尽后其余级的实际概率会**自动上升**（分母变小）。这是期望行为，但**必须在 Web 界面显示"理论概率"列**（`weight_i / Σweight`，实时计算），让商家看见真实分布，而不是自己按百分比心算。

- **随机源**：`random_value` 由调用方注入，生产环境用 `esp_random()`（硬件 TRNG），测试注入固定序列。
- 数学正确性验证：用 1,000,000 次注入均匀分布 `0..Σweight-1`，统计各级频次，偏差 < 0.5%（单测 `test_prize_pool.cc`）。
- 库存验证：`quantity = 3` 的级连续抽 10 次，**恰好出现 3 次**，且第 4 次起不再出现（单测）。

### 5.3 水果机翻页效果（`SlotMachineEffect`）

**手感模型（关键设计决策 D3）**：

```
t=0        加速带        匀速"高速"带              减速带                 弹跳        停稳
 |----------|--------------------------------|--------------------|-----------|--------|
 0        ~2 hops        ~6 hops                ~10 hops            2~3 hops
 80ms/hop  80ms/hop      80→200ms/hop           200→400ms/hop       400ms      —
```

> 表中"每跳耗时"已是 §"时序模型"修正后的**真实耗时**（含 `ActionDelay` 的 1 tick 额外开销），不是名义 `delays` 值。
> 起始 `80 ms/跳` 已经足够快（12.5 跳/秒）——翻页机的机械响应本身远慢于此，继续降到 40 ms **不会有视觉收益**，只会让 `delays` 与真实耗时脱节。因此 `min_hop_ms` 保留 40（名义值），真实值 80。

实现：生成一条 **`ActionSequence`**，每步 = `ActionGroup{ ActionPaging(1, hop_page, adj), ActionDelay(d_i) }`。

```cpp
// main/lottery/slot_machine_effect.h
struct SlotMachineParams {
    int  total_pages      = 30;    // 默认 30 页，可配（需求确认）
    int  min_hop_ms       = 40;    // 名义起始步进间隔（真实耗时 80 ms，见"时序模型"）
    int  max_hop_ms       = 400;   // 名义末段步进间隔（真实耗时 400 ms）
    int  min_hops         = 18;    // 最少跳页次数
    int  max_hops         = 20;    // 最多跳页次数（默认收紧到 20，使时长区间集中；见下方时序表）
    int  settle_bounces   = 2;     // 停稳前"过冲再回弹"次数
};

// 纯逻辑：给定随机源，产出可重放的跳页计划（可单测）
struct HopPlan {
    std::vector<int> pages;                 // 每次跳到的页
    std::vector<int> delays;                // 对应的名义等待毫秒（均为 40 的倍数）
    int              final_page = 0;
    /// 预期真实耗时（ms）= Σ 40*ceil(delays[i]/40)。
    /// 由 Plan() 一并算出，供日志打印与单测断言（见"时序模型"）。
    int              expected_duration_ms = 0;
};
HopPlan SlotMachineEffect::Plan(int final_page, const SlotMachineParams& p,
                                const std::vector<uint32_t>& randoms);

// 转为动作序列（依赖 MotionEngine 的 Action 体系）
std::vector<std::unique_ptr<Action>>
SlotMachineEffect::Build(const HopPlan& plan, uint8_t speed, int paging_channel_count);
```

**跳页规则**：

1. `pages[i]` 从 `[0, total_pages)` 均匀随机，但 **剔除 `final_page`**（除了最后一跳）——保证"最后一下才落定"。
2. **禁止与上一跳相同**（否则视觉上"卡住不动"）。
3. 末段 3 跳强制进入 `final_page` 的邻域 `{(final_page-1+N)%N, (final_page+1)%N, final_page}`，产生"擦边"紧张感。
4. `delays[i]` 用**缓入缓出**（ease-out）曲线映射到 `[min_hop_ms, max_hop_ms]`，并对齐到 40 ms 的整数倍（`MotionEngine::kTickIntervalMs`），避免抖动：

```
delays[i] = ceil_to_40ms( min_hop_ms + (max_hop_ms - min_hop_ms) * (i/(n-1))^2 )
```

5. 弹跳：最后追加 `settle_bounces` 次 `final_page → 邻页 → final_page`，delay 固定 400 ms，制造"停一下又动一下"的机械感。
6. 每次 `ActionPaging` 的 `page_value` 走 `PageAdjust::GetInstance().GetAdjustedValue(0, hop_page)`（cell 恒为 0），与 `page-player` 的微调语义一致。

**时序模型（重要，按源码实测语义）**

`ActionDelay::Update()` 的判定是 `elapsed += tick; if (elapsed >= millis) done;`（`action_delay.cc:14-25`），因此**一个 `millis=40` 的延时实际花费 2 个 tick = 80 ms**。每跳的真实耗时是：

```
real_hop_ms(m) = (floor(m / 40) + 1) * 40 = 40 * ceil(m / 40)   // m 为 40 的倍数时
```

这不是缺陷（`page-player` 的既有语义如此），但**必须计入总时长**，否则现场核对"预期时长"永远对不上。设计取 `m = 40 → 80 ms/跳`。

**总时长是一个区间，不是单值（M1 修正）**：

跳数 `n` 在 `[min_hops, max_hops] = [18, 26]` 之间随机，因此总时长**随 n 变化**。按本文档自己的延迟公式实测：

**默认参数下（`min_hops = 18`、`max_hops = 20`）**：

| n | 减速段 `Σ40*ceil(m_i/40)` | 弹跳段（2 次 × **2 跳** × 400 ms = 1,600 ms） | 合计 |
|---|---|---|---|
| 18 | **3,320 ms** | 1,600 ms | **≈ 4.9 s** |
| 19 | **3,520 ms** | 1,600 ms | **≈ 5.1 s** |
| 20 | **3,720 ms** | 1,600 ms | **≈ 5.3 s** |

> **默认区间 ≈ 4.9 ~ 5.3 s。** 这是**逐跳复算**的准确值。
> 修订史（记录在此以免再犯）：初稿写死"≈5.1 s"（按 n=18 单点）；第二稿把弹跳段误算成 `2 次 × 4 跳 = 3,200 ms`、减速段也偏高，得出错误的"5.0~7.2 s"；第三稿修正了弹跳口径但 `max_hops` 仍是 26（区间 4.9~6.4 s），与 DoD 不一致。**现在 `max_hops` 默认已收紧到 20，全文区间统一为 4.9~5.3 s。**
>
> 若把 `max_hops` 放回 26，区间变为 **≈ 4.9 ~ 6.4 s**（减速段 n=26 → 4,760 ms）。
>
> 上表的减速段可直接核对：n=18 的 18 个名义 `delays` 为
> `[40, 80, 80, 80, 80, 80, 120, 120, 120, 160, 200, 200, 240, 280, 320, 360, 360, 400]`，
> 每个再取 `40*ceil(m/40)` 后求和 = **3,320 ms**。
>
> **唯一权威口径是运行期算出的 `HopPlan::expected_duration_ms`**（= `Σ 40*ceil(delays[i]/40)`，含弹跳段），每次开奖 `ESP_LOGI` 打印，现场以此核对。**文档给出的秒数仅供预估，不作为验收依据。**
>
> 若现场觉得偏慢，调参即可（无需改代码）：
> - `max_hops` 20→14：区间约 **≈ 4.4 ~ 4.9 s**
> - 再配合 `max_hop_ms` 400→240、`settle_bounces` 2→1：可压到 **≈ 3.1 ~ 3.4 s**
>
> ⚠️ **`settle_bounces` 的口径**：一次"弹跳" = `final → 邻页 → final`，共 **2 跳**。因此 `settle_bounces = 2` 是 **4 跳 / 1,600 ms**，不是 8 跳。实现时以"每跳 400 ms"为单位累加，别把次数当跳数重复乘（本表此前就是在这里算错的）。

> 效果**不阻塞**：整条序列一次性 `Reschedule` 给 `MotionEngine`，由引擎的 FreeRTOS 任务按 40 ms tick 驱动。`LotteryController` 不 sleep、不占用主循环。

### 5.4 抽奖状态机（`LotteryController`）

**启动阶段（§5.5，决策 D11）**

```
 kBoot ──► kHoming ──► kHomed ──► kIdle
 (上电)   (归零 ~11s)  (落 NVS)   (可开奖)
             │
             │ 超时重试 3 次仍失败
             ▼
       kHomingFailed ──► 终态：拒绝开奖，需人工介入后重启
```

**抽奖循环（仅在 `kIdle` 可进入）**

```
                   按钮 / POST /api/draw
        ┌──────────────────────────────────────────────┐
        │                                              ▼
     ┌──┴───┐                                   ┌───────────┐
     │ kIdle│                                   │ kSpinning │ 忽略后续触发（D4）
     └──▲───┘                                   └─────┬─────┘
        │                                             │ MotionEngine 管线空
        │ auto_return_ms (默认 1.5s)                  ▼
     ┌──┴───┐                                   ┌───────────┐
     │ kDone│ ◄────────── 打印任务完成 ───────── │ kPrinting │
     └──────┘                                   └───────────┘
```

> `kBoot / kHoming / kHomed / kHomingFailed / kOutOfPrizes` 见 §5.5。**归零完成前不接受抽奖触发**（决策 D11）。
> **`kHomingFailed`** 是**终态**（API 报 `homing_failed`）：宁可不开奖，不可开错奖；需人工排查机械/接线后重启。
> **`kOutOfPrizes`**（`out_of_prizes`）：奖品发完且策略为 `kRefuse` 时的终态，等待商家补货或重置库存。

| 状态 | 进入动作 | 退出条件 |
|---|---|---|
| `kBoot` | 加载 `PageMapping`/`PageAdjust`/奖池，`Resize(1, total_pages)` | 加载完成 + `MotionEngine::Start()` 返回 → `kHoming` |
| `kHoming` | 下发归零序列（§5.5），拒绝所有触发。**500 ms 稳定期已包含在序列末尾**（`ActionDelay(500)`），故不在回调里再等 | `OnScheduleDone()`（守卫 `state_ == kHoming`）→ **直接** `kHomed` |
| `kHomed` | `current_page = 0` 落 NVS，日志"可以开奖" | 立即 → `kIdle` |
| `kIdle` | 允许触发 | 收到触发 → 抽奖 → `Reschedule(plan)` → `kSpinning` |
| `kSpinning` | 记录 `draw_id`、掷随机、生成 `HopPlan` | `OnScheduleDone()` 回调 → `kPrinting` |
| `kPrinting` | `PrinterService::Enqueue(receipt)`（非阻塞入队） | 打印任务回报成功/失败 → `kDone` |
| `kDone` | 广播结果（Web `/api/lottery/status`、日志、可选蜂鸣） | 短暂停留（默认 1.5 s）→ `kIdle` |

**单飞保证（关键设计决策 D4）**：`kSpinning/kPrinting` 期间**所有新触发一律丢弃并记日志**。理由：翻页机是机械机构，重入会导致 DMX 通道被两条序列争抢 → 停在错误页；且会打印重复票。

`kPrinting` 的完成判定：`PrinterService` 用一个深度 1 的 FreeRTOS Queue + 独立任务（优先级 5，栈 4 KB），任务内 `raw_printer_*` 阻塞发送，完成后通过 `Application::Schedule()` 回调主线程置状态。

> **为什么不复用 `PagingController`**：`PagingController` 的职责是"通用翻页/巡页/定时"，加入抽奖状态机会违反 SRP（`page-player/AGENTS.md` 规则 5.1）。`LotteryController` 只持有 `MotionEngine&` + `PrinterService&` + `PrizePool&`，调 `MotionEngine::Reschedule` 完成一次抽奖。`lucky-page` 中**不搬运** `PagingController`。

### 5.5 开机归零（Boot Homing）—— 关键，不可省

**问题**：翻页机是**开环**机械机构——固件只知道"我发了多少行程值"，**无法读回机器当前停在第几页**（`ActionPaging` 是纯 DMX 写，`MotionEngine` 无回读通道）。`page-player` 正是如此：`ResetPage()` 只发动作，不验证结果；而 `Application` **在开机时并不调用 `ResetPage()`**，只从 NVS 恢复 `current_page`（`application.cc:125` 的 `ctx.current_page = settings.current_page()`）。

**后果**：重启后固件的 `current_page` 是**记忆值**，而机器的物理位置可能是**断电时被手动拨动的任意页**。此时抽奖的"先抽奖 → 翻到该页"会**停在错误的页**——票面写一等奖，机器停在三等奖。这是**业务事故**（R4/R5 直接失效）。

**设计：开机归零状态机（在 `LotteryController` 之前完成，`kIdle` 之前）**

```
kBoot → kHoming（下发归零序列）→ kHomed（current_page = 0）→ kIdle（接受抽奖）
```

1. `kBoot`：等待 `PageMapping::LoadMapping()` + `PageAdjust::LoadAdjust()` + `Resize(1, total_pages)` 全部完成，且 `MotionEngine::Start()` 已返回。**顺序不可颠倒**——否则归零序列会用到未加载的行程值。

> **⚠️ 静默失效陷阱（必须在实现时防住）**：`PageAdjust::GetAdjustedValue(cell, page)` 在映射未加载或 `page` 越界时**不报错，静默返回 `0`**（`page_adjust.cc:139-146（关键行 141）`：`int base = (page >= 0 && page < (int)mapping.size()) ? mapping[page] : 0;`）。后果是**所有页都翻到同一个 DMX 值**——机器"翻页"了但每页都停在同一位置，**外观上像正常翻页，实际永远停在同一位**。这种故障在现场极难定位。
>
> 因此 `kBoot` **必须硬断言**以下两条，任一不满足就留在 `kBoot` 重试（打 `ESP_LOGE`），**绝不带病下发**：
>
> ```cpp
> // 断言 1：PageMapping 已加载且覆盖全部页
> ESP_ERROR_CHECK(mapping.size() >= (size_t)total_pages);   // 生产用显式判断+重试，不用 CHECK 复位
> // 断言 2：各页行程值不全相同（否则说明映射退化，翻页无意义）
> //   取 mapping[0..total_pages) 的 min/max，若 min == max 则判定退化
> ```
>
> 同时 `SlotMachineEffect::Build()` 与归零序列在取 `adj` 后应 `ESP_LOGD` 记录 `(page, adj)`，便于 `pdump`/日志核对"每页是否真的不同"。
2. `kHoming`：下发与 `page-player/ResetPage()` 等价的归零序列（`paging_controller.cc:112-153`）：

```
ActionSequence[
  ActionGroup{ 每台 ActionPaging(cell=1, page=0, adj, speed, ..., ch07=150), ActionDelay(10000) },
  ActionGroup{ 每台 ActionPaging(cell=1, page=0, adj, speed, ..., ch07=0) }
]
```

   > 第一段带 `ch07=150`（重置通道）并 `ActionDelay(10000)` 等 10 秒让机械完全释放；第二段收掉重置通道。**这是 `page-player` 已验证的机械时序，照抄不要改**。10 秒是硬编码的机械等待，不是可调参数。
3. 归零序列下发后，**必须等 `OnScheduleDone()` 再置 `current_page = 0`**——不能下发即认为到位。稳定期**只保留一个来源**（原稿在 §5.4 与 §5.5 各写一次、会叠加）：

   - **采用**：归零序列**末尾自带** `ActionDelay(500)`（属序列的一部分），`OnScheduleDone()` 触发即表明"序列 + 稳定期"都已结束，**直接置 `kHomed`**，**不再另起 `esp_timer`**。
   - ⚠️ **必须加状态守卫**：`MotionEngine::Reschedule()` 会把 `was_schedule_done_` 置回 `false`（`page-player/main/motion/motion_engine.cc:212`），因此一次归零可能触发**两次** `OnScheduleDone`。回调内**必须**先判 `if (state_ != kHoming) return;`，否则第二次触发会重复置位/误启动定时器。
4. `kHomed → kIdle`：`settings.set_current_page(0); settings.RequestSave();`，`ESP_LOGI("归零完成，可以开奖")`。
5. **归零期间拒绝所有抽奖触发**（按钮、`POST /api/draw`），返回 `409 {status:"homing"}`。Web 的 `/api/lottery/status` 返回 `state:"homing"`，前端显示"正在归零，请稍候"。

**开机耗时**：约 **10.5 s**（`ActionDelay(10000)` 真实恰好 10,000 ms，因为 10000 是 40 的整数倍；+ 归零行程 + 500 ms 稳定）。此前写"11~13 s"偏高。Web 端倒计时由后端返回 `homing_remaining_ms` 驱动，不写死前端常量。这是**必须付出的代价**，因为开环机构无法在更短时间内确认位置。若商家无法接受，唯一替代方案是**加装位置传感器/限位开关**（硬件改动，见 §11-R9），固件侧无解。

**断电中断的处理**：若在 `kHoming` 或 `kSpinning` 中途断电，重启后 `state` 恒为 `kBoot`（**不持久化中间状态**），必然重新走一遍归零——因此"断电时机器停在哪"不影响正确性。**禁止**把 `Spinning` 状态写入 NVS 用于"断点续传"，因为物理位置已不可知。

> **决策 D11：开机必须归零，归零完成前不得开奖。** 这是保证 D2（落页 == 票面）在**每次重启后**都成立的唯一手段。

---

## 六、打印设计

### 6.1 小票内容（58 mm 纸，32 字符/行；80 mm 纸 48 字符/行）

> 以下为**内置默认模板**的渲染效果；文案可在 Web 端编辑（§7.3），整段结构可配。

```
       [LOGO 位图]                     ← 可选，logo_enabled（§6.4）
        幸运翻页机                     ← {shop}
   ══════════════════════
         恭 喜 中 奖                   ← win_title / lose_title（2 倍字号居中）
   ══════════════════════
           一 等 奖                    ← {prize}（2 倍字号、加粗、居中）
        奖品：扫地机器人               ← {note}（常规字号居中）
 --------------------------------------
 票号：20260929-0007                   ← {ticket}
 时间：2026-09-29 14:32:07             ← {time}
 页码：P1                              ← {page}（1-based 展示）
 校验：3F2A                            ← {check}
 --------------------------------------
  请凭此票到服务台领奖                  ← receipt_footer
  有效期至：2026-10-06                  ← {validity}
 --------------------------------------
 [QR]  核销二维码（内容 = 票号+校验码）
                                       ← 走纸 4 行
```

- 全部中文走 **GBK（CP936）**，先发 `ESC t 0` 设定代码页，再发 GBK 字节。
- 票号 `TICKET = YYYYMMDD-NNNN`，`NNNN` 为当日流水（NVS 持久化，跨日归零）。
- 校验码：`draw_id` 的 CRC16-CCITT 十六进制 4 位，防手改。
- **未中奖**（`谢谢参与` 级）：标题用 `lose_title`（默认 `感 谢 参 与`），**不打二维码**，减少纸张与顾客落差。

### 6.2 UTF-8 → GBK 编码器（`GbkEncoder`）

按 `tft_displaye` 的实测结论：`raw_printer_text_utf8()` **原样透传字节**，且厂商核心的文本 API 只保证 ASCII。中文必须由**应用侧**转码。

**方案：build-time 生成完整 GBK 双字节映射表。**

```
索引公式： index = (gbk_hi - 0x81) * 191 + (gbk_lo - 0x40)
          // 0x40..0xFE 含端点共 191 列（0x7F 保留为空槽，不跳过、不压缩，以保证 stride 自洽）
          // hi: 0x81..0xFE 共 126 行
条目：     uint16_t unicode_cp                            // 0 = 该 GBK 位置无字符
表尺寸：   126 * 191 * 2 B = 48,132 B ≈ 47.0 KB (flash, const)
最大索引： (0xFE-0x81)*191 + (0xFE-0x40) = 24065 < 24066 ✅ 不越界
```

- 生成脚本 `scripts/gen_gbk_table.py`：用 Python `codecs` 遍历 `0x81..0xFE` × `0x40..0xFE`（跳过 `0x7F`），`bytes([hi,lo]).decode('gbk')` 成功则写入码点；输出 `main/printer/gbk_table.cc`（ASCII 数组字面量，**生成物入库**，构建不依赖 Python）。
- **反向查找**（GBK 字节 → 查表时**不需要**；只有"码点 → GBK"方向需要）。若用完整反查索引，21,791 项 × 4 B ≈ 85 KB，过大。

> **尺寸复核（实测，非估算）**：
>
> | 项 | 项数 | 字节 |
> |---|---|---|
> | 正查表（`126 × 191 × uint16_t`） | 24,066 槽位 | 48,132 |
> | 完整反查索引（`(cp,gbk)` × uint32） | 21,791 | 87,164 |
> | **合计（若用完整反查）** | | **≈ 132 KB** |
>
> 实测 GB2312 子集分布：一级汉字（`0xB0A1–0xD7F9`）3,755 字；二级汉字（`0xD8A1–0xF7FE`）3,008 字；全角标点（`0xA1A1–0xA9FE`）715 字；**合计 7,478 项**。
>
> - **方案 A（推荐/采用）**：正查表 **48,132 B**；反查只建 **GB2312 子集**（7,478 项），按码点升序二分查找 → `7478 × 4 B = 29,912 B`。合计 **48,132 + 29,912 = 78,044 B ≈ 76.2 KB flash**。
>
> ⚠️ **此前版本的 `stride=190` 是错的**：`lo` 的取值区间 `0x40..0xFE` 含端点共 **191** 个位置，而 `stride` 写成 190 会让 `hi=0xFE, lo=0xFE` 算出索引 `23940`，**恰好等于表长 23940 → 越界读**（数组末索引是 23939）。修正为 `stride=191`、表长 `24066`。`0x7F` 在 GBK 中不对应任何字符，但**必须保留为占位槽**，否则 stride 不再等于列宽。
> - **方案 B（极致省 flash）**：只收录**实际用到的字符**——小票固定文案 + 默认奖池，实测 < 300 字，表 < 2 KB。但 Web 端用户自定义奖品名会被打成 `?`。
>
> **决策 D5**：采用**方案 A**。理由：奖池名由商家在 Web 自由填写，方案 B 会在现场"打出问号"，风险不可接受；**76.8 KB flash 在 16 MB 里可忽略**（占比 < 0.5%）。
>
> **注意**：方案 A 的反查**仅覆盖 GB2312 子集**，因此"生僻字/繁体/日文假名/Emoji"若不在 GB2312 内会被打成 `?`（并计数告警）。这是**有意的取舍**：GB2312 覆盖现代简体中文日常用字 99.9%+。若确实需要生僻字，用方案 B' —— 按需把额外码点追加进反查表（保持升序，二分查找不变）。

未收录字符 → 输出 `?`（0x3F）并 `ESP_LOGW` 记录码点，保证"永不静默乱码"。

### 6.3 二维码（`raw_printer_qr`）

`tft_displaye` 的厂商核心因 **RISC-V Zbb 指令缺陷**（`PRINTER.md` §七）未实现二维码。二维码由**我们自己拼 ESC/POS `GS ( k`**。

**五条指令，逐字节（已按 Epson 参考手册 `GS ( k` Function 165/167/169/180/181 核对）**：

| 用途 | ASCII | 完整十六进制 | `pL` 含义 |
|---|---|---|---|
| 设模型 | `GS ( k pL pH cn fn n1 n2` | `1D 28 6B 04 00 31 41 32 00` | `pL=4` = 后续 4 字节（`cn fn n1 n2`） |
| 设模块尺寸 | `GS ( k pL pH cn fn n` | `1D 28 6B 03 00 31 43 06` | `pL=3`；`n=6` → 模块 6×6 dots |
| 设纠错级别 | `GS ( k pL pH cn fn n` | `1D 28 6B 03 00 31 45 31` | `pL=3`；`n=49('1')` → 级别 M（15%） |
| 存数据 | `GS ( k pL pH cn fn m d1..dk` | `1D 28 6B <pL> <pH> 31 50 30 <data>` | **`pL = k + 3`**（`cn`,`fn`,`m` 三个字节） |
| 打印 | `GS ( k pL pH cn fn m` | `1D 28 6B 03 00 31 51 30` | `pL=3` |

字段含义：`cn = 0x31`（49，固定），`fn` = `0x41`(65) 设模型 / `0x43`(67) 尺寸 / `0x45`(69) 纠错 / `0x50`(80) 存数据 / `0x51`(81) 打印；`m = 0x30`（48，固定）。

> ⚠️ **易错点：`1D 28 6B 04 00 31 41 32` 只有 8 字节，必须补上 `n2 = 00` 才是合法的 9 字节指令。** `pL=04` 声明了 4 个后续字节（`cn fn n1 n2`），漏掉 `n2` 会让打印机把**下一条指令的第一个字节**当成 `n2` 吞掉，导致后续所有指令整体错位——表现就是"二维码打不出/打出乱码"，且极难肉眼定位。这是本设计**必须用单测锁死字节流**的原因。

- 纠错级别取值：`48('0')=L(7%)`、`49('1')=M(15%)`、`50('2')=Q(25%)`、`51('3')=H(30%)`。本设计用 **M**。
- 数据用**票号 + 校验码的 ASCII**。「20260929-0007」+ 4 位校验 = **17 字节** → `pL = 17 + 3 = 20 = 0x14`, `pH = 0`（< 单包上限 124），无需分包，实现约 60 行。
- 纯字节拼接 → **可单测**（`test_qr_code.cc` 断言字节流精确匹配），不依赖硬件。
- 若**新批次打印机固件不支持 `GS ( k`**，降级为**只打印票号文本**（配置项 `qr_enabled`，默认 `true`，现场失败即关）。

### 6.4 商家 Logo（可上传，`raw_printer_logo`）

需求确认：**可上传商家 logo**。

**核心决策 D14：图像转换放在浏览器侧，固件只收 1-bit 栅格数据。**

理由：ESP32-S3 上塞 PNG/JPEG 解码器要额外组件与 RAM，而浏览器有现成的 canvas 与成熟的重采样/二值化能力。让浏览器把 logo 转成"目标宽度的 1-bit 单色位图"再上传，固件只需存**原始栅格**并用 ESC/POS 栅格指令打印——**零解码库、零动态内存、结果可预览所见即所得**。

**尺寸与内存**：

| 项 | 值 |
|---|---|
| 纸宽 | 58 mm → 384 dots；80 mm → 576 dots |
| 目标宽 | 默认 384 dots（=48 字节/行） |
| 高度上限 | **256 dots**（足够放 logo；限制高度才能定长缓冲） |
| 原始数据 | 58 mm：`48 B/行 × 256 行 = 12,288 B`；80 mm：`72 B/行 × 256 行 = 18,432 B`。按**实际高度**存，不定长 |
| 落盘 | `/spiffs/logo.bin` = 8 B 头 + 位图数据（**最大 `8 + 18,432 = 18,440 B`**）；文件不存在表示未上传 |
| 内存 | `LogoStore::Load()` 用**定长 18,440 B 静态缓冲**（放 PSRAM 或静态数组），避免动态分配 |

**头部格式（8 字节，便于将来改宽度而不破坏兼容）**：

```
偏移 0..3 : magic "LGO1"
偏移 4..5 : width_dots  (uint16 LE, 必须 8 的倍数)
偏移 6..7 : height_dots (uint16 LE)
偏移 8..   : 位图数据，MSB 优先，每行 = width_dots/8 字节，从上到下
```

**上传接口**：`POST /api/logo`（`multipart/form-data` 或 `application/octet-stream`）

- 前端流程：`<input type="file">` → `Image` → 画到 canvas 缩放到目标宽 → 灰度化（`0.299R+0.587G+0.114B`）→ **二值化**（默认阈值 128，提供滑块实时预览）→ 打包头部 + 位图 → `POST`。
- **抖动（dithering）可选**：提供"Floyd–Steinberg 抖动"开关，让带渐变的 logo 打出层次感；默认关（线稿/文字型 logo 用硬阈值更锐利）。
- 固件侧校验：magic 必须 `LGO1`、`width_dots` 为 8 的倍数且 ≤ 576、`height_dots ≤ 256`、**总长度必须精确匹配**（防截断/溢出）。任一不过 → `400` + 中文原因，**不落盘**。
- **写入用"临时文件 + rename"**：先写 `/spiffs/logo.bin.tmp` 再 `rename()`，避免上传中途断电留下半个 logo 导致每次开机打印乱码（决策 D15）。

**打印（ESC/POS 栅格位图）**：

```
GS v 0 m xL xH yL yH d1...dk        →  1D 76 30 00 <xL> <xH> <yL> <yH> <data>
```

- `m = 0`（普通模式），`xL/xH` = 每行字节数（48 → `xL=0x30, xH=0x00`），`yL/yH` = 行数。
- **必须分包发送**：单条 `GS v 0` 的数据量受打印机缓冲限制，且 UART 发送阻塞时间过长会拖慢打印任务。按**每包 ≤ 24 行**（`24 × 48 = 1,152 B`）切分，**每包单独一条 `GS v 0`**（`yL/yH` 为该包行数），包间无需额外延时（UART FIFO 自然流控）。
  > **分包的真正理由（已按源码修正）**：不是"缓冲溢出"——`raw_printer.c` 的 `append()` 是 **write-through**（`raw_printer.c:82-113`）：数据大于缓冲时直接 `send_direct`，装不下时先 `flush_locked`，**调用方永远不会撑爆它**，且 `PRINTER_ERR_NO_SPACE` 在 `raw_printer.c` 中**从未被使用**。
  >
  > **真正原因是超时**：`raw_printer.c` 所有 append 路径**硬编码 `timeout_ms = 1000`**（`raw_printer.c:146/201/.../313`），而链路层**尊重该超时**（`dw_printer_uart.c` 用 `uart_tx_chars` 轮询并超时返回）。实测：**12,288 B @115200 8N1 ≈ 1,067 ms > 1,000 ms → 必然超时失败**；18,432 B ≈ 1,600 ms 更糟。分包成 1,152 B ≈ 100 ms，留 ~10 倍余量。
  >
  > 另：发送缓冲参考实现是 **512 B**（`tft_displaye/main/printer_app.c:31`），不是 1 KB。
- **居中**：logo 宽度小于纸宽时，用 `ESC a 1`（居中）即可，无需手工补白。
- 打印任务开销：12 KB @115200 8N1 ≈ **1.07 s**，占了整票的主要时间。因此**建议现场把 logo 控制在 384×96 以内**（≈ 4.6 KB ≈ 0.4 s）。发送缓冲按 **512 B** 设计（对齐 `tft_displaye/main/printer_app.c:31`）。
- 失败降级：`logo_enabled` 但 `/spiffs/logo.bin` 缺失/校验失败 → **跳过 logo 继续打票**，`ESP_LOGW`，绝不因 logo 问题导致整票打不出。

---

## 七、配置与持久化

### 7.1 新增 Settings 项

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `total_pages` | int | **30** | 总页数（= 可停靠的页数），可配（需求确认） |
| `stroke_min_dmx` | int | 0 | **行程跨度下限**（D21）：映射生成区间起点，0..255 |
| `stroke_max_dmx` | int | 255 | **行程跨度上限**（D21）：映射生成区间终点，0..255。`value_i = min + i*(max-min)/(N-1)` |
| `min_page_gap_dmx` | int | 8 | **相邻页最小间隔**（D21）：开机断言用；现场标定后调大。⚠️ 受 DMX 8-bit 限制，`(max-min)/(N-1) >= 该值` 才可能通过 |
| `step_speed` | int | 60 | `ActionPaging` 的 DMX 速度通道值（沿用语义） |
| `paging_channel_count` | int | 7 | 每台翻页机占用 DMX 通道数（3 或 7） |
| `dmx_start_addr` | int | 1 | DMX 起始地址 |
| `prize_pool` | string(JSON) | 见 §7.2 | 奖池（分级 + 数量 + 概率 + 剩余） |
| `depletion_policy` | int | 0 | 奖品耗尽策略：`0=kFallbackToLose` / `1=kRefuse`（决策 D13） |
| `printer_baud` | int | 115200 | 打印机波特率 |
| `printer_tx_pin` | int | **9** | ESP32 TX（接打印机 RX）。现场可改，免重烧。**禁用 43/44**（D18） |
| `printer_rx_pin` | int | **10** | ESP32 RX（接打印机 TX）。**禁用 43/44**（D18） |
| `printer_flow_pin` | int | **11** | 打印机 CTS 所在脚（流控关闭时仅占位） |
| `printer_flow_mode` | int | **0** | 0=关/1=CTS/2=RTS/3=双向。默认关，避免两个输出端抢一根线 |
| `paper_cols` | int | 32 | 纸宽列数：58 mm=32 / 80 mm=48 |
| `qr_enabled` | bool | true | 二维码开关 |
| `logo_enabled` | bool | false | 是否在票头打印 logo |
| `logo_path` | string | `/spiffs/logo.bin` | logo 位图路径（§6.4） |
| `auto_return_ms` | int | 1500 | `kDone` 停留时长 |
| `ticket_prefix` | string | "" | 票号前缀（多场地区分） |
| `serial_daily` | int | 0 | 当日流水号 |
| `serial_date` | string | "" | 流水号所属日期（跨日归零判定） |
| `shop_name` | string | "幸运翻页机" | 票头商家名（UTF-8，可空） |
| `win_title` | string | "恭喜中奖" | 中奖标题（可编辑） |
| `lose_title` | string | "感谢参与" | 未中奖标题（可编辑） |
| `receipt_header` | string | 见 §7.3 | 票头模板（可编辑，支持占位符） |
| `receipt_footer` | string | 见 §7.3 | 票尾模板（可编辑，支持占位符） |
| `validity_days` | int | 7 | 领奖有效期天数（供 `{validity}` 推算），0 = 不显示该行 |

### 7.2 奖池 JSON 格式

```json
{
  "version": 2,
  "total_pages": 30,
  "depletion_policy": 0,
  "tiers": [
    { "level": 1, "page": 0, "weight": 1,  "quantity": 1,  "remaining": 1,  "name": "一等奖",   "note": "奖品：扫地机器人" },
    { "level": 2, "page": 1, "weight": 5,  "quantity": 5,  "remaining": 5,  "name": "二等奖",   "note": "奖品：蓝牙音箱"   },
    { "level": 3, "page": 2, "weight": 20, "quantity": 30, "remaining": 30, "name": "三等奖",   "note": "奖品：定制水杯"   },
    { "level": 4, "page": 3, "weight": 74, "quantity": -1, "remaining": 0,  "name": "谢谢参与", "note": "欢迎下次再来"     }
  ]
}
```

> `quantity = -1` 表示**不限量**（兜底级用）。`weight` 是**相对权重**不是百分比；Web 端需显示实时"理论概率"列（`weight_i / Σweight`）。默认 30 页时，前几级只占 0~3 页，其余 26 页留给后续扩展或"谢谢参与"的多个停靠位。

**保存时同步自检**（任一不过返回 `400` + 中文原因）：

| 校验 | 规则 | 报错示例 |
|---|---|---|
| 页码范围 | `0 <= page < total_pages` | `"页码 35 超出范围（共 30 页）"` |
| 页码唯一 | 两级不得占同一 `page` | `"页码 2 已被「三等奖」占用"` |
| 等级唯一 | `level` 不重复且在 `1..N` | `"等级 2 重复出现"` |
| 数量合法 | `quantity >= 1` 或 `-1` | `"一等奖数量必须是正数或不限量"` |
| 剩余合法 | `0 <= remaining <= quantity` | `"三等奖剩余 40 超过总量 30"` |
| 兜底级存在 | `depletion_policy==0` 时必须有一个 `quantity == -1` 的级 | `"存在奖品耗尽风险，请设一个不限量的兜底等级（如「谢谢参与」）"` |
| 至少一级可抽 | 存在 `weight > 0 && remaining != 0` | `"所有等级均已耗尽或权重为 0，无法开奖"` |
| 级数上限 | `N <= 16` | `"最多 16 个等级，当前 20 个"` |

- 存 NVS（`Settings` 的 string 项，cJSON 解析）；**同时**落盘 `/spiffs/prizes.json` 便于导入导出。
- **加载失败兜底**：JSON 非法 → 保留上一份有效配置（`prize_pool_backup`）并 `ESP_LOGE`；两份都坏 → 用**编译内建的默认 4 级奖池**（设备永远能开奖，永不"砖"在无奖池状态）。
- **库存持久化（决策 D12，已按源码修正）**：`remaining` 扣减后必须走 **`NvsSaver::SaveSync()`（同步写盘）**，**不能用 `RequestSave()`**。

  > ⚠️ **修正说明**：此前版本写"每次扣减后 `RequestSave()`，断电宁多发一个"——**该论断不成立**。`RequestSave()` 是**非阻塞异步且会静默丢弃**的（`page-player/main/nvs_saver.cc`）：`xQueueSend(queue_, &signal, 0)` 队列深度仅 **2**（`nvs_saver.h`），满即丢；真正写盘在低优先级任务里（`nvs_saver.cc`）且**无完成通知**。因此"落盘"并不保证发生，**超发数量不可控**，不能宣称"只多发一个"。
  >
  > 库存扣减是**低频事件**（每次抽奖间隔 ≥5 s），同步写盘代价可忽略，因此**直接用 `SaveSync()`**，获得"扣减即落盘"的真实语义。
  >
  > **同步/异步划分**：库存、票号流水 → `SaveSync()`（低频、要求强持久化）；`current_page` 等高频状态 → `RequestSave()`（可容忍丢失）。

- **⚠️ `nvs_saver.cc` 绝对不能"原样搬运"**：`NvsSaver::DoSave()` 是**硬编码字段全量清单**（`page-player/main/nvs_saver.cc:63-97`，逐行 `write_i32(...)`），`Settings::Load()` 也只读**固定 7 个 key**（`page-player/main/settings.cc:115-160`）。**不在清单里的字段永远不会落盘、也不会被读回。**
  本项目新增的 `prize_pool` / `receipt_*` / `shop_name` / `printer_*` / `paper_cols` / `logo_*` / `depletion_policy` / `serial_*` **全部不在其中** → 按"原样搬运"会导致**奖池库存永不持久化（一等奖可无限抽中）、商家配置每次重启丢失**。
  **必须把 `DoSave()` 与 `Settings::Load()` 改为数据驱动**（一张 `{key,type,accessor}` 表驱动四处），或显式补全并加"字段清单一致性"单测。→ 见计划 Task 0.3 新增 Step。

### 7.3 小票文案模板（可编辑）

需求确认：**小票文案可在 Web 端编辑**。采用**占位符模板**，而非硬编码布局。

**可用占位符**：

| 占位符 | 替换为 | 示例 |
|---|---|---|
| `{shop}` | `shop_name` | 幸运翻页机 |
| `{prize}` | 中奖等级名 | 一等奖 |
| `{note}` | 等级副标题 | 奖品：扫地机器人 |
| `{ticket}` | 票号 | 20260929-0007 |
| `{time}` | 开奖时间 | 2026-09-29 14:32:07 |
| `{page}` | 停靠页码（1-based 展示） | P1 |
| `{validity}` | 有效期日期（按 `validity_days` 推算） | 2026-10-06 |
| `{check}` | 校验码 | 3F2A |
| `{title}` | 中奖/未中奖标题（按结果自动选 `win_title`/`lose_title`） | 恭喜中奖 |
| `{nl}` | 换行 | — |

**内置默认模板**（商家可覆盖）：

- `receipt_header` = `{shop}{nl}══════════════════{nl}`
- 票体（固定结构，无需编辑）：`{title}`（2 倍字号居中）→ `{prize}`（2 倍字号加粗居中）→ `{note}`（居中）→ 分隔线 → `票号：{ticket}` / `时间：{time}` / `页码：{page}`
- `receipt_footer` = `────────────────────────{nl}请凭此票到服务台领奖{nl}有效期至：{validity}{nl}`

**排版规则**（`ReceiptLayout` 负责，纯逻辑可单测）：

1. **宽度按显示列计**：ASCII = 1 列，GBK 双字节字符 = 2 列。居中按**显示宽度**补空格，不按字节数。
2. 每行显示宽度 **不得超过 `paper_cols`**；超出则按显示宽度**安全折行**（绝不切断多字节字符）。
3. 占位符替换在**转 GBK 之前**（先按 UTF-8 组装，再整体转码），避免半字符转码错误。
4. 出现**未知占位符** → 原样保留 + `ESP_LOGW`（不静默丢弃）。
5. `GET /api/receipt/preview` 返回**渲染后的纯文本预览**，前端可"打印预览"，降低现场试错成本。

### 7.3b 小票块模板 v3（可编辑内容框架，2026-09-29 新增）

**需求**：商家在 Web UI 上自由编辑小票的**内容框架** —— 添加**固定文字**与**参数化文字**
（当前日期时间等），调整顺序/对齐/字号，并在任意位置放 Logo。

**为什么升级**：v2 只能改 `header`/`footer` 两段文本，中间的标题/奖项/信息行是硬编码的，
商家无法增删或重排。v3 把整张票变成**有序块列表**。

#### 数据模型（存 NVS `receipt_tmpl`，JSON）

```json
{
  "version": 3,
  "paper_cols": 32,
  "blocks": [
    {"type": "logo"},
    {"type": "line", "text": "{shop}", "align": "center"},
    {"type": "line", "text": "==================", "align": "center"},
    {"type": "line", "text": "{title}", "align": "center", "size": 2, "bold": true},
    {"type": "line", "text": "{prize}", "align": "center", "size": 2, "bold": true},
    {"type": "line", "text": "{note}", "align": "center"},
    {"type": "separator"},
    {"type": "line", "text": "票号：{ticket}"},
    {"type": "line", "text": "时间：{datetime}"},
    {"type": "line", "text": "页码：P{page}"},
    {"type": "line", "text": "校验：{check}"},
    {"type": "separator"},
    {"type": "line", "text": "请凭此票到服务台领奖"},
    {"type": "line", "text": "有效期至：{validity}"},
    {"type": "qr", "content": "{ticket}{check}"},
    {"type": "feed", "lines": 4}
  ]
}
```

#### 块类型

| type | 字段 | 约束 |
|---|---|---|
| `line` | `text` `align`(left/center/right) `size`(1\|2) `bold`(bool) | text 可含参数；按显示宽度折行 |
| `separator` | `char`（默认 `-`） | 用该字符填满纸宽 |
| `qr` | `content` `size`(1..16，默认 6) `ecc`(48/49/50/51，默认 49) | content 支持参数 |
| `logo` | — | 插入已上传 Logo；未上传/损坏则**跳过**（D15） |
| `feed` | `lines`(0..10，默认 4) | 走纸 N 行 |

整体约束：`blocks` 数量 1..64；单块 `text`/`content` ≤ 512 字节。

#### 参数（占位符）

| 占位符 | 含义 | 示例 |
|---|---|---|
| `{shop}` | 商家名 | 幸运翻页机 |
| `{title}` | 标题（按是否中奖自动取 `{win_title}`/`{lose_title}`） | 恭喜中奖 |
| `{prize}` `{note}` | 奖项名 / 副标题 | 一等奖 / 奖品：扫地机器人 |
| `{ticket}` `{check}` `{page}` | 票号 / 校验码 / 页码(1-based) | 20260929-0007 / 3F2A / 3 |
| `{validity}` | 有效期（`validity_days>0` 时） | 2026-10-06 |
| `{date}` `{time}` `{datetime}` | 当前日期 / 时间 / 两者 | 2026-09-29 / 14:32:07 |
| `{year}` `{month}` `{day}` | 年月日（补零） | 2026 / 09 / 29 |
| `{hour}` `{minute}` `{second}` | 时分秒（补零） | 14 / 32 / 07 |
| `{weekday}` | 星期 | 星期二 |
| `{nl}` | 换行（兼容 v2 模板） | |

- 大小写敏感；未知参数**原样保留**并计数（Web 预览会告警）。
- 日期时间一律按 CST-8（与票号一致），来源 `TimeSource::NowEpoch()`。

#### 迁移（v2 → v3，必须无损）

`receipt_tmpl` 为空时，用旧字段生成等价块模板：
`header` 按行拆块（居中）→ 标题/奖项/副标题块 → 信息行块 → 分隔线 → `footer` 按行拆块 → QR → feed。
**验收**：同一组数据下，迁移后渲染的**可见文本**与 v2 路径逐行一致（自动化测试锁定）。

#### Web API（新增 5 个端点）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/receipt/template` | 当前生效模板（含 v2 迁移结果）+ `shop_name`/`logo_enabled` |
| PUT | `/api/receipt/template` | 校验并保存模板（畸形 JSON → 400 + 中文原因，绝不落库） |
| POST | `/api/receipt/template/preview` | **保存前预览**（body = 模板 JSON，可选 `"lose":true`）→ `{text, unknown_params, blocks}` |
| GET | `/api/receipt/params` | 参数清单 + 示例值 + 出厂默认模板（UI 直接渲染，避免两套清单漂移） |
| POST | `/api/time` | 浏览器校时（**AP-only 现场无 NTP 的必要补充**，见下） |

#### 时间源问题（本特性暴露的真实限制）

设备默认只开 AP（供商家连 `192.168.4.1`），**没有 STA 就没有 NTP**，
于是 `{date}/{time}` 会打印占位符、票号日期也会退化为 1970。补充手段：
- `POST /api/time`：商家一打开配置页，浏览器就把本机时间送给设备校准（零成本、自动）
- 串口 `ptime <epoch>`：无浏览器时可手工校时
- `TimeSource::SetFromWeb()` 只接受 2020..2100 的合理值，并把该时间持久化为回退基准

#### 串口命令（新增）

| 命令 | 用途 |
|---|---|
| `pdemo 1\|2\|3` | 安装内置演示模板（1 经典 / 2 简洁 / 3 促销），持久化并打印预览 |
| `ptime <epoch>` | 手工校时 |
| `ptmpl <json>` | 用给定模板渲染预览（不落库；受串口行缓冲限制，长模板请走 Web） |

#### 测试矩阵（全部纯函数，片上可跑）

| 组 | 用例 |
|---|---|
| JSON | 往返相等（含全部块类型）/ 非法 JSON / 缺字段取默认 / version 不符 |
| 校验 | 未知 type / 空 blocks / 超 64 块 / align 非法 / size 越界 / ecc 非法 / feed 越界 / text 超长 |
| 参数 | 全部参数精确断言（固定 epoch CST-8）/ 未知参数原样+计数 / `{}` `{` `}` 边界 / 重复参数 |
| 排版 | 折行不切断多字节 / size=2 折半 / 左中右对齐填充数 / 分隔线填满 32 与 48 |
| 字节 | size=2 → `1D 21 11`；bold → `1B 45 01/00`；QR 五指令精确；feed N 个 LF |
| 金字节 | 默认模板 + 固定数据 → 与期望字节完全一致（锁死回归） |
| 迁移 | v2 header/footer → v3，可见文本逐行等价 |
| Logo | 有 Logo 插入 `GS v 0` 分包；无 Logo 跳过且其余照常（D15） |
| 预览 | 每行显示宽度 ≤ 纸宽（含长短文本/长 URL 二维码，32 与 48 列）/ 不含指令字节 |
| 校时 | `SetFromWeb` 拒 0/1970/2101 等坏值；接受合理值并持久化；校时后 `{date}` 即刻可用 |
| 畸形输入 | 18 种畸形模板 JSON 必须"返回错误且不崩"（真机曾因 use-after-free 触发 39 次重启） |

### 7.4 配置校验时机

- Web `PUT /api/prizes` / `PUT /api/receipt` 时**同步**校验，非法直接 `400 + 中文错误原因`，不落库。
- 开机加载时再校验一次；失败走 §7.2 兜底链。

---

## 八、接口设计

### 8.1 REST API（无认证；局域网使用）

**抽奖与状态**

| 方法 | 路径 | 作用 | 返回 |
|---|---|---|---|
| `POST` | `/api/draw` | 手动触发一次抽奖（等价按钮） | `200 {status:"started"}` / `409 {status:"busy",state:"..."}` |
| `GET` | `/api/lottery/status` | 轮询当前状态与最近结果 | 见下 |
| `GET` | `/api/status` | 设备状态（页数、DMX、打印机就绪、票号、logo 是否已上传） | JSON |
| `POST` | `/api/printer/test` | 打一张测试票（含中文 + logo + 二维码） | `200 {status:"queued"}` |
| `POST` | `/api/page/goto` | **标定点动**：驱动到指定页（`{page}`），供逐页核对行程值 | `200 {status:"started"}` / `409` |

**奖池（分级 / 数量 / 概率）**

| 方法 | 路径 | 作用 | 返回 |
|---|---|---|---|
| `GET` | `/api/prizes` | 读奖池（含 `remaining` 与**实时理论概率**） | `{version,total_pages,depletion_policy,tiers:[...]}` |
| `PUT` | `/api/prizes` | 写奖池（§7.2 自检通过后落 NVS + SPIFFS） | `200` / `400 {error:"中文原因"}` |
| `POST` | `/api/prizes/reset-stock` | 把各级 `remaining` 重置回 `quantity`（新一期活动用） | `200` / `400` |
| `GET` | `/api/pages` | 读总页数与行程跨度（**只读**；names/values 归 `/api/page-mapping`） | `{total_pages, stroke_min_dmx, stroke_max_dmx, min_page_gap_dmx}` |
| `PUT` | `/api/pages` | **只改 `total_pages`**（改前校验没有 `page` 越界；**不再接收 names/values**，避免与 page-mapping 职责重叠 —— N5） | `200` / `400` |
| `PUT` | `/api/page-mapping` | **行程值唯一权威入口**：`{values:[N], names:[N]}` → `PageMapping::SetMapping` → `SaveMapping` + `PageAdjust::Resize(1,N)` | `200` / `400 {error:"行程值退化/长度不足"}` |

**小票文案与 Logo**

| 方法 | 路径 | 作用 | 返回 |
|---|---|---|---|
| `GET` | `/api/receipt` | 读文案配置（模板 + 商家名 + 标题 + 有效期） | JSON |
| `PUT` | `/api/receipt` | 写文案配置（未知占位符告警、行宽校验） | `200` / `400 {error:"..."}` |
| `GET` | `/api/receipt/preview` | 用**当前配置 + 样例数据**渲染预览文本 | `{text:"...", lines:[...]}` |
| `POST` | `/api/logo` | 上传 logo（`LGO1` 头 + 1-bit 栅格，§6.4） | `200 {width,height,bytes}` / `400` |
| `GET` | `/api/logo` | 读 logo 元信息（不返回位图本体） | `{exists,width,height,bytes}` / `404` |
| `DELETE` | `/api/logo` | 删除 logo | `200` |

> **端点较多是需求驱动的**（奖池分级配置 + 文案可编辑 + logo 上传），不是过度设计。所有写操作统一走"**同步校验 → 校验通过才落库**"的路径。

`GET /api/prizes` 额外返回**实时理论概率**（前端直接显示，避免商家拿 `weight` 当百分比心算）：

```json
{
  "version": 2,
  "total_pages": 30,
  "depletion_policy": 0,
  "total_weight_active": 100,
  "tiers": [
    { "level": 1, "page": 0, "name": "一等奖", "weight": 1, "probability": 0.01,
      "quantity": 1,  "remaining": 1,  "depleted": false },
    { "level": 4, "page": 3, "name": "谢谢参与", "weight": 74, "probability": 0.74,
      "quantity": -1, "remaining": -1, "depleted": false }
  ]
}
```

**Web 前端页面（5 页）**：`index.html`（抽奖 + 状态 + 测试票 + 归零倒计时）、`prizes.html`（分级奖池 + 概率列 + 库存）、`receipt.html`（文案编辑 + 实时预览）、`logo.html`（上传 + 二值化阈值/抖动预览）、**`calibrate.html`（30 页行程标定 + 逐页点动）**。

`GET /api/lottery/status`：

```json
{
  "state": "boot|homing|homing_failed|idle|spinning|printing|done|out_of_prizes",
  "draw_id": 128,
  "last": {
    "ticket": "20260929-0007",
    "prize_name": "一等奖",
    "page": 0,
    "time": "2026-09-29 14:32:07",
    "print": "ok|failed|skipped"
  },
  "stats": { "total_draws": 512, "today_draws": 7 }
}
```

- `POST /api/draw` 在 `boot/homing/homing_failed/out_of_prizes/spinning/printing` 状态下返回 **`409 {status:"busy", state:"<当前状态>"}`**，前端据此提示"正在归零，请稍候"。仅 `idle`（及 `done`）接受触发。
- **`out_of_prizes`**：全部限量奖级已耗尽且 `depletion_policy = kRefuse` 时进入（N3）。这是**正常业务终态**，不是故障——Web 应显示"奖品已发完"而不是红色错误。
- 前端 `index.html` 在 `homing` 时显示进度提示与倒计时（**由后端 `homing_remaining_ms` 驱动**，约 10.5 s；**不要在前端写死秒数**），避免被误认为卡死（R9）。

**并发约束 —— 读写两路都要定义线程归属（M3，防 use-after-free）**

此前版本只约束了**写**，把**读**留成了真空。但读路径同样危险，而且**后果是崩溃而非脏读**：

- `ReceiptTemplate` / `PrizePool` 内部用 `std::string` / `std::vector`。若主线程（因写请求）**重新赋值**这些对象，而 HTTP 任务同时持有 `c_str()` 或迭代器 → **use-after-free（LoadProhibited 复位）**。
- `GET /api/lottery/status` 被前端**高频轮询**，若也走 `Schedule()` + 等信号量，会给主线程造成持续压力。

**读写线程模型（强制）**：

| 类别 | 端点 | 数据访问方式 |
|---|---|---|
| **写** | `POST /api/draw`、`PUT /api/prizes`、`PUT /api/pages`、`PUT /api/receipt`、`POST/DELETE /api/logo`、`POST /api/prizes/reset-stock` | handler 只解析 JSON → `Application::Schedule()` → **主线程**校验+落库。handler 不触碰任何业务对象 |
| **重读**（需渲染/计算） | `GET /api/prizes`（算实时概率）、`GET /api/receipt/preview`（渲染模板）、`GET /api/receipt`、`GET /api/logo` | **同样走主线程**：handler `Schedule()` 一个任务，主线程把结果构造成**值类型**（`std::string` JSON / 纯文本）后回填，handler 再发送。**禁止**在 HTTP 任务里直接读业务对象 |
| **轻读**（高频） | `GET /api/lottery/status`、`GET /api/status` | **不过 `Schedule()`**，走**双缓冲 + 原子序号（seqlock）快照**，见下方实现约束 |

**⚠️ 为什么不能直接"用 `std::atomic` 装快照"（本设计的自我纠正）**

`std::atomic<T>` 要求 `T` **可平凡复制（trivially copyable）**。而 `last` 里含 `ticket` / `prize_name` / `time_str` 这些**变长文本**——若用 `std::string` 就**不满足该要求，无法放进 `std::atomic`**，硬做会编译失败或退化为加锁。

**正确做法：双缓冲 + 原子序号（seqlock），快照内只用定长 `char[]`**

```cpp
// main/lottery/lottery_status.h —— 全部是定长 POD，无指针、无堆分配、可平凡复制
struct LotteryStatusSnapshot {
    uint8_t  state;                 // LotteryState 的底层值
    uint32_t seq;                   // 写序号（偶数=稳定，奇数=写入中）
    uint32_t draw_id;
    uint8_t  page;
    uint8_t  print_outcome;
    uint8_t  printer_fault;         // R8 的红色状态灯
    uint32_t total_draws;
    uint32_t today_draws;
    char     ticket[20];            // 定长，UTF-8/ASCII，' ' 结尾
    char     prize_name[32];        // 定长
    char     time_str[24];          // "2026-09-29 14:32:07"
};
static_assert(sizeof(LotteryStatusSnapshot) <= 128, "快照必须保持小且定长");

// 写（仅主线程）：写非活动缓冲 → 更新 seq → 用 release 语义发布
// 读（HTTP 任务，无锁）：
//   do { s1 = seq.load(acquire); 若 s1 为奇数则重试; 拷贝 slot[s1&1]; s2 = seq.load(acquire); }
//   while (s1 != s2);            // 期间发生过写入 → 重读
```

- 写入方**只写非活动槽位**，再用原子序号发布；读方校验序号前后一致，**绝不阻塞写方，也永不读到半个快照**。
- **禁止**在快照结构里出现 `std::string` / `std::vector` / 裸指针 / 任何需要析构的成员。
- **禁止**用 `std::atomic<LotteryStatusSnapshot>`（结构含 `char[]` 虽可平凡复制，但拷贝成本高且 `seq` 机制更清晰）；用"槽位数组 + 原子 `seq`"。
- `GET /api/status`（设备状态：DMX/打印机/页数）同样按此模式，或**直接归入"重读"走主线程**（它是低频接口，走主线程更简单）。**二选一，实现时定死并写注释。**

> **规则**：**跨线程共享的任何数据，要么是不可变的值快照（无指针），要么只能由主线程访问。** 绝不允许 HTTP 任务持有可能被主线程释放的指针/引用。
>
> `POST /api/page/goto {page}`（标定点动，见 §8.1）也走写路径（`Schedule` 到主线程）。

### 8.1b WiFi STA 配置与连接（2026-09-29 新增）

**需求**：让设备连上商家路由器（STA），从而获得 **NTP 时间**（解决 §7.3b 的"AP-only 无时间源"），
同时保留自己的 AP 作为配置通道与兜底。

#### 模式选择：APSTA（不是二选一）

| 模式 | 何时用 | 为什么 |
|---|---|---|
| **APSTA** | 配置了 SSID 时（默认） | STA 拿时间/联网；AP 始终在，`192.168.4.1` 永远可进（配错了也能救回来） |
| AP-only | SSID 为空 | 出厂/未配置；商家直连设备配置 |

> 关键：**绝不能"设为 STA 后 AP 就消失"** —— 一旦密码输错，商家就再也进不去配置页了。

#### 借鉴 page-player 的实现要点（2026-09-29 核对）

| 项 | 做法 |
|---|---|
| AP 信道 | **必须为 0（自动跟随 STA 信道）**，写死信道会在 APSTA 下诱发 STA auth 失败 |
| DHCP DNS | 指向本机 AP IP，客户端 captive portal（CNA）探测才会弹配置页 |
| 省电 | `WIFI_PS_NONE`（保证 HTTP/抽奖响应） |
| 驱动 NVS | `cfg.nvs_enable=false`（不写驱动自己的 NVS 副本） |
| 多网络 | 最多 10 个，最近使用排最前，满则丢最旧；连不上自动换下一个 |
| STA 状态机 | kIdle/kScanning/kConnecting/kConnected/kDisconnected + 操作白名单（连接中禁止扫描） |
| 不照搬 | 其"事件处理里 `vTaskDelay` 重连"会阻塞事件循环 → 本项目用独立退避任务 |

#### 数据与字段

| 字段 | 键 | 默认 | 说明 |
|---|---|---|---|
| `wifi_list` | `wifi_list` | "" | 多网络 JSON（含密码，仅设备内部用；接口只回 SSID + 是否加密） |
| `wifi_ssid` | `wifi_ssid` | "" | 当前/最近使用的 SSID（空 = 仅 AP） |
| `wifi_pass` | `wifi_pass` | "" | 空 = 开放网络；否则 8..63 字符 |

- 明文存 NVS（本设备离线自用；**接口与日志一律不回显密码**，见下）
- SSID 按**原始字节**保存（路由器 SSID 常为 UTF-8 中文，原样透传）

#### 校验规则（纯函数，可测）

| 项 | 规则 |
|---|---|
| SSID | 长度 1..32 **字节**（UTF-8 中文字符按字节计）；不得含控制字符 |
| 密码 | 空（开放）或 8..63 字节；不得含控制字符 |
| 拒绝 0 长度的 SSID 配置为 STA | — |

#### 连接行为（绝不阻断抽奖）

1. 开机：`esp_netif_init` → `esp_event_loop_create_default` → `esp_wifi_init` →
   创建 AP/STA netif → `WIFI_MODE_APSTA`（有 SSID 时）→ start
2. STA 连接失败：指数退避重试（1s→2s→4s…**上限 60s**），期间 AP 照常、抽奖照常
3. 连上（`IP_EVENT_STA_GOT_IP`）：记录 IP/RSSI，触发一次 SNTP 校时
4. 断开（`WIFI_EVENT_STA_DISCONNECTED`）：记原因（密码错/找不到 AP），按退避重连
5. 任何 WiFi 错误只记日志，**绝不 `abort()`**（D23 原则）

#### Web API（新增 4 个端点）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/wifi` | 状态：模式、STA 是否连上、SSID、**STA IP**、RSSI/信号格、AP 客户端数、是否已配置、**是否已校时**（**不回显密码**） |
| POST | `/api/wifi/scan` | 扫描周边 AP → `[{ssid, rssi, bars, secure}]`（按信号强→弱排序，去重） |
| PUT | `/api/wifi` | `{ssid, password}` → 校验并保存 + 异步重连；**该校验失败返回 400 且不保存** |
| DELETE | `/api/wifi` | 忘记网络 → 清 SSID/密码 + 断开，回到 AP-only |

#### 串口命令（新增）

`wifi`（状态+IP）、`wifi scan`、`wifi set <ssid> <pass>`、`wifi off`

#### 测试矩阵（纯函数为主，片上可跑）

| 组 | 用例 |
|---|---|
| 校验 | SSID 0/1/32/33 字节；密码 0(开放)/7/8/63/64；控制字符；中文 SSID（UTF-8 11 字节）；边界恰好 32/63 |
| 模式 | 无 SSID → AP-only；有 SSID → APSTA；`wifi off` 后回 AP-only |
| 退避 | 1s→2s→4s→…→上限 60s 封顶；attempt 负数/超大不越界 |
| JSON | **SSID 含引号/反斜杠/控制字符时的转义**（路由器名叫 `"; drop` 不得破坏 JSON）；**状态 JSON 绝不含密码字段** |
| 信号 | RSSI → 0..4 格映射（-30 强 … -90 弱/−100 无） |
| 快照 | 状态为定长 POD（跨线程读，与 D23 同规），`static_assert` 平凡可拷贝 |
| 持久化 | ssid/pass 存取往返；`wifi off` 清空；密码不出现在 `Settings` 打印摘要里 |

### 8.2 按钮

复用 `Button`（iot_button）的 **`OnPress`**（与 `page-player` 的"按下即触发"语义一致），回调内只调 `Application::Schedule(...)`，不阻塞。

**去抖**：`iot_button` 自带；`LotteryController` 的状态机额外兜底（`kSpinning` 期间丢弃）。

**长按**：`OnLongPress` 绑定"打印测试票"，方便现场不联网排查打印机。

### 8.3 串口命令（现场排障，沿用 tft_displaye 的思路）

| 命令 | 作用 |
|---|---|
| `draw` | 触发一次抽奖（等价按钮） |
| `ptest` | 打测试票 |
| `pstatus` | 打印机 UART/波特率/成功失败计数 |
| `pdump` | 最后一次发出的原始字节 hex（核对 ESC/POS 与 GBK） |
| `prizes` | 打印当前奖池到日志 |
| `gbtest <中文>` | 转码自检：输出原文 + GBK hex（快速定位乱码） |

---

## 九、打印机组件搬运方案

`tft_displaye/components/dw_printer` 的核心结论：

- 厂商预编译库 `dwPrinterSDK_V1.0.1.a` 的 **RISC-V 版带 Zbb 扩展**，P4/C6/H2 会 `Illegal instruction`。
- **ESP32-S3 是 Xtensa，不受影响**——理论上可以用厂商核心。
- 但 `tft_displaye` 已用 `raw_printer.c` 实现了**等效的文本/原始字节/缓冲**，**零动态内存**，且不依赖任何预编译二进制。

**决策 D6：`lucky-page` 采用 `raw_printer.c` 路径，不用厂商 .a。**

理由：
1. 免二进制依赖 → 可审计、可跨架构（S3/P4/C6 都能编）；
2. 无动态内存，符合 `page-player/AGENTS.md` 的嵌入式约束；
3. 测试票的字节流已在 `tft_displaye` **实机验证通过**（`PRINTER.md` §四，连续 4 张无失败）；
4. 我们需要的文本能力它已完整覆盖。

**搬运清单**：

| 源文件 | 目标 | 改动 |
|---|---|---|
| `include/raw_printer.h` | `components/dw_printer/include/` | 原样 |
| `raw_printer.c` | 同上 | 原样 |
| `dw_printer_uart.c` | `lucky_printer_uart.c` | 裁剪：去掉 flow-control 调试命令、去掉控制台命令，保留 `wr/rd/delay` 回调 + 尊重超时 |
| `include/printer_qrcode.h` 等厂商头 | 不搬 | v1 用自定义 `raw_printer_qr.c` |
| `lib/*.a`、`adapter/` | 不搬 | — |

**新增**：`raw_printer_qr.h/.c`（§6.3）。

---

## 十、构建与验证

### 10.1 构建

```bash
# 一次性
idf.py set-target esp32s3
# 日常
scripts/build.sh s3          # 构建到 build-esp32s3/
scripts/build.sh s3 flash monitor
```

### 10.2 单元测试（TDD，先写测试）

`test_apps/` 用现有 `test_apps` 风格（`stub_settings.cc` 等桩），覆盖**纯逻辑**模块：

| 测试文件 | 覆盖 | 关键断言 |
|---|---|---|
| `test_prize_pool.cc` | 加权抽取、边界、校验 | 100 万次注入均匀随机，各奖频次误差 < 0.5%；`page` 重复配置被拒 |
| `test_slot_machine_effect.cc` | `Plan()` | ① 跳页数 ∈ [min,max]；② `pages[0..n-2] != final_page`；③ 末跳 == `final_page`；④ delay 单调不减且 ∈ [min,max] 且为 40 的倍数；⑤ 无相邻重复页 |
| `test_gbk_encoder.cc` | UTF-8→GBK | `"一等奖"` → `D2BB B5C8 BDB1`（逐字节断言，防同类笔误）；ASCII 透传；未收录字 → `?`；非法 UTF-8 不崩溃 |
| `test_receipt_layout.cc` | 排版 | 58 mm 行宽 ≤ 32 字符；票号格式 `YYYYMMDD-NNNN`；校验码稳定性 |
| `test_draw_record.cc` | 票号序列 | 同日递增；跨日归零；**epoch 由外部注入**（不依赖网络）；NVS 读写往返 |
| `test_qr_code.cc` | `GS ( k` 字节流 | 精确匹配预期 hex；**锁 `bytes[9..15]` 为下一条指令头**（漏 `n2` 必然错位）；`pL = len+3` |
| `test_printer_logo.cc` | `LGO1` 头 + `GS v 0` 分包 | ① 头部校验 7 类非法输入全被拒（含**长度精确匹配**）；② 96 行 → 4 包、每包 ≤1152 B；③ 100 行的余数包 `yL=4`；④ **位序 MSB 优先**（`cmds[0][7]==0x80`）；⑤ **LE 逐字节解析**（禁止 `reinterpret_cast`） |
| `test_lottery_controller.cc` | 归零状态机 | ① 映射不足/不互异时 `Start()` 返回 false 且**不下发动作**；② 序列结构 = 1 个 `ActionSequence` 含 2 个 `ActionGroup`（第 1 组 `ch07=150` + 10000 ms）；③ 稳定期 500 ms 在**序列末尾**且 `kHomed` 只置一次；④ `state_ != kHoming` 时回调直接 return；⑤ 超时 3 次 → `kHomingFailed` |

> **共 8 个测试文件。** ⚠️ **每个用例都必须含真实断言**——`AGENTS.md` §6.4 明令"禁止写无断言的测试"，且只有注释的空壳用例在 Unity 下**恒通过**，会让"先看到红灯"的 TDD 步骤变成假承诺。
> 计划的每个 TDD 任务的"运行 → 必须 Red"均以此为前提。

### 10.3 硬件验证（编译 + 真机日志，按 AGENTS.md §6.3 妥协方案）

1. **编译验证**：`scripts/build.sh s3` 零错误零警告。
2. **串口日志**：烧录后自动开 `logs/serial-<ts>.log`，检查启动各模块 `ESP_LOGI`。
3. **翻页验证**：`draw` 命令触发，日志打印 `HopPlan` 全序列 + 预期总时长，肉眼核对机器落页与 `final_page` 一致。
4. **打印验证**：`pstatus` / `pdump` 确认字节流 → `ptest` 出票 → 肉眼核对**中文不乱码**、二维码可扫。
5. **Web 验证**：`curl` 打全部端点（含各 400 分支）；浏览器测 4 页：奖池保存后刷新保留、文案预览与实打一致、logo 上传后打印清晰。

### 10.4 不做 OTA 的例外说明

v1 不做 OTA 门户（非目标）。但保留分区表里的 OTA 双槽，以便后续加 OTA 时**不需要改分区**（改分区会导致现场设备无法原地升级）。

---

## 十一、风险与待确认

| ID | 风险 | 影响 | 缓解 |
|---|---|---|---|
| **R1** | **已解决**：`GPIO43/44` 实为板上 UART0（`TX0/RX0`）且接板载 USB-UART 桥 | 若误用会与桥抢总线、干扰下载 | **改用 `GPIO47`(TX)/`GPIO48`(RX)**（实物照片核对，§4.2）。`page-player/docs/esp32s3-pinout.md` 与实际板子不符，已标注以实物为准。引脚 NVS 可改，免重烧 |
| **R2** | 打印机丝印 RX/TX 方向相反（`tft_displaye` 已踩过） | 不出票 | 排障第一步即对调数据线；`pdump` + `pstatus` 先证明固件侧已发出 |
| **R3** | 打印机固件不支持 `GS ( k` 二维码 | 二维码打不出 | `qr_enabled` 开关；降级为纯票号文本 |
| **R4** | **奖池页码与实际行程没对齐 → 停错页（可行性问题，非风险提示）** | 奖品发错（业务事故） | 见下方"**30 页分辨率问题**"专节——**"目视校准"改变不了分辨率**，必须补"行程跨度"配置 + 最小间隔判据 |
| **R5** | 翻页机械抖动/丢步导致落页偏差 | 停错页 | 落定后**二次定位**：等 `expected_duration_ms + 500 ms` 再补发一次 `final_page` 的 `ActionPaging`（幂等） |
| **R6** | GBK 表占用 **78,044 B ≈ 76.2 KB** flash（实测） | 体积 | 可接受（16 MB flash，占比 < 0.5%）。**反查仅覆盖 GB2312（7,478 字）**，生僻字/繁体/假名/Emoji 会打成 `?` 并告警；商家文案请用简体常用字 |
| **R7** | 断网/无 NTP → 票号时间错误 | 小票时间不对 | 无网络时用开机时间 + 持久化的"最后已知时间"；票号流水号不依赖时钟（只用于跨日判定） |
| **R8** | 纸尽/过热未检测（未接打印机 RX） | 打不出票且无提示 | v1 不接 RX 做状态轮询，但**不能只靠人工敲 `pstatus`**：① `PrinterService` 对连续失败做**自动重试**（默认 2 次、间隔 300 ms）；② 连续失败 ≥3 次置 **`printer_fault`**，`/api/lottery/status` 与 Web 首页显示**红色状态灯**并提示检查纸张/连线；③ 每次失败 `ESP_LOGE` 带累计计数 |
| **R9** | **开机归零需约 10.5 s**，商家可能嫌慢 | 现场体验 | 归零是开环机构的**必需代价**（§5.5）。若不可接受，唯一出路是**加装位置传感器/限位开关**走闭环（硬件改动）；Web 端在 `kHoming` 显示"正在归零，请稍候"避免误解为卡死 |
| **R10** | 归零期间断电/反复重启 → 每次都等 11 s | 调试体验差 | 归零状态**不持久化**；调试期可开 Kconfig `CONFIG_LUCKY_SKIP_HOMING_ON_BOOT`（**仅调试，出厂必须在 `sdkconfig.defaults` 显式设 n**） |
| **R11** | **库存与概率的组合可能"发不出奖"**：高级奖设 `weight=1` 又设 `quantity=1`，抽中概率极低，可能整场活动一等奖发不出去 | 商家投诉"内定" | Web 端显示实时**理论概率**与"预计抽中所需次数"；`prizes.html` 对 `probability < 0.5%` 的级给出**黄色警示**；建议现场按 `1/概率 × 预计客流` 核算投放量 |
| **R12** | **Logo 上传后打印乱码/过长** | 票打不出 | 头部严格校验（magic/宽度为 8 倍数/高度 ≤256/长度精确）；`GS v 0` 必须**分包 ≤24 行**（真实原因是**超时**：`raw_printer.c` 所有 append 硬编码 `timeout_ms=1000`，而 12,288 B @115200 ≈ 1,067 ms > 1000 ms 必然失败；**不是**缓冲溢出——`append()` 是 write-through，`PRINTER_ERR_NO_SPACE` 从未被使用，见 §6.4）；写盘用临时文件 + `rename`（避免半截 logo）；logo 异常一律**跳过 logo 继续打票**，绝不因 logo 导致整票失败 |
| **R13** | 30 页 × 每页行程值若未重新标定，`PageMapping` 还是旧的 10 页长度 | 页码越界 → 静默返 0 → 永远停同一位 | §5.5 的 `kBoot` 硬断言（映射长度 ≥ `total_pages` 且行程值非退化）；`PUT /api/pages` 改页数时**强制要求重新核对映射**，否则拒绝保存 |

**阻塞项状态：已全部闭环，仅剩评审**

| 项 | 状态 |
|---|---|
| 打印机 TX/RX 引脚 | ✅ **已定**：按实物照片（`docs/ESP32S3-PIN.png`）核定为 **`GPIO47`(TX) / `GPIO48`(RX)**。`GPIO43/44` 是板上 `TX0/RX0`（接 USB-UART 桥），**不能用于打印机** |
| 奖池默认数值 | ✅ **已定**：30 页 + 4 级（一等 1 个 / 二等 5 个 / 三等 30 个 / 谢谢参与不限量；权重 1:5:20:74）。直接写进**编译内建默认奖池** |
| 小票文案 | ✅ **已定**：用内置默认模板（全部文案仍可在 `receipt.html` 编辑） |
| 出卡机联动（弹卡） | ✅ **不做**：明确列为非目标，不加任何 GPIO/串口触发 |
| Logo 默认开关 | `logo_enabled = false`（避免未上传时白跑）；上传后可在 `logo.html` 一键开启。**此项无需你确认**，属合理默认 |
| **评审** | ⏳ **待进行**——这是进入实现的唯一前置（`page-player/AGENTS.md` 规则 4） |

> **交付状态：本设计已冻结，等待评审。** 未经评审不写任何实现代码。

---

## 十二、关键设计决策汇总

| ID | 决策 | 理由 |
|---|---|---|
| D1 | 芯片选 **ESP32-S3** | 1 台翻页机 + 1 台打印机，S3 资源足够，成本最低 |
| D2 | **先抽奖 → 再翻到该页**（页码由奖品决定） | 保证顾客所见（停在第几页）与打印结果 100% 一致 |
| D3 | 减速用 **缓入缓出 + 对齐 40 ms tick + 末段邻域擦边 + 弹跳** | 拟真水果机手感；对齐 tick 避免抖动 |
| D4 | **单飞**：`kSpinning/kPrinting` 期间丢弃所有新触发 | 机械机构不可重入；避免重复打票 |
| D5 | GBK 表用**完整正查（stride 191）+ GB2312 子集反查**（**78,044 B ≈ 76.2 KB**） | 商家可自由填奖品名（GB2312 内），绝不现场打出问号；占比 < 1% flash |
| D6 | 打印机用 **`raw_printer.c`**，不用厂商 `.a` | 免二进制依赖、零动态内存、跨架构、已实机验证 |
| D7 | 新增 `LotteryController`，**不搬运** `PagingController` | SRP：抽奖状态机与通用翻页职责分离 |
| D8 | 中文排版**不走厂商核心**，应用侧转 GBK | 实测厂商核心只保证 ASCII，中文必乱码 |
| D9 | 奖池非法配置**三级兜底**（NVS → 备份 → 编译内建） | 设备永远能开奖，绝不"砖"在无奖池 |
| D10 | 保留 OTA 双槽分区但不做 OTA 门户 | 后续加 OTA 无需改分区（改分区无法原地升级） |
| D11 | **开机必须归零，归零完成前禁止开奖**（§5.5） | 翻页机是开环机构，无法回读物理位置；不归零则 D2（落页==票面）在重启后会失效，属业务事故 |
| D12 | **概率与数量分开建模**，`quantity` 跟踪 `remaining` 库存 | 只按概率抽会让"一等奖 1 个"发出多个，现场奖品不够，属业务事故。库存耗尽后该级自动退出候选集 |
| D13 | 奖品耗尽策略可配：**默认 `kFallbackToLose`**（回退到不限量的"谢谢参与"级），可选 `kRefuse`（拒绝开奖） | 商家通常要保证机器永远能出票（不冷场）；但若宁可不开奖也不发错奖，可切 `kRefuse`。`kFallbackToLose` 要求配置里必须存在一个 `quantity == -1` 的兜底级，否则保存即报错 |
| D14 | **Logo 图像转换放浏览器侧**，固件只收 `LGO1` 头 + 1-bit 栅格 | S3 上塞 PNG/JPEG 解码器要额外组件与 RAM；浏览器 canvas 有成熟重采样/二值化/抖动能力，且**所见即所得可预览**。固件侧零解码库、零动态内存 |
| D15 | Logo 写盘用**临时文件 + `rename()`**；Logo 异常一律**跳过 logo 继续打票** | 上传中途断电若留下半截 logo，会导致**每次开机打票都乱码**；异常降级保证"票一定能打出来"，不因装饰性内容阻断主功能 |
| D16 | **总页数（默认 30）与页码名称可配**，改页数时强制重核 `PageMapping` | 30 页需要 30 个不同行程值；若映射仍是旧的 10 页长度，`GetAdjustedValue()` 会静默返回 0 → 永远停同一位（R13） |
| D17 | 小票文案用**占位符模板**（`{prize}`/`{ticket}`/…），排版按**显示宽度**（中文 2 列）计算 | 商家要能改文案；硬编码布局改不了。按显示宽度而非字节数排版，才能避免中文行超宽/错位 |
| D18 | **打印机用 `GPIO47`(TX) / `GPIO48`(RX)**；**禁止** `GPIO43/44`（实物上是 `TX0/RX0`，接板载 USB-UART 桥） | 以实物照片 `docs/ESP32S3-PIN.png` 为准。`page-player/docs/esp32s3-pinout.md` 标 43/44"未使用"，**与实际板子不符**，本项目不采用该文档的引脚结论。误用 43/44 会与桥抢总线并干扰固件下载 |
| D19 | **实物照片是"板级引脚"的唯一依据**；`page-player` 的引脚文档只作脚本/代码参考 | 避免再次基于错误文档做硬件决策。文档与实物冲突时，**先改文档再改代码**，并把差异写进 `board config.h` 注释 |
| D20 | 板型 = **ESP32-S3-N16R8**（16 MB Flash + 8 MB **Octal** PSRAM），分区用 **`partitions/v2/16m.csv`** | 实物 Flash 是 16 MB。用 8m 分区表会浪费一半 Flash，且后续 OTA/资源扩展受限 |
| D21 | 补**行程跨度**配置（`stroke_min_dmx`/`stroke_max_dmx`）+ 断言升级为**互异性 + 最小间隔** + 明确「30 页可用性判据」（§4.2b） | 30 页均匀映射只有 **3.1%/页** 行程，机械精度不足就跨页落错。**逐页标定只能平移、不能拉开间距**——这是分辨率问题，必须在开机断言暴露，不能留到开奖随机出错 |
| D22 | 库存/票号流水走 **`SaveSync()`**；`current_page` 等高频状态走 `RequestSave()`；**`nvs_saver` 不可原样搬运**，必须扩展字段清单 | `RequestSave()` 异步且静默丢弃（队列深度 2），无法保证库存落盘；且 `DoSave()`/`Settings::Load()` 是硬编码字段表，新字段默认**永不持久化** |
| D23 | Web **读**路径也纳入线程模型：重读走主线程（值类型回填）；高频轻读走**双缓冲 + 原子序号（seqlock）+ 全定长 POD 快照** | 主线程重赋值 `std::string` 而 HTTP 任务持 `c_str()` → **use-after-free 复位**，不是脏读。**且 `std::atomic` 要求可平凡复制，装不了含 `std::string` 的结构**——故快照必须全用定长 `char[]` + 序号发布，不能是 `std::atomic<含 string 的结构>` |

---

## 7.3c 小票模版「类型/模式」体系（2026-09-30 扩展设计）

### 背景

小票版式已经演化出两种**本质不同**的排版方式：

1. **画布模式**（当前实现）：背景图（花纹边框/水印）与文字**逐像素叠加**，
   由设备端把整票合成成一张位图后整幅输出（`GS v 0`）。
   优点：四边完整边框、可做水印；代价：**帧大（约 32KB）、打印慢（约 8 秒）、框内行数受图高限制**。
2. **花边模式**（原"方案 B"）：只把图**顶部一段**与**底部一段**当作花边打印，
   中间内容走打印机自带文字（普通行式）。
   优点：**行数不受限、帧小（约 3~4KB）、打印快（约 1.5 秒）**；代价：**画不出左右竖线**。

两种方式各有取舍，且取舍点不同（外观完整度 ↔ 速度/容量），因此**不应二选一写死**，
而应作为**模版类型**由用户选择。本设计据此扩展。

### 模式定义

| 模式 | JSON `mode` | 渲染方式 | 帧大小 | 打印耗时 | 行数上限 | 左右竖线 |
|---|---|---|---|---|---|---|
| 纯文字 | `flow` | 全部走打印机文字 | ~0.5KB | ~1s | 不限 | 无 |
| **画布** | `canvas` | 整票位图合成（文字与图逐像素叠加） | ~32KB | ~8s | 受图高限制 | **有** |
| **花边** | `banner` | 上下两段花边位图 + 中间文字 | ~3~4KB | ~1.5s | **不限** | 无 |

> `flow` 是既有的"无背景"路径，一并纳入模式体系，使三种模式**同构可选**。

### 数据模型（向后兼容）

```json
{
  "version": 3,
  "mode": "canvas",            // flow | canvas | banner，缺省 = flow
  "paper_cols": 32,
  "banner_top_rows": 2,        // 仅 banner：顶部花边占几行（1 行 = 24 点）
  "banner_bottom_rows": 3,     // 仅 banner：底部花边占几行
  "blocks": [ ... ]
}
```

**兼容规则**：
- `mode` 缺省时按下述推断，**旧模版行为不变**：
  - 有 `bg` 块 → `canvas`（与当前行为一致）
  - 无 `bg` 块 → `flow`
- `banner_*` 仅在 `banner` 模式生效；其它模式忽略（不报错，便于切换来回试）

### 渲染分派

```
Render(template, params, logo)
  └─ switch (mode)
       ├─ kFlow   → RenderText()            既有行式路径
       ├─ kCanvas → RenderHybrid()          既有（评审 §23）
       └─ kBanner → RenderBanner()          新增
```

`RenderBanner` 的纸面顺序：

```
[顶部花边]  ← 从图片裁前 top_rows*24 行，整幅 GS v 0
[内容文字]  ← 打印机文字，行数不限（含二维码指令就地打印）
[底部花边]  ← 从图片裁后 bottom_rows*24 行，整幅 GS v 0
[末尾空行]  ← feed 块（裁剪用）
```

关键点：
1. 花边高度以**行**为单位（`banner_*_rows`），保证与文字行栅格**严格对齐**
   （与 §23.3 的 `ESC 3 24` 同理）
2. 裁图需要"按行裁 1-bit 位图"的能力 → `BmpCanvas` 增加 `DrawImageRows(...)`
   或直接切片（源数据按行连续，切片是 O(1) 偏移）
3. 花边模式**不做叠加**（无文字压图）；需要叠加就用画布模式
4. 图片高度校验：`(top_rows + bottom_rows) * 24 <= 图高`，否则模版非法

### UI（画布页）

- 顶部新增「**版式类型**」下拉：`纯文字 / 画布（整框）/ 花边（上下分段）`
- 选择「花边」时，属性区出现：**顶部花边行数**、**底部花边行数**（1..10）
- 画布上按所选模式渲染元素位置（花边模式下首行内容从顶部花边之后开始）
- 底部提示条显示**该模式的代价**（帧大小/耗时/行数限制），让用户明明白白选

### 与其它模块的关系

| 模块 | 影响 |
|---|---|
| Logo/背景图上传 | 不变（三模式共用同一个图槽） |
| 服务端预览 | 三模式都要能预览（花边模式用文本预览 + 花边占位行标注） |
| 抽奖出票 | 不变（同一 `printer_service` 调用，仅渲染分派不同） |
| 迁移 | 旧模版 `mode` 缺省 → 按上述推断，**输出与升级前逐字节一致**（有回归测试锁定） |

### 测试矩阵

| 用例 | 断言 |
|---|---|
| 缺省 mode + 无 bg | 走 flow，输出与旧版**逐字节一致** |
| 缺省 mode + 有 bg | 走 canvas，输出与旧版逐字节一致 |
| banner 模式 | 输出含两段 `GS v 0`，且**两段之间是文字**（顺序断言） |
| banner 行数对齐 | 两段花边的字节长度 = `rows*24*stride` |
| banner 图片过矮 | 校验失败并给出明确原因 |
| banner 忽略 bg 块 | 即使有 bg 块也不做整票位图（帧 < 阈値） |
| 三模式切换 | 同一 blocks 三种 mode 都能渲染成功（不崩、不空） |

### 7.3d 模板按「类型」分槽存储（2026-09-30 扩展）

**需求**：每种版式类型应有**各自的模板**；切换类型时**刷新/切换**到该类型的模板。

**为什么不共用一份**：三模式的排版语义差异极大（画布用 row/col + 背景层，花边用
`banner_*_rows`，纯文字用流式），共用一份会出现"切到某类型后版式面目全非"。

#### 存储（NVS）

| 键 | 含义 |
|---|---|
| `receipt_tmpl` | **flow**（纯文字）模板（沿用旧键，兼容） |
| `tmpl_canvas` | **canvas**（画布）模板 |
| `tmpl_banner` | **banner**（花边）模板 |
| `tmpl_mode` | 当前生效模式 0=flow / 1=canvas / 2=banner |

键长均 ≤14（NVS 限制，有单测锁定）。

#### 行为

1. **加载**：`LoadTemplateFromSettings()` 读**生效模式**的槽；槽为空 → 回退**该模式的内置版式**
   （flow=经典 / canvas=水印 / banner=花边），保证任何模式下都有可用模板。
2. **保存**：`PUT /api/receipt/template` 把模板存进**它自己的模式槽**并设为生效模式
   ——「切换类型 + 保存」一步到位。
3. **读取指定类型**：`GET /api/receipt/template?mode=0|1|2` 读该槽（**不改变**生效模式），
   UI 切换下拉时用它加载该类型模板。
4. **升级迁移**：`MigrateTemplateSlots()` 把旧的单槽模板按 `EffectiveMode()` 搬进对应槽
   （只搬一次、不覆盖已有槽），用户配置不丢。

#### UI

画布页「版式类型」下拉切换即**拉取并载入该类型的模板**（画布内容整体刷新），
改完点「保存模板」存回当前类型；底部提示明确告知"已载入「X」类型的模板"。

#### 测试矩阵

| 用例 | 断言 |
|---|---|
| `PerModeSlots.EachModeKeepsItsOwnTemplate` | 三槽互不干扰；按生效模式取到各自模板（含 banner 行数） |
| `PerModeSlots.EmptySlotFallsBackToBuiltinOfThatMode` | 槽为空时回退该模式内置版式，且模式正确 |
| 浏览器实测 | 切换 flow/canvas/banner → 画布载入 18 / 11（含背景）/ 9 个元素 |
