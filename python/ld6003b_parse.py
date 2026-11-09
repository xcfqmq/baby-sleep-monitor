#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解析 LD6003B 的串口原始数据（用户抓包），验证帧结构 + 解码载荷"""
import re
import struct

RAW = """
01 01 0F 00 10 0A 0A E0 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FE 01 01 10 00 18 0A 04 F9 01 00 00 00 7C BB 2E 3D 50 BE AC 3F D1 FA 56 BE FE FF FF FF 01 00 00 00 94
01 01 11 00 10 0A 0A FE 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FE 01 01 12 00 18 0A 04 FB 01 00 00 00 03 D2 EB 3C 70 A2 9D 3F C2 EB 37 BF FF FF FF FF 01 00 00 00 28
01 01 13 00 10 0A 0A FC 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FE 01 01 14 00 18 0A 04 FD 01 00 00 00 BA 33 92 3C 4A CC 92 3F E5 6F 6F BF FF FF FF FF 01 00 00 00 A9
01 01 19 00 10 0A 0A F6 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FE 01 01 1A 00 18 0A 04 F3 01 00 00 00 62 EE 1D 3D 3B 39 8A 3F 50 7F 7C BE F9 FF FF FF 01 00 00 00 0F
01 01 27 00 10 0A 0A C8 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FE 01 01 28 00 18 0A 04 C1 01 00 00 00 67 3A FD BC B4 A4 64 3F 1A 0B 6C 3E 02 00 00 00 01 00 00 00 E9
01 01 4B 00 10 0A 0A A4 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FE 01 01 4C 00 18 0A 04 A5 01 00 00 00 A4 F2 7F 3D E4 43 30 3F 59 67 2B 40 01 00 00 00 01 00 00 00 17
"""

# 把所有十六进制字节抠出来
data = bytes(int(b, 16) for b in re.findall(r"\b[0-9A-Fa-f]{2}\b", RAW))
print(f"总字节数: {len(data)}")

# 按协议切帧: SOF(1) ID(2,大端) LEN(2,大端) TYPE(2,大端) HEAD_CKSUM(1) DATA(LEN) DATA_CKSUM(1)
i = 0
frames = []
while i < len(data) - 8:
    if data[i] != 0x01:
        i += 1
        continue
    fid = struct.unpack(">H", data[i + 1:i + 3])[0]
    ln = struct.unpack(">H", data[i + 3:i + 5])[0]
    typ = struct.unpack(">H", data[i + 5:i + 7])[0]
    hck = data[i + 7]
    total = 1 + 2 + 2 + 2 + 1 + ln + 1
    if ln > 64 or i + total > len(data):
        i += 1
        continue
    payload = data[i + 8:i + 8 + ln]
    frames.append((fid, typ, ln, hck, payload, data[i + 8 + ln]))
    i += total

print(f"解析出 {len(frames)} 帧\n")
print(f"{'#':>3} {'帧ID':>6} {'TYPE':>7} {'LEN':>4} {'头校验':>6} {'数据校验':>7}  载荷(前 24 字节)")
for n, (fid, typ, ln, hck, pl, dck) in enumerate(frames):
    print(f"{n:>3} 0x{fid:04X}  0x{typ:04X} {ln:>4}   0x{hck:02X}   0x{dck:02X}   {pl[:24].hex(' ')}")

print("\n=== 按 TYPE 分类 ===")
types = {}
for fid, typ, ln, hck, pl, dck in frames:
    types.setdefault(typ, []).append((fid, ln, pl))
for typ, items in types.items():
    print(f"TYPE 0x{typ:04X}: {len(items)} 帧, LEN={items[0][1]}")

print("\n=== 解码 TYPE 0x0A04（按 目标信息 假设：id(u32) x(f32) y(f32) z(f32) dop(i32) 计数(u32)）===")
for fid, typ, ln, hck, pl, dck in frames:
    if typ != 0x0A04 or ln < 24:
        continue
    ident, x, y, z, dop, cnt = struct.unpack("<IfffIi", pl[:24])
    print(f"帧0x{fid:04X}: id={ident}  x={x:+.4f} y={y:+.4f} z={z:+.4f} m  dopIdx={dop}  末字段={cnt}")

print("\n=== TYPE 0x0A0A 的载荷（所有帧都一样吗？）===")
sig = set(pl for _, typ, ln, _, pl, _ in frames if typ == 0x0A0A)
for s in sig:
    print("  ", s.hex(" "))
