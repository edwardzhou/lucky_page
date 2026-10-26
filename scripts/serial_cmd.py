#!/usr/bin/env python3
"""serial_cmd.py — 向设备控制台发一条命令（安全版）

用法：
    python3 scripts/serial_cmd.py sndwin
    python3 scripts/serial_cmd.py "sndtest 440 1000" --wait 4

为什么不是直接 miniterm/screen：
  1. 打开 /dev/cu.* 会拉 DTR/RTS，芯片可能被带进下载模式或复位；
     本脚本打开后立刻 dtr=False/rts=False，并用 esptool hard-reset 把芯片
     拉回正常启动，等控制台就绪后再发命令。
  2. ESP-IDF 的 USB-Serial-JTAG VFS 是非阻塞的：逐键输入时半个单词会被当成
     一条命令（历史现象：每敲一个键都回"未知命令"）。固件已改为"累积到换行
     才执行"，两种输入方式都兼容；脚本仍按整行发送最稳。

退出码：0 成功（有输出）、1 串口/复位失败。
"""
import argparse
import os
import subprocess
import sys
import time

import serial

DEFAULT_PORT = os.environ.get("LUCKY_PORT", "/dev/cu.usbmodem144201")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", help="要发送的整行命令，如 sndwin")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--boot-wait", type=float, default=6.0, help="复位后等待启动的秒数")
    ap.add_argument("--wait", type=float, default=3.0, help="发送后读取响应的秒数")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, 115200, timeout=1)
    except Exception as e:
        print(f"打开串口失败：{e}", file=sys.stderr)
        return 1
    ser.dtr = False
    ser.rts = False
    # 打开端口本身可能已把芯片带进下载模式 → 硬复位回正常启动
    subprocess.run(["python3", "-m", "esptool", "--chip", "esp32s3", "-p", args.port,
                    "--before", "no-reset", "--after", "hard-reset", "run"],
                   capture_output=True)
    time.sleep(args.boot_wait)
    # 复位后第一行可能被启动残留字节污染（实测首个整行命令会回"未知命令"）：
    # 先发一个空行冲刷，再发命令；若仍判未知命令则重试一次
    out = ""
    for attempt in range(2):
        ser.reset_input_buffer()
        ser.write(b"\n")
        time.sleep(0.2)
        ser.write((args.command + "\n").encode())
        time.sleep(args.wait)
        out = ser.read(65536).decode("utf-8", "replace")
        if "未知命令" not in out:
            break
    ser.close()
    # 只打印设备响应里像命令输出的行（过滤启动/日志噪声）
    lines = [l for l in out.splitlines() if l.strip()]
    for line in lines[-30:]:
        print(line)
    return 0 if lines else 1


if __name__ == "__main__":
    sys.exit(main())
