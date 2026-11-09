#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mqtt_plot.py —— 通过 MQTT 接收 ESP32 振动数据并实时画频谱图

固件端（project1/main/main.c）发布到主题 esp32/vibration 的 JSON 包格式：
{
  "rate_hz": 100,          # 采样率
  "first_seq": 0,          # 首样本序号
  "first_t_us": 123456,    # 首样本时间戳（微秒）
  "samples": [             # 每包 50 个采样点（100 Hz，每 0.5 s 一包）
    {"t":..., "ax":..., "ay":..., "az":..., "gx":..., "gy":..., "gz":...},
    ...
  ]
}

本脚本做的事：
  1. 订阅 MQTT 主题，把各包按时间戳拼成连续采样流（检测到断档自动重置）
  2. 上图：最近约 10 秒的加速度模长波形（含 ~1g 重力分量）
  3. 下图：滑动窗口 FFT 频谱（去掉重力直流分量，红色圆点标记主频）

用法（在 game 目录下）：
    .venv\\Scripts\\python.exe mqtt_plot.py               # 收真实数据（默认连本机 127.0.0.1）
    .venv\\Scripts\\python.exe mqtt_plot.py --demo        # 自检：内置 12.5Hz 正弦振动，无需硬件
    .venv\\Scripts\\python.exe mqtt_plot.py --broker 192.168.1.102   # 连别的 broker

说明：
  - 默认 broker 是 127.0.0.1：脚本和 Mosquitto 跑在同一台电脑上，本机回环最稳，
    不受网卡 IP 变化、防火墙、校园网隔离的影响。
  - ESP32 端连的是电脑的局域网 IP（固件 mqtt.c 里的 MQTT_BROKER_URI）。
  - 依赖：paho-mqtt、numpy、matplotlib（.venv 里都已装好）。
"""

import argparse
import json
import random
import sys
import threading
import time
from collections import deque

# 依赖缺失时给出友好提示（防止双击 .py 闪退看不到报错）
try:
    import numpy as np
    import paho.mqtt.client as mqtt
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation
except ImportError as e:
    print("缺少依赖包:", e)
    print("请用项目虚拟环境运行：")
    print("    .\\.venv\\Scripts\\python.exe mqtt_plot.py")
    input("按回车键退出 ...")
    sys.exit(1)

# 输出重定向（管道/后台）时也按行刷新，保证控制台能实时看到日志
try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass

# 用微软雅黑/黑体渲染窗口里的中文，避免显示成方框
plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

# ======================= 可调配置 =======================
DEFAULT_BROKER = "127.0.0.1"       # 本机 Mosquitto；也可用 --broker 覆盖
DEFAULT_PORT   = 1883
DEFAULT_TOPIC  = "esp32/vibration"

FFT_WINDOW     = 1024   # FFT 用的样本数（@100Hz 约 10 s，频率分辨率 ~0.1 Hz）
BUFFER_MAX     = 4096   # 环形缓冲上限（@100Hz 约 40 s），防止内存无限增长
GAP_RESET_US   = 2_000_000   # 时间戳断档超过 2 秒才重置缓冲区。
                             # 注意：固件每发完一包要花几十 ms（JSON 打包+WiFi 发送），
                             # 包与包之间有 46~90 ms 的正常间隔，不能当成断档。

# ======================= 全局状态 =======================
g_lock         = threading.Lock()
g_buf          = deque(maxlen=BUFFER_MAX)   # 元素: (t_us, ax, ay, az, gx, gy, gz)
g_last_t_us    = None
g_rate_hz      = 100.0
g_batch_count  = 0
g_sample_count = 0
g_connected    = False
g_demo         = False
g_drawn_key    = None     # 上次绘制的数据指纹 (样本数, 最后时间戳)，无新数据时跳过重绘


# ======================= MQTT 回调 =======================
def on_connect(client, userdata, flags, reason_code, properties):
    global g_connected
    if not reason_code.is_failure:
        g_connected = True
        client.subscribe(userdata["topic"], qos=0)
        print(f"[+] 已连接 broker，订阅主题 {userdata['topic']}")
    else:
        g_connected = False
        print(f"[!] 连接被拒绝: {reason_code}")


def on_disconnect(client, userdata, disconnect_flags, reason_code, properties):
    global g_connected
    g_connected = False
    print(f"[-] 与 broker 断开 (code={reason_code})，paho 会自动重连 ...")


def on_message(client, userdata, msg):
    global g_last_t_us, g_rate_hz, g_batch_count, g_sample_count

    try:
        pkt = json.loads(msg.payload.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        print("[!] 收到无法解析的 JSON 消息，跳过")
        return

    rate = float(pkt.get("rate_hz") or 100.0)
    if rate > 0:
        g_rate_hz = rate

    samples = pkt.get("samples")
    if not isinstance(samples, list) or len(samples) == 0:
        return

    added = 0
    with g_lock:
        for s in samples:
            try:
                t_us = float(s["t"])
                ax, ay, az = float(s["ax"]), float(s["ay"]), float(s["az"])
                gx, gy, gz = float(s["gx"]), float(s["gy"]), float(s["gz"])
            except (KeyError, TypeError, ValueError):
                continue

            # 断档检测：时间戳回退或间隔超过 2 秒，说明丢包严重/设备重启，
            # 清空缓冲区重新开始，避免把断档混进 FFT 产生假谱峰。
            # 包与包之间几十 ms 的正常间隔不算断档。
            if g_last_t_us is not None:
                gap_us = t_us - g_last_t_us
                if gap_us <= 0 or gap_us > GAP_RESET_US:
                    print(f"[i] 采样流断档 (gap={gap_us:.0f} us)，重置缓冲区")
                    g_buf.clear()

            g_buf.append((t_us, ax, ay, az, gx, gy, gz))
            g_last_t_us = t_us
            added += 1

        g_batch_count += 1
        g_sample_count += added

    if g_batch_count == 1:
        print(f"[i] 首包解析成功: {added} 个采样点, 例 az={g_buf[-1][3]:.3f} g")
    elif g_batch_count % 20 == 0:
        print(f"[i] 已收 {g_batch_count} 包 / {g_sample_count} 采样点 (rate={g_rate_hz:.0f} Hz)")


# ======================= 自检模式：内置正弦振动 =======================
def demo_generator():
    """模拟 100Hz 采样：12.5Hz 主振 + 25Hz 二次谐波 + 小噪声，验证绘图管线。"""
    global g_last_t_us, g_sample_count, g_batch_count
    print("[*] DEMO 模式：内置 12.5 Hz 正弦振动（应看到 12.5 Hz 主频峰）")
    t_us = 0
    while True:
        t_s = t_us * 1e-6
        ax = 0.30 * np.sin(2 * np.pi * 12.5 * t_s) + 0.08 * np.sin(2 * np.pi * 25.0 * t_s)
        ay = 0.10 * np.sin(2 * np.pi * 7.0 * t_s)
        az = 1.0 + 0.03 * np.sin(2 * np.pi * 12.5 * t_s)
        with g_lock:
            g_buf.append((float(t_us), ax, ay, az, 0.0, 0.0, 0.0))
            g_last_t_us = float(t_us)
            g_sample_count += 1
            g_batch_count = g_sample_count // 50
        t_us += 10000
        time.sleep(0.01)


# ======================= 信号处理 =======================
def fft_magnitude(t_us, ax, ay, az):
    """对加速度模长做单边 FFT，返回 (频率轴[Hz], 幅度[g])，已去掉重力直流分量。"""
    t_us = np.asarray(t_us, dtype=np.float64)
    n = t_us.size
    if n < 8:
        return np.array([]), np.array([])

    mag = np.sqrt(ax * ax + ay * ay + az * az)
    mag = mag - np.mean(mag)          # 去掉重力引起的直流分量

    win = np.hanning(n)
    sp = np.abs(np.fft.rfft(mag * win))
    sp *= 2.0 / np.sum(win)           # 单边幅度校正（单位 g）

    dt = np.median(np.diff(t_us)) * 1e-6
    if not np.isfinite(dt) or dt <= 0:
        dt = 1.0 / max(g_rate_hz, 1e-3)
    freq = np.fft.rfftfreq(n, d=dt)
    return freq, sp


# ======================= 绘图 =======================
fig, (ax_t, ax_f) = plt.subplots(
    2, 1, figsize=(11, 7), gridspec_kw={"height_ratios": [1, 1]})
fig.canvas.manager.set_window_title("ESP32 Vibration Monitor")

line_t,  = ax_t.plot([], [], lw=0.8, color="#1f77b4")
line_f,  = ax_f.plot([], [], lw=0.8, color="#2ca02c")
peak_pt, = ax_f.plot([], [], "ro", ms=6)

ax_t.set_title("Acceleration magnitude (time domain)")
ax_t.set_ylabel("Accel (g)")
ax_t.grid(True, alpha=0.3)

ax_f.set_title("FFT spectrum (DC removed)")
ax_f.set_xlabel("Frequency (Hz)")
ax_f.set_ylabel("Amplitude (g)")
ax_f.grid(True, alpha=0.3)

status_txt = ax_t.text(0.02, 0.95, "", transform=ax_t.transAxes,
                       va="top", fontsize=9)
wait_txt = ax_f.text(0.5, 0.5,
                     "WAITING FOR DATA ...\n等待 ESP32 数据（约 10 秒攒满频谱窗口）",
                     ha="center", va="center", fontsize=13,
                     color="#666666", transform=ax_f.transAxes)


def animate(frame):
    global g_drawn_key

    with g_lock:
        if len(g_buf) < 64:
            mode = "DEMO" if g_demo else "LIVE"
            status_txt.set_text(
                f"Mode:{mode}  Samples:{len(g_buf):<4} "
                f"MQTT:{'UP' if (g_connected or g_demo) else 'DOWN':<5} "
                f"Rate:{g_rate_hz:.0f} Hz  等待数据 ...")
            return
        # 数据指纹：没有新数据到达就直接返回。
        # 固件每 0.5s 才来一包，之前每秒无意义重绘 10 次是卡顿主因；
        # 现在实际重绘频率 ≈ 数据到达频率（~2 Hz），窗口非常流畅。
        key = (len(g_buf), g_buf[-1][0])
        if key == g_drawn_key:
            return
        g_drawn_key = key
        data = np.asarray(list(g_buf), dtype=np.float64)   # 快照

    t_us = data[:, 0]
    ax, ay, az = data[:, 1], data[:, 2], data[:, 3]

    t = (t_us - t_us[0]) * 1e-6
    mag = np.sqrt(ax * ax + ay * ay + az * az)

    # ---- 时域：最近 10 秒 ----
    line_t.set_data(t, mag)
    if t[-1] > 0:
        ax_t.set_xlim(max(0.0, t[-1] - 10.0), t[-1] + 0.2)
    ax_t.set_ylim(0.0, max(0.2, 1.15 * float(np.max(mag))))

    # ---- 频域：最近 FFT_WINDOW 个点 ----
    w = min(FFT_WINDOW, len(t_us))
    freq, amp = fft_magnitude(t_us[-w:], ax[-w:], ay[-w:], az[-w:])
    line_f.set_data(freq, amp)

    nyq = g_rate_hz / 2.0
    ax_f.set_xlim(0, nyq)
    peak_hz, peak_amp = 0.0, 0.0
    if amp.size > 1:
        mask = (freq >= 1.0) & (freq <= nyq - 0.5)     # 忽略直流和奈奎斯特边缘
        if np.any(mask):
            idx = np.argmax(amp[mask])
            peak_hz = float(freq[mask][idx])
            peak_amp = float(amp[mask][idx])
            peak_pt.set_data([peak_hz], [peak_amp])
    ax_f.set_ylim(0.0, max(1e-3, 1.2 * float(np.max(amp)) if amp.size else 0.1))

    wait_txt.set_text("")
    mode = "DEMO" if g_demo else "LIVE"
    status_txt.set_text(
        f"Mode:{mode}  Batches:{g_batch_count:<6} Samples:{g_sample_count:<7} "
        f"MQTT:{'UP' if (g_connected or g_demo) else 'DOWN':<5} Rate:{g_rate_hz:.0f} Hz  "
        f"Peak:{peak_hz:5.2f} Hz ({peak_amp * 1000:6.1f} mg)")


# ======================= 主程序 =======================
def main():
    global g_demo

    parser = argparse.ArgumentParser(description="ESP32 振动数据 MQTT 接收 + 频谱显示")
    parser.add_argument("--broker", default=DEFAULT_BROKER, help="MQTT broker IP（默认 %(default)s，本机回环）")
    parser.add_argument("--port",   type=int, default=DEFAULT_PORT, help="MQTT 端口（默认 %(default)s）")
    parser.add_argument("--topic",  default=DEFAULT_TOPIC, help="订阅主题（默认 %(default)s）")
    parser.add_argument("--demo",   action="store_true", help="自检模式：内置正弦振动，不联网")
    args = parser.parse_args()

    g_demo = args.demo

    if args.demo:
        threading.Thread(target=demo_generator, daemon=True).start()
    else:
        # client_id 加随机后缀：开多个窗口互不干扰（相同 id 会把对方踢下线）
        client_id = f"pc_vibration_monitor_{random.randint(1000, 9999)}"
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        client.user_data_set({"topic": args.topic})
        client.on_connect    = on_connect
        client.on_disconnect = on_disconnect
        client.on_message    = on_message
        client.reconnect_delay_set(min_delay=1, max_delay=10)

        print(f"[*] 启动: broker={args.broker}:{args.port}  topic={args.topic}")
        print(f"[*] 等 ESP32 上电发数据（每 0.5 s 一包，攒够 ~{FFT_WINDOW} 点后出频谱）")
        # connect_async 不阻塞、失败自动重试；loop_start 在后台线程跑网络
        client.connect_async(args.broker, args.port, keepalive=60)
        client.loop_start()

    # 固件每 0.5s 才来一包数据，interval=250ms 足够跟手；
    # 配合 animate 里“无新数据直接返回”，实际重绘 ~2 Hz，窗口丝滑不卡
    ani = FuncAnimation(fig, animate, interval=250, blit=False, cache_frame_data=False)
    try:
        plt.show()
    except KeyboardInterrupt:
        pass
    finally:
        if not args.demo:
            client.loop_stop()
            client.disconnect()
        print("[*] 退出")


if __name__ == "__main__":
    main()
