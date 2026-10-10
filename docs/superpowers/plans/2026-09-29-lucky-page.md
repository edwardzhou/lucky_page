# lucky-page 实现计划 —— 单机抽奖翻页机

> **Goal:** 在 `lucky-page/` 新建一个只控制 **1 台翻页设备**的抽奖固件：按下按钮 → 水果机随机翻页减速落地 → TTL 热敏打印机打中奖小票。
>
> **Architecture:** 以 `page-player` 为基线裁剪（只保留单机 DMX 输出 + 翻页动作管线 + 按钮 + 最小 Web），新增 `LotteryController`（抽奖状态机）与 `PrinterService`（打印队列）两个领域模块；打印机驱动从 `tft_displaye/components/dw_printer` 搬运 `raw_printer.c` 并新增 GBK 转码与 ESC/POS 二维码。
>
> **Tech Stack:** ESP-IDF 5.5.x / C++17 / FreeRTOS / cJSON / ESP32-S3 (Xtensa) / 东为-兼容 ESC/POS 热敏打印机
>
> **前置文档：** `docs/superpowers/specs/2026-09-29-lucky-page-design.md`（必须先评审通过）
>
> **设计决策引用：** D1~D23 见设计文档 §12；风险 R1~R13 见 §11

---

## 开工前必须确认（阻塞项）

- [x] **Q1** 打印机 TX/RX GPIO —— **已按实物照片 `docs/ESP32S3-PIN.png` 核定：`GPIO47`(TX) / `GPIO48`(RX)**。
      ⚠️ `GPIO43/44` 在该板上丝印为 `TX0/RX0` 且接板载 USB-UART 桥，**不可用**（`page-player/docs/esp32s3-pinout.md` 与实际板子不符）。
      ⚠️ `GPIO47` 在 `page-player` 是 LCD MOSI，但 `lucky-page` 不驱动 LCD，故空闲——`board config.h` 必须注释此差异。
- [x] **Q2** 机台页数与奖池 —— **已确认：默认值可用（30 页 + 4 级）**。
      一等 `page=0` 数量 1 / 二等 `page=1` 数量 5 / 三等 `page=2` 数量 30 / 谢谢参与 `page=3` 不限量；权重 `1:5:20:74`；`depletion_policy = kFallbackToLose`。
      这组值直接作为**编译内建默认奖池**（三级兜底的最末一级），保证开箱即用。
- [x] **Q3** 小票文案 —— **已确认：内置默认模板可用**（`{shop}` / `{title}` / `{prize}` / `{note}` / 票号 / 时间 / 页码 / 校验 / `请凭此票到服务台领奖` / 有效期 7 天）。
      文案全部可在 `receipt.html` 改，不阻塞实现。
- [x] **Q4** **不需要出卡机联动（弹卡）** —— 明确列为非目标，**不加** GPIO/串口触发。
- [ ] **Q5** 设计文档评审通过（按 `page-player/AGENTS.md` 规则 4，方案评审 + 计划评审）。

> ✅ **本计划已放行执行**（用户 2026-09-29 明确「开始按照设计实现」）。
> 本计划已按**两轮** Momus 评审修订完毕（含 44 条空壳测试重写为真实断言），评审记录见 `docs/superpowers/reviews/2026-09-29-review.md`。

---

## 文件结构

| 文件 | 操作 | 职责 |
|---|---|---|
| `lucky-page/CMakeLists.txt` | 新增 | `project(lucky-page VERSION 0.1.0)` |
| `lucky-page/sdkconfig.defaults` | 新增 | `CONFIG_IDF_TARGET="esp32s3"`、PSRAM、tick 1000 Hz、C++ 异常 |
| `lucky-page/partitions/16m.csv` | 搬运 | 沿用 page-player **16m**（实物 N16R8 = 16 MB Flash；含 OTA 双槽，D10） |
| `lucky-page/scripts/build.sh` | 搬运+裁剪 | 仅保留 `s3` target |
| `lucky-page/scripts/gen_gbk_table.py` | 新增 | 生成 `main/printer/gbk_table.cc` |
| `lucky-page/main/CMakeLists.txt` | 新增 | 源文件清单 + SPIFFS 资源打包 |
| `lucky-page/main/Kconfig.projbuild` | 新增 | 板型/打印机引脚默认值 |
| `lucky-page/main/main.cc` | 搬运+裁剪 | 入口 |
| `lucky-page/main/application.{h,cc}` | 搬运+裁剪 | 单例调度中心 |
| `lucky-page/main/settings.{h,cc}` | 搬运+扩展 | + 奖池/打印机/票号配置 |
| `lucky-page/main/nvs_saver.{h,cc}` | 原样搬运 | 异步 NVS 保存 |
| `lucky-page/main/lottery/prize.h` | 新增 | `PrizeTier` 结构（等级/页码/权重/数量/剩余）+ 约束常量 |
| `lucky-page/main/lottery/prize_pool.{h,cc}` | 新增 | 分级奖池 + 加权抽取 + **库存扣减/耗尽策略**（纯逻辑） |
| `lucky-page/main/lottery/slot_machine_effect.{h,cc}` | 新增 | 减速跳页计划（纯逻辑） |
| `lucky-page/main/lottery/lottery_controller.{h,cc}` | 新增 | 抽奖状态机 |
| `lucky-page/main/lottery/draw_record.{h,cc}` | 新增 | 票号 + 当日流水（纯逻辑，**epoch 由外部注入**） |
| `lucky-page/main/lottery/time_source.{h,cc}` | 新增 | 时间源：SNTP > NVS 回退 > 编译时间（N-6） |
| `lucky-page/main/lottery/lottery_controller.{h,cc}` | 新增 | 抽奖**与开机归零**状态机（合并，M-5） |
| `lucky-page/main/motion/*` | 搬运子集 | `action*`, `cell.h`, `pipeline.*`, `motion_engine.*` |
| `lucky-page/main/page_mapping.{h,cc}` | 搬运 | 页码 → DMX 行程映射（设计 D2 前置依赖） |
| `lucky-page/main/page_adjust.{h,cc}` | 搬运 | 逐页微调偏移（slot machine 每次跳页都调用） |
| `lucky-page/main/dmx512/dmx512_service.{h,cc}` | 搬运+裁剪 | 1 路 UART 纯输出 |
| `lucky-page/main/printer/gbk_encoder.{h,cc}` | 新增 | UTF-8 → GBK |
| `lucky-page/main/printer/gbk_table.cc` | 生成物 | 生成后入库 |
| `lucky-page/main/printer/receipt_layout.{h,cc}` | 新增 | **占位符模板渲染** + 显示宽度排版（纯逻辑） |
| `lucky-page/main/printer/printer_logo.{h,cc}` | 新增 | `LGO1` 头校验 + `GS v 0` 分包（纯逻辑） |
| `lucky-page/main/printer/logo_store.{h,cc}` | 新增 | `/spiffs/logo.bin` 读写（临时文件 + rename） |
| `lucky-page/main/printer/printer_service.{h,cc}` | 新增 | 队列 + 打印任务 |
| `lucky-page/main/printer/printer_uart.{h,cc}` | 新增 | UART2 bring-up |
| `lucky-page/components/dw_printer/` | 搬运+裁剪 | `raw_printer.c` + `lucky_printer_uart.c` |
| `lucky-page/components/dw_printer/raw_printer_qr.{h,c}` | 新增 | `GS ( k` 二维码 |
| `lucky-page/main/boards/esp32s3_lucky/config.h` | 新增 | 新板级引脚 |
| `lucky-page/main/boards/*` | 搬运+裁剪 | `board`, `wifi_board`, `common/button` |
| `lucky-page/main/web/web_server.{h,cc}` | 搬运+裁剪 | 抽奖/状态 + 奖池分级 + 页数 + 文案 + logo 端点（§设计 8.1） |
| `lucky-page/main/web/assets/spiffs_contents/{index,prizes,receipt,logo,calibrate}.html` | 新增 | 前端 **5** 页（抽奖 / 奖池 / 文案 / logo / 行程标定） |
| `lucky-page/test_apps/**` | 搬运+新增 | 单测宿主 + 8 个测试文件 |

---

## Phase 0：骨架与构建打通

### Task 0.1：创建工程骨架与 target

**Files:** `CMakeLists.txt`, `sdkconfig.defaults`, `partitions/16m.csv`, `main/CMakeLists.txt`, `main/main.cc`, `scripts/build.sh`

- [ ] **Step 1** 从 `page-player` 复制 `partitions/v2/16m.csv`、`scripts/build.sh`，改 `scripts/build.sh` 只保留 `s3`。
  **实物是 N16R8（16 MB Flash + 8 MB Octal PSRAM）**，故 `sdkconfig.defaults` 必须含 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`、`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/16m.csv"`、`CONFIG_SPIRAM_MODE_OCT=y`（直接以 `page-player/sdkconfig.defaults.esp32s3` 为基线抄）。
- [ ] **Step 2** 写根 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
project(lucky-page VERSION 0.1.0)
```

- [ ] **Step 3** 写最小 `sdkconfig.defaults`（target/PSRAM/tick/C++ 异常，抄 `page-player/sdkconfig.defaults.esp32s3`）。
- [ ] **Step 4** 写只打日志的 `main/main.cc` + 最小 `main/CMakeLists.txt`。
- [ ] **Step 4b（B-5，必须）** 在 `main/CMakeLists.txt` 加 **SPIFFS 镜像打包**，分区名必须与 `partitions/v2/16m.csv` 的 `assets` 一致（照 `page-player/main/CMakeLists.txt:74-77`）：

```cmake
set(SPIFFS_CONTENTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/spiffs_contents")
if(EXISTS "${SPIFFS_CONTENTS_DIR}")
    spiffs_create_partition_image(assets "${SPIFFS_CONTENTS_DIR}" FLASH_IN_PROJECT)
endif()
```
  `main/CMakeLists.txt` 的 `REQUIRES` 必须含 `spiffs`。

> ⚠️ **漏掉这一步的后果**：`/spiffs/` 分区**没有镜像** → Web 4 个页面全 404、`/spiffs/prizes.json` 不存在、`/spiffs/logo.bin` 无处可写（**Task 1.4 的 Logo 功能整体失效**）、出厂 `page_mapping.json` 无法下发。整个 Phase 4 与 Logo 功能会白做，且到那时才发现。

- [ ] **Step 4c** 建 `main/assets/spiffs_contents/`，放一个占位 `index.html`，**烧录后立刻 `curl http://<ip>/index.html` 确认 200**（早验证，不要拖到 Phase 4）。
- [ ] **Step 5** 编译验证：

```bash
cd lucky-page && bash scripts/build.sh s3          # scripts/build.sh 自带 IDF 环境（page-player/scripts/build.sh:7-10）
```
Expected: 链接成功产出 `build-esp32s3/lucky-page.elf`。

> **注意**：`set-target` 首次需联网下载组件元数据；后续增量构建离线可用。

---

### Task 0.2：搬运 Motion 动作管线子集

**Files:** `main/motion/{action.h,action_paging.*,action_delay.*,action_sequence.*,action_group.*,action_repeat.*,cell.h,pipeline.*,motion_engine.*}`, `main/page_mapping.*`, `main/page_adjust.*`

- [ ] **Step 1** 从 `page-player/main/motion/` 复制上述文件（**不改逻辑**，只删与多机/巡页相关的非必需部分）。
- [ ] **Step 2** `page_adjust.*` 与 `page_mapping.*` 从 `page-player/main/` 复制。二者接口（**保持不变**）：

```cpp
// page_adjust.h —— slot machine 每次跳页都要取调整后的行程值
class PageAdjust {
public:
    static PageAdjust& GetInstance();
    int8_t  GetOffset(int cell, int page) const;
    uint8_t GetAdjustedValue(int cell, int page) const;   // clamp(mapping[page] + offset)
    void    SetOffset(int cell, int page, int8_t offset);
    void    Resize(int cell_count, int total_pages);      // cell_count 恒为 1
    void    LoadAdjust();  void SaveAdjust();  void ResetAll();
};

// page_mapping.h —— 页码 → DMX 行程值
class PageMapping {
public:
    static PageMapping& GetInstance();
    const std::vector<uint8_t>&     GetMapping();     // O(1)，首次调用自动加载
    const std::vector<std::string>& GetPageNames();
    void SetMapping(const std::vector<uint8_t>& values,
                    const std::vector<std::string>& names = {});
    void SetDefaultMapping(int total_pages);          // 均匀分配 255/N
    void LoadMapping();  void SaveMapping();
};
```

- [ ] **Step 3** 确认 `ActionPaging` 构造签名与 page-player 一致：

```cpp
ActionPaging(int cell_id, int page, uint8_t page_value,
             uint8_t ch02 = 0, uint8_t ch03 = 0, uint8_t ch04 = 0,
             uint8_t ch05 = 0, uint8_t ch06 = 0, uint8_t ch07 = 0,
             int paging_channel_count = 7);
```

- [ ] **Step 4** 编译验证。

---

### Task 0.3：搬运 Settings / NvsSaver / Dmx512Service（单路裁剪）

**Files:** `main/settings.{h,cc}`, `main/nvs_saver.{h,cc}`, `main/dmx512/dmx512_service.{h,cc}`, `main/boards/**`

- [ ] **Step 1** 复制 `nvs_saver.*`（原样）。
- [ ] **Step 2** 复制 `settings.*`，**删除**非目标项（`timer_*`、`artnet_*`、`working_mode` 保留但只留抽奖模式、`cell_mapping_mode` 简化为 direct）。
- [ ] **Step 2b（B-3 / D22，**最易漏且致命**）** **扩展持久化字段清单**。

> `NvsSaver::DoSave()` 是**硬编码字段全量清单**（`page-player/main/nvs_saver.cc:63-97`，逐行 `write_i32`），
> `Settings::Load()` 只读**固定 7 个 key**（`page-player/main/settings.cc:115-160`）。
> **不在清单里的字段永远不落盘、也读不回来。** 若按"原样搬运"，本项目新增的
> `prize_pool` / `receipt_*` / `shop_name` / `printer_*` / `paper_cols` / `logo_*` /
> `depletion_policy` / `stroke_*` / `serial_*` **全部失效** → **库存永不持久化（一等奖可无限抽中）+ 商家配置每次重启丢失**。

  **做法（推荐数据驱动，避免未来再漏）**：把字段清单抽成**一张表**，由它统一驱动 `Load()` 与 `DoSave()`：

```cpp
// main/settings_fields.inc（或 settings.cc 内的静态表）
// { key, 类型, 成员偏移/访问器 }
// 新增字段只改这一处 → Load 与 DoSave 自动同步
```
  若为省时间选择"手写补全"，**必须**加一条**一致性单测**：遍历 `Settings` 声明支持的全部 key，断言每个都出现在 `DoSave()` 的实现里。**否则这是会反复复发的坑。**

- [ ] **Step 2c** 把 `cell_count == 1` 的**强制提前到这里**（原在 Task 3.2 Step 5，属 Phase 3 末尾，中间可能带错配置运行）：`Settings::Load()` 后立即断言并强制置 1，`ESP_LOGE` 记录被纠正的值。
- [ ] **Step 3** 复制 `dmx512_service.*`，裁剪为 **1 路 UART 纯输出**（删除 ArtNet 输入模式、CH432T、`DmxInputMode` 切换）。
- [ ] **Step 4** 复制 `boards/{board.h,board.cc,wifi_board.h,wifi_board.cc,common/**}`，新增 `boards/esp32s3_lucky/config.h`（设计 §4.2 引脚表）。
- [ ] **Step 4b（B-1 / D18）** 断言打印机引脚默认值正确，并在 `config.h` 顶部写明差异：

```cpp
// main/boards/esp32s3_lucky/config.h 顶部注释（必须保留）
// ⚠️ 本板 G43/G44 = TX0/RX0，接板载 USB-UART 桥，禁止占用（设计 D18/D19）。
// ⚠️ 打印机引脚以设计 §4.3 的实物标定值为准（G9/G10/G11），勿照抄 tft_displaye（那是 P4 板）。
#define PRINTER_UART_NUM  UART_NUM_2
#define PRINTER_TX_PIN    GPIO_NUM_9    // 必须 9（设计 §4.3 实物标定）
#define PRINTER_RX_PIN    GPIO_NUM_10   // 必须 10
```
```cpp
static_assert(PRINTER_TX_PIN == GPIO_NUM_9, "打印机 TX 必须为 G9（设计 §4.3；禁用 G43/44）");
static_assert(PRINTER_RX_PIN == GPIO_NUM_10, "打印机 RX 必须为 G10（设计 §4.3；禁用 G43/44）");
```
  ⚠️ **设计 §7.1 的 `printer_tx_pin`/`printer_rx_pin` 默认值必须是 9/10**（2026-09-29 真机校正；曾误改为 47/48，见评审 §八）。B-1 的原始风险是设计 §7.1 曾写 43/44——实现者照抄那张表就会把打印机接在 USB-UART 桥上，**完全不通且极难归因**。
- [ ] **Step 5** 编译验证：确认 `Dmx512Service` 单路裁剪后无残留引用。

---

### Task 0.4：单测宿主工程 `test_apps`（M-7，必须）

> ⚠️ **不建这个，所有单测都跑不起来。** `page-player/test_apps/main/CMakeLists.txt` 用
> `file(GLOB)` **白名单式**列源文件——只编译 `main/motion/`、板级、display、audio，外加**显式列出的 2 个** main 源文件。
> 本项目新增的 `prize_pool.cc` / `slot_machine_effect.cc` / `gbk_encoder.cc` / `receipt_layout.cc` /
> `printer_logo.cc` / `draw_record.cc` 等 **一个都不在里面** → **8 个测试文件全部链接失败**。

**Files:** `test_apps/CMakeLists.txt`, `test_apps/sdkconfig.defaults`, `test_apps/main/CMakeLists.txt`, `test_apps/main/{main.cc,idf_component.yml,stub_settings.cc,stub_wifi_board.cc}`, `test_apps/run_tests.py`

> ⚠️ **`stub_settings.cc` / `stub_wifi_board.cc` 必须一起搬**（N-5）。它们提供被测模块依赖的 `Settings::GetInstance()`、`NvsSaver::SaveSync()`、`Board::GetInstance()` 等桩实现。
> SRCS 里的 `draw_record.cc`（注入日期/计数）与 `slot_machine_effect.cc`（`Build()` 经 `PageAdjust`→`Settings`）**都依赖 `Settings`**；缺桩 → **链接失败**。

- [ ] **Step 1** 搬运 `test_apps/CMakeLists.txt` + `sdkconfig.defaults` + `main/main.cc` + `main/idf_component.yml` + **`main/stub_settings.cc`** + **`main/stub_wifi_board.cc`** + `run_tests.py`。
- [ ] **Step 1b（N-5）** **同步 `stub_settings.cc` 到新签名**。本项目给 `Settings` 增加了 `prize_pool`/`receipt_*`/`stroke_*` 等 getter/setter，并新增了 `NvsSaver::SaveSync()` 的调用路径；**stub 必须逐一对齐**，否则单测桩与真机 `settings.cc` 行为不一致（单测绿、真机错）。
  做法：把 `stub_settings.cc` 里所有 `Get*/Set*` 与真机 `settings.h` 的公开接口做一次清单比对，**缺一个就补一个**；并加一条编译期断言（或在 fake 里 `static_assert`）确保没有遗漏的纯虚/未实现符号。
> 🔴 **N-4 顺序依赖（必须先解决）**：下面的 SRCS 含 `gbk_table.cc`，而它由 **Task 1.2（Phase 1）** 的脚本生成。
> 若在干净树上就走到本 Task，CMake 会因**文件不存在**在配置期直接报错。
> **解决办法（采用后者，不动 Phase 结构）**：在 Task 0.4 **之前**先跑一次生成脚本产出占位表：

- [ ] **Step 0（N-4，必须最先做）** 先把生成脚本与空表落地，使 `gbk_table.cc` 存在：

```bash
python3 scripts/gen_gbk_table.py      # 产出 main/printer/gbk_table.cc（Task 1.2 会正式实现该脚本）
```
  若此时脚本尚未实现，则**先创建一个最小的 `gbk_table.cc` 占位**（空的 `const uint16_t kGbkForward[...] = {};` + 空的 `kGbkReverse[...]`），
  让 Task 0.4 的宿主能编译；Task 1.2 再把它替换为真实生成物。
  **并加注释说明"这是占位，Task 1.2 会覆盖"**，避免后人以为已生成完毕。

- [ ] **Step 2** 改 `test_apps/main/CMakeLists.txt`：新增 `LOTTERY_DIR` / `PRINTER_DIR`，把**全部新模块**加入 SRCS。
  ⚠️ **插入位置**：必须放在基线已有的 `set(MAIN_SOURCES ...)` **之后**、`idf_component_register(SRCS ...)` **之前**——`list(APPEND MAIN_SOURCES ...)` 在 `set()` 之前执行会被后者整体覆盖。

```cmake
set(LOTTERY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main/lottery")
set(PRINTER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main/printer")
# 每新增一个可单测模块，必须同步加进这里（否则测试链接失败）
list(APPEND MAIN_SOURCES
    "${LOTTERY_DIR}/prize_pool.cc"
    "${LOTTERY_DIR}/slot_machine_effect.cc"
    "${LOTTERY_DIR}/draw_record.cc"
    "${PRINTER_DIR}/gbk_encoder.cc"
    "${PRINTER_DIR}/gbk_table.cc"
    "${PRINTER_DIR}/receipt_layout.cc"
    "${PRINTER_DIR}/printer_logo.cc"
)
```
- [ ] **Step 3** `test_apps/sdkconfig.defaults` 去掉 `CONFIG_BOARD_TYPE_ESP32S3_DEVKIT` 依赖（或提供同名 Kconfig）。
- [ ] **Step 4** 先写一个冒烟测试并跑通宿主：

```cpp
TEST(Smoke, HostBuilds) { EXPECT_EQ(1 + 1, 2); }
```
```bash
python3 test_apps/run_tests.py      # Expected: 1 Tests 0 Failures
```
- [ ] **Step 5** **测试文件数 = 8**（以此清单为准，文件表与 DoD 已同步）：`test_prize_pool` / `test_slot_machine_effect` / `test_gbk_encoder` / `test_receipt_layout` / `test_printer_logo` / `test_qr_code` / `test_draw_record` / `test_lottery_controller`。

---

## Phase 1：打印机链路（可独立验证，先于抽奖逻辑）

### Task 1.1：搬运 dw_printer 并新增二维码

**Files:** `components/dw_printer/{include/raw_printer.h,raw_printer.c,lucky_printer_uart.c,raw_printer_qr.{h,c},CMakeLists.txt}`

- [ ] **Step 1** 复制 `tft_displaye/components/dw_printer/include/raw_printer.h` 与 `raw_printer.c`（**原样**，见设计 D6）。
- [ ] **Step 2** 复制 `dw_printer_uart.c` → `lucky_printer_uart.c`，裁剪掉 flow-control 调试与控制台命令，保留 `write/read/delay_ms` 回调，**保留超时语义**（`uart_tx_chars()` 轮询而非阻塞 `uart_write_bytes`）。
- [ ] **Step 3** 写 `raw_printer_qr.h`：

```c
#pragma once
#include "raw_printer.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief 生成并暂存二维码指令（GS ( k 序列），不发送。 */
printer_status_t raw_printer_qr_stage(raw_printer_t *printer, const char *text,
                                      uint8_t module_size, uint8_t ecc);
/** @brief 发送"打印二维码"指令。 */
printer_status_t raw_printer_qr_print(raw_printer_t *printer, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
```

- [ ] **Step 4** 写 `test_qr_code.cc`（**先写测试**，按设计 §10.2）：断言 `raw_printer_qr_stage("2026", 6, 49)` 产出的字节流精确等于：

```
1D 28 6B 04 00 31 41 32 00      // 设模型: pL=04(声明4字节), cn=31, fn=41, n1=32('2'), n2=00
1D 28 6B 03 00 31 43 06         // 设模块尺寸 = 6
1D 28 6B 03 00 31 45 31         // 设纠错 = 49('1', M 级 15%)
1D 28 6B 07 00 31 50 30 32 30 32 36   // 存数据: pL=0x07=len(4)+3, pH=00, cn=31, fn=50, m=30, data="2026"
1D 28 6B 03 00 31 51 30         // 打印
```

**必须加上这两条边界断言**（照抄下方，不要省）：

```cpp
// 1) 设模型必须是 9 字节，最后一个是 n2=0x00。
//    漏掉 n2 会让打印机吞掉下一条指令的首字节，导致后续全部错位。
TEST(QrCode, ModelCommandIsNineBytesWithN2) {
    auto bytes = raw_printer_qr_stage("2026", 6, 49);
    ASSERT_GE(bytes.size(), 9u);
    EXPECT_EQ(bytes[0], 0x1D); EXPECT_EQ(bytes[1], 0x28); EXPECT_EQ(bytes[2], 0x6B);
    EXPECT_EQ(bytes[3], 0x04); EXPECT_EQ(bytes[4], 0x00);   // pL=4, pH=0
    EXPECT_EQ(bytes[5], 0x31); EXPECT_EQ(bytes[6], 0x41);   // cn=49, fn=65
    EXPECT_EQ(bytes[7], 0x32); EXPECT_EQ(bytes[8], 0x00);   // n1=50, n2=0  ← 关键
    // ❌ 删掉自证式断言 EXPECT_EQ(3u+2u+bytes[3], 9u) —— 上一行已断言 bytes[3]==0x04，
    //    该式恒为真、永不失败，测不出"漏 n2"（M-4）。
    // ✅ 用「下一条指令的起点」锁边界：漏字节必然在此处错位
    EXPECT_EQ(bytes[9],  0x1D);   // 若漏了 n2，这里会是 0x00 → 立刻红灯
    EXPECT_EQ(bytes[10], 0x28);
    EXPECT_EQ(bytes[11], 0x6B);
    EXPECT_EQ(bytes[12], 0x03);   // 尺寸指令的 pL
    EXPECT_EQ(bytes[15], 0x43);   // fn = 67 设模块尺寸
}
// 2) 存数据的 pL 必须等于数据长度 + 3
TEST(QrCode, StoreDataLengthFieldIsLenPlusThree) {
    auto bytes = raw_printer_qr_stage("20260929-0007", 6, 49);  // data len = 13
    // 定位存数据指令: 1D 28 6B <pL> <pH> 31 50 30
    size_t off = 9 + 3 + 3;                 // 跳过前三条（9+3+3 = 15）
    EXPECT_EQ(bytes[off], 0x1D);
    EXPECT_EQ(bytes[off + 3], 13 + 3);      // pL = 16 = 0x10
    EXPECT_EQ(bytes[off + 4], 0x00);        // pH
    EXPECT_EQ(bytes[off + 5], 0x31);        // cn
    EXPECT_EQ(bytes[off + 6], 0x50);        // fn
    EXPECT_EQ(bytes[off + 7], 0x30);        // m
    // 总长度 = 9(模型) + 3(尺寸) + 3(纠错) + (3+2+3+13)(存数据) + 3(打印) = 39
    EXPECT_EQ(bytes.size(), 39u);
}
```

- [ ] **Step 5** 运行测试 → **必须失败**（Red）。
- [ ] **Step 6** 实现 `raw_printer_qr.c`（按设计 §6.3 的 `GS ( k` 字节表）。
- [ ] **Step 7** 运行测试 → 通过（Green）。
- [ ] **Step 8** 编译验证 `scripts/build.sh s3`。

---

### Task 1.2：GBK 表生成脚本 + 编码器（TDD）

**Files:** `scripts/gen_gbk_table.py`, `main/printer/gbk_table.cc`(生成), `main/printer/gbk_encoder.{h,cc}`, `test_apps/main/test_gbk_encoder.cc`

- [ ] **Step 1** 写 `scripts/gen_gbk_table.py`：

```python
# 生成 main/printer/gbk_table.cc
# 正查：index = (hi-0x81)*191 + (lo-0x40)，条目 uint16_t unicode_cp（0 = 空）
# ⚠️ stride 必须是 191（lo 区间 0x40..0xFE 含端点共 191 个位置）。
#    写成 190 会让 hi=0xFE,lo=0xFE 算出索引 23940 == 表长 → 越界读（M-1/n5）。
# 反查：按 cp 升序的 (cp, gbk) 数组，仅收录 GB2312 常用字集
import codecs, sys

ROWS   = 0xFE - 0x81 + 1     # 126 行
STRIDE = 0xFE - 0x40 + 1     # 191 列（0x40..0xFE 含端点；0x7F 保留为空槽）
assert STRIDE == 191, STRIDE
forward = [0] * (ROWS * COLS)
for hi in range(0x81, 0xFF):
    for lo in range(0x40, 0xFF):
        if lo == 0x7F:
            continue
        try:
            cp = ord(bytes([hi, lo]).decode('gbk'))
        except Exception:
            continue
        idx = (hi - 0x81) * STRIDE + (lo - 0x40)
        if cp > 0xFFFF:
            continue
        forward[idx] = cp
# 反查子集：GB2312 一级(0xB0A1-0xD7F9) + 二级(0xD8A1-0xF7FE) + 全角标点(0xA1A1-0xA9FE)
reverse = []
for hi in list(range(0xB0, 0xD8)) + list(range(0xD8, 0xF8)) + list(range(0xA1, 0xAA)):
    for lo in range(0xA1, 0xFF):
        try:
            ch = bytes([hi, lo]).decode('gbk')
        except Exception:
            continue
        if len(ch) == 1 and ord(ch) <= 0xFFFF:
            reverse.append((ord(ch), (hi << 8) | lo))
reverse.sort()
# ... 输出 C++ 数组字面量
```

- [ ] **Step 2** 运行脚本生成 `main/printer/gbk_table.cc`。**自检必须在脚本内用 `assert` 完成**（不要依赖人肉比对输出，否则实现者会以为是自己写错而反复调参）：

```python
# gen_gbk_table.py 末尾
assert len(forward) == 24066, len(forward)          # 126 * 191
assert len(reverse) == 7478,  len(reverse)          # GB2312 子集
total = len(forward) * 2 + len(reverse) * 4
assert total == 78044, total
print(f"forward_slots={len(forward)} reverse_entries={len(reverse)} total_bytes={total}")
```

```bash
python3 scripts/gen_gbk_table.py && ls -l main/printer/gbk_table.cc
```
Expected: `forward_slots=24066 reverse_entries=7478 total_bytes=78044`（≈ 76.2 KB flash，见设计 D5）

- [ ] **Step 3** 写 `gbk_encoder.h`：

```cpp
#ifndef _GBK_ENCODER_H_
#define _GBK_ENCODER_H_

#include <cstddef>
#include <cstdint>
#include <string>

/// UTF-8 → GBK(CP936) 编码器。
/// 未收录字符输出 '?' 并计入 replaced 计数（永不静默乱码）。
class GbkEncoder {
public:
    struct Result {
        std::string bytes;      // GBK 字节流
        size_t      replaced = 0;  // 被替换为 '?' 的字符数
    };
    static Result Encode(const std::string& utf8);
    /// 单码点查询，未收录返回 0。
    static uint16_t LookupCp(uint32_t codepoint);
};

#endif  // _GBK_ENCODER_H_
```

- [ ] **Step 4** 写 `test_gbk_encoder.cc`（**先写测试**）：

```cpp
TEST(GbkEncoder, ChinesePrize) {
    auto r = GbkEncoder::Encode("一等奖");
    EXPECT_EQ(Hex(r.bytes), "D2BBB5C8BDB1");   // 逐字节，末字节 B1 不是 31
    EXPECT_EQ(r.replaced, 0u);
}
TEST(GbkEncoder, AsciiPassThrough) {
    EXPECT_EQ(GbkEncoder::Encode("A1-").bytes, "A1-");
}
TEST(GbkEncoder, UnmappedBecomesQuestion) {
    auto r = GbkEncoder::Encode("\xF0\x9F\x98\x80");  // emoji，GBK 无
    EXPECT_EQ(r.bytes, "?");
    EXPECT_EQ(r.replaced, 1u);
}
TEST(GbkEncoder, InvalidUtf8DoesNotCrash) {
    auto r = GbkEncoder::Encode("\xFF\xFE");
    EXPECT_EQ(r.replaced >= 1u, true);
}
```

- [ ] **Step 5** 运行 → Red。**Step 6** 实现 `gbk_encoder.cc`（UTF-8 解码 + 正查/反查表访问）。**Step 7** 运行 → Green。

---

### Task 1.3：小票模板渲染 + 打印机服务（TDD）

> 依据：设计 §6.1/§7.3、决策 D17、需求 R6.3。文案**必须可编辑**（占位符模板），排版按**显示宽度**（中文 2 列）。

**Files:** `main/printer/receipt_layout.{h,cc}`, `main/printer/printer_service.{h,cc}`, `test_apps/main/test_receipt_layout.cc`

- [ ] **Step 1** 写 `receipt_layout.h`（纯逻辑，不碰硬件）：

```cpp
/// 小票可编辑模板（存 NVS，见设计 §7.3）
struct ReceiptTemplate {
    std::string shop_name    = "幸运翻页机";
    std::string win_title    = "恭喜中奖";
    std::string lose_title   = "感谢参与";
    std::string header       = "{shop}{nl}══════════════════{nl}";
    std::string footer       = "────────────────────────{nl}"
                               "请凭此票到服务台领奖{nl}"
                               "有效期至：{validity}{nl}";
    int         validity_days = 7;      // 0 = 不显示 {validity}
    int         paper_cols    = 32;     // 58mm=32 / 80mm=48
};

/// 本次开奖的数据（占位符的取值来源）
struct ReceiptData {
    std::string ticket;      // "20260929-0007"
    std::string time_str;    // "2026-09-29 14:32:07"
    std::string prize_name;  // "一等奖"
    std::string prize_note;  // "奖品：扫地机器人"
    std::string validity;    // "2026-10-06"
    std::string check;       // "3F2A"
    int         page = 0;    // 0-based；渲染 {page} 时 +1
    bool        is_lose = false;   // → 用 lose_title，且不打二维码
    bool        qr_enabled = true;
    bool        logo_enabled = false;
};

class ReceiptLayout {
public:
    /// 每行显示宽度（ASCII=1 列，GBK 双字节字符=2 列）
    static int DisplayWidth(const std::string& utf8);
    /// 占位符替换（在转 GBK 之前做，UTF-8 组装）
    static std::string Substitute(const std::string& tmpl, const ReceiptTemplate& t,
                                  const ReceiptData& d, int* unknown_placeholders);
    /// 渲染成 ESC/POS 字节流（内部先 Substitute 再 GbkEncoder::Encode）
    static std::string Render(const ReceiptTemplate& t, const ReceiptData& d);
    /// 仅供预览：渲染成人类可读纯文本（不转 GBK）
    static std::string RenderPreviewText(const ReceiptTemplate& t, const ReceiptData& d);
};
```

- [ ] **Step 2** 写 `test_receipt_layout.cc`（**先写测试**）：

```cpp
// 辅助
static ReceiptData MakeWin() {
    ReceiptData d;
    d.ticket = "20260929-0007"; d.time_str = "2026-09-29 14:32:07";
    d.prize_name = "一等奖";    d.prize_note = "奖品：扫地机器人";
    d.validity = "2026-10-06";  d.check = "3F2A";
    d.page = 0; d.is_lose = false; d.qr_enabled = true;
    return d;
}
static std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

TEST(Receipt, EveryLineWithinPaperCols) {
    ReceiptData d = MakeWin();
    for (int cols : {32, 48}) {
        ReceiptTemplate t; t.paper_cols = cols;
        auto text = ReceiptLayout::RenderPreviewText(t, d);
        for (auto& line : SplitLines(text)) {
            EXPECT_LE(ReceiptLayout::DisplayWidth(line), (size_t)cols)
                << "cols=" << cols << " 超宽行: [" << line << "]";
        }
    }
}
TEST(Receipt, CenteringUsesDisplayWidthNotBytes) {
    // "一等奖" 显示宽 6 列（3 个汉字 × 2），字节数却是 9
    EXPECT_EQ(ReceiptLayout::DisplayWidth("一等奖"), 6u);
    EXPECT_EQ(ReceiptLayout::DisplayWidth("ABC"), 3u);
    EXPECT_EQ(ReceiptLayout::DisplayWidth("A中"), 3u);

    ReceiptTemplate t; t.paper_cols = 32;
    ReceiptData d = MakeWin(); d.prize_name = "一等奖";
    auto lines = SplitLines(ReceiptLayout::RenderPreviewText(t, d));
    // 找到含"一等奖"的那一行，检查左右填充差 <= 1（按显示宽度居中）
    for (auto& l : lines) {
        if (l.find("一等奖") == std::string::npos) continue;
        size_t lead = l.find_first_not_of(' ');
        size_t trail = l.size() - 1 - l.find_last_not_of(' ');
        int diff = (int)lead - (int)trail;
        EXPECT_LE(diff < 0 ? -diff : diff, 1) << "居中偏了: [" << l << "]";
        break;
    }
}
TEST(Receipt, LoseTicketHasNoQrCode) {
    ReceiptTemplate t; ReceiptData d = MakeWin();
    d.is_lose = true; d.prize_name = "谢谢参与";
    std::string bytes = ReceiptLayout::Render(t, d);
    EXPECT_EQ(bytes.find("\x1D\x28\x6B"), std::string::npos) << "未中奖不应含 GS ( k";
}
TEST(Receipt, WinTicketHasQrCode) {
    ReceiptTemplate t; ReceiptData d = MakeWin(); d.qr_enabled = true;
    std::string bytes = ReceiptLayout::Render(t, d);
    EXPECT_NE(bytes.find("\x1D\x28\x6B"), std::string::npos) << "中奖应含 GS ( k";
}
TEST(Receipt, SubstitutesAllPlaceholders) {
    ReceiptTemplate t;
    t.shop_name = "测试店"; t.win_title = "中奖啦";
    t.header = "{shop}|{title}|{nl}"; t.footer = "{prize}/{note}/{ticket}/{time}/{page}/{validity}/{check}{nl}";
    ReceiptData d = MakeWin();
    int unknown = 0;
    std::string out = ReceiptLayout::Substitute(t.header + t.footer, t, d, &unknown);
    EXPECT_EQ(unknown, 0);
    EXPECT_NE(out.find("测试店"), std::string::npos);
    EXPECT_NE(out.find("中奖啦"), std::string::npos);
    EXPECT_NE(out.find("一等奖"), std::string::npos);
    EXPECT_NE(out.find("扫地机器人"), std::string::npos);
    EXPECT_NE(out.find("20260929-0007"), std::string::npos);
    EXPECT_NE(out.find("2026-09-29 14:32:07"), std::string::npos);
    EXPECT_NE(out.find("2026-10-06"), std::string::npos);
    EXPECT_NE(out.find("3F2A"), std::string::npos);
    EXPECT_EQ(out.find("{"), std::string::npos) << "还有未替换的占位符: " << out;
}
TEST(Receipt, PageIsOneBasedInOutput) {
    ReceiptTemplate t; t.footer = "P{page}"; ReceiptData d = MakeWin(); d.page = 0;
    int u = 0;
    EXPECT_EQ(ReceiptLayout::Substitute(t.footer, t, d, &u), "P1");   // 0-based → 显示 P1
    d.page = 29;
    EXPECT_EQ(ReceiptLayout::Substitute(t.footer, t, d, &u), "P30");
}
TEST(Receipt, UnknownPlaceholderPreservedAndCounted) {
    ReceiptTemplate t; t.footer = "X{bogus}Y"; ReceiptData d = MakeWin();
    int unknown = 0;
    std::string out = ReceiptLayout::Substitute(t.footer, t, d, &unknown);
    EXPECT_EQ(unknown, 1);
    EXPECT_EQ(out, "X{bogus}Y");          // 原样保留，不静默丢弃
}
TEST(Receipt, LongLineWrapsSafelyWithoutSplittingMultibyte) {
    ReceiptTemplate t; t.paper_cols = 32;
    ReceiptData d = MakeWin();
    d.prize_note = std::string(40, 'x');   // 用中文更严格：40 个汉字 = 80 列
    for (int i = 0; i < 20; ++i) d.prize_note += "奖品";
    auto lines = SplitLines(ReceiptLayout::RenderPreviewText(t, d));
    for (auto& l : lines)
        EXPECT_LE(ReceiptLayout::DisplayWidth(l), 32u) << "折行后仍超宽: [" << l << "]";
    // 折行不得产生半个多字节字符：渲染成 GBK 后不应出现单独的 0x3F(?) 之外的空洞
    // （最直接的判据：显示宽度为偶数分段，且不存在非法 GBK 尾字节）
    std::string gbk = GbkEncoder::Encode(ReceiptLayout::RenderPreviewText(t, d)).bytes;
    for (size_t i = 0; i < gbk.size(); ++i) {
        unsigned char c = (unsigned char)gbk[i];
        if (c >= 0x81 && c <= 0xFE) {                 // GBK 首字节
            ASSERT_LT(i + 1, gbk.size()) << "GBK 首字节后缺尾字节（折行切断了多字节字符）";
            unsigned char t2 = (unsigned char)gbk[++i];
            EXPECT_TRUE((t2 >= 0x40 && t2 <= 0xFE && t2 != 0x7F)) << "非法 GBK 尾字节";
        }
    }
}
TEST(Receipt, ValidityHiddenWhenDaysZero) {
    ReceiptTemplate t; t.validity_days = 0;
    t.footer = "A{nl}有效期至：{validity}{nl}B";
    ReceiptData d = MakeWin();
    int u = 0;
    std::string out = ReceiptLayout::Substitute(t.footer, t, d, &u);
    EXPECT_EQ(out.find("有效期"), std::string::npos) << "validity_days=0 时不应出现有效期行";
}
TEST(Receipt, CustomTemplateIsHonored) {
    ReceiptTemplate t;
    t.shop_name = "老王小卖部";
    t.header = "{shop}{nl}欢迎光临{nl}";
    ReceiptData d = MakeWin();
    auto text = ReceiptLayout::RenderPreviewText(t, d);
    EXPECT_NE(text.find("老王小卖部"), std::string::npos);
    EXPECT_NE(text.find("欢迎光临"), std::string::npos);
}
```

- [ ] **Step 3** 运行 → **Red**。**Step 4** 实现 `receipt_layout.cc`（先 `Substitute` 组装 UTF-8 → 按 `DisplayWidth` 排版/折行 → 最后整体 `GbkEncoder::Encode`）。
- [ ] **Step 5** 运行 → **Green**。
- [ ] **Step 6** 写 `printer_service.h`：

```cpp
class PrinterService {
public:
    static constexpr int kQueueDepth = 1;   // 深度 1：单飞（设计 D4）

    esp_err_t Init(int tx_pin, int rx_pin, int baud);
    /// 非阻塞入队。队列满返回 ESP_ERR_TIMEOUT。
    esp_err_t Enqueue(ReceiptTemplate tmpl, ReceiptData data);
    /// 打一张测试票（走同一条渲染路径，便于现场核对中文/logo/二维码）
    esp_err_t EnqueueTestTicket(const ReceiptTemplate& tmpl);
    /// 注册"打印完成"回调（在 Application 主线程上下文调用）。
    void SetDoneCallback(std::function<void(bool ok)> cb);
    void GetStats(int* ok, int* fail) const;
    bool IsReady() const;

private:
    static void TaskEntry(void* arg);   // 优先级 5，栈 4096，独立阻塞发送
    // raw_printer_t ctx_; uint8_t buf_[512]; QueueHandle_t queue_; ...   // 512B，对齐 tft_displaye/main/printer_app.c:31
};
```

- [ ] **Step 7** 编译验证。**Step 8** 真机验证（阻塞项：需接打印机）：`ptest` 命令打测试票，核对中文不乱码、二维码可扫。

> **不做**：不接打印机 RX 的缺纸/过热状态轮询（设计 §11-R8，v1 明确不做）。

---

### Task 1.4：商家 Logo 上传与打印（TDD）

> 依据：设计 §6.4、决策 D14/D15、需求"可上传商家 logo"。
> **核心决策**：图像转换在**浏览器侧**完成（canvas 缩放 + 灰度 + 二值化/抖动），固件只收 `LGO1` 头 + 1-bit 栅格——**固件零解码库**。

**Files:** `main/printer/printer_logo.{h,cc}`, `main/printer/logo_store.{h,cc}`, `test_apps/main/test_printer_logo.cc`

- [ ] **Step 1** 写 `printer_logo.h`（纯逻辑，不碰硬件与文件系统）：

```cpp
/// Logo 位图头部（8 字节）+ 位图数据
struct LogoHeader {
    char     magic[4];     // "LGO1"
    uint16_t width_dots;   // 必须是 8 的倍数
    uint16_t height_dots;  // <= kMaxLogoHeight
};
static constexpr int kMaxLogoHeight  = 256;
static constexpr int kMaxLogoWidthPx = 576;               // 80mm 纸满宽
static constexpr int kMaxLogoBytes   = 8 + (576 / 8) * 256;  // 8 + 18,432

struct LogoInfo {
    bool     valid = false;
    uint16_t width_dots = 0;
    uint16_t height_dots = 0;
    int      data_bytes = 0;
    const char* error = nullptr;   // 中文错误原因
};

class PrinterLogo {
public:
    /// 校验 8 字节头 + 数据长度是否精确匹配（防截断/溢出）
    static LogoInfo Validate(const uint8_t* buf, size_t len);
    /// 生成 GS v 0 栅格指令，按每包 max_rows 行分包（默认 24 行）
    /// 返回多个独立指令（每条自带 xL/xH/yL/yH）
    static std::vector<std::string> BuildRasterCommands(
        const LogoInfo& info, const uint8_t* data, int max_rows = 24);
    /// 居中前缀（ESC a 1）与恢复（ESC a 0）
    static std::string CenterOn();   // "1B 61 01"
    static std::string CenterOff();  // "1B 61 00"
};
```

- [ ] **Step 2** 写 `test_printer_logo.cc`（**先写测试**）：

```cpp
// ⚠️ 以下均为**含真实断言**的用例（M-3：原稿只写注释，Unity 下恒通过，Red 阶段不会有红灯）
static void WriteLe16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static std::vector<uint8_t> MakeLogo(uint16_t w, uint16_t h) {
    std::vector<uint8_t> b(8 + (size_t)(w / 8) * h, 0);
    memcpy(b.data(), "LGO1", 4); WriteLe16(b.data() + 4, w); WriteLe16(b.data() + 6, h);
    return b;
}

TEST(Logo, RejectsBadMagic) {
    uint8_t buf[8 + 48] = {}; memcpy(buf, "LGO2", 4);
    WriteLe16(buf + 4, 384); WriteLe16(buf + 6, 1);
    auto info = PrinterLogo::Validate(buf, sizeof(buf));
    EXPECT_FALSE(info.valid);
    EXPECT_NE(info.error, nullptr);
}
TEST(Logo, RejectsNonMultipleOf8) {
    uint8_t buf[8 + 48] = {}; memcpy(buf, "LGO1", 4);
    WriteLe16(buf + 4, 380); WriteLe16(buf + 6, 1);          // 380 不是 8 的倍数
    EXPECT_FALSE(PrinterLogo::Validate(buf, sizeof(buf)).valid);
}
TEST(Logo, RejectsTooWide) {
    uint8_t buf[8 + 48] = {}; memcpy(buf, "LGO1", 4);
    WriteLe16(buf + 4, 584); WriteLe16(buf + 6, 1);          // > 576
    EXPECT_FALSE(PrinterLogo::Validate(buf, sizeof(buf)).valid);
}
TEST(Logo, RejectsTooTall) {
    uint8_t buf[8 + 48] = {}; memcpy(buf, "LGO1", 4);
    WriteLe16(buf + 4, 384); WriteLe16(buf + 6, 257);        // > 256
    EXPECT_FALSE(PrinterLogo::Validate(buf, sizeof(buf)).valid);
}
TEST(Logo, RejectsTruncatedData) {
    uint8_t buf[8 + 100] = {}; memcpy(buf, "LGO1", 4);
    WriteLe16(buf + 4, 384); WriteLe16(buf + 6, 96);         // 声明需要 8+4608 字节
    auto info = PrinterLogo::Validate(buf, sizeof(buf));      // 只给 108 字节
    EXPECT_FALSE(info.valid);
    EXPECT_NE(info.error, nullptr);
}
TEST(Logo, RejectsExcessData) {
    std::vector<uint8_t> buf(8 + 48 * 2 + 1, 0); memcpy(buf.data(), "LGO1", 4);
    WriteLe16(buf.data() + 4, 384); WriteLe16(buf.data() + 6, 2);
    EXPECT_FALSE(PrinterLogo::Validate(buf.data(), buf.size()).valid);   // 多 1 字节
}
TEST(Logo, AcceptsValid384x96) {
    std::vector<uint8_t> buf(8 + 48 * 96, 0); memcpy(buf.data(), "LGO1", 4);
    WriteLe16(buf.data() + 4, 384); WriteLe16(buf.data() + 6, 96);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.width_dots, 384);
    EXPECT_EQ(info.height_dots, 96);
}
TEST(Logo, BitOrderIsMsbFirst) {
    // M-9：位序反了图案会镜像/花屏，功能测试很难定位 → 必须单测锁死
    std::vector<uint8_t> buf(8 + 48, 0); memcpy(buf.data(), "LGO1", 4);
    WriteLe16(buf.data() + 4, 384); WriteLe16(buf.data() + 6, 1);
    buf[8] = 0x80;                                   // 最左像素点亮
    auto cmds = PrinterLogo::BuildRasterCommands(
        PrinterLogo::Validate(buf.data(), buf.size()), buf.data() + 8, 24);
    ASSERT_EQ(cmds.size(), 1u);
    // cmds[0] 是**完整 GS v 0 指令** = 7 字节头(1D 76 30 00 xL xH yL yH) + 数据。
    // 因此第一个**数据**字节的下标是 7，不是 .back()（.back() 是 data[47] == 0x00，
    // 断言它 == 0x80 必然失败 —— 本行此前写错）。
    EXPECT_EQ((uint8_t)cmds[0][7], 0x80);            // 首数据字节必须是 0x80（MSB 优先）
}
TEST(Logo, ReadsLe16NotHostOrder) {
    // M-8：禁止 reinterpret_cast<LogoHeader*> —— 必须逐字节解析（buf[4] | buf[5]<<8）
    uint8_t buf[8 + 48] = {}; memcpy(buf, "LGO1", 4);
    buf[4] = 0x80; buf[5] = 0x01;                    // LE → 384
    WriteLe16(buf + 6, 1);
    EXPECT_EQ(PrinterLogo::Validate(buf, sizeof(buf)).width_dots, 384);
}

TEST(Logo, RasterHeaderBytes) {
    // 384 dots 宽 = 48 B/行；96 行、每包 24 行 → 恰好 4 包
    auto buf = MakeLogo(384, 96);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    auto cmds = PrinterLogo::BuildRasterCommands(info, buf.data() + 8, /*max_rows=*/24);
    ASSERT_EQ(cmds.size(), 4u);
    const std::string& c = cmds[0];
    // GS v 0 m xL xH yL yH  →  1D 76 30 00 30 00 18 00
    EXPECT_EQ((uint8_t)c[0], 0x1D); EXPECT_EQ((uint8_t)c[1], 0x76);
    EXPECT_EQ((uint8_t)c[2], 0x30); EXPECT_EQ((uint8_t)c[3], 0x00);   // m = 0
    EXPECT_EQ((uint8_t)c[4], 0x30); EXPECT_EQ((uint8_t)c[5], 0x00);   // xL,xH = 48
    EXPECT_EQ((uint8_t)c[6], 0x18); EXPECT_EQ((uint8_t)c[7], 0x00);   // yL,yH = 24
    EXPECT_EQ(c.size(), (size_t)(8 + 24 * 48));                       // 头 8B + 数据
}
TEST(Logo, SplitsIntoMultipleCommandsWhenTall) {
    // 96 行 / 每包最多 24 行 → 4 条指令（硬性防回归）。
    // ⚠️ 分包的真实理由**不是**缓冲溢出（M-2）：raw_printer.c 的 append() 是 write-through
    //    （raw_printer.c:82-113），数据过大时 send_direct、装不下时先 flush，
    //    调用方永远不会撑爆它，且 PRINTER_ERR_NO_SPACE 在 raw_printer.c 中从未被使用。
    //    真实理由是**超时**：raw_printer.c 所有 append 硬编码 timeout_ms=1000
    //    （raw_printer.c:146/201/.../313），链路层尊重该超时。实测
    //    12,288 B @115200 8N1 ≈ 1,067 ms > 1,000 ms → 必然超时失败。
    //    分包成 1,152 B ≈ 100 ms，留 ~10 倍余量。
    auto buf = MakeLogo(384, 96);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    auto cmds = PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24);
    EXPECT_EQ(cmds.size(), 4u);
    // 单包数据量必须 ≤ 1152 B（≈100 ms），给 1000 ms 超时留足余量
    for (auto& c : cmds) EXPECT_LE((int)c.size() - 8, 1152);
    // 若有人改成"一条指令装满 12 KB" → 下面这条会失败
    for (auto& c : cmds) EXPECT_LT(c.size(), (size_t)10000);
}
TEST(Logo, LastPacketCarriesRemainder) {
    // 100 行 / 每包 24 行 → 3 包满(24) + 1 包 4 行
    auto buf = MakeLogo(384, 100);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    auto cmds = PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24);
    ASSERT_EQ(cmds.size(), 4u);
    EXPECT_EQ((uint8_t)cmds[0][6], 24);      // 第 1 包 yL=24
    EXPECT_EQ((uint8_t)cmds[1][6], 24);
    EXPECT_EQ((uint8_t)cmds[2][6], 24);
    EXPECT_EQ((uint8_t)cmds[3][6], 4);       // 最后 4 行
    EXPECT_EQ((uint8_t)cmds[3][7], 0x00);    // yH
    EXPECT_EQ(cmds[3].size(), (size_t)(8 + 4 * 48));
}
TEST(Logo, TotalPayloadEqualsBitmapSize) {
    // 各包数据字节数之和 == width/8*height（不重不漏）
    const int w = 384, h = 100;
    auto buf = MakeLogo(w, h);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    auto cmds = PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24);
    size_t sum = 0;
    for (auto& c : cmds) sum += c.size() - 8;
    EXPECT_EQ(sum, (size_t)((w / 8) * h));
}
TEST(Logo, EachPacketIsIndependentCommand) {
    // 每条指令都必须以 1D 76 30 00 自带头（不能依赖上一条的包头）
    auto buf = MakeLogo(384, 96);
    auto info = PrinterLogo::Validate(buf.data(), buf.size());
    ASSERT_TRUE(info.valid);
    for (auto& c : PrinterLogo::BuildRasterCommands(info, buf.data() + 8, 24)) {
        ASSERT_GE(c.size(), 8u);
        EXPECT_EQ((uint8_t)c[0], 0x1D);
        EXPECT_EQ((uint8_t)c[1], 0x76);
        EXPECT_EQ((uint8_t)c[2], 0x30);
        EXPECT_EQ((uint8_t)c[3], 0x00);
    }
}
```

- [ ] **Step 3** 运行 → **Red**。
- [ ] **Step 4** 实现 `printer_logo.cc`（`GS v 0 m xL xH yL yH` + 分包）。
- [ ] **Step 5** 运行 → **Green**。
- [ ] **Step 6** 写 `logo_store.h`（文件系统，走临时文件 + rename，决策 D15）：

```cpp
class LogoStore {
public:
    static constexpr const char* kPath    = "/spiffs/logo.bin";
    static constexpr const char* kTmpPath = "/spiffs/logo.bin.tmp";

    /// 原子写入：写 tmp → fsync → rename → 删旧。任一步失败都不破坏已有 logo。
    static esp_err_t Save(const uint8_t* buf, size_t len);
    /// 读取（内部按 kMaxLogoBytes 定长缓冲，避免动态分配）
    static esp_err_t Load(uint8_t* out, size_t out_cap, size_t* out_len);
    static esp_err_t Remove();
    static bool Exists();
    /// 仅读头部 8 字节判断元信息（不加载整张位图）
    static LogoInfo ReadInfo();
};
```

- [ ] **Step 7** 单测（可用 SPIFFS 或内存桩）：`Save` 成功后可 `ReadInfo`；**Save 中途失败时旧 logo 完好**（用桩注入 `fwrite` 失败验证）；`Remove` 后 `Exists()==false`。
- [ ] **Step 8** 集成到 `PrinterService`：渲染小票时若 `logo_enabled && LogoStore::Exists()`，在票头前插入 `CenterOn()` + 各分包指令 + `CenterOff()`；**logo 校验失败则跳过 logo 继续打票**（决策 D15），`ESP_LOGW`。
- [ ] **Step 9** 编译 + 真机验证：上传一张 PNG logo → 打印 → 核对图形清晰、**不出现整票打不出**；再故意上传一个坏的 `logo.bin`（用 `curl` 直接发乱数据）→ 断言仍是 `400` 且**票照常能打**。

> **UART 时间预算**：384×96 的 logo ≈ 4.6 KB @115200 ≈ **0.4 s**；满幅 384×256 ≈ 12.3 KB ≈ **1.1 s**，占了整票主要耗时。因此 `logo.html` 要提示"建议高度 ≤ 96 dots"，并在 `GET /api/logo` 返回估算打印耗时。

---

## Phase 2：抽奖领域逻辑（TDD，纯逻辑先行）

### Task 2.1：PrizePool —— 分级奖池 + 加权抽取 + 库存

> 依据：设计 §5.1/§5.2、决策 D12/D13、需求 R5。**"数量"和"概率"必须分开建模**——只按概率抽会让"一等奖 1 个"发出多个。

**Files:** `main/lottery/prize.h`, `main/lottery/prize_pool.{h,cc}`, `test_apps/main/test_prize_pool.cc`

- [ ] **Step 1** 写 `prize.h`（设计 §5.1）：

```cpp
struct PrizeTier {
    int      level;        // 1..N，1 = 一等奖
    int      page;         // [0, total_pages)
    uint32_t weight;       // 相对权重，0 = 停用
    int      quantity;     // >=1，或 -1 = 不限量
    int      remaining;    // 运行时剩余；quantity==-1 时忽略
    char     name[32];
    char     note[48];
};
static constexpr int kMaxTiers = 16;

enum class DepletionPolicy { kFallbackToLose = 0, kRefuse = 1 };
```

- [ ] **Step 2** 写 `test_prize_pool.cc`（**先写测试**）。**必须覆盖下面全部用例**：

```cpp
// 辅助：构造一个 4 级奖池（权重 1:5:20:74；级别 4 不限量作兜底）
static std::vector<PrizeTier> Make4Tiers() {
    return {
        {1, 0,  1, 1,  1,  "一等奖",   "奖品：扫地机器人"},
        {2, 1,  5, 5,  5,  "二等奖",   "奖品：蓝牙音箱"},
        {3, 2, 20, 30, 30, "三等奖",   "奖品：定制水杯"},
        {4, 3, 74, -1, 0,  "谢谢参与", "欢迎下次再来"},
    };
}
static constexpr int kPages = 30;

// ---------- 权重分布 ----------
TEST(PrizePool, WeightedDistribution) {
    PrizePool pool(Make4Tiers(), kPages, DepletionPolicy::kFallbackToLose);
    ASSERT_EQ(pool.Validate().error, nullptr);
    const uint32_t total = 1 + 5 + 20 + 74;
    int hits[4] = {0, 0, 0, 0};
    for (uint32_t r = 0; r < 1000000; ++r) {
        auto out = pool.Decide(r % total);
        ASSERT_EQ(out.status, DrawStatus::kOk);
        hits[out.result.tier_index]++;
    }
    const double expect[4] = {1.0/100, 5.0/100, 20.0/100, 74.0/100};
    for (int i = 0; i < 4; ++i) {
        double got = hits[i] / 1000000.0;
        EXPECT_NEAR(got, expect[i], 0.005) << "tier " << i;   // 偏差 < 0.5%
    }
}
TEST(PrizePool, SingleTierAlwaysWins) {
    std::vector<PrizeTier> v = {{1, 7, 10, -1, 0, "唯一奖", ""}};
    PrizePool pool(v, kPages, DepletionPolicy::kFallbackToLose);
    for (uint32_t r : {0u, 1u, 9u, 100u, 12345u}) {
        auto out = pool.Decide(r);
        ASSERT_EQ(out.status, DrawStatus::kOk);
        EXPECT_EQ(out.result.tier_index, 0);
        EXPECT_EQ(out.result.page, 7);
    }
}

// ---------- 库存（核心） ----------
TEST(PrizePool, QuantityIsRespectedExactly) {
    // 级1 有限量 3；级2 不限量。级1 的 weight 极大以便"若未耗尽必被抽中"
    std::vector<PrizeTier> v = {
        {1, 0, 1000, 3, 3, "限量", ""},
        {2, 1, 1,   -1, 0, "兜底", ""},
    };
    PrizePool pool(v, kPages, DepletionPolicy::kFallbackToLose);
    int tier1 = 0;
    for (int i = 0; i < 10; ++i) {
        auto out = pool.DrawAndConsume(0);        // r=0 必落在权重最大的候选级
        ASSERT_EQ(out.status, DrawStatus::kOk);
        if (out.result.tier_index == 0) tier1++;
    }
    EXPECT_EQ(tier1, 3);                          // 恰好 3 次，不多不少
    // 第 4 次起候选集不含级1 → 必落级2
    auto after = pool.DrawAndConsume(0);
    ASSERT_EQ(after.status, DrawStatus::kOk);
    EXPECT_EQ(after.result.tier_index, 1);
}
TEST(PrizePool, DepletedTierRenormalizesOthers) {
    std::vector<PrizeTier> v = {
        {1, 0, 50, 1,  1, "A", ""},
        {2, 1, 50, -1, 0, "B", ""},
    };
    PrizePool pool(v, kPages, DepletionPolicy::kFallbackToLose);
    auto first = pool.DrawAndConsume(0);          // r=0 → 落 A（前缀和 [0,50) 是 A）
    ASSERT_EQ(first.status, DrawStatus::kOk);
    EXPECT_EQ(first.result.tier_index, 0);
    // A 已耗尽 → Σweight 只剩 50，r=0..49 必须 100% 落 B
    for (uint32_t r = 0; r < 50; ++r) {
        auto out = pool.Decide(r);
        ASSERT_EQ(out.status, DrawStatus::kOk);
        EXPECT_EQ(out.result.tier_index, 1) << "r=" << r << " 仍抽到已耗尽的 A";
        EXPECT_EQ(out.result.total_weight, 50u);  // 分母已重算
    }
}
TEST(PrizePool, UnlimitedTierNeverDepletes) {
    std::vector<PrizeTier> v = {{1, 0, 10, -1, 0, "不限量", ""}};
    PrizePool pool(v, kPages, DepletionPolicy::kFallbackToLose);
    for (int i = 0; i < 1000; ++i) {
        auto out = pool.DrawAndConsume(0);
        ASSERT_EQ(out.status, DrawStatus::kOk);
    }
    EXPECT_EQ(pool.tiers()[0].remaining, 0);      // quantity==-1 时 remaining 不被修改
    auto still = pool.Decide(0);
    EXPECT_EQ(still.status, DrawStatus::kOk);     // 仍可抽中
}
TEST(PrizePool, DecideDoesNotMutate) {
    PrizePool pool(Make4Tiers(), kPages, DepletionPolicy::kFallbackToLose);
    const int before = pool.tiers()[0].remaining;
    for (int i = 0; i < 100; ++i) (void)pool.Decide(0);
    EXPECT_EQ(pool.tiers()[0].remaining, before);  // 纯函数：不改库存
}

// ---------- 耗尽策略（D13） ----------
TEST(PrizePool, FallbackToLoseWhenAllDepleted) {
    std::vector<PrizeTier> v = {
        {1, 0, 100, 1,  1, "一等奖", ""},
        {2, 1, 1,   -1, 0, "谢谢参与", ""},   // 兜底
    };
    PrizePool pool(v, kPages, DepletionPolicy::kFallbackToLose);
    (void)pool.DrawAndConsume(0);               // 耗尽一等奖
    auto out = pool.DrawAndConsume(0);
    ASSERT_EQ(out.status, DrawStatus::kOk);
    EXPECT_EQ(out.result.tier_index, 1);
    EXPECT_TRUE(out.result.depleted_fallback);
}
TEST(PrizePool, RefuseWhenAllDepletedAndPolicyRefuse) {
    std::vector<PrizeTier> v = {
        {1, 0, 100, 1, 1, "一等奖", ""},
        {2, 1, 1,   -1, 0, "谢谢参与", ""},   // 有兜底级，但策略是 Refuse
    };
    PrizePool pool(v, kPages, DepletionPolicy::kRefuse);
    (void)pool.DrawAndConsume(0);               // 耗尽一等奖
    auto out = pool.DrawAndConsume(0);
    // 失败必须可表达（DrawOutcome.status），而不是靠非法 DrawResult 猜
    EXPECT_EQ(out.status, DrawStatus::kRefuse);
    EXPECT_NE(out.error, nullptr);
}

// ---------- 配置校验 ----------
static const char* ValidateErr(std::vector<PrizeTier> v, DepletionPolicy p) {
    PrizePool pool(std::move(v), kPages, p);
    return pool.Validate().error;
}
TEST(PrizePool, RejectsDuplicatePage) {
    const char* e = ValidateErr({{1,2,10,1,1,"A",""},{2,2,10,1,1,"B",""}},
                                DepletionPolicy::kFallbackToLose);
    ASSERT_NE(e, nullptr);
    EXPECT_NE(std::string(e).find("页码"), std::string::npos);
}
TEST(PrizePool, RejectsDuplicateLevel) {
    const char* e = ValidateErr({{1,0,10,1,1,"A",""},{1,1,10,1,1,"B",""}},
                                DepletionPolicy::kFallbackToLose);
    ASSERT_NE(e, nullptr);
    EXPECT_NE(std::string(e).find("等级"), std::string::npos);
}
TEST(PrizePool, RejectsOutOfRangePage) {
    // kPages = 30 → page 30 越界（合法范围 0..29）
    const char* e = ValidateErr({{1,30,10,1,1,"A",""},{2,1,10,-1,0,"B",""}},
                                DepletionPolicy::kFallbackToLose);
    ASSERT_NE(e, nullptr);
    EXPECT_NE(std::string(e).find("30"), std::string::npos);
}
TEST(PrizePool, RejectsZeroQuantity) {
    const char* e = ValidateErr({{1,0,10,0,0,"A",""}}, DepletionPolicy::kRefuse);
    ASSERT_NE(e, nullptr);
}
TEST(PrizePool, RejectsRemainingOverQuantity) {
    const char* e = ValidateErr({{1,0,10,3,40,"A",""}}, DepletionPolicy::kRefuse);
    ASSERT_NE(e, nullptr);
}
TEST(PrizePool, RejectsTooManyTiers) {
    std::vector<PrizeTier> v;
    for (int i = 0; i < kMaxTiers + 1; ++i)          // 17 级 > kMaxTiers(16)
        v.push_back({i + 1, i % kPages, 1, 1, 1, "x", ""});
    const char* e = ValidateErr(std::move(v), DepletionPolicy::kRefuse);
    ASSERT_NE(e, nullptr);
}
TEST(PrizePool, RejectsNoDrawableTier) {
    // 全部 weight=0 → 无级可抽
    const char* e = ValidateErr({{1,0,0,1,1,"A",""},{2,1,0,1,1,"B",""}},
                                DepletionPolicy::kRefuse);
    ASSERT_NE(e, nullptr);
}
TEST(PrizePool, RejectsMissingFallbackWhenPolicyFallback) {
    // policy 要求回退，但没有任何 quantity==-1 的兜底级
    const char* e = ValidateErr({{1,0,10,1,1,"A",""},{2,1,10,5,5,"B",""}},
                                DepletionPolicy::kFallbackToLose);
    ASSERT_NE(e, nullptr);
    EXPECT_NE(std::string(e).find("兜底"), std::string::npos);
}

// ---------- JSON 往返 ----------
TEST(PrizePool, JsonRoundTrip) {
    PrizePool src(Make4Tiers(), kPages, DepletionPolicy::kFallbackToLose);
    src.tiers_mutable()[2].remaining = 17;            // 改一个非默认值
    std::string json = src.ToJson();
    PrizePool dst = PrizePool::FromJson(json, &err);
    ASSERT_EQ(err, nullptr);
    ASSERT_EQ(dst.tiers().size(), src.tiers().size());
    for (size_t i = 0; i < src.tiers().size(); ++i) {
        EXPECT_EQ(dst.tiers()[i].level,     src.tiers()[i].level);
        EXPECT_EQ(dst.tiers()[i].page,      src.tiers()[i].page);
        EXPECT_EQ(dst.tiers()[i].weight,    src.tiers()[i].weight);
        EXPECT_EQ(dst.tiers()[i].quantity,  src.tiers()[i].quantity);
        EXPECT_EQ(dst.tiers()[i].remaining, src.tiers()[i].remaining) << "remaining 必须往返保留";
        EXPECT_STREQ(dst.tiers()[i].name,   src.tiers()[i].name);
    }
}
TEST(PrizePool, RejectsMalformedJson) {
    const char* err = nullptr;
    (void)PrizePool::FromJson("{not json", &err);
    ASSERT_NE(err, nullptr);                          // 解析失败且不崩溃
}
TEST(PrizePool, RejectsMissingField) {
    const char* err = nullptr;
    (void)PrizePool::FromJson("{\"version\":2,\"total_pages\":30}", &err);  // 缺 "tiers"
    ASSERT_NE(err, nullptr);
}
TEST(PrizePool, RejectsVersionMismatch) {
    const char* err = nullptr;
    (void)PrizePool::FromJson("{\"version\":1,\"tiers\":[]}", &err);         // 旧版 1
    ASSERT_NE(err, nullptr);
}

// ---------- 实时概率（Web 显示用） ----------
TEST(PrizePool, ComputesDisplayProbability) {
    PrizePool pool(Make4Tiers(), kPages, DepletionPolicy::kFallbackToLose);
    auto probs = pool.DisplayProbabilities();
    ASSERT_EQ(probs.size(), 4u);
    double sum = 0;
    for (double x : probs) sum += x;
    EXPECT_NEAR(sum, 1.0, 1e-9);
    EXPECT_NEAR(probs[0], 1.0 / 100, 1e-9);
    EXPECT_NEAR(probs[3], 74.0 / 100, 1e-9);

    // 耗尽一级后重算：分母变小，其余级概率上升；耗尽级自身为 0
    pool.tiers_mutable()[0].remaining = 0;
    auto p2 = pool.DisplayProbabilities();
    EXPECT_DOUBLE_EQ(p2[0], 0.0);
    EXPECT_NEAR(p2[1] + p2[2] + p2[3], 1.0, 1e-9);
    EXPECT_GT(p2[1], probs[1]);                       // 99 分母 → 概率上升
}
```

- [ ] **Step 3** 运行 → **必须失败**（Red）。
- [ ] **Step 4** 实现 `prize.h` + `prize_pool.cc`。**失败语义必须显式化（B-3 / M-8）**——`DrawResult` 是值类型、没有错误通道，无法表达 `kRefuse`：

```cpp
// main/lottery/prize.h
enum class DrawStatus { kOk, kAllDepleted, kRefuse, kInvalidConfig };

struct DrawOutcome {
    DrawStatus status = DrawStatus::kInvalidConfig;
    DrawResult result{};                       // status != kOk 时无效
    const char* error = nullptr;               // 中文原因，供 Web 400 回传
};

// 候选集 = weight>0 && (quantity==-1 || remaining>0)
DrawOutcome Decide(uint32_t random_value) const;          // 纯函数，不改库存
DrawOutcome DrawAndConsume(uint32_t random_value);        // Decide + 扣减 + SaveSync
// Validate(): 设计 §7.2 的 8 条自检，返回 error 中文原因
```

> ⚠️ **不要写成 `DrawResult Draw(...)`**：设计 §5.2 定义的是 `Decide()` / `DrawAndConsume()`，
> 而 Task 3.1 原先调用的 `pool_.Draw()` **在本计划与设计里都不存在**（必然编译失败）。

- [ ] **Step 5** 运行 → **全部 Green**。
- [ ] **Step 6** 编译验证 `scripts/build.sh s3`。
- [ ] **Step 7** **库存必须走同步保存（B-3 / D22）**：

```cpp
// DrawAndConsume() 内，扣减 remaining 之后：
NvsSaver::GetInstance().SaveSync();     // 同步写盘；不要用 RequestSave()
```

> **为什么不能用 `RequestSave()`**：它是非阻塞异步（`xQueueSend(..., 0)`），队列深度仅 **2**
> （`page-player/main/nvs_saver.cc`），**满即静默丢弃**，且写盘在低优先级任务里无完成通知。
> 库存是**必须可靠落盘**的业务数据，且扣减是低频事件（每次抽奖 ≥5 s），同步写代价可忽略。
> 单测：注入桩断言 `SaveSync` 被调用；并在真机上"抽中即断电重启"验证 `remaining` 已递减。

### Task 2.1b：奖池默认值与页数配置

**Files:** `main/lottery/prize_pool.cc`（内建默认奖池）, `main/settings.{h,cc}`

- [ ] **Step 1** 写**编译内建默认奖池**（30 页 + 4 级，用于三级兜底的最末一级，保证开箱即用）：

```cpp
// page 0/1/2 对应前三级；page 3 为兜底"谢谢参与"；其余 26 页留扩展
{1, 0,  1, 1,  1, "一等奖",   "奖品：扫地机器人"},
{2, 1,  5, 5,  5, "二等奖",   "奖品：蓝牙音箱"},
{3, 2, 20, 30, 30, "三等奖",  "奖品：定制水杯"},
{4, 3, 74, -1, 0, "谢谢参与", "欢迎下次再来"},
```

- [ ] **Step 2** 单测：默认奖池必须**自身通过 `Validate()`**（否则三级兜底的最末一级失效）；且 `probability` 合计 ≈ 1.0。
- [ ] **Step 3** 实现 `PUT /api/pages` 的**改页数保护**（决策 D16 / R13）：改 `total_pages` 时，若已有奖池里有 `page >= 新页数` 的级，**拒绝保存**并提示先改奖池；同时把 `PageMapping` 长度不足视为需要重新标定，返回警告字段 `needs_mapping_recalibration: true`。
- [ ] **Step 4** 单测：30 页奖池改成 10 页 → 因存在 `page >= 10` 的级而拒绝，错误信息含具体页码。
- [ ] **Step 5** 编译验证。

---

### Task 2.1c：奖池三级兜底链（D9 / R5.8，必须）

> 编号已重排为 2.1 → 2.1b → 2.1c → 2.1d（N-8：原先 `2.1b-2` 排在 `2.1b` 之前、编号非单调，且 `2.1c` 缺失）。
> **执行顺序**：2.1b（页数/默认奖池）→ 2.1c（兜底链）→ 2.1d（30 页标定）。三者互不阻塞，但按此顺序阅读最顺。

> `plan.md` 原先只在 Task 5.2 做"验证"，**没有任何实现 Step**——第一级（加载+校验）与第二级（备份）都没人写。

**Files:** `main/lottery/prize_pool.{h,cc}`, `main/settings.{h,cc}`

- [ ] **Step 1** 实现 `LoadChain()`：读 NVS `prize_pool` → `Validate()` → 失败读 `prize_pool_backup` → 再失败用**编译内建默认奖池**。每一级都 `ESP_LOGW` 说明来源。
- [ ] **Step 2** 保存时**先写备份、再写主**（保证备份永远不比主旧）。
- [ ] **Step 3** 测试：① 主键损坏 → 用备份；② 主+备份都损坏 → 用内建且能通过 `Validate()`；③ 备份比主新时以主为准（主是可解析的）。
- [ ] **Step 4** `Settings::Load()`/`DoSave()` 必须包含 `prize_pool` 与 `prize_pool_backup`（见 Task 0.3 的新增 Step）。

> **`remaining` 的持久化时机（D12 / D22，已按源码修正）**：扣减后 **必须 `NvsSaver::GetInstance().SaveSync()` 同步写盘**（见 Task 2.1 Step 7），**不要用 `Settings::RequestSave()`**。
>
> ❌ **已作废的旧论断**：本计划此前写过"用 `RequestSave()`，断电宁多发一个"。该论断**不成立**——`RequestSave()` 是异步非阻塞且**队列满即静默丢弃**（深度 2，`page-player/main/nvs_saver.cc`），"落盘"并不保证发生，超发数量不可控。**不要在 `prize_pool.cc` 顶部写任何"宁多发一个"的注释。**

---

### Task 2.1d：30 页行程值标定（B-4 / R6.3 / R13 / D16 / D21，**不修则无法开奖**）

> ⚠️ 全计划原先 **没有任何 Step 调用 `PageMapping::SetMapping`**，也不打包出厂 `page_mapping.json`。
> 而 Task 3.0 的前置断言要求"映射覆盖全部页" → **断言必然失败 → `Start()` 永远返回 false → 永远停在 `kBoot` → 无法开奖**。
> 计划自己没提供出路。**这是"照此计划无法投用"级别的问题。**

**Files:** `main/page_mapping.{h,cc}`, `main/settings.{h,cc}`, `main/web/web_server.cc`, `main/web/assets/spiffs_contents/calibrate.html`

- [ ] **Step 1** 提供**唯一权威入口** `PUT /api/page-mapping`：body `{values:[N], names:[N]}` → 校验后
      `PageMapping::SetMapping(values, names)` → `SaveMapping()` → `PageAdjust::Resize(1, N)`。
- [ ] **Step 1b（必须，否则 `stroke_min/max` 静默失效）** 在 `kBoot` 里**先显式生成并落盘映射**，再断言/归零：

```cpp
// ⚠️ PageMapping::GetMapping() 是惰性加载；LoadMapping() 在文件缺失时会隐式调
//    SetDefaultMapping(total_pages)，而那正是写死 i*255/N 的均匀公式
//    （page-player/main/page_mapping.cc:61-73, 85-87）。
//    => 只加 stroke_min/max 配置而走隐式默认，行程跨度会被完全忽略，
//       且恰好能通过"互异 + 间隔 >= 8"断言（因为它就是按 8 造的）→ 查不出来的静默失效。
std::vector<uint8_t> values = GenerateFromStrokeSpan(
    settings.stroke_min_dmx(), settings.stroke_max_dmx(), settings.total_pages());
PageMapping::GetInstance().SetMapping(values, default_names);   // 显式落盘
PageAdjust::GetInstance().Resize(1, settings.total_pages());
// 此后 GetMapping() 时 loaded_ 已为 true，不会再触发隐式默认
```
  单测：① `stroke_min=0, stroke_max=255, N=30` 时值与 `i*255/29` 一致；② 扩大跨度后**相邻差变大**（证明配置真的生效）；
  ③ 若只设 `stroke_min/max` 而不调 `SetMapping`，断言 `GetMapping()` 会返回均匀值（**把这个陷阱固化成测试**）。
- [ ] **Step 2** **打包一份出厂 `page_mapping.json`** 进 `main/assets/spiffs_contents/`（依赖 Task 0.1 Step 4b 的 SPIFFS 镜像），内容为 `SetDefaultMapping(30)` 的结果。
- [ ] **Step 3** 新增 `calibrate.html`：30 行「页码 / 行程值 / 名称」表格 + **「点动到该页」**按钮（调 `POST /api/page/goto {page}`）+ 人工核对后回填 + 保存。
- [ ] **Step 4** 校验（对应 D21，**取代原"min != max"弱断言**）：

```cpp
// 1) 长度必须 == total_pages
// 2) 排序后查重：任意两个值必须互不相同
// 3) 相邻差 >= kMinPageGapDmx（默认 8，实测标定后调大）
// 4) 值必须落在 [stroke_min_dmx, stroke_max_dmx] 内
```
- [ ] **Step 5** 单测：① 提交 30 个相同值 → `400`「行程值退化」；② 提交 29 个 → `400`「长度不足」；
      ③ `SetDefaultMapping(30)` 后 30 个值互不相同且最大间隔 8。
- [ ] **Step 6** **真机验可用性判据（D21 / §4.2b）**：逐页点动 30 次，人工核对机器是否落到正确页面。
      **若任意两次落到同一格、或落页与目标不符 → 该机台不支持 30 页**，必须下调 `total_pages`（建议 10~16）或换行程更大的执行器。**结论记入 README。**

---

### Task 2.2：SlotMachineEffect 减速计划

**Files:** `main/lottery/slot_machine_effect.{h,cc}`, `test_apps/main/test_slot_machine_effect.cc`

- [ ] **Step 1** 写 `slot_machine_effect.h`（设计 §5.3 的 `SlotMachineParams` / `HopPlan` / `Plan()` / `Build()`）。
- [ ] **Step 2** 写 `test_slot_machine_effect.cc`（**先写测试**）：

```cpp
// 辅助：可预测的伪随机源（LCG），保证测试可重放
static std::vector<uint32_t> MakeRandoms(size_t n, uint32_t seed = 12345) {
    std::vector<uint32_t> v(n); uint32_t x = seed;
    for (size_t i = 0; i < n; ++i) { x = x * 1103515245u + 12345u; v[i] = (x >> 16) & 0x7FFF; }
    return v;
}
static int RealMs(int nominal_ms) { return 40 * ((nominal_ms + 39) / 40); }   // ActionDelay 语义
static int DecelTotal(const HopPlan& p, int settle_bounces) {
    int t = 0;
    for (int i = 0; i < (int)p.delays.size() - settle_bounces * 2; ++i) t += RealMs(p.delays[i]);
    return t;
}

TEST(SlotMachine, HopsInRange) {
    // 对 200 组不同随机序列验证跳数落在 [min_hops, max_hops]
    ReloadSlotMachineParamsForTest();
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        auto plan = SlotMachineEffect::Plan(/*final_page=*/3, params, MakeRandoms(64, seed));
        EXPECT_GE((int)plan.pages.size(), params.min_hops) << "seed=" << seed;
        EXPECT_LE((int)plan.pages.size(), params.max_hops) << "seed=" << seed;
        EXPECT_EQ(plan.pages.size(), plan.delays.size());
    }
}
TEST(SlotMachine, NeverLandsEarly) {
    // 除最后一跳外，任何一跳都不得等于 final_page
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        const int fp = 3;
        auto plan = SlotMachineEffect::Plan(fp, params, MakeRandoms(64, seed));
        for (size_t i = 0; i + 1 < plan.pages.size(); ++i)
            EXPECT_NE(plan.pages[i], fp) << "seed=" << seed << " 第 " << i << " 跳提前落定";
    }
}
TEST(SlotMachine, LastHopIsFinal) {
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        const int fp = 7;
        auto plan = SlotMachineEffect::Plan(fp, params, MakeRandoms(64, seed));
        ASSERT_FALSE(plan.pages.empty());
        EXPECT_EQ(plan.pages.back(), fp);
        EXPECT_EQ(plan.final_page, fp);
    }
}
TEST(SlotMachine, DelaysMonotonicAndAligned) {
    // 减速段（前 n - settle*2 跳）单调不减；全部 delay 是 40 的倍数且在 [min,max]
    int n = params.settle_bounces * 2;
    for (uint32_t seed = 1; seed <= 100; ++seed) {
        auto plan = SlotMachineEffect::Plan(3, params, MakeRandoms(64, seed));
        ASSERT_GT((int)plan.delays.size(), n);
        for (size_t i = 0; i < plan.delays.size(); ++i) {
            EXPECT_EQ(plan.delays[i] % 40, 0) << "未对齐 tick, seed=" << seed << " i=" << i;
            EXPECT_GE(plan.delays[i], params.min_hop_ms);
            EXPECT_LE(plan.delays[i], params.max_hop_ms);
        }
        for (size_t i = 1; i + n < plan.delays.size(); ++i)
            EXPECT_GE(plan.delays[i], plan.delays[i - 1]) << "减速段非单调, seed=" << seed;
    }
}
TEST(SlotMachine, NoAdjacentRepeat) {
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        auto plan = SlotMachineEffect::Plan(3, params, MakeRandoms(64, seed));
        for (size_t i = 1; i < plan.pages.size(); ++i)
            EXPECT_NE(plan.pages[i], plan.pages[i - 1]) << "seed=" << seed << " 相邻重复页 i=" << i;
    }
}
TEST(SlotMachine, LateHopsNearFinal) {
    // 弹跳开始前的最后 3 跳，必须落在 final 的邻域（擦边紧张感）
    const int fp = 5, N = params.total_pages;
    for (uint32_t seed = 1; seed <= 100; ++seed) {
        auto plan = SlotMachineEffect::Plan(fp, params, MakeRandoms(64, seed));
        int end = (int)plan.pages.size() - params.settle_bounces * 2;   // 弹跳段起点
        ASSERT_GE(end, 4);
        for (int i = end - 3; i < end; ++i) {
            int p = plan.pages[i];
            bool near = (p == fp) || (p == (fp - 1 + N) % N) || (p == (fp + 1) % N);
            EXPECT_TRUE(near) << "seed=" << seed << " 末段第 " << i << " 跳=" << p << " 不在邻域";
        }
    }
}
TEST(SlotMachine, RealDurationReasonable) {
    // 见设计 §5.3 时序模型。逐跳复算（默认 min_hops=18, max_hops=20）：
    //   n=18 → 4,920 ms；n=20 → 5,320 ms。断言区间必须是 [4900, 5400] ms。
    // ⚠️ 不要放宽下界：下界过低会放过"弹跳段漏算"的实现（这正是本设计
    //    前两稿算错的地方——把 settle_bounces 的次数当跳数重复乘/漏乘）。
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        auto plan = SlotMachineEffect::Plan(3, params, MakeRandoms(64, seed));
        int real = 0;
        for (int d : plan.delays) real += RealMs(d);
        real += params.settle_bounces * 2 * 400;          // 每次弹跳 = 2 跳
        EXPECT_EQ(real, plan.expected_duration_ms);        // 自算与实现必须一致
        EXPECT_GE(real, 4900) << "seed=" << seed;
        EXPECT_LE(real, 5400) << "seed=" << seed;
    }
}
TEST(SlotMachine, BounceIsTwoHopsPerBounce) {
    // 口径防回归：settle_bounces=2 必须是 4 跳 / 1600 ms，不是 8 跳 / 3200 ms
    auto plan = SlotMachineEffect::Plan(3, params, MakeRandoms(64, 7));
    int bounce_ms = 0, hops = 0;
    for (int d : plan.delays) if (d == 400) { bounce_ms += RealMs(d); hops++; }
    EXPECT_EQ(params.settle_bounces * 2, 4);            // 参数口径
    EXPECT_EQ(bounce_ms, 1600) << "弹跳总时长应为 1600ms（4 跳 × 400ms），实际 " << bounce_ms;
    EXPECT_EQ(hops, 4) << "弹跳跳数应为 4";
}
TEST(SlotMachine, PredictsDurationMatchingTicks) {
    // expected_duration_ms 必须等于 Σ 40*ceil(delays[i]/40)（含弹跳），
    // 因为现场核对就靠它——算错会让"预期时长"永远对不上。
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        auto plan = SlotMachineEffect::Plan(9, params, MakeRandoms(64, seed));
        int sum = 0;
        for (int d : plan.delays) sum += RealMs(d);
        EXPECT_EQ(plan.expected_duration_ms, sum) << "seed=" << seed;
    }
}
TEST(SlotMachine, DeterministicForSameRandoms) {
    auto r = MakeRandoms(64, 999);
    auto a = SlotMachineEffect::Plan(4, params, r);
    auto b = SlotMachineEffect::Plan(4, params, r);
    EXPECT_EQ(a.pages, b.pages);
    EXPECT_EQ(a.delays, b.delays);
    EXPECT_EQ(a.expected_duration_ms, b.expected_duration_ms);
}
```

- [ ] **Step 3** 运行 → Red。**Step 4** 实现 `Plan()`：均匀随机跳页（剔除 `final_page`）→ ease-out 曲线 `min + (max-min) * (i/(n-1))^2` → 对齐 40 ms → 末 3 跳邻域 → 弹跳段。
- [ ] **Step 5** 运行 → Green。
- [ ] **Step 6** 实现 `Build()`：把 `HopPlan` 转成 `ActionSequence`，每步 `ActionGroup{ActionPaging(1, page, adj, 0, speed), ActionDelay(delay)}`，`adj = PageAdjust::GetAdjustedValue(0, page)`，`cell_id` 恒为 **1**。编译验证。

---

### Task 2.3：DrawRecord 票号与流水

**Files:** `main/lottery/draw_record.{h,cc}`, `test_apps/main/test_draw_record.cc`

- [ ] **Step 1** 写测试：同一天递增；跨日归零；`YYYYMMDD-NNNN` 格式（补零）；CRC16 校验码稳定性；无 RTC 时不崩溃。
- [ ] **Step 2** 运行 → Red。**Step 3** 实现（纯逻辑 + **注入的** epoch/日期与计数）。**Step 4** 运行 → Green。
- [ ] **Step 5（N-6，职责边界必须写清）** **`DrawRecord` 不负责取时间**——只接受**注入的当前 epoch**：

```cpp
// main/lottery/draw_record.h —— 纯逻辑，不 include 任何 SNTP / 网络 / wifi 头文件
class DrawRecord {
public:
    /// @param now_epoch 由调用方注入的当前时间；未知时传 0
    std::string NextTicket(time_t now_epoch);
    bool NeedsDailyReset(time_t now_epoch) const;
    // 无 esp_sntp / wifi 依赖 → 单测不需要网络桩
};
```

> **时间从哪来**：由独立的 **`main/lottery/time_source.{h,cc}`**（Task 2.4）提供，
> 内部做"SNTP 已同步 > 持久化 `last_known_epoch` + 开机时长 > 编译时间"三级回退，
> 对外只暴露 `time_t TimeSource::NowEpoch()`。
> `Application` 把 `NowEpoch()` 结果**注入**给 `DrawRecord`。
> **禁止**把 SNTP 塞进 `draw_record.cc`（违反 SRP，且会让单测依赖网络）。

---

### Task 2.4：时间基准（B-2 / R7，**原计划完全没有**）

> ⚠️ 全计划里 `sntp`/`NTP`/`时间同步` 出现 **0 次**。但三处功能硬依赖时钟：
> ① `ReceiptData::time_str`（小票"2026-09-29 14:32:07"）；② 票号 `YYYYMMDD-NNNN`；
> ③ 跨日归零判定（`serial_date` vs 今日）。Task 2.3 只写了"无 RTC 时不崩溃"——
> **"不崩溃"不等于"时间正确"**，而 R7 的缓解措施没有任何 Step 去实现。

**Files:** `main/lottery/time_source.{h,cc}`（**新增，独立于 `draw_record`**，见 Task 2.3 Step 5 / N-6）, `main/application.cc`, `main/wifi/*`

- [ ] **Step 1** 定义时间源优先级：`SNTP 已同步` > `持久化最后已知时间 + 开机时长` > `编译时间 __DATE__/__TIME__`。对外接口 `time_t TimeSource::NowEpoch()`，**不含业务逻辑**。
- [ ] **Step 2** 接入 SNTP（`esp_sntp`），时区 `CST-8`；同步成功后把 epoch 写入 NVS `last_known_epoch`。
- [ ] **Step 3** 单测：① 无网络时 `NowEpoch()` 返回持久化时间而非 1970（注入桩网络状态）；② 跨日判定用**本地日期**；
      ③ 时间回拨（NTP 校时把日期调回昨天）时票号流水**不重复**（`DrawRecord` 侧测，时间由注入控制）。
- [ ] **Step 4** `GET /api/status` 增加 `time_synced: bool` 与 `now`；前端未同步时显示"时间未校准"告警。
- [ ] **Step 5** 真机：断网开机 → 打票时间**非 1970**；联网后时间正确。

---

## Phase 3：抽奖状态机与集成

### Task 3.0：开机归零（Boot Homing）—— 必须先于抽奖实现

> 依据：设计 §5.5、决策 D11、需求 R2.5。**这是保证"落页 == 票面"在每次重启后都成立的前提**，不能跳过。
> 若先做抽奖再做归零，则每次测试都要手动把机器拨到第 0 页，且线上会出现"票面一等奖、机器停三等奖"的事故。

**Files:** `main/lottery/lottery_controller.{h,cc}`（**归零并入同一状态机**；**不建** `boot_homing.*`）

> **M-5 修正**：此前版本把前 4 个状态**定义了两遍**（`HomingState` 与 `LotteryState` 的前 4 项），
> 必然要写双向同步代码，漂移时无人发现；且原文写了"**或并入** `lottery_controller`"——**模糊表述，实现者要自行决策架构**。
> **现在定死：只有一个状态机**（抽奖与归零在同一条时间轴上），归零做成 `LotteryController` 的**私有方法**。

- [ ] **Step 1** 定义**单一**状态与接口（**先写测试，见 Step 6 的顺序要求**）：

```cpp
// main/lottery/lottery_controller.h —— 唯一状态机
enum class LotteryState {
    kBoot,          // 加载 PageMapping/PageAdjust/奖池 + Resize(1,total_pages)
    kHoming,        // 归零中，拒绝一切触发
    kHomed,         // 落 current_page=0 到 NVS
    kHomingFailed,  // 终态，需人工介入
    kIdle, kSpinning, kPrinting, kDone,
    kOutOfPrizes,   // 奖品全耗尽且 policy==kRefuse
};

class LotteryController {
public:
    LotteryController(MotionEngine& engine, PrinterService& printer, PrizePool& pool,
                      Settings& settings);
    /// 由 Application 初始化序列末尾调用；内部先做前置检查再进入 kHoming
    void Start();
    LotteryState GetState() const;
    bool IsDrawable() const;              // kIdle/kDone 才允许开奖

private:
    bool CheckPrerequisites();            // 映射覆盖全页 + 互异 + 最小间隔（D21）
    void BeginHoming();
    void OnHomingScheduleDone();          // 必须判 state_==kHoming 才生效
    void OnHomingTimeout();
    std::vector<std::unique_ptr<Action>> BuildHomingActions(int total_pages, uint8_t speed);

    static constexpr int kHomingSettleMs = 10000;   // 机械释放等待，照抄 page-player，不可改
    static constexpr int kHomingStableMs = 500;     // 归零序列末尾的稳定期
    static constexpr int kHomingMaxRetry = 3;
};
```

- [ ] **Step 2** 实现 `Start()`：**前置断言**——`PageMapping::GetMapping().size() >= total_pages` 且 `PageAdjust` 已加载，否则 `ESP_LOGE` 并返回 `false`（由 `Application` 稍后重试），**绝不带着空映射下发归零**。

- [ ] **Step 3** 实现归零序列（**照抄 `page-player/main/paging_controller.cc:112-153` 的机械时序，不要改动 10 秒**）：

```cpp
std::vector<std::unique_ptr<Action>> BuildHomingActions(int total_pages, uint8_t speed) {
    const int cell = 1;                                  // lucky-page 恒为单机
    const uint8_t adj = PageAdjust::GetInstance().GetAdjustedValue(0, 0);

    std::vector<std::unique_ptr<Action>> step1_children;
    step1_children.push_back(std::make_unique<ActionPaging>(
        cell, 0, adj, 0, speed, 0, 0, 0, 150));          // ch07=150 拉重置
    step1_children.push_back(std::make_unique<ActionDelay>(10000));  // 等机械完全释放
    auto step1 = std::make_unique<ActionGroup>(std::move(step1_children));

    std::vector<std::unique_ptr<Action>> step2_children;
    step2_children.push_back(std::make_unique<ActionPaging>(
        cell, 0, adj, 0, speed, 0, 0, 0, 0));            // 收掉重置通道
    auto step2 = std::make_unique<ActionGroup>(std::move(step2_children));

    std::vector<std::unique_ptr<Action>> steps;
    steps.push_back(std::move(step1));
    steps.push_back(std::move(step2));
    std::vector<std::unique_ptr<Action>> actions;
    actions.push_back(std::make_unique<ActionSequence>(std::move(steps)));
    return actions;
}
```

- [ ] **Step 4** 实现 `OnScheduleDone()`：**仅在 `kHoming` 时**生效 → 启动一次性 `esp_timer`（`kPostHomingStableMs`）→ 到期置 `kHomed`。`LotteryController` 收到 `kHomed` 后落 `current_page = 0` 到 NVS 并转 `kIdle`。

- [ ] **Step 5** 实现失败重试：`kHoming` 超时（`kHomingSettleMs + 3000` ms 内未收到 done）→ 重发归零序列，最多 3 次；3 次仍失败 → `kFailed`，Web `/api/lottery/status` 返回 `state:"homing_failed"`，**拒绝开奖**并记 `ESP_LOGE`（宁可不开奖，不可开错奖）。

- [ ] **Step 6（M-6，顺序修正：测试必须驱动实现）** **先写 `test_lottery_controller.cc`，再回来写实现**。原计划把 4 个实现步骤全排在唯一测试步骤之前，**违反 `AGENTS.md` §6「禁止跳过测试直接写实现」**，测试只能验证已写完的东西、看不到 Red。

  正确顺序（**按此重排 Step 2~5 与 Step 6**）：
  1. **先写测试** → 运行 → **必须 Red**：
     - ① 映射不足/不互异时 `Start()` 返回 false 且**未调用** `Reschedule`（用桩记录调用）
     - ② 映射充分时下发 **1 个 `ActionSequence` 含 2 个 `ActionGroup`**；第 1 组含 `ch07=150` + `ActionDelay(10000)`；第 2 组 `ch07=0` 无延时
     - ③ 归零序列末尾含 `ActionDelay(500)` 稳定期，且 `OnScheduleDone` **只置一次** `kHomed`
     - ④ `state_ != kHoming` 时 `OnHomingScheduleDone()` **必须直接 return**（防 `Reschedule` 导致回调二次触发）
     - ⑤ 超时 3 次后状态为 `kHomingFailed`
  2. 定义状态与接口（最小实现使测试能编译）
  3. 实现 `BuildHomingActions()` / `BeginHoming()` → 部分 Green
  4. 实现超时重试 → Green

- [ ] **Step 7** 编译 + 真机验证：
  - 手动把翻页机拨到第 3 页 → 复位设备 → 观察归零到第 0 页 → 再抽奖 → **落页正确**。
  - 在归零的 10 秒窗口内按按钮 / `curl POST /api/draw` → 断言返回 `409`，**不产生翻页也不出票**。
  - 归零中途断电重启 → 断言重新完整归零。

> **调试便利（m-3：必须有可执行 Step，不能只写尾注）**：
> - [ ] 在 `main/Kconfig.projbuild` 增加 `CONFIG_LUCKY_SKIP_HOMING_ON_BOOT`（bool，默认 `n`）
> - [ ] 在 `sdkconfig.defaults` **显式写 `CONFIG_LUCKY_SKIP_HOMING_ON_BOOT=n`**（出厂必须关闭）
> - [ ] 加单测/真机验证：置 `y` 时跳过 10 s 等待、直接进 `kIdle` 可开奖，但日志必须打 `WARN: homing skipped`

---

### Task 3.1：LotteryController

**Files:** `main/lottery/lottery_controller.{h,cc}`

- [ ] **Step 1** 写头文件（设计 §5.4）。**`LastDrawResult` 此前在全文档中未定义**（B-3），一并补上：

```cpp
// main/lottery/lottery_controller.h
enum class PrintOutcome { kOk, kFailed, kSkipped };

struct LastDrawResult {
    bool         valid = false;
    std::string  ticket;
    std::string  prize_name;
    int          page = 0;          // 0-based
    int          level = 0;
    std::string  time_str;
    PrintOutcome print = PrintOutcome::kSkipped;
};
```
> 该结构**只在主线程写入**；对 Web 暴露时必须转成 Task 0.1/§8.1 所述的**无指针 atomic 快照**（D23），不得把含 `std::string` 的对象直接交给 HTTP 任务。

```cpp
// ⚠️ 状态枚举的唯一定义在 Task 3.0 Step 1（含 kBoot/kHoming/kHomed/kHomingFailed/kOutOfPrizes）。
//    此处不再重复定义，避免两份漂移（M-5）。

class LotteryController {
public:
    LotteryController(MotionEngine& engine, PrinterService& printer, PrizePool& pool);
    /// 触发一次抽奖。非 kIdle/kDone 时返回 false（单飞，设计 D4）。
    bool Trigger();

    /// 最近一次开奖结果（值拷贝，线程安全；B-3 补上此前未定义的类型）
    LastDrawResult GetLastResult() const;

    /// 由 MotionEngine 的 schedule-done 回调驱动（Application 中 Schedule 到主线程）
    void OnScheduleDone();
    /// 由 PrinterService 的完成回调驱动
    void OnPrintDone(bool ok);

private:
    void StartSpin(uint32_t random_value);
    void EnterDone();

    MotionEngine&   engine_;
    PrinterService& printer_;
    PrizePool&      pool_;
    std::atomic<LotteryState> state_{LotteryState::kIdle};
    LastDrawResult  last_{};
    esp_timer_handle_t done_timer_ = nullptr;   // kDone → kIdle (auto_return_ms)
};
```

- [ ] **Step 2（B-3）** 实现 `Trigger()`：`IsDrawable()` 检查 → **`pool_.DrawAndConsume(esp_random())`**（**不是不存在的 `pool_.Draw()`**）→ 按 `DrawOutcome::status` 分支：

```cpp
auto outcome = pool_.DrawAndConsume(esp_random());
if (outcome.status == DrawStatus::kRefuse || outcome.status == DrawStatus::kAllDepleted) {
    state_ = LotteryState::kOutOfPrizes;      // 报 /api/lottery/status: "out_of_prizes"
    return false;                              // 拒绝开奖：宁可不发，不发错的
}
if (outcome.status != DrawStatus::kOk) {
    ESP_LOGE(TAG, "抽奖失败: %s", outcome.error ? outcome.error : "未知");
    return false;
}
// 继续：draw_record_.Next() → SlotMachineEffect::Plan() → engine_.Reschedule(Build(plan))
// → state_ = kSpinning → ESP_LOGI 打印完整 HopPlan + expected_duration_ms
```
- [ ] **Step 3** 实现 `OnScheduleDone()`：仅当 `state_ == kSpinning` 时生效 → 组装 `ReceiptData` → `printer_.Enqueue(...)` → `state_ = kPrinting`。

> **关键**：`PagingController::OnScheduleDone` 的语义是"管线从非空变空触发一次"（`motion_engine.cc:256`）。**必须**在 `state_ != kSpinning` 时忽略该回调，否则上一次抽奖的尾触发会把下一次状态机打乱。

- [ ] **Step 4** 实现 `OnPrintDone(bool)` → `state_ = kDone` → 启动一次性 `esp_timer`（`auto_return_ms`）→ `kIdle`。
- [ ] **Step 5** 编译验证。

---

### Task 3.2：Application 集成（按钮 + 管线回调 + 模块初始化）

**Files:** `main/application.{h,cc}`

- [ ] **Step 1（M-5）** 在 `ContinueInitialization()` 按依赖顺序初始化，**必须含 `LotteryController.Start()`**（否则没人启动归零 → 永远停在 `kBoot`）：

```
Dmx512Service
  → MotionEngine(cell_count=1, paging_channel_count)
  → PageMapping::LoadMapping()          // 必须在归零前
  → PageAdjust::LoadAdjust()            // 必须在归零前
  → PrizePool::LoadChain()              // 三级兜底（Task 2.1c）
  → PrinterService.Init()
  → LotteryController(...)
  → LotteryController::Start()          // ← 触发前置检查 → kHoming
  → WebServer
```

> ⚠️ `PageMapping` / `PageAdjust` 必须在 `Start()` **之前**加载完成，否则归零序列会取到未加载的行程值（`GetAdjustedValue` 静默返 0，见设计 §5.5）。
- [ ] **Step 2** 接管线完成回调（照 `page-player/main/application.cc:234`）：

```cpp
motion_engine_->SetScheduleDoneCallback(
    [this]() { Schedule([this]() { lottery_controller_->OnScheduleDone(); }); });
```

- [ ] **Step 3** 接打印完成回调：`printer_service_->SetDoneCallback([this](bool ok){ Schedule([this,ok](){ lottery_controller_->OnPrintDone(ok); }); });`
- [ ] **Step 4** 接按钮（`OnPress` → `Schedule` → `Trigger`；`OnLongPress` → 打测试票）：

```cpp
module_button_ = std::make_unique<Button>(BUTTON_MODULE_GPIO, 1);   // active_high，沿用 page-player
module_button_->OnPress([this]() {
    Schedule([this]() { if (!lottery_controller_->Trigger()) ESP_LOGW(TAG, "抽奖进行中，忽略触发"); });
});
module_button_->OnLongPress([this]() { Schedule([this]() { PrintTestTicket(); }); });
```

- [ ] **Step 5** **强制约束**：`cell_count` 恒为 `1`；断言 `settings.cell_count() == 1`，否则 `ESP_LOGE` 并强制置 1。
- [ ] **Step 6** 编译验证。**Step 7** 真机验证：串口 `draw` → 观察翻页减速落地 + 出票，核对落页 == 票面奖项页。

---

### Task 3.3：串口排障命令

**Files:** `main/console_cmd.{h,cc}`（参考 `tft_displaye/main/console_cmd.c`）

- [ ] **Step 1** 实现设计 §8.3 的 6 个命令：`draw` / `ptest` / `pstatus` / `pdump` / `prizes` / `gbtest <中文>`。
- [ ] **Step 2** `pdump` 需在 `lucky_printer_uart.c` 里保留"最近一次发送字节"的抓取缓冲（移植 `tft_displaye` 的做法）。
- [ ] **Step 3** 编译 + 真机验证每个命令都有输出。

---

## Phase 4：Web 配置界面

### Task 4.1：WebServer 端点

**Files:** `main/web/web_server.{h,cc}`

- [ ] **Step 1** 从 `page-player/main/web_server.cc` 裁剪出最小 HTTP 框架（路由注册 + Captive Portal + SPIFFS 托管）。**注意 `max_uri_handlers` 要够**（`page-player` 曾因 60 不够调到 80；本机端点更多，见 Step 2）。
- [ ] **Step 2** 实现设计 §8.1 的**全部端点**，按三组：

| 组 | 端点 |
|---|---|
| 抽奖/状态 | `POST /api/draw`、`GET /api/lottery/status`、`GET /api/status`、`POST /api/printer/test` |
| 奖池/页数 | `GET|PUT /api/prizes`、`POST /api/prizes/reset-stock`、`GET|PUT /api/pages` |
| 文案/Logo | `GET|PUT /api/receipt`、`GET /api/receipt/preview`、`POST|GET|DELETE /api/logo` |

- [ ] **Step 3** **所有 handler 只解析 JSON 并 `Application::Schedule()`**，不在 HTTP 任务上下文碰 MotionEngine。
- [ ] **Step 4** 写操作统一走"**同步校验 → 通过才落库**"：
  - `PUT /api/prizes` → `PrizePool::Validate()`，失败 `400` + 中文原因（如 `"页码 2 已被「三等奖」占用"`）
  - `PUT /api/pages` → 改页数保护（决策 D16）：奖池里有 `page >= 新页数` 的级则**拒绝**，并返回 `needs_mapping_recalibration`
  - `PUT /api/receipt` → 行宽/未知占位符校验
  - `POST /api/logo` → `PrinterLogo::Validate()`，失败 `400`
- [ ] **Step 5** `GET /api/prizes` 必须返回**实时理论概率** `probability` 与 `depleted` 标记（前端直接显示，避免商家拿 `weight` 当百分比心算，缓解 R11）。
- [ ] **Step 6** `POST /api/logo` 用 `LogoStore::Save()`（临时文件 + rename，决策 D15）；**校验失败绝不落盘**。
- [ ] **Step 7** `curl` 验证每个端点，**含全部 400 分支**（非法奖池、越界页数、坏 logo、坏文案模板）。用脚本跑一遍并保留输出。

---

### Task 4.2：前端 5 页

**Files:** `main/web/assets/spiffs_contents/{index,prizes,receipt,logo,calibrate}.html`

- [ ] **Step 1** `index.html`：大号「抽奖」按钮 + 实时状态（轮询 `/api/lottery/status`）+ 抽奖次数统计 + 「打印测试票」。
  **必须处理归零状态**（R9/D11）：`state=="homing"` 时显示"正在归零，请稍候（约 11 s）"倒计时并**禁用抽奖按钮**；`homing_failed` 显示红色告警。
- [ ] **Step 2** `prizes.html`：**分级奖池表格**——等级 / 页码（下拉，从 `/api/pages` 取）/ 权重 / **理论概率（只读，实时计算）** / 数量 / 剩余 / 名称 / 副标题，支持增删级、保存、「重置库存」。
  - 显示"权重合计"与"各级理论概率"，让概率含义可见（R11）
  - 对 `probability < 0.5%` 的级给**黄色警示**
  - 提供「逐页点动」调试按钮 → **后端端点 `POST /api/page/goto` 见 Task 2.1d Step 1 与设计 §8.1**（原计划只有前端按钮、没有后端端点）
  - 总页数编辑区（30 默认），保存走 `PUT /api/pages` 的越界保护
- [ ] **Step 3** `receipt.html`：文案编辑（商家名 / 中奖标题 / 未中奖标题 / 票头模板 / 票尾模板 / 有效期天数 / 纸宽），**右侧实时调用 `/api/receipt/preview` 显示渲染预览**；未知占位符高亮提示。
- [ ] **Step 4** `logo.html`：`<input type="file">` 选图 → canvas 缩放到目标宽度 → **阈值滑块 + 抖动开关实时预览** → 上传 `POST /api/logo`；显示当前 logo 尺寸/字节数/**预计打印耗时**；「删除 logo」按钮；提示"建议高度 ≤ 96 dots"。
- [ ] **Step 4b** `calibrate.html`（**Task 2.1d** 的前端）：30 行「页码 / 行程值 / 名称」表格 + 「点动到该页」（`POST /api/page/goto`）+ 人工核对回填 + 保存（`PUT /api/page-mapping`）；**顶部显示 D21 的"30 页可用性判据"提示**。
- [ ] **Step 5** 浏览器验证：保存奖池 → 刷新数据保留 → `curl /api/prizes` 一致；保存文案 → 预览与实打小票**逐字一致**；上传 logo → 打印后图形清晰。
- [ ] **Step 6** 验证各页的**非法输入中文报错**（重复页码、页数改小导致越界、坏模板、坏 logo）。

---

## Phase 5：硬化与收尾

### Task 5.1：二次定位（缓解 R5）

- [ ] **Step 1** 在 `LotteryController::OnScheduleDone()` 里，进入 `kPrinting` **之前**追加一次幂等的 `ActionPaging(cell_id=1, final_page, adj)` 补发，`ActionDelay(400)` 后确认。

> 理由：`ActionPaging` 是幂等的"设置 DMX 行程值"，二次下发不会造成机械过冲，但能修正机械抖动/丢步导致的落页偏差。**必须在打印前完成**，避免"票打了但机器没到位"。

- [ ] **Step 2** 真机连续触发 30 次，统计落页与票面一致率，目标 **30/30**。

### Task 5.2：压力与异常

- [ ] **Step 1** 连续快速按按钮 20 次 → 断言只产生 1 次抽奖 1 张票（单飞 D4 生效）。
- [ ] **Step 2** 抽奖中途拔打印机 → 断言翻页仍正常完成、`pstatus` fail 计数 +1、状态机回到 `kIdle`（**不卡死**）。
- [ ] **Step 3** 抽奖中途断 WiFi → 断言 `draw` 串口命令仍可触发（核心功能不依赖网络）。
- [ ] **Step 4** 故意写坏 NVS 奖池 → 断言三级兜底链生效（NVS → 备份 → 编译内建，D9）。

### Task 5.2b：串口日志监控（m-14，`AGENTS.md`「串口日志诊断」强制）

> `AGENTS.md` 明令"**烧录后必须自动启动串口日志监控，输出到 `logs/` 目录**"。原计划里 `logs/serial` **出现 0 次**，属违规。

- [ ] **Step 1** 在 Task 0.1 建 `lucky-page/logs/` 并加入 `.gitignore`。
- [ ] **Step 2** 搬运/适配串口监控脚本（参考 `page-player/AGENTS.md` 的 `logs/serial-<YYYYMMDD-HHMMSS>.log` 命名与 `page-player/scripts/`）。
- [ ] **Step 3** 在**每个真机验证 Step** 前置"启动监控 → 记录日志路径"，并把该路径写进交付说明。
- [ ] **Step 4** 崩溃时按 AGENTS.md 做 `addr2line` 解析：**S3 用 `xtensa-esp32s3-elf-addr2line -pfiaC -e build-esp32s3/lucky-page.elf <addr>`**，结论记入评审/handoff。

### Task 5.3：文档与版本

- [ ] **Step 1** 写 `lucky-page/README.md`：接线图 + 构建烧录 + 奖池配置 + 排障表（把设计 §11 风险表转成"现象→原因→处理"）。
- [ ] **Step 2** 校验 `lucky-page/reqs/req0001_lucky-page.md`（**已存在，200 行**）。**不要覆盖**——只核对内容与设计/计划是否一致，若有偏差则更新而非重写。
- [ ] **Step 3** 版本号与 `CMakeLists.txt` 的 `VERSION 0.1.0` 同 commit。
- [ ] **Step 4** 代码评审（按 `page-player/AGENTS.md` 规则 4：每次 commit 前评审）。

---

## 验证清单（Definition of Done）

| # | 项 | 通过标准 |
|---|---|---|
| 1 | 编译 | `scripts/build.sh s3` 零错误；零新增警告 |
| 2 | 单元测试 | **8 个测试文件全绿**（`test_apps/run_tests.py`）：prize_pool / slot_machine_effect / gbk_encoder / receipt_layout / printer_logo / qr_code / draw_record / **lottery_controller**。**且不得存在只有注释、没有断言的空壳用例**（`AGENTS.md` §6.4） |
| 3 | **开机归零** | 人为拨到第 3 页 → 复位 → 自动归零到第 0 页；归零期间触发被拒（`409`，不翻页不出票）；归零中断电重启会重新归零 |
| 3b | 按钮抽奖 | 按下 → 减速翻页 **4.9~5.3 s**（以 `expected_duration_ms` 日志为准）→ 落页正确 |
| 4 | 打印 | `ptest` 出票，**中文不乱码**，二维码可扫 |
| 5 | 一致性 | 连续 30 次，落页 == 票面奖项页（30/30） |
| 6 | 单飞 | 快速连按 20 次 → 仅 1 次抽奖 1 张票 |
| 7 | 容错 | 拔打印机/断网均不卡死，状态机回 `kIdle` |
| 8 | Web | 全部端点通；非法输入返回 400 且不落库 |
| 9 | 兜底 | 奖池 JSON 损坏时仍能开奖（三级兜底） |
| 10 | 文档 | README + req + spec + plan + 评审记录齐备；版本号与 tag 一致 |
| 10b | **代码格式**（m-13） | `find main -iname '*.cc' -o -iname '*.h' \| xargs clang-format --dry-run -Werror` 通过 |
| **11** | **奖池分级可配** | 30 页可改；增删等级、改数量/权重后**刷新持久**；页面显示**实时理论概率**且合计 ≈ 100% |
| **12** | **库存精确** | 某级 `quantity=3` → 连抽 10 次**恰好中 3 次**，第 4 次起不再出现；`剩余` 落 NVS，重启后不回调 |
| **13** | **奖品耗尽** | 所有限量级耗尽 → 按策略回退到"谢谢参与"（或 `kRefuse` 时拒绝开奖并报 `out_of_prizes`），**机器不冷场、不崩** |
| **14** | **文案可编辑** | 改商家名/标题/票头票尾 → 预览与实打小票**逐字一致**；未知占位符有告警；超宽行安全折行且**中文不错位** |
| **15** | **Logo 可上传** | 上传 PNG → 打印图形清晰；上传坏数据返回 `400`；**logo 异常时票仍能正常打出**（降级不阻断） |
| **16** | **30 页映射** | `PageMapping` 覆盖全部 30 页、行程值**两两互不相同**、**且相邻差 ≥ `min_page_gap_dmx`**、且都落在 `[stroke_min_dmx, stroke_max_dmx]`；否则 `kBoot` 拒绝进入 `kIdle`（D21） |
| **17** | **改页数保护** | 奖池含 `page >= 新页数` 时改页数被拒，报错含具体页码 |

---

## 不建议做的事（避免过度设计）

- ❌ 不引入 `PagingController` / 巡页 / 定时 / 计数器 / 时钟模式（非目标，且违反 SRP）
- ❌ 不接厂商预编译 `.a`（Zbb 风险 + 二进制依赖，设计 D6）
- ❌ 不做 OTA 门户（**但保留 OTA 双槽分区**，D10）
- ❌ 不做语音/音频/蓝牙/MCP/License（非目标）
- ❌ 不做打印机缺纸/过热状态轮询（R8，v1 明确不做）
- ❌ 不做多机/矩阵（`cell_count` 恒为 1）
