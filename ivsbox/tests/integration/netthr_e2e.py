#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`netthr.*` 热生效的装配层端到端取证（VM/板端以 root 跑真 ivsboxd）

为什么必须有这一层：`iv_agent_reload_netthr()` 的单测（`test_agent_netthr`）只证明
"给它一组值它能落到模块"。**"改配置会不会走到它"** 取决于 `main.c` 的订阅掩码 ——
掩码漏了 class，分支写得再对也永远不触发，编译、单测全绿、运行期零反应
（2026-10-21 的 `probe.*`、10-22 的 `camera.detect.*` 都这么踩过）。只有真拉起
进程、真下发 `config.apply` 才看得见。

这一轮还额外钉住一条**口径变更**：
  `netthr.loss_count` 从此是切换门限（`iv_netmgr.cfg.fail_n`）的唯一来源，
  `probe.max_fail` 只管探测判 DOWN。此前两处同源 ⇒ 网页上「连续失败次数上限」
  和「丢包次数上限」两个键实际只有一个算数。C3 就是这条的对照：改 `probe.max_fail`
  必须**不再**带动 `loss_count`。

判据是 `netthr.status`（ctl，读模块真值）＋ 落盘日志（ivsboxd 不往 stdout 打日志）：
  C1  出厂基线：loss_count=3 / probe_fail_n=3 / reboot_max=0 / cooldown=60 / credit 全 0
  C2  改 netthr.loss_count ⇒ 回读 7（**不重启**），probe_fail_n 仍 3
  C3  改 probe.max_fail ⇒ probe_fail_n 变 9，loss_count **仍 7**（两处已不同源）
  C4  改 netthr.reboot_max=2 ⇒ 回读 2，credit 全 2（限次真的发下去了）
  C5  改 netthr.reboot_interval_s=25 ⇒ 回读 25，reboot_max 仍 2
  C6  改同分类的无关键 netthr.timeout_ms ⇒ 回调**真发生了**但早返回（不重置 credit）
  C7  完全同值 apply ⇒ 配置层就判定无变化，连回调都没有（早返回是第二道防线）
  C8  收尾全恢复默认并核对（含原本被改成 9 的 probe.max_fail）

用法（必须先 `make build`）：
    cd <repo>/ivsbox
    sudo python3 tests/integration/netthr_e2e.py
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
# 日志按小时分文件、跨轮复用同一个文件，不记偏移的话上一轮的行会被当成
# 这一轮的证据（第一次跑证伪时就吃过这个亏：该红的地方照样绿）。
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


def status():
    """`netthr.status` 的 d 段（读模块真值，不是配置值）。"""
    return ctl('netthr.status').get('d') or {}


def apply_items(items):
    ver = (ctl('config.class', {'cls': 'netthr'}).get('d') or {}).get('version')
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

        print('C1 出厂基线')
        st = status()
        chk(st.get('loss_count') == 3, 'C1a loss_count = 3（netthr.loss_count 默认）')
        chk(st.get('probe_fail_n') == 3, 'C1b probe_fail_n = 3（probe.max_fail 默认）')
        chk(st.get('reboot_max') == 0, 'C1c reboot_max = 0（自愈出厂禁用）')
        chk(st.get('reboot_interval_s') == 60, 'C1d reboot_interval_s = 60')
        chk(st.get('credit') == [0] * 8, 'C1e credit 全 0（禁用即无次数）')

        print('C2 改 netthr.loss_count ⇒ 热生效，且不碰探测门限')
        r = apply_items({'netthr.loss_count': 7})
        chk(r.get('ok') is True, 'C2a config.apply 成功')
        chk(wait_log(r'netthr\.loss_count -> 7') is not None,
            'C2b 出现 netthr.loss_count -> 7（订阅掩码含 NETTHR）')
        st = status()
        chk(st.get('loss_count') == 7, 'C2c 回读 loss_count = 7（不重启主控）')
        chk(st.get('probe_fail_n') == 3, 'C2d probe_fail_n 不受影响（各管各的）')

        print('C3 改 probe.max_fail ⇒ 只动探测门限，**不再**带动切换门限')
        before = len(logtext())
        r = apply_items({'probe.max_fail': 9})
        chk(r.get('ok') is True, 'C3a config.apply 成功')
        chk(wait_log(r'probe reloaded: .*fail_n=9') is not None,
            'C3b 出现 probe reloaded … fail_n=9')
        st = status()
        chk(st.get('probe_fail_n') == 9, 'C3c 回读 probe_fail_n = 9')
        chk(st.get('loss_count') == 7,
            'C3d loss_count 仍 7（2026-10-22 起两处不再同源）')
        chk('netthr.loss_count ->' not in logtext()[before:],
            'C3e 改 probe.max_fail 不会触发 netthr 门限热更')

        print('C4 改 netthr.reboot_max ⇒ 自愈限次真的发到模块')
        r = apply_items({'netthr.reboot_max': 2})
        chk(r.get('ok') is True, 'C4a config.apply 成功')
        chk(wait_log(r'netthr recovery -> reboot_max=2 cooldown=60s') is not None,
            'C4b 出现 netthr recovery -> reboot_max=2 cooldown=60s')
        st = status()
        chk(st.get('reboot_max') == 2, 'C4c 回读 reboot_max = 2')
        chk(st.get('reboot_interval_s') == 60, 'C4d 冷却未动，仍 60 s')
        chk(st.get('credit') == [2] * 8,
            'C4e credit 全 2（限次真的发下去了，不是只改了配置）')

        print('C5 改 netthr.reboot_interval_s ⇒ 只动冷却')
        r = apply_items({'netthr.reboot_interval_s': 25})
        chk(r.get('ok') is True, 'C5a config.apply 成功')
        chk(wait_log(r'netthr recovery -> reboot_max=2 cooldown=25s') is not None,
            'C5b 出现 netthr recovery -> reboot_max=2 cooldown=25s')
        st = status()
        chk(st.get('reboot_interval_s') == 25, 'C5c 回读 reboot_interval_s = 25')
        chk(st.get('reboot_max') == 2, 'C5d reboot_max 仍 2')

        print('C6 同分类的无关键变化 ⇒ 早返回，不重置 credit')
        # 关键：必须让 `on_cfg_committed(NETTHR)` **真的被回调**，否则测的是
        # "配置层认为没变化"（那是 C7 的事），不是 agent 侧的同值早返回。
        # 所以改 `netthr.timeout_ms` —— 它与自愈/门限全无关，但同属 NETTHR 类。
        before = len(logtext())
        r = apply_items({'netthr.timeout_ms': 4000})
        chk(r.get('ok') is True, 'C6a config.apply 成功')
        time.sleep(1.0)
        tail = logtext()[before:]
        chk('netthr recovery ->' not in tail and 'netthr.loss_count ->' not in tail,
            'C6b 早返回：不出热更日志（iv_recovery_set_limits 会重置 credit，不能白调）')
        st = status()
        chk(st.get('reboot_max') == 2 and st.get('reboot_interval_s') == 25 and
            st.get('credit') == [2] * 8,
            'C6c 自愈状态一字未动')

        print('C7 完全同值 apply ⇒ 配置层就判定无变化')
        before = len(logtext())
        r = apply_items({'netthr.reboot_max': 2})
        chk(r.get('ok') is True, 'C7a config.apply 成功')
        time.sleep(1.0)
        tail = logtext()[before:]
        chk('netthr recovery ->' not in tail,
            'C7b 无回调也就不可能重置（第一道防线是配置层的变化判定）')
        st = status()
        chk(st.get('reboot_max') == 2 and st.get('credit') == [2] * 8,
            'C7c 自愈状态原样')

        print('C8 收尾恢复默认')
        r = apply_items({'netthr.loss_count': 3, 'netthr.reboot_max': 0,
                         'netthr.reboot_interval_s': 60, 'netthr.timeout_ms': 2000})
        chk(r.get('ok') is True, 'C8a 恢复 netthr.* 默认')
        r = apply_items({'probe.max_fail': 3})
        chk(r.get('ok') is True, 'C8b 恢复 probe.max_fail=3')
        st = status()
        chk(st.get('loss_count') == 3 and st.get('probe_fail_n') == 3,
            'C8c 两个门限都回默认 3')
        chk(st.get('reboot_max') == 0 and st.get('reboot_interval_s') == 60,
            'C8d 自愈回默认（禁用 / 60 s）')
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
    print('netthr e2e passed (33 assertions)')
    sys.exit(rc)
