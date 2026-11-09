#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
broker_responder.py —— 每 2 秒向局域网广播本机 MQTT broker 地址。

用法：
    python broker_responder.py

保持运行即可。电脑换 WiFi / IP 变了都不用改固件：
ESP32 开机时监听 UDP 8888，收到通告就用最新地址连 broker。

为什么是"电脑广播、ESP32 监听"？
    Windows 防火墙默认拦截"从外部主动进来的" UDP 包。如果反过来
    （ESP32 查询、电脑回复），回复包会在电脑侧被防火墙丢掉。
    由电脑主动广播，出站包是自己发的，不受影响。

加固点（针对换网络后 WinError 10065）：
  1. 每轮重新检测本机 IP，IP 变化立即提示；
  2. 每次广播重建 socket，避免旧路由失效；
  3. 同时发「子网广播」和「全局广播」两个目标。

注意：ESP32 和电脑必须连同一个 WiFi / 热点，广播无法跨网段。
"""
import socket
import time

DISCOVERY_PORT = 8888        # 与 ESP32 的 BROKER_DISC_PORT 一致
MQTT_PORT      = 1883        # mosquitto 端口
INTERVAL_S     = 2.0         # 广播间隔（秒）


def local_ip():
    """取本机局域网出口 IP（UDP connect 只选路，不会真的发包）"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return None
    finally:
        s.close()


def subnet_broadcast(ip: str) -> str:
    """按 /24 推断同网段广播地址（家庭网络够用；/16 用全局广播兜底）"""
    parts = ip.split(".")
    if len(parts) == 4:
        return ".".join(parts[:3] + ["255"])
    return "255.255.255.255"


def send_once(payload: bytes, target: str) -> bool:
    """新建 socket 发一次广播（每次重建，避免换网络后路由失效）"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        s.sendto(payload, (target, DISCOVERY_PORT))
        return True
    except OSError:
        return False
    finally:
        s.close()


def main() -> None:
    last_ip = None
    print(f"[announce] 目标 UDP {DISCOVERY_PORT}，MQTT 端口 {MQTT_PORT}，间隔 {INTERVAL_S:.0f}s")
    print("           保持运行，ESP32 开机即可自动发现（Ctrl+C 退出）")

    while True:
        ip = local_ip()
        if ip is None:
            time.sleep(INTERVAL_S)
            continue

        if ip != last_ip:
            print(f"[announce] 本机 IP: {ip} → 通告 broker {ip}:{MQTT_PORT}")
            last_ip = ip

        payload = f"MQTT_BROKER {ip}:{MQTT_PORT}".encode()
        ok = False
        for target in (subnet_broadcast(ip), "255.255.255.255"):
            if send_once(payload, target):
                ok = True

        if not ok:
            print(f"[announce] 广播失败（IP={ip}），可能网卡未就绪或不在同一局域网")

        time.sleep(INTERVAL_S)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[announce] 已退出")
