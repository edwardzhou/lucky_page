# 设计：抽奖按钮（GPIO8）低电平触发 + 去抖 + 1 秒限流

> 日期：2026-10-11　|　状态：已评审（用户要求："GPIO8 抽奖按钮 确认也是低电平触发，
> 并且加有抖动处理，一秒内只能触发一次"）

## 1. 现状与问题

| 项 | 现状 |
|---|---|
| 触发电平 | `Button(BUTTON_MODULE_GPIO, 1)` = **高电平触发**（active_high）；用户确认实际接线是**低电平触发**（上拉 + 按下接地） |
| 去抖 | **没有**：`esp_button` 4.2.1 的 `button_config_t` 只有 `long_press_time` / `short_press_time`，**没有可配的去抖参数**（内部 10 ms 轮询不等于去抖） |
| 触发限流 | **没有**：`LotteryController::Trigger()` 只有状态白名单（非 idle 拒绝），机械抖动/连击可能造成短时间多次触发尝试 |

## 2. 设计

### 2.1 电平与参数（板级常量，`boards/esp32s3_lucky/config.h`）

```c
#define BUTTON_MODULE_GPIO           GPIO_NUM_8
#define BUTTON_MODULE_ACTIVE_LEVEL   0      // 0 = 按下为低（上拉 + 接地）—— 用户确认的接线
#define BUTTON_MODULE_DEBOUNCE_MS    30     // 软件去抖窗口：窗口内的抖动全部吞掉
#define BUTTON_MODULE_COOLDOWN_MS    1000   // 触发限流：1 秒内只接受一次（用户要求）
```
- `static_assert`：电平 ∈ {0,1}、去抖 < 限流、与恢复按钮(GPIO5)/其它功能脚不冲突。
- **接线自检**：新增串口 `btbtn` 打印引脚号/触发电平/**当前电平**（上拉未按下应为 1）
  + 闸门统计（接受/去抖吞掉/限流忽略次数 + 上次接受时间）→ 用户按一下就能确认电平与去抖是否生效。

### 2.2 纯逻辑闸门 `ButtonGate`（`main/button_gate.{h,cc}`，可上单测宿主）

```cpp
class ButtonGate {
    ButtonGate(int64_t debounce_ms, int64_t cooldown_ms);
    bool OnPress(int64_t now_ms);   // 由"按下事件"驱动；true = 接受本次触发
    int accepted()/rejected_debounce()/rejected_cooldown();  int64_t last_trigger_ms();
};
```
判定顺序：
1. **去抖**：距上次事件（含被吞的）< `debounce_ms` → 吞掉，并把时间戳顺延（经典"等安静"语义）；
2. **限流**：距上次**被接受**的触发 < `cooldown_ms` → 忽略；
3. 否则接受并计数。

不用 `INT64_MIN` 做哨兵（`now - INT64_MIN` 会溢出），改用 `has_event_/has_trigger_` 布尔。

### 2.3 接线（`application.cc`）

- 抽奖（`OnPress` → `Trigger()`）与打自检页（`OnLongPress` → `PrintTestTicket()`）
  **各用一个 `ButtonGate` 实例**（互不饿死：长按不会因抽奖刚触发而被限流挡掉）；
- 被拒时打 `ESP_LOGD`（去抖/限流原因），便于现场排障；
- 回调仍在 esp_timer 上下文 → 保持 `Schedule()` 投递主线程不变。

## 3. 验证

| 层级 | 内容 |
|---|---|
| 单测 | `ButtonGate`：首按接受；30 ms 内连按全被去抖（且不计入限流）；999 ms 拒绝、1000 ms 接受；抖动串不产生多次接受；统计计数正确 |
| 编译期 | 引脚/电平/去抖/限流静态断言 |
| 真机 | 串口 `btbtn`：引脚 8、active_low、**当前电平 = 1（未按下）**、阈值 30/1000；按一下按钮后计数 +1（用户可据此确认接线，**不需要出票**） |
| 回归 | 编译 + 片上单测 + `api_verify`（默认不出纸）+ `web_smoke`（冒烟本就不按物理按钮） |

## 4. 风险

| 风险 | 对策 |
|---|---|
| 若实际接线是"高电平触发"，改成低电平后按钮将失效 | `btbtn` 会打印未按下时的电平：若为 0（异常）说明接线与假设不符 → 据实回报，并把电平改回常量 1；AGENTS 记录该判据 |
| 长按同时会先触发一次抽奖（既有行为：`OnPress` 与 `OnLongPress` 都注册了） | 本次不改该交互（不在需求内），但在评审里明确记录；限流只作用于各自闸门 |
