#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""read_serial.py —— 抓取串口日志（复位后读 N 秒）

用法（用 IDF 自带的 python，它带 pyserial）：
    C:\\Espressif\\tools\\python\\v5.3.5\\venv\\Scripts\\python.exe read_serial.py COM10 15 --reset
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("seconds", nargs="?", type=float, default=12.0)
    ap.add_argument("--reset", action="store_true", help="先拉 RTS 复位再读")
    ap.add_argument("--baud", type=int, default=115200)
    a = ap.parse_args()

    s = serial.Serial(a.port, a.baud, timeout=0.3)
    if a.reset:
        s.setRTS(True)
        time.sleep(0.2)
        s.setRTS(False)

    end = time.time() + a.seconds
    buf = bytearray()
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
    s.close()
    sys.stdout.write(buf.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
