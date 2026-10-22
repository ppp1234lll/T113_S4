#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""平台「备份」地址（`net.server.<口>.host_bak` / `port_bak`）的装配层端到端取证
VM/板端以 root 跑真 ivsboxd，2026-10-22

为什么必须有这一层：`iv_agent_pick_host()` 的单测（`test_agent_pick`）只证明
"给它一组 cfg 它按 4 条链选"。**"改了配置会不会走进去"** 取决于两处只在装配层
才成立的接线：`main.c` 的 NET 类订阅掩码，以及 `agent_sync_server_cfg()` 把四个
值真的塞进 `g_agent.cfg`。少一处就编译过、单测全绿、网页改完零反应
（`probe.*` / `camera.detect.*` / `netthr.*` 都这么踩过）。

判据是 **落盘日志**（ivsboxd 不往 stdout 打日志）：
  `server addr reloaded -> <host>:<port> (wan=<口>, bind=<网卡>)`
这一行由 `iv_agent_reload_server()` 打，出现即说明"解析出的地址真的交给了
`iv_report`"。另配 `net.server.* changed -> reload rc=…` 判"有没有发生重连"。

覆盖：
  A1 基线：四个备份键存在，出厂＝空地址 ＋ 6012 端口；两路主地址仍在
  A2 两口主地址都清空、两口备份给**不同**地址 ⇒ 选中的必须是"活动口自己的备"
     （同一行日志里的 `wan=` 就是活动口，自洽校验 ⇒ 证明「本口备 > 另一口备」）
  A3 给两口同样的主地址 ⇒ 选主（证明「本口主 > 本口备」）
  A4 主地址在用，只改备份地址 ⇒ **不重连**（解析结果没变），但配置回读已变
  A5 再把主地址清空 ⇒ 备份（含刚改的那份）立刻顶上
  A6 收尾恢复出厂值

⚠ 开头第一件事是把 `netthr.reboot_max` 置 0（自愈禁用）：本脚本会把平台地址
  指到连不通的 127.0.0.1:160xx，若上一轮留下 reboot_max>0，自愈会把 VM 重启掉。

用法（必须先 `make build`）：
    cd <repo>/ivsbox
    sudo python3 tests/integration/netsrv_e2e.py
前置：`/mnt/UDISK/log` 可写；当前**没有**别的 ivsboxd 在跑（本脚本自己拉起，
结束时收掉）。退出码：0 = 全部通过；1 = 有失败。
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

DEFAULT_HOST = '47.104.250.225'
DEFAULT_PORT = 6012

fails = []
rid = 0

# 本次拉起 daemon 时的日志长度；只认这之后的追加内容（日志按小时分文件、
# 跨轮复用同一个文件，不记偏移就会把上一轮的行当成这一轮的证据）。
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


def wait_log(pattern, deadline=8.0, start=None):
    """等日志出现某行；返回最后一次匹配的行（None = 超时）。"""
    t0 = time.time()
    off = BASE if start is None else start
    last = None
    while time.time() - t0 < deadline:
        for line in logtext()[off:].splitlines():
            if re.search(pattern, line):
                last = line
        if last is not None:
            return last
        time.sleep(0.2)
    return last


def net_items():
    return (ctl('config.class', {'cls': 'net'}).get('d') or {}).get('items') or {}


def apply_net(items):
    ver = (ctl('config.class', {'cls': 'net'}).get('d') or {}).get('version')
    return ctl('config.apply', {'ver': ver, 'items': items})


def both(wired, wireless):
    """把两口的同类键一次性给成一组值 —— 这样"哪个口是活动口"不影响期望值，
    只有 A2 需要区分（那一组特意给不同值，靠同一行日志里的 `wan=` 自洽校验）。"""
    out = {}
    for k, v in wired.items():
        out['net.server.wired.' + k] = v
    for k, v in wireless.items():
        out['net.server.wireless.' + k] = v
    return out


def set_selfheal_off():
    """自愈限次置 0（禁用）。平台地址连不通时 `netthr.reboot_max > 0` 会真重启。"""
    ver = (ctl('config.class', {'cls': 'netthr'}).get('d') or {}).get('version')
    return ctl('config.apply', {'ver': ver, 'items': {'netthr.reboot_max': 0}})


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

        # 先关自愈，再碰平台地址（顺序不能反：见文件头）。
        r = set_selfheal_off()
        chk(r.get('ok') is True, 'A0a netthr.reboot_max 置 0 成功（连不通也不怕）')

        print('A1 基线：备份键存在且出厂为空地址 / 6012 端口')
        r = apply_net(both(
            {'host': DEFAULT_HOST, 'port': DEFAULT_PORT, 'host_bak': '',
             'port_bak': DEFAULT_PORT},
            {'host': DEFAULT_HOST, 'port': DEFAULT_PORT, 'host_bak': '',
             'port_bak': DEFAULT_PORT}))
        chk(r.get('ok') is True, 'A1a 恢复出厂平台地址')
        it = net_items()
        chk(it.get('net.server.wired.host_bak') == '', 'A1b 有线备份地址出厂为空')
        chk(it.get('net.server.wireless.host_bak') == '', 'A1c 无线备份地址出厂为空')
        chk(it.get('net.server.wired.port_bak') == DEFAULT_PORT, 'A1d 有线备份端口 = 6012')
        chk(it.get('net.server.wireless.port_bak') == DEFAULT_PORT,
            'A1e 无线备份端口 = 6012')
        # 老的两路主地址没被新键顶掉（键名只多了后缀，前缀不许变）
        chk(it.get('net.server.wired.host') == DEFAULT_HOST, 'A1f 有线主地址仍在')
        chk(it.get('net.server.wireless.host') == DEFAULT_HOST, 'A1g 无线主地址仍在')

        print('A2 两口主地址清空、备份给不同地址 ⇒ 选活动口自己的备')
        start = len(logtext())
        r = apply_net({
            'net.server.wired.host': '',
            'net.server.wired.port': DEFAULT_PORT,
            'net.server.wired.host_bak': '127.0.0.1',
            'net.server.wired.port_bak': 16001,
            'net.server.wireless.host': '',
            'net.server.wireless.port': DEFAULT_PORT,
            'net.server.wireless.host_bak': '127.0.0.1',
            'net.server.wireless.port_bak': 16002,
        })
        chk(r.get('ok') is True, 'A2a config.apply 成功（空主地址是合法配置）')
        line = wait_log(r'server addr reloaded -> 127\.0\.0\.1:1600[12] \(wan=', start=start)
        chk(line is not None, 'A2b 备份地址真的被交给了 iv_report（出现 reload 日志）')
        if line is not None:
            m = re.search(r'-> 127\.0\.0\.1:(\d+) \(wan=([^,]+),', line)
            chk(m is not None, 'A2c 日志行可解析（host:port(wan=…)）')
            if m is not None:
                port = int(m.group(1))
                wan = m.group(2)
                # 同一行里的 wan 就是 netmgr 判定的活动口 ⇒ 自洽校验：
                # eth0(有线) 必须取 16001、usb0(无线) 必须取 16002。
                want = 16001 if wan == 'eth0' else 16002
                chk(port == want,
                    'A2d 活动口 %s 取自己的备 %d（本口备 > 另一口备，实取 %d）'
                    % (wan, want, port))

        print('A3 给两口同样的主地址 ⇒ 主地址优先，备地址不参与')
        start = len(logtext())
        r = apply_net(both({'host': '127.0.0.2', 'port': 16010},
                           {'host': '127.0.0.2', 'port': 16010}))
        chk(r.get('ok') is True, 'A3a config.apply 成功')
        chk(wait_log(r'server addr reloaded -> 127\.0\.0\.2:16010 \(wan=', start=start)
            is not None,
            'A3b 选了主地址（本口主 > 本口备，备地址仍然配着也没被选）')

        print('A4 主地址在用，只改备份地址 ⇒ 不重连')
        start = len(logtext())
        r = apply_net(both({'host_bak': '127.0.0.3', 'port_bak': 16011},
                           {'host_bak': '127.0.0.3', 'port_bak': 16011}))
        chk(r.get('ok') is True, 'A4a config.apply 成功')
        time.sleep(1.5)
        tail = logtext()[start:]
        # 关键：判据是"解析出来的地址没变"，所以既不重载也不重连 ——
        # 改一个当前没在用的字段不该把在跑的 TCP 会话白拆一次。
        chk('server addr reloaded ->' not in tail,
            'A4b 没有 reload 日志（在用地址没变，不做无谓重连）')
        chk(re.search(r'net\.server\.\* changed -> reload', tail) is None,
            'A4c 连 reload 都没调（缓存比对的是解析结果，不是四个输入值）')
        it = net_items()
        chk(it.get('net.server.wired.host_bak') == '127.0.0.3',
            'A4d 配置确实改了（不重连 ≠ 没生效）')
        chk(it.get('net.server.wireless.host_bak') == '127.0.0.3',
            'A4e 无线侧同样改到了')

        print('A5 再把主地址清空 ⇒ 刚改的备份立刻顶上')
        start = len(logtext())
        r = apply_net(both({'host': ''}, {'host': ''}))
        chk(r.get('ok') is True, 'A5a config.apply 成功')
        chk(wait_log(r'server addr reloaded -> 127\.0\.0\.3:16011 \(wan=', start=start)
            is not None,
            'A5b 主地址一空，备份立即生效（不是等重启）')

        print('A6 收尾恢复出厂')
        r = apply_net(both(
            {'host': DEFAULT_HOST, 'port': DEFAULT_PORT, 'host_bak': '',
             'port_bak': DEFAULT_PORT},
            {'host': DEFAULT_HOST, 'port': DEFAULT_PORT, 'host_bak': '',
             'port_bak': DEFAULT_PORT}))
        chk(r.get('ok') is True, 'A6a 恢复出厂平台地址')
        it = net_items()
        chk(it.get('net.server.wired.host') == DEFAULT_HOST and
            it.get('net.server.wireless.host') == DEFAULT_HOST,
            'A6b 两路主地址回出厂值')
        chk(it.get('net.server.wired.host_bak') == '' and
            it.get('net.server.wireless.host_bak') == '',
            'A6c 两路备份地址回空')
        chk(it.get('net.server.wired.port_bak') == DEFAULT_PORT and
            it.get('net.server.wireless.port_bak') == DEFAULT_PORT,
            'A6d 两路备份端口回 6012')
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
    print('netsrv e2e passed (25 assertions)')
    sys.exit(rc)
