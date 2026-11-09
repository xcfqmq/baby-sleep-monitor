#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
collect_sleep_data.py —— 采集多模态融合训练样本（按键打标）

用法（先在电脑上跑起 broker，ESP32 已上电并发布数据）：
    python collect_sleep_data.py [broker_ip]

按键：
    1~5 = 切换状态标签（切换后有 5 秒就位缓冲，缓冲期样本被丢弃）
    s   = 开始 / 暂停 采集
    q   = 退出（数据已保存）

注意：
    - 脚本启动后处于「待机」状态，不会自动采集；按 1~5 或 s 才开始。
    - 每次运行会生成一个带时间戳的新 CSV，不会覆盖之前的数据。
"""
import csv
import json
import os
import sys
import time

import msvcrt                       # Windows 无回车按键
import paho.mqtt.client as mqtt

BROKER = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = 1883
TOPIC_FUSION = "esp32/fusion"
TOPIC_RADAR = "esp32/radar"

# 数据统一存到 game\data\ 下，每次运行一个带时间戳的新文件
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(BASE_DIR, "data")
os.makedirs(DATA_DIR, exist_ok=True)

FIELDS = ["ts", "state", "breath_rate", "heart_rate",
          "motion", "brightness", "present", "distance_cm",
          "occluded", "box_x", "box_y", "box_w", "box_h"]

STATES = {
    b"1": "off_bed",
    b"2": "awake",
    b"3": "light_sleep",
    b"4": "deep_sleep",
    b"5": "abnormal",
}

SETTLE_SECONDS = 5                   # 按键后丢弃的过渡秒数（人还没就位，别采）
current_state = "off_bed"
state_count = 0
settle_until = 0.0
recording = False                    # 是否正在采集（启动时待机）
latest_radar = {}
writer = None
fobj = None
csv_file = None


def on_connect(client, userdata, flags, rc):
    print(f"[mqtt] 已连接 {BROKER} (rc={rc})")
    client.subscribe(TOPIC_FUSION)
    client.subscribe(TOPIC_RADAR)


def on_message(client, userdata, msg):
    global latest_radar, state_count
    try:
        data = json.loads(msg.payload.decode())
    except (ValueError, UnicodeDecodeError):
        return

    if msg.topic == TOPIC_RADAR:
        latest_radar = data
        return

    if msg.topic == TOPIC_FUSION:
        if not recording:
            return                     # 待机，不采集
        if time.time() < settle_until:
            remain = settle_until - time.time()
            print(f"  ... 就位中({current_state})，还剩 {remain:.0f}s 后开始采集")
            return

        row = {
            "ts": round(time.time(), 2),
            "state": current_state,
            "breath_rate": data.get("breath_rate", 0.0),
            "heart_rate": data.get("heart_rate", 0.0),
            "motion": data.get("motion", 0),
            "brightness": data.get("brightness", 0),
            "present": 1 if data.get("present") else 0,
            "distance_cm": latest_radar.get("distance_cm", 0.0),
            "occluded": 1 if data.get("occluded") else 0,
            "box_x": data.get("box_x", 0),
            "box_y": data.get("box_y", 0),
            "box_w": data.get("box_w", 0),
            "box_h": data.get("box_h", 0),
        }
        state_count += 1
        writer.writerow(row)
        fobj.flush()
        print(f"  [{row['ts']}] {row['state']:<12} #{state_count:<4} "
              f"呼吸={row['breath_rate']:>5.1f} 心率={row['heart_rate']:>5.1f} "
              f"运动={row['motion']:>4} 亮度={row['brightness']:>4} "
              f"距离={row['distance_cm']:>5.1f}cm")


def main():
    global current_state, state_count, settle_until, recording

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER, PORT, 60)
    client.loop_start()

    print("=" * 60)
    print(" 睡眠状态采集（按键打标）")
    print("   1=off_bed  2=awake  3=light_sleep  4=deep_sleep  5=abnormal")
    print("   s=开始/暂停采集    q=退出（自动保存）")
    print(f"   按 1~5 或 s 才开始采集（每次切换后缓冲 {SETTLE_SECONDS} 秒）")
    print(f"   样本写入 {csv_file}")
    print("=" * 60)

    while True:
        if msvcrt.kbhit():
            ch = msvcrt.getch()
            if ch in STATES:
                current_state = STATES[ch]
                state_count = 0
                settle_until = time.time() + SETTLE_SECONDS
                if not recording:
                    recording = True
                    print(f"\n  >>> 开始采集: {current_state}（{SETTLE_SECONDS} 秒后就位）<<<\n")
                else:
                    print(f"\n  >>> 切换到: {current_state}（{SETTLE_SECONDS} 秒后就位）<<<\n")
            elif ch in (b"s", b"S"):
                recording = not recording
                if recording:
                    settle_until = time.time() + SETTLE_SECONDS
                    print(f"\n  >>> 开始采集: {current_state}（{SETTLE_SECONDS} 秒后就位）<<<\n")
                else:
                    print("\n  >>> 暂停采集（按 s 继续）<<<\n")
            elif ch in (b"q", b"Q", b"\x1b"):
                break
        time.sleep(0.1)

    client.loop_stop()
    client.disconnect()
    print(f"已退出，数据保存在 {csv_file}")


if __name__ == "__main__":
    # 每次运行一个新文件（带时间戳），不会覆盖之前的采集
    csv_file = os.path.join(DATA_DIR,
                            "sleep_" + time.strftime("%Y%m%d_%H%M%S") + ".csv")
    fobj = open(csv_file, "w", newline="", encoding="utf-8")
    writer = csv.DictWriter(fobj, fieldnames=FIELDS)
    writer.writeheader()
    try:
        main()
    finally:
        fobj.close()
