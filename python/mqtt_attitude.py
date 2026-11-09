#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mqtt_attitude.py —— MQTT 接收 ESP32/BMI270 数据，实时显示板子姿态（roll/pitch/yaw）

原理：
  - 加速度计测重力方向 → 算 roll/pitch 的绝对参考（振动时噪声大、动态响应差）
  - 陀螺仪积分 → 动态响应好，但会慢慢漂移
  - 互补滤波融合两者：angle = α·(angle + gyro·dt) + (1-α)·accel_angle
  - yaw（偏航）没有磁力计参考，只能靠陀螺仪积分 → 必然漂移，按键盘 r 归零

用法（在 game 目录下）：
    .venv\\Scripts\\python.exe mqtt_attitude.py          # 真实数据（默认连本机 127.0.0.1）
    .venv\\Scripts\\python.exe mqtt_attitude.py --demo   # 自检：模拟板子摆动，无需硬件

按键：
    r —— yaw 归零
    q —— 退出

坐标约定：板子平放、z 轴朝上、x 轴指向前方（机体坐标系，x=前/红、y=右/绿、z=上/蓝）。
如果你的安装方向不同，改 update_filter() 里加速度计姿态的两行公式即可。

流畅性设计：
  - animate 只在有新数据或姿态仍在过渡时才重绘（避免之前每 100ms 全图重绘的卡顿）
  - 显示角度向滤波结果做指数平滑过渡，数据成包到达（每 0.5s 一包）时画面不跳变
"""

import argparse
import json
import math
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
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
except ImportError as e:
    print("缺少依赖包:", e)
    print("请用项目虚拟环境运行：")
    print("    .\\.venv\\Scripts\\python.exe mqtt_attitude.py")
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

ALPHA          = 0.98     # 互补滤波系数（@100Hz，时间常数 ≈ 0.5s）
BIAS_SAMPLES   = 100      # 用前 100 个样本估计陀螺仪零偏（上电后板子先放稳）
GYRO_TRUST_G   = 0.35     # 加速度模长偏离 1g 超过此值时，降低加速度计权重（剧烈运动时重力参考不可信）
EASE_K         = 0.35     # 显示角度每帧向目标过渡的比例（画面平滑用，不影响滤波）

# ======================= 全局状态 =======================
g_lock         = threading.Lock()
g_pending      = deque(maxlen=4096)  # 待处理的原始样本 (t_us, ax, ay, az, gx, gy, gz)
g_angles       = deque(maxlen=1200)  # 滤波后的角度历史 (t_s, roll°, pitch°, yaw°)
g_attitude     = [0.0, 0.0, 0.0]     # 滤波结果 roll/pitch/yaw（弧度）
g_disp         = [0.0, 0.0, 0.0]     # 显示用角度（向 g_attitude 平滑过渡）
g_bias         = [0.0, 0.0, 0.0]     # 陀螺零偏估计（dps）
g_bias_n       = 0
g_prev_t_us    = None                # 滤波用：上一个样本时间戳（只由 animate 维护）
g_stream_t_us  = None                # 断档检测用：流上最后一个样本时间戳（只由 on_message 维护）
g_reset        = False               # 断档标志：animate 看到后重置 dt 链并重新初始化姿态
g_rate_hz      = 100.0
g_batch_count  = 0
g_sample_count = 0
g_connected    = False
g_demo         = False


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
    global g_rate_hz, g_batch_count, g_sample_count, g_stream_t_us, g_reset

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

            # 断档检测：间隔超过 2 秒说明丢包严重/设备重启，丢弃旧数据重新开始。
            # 包与包之间几十 ms 的正常间隔不算断档（固件发包要占用时间）。
            if g_stream_t_us is not None:
                gap_us = t_us - g_stream_t_us
                if gap_us <= 0 or gap_us > 2_000_000:
                    print(f"[i] 采样流断档 (gap={gap_us:.0f} us)，重置姿态缓冲区")
                    g_pending.clear()
                    g_angles.clear()
                    g_reset = True
            g_stream_t_us = t_us

            g_pending.append((t_us, ax, ay, az, gx, gy, gz))
            added += 1

        g_batch_count += 1
        g_sample_count += added

    if g_batch_count == 1:
        print(f"[i] 首包解析成功: {added} 个采样点")
    elif g_batch_count % 20 == 0:
        print(f"[i] 已收 {g_batch_count} 包 / {g_sample_count} 采样点 (rate={g_rate_hz:.0f} Hz)")


# ======================= 姿态解算 =======================
def update_filter(ax, ay, az, gx, gy, gz, dt):
    """互补滤波一步。返回 (roll, pitch, yaw) 弧度。
    机体坐标：x=前，y=右，z=上；roll 绕 x，pitch 绕 y，yaw 绕 z。"""
    global g_attitude, g_prev_t_us

    roll, pitch, yaw = g_attitude

    # 加速度计姿态（重力参考）
    roll_a  = math.atan2(ay, az)
    pitch_a = math.atan2(-ax, math.sqrt(ay * ay + az * az))

    if g_prev_t_us is None or dt <= 0:
        # 第一帧/断档后：直接用加速度计姿态初始化 roll/pitch，避免积分跳跃
        g_attitude = [roll_a, pitch_a, yaw]
        return g_attitude

    # 自适应权重：剧烈运动时重力参考不可信，更相信陀螺仪
    mag = math.sqrt(ax * ax + ay * ay + az * az)
    a = 0.99 if abs(mag - 1.0) > GYRO_TRUST_G else ALPHA

    gx_r = math.radians(gx - g_bias[0])
    gy_r = math.radians(gy - g_bias[1])
    gz_r = math.radians(gz - g_bias[2])

    roll  = a * (roll  + gx_r * dt) + (1 - a) * roll_a
    pitch = a * (pitch + gy_r * dt) + (1 - a) * pitch_a
    yaw  += gz_r * dt

    g_attitude = [roll, pitch, yaw]
    return g_attitude


def rot_matrix(roll, pitch, yaw):
    """机体 → 世界旋转矩阵（ZYX 顺序）。"""
    cx, sx = math.cos(roll), math.sin(roll)
    cy, sy = math.cos(pitch), math.sin(pitch)
    cz, sz = math.cos(yaw), math.sin(yaw)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


# ======================= 绘图 =======================
fig = plt.figure(figsize=(12, 7))
fig.canvas.manager.set_window_title("ESP32 IMU Attitude")
gs = fig.add_gridspec(2, 2, width_ratios=[3, 2], height_ratios=[3, 1])

ax3d = fig.add_subplot(gs[:, 0], projection="3d")
ax_hist = fig.add_subplot(gs[0, 1])
ax_txt = fig.add_subplot(gs[1, 1])

# ---- 3D 板子模型（机体坐标系，中心在原点） ----
BOARD = np.array([
    [-2.0, -1.25, -0.2], [2.0, -1.25, -0.2], [2.0, 1.25, -0.2], [-2.0, 1.25, -0.2],
    [-2.0, -1.25,  0.2], [2.0, -1.25,  0.2], [2.0, 1.25,  0.2], [-2.0, 1.25,  0.2],
])
FACES = [[0, 1, 2, 3], [4, 5, 6, 7], [0, 1, 5, 4], [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]]
AXIS_TIPS = np.array([[2.4, 0.0, 0.0], [0.0, 2.4, 0.0], [0.0, 0.0, 2.4]])   # x/y/z 轴箭头
AXIS_COLORS = ["#d62728", "#2ca02c", "#1f77b4"]                             # 红/绿/蓝

board_poly = Poly3DCollection([], facecolor="#7fb8e6", edgecolor="#333333",
                              linewidth=0.6, alpha=0.9)
ax3d.add_collection3d(board_poly)
axis_lines = []
for c in AXIS_COLORS:
    ln, = ax3d.plot([0, 0], [0, 0], [0, 0], color=c, lw=3)
    axis_lines.append(ln)

ax3d.set_xlim(-3.2, 3.2)
ax3d.set_ylim(-3.2, 3.2)
ax3d.set_zlim(-3.2, 3.2)
ax3d.set_box_aspect((1, 1, 1))
ax3d.set_axis_off()
ax3d.view_init(elev=24, azim=-64)
ax3d.set_title("板子姿态（拖动鼠标可旋转视角）")

# ---- 角度历史 ----
line_r, = ax_hist.plot([], [], color="#d62728", lw=1.2, label="Roll")
line_p, = ax_hist.plot([], [], color="#2ca02c", lw=1.2, label="Pitch")
line_y, = ax_hist.plot([], [], color="#1f77b4", lw=1.2, label="Yaw")
ax_hist.set_ylim(-185, 185)
ax_hist.set_ylabel("角度 (deg)")
ax_hist.grid(True, alpha=0.3)
ax_hist.legend(loc="upper right", fontsize=8)
ax_hist.set_title("姿态角历史（最近 10 秒）")

# ---- 大字读数 ----
ax_txt.set_axis_off()
txt_att = ax_txt.text(0.5, 0.62, "", ha="center", va="center",
                      fontsize=22, fontweight="bold")
txt_info = ax_txt.text(0.5, 0.12, "等待数据 ...", ha="center", va="center",
                       fontsize=9, color="#666666")

# 等待数据时的占位文本
wait3d = ax3d.text2D(0.5, 0.5, "等待 ESP32 数据 ...", transform=ax3d.transAxes,
                     ha="center", va="center", fontsize=14, color="#666666")


def animate(frame):
    global g_bias_n, g_prev_t_us, g_disp, g_bias, g_reset

    with g_lock:
        if not g_pending:
            return
        pending = list(g_pending)
        g_pending.clear()
        if g_reset:
            g_prev_t_us = None    # 断档后 dt 链归零，下一帧用加速度计重新初始化姿态
            g_reset = False

        # 逐样本滤波（包内 10ms 间隔，dt 用真实时间戳）
        for t_us, ax, ay, az, gx, gy, gz in pending:
            dt = (t_us - g_prev_t_us) * 1e-6 if g_prev_t_us is not None else 0.01
            dt = min(max(dt, 0.001), 0.1)     # 限幅，防止异常值

            if g_bias_n < BIAS_SAMPLES:
                g_bias[0] += gx
                g_bias[1] += gy
                g_bias[2] += gz
                g_bias_n += 1
                if g_bias_n == BIAS_SAMPLES:
                    g_bias = [b / BIAS_SAMPLES for b in g_bias]
                    print(f"[i] 陀螺零偏估计: gx={g_bias[0]:+.3f} "
                          f"gy={g_bias[1]:+.3f} gz={g_bias[2]:+.3f} dps（已自动补偿）")

            r, p, y = update_filter(ax, ay, az, gx, gy, gz, dt)
            g_prev_t_us = t_us
            g_angles.append((t_us * 1e-6, math.degrees(r), math.degrees(p), math.degrees(y)))

        roll, pitch, yaw = g_attitude
        last_t = g_angles[-1][0] if g_angles else 0.0

    # ---- 显示角度向目标平滑过渡（画面流畅的关键） ----
    target = list(g_attitude)
    g_disp = [d + (t - d) * EASE_K for d, t in zip(g_disp, target)]
    dr, dp, dy = g_disp

    # ---- 3D 模型 ----
    R = rot_matrix(dr, dp, dy)
    v = (R @ BOARD.T).T
    faces = [[v[i] for i in f] for f in FACES]
    board_poly.set_verts(faces)
    tips = (R @ AXIS_TIPS.T).T
    for ln, tip in zip(axis_lines, tips):
        ln.set_data([0, tip[0]], [0, tip[1]])
        ln.set_3d_properties([0, tip[2]])
    wait3d.set_text("")

    # ---- 历史曲线 ----
    if len(g_angles) >= 2:
        arr = np.asarray(g_angles)
        t = arr[:, 0]
        line_r.set_data(t, arr[:, 1])
        line_p.set_data(t, arr[:, 2])
        line_y.set_data(t, arr[:, 3])
        if t[-1] > 0:
            ax_hist.set_xlim(max(0.0, t[-1] - 10.0), t[-1] + 0.2)

    # ---- 读数与状态 ----
    txt_att.set_text(f"Roll   {math.degrees(dr):+6.1f}°\n"
                     f"Pitch  {math.degrees(dp):+6.1f}°\n"
                     f"Yaw    {math.degrees(dy):+6.1f}°")
    mode = "DEMO" if g_demo else "LIVE"
    mq = "UP" if (g_connected or g_demo) else "DOWN"
    txt_info.set_text(
        f"模式:{mode}  MQTT:{mq}  rate:{g_rate_hz:.0f} Hz\n"
        f"样本:{g_sample_count}  包:{g_batch_count}\n"
        f"Yaw 无磁力计会漂移 | 按 r 归零 | q 退出")


def on_key(event):
    if event.key == "r":
        with g_lock:
            g_attitude[2] = 0.0
        print("[i] Yaw 已归零")
    elif event.key == "q":
        plt.close(fig)


# ======================= 自检模式：模拟板子摆动 =======================
def demo_generator():
    """模拟 100Hz 采样：roll/pitch 正弦摆动 + yaw 缓慢旋转，带零偏和噪声。"""
    print("[*] DEMO 模式：模拟板子摆动（roll/pitch 摆动，yaw 缓慢漂移）")
    t_us = 0
    rng = np.random.default_rng(42)
    while True:
        t = t_us * 1e-6
        roll  = math.radians(30) * math.sin(2 * math.pi * 0.15 * t)
        pitch = math.radians(25) * math.sin(2 * math.pi * 0.20 * t + 0.7)
        yaw   = math.radians(20) * math.sin(2 * math.pi * 0.10 * t + 0.3) + 0.02 * t
        # 解析求导得到角速度（rad/s）
        roll_d  = math.radians(30) * 2 * math.pi * 0.15 * math.cos(2 * math.pi * 0.15 * t)
        pitch_d = math.radians(25) * 2 * math.pi * 0.20 * math.cos(2 * math.pi * 0.20 * t + 0.7)
        yaw_d   = math.radians(20) * 2 * math.pi * 0.10 * math.cos(2 * math.pi * 0.10 * t + 0.3) + 0.02
        # 世界重力转到机体坐标
        g_body = rot_matrix(roll, pitch, yaw).T @ np.array([0.0, 0.0, 1.0])
        ax, ay, az = g_body + rng.normal(0, 0.005, 3)
        gx = math.degrees(roll_d) + 0.10
        gy = math.degrees(pitch_d) - 0.06
        gz = math.degrees(yaw_d) + 0.08

        with g_lock:
            g_pending.append((float(t_us), float(ax), float(ay), float(az),
                              float(gx), float(gy), float(gz)))
        t_us += 10000
        time.sleep(0.01)


# ======================= 主程序 =======================
def main():
    global g_demo

    parser = argparse.ArgumentParser(description="ESP32 BMI270 姿态显示（roll/pitch/yaw）")
    parser.add_argument("--broker", default=DEFAULT_BROKER, help="MQTT broker IP（默认 %(default)s，本机回环）")
    parser.add_argument("--port",   type=int, default=DEFAULT_PORT, help="MQTT 端口（默认 %(default)s）")
    parser.add_argument("--topic",  default=DEFAULT_TOPIC, help="订阅主题（默认 %(default)s）")
    parser.add_argument("--demo",   action="store_true", help="自检模式：模拟板子摆动，不联网")
    args = parser.parse_args()

    g_demo = args.demo

    if args.demo:
        threading.Thread(target=demo_generator, daemon=True).start()
    else:
        import random
        client_id = f"pc_attitude_{random.randint(1000, 9999)}"
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        client.user_data_set({"topic": args.topic})
        client.on_connect    = on_connect
        client.on_disconnect = on_disconnect
        client.on_message    = on_message
        client.reconnect_delay_set(min_delay=1, max_delay=10)

        print(f"[*] 启动: broker={args.broker}:{args.port}  topic={args.topic}")
        print(f"[*] 提示：上电后板子先静止 {BIAS_SAMPLES / 100:.0f} 秒，用于估计陀螺零偏")
        client.connect_async(args.broker, args.port, keepalive=60)
        client.loop_start()

    fig.canvas.mpl_connect("key_press_event", on_key)
    # 100ms 轮询但只在有新数据时重绘，实际 CPU 占用很低、画面平滑
    ani = FuncAnimation(fig, animate, interval=100, blit=False, cache_frame_data=False)
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
