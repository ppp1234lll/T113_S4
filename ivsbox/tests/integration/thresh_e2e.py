#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""阈值下发 0xF1 的装配层端到端取证（VM/板端以 root 跑真 ivsboxd）

为什么必须有这一层：`iv_agent_apply_thresh()` 的单测与 C 集成用例只证明"给它一份
阈值它能发对帧"。**"改配置会不会走到它"** 取决于 `main.c` 的订阅掩码 —— 掩码漏了
class，分支写得再对也永远不触发，编译、单测、集成用例全绿，运行期零反应
（2026-10-21 的 `probe.*` 就是这么踩的）。只有真拉起进程、真下发 `config.apply`
才看得见。这也是本次两条反向证伪（去掩码 / 空驱动）的取证工具。

判据是落盘日志（ivsboxd 不往 stdout 打日志）：
  A1  改 sensor.temp_high ⇒ 出现 `thresh queued`，载荷含 "tu":55 且**不含** hv
  A2  再改 elec.volt_high ⇒ 载荷变成 {"hv":250,...,"tu":55}（九键顺序固定）
  A3  九项全 0（把两个键清零）⇒ **不再**排队新载荷（IV_ESTATE 不算错误）
  A4  本机没有 /dev/ttySAC5 ⇒ 出现 `thresh not sent yet`（重试在跑，不是静默丢弃）
  A5  重试不是每拍刷屏：6 s 内该行新增 ≤ 2（退避 1 s ＋ 每 60 次降噪）

用法（必须先 `make build`）：
    cd <repo>/ivsbox
    sudo python3 tests/integration/thresh_e2e.py
前置：`/mnt/UDISK/log` 可写（`sudo mkdir -p /mnt/UDISK/log`）；当前**没有**别的
ivsboxd 在跑（本脚本自己拉起 build/host/ivsboxd，结束时收掉）。
退出码：0 = 全部通过；1 = 有失败。
"""
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time

CTL = '/var/run/ivsbox/ctl'
LOGROOT = '/mnt/UDISK/log'
BIN = os.path.abspath('build/host/ivsboxd')
MAGIC = 0x42535649
HDR = struct.Struct('<IHHIIQII')

fails = []
rid = 0

# 本次拉起 daemon 时的日志长度；只认这之后的追加内容。
# 日志按小时分文件、跨轮复用同一个文件，不记偏移的话上一轮的 `thresh queued`
# 会被当成这一轮的证据（第一次跑证伪时就吃了这个亏：该红的地方照样绿）。
BASE = 0


def chk(cond, what):
    if cond:
        print('  PASS ' + what)
    else:
        print('  FAIL ' + what)
        fails.append(what)


def ctl(method, params=None):
    """一条 ctl 请求。参数必须嵌在 p 里（见项目约定）。"""
    global rid
    rid += 1
    payload = json.dumps({'m': method, 'p': params or {}}).encode()
    hdr = HDR.pack(MAGIC, 1, 1, 0, 0, rid, len(payload), 0)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.settimeout(10)
    s.connect(CTL)
    s.sendall(hdr + payload)
    buf = s.recv(70000)
    s.close()
    _m, _ver, _typ, _fl, _pad, _rqid, plen, _rsv = HDR.unpack(buf[:32])
    return json.loads(buf[32:32 + plen].decode())


def wait_ctl(deadline=15.0):
    t0 = time.time()
    while time.time() - t0 < deadline:
        if os.path.exists(CTL):
            try:
                ctl('system.snapshot')
                return True
            except Exception:
                pass
        time.sleep(0.2)
    return False


def logfile():
    """当前这一小时的落盘日志。ivsboxd 不往 stdout 打日志。"""
    best = None
    for day in sorted(os.listdir(LOGROOT)):
        d = os.path.join(LOGROOT, day)
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            p = os.path.join(d, f)
            if best is None or os.path.getmtime(p) > os.path.getmtime(best):
                best = p
    return best


def logtext():
    p = logfile()
    if p is None:
        return ''
    with open(p, 'rb') as fh:
        return fh.read().decode('utf-8', 'replace')


def wait_log(pattern, deadline=8.0):
    """等某条日志出现；返回最后一次匹配到的行（None = 超时）。"""
    t0 = time.time()
    last = None
    while time.time() - t0 < deadline:
        for line in logtext()[BASE:].splitlines():
            if re.search(pattern, line):
                last = line
        if last is not None:
            return last
        time.sleep(0.2)
    return last


def apply_items(items):
    r = ctl('config.class', {'cls': 'sensor'})
    ver = (r.get('d') or {}).get('version')
    return ctl('config.apply', {'ver': ver, 'items': items})


def main():
    global BASE

    if wait_ctl(0.1):
        print('FAIL: 已经有 ivsboxd 在跑，换个环境再测')
        return 1

    p = subprocess.Popen([BIN], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    try:
        if not wait_ctl():
            print('FAIL: ivsboxd 没起来')
            return 1

        BASE = len(logtext())

        print('A1 改 sensor.temp_high')
        r = apply_items({'sensor.temp_high': 55})
        chk(r.get('ok') is True, 'A1a config.apply 成功')
        line = wait_log(r'thresh queued')
        chk(line is not None, 'A1b 出现 thresh queued（订阅掩码含 SENSOR）')
        if line is not None:
            chk('"tu":55' in line, 'A1c 载荷含 "tu":55')
            chk('"hv"' not in line, 'A1d 载荷不含未设置的 hv（0 ＝ 不设置）')

        print('A2 再改 elec.volt_high')
        r = apply_items({'elec.volt_high': 250})
        chk(r.get('ok') is True, 'A2a config.apply 成功')
        line = wait_log(r'"hv":250')
        chk(line is not None, 'A2b 出现含 "hv":250 的载荷（订阅掩码含 ELEC）')
        if line is not None:
            chk('"tu":55' in line, 'A2c 九键一起发：tu 仍在（配置是权威）')
            chk(line.index('"hv":250') < line.index('"tu":55'),
                'A2d 键序固定 hv … tu')

        print('A4 没有采集板时必须重试，不是静默丢弃')
        line = wait_log(r'thresh not sent yet', deadline=6.0)
        chk(line is not None, 'A4a 出现 thresh not sent yet')

        n0 = sum(1 for l in logtext().splitlines() if 'thresh not sent yet' in l)
        time.sleep(6.0)
        n1 = sum(1 for l in logtext().splitlines() if 'thresh not sent yet' in l)
        chk(n1 - n0 <= 2,
            'A5 6 s 内新增重试日志 ≤ 2 行（退避 1 s ＋ 每 60 次降噪），实得 %d' % (n1 - n0))

        print('A3 九项全 0 ⇒ 不再排队')
        before = len(logtext())
        r = apply_items({'elec.volt_high': 0, 'sensor.temp_high': 0})
        chk(r.get('ok') is True, 'A3a 清零成功')
        time.sleep(1.5)
        tail = logtext()[before:]
        chk('thresh queued' not in tail,
            'A3b 全 0 时不排队（IV_ESTATE 被当成"没什么可发"，不报错也不假装发了）')
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()

    return 0


if __name__ == '__main__':
    rc = main()
    print('')
    if fails:
        print('%d FAILED' % len(fails))
        sys.exit(1)
    print('thresh e2e passed (12 assertions)')
    sys.exit(rc)
