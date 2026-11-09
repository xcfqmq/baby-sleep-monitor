#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解析 HLK-LD6002（呼吸心率雷达）串口原始数据

帧结构（与 LD6003B 同族，已用实测数据验证）：
  SOF(1)=0x01 | 帧ID(2,大端) | LEN(2,大端) | TYPE(2,大端) | 头校验(1) | DATA(LEN) | 数据校验(1)
  DATA 里的数值都是 float32 小端（厂商《解析参考》确认）
"""
import re
import struct

RAW = """
01 1E EA 00 01 0A 38 39 01 FE 01 1E EB 00 14 0A 04 11 01 00 00 00 36 25 21 3D BE A5 A7 3E 00 00 00 00 00 00 00 00 73 01 1E EC 00 02 0A 18 1C 01 00 FE 01 1E ED 00 08 0A 16 19 01 00 00 00 9A 99 E5 41 59 01 1E EE 00 02 0F 09 0A 01 00 FE 01 1E EF 00 08 0A 17 1A 94 72 FC 3C 01 D9 A6 3E 99 01 1E F0 00 0C 0A 13 05 00 E4 5C BD BA 63 0F 3E 2E 1F BF BC 20 01 1E F1 00 04 0A 14 0B 00 00 C0 41 7E 01 1E F2 00 18 01 00 0B 62 72 65 61 74 68 20 72 61 74 65 20 3D 20 32 34 2E 30 30 30 30 30 30 20 E0
01 1E F5 00 01 0A 38 26 01 FE 01 1E F6 00 14 0A 04 0C 01 00 00 00 AF 29 83 3D CA F3 BA 3E 00 00 00 00 00 00 00 00 7B 01 1E F7 00 02 0A 18 07 01 00 FE 01 1E F8 00 08 0A 16 0C 01 00 00 00 9A 99 E5 41 59 01 1E F9 00 02 0F 09 1D 01 00 FE 01 1E FA 00 08 0A 17 0F 54 46 42 3D 66 E6 B0 3E 9C 01 1E FB 00 0C 0A 13 0E C0 64 81 3E A4 7F 21 3E B1 38 EA 3C 7F 01 1E FC 00 04 0A 14 06 00 00 C0 41 7E 01 1E FD 00 18 01 00 04 62 72 65 61 74 68 20 72 61 74 65 20 3D 20 32 34 2E 30 30 30 30 30 30 20 E0
01 1F 0B 00 01 0A 38 D9 01 FE 01 1F 0C 00 14 0A 04 F7 01 00 00 00 21 29 AE 3D AD D8 C5 3E 00 00 00 00 00 00 00 00 EB 01 1F 0D 00 02 0A 18 FC 01 00 FE 01 1F 0E 00 08 0A 16 FB 01 00 00 00 9A 99 E5 41 59 01 1F 0F 00 02 0F 09 EA 01 00 FE 01 1F 10 00 08 0A 17 E4 1A AD 98 3D 21 1C BF 3E 51 01 1F 11 00 0C 0A 13 E5 00 16 64 BE 1E E6 2E 3E 13 E2 13 BD 84 01 1F 12 00 04 0A 14 E9 00 00 C0 41 7E 01 1F 13 00 18 01 00 EB 62 72 65 61 74 68 20 72 61 74 65 20 3D 20 32 34 2E 30 30 30 30 30 30 20 E0 01 1F 14 00 04 0A 15 EE 00 00 88 42 35 01 1F 15 00 17 01 00 E2 68 65 61 72 74 20 72 61 74 65 20 3D 20 36 38 2E 30 30 30 30 30 30 20 8A
01 1F 4A 00 0C 0A 13 BE 00 68 0B 3C B0 E4 74 3D 5C 02 4D BC 12 01 1F 4B 00 04 0A 14 B0 00 00 C0 41 7E 01 1F 4C 00 18 01 00 B4 62 72 65 61 74 68 20 72 61 74 65 20 3D 20 32 34 2E 30 30 30 30 30 30 20 E0 01 1F 4D 00 04 0A 15 B7 00 00 92 42 2F 01 1F 4E 00 17 01 00 B9 68 65 61 72 74 20 72 61 74 65 20 3D 20 37 33 2E 30 30 30 30 30 30 20 80
01 1F 85 00 18 01 00 7D 62 72 65 61 74 68 20 72 61 74 65 20 3D 20 32 34 2E 30 30 30 30 30 30 20 E0 01 1F 86 00 04 0A 15 7C 00 00 98 42 25 01 1F 87 00 17 01 00 70 68 65 61 72 74 20 72 61 74 65 20 3D 20 37 36 2E 30 30 30 30 30 30 20 85
"""

data = bytes(int(b, 16) for b in re.findall(r"\b[0-9A-Fa-f]{2}\b", RAW))
print(f"总字节数: {len(data)}\n")

# ---- 切帧 ----
i, frames = 0, []
while i + 8 < len(data):
    if data[i] != 0x01:
        i += 1
        continue
    fid = struct.unpack(">H", data[i + 1:i + 3])[0]
    ln = struct.unpack(">H", data[i + 3:i + 5])[0]
    typ = struct.unpack(">H", data[i + 5:i + 7])[0]
    if ln > 64 or i + 9 + ln > len(data):
        i += 1
        continue
    frames.append((fid, typ, ln, data[i + 7], data[i + 8:i + 8 + ln], data[i + 8 + ln]))
    i += 9 + ln

print(f"解析出 {len(frames)} 帧\n")

f32 = lambda b, o: struct.unpack("<f", b[o:o + 4])[0]
u32 = lambda b, o: struct.unpack("<I", b[o:o + 4])[0]

print("=== 消息类型统计 ===")
kinds = {}
for fid, typ, ln, hck, pl, dck in frames:
    kinds.setdefault(typ, set()).add(ln)
for typ, lns in sorted(kinds.items()):
    print(f"  TYPE 0x{typ:04X}  LEN={sorted(lns)}")

print("\n=== 生命体征（核心）===")
for fid, typ, ln, hck, pl, dck in frames:
    if typ == 0x0A14:
        print(f"  0x{fid:04X} 呼吸速率 = {f32(pl,0):.2f} bpm")
    elif typ == 0x0A15:
        print(f"  0x{fid:04X} 心跳速率 = {f32(pl,0):.2f} bpm")
    elif typ == 0x0A16 and ln >= 8:
        print(f"  0x{fid:04X} 距离 = {f32(pl,4):.2f} cm  (flag={u32(pl,0)})")
    elif typ == 0x0A13 and ln >= 12:
        print(f"  0x{fid:04X} 相位: 总={f32(pl,0):+.4f} 呼吸={f32(pl,4):+.4f} 心跳={f32(pl,8):+.4f}")
    elif typ == 0x0A17 and ln >= 8:
        print(f"  0x{fid:04X} 位置: x={f32(pl,0):+.4f} y={f32(pl,4):+.4f}")
    elif typ == 0x0A04 and ln >= 16:
        print(f"  0x{fid:04X} 目标: id={u32(pl,0)} x={f32(pl,4):+.4f} y={f32(pl,8):+.4f} z={f32(pl,12):+.4f}")

print("\n=== 文本帧（TYPE 0x0100，模块自带调试输出，可与二进制值交叉验证）===")
for fid, typ, ln, hck, pl, dck in frames:
    if typ == 0x0100:
        print(f"  0x{fid:04X} \"{pl.decode('ascii', 'replace').strip()}\"")

print("\n=== 其他短帧原始内容 ===")
for fid, typ, ln, hck, pl, dck in frames:
    if typ in (0x0A38, 0x0A18, 0x0F09):
        print(f"  0x{fid:04X} TYPE=0x{typ:04X} LEN={ln} DATA={pl.hex(' ')}")
