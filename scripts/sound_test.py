#!/usr/bin/env python3
"""sound_test.py — 喇叭/音效自动化验收（2026-10-25，评审 36）

用法：python3 scripts/sound_test.py [设备IP] [串口]
  设备IP：可选；提供则同时做 HTTP 探活（GET /api/status，只读不出纸）
  串口：默认 /dev/cu.usbmodem144201（可用 LUCKY_PORT 覆盖）

验证矩阵（全部不触发抽奖/翻页/出纸）：
  1. sndplay  欢迎音   → 等 "欢迎音播放完成"
  2. sndtest  正弦诊断音 → 等 "正弦测试音完成"
  3. sndwin   中奖音效  → 等 "中奖音效播放完成"
  4. （可选）sndvol 音量往返 → 确认设置生效日志

通过标准：三条命令的"播放完成"日志全部出现（日志是自动化的"人耳替身"，
最终音质仍以人耳为准——纸面级检查）。
"""
import os
import subprocess
import sys
import time

import serial

PORT = sys.argv[2] if len(sys.argv) > 2 else os.environ.get('LUCKY_PORT', '/dev/cu.usbmodem144201')
IP = sys.argv[1] if len(sys.argv) > 1 else None
BAUD = 115200

CASES = [
    ("sndplay", "欢迎音", "欢迎音播放完成"),
    ("sndtest", "正弦诊断音", "正弦测试音完成"),
    ("sndwin", "中奖音效", "中奖音效播放完成"),
    ("sndlose", "落空音效", "落空音效播放完成"),
]


def open_serial():
    ser = serial.Serial(PORT, BAUD, timeout=1)
    # 勿拉 EN/GPIO0（会把芯片带进下载模式，AGENTS §1.1）
    ser.dtr = False
    ser.rts = False
    ser.reset_input_buffer()
    return ser


def run_case(ser, cmd, name, done_mark):
    ser.reset_input_buffer()
    ser.write((cmd + '\n').encode())
    deadline = time.time() + 10
    while time.time() < deadline:
        chunk = ser.read(4096).decode('utf-8', 'replace')
        if done_mark in chunk:
            return True, chunk
        time.sleep(0.1)
    return False, ''


def main():
    ok = True

    print(f"[1/3] 串口音效验收（{PORT}）...")
    try:
        ser = open_serial()
    except Exception as e:
        print(f"FAIL: 打开串口失败：{e}")
        sys.exit(1)
    for cmd, name, mark in CASES:
        passed, chunk = run_case(ser, cmd, name, mark)
        tag = "PASS" if passed else "FAIL"
        print(f"  [{tag}] {cmd:<8} {name}")
        if not passed:
            ok = False
            print("    （未捕获完成日志；原始输出尾部）", chunk[-200:])
    ser.close()

    if IP:
        print(f"[2/3] HTTP 探活（{IP}，只读）...")
        import urllib.request
        try:
            with urllib.request.urlopen(f"http://{IP}/api/status", timeout=5) as r:
                print(f"  [PASS] GET /api/status → {r.status}")
        except Exception as e:
            print(f"  [FAIL] GET /api/status：{e}")
            ok = False
    else:
        print("[2/3] 未提供设备 IP，跳过 HTTP 探活")

    print(f"[3/3] 结果：{'全部通过' if ok else '存在失败'}")
    print("注：日志完成 ≠ 音质合格；音质以人耳复核为准（纸面级检查，AGENTS 规则五）")
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
