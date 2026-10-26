# 评审 38：控制台逐键输入被切成多条命令（「未知命令」刷屏）

日期：2026-10-26 · 结果：**通过**

## 现象

用户用 `python3 -m serial.tools.miniterm` 手工敲 `sndwin`，每敲一个键都回一行
「未知命令（help 查看）」；而脚本一次性写整行却能正常执行。

## 根因

USB-Serial-JTAG 的 VFS 是**非阻塞**的：`fgets()` 读到一个字符后，下一次
`read()` 返回 0 被 stdio 当作 **EOF** → `fgets` 带着半个单词返回 →
控制台把它当成一条完整命令分发。逐键输入的每个字符都触发一次，于是刷屏
「未知命令」。

## 修法

1. `console_cmd.cc`：`ConsoleTask` 改为**自己累积字符**（`fgetc` 循环），
   只有收到 `\r`/`\n` 才调用 `ProcessLine()`；无数据/EOF 只让出 CPU；
   支持退格回显、超长行丢弃。分发主体抽成 `ProcessLine()`。
2. `ProcessLine()` 先修剪行尾 CR/LF/空格——兼容"整行写入 `cmd\n`"与逐键两种输入。
3. 新增 `scripts/serial_cmd.py`：安全发一条控制台命令（打开端口后立刻
   `dtr=False/rts=False` + esptool 硬复位回正常启动 + 首行冲刷/失败重试）。
   复位后第一行可能被启动残留污染（实测首条整行命令会回未知命令），已处理。

## 验证证据

- 逐键输入（每键 0.1~0.15s，再回车）：`中奖音效播放中` +
  `SpeakerSvc: 中奖音效播放完成` ✅
- 整行写入：`sndwin\r` / `sndwin\r\n` 正常；`sndwin\n` 在"复位后第一条"会被
  残留字节污染，helper 的空行冲刷 + 重试已消除该偶发。
- `python3 scripts/serial_cmd.py sndwin`：冷启动后一次成功。
- 编译通过；片上单测未受影响（287 基线）。

## 附带教训

- 调试串口时**不要在 HTTP/浏览器测试同时进行**：pyserial/screen 打开端口会拉
  DTR/RTS，可能把芯片复位或带进下载模式（本次一度出现设备不可达、端口被占用，
  系用户残留的 miniterm 进程 + 反复打开端口所致）。
- 若必须同时用，先 `esptool ... run` 硬复位确认应用在跑，再继续 HTTP 测试。
