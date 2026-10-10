#!/usr/bin/env python3
"""lucky-page 单测运行器（片上 unity：烧录 → 复位 → 串口读结果）"""
import serial, time, sys, subprocess, os

PORT = os.environ.get('LUCKY_PORT', '/dev/cu.usbmodem1433101')  # USB-Serial-JTAG（本板唯一口）
BAUD = 115200
DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(DIR, 'build')

# ESP-IDF v6.1.0 环境（与 scripts/build.sh 保持一致；v6.1 的 cmake/ninja 不在 venv 里，须显式进 PATH）
ENV = (
    'export IDF_TOOLS_PATH="$HOME/.espressif/tools"; '
    'export IDF_PATH="$HOME/.espressif/v6.1/esp-idf"; '
    'export IDF_PYTHON_ENV_PATH="$HOME/.espressif/tools/python/v6.1/venv"; '
    'export ESP_IDF_VERSION="6.1"; '  # v6.1 组件管理器要求（官方 activate.py 会设置）
    'export PATH="$IDF_TOOLS_PATH/cmake/4.0.3/CMake.app/Contents/bin:'
    '$IDF_TOOLS_PATH/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:'
    '$IDF_TOOLS_PATH/ninja/1.12.1:'
    '$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$PATH"'
)

print(f"[1/3] Building & flashing ({PORT})...")
sys.stdout.flush()
r = subprocess.run(['bash', '-c', f'{ENV}; cd {DIR} && idf.py -B build -p {PORT} flash 2>&1'],
                   capture_output=True, text=True, timeout=600)
if 'Done' not in r.stdout and 'Hash of data verified' not in r.stdout:
    print("FAIL:", r.stdout[-500:]); sys.exit(1)
print("OK")

print("[2/3] Reset board + capture...")
sys.stdout.flush()
time.sleep(1)
ser = serial.Serial(PORT, BAUD, timeout=1)
ser.reset_input_buffer()
# USB-Serial-JTAG：idf.py flash 已 hard reset，直接采集（勿手动拉 DTR/RTS，可能进下载模式）

start = time.time()
output = b''
while time.time() - start < 180:
    chunk = ser.read(8192)
    if chunk:
        output += chunk
        # unity 汇总行出现即整轮结束，提前退出（防重启循环重复计数）
        if b'Tests ran' in output or b'unity_result' in output:
            time.sleep(0.5)
            output += ser.read(8192)
            break
    elif output and time.time() - start > 30:
        break
ser.close()

print("[3/3] Results:")
print("=" * 60)
text = output.decode('utf-8', errors='replace')
for line in text.split('\n'):
    s = line.strip()
    if any(k in s for k in [':PASS', ':FAIL', 'Running ', 'Tests ', 'Test ran']):
        print(s)

p = text.count(':PASS')
f = text.count(':FAIL')
print("=" * 60)
print(f"{p} PASSED, {f} FAILED")

with open('/tmp/lucky_test_results.txt', 'w') as fh:
    fh.write(text)

sys.exit(1 if f > 0 else 0)
