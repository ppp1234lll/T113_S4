#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""平台下行指令逐条回归工装（板端运行）。

作用：冒充平台，把参考实现（MCU `com.c`）里的全部命令码逐条下发一遍，
      收集设备应答，用于"平台指令对照表"的实测列。

用法（板端）：
    python3 fake_plat.py                # 只发安全命令
    python3 fake_plat.py --unsafe       # 连重启/复位/继电器一起发（会断链、会动硬件）
    python3 fake_plat.py --only F5,E3   # 只发指定命令

前提：
    1. 本脚本在**设备本机**监听 0.0.0.0:9998，设备侧把
       net.server.wireless.host/port 临时指向 <设备自身 usb0 IP>:9998；
    2. 抓完必须把地址改回平台（工装不改配置，改配置由外层脚本负责，
       因为改完链路会重连，脚本进程可能跟着断）。

为什么不用抓包：设备到平台是**出站**连接，抓包只能看到我们发的东西；
要逐条下发，必须让设备主动连到"我"。
"""
import argparse
import json
import socket
import struct
import sys
import time

HEAD_DL = 0xF0F0        # 下行帧头（平台→设备），参考实现 COM_REC_HAED_HEX
HEAD_UL = 0x0F0F        # 上行帧头（设备→平台），参考实现 COM_HEAD_HEX
TAIL = 0xFFFF
VER = 0x11
DEVTYPE = 0x0400
TID = 0x0B2CC1
DEBUG = False          # 由外层用 --tid 覆盖；仅下行帧会被设备校验


def crc8(data):
    """CRC-8/SMBUS：poly 0x07、init 0、不反射、不异或（同 iv_crc8）。"""
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if (c & 0x80) else ((c << 1) & 0xFF)
    return c


def build(cmd, payload=b"", qn1=0x11223344, qn2=0x55667788,
          devtype=DEVTYPE, tid=None):
    if tid is None:
        tid = TID
    body = struct.pack(">BHB", VER, devtype, (tid >> 16) & 0xFF) + \
        struct.pack(">H", tid & 0xFFFF) + struct.pack(">B", cmd) + \
        struct.pack(">II", qn1, qn2) + struct.pack(">B", len(payload)) + payload
    c = crc8(body)
    return struct.pack(">H", HEAD_DL) + body + struct.pack(">B", c) + \
        struct.pack(">H", TAIL)


def parse_stream(buf):
    """从字节流里抠出完整上行帧。返回 (frames, rest)。

    上行有**两种**封帧（见参考实现 Middlewares/cjson/mycJSON.c）：
      1. 查询类应答：`##{...}##`  —— 无长度域、无 CRC，定长前缀/后缀共 4 字节
      2. 配置应答/心跳：二进制 `0f0f <body> crc ffff`
    注意上下行帧头**不同**：上行 `0f0f`、下行 `f0f0`（参考实现 com.c:4-7）。
    两种封帧都可能在同一个 TCP 流里出现，按最先出现的帧头分派。
    """
    out = []
    i = 0
    n = len(buf)
    while i < n - 1:
        jb = buf.find(b"\x0f\x0f", i)
        jt = buf.find(b"##", i)
        if jb < 0 and jt < 0:
            break
        if jb < 0:
            jb = n
        if jt < 0:
            jt = n
        if jt < jb:
            k = buf.find(b"##", jt + 2)
            if k < 0:
                return out, buf[jt:]
            payload = buf[jt + 2:k]
            out.append({"kind": "txt", "cmd": None, "qn1": 0, "qn2": 0,
                        "len": len(payload), "data": payload})
            i = k + 2
            continue
        # 二进制帧最短 = 2+1+2+3+1+4+4+1 + 0 + 1+2 = 21 字节
        if n < jb + 21:
            return out, buf[jb:]
        ln = buf[jb + 17]
        total = 18 + ln + 3
        if n < jb + total:
            return out, buf[jb:]
        fr = buf[jb:jb + total]
        if fr[-2:] != b"\xff\xff" or crc8(fr[2:18 + ln]) != fr[18 + ln]:
            i = jb + 1
            continue
        out.append({
            "kind": "bin",
            "cmd": fr[8],
            "qn1": struct.unpack(">I", fr[9:13])[0],
            "qn2": struct.unpack(">I", fr[13:17])[0],
            "len": ln,
            "data": fr[18:18 + ln],
        })
        i = jb + total
    return out, buf[i:] if i < n else b""


def frame_cmd(fr):
    """取出该帧对应的命令码：二进制帧看帧头，JSON 帧看 data.cmd（十六进制串）。"""
    if fr["kind"] == "bin":
        return fr["cmd"]
    try:
        j = json.loads(fr["data"].decode("utf-8", "replace"))
    except Exception:
        return None
    d = j.get("data")
    c = d.get("cmd") if isinstance(d, dict) else None
    if isinstance(c, str):
        try:
            return int(c, 16)
        except ValueError:
            return None
    return None


# 命令表：(码, 名称, 载荷, 危险?)
# 载荷取自参考实现 com.c 的解析方式（大端/文本原样）。
CMDS = [
    (0xE1, "查询当前参数", b"", 0),
    (0xE2, "立即上报状态", b"\x00", 0),
    (0xE3, "查询软件版本", b"", 0),
    (0xE4, "搜索摄像机信息", b"\x00", 0),
    (0xE5, "查询IPC信息", b"", 0),
    (0xE6, "查询LBS信息", b"", 0),
    (0xE7, "查询SNMP参数", b"\x04", 0),
    (0xE8, "查询PING信息", b"\xc0\xa8\x02\x01", 0),
    (0xF5, "配置上报间隔", struct.pack(">BH", 1, 180), 0),
    (0xF2, "配置PING间隔", struct.pack(">HB", 5000, 3), 0),
    (0xF3, "配置传输模式", b"\x04", 0),
    (0xF8, "配置主网检测IP", b"\xc0\xa8\x02\x01\xc0\xa8\x02\x02", 0),
    (0xA3, "配置设备名称", b"test-name", 0),
    (0xA4, "配置阈值", struct.pack(">HHHBH", 300, 200, 10, 30, 5), 0),
    (0xA5, "配置搜索方式", b"\x01", 0),
    (0xA2, "配置摄像机账号", b"1 admin pwd 80", 0),
    (0xA7, "485透传", b"\x01\x02\x03", 0),
    (0xA9, "ONVIF配IP", b"\x01\xc0\xa8\x02\x64", 0),
    (0xBE, "配置SNMP OID", b"\x04\x01\x31\x2e\x33\x2e\x36", 0),
    (0xFE, "配置网络延时", b"\x05", 0),
    (0xBF, "配置网络传输电源", b"\x01\x02", 0),
    (0xB1, "校时", struct.pack(">I", 1760000000), 0),
    # ---- 危险：会动硬件 / 断链 / 改身份，默认不发 ----
    (0xF1, "配置服务器地址", b"outer##47.104.250.225:6013", 1),
    (0xF4, "配置本地网络", b"\xc0\xa8\x02\x78\xff\xff\xff\x00\xc0\xa8\x02\x01", 1),
    (0xF6, "配置风扇温度", b"\x50\x28", 1),
    (0xF7, "配置摄像机IP", b"\x01\xc0\xa8\x02\x64", 1),
    (0xF9, "配置风扇湿度", b"\x5a\x28", 1),
    (0xFB, "补光灯时段", b"18:00-6:00", 1),
    (0xAC, "箱门时段", b"00:00-23:59", 1),
    (0xFD, "配置MAC", b"\x02\x00\x00\x44\x6c\x40", 1),
    (0xA8, "配置设备ID", b"\x0b\x2c\xc1", 1),
    (0xC1, "风扇启停", b"\x02\x00", 1),
    (0xC4, "外设电源", b"\x02\x02", 1),
    (0xD9, "电源重启", b"\x00", 1),
    (0xDA, "单路摄像机供电", b"\x01", 1),
    (0xDC, "复位以太网", b"\x00", 1),
    (0xDD, "复位GPRS", b"\x00", 1),
    (0xDE, "断电重启4G", b"\x00", 1),
    (0xB2, "文件上传", b"\x00", 1),
    (0xB3, "更新系统", b"\x00", 1),
]


def summarize(fr):
    d = fr["data"]
    if fr["kind"] == "bin" and fr["len"] == 1:
        return "ACK 0x%02X" % d[0]
    try:
        txt = d.decode("utf-8", "replace")
    except Exception:
        return "BIN %s" % d[:40].hex()
    if txt.startswith("{"):
        try:
            j = json.loads(txt)
            det = j.get("data", j)
            keys = list(det.keys()) if isinstance(det, dict) else []
            return "JSON %dB keys=%s" % (fr["len"], ",".join(keys[:12]))
        except Exception:
            return "JSON(bad) %dB %s..." % (fr["len"], txt[:60])
    return "TXT %dB %s..." % (fr["len"], txt[:60])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9998)
    ap.add_argument("--tid", type=lambda s: int(s, 16), default=TID)
    ap.add_argument("--unsafe", action="store_true")
    ap.add_argument("--only", default="")
    ap.add_argument("--wait", type=float, default=2.0, help="每条等待秒数")
    ap.add_argument("--pause", action="store_true", help="危险命令前等回车")
    ap.add_argument("--debug", action="store_true", help="打印原始收包")
    ap.add_argument("--debug2", action="store_true", help=argparse.SUPPRESS)
    a = ap.parse_args()

    global DEBUG
    DEBUG = a.debug

    tid = a.tid

    only = set(x.strip().upper() for x in a.only.split(",") if x.strip())
    todo = []
    for code, name, payload, danger in CMDS:
        if only and ("%02X" % code) not in only:
            continue
        if danger and not a.unsafe:
            continue
        todo.append((code, name, payload, danger))
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(1)
    print("LISTEN %d, tid=%06X, commands=%d" % (a.port, tid, len(todo)))
    sys.stdout.flush()

    srv.settimeout(120)
    try:
        conn, addr = srv.accept()
    except socket.timeout:
        print("TIMEOUT: no device connected")
        return 1
    print("CONNECTED from %s:%d" % addr)
    sys.stdout.flush()
    conn.settimeout(0.2)

    buf = b""
    results = []
    stats = {"bytes": 0}

    def drain(seconds):
        nonlocal buf
        got = []
        end = time.time() + seconds
        while time.time() < end:
            try:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                stats["bytes"] += len(chunk)
                if DEBUG:
                    print("  RX %s" % chunk[:64].hex())
                buf += chunk
            except socket.timeout:
                pass
            except OSError:
                break
            frames, buf = parse_stream(buf)
            got.extend(frames)
        return got

    # 先吃掉设备连上来的首包（心跳/补传），免得混进第一条的结果
    drain(1.0)

    for code, name, payload, danger in todo:
        if danger and a.pause:
            input("press ENTER to send 0x%02X %s" % (code, name))
        frame = build(code, payload, tid=tid)
        try:
            conn.sendall(frame)
        except OSError:
            print("SEND-FAIL 0x%02X" % code)
            break
        got = [f for f in drain(a.wait) if frame_cmd(f) == code]
        if not got:
            res = "(no reply)"
        else:
            res = " | ".join(summarize(f) for f in got)
        results.append((code, name, res))
        print("0x%02X %-16s -> %s   [rx_total=%dB]" % (code, name, res, stats["bytes"]))
        sys.stdout.flush()

    print("\n==== SUMMARY ====")
    for code, name, res in results:
        print("0x%02X\t%s\t%s" % (code, name, res))
    conn.close()
    srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
