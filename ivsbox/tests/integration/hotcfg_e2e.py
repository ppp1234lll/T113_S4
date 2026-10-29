#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`camera.ch.N.url`/`.record`/`camera.rec.*` 与 `camera.snmp.*`/`switch.snmp.*`
热生效的装配层端到端取证（VM/板端以 root 跑真 ivsboxd）

为什么必须有这一层：`iv_agent_reload_rec()` / `iv_agent_reload_snmp()` 的单测
（`test_agent_hotcfg`、`test_recctl` c08）只证明"给它一组值它能落到模块/状态机"。
**"改配置会不会走到它"** 取决于 `main.c` 的 `on_cfg_committed()` 订阅掩码 ——
掩码漏了 class，分支写得再对也永远不触发，编译、单测全绿、运行期零反应。
2026-10-21 的 `probe.*`、10-22 的 `camera.detect.*` 都这么踩过，所以这两组键
（CAMERA 与 SWITCH 两个 class）必须真拉起进程、真下发 `config.apply` 才认。

判据：
  - **模块真值**：`snmp.result` 直接读 `iv_agent_snmp_job_t.slot[].oid`
    （作业被重建过才会变成新 OID，读的不是配置值）；
  - **落盘日志**：`ivsboxd` 不往 stdout 打日志，热更成功会打
    `snmp <目标名> reloaded: …` / `rec sources reloaded: …`（这几行**只有热更
    路径**会打 —— 重启后开机装配走的是 open 分支，所以它们同时是"没重启"的证据）。
    目标名是 `cam0`..`cam5` / `switch`（6 路各一条 ＋ 交换机，2026-10-29 扩表；
    早先打的是 `snmp target 0/1`）。

  H1 基线：`camera.snmp.hik.*` 默认 8 个槽位、`sysName` 槽＝1.3.6.1.2.1.1.5.0
  H2 改 `camera.snmp.hik.sysName` ⇒ `snmp.result` 立即是新 OID ＋ 热更日志
  H3 改 `camera.ch.0.url` + `.record=true` ⇒ 录像源表 reload（urls=1/1）
  H4 改 `camera.rec.hold_ms` / `.debounce_ms` ⇒ 同一条链上再 reload，参数跟着变
  H5 改 `switch.snmp.community` ⇒ **SWITCH 类**也订阅了；同时摄像机目标一个字不动
  H6 改 camera 类里毫不相干的 `camera.detect.rtt_ms` ⇒ 回调真发生了但**零热更**
     （整类提交是常态：早返回不是优化，是不把就绪结果/在录会话白白拆掉）
  H7 完全同值 apply ⇒ 配置层就判定无变化，连回调都没有（第二道防线）
  H8 收尾全恢复默认并核对（含 `snmp.result` 回到默认 OID）
  H9 改 `camera.ch.N.vendor` ⇒ 该路选另一套 OID 表（6 路各按品牌，判据＝槽位数）

用法（必须先 `make build`）：
    cd <repo>/ivsbox
    sudo python3 tests/integration/hotcfg_e2e.py
前置：`/mnt/UDISK/log` 可写；当前**没有**别的 ivsboxd 在跑（本脚本自己拉起
build/host/ivsboxd，结束时收掉，并在收掉前把改过的键恢复默认）。
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

DEFAULT_SYSNAME_OID = '1.3.6.1.2.1.1.5.0'
NEW_SYSNAME_OID = '1.3.6.1.4.1.39165.9.9.0'

fails = []
total = 0   # 断言条数（收尾打印用；硬编码数字迟早和实际条数对不上）
rid = 0

# 本次拉起 daemon 时的日志长度；只认这之后的追加内容。
# 日志按小时分文件、跨轮复用同一个文件，不记偏移的话上一轮的行会被当成
# 这一轮的证据（第一次跑证伪时就吃过这个亏：该红的地方照样绿）。
BASE = 0


def chk(cond, what):
    global total
    total += 1
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


def tail():
    return logtext()[BASE:]


def wait_log(pattern, deadline=8.0):
    """等某条日志出现；返回最后一次匹配到的行（None = 超时）。"""
    t0 = time.time()
    last = None
    while time.time() - t0 < deadline:
        for line in tail().splitlines():
            if re.search(pattern, line):
                last = line
        if last is not None:
            return last
        time.sleep(0.2)
    return last


def quiet(before, seconds=1.0):
    """让出一点时间，确认"该没有的日志"真的没来。
    ⚠ 返回的是 `before` **之后**的增量，不是 `BASE` 之后的 —— 用 `tail()` 会把
    前面几步的热更日志也算进来，断言恒假（第一次跑就踩了）。"""
    time.sleep(seconds)
    return logtext()[before:]


def apply_items(cls, items):
    ver = (ctl('config.class', {'cls': cls}).get('d') or {}).get('version')
    return ctl('config.apply', {'ver': ver, 'items': items})


def cam_slots(which='cam'):
    """`snmp.result` 的 slots 段（读作业真值，不是配置值）。
    `which` 认 `cam` / `cam0`..`cam5` / `switch`（2026-10-29 扩到 6 路 ＋ 交换机）。"""
    return (ctl('snmp.result', {'which': which}).get('d') or {}).get('slots') or []


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

        print('H1 基线')
        # 摄像机默认 **8/9** 条：`n_oid` 是"最后一个非空槽位下标 + 1"的位置语义，
        # 而 `camera.snmp.hik.devStatus` 出厂默认就是空串（海康 MIB 无此节点、
        # 留空＝不采）。别把 9 写死 —— 那样改的是出厂默认，不是这条逻辑。
        slots = cam_slots()
        chk(len(slots) == 8, 'H1a 摄像机 SNMP 作业 8 个槽位（9 条里 devStatus 默认空）')
        chk(len(slots) > 2 and slots[2].get('oid') == DEFAULT_SYSNAME_OID,
            'H1b 槽位 sysName 的 OID 是默认值')
        chk(len(slots) > 2 and slots[2].get('name') == 'sysName',
            'H1c 槽位名来自代码表、不来自配置')

        print('H2 改 camera.snmp.hik.sysName ⇒ 不重启即生效')
        r = apply_items('camera', {'camera.snmp.hik.sysName': NEW_SYSNAME_OID})
        chk(r.get('ok') is True, 'H2a config.apply 成功')
        chk(wait_log(r'snmp cam0 reloaded: .*slots=8') is not None,
            'H2b 出现 snmp cam0 reloaded（CAMERA 类订阅掩码含 SNMP 热更）')
        slots = cam_slots()
        chk(len(slots) > 2 and slots[2].get('oid') == NEW_SYSNAME_OID,
            'H2c snmp.result 立即是新 OID（读的是重建后的作业，不是配置）')

        print('H3 改 camera.ch.0.url + .record ⇒ 录像源表热更')
        before = len(logtext())
        r = apply_items('camera', {'camera.ch.0.url': 'rtsp://10.0.0.11/ch0',
                                   'camera.ch.0.record': True})
        chk(r.get('ok') is True, 'H3a config.apply 成功')
        line = wait_log(r'rec sources reloaded: urls=1/1 hold=10000ms debounce=60000ms')
        chk(line is not None, 'H3b 出现 rec sources reloaded urls=1/1（默认 hold/debounce）')
        # 同一类里 SNMP 目标没变 ⇒ 不许跟着重建（否则就绪结果每次都被清空）
        chk('snmp cam0 reloaded' not in logtext()[before:],
            'H3c 只改 camera.ch.* 时 SNMP 作业不重建（早返回）')

        print('H4 改 camera.rec.hold_ms / .debounce_ms ⇒ 同一条链上跟着走')
        r = apply_items('camera', {'camera.rec.hold_ms': 3000})
        chk(r.get('ok') is True, 'H4a config.apply 成功')
        chk(wait_log(r'rec sources reloaded: urls=1/1 hold=3000ms debounce=60000ms')
            is not None, 'H4b hold 热更到 3000 ms')
        r = apply_items('camera', {'camera.rec.debounce_ms': 20000})
        chk(r.get('ok') is True, 'H4c config.apply 成功')
        chk(wait_log(r'rec sources reloaded: urls=1/1 hold=3000ms debounce=20000ms')
            is not None, 'H4d debounce 热更到 20000 ms')

        print('H5 改 switch.snmp.community ⇒ SWITCH 类也订阅了')
        before = len(logtext())
        r = apply_items('switch', {'switch.snmp.community': 'swpriv'})
        chk(r.get('ok') is True, 'H5a config.apply 成功')
        chk(wait_log(r'snmp switch reloaded: .*slots=12') is not None,
            'H5b 出现 snmp switch reloaded（SWITCH 类掩码也对）')
        chk('snmp cam0 reloaded' not in logtext()[before:],
            'H5c 摄像机目标没变 ⇒ 不重建（逐目标比对，不是整包重建）')

        print('H6 camera 类里的无关键 ⇒ 回调真发生但零热更')
        # 关键：必须让 `on_cfg_committed(CAMERA)` **真的被回调**，否则测的是
        # "配置层认为没变化"（那是 H7 的事），不是 agent 侧的同值早返回。
        before = len(logtext())
        r = apply_items('camera', {'camera.detect.rtt_ms': 300})
        chk(r.get('ok') is True, 'H6a config.apply 成功')
        text = quiet(before, 1.2)
        chk('rec sources reloaded' not in text and 'snmp cam0 reloaded' not in text,
            'H6b 早返回：不出热更日志（否则每存一次配置就拆一轮在录会话）')
        slots = cam_slots()
        chk(len(slots) > 2 and slots[2].get('oid') == NEW_SYSNAME_OID,
            'H6c SNMP 作业原样（没被 memset 掉）')

        print('H7 完全同值 apply ⇒ 配置层就判定无变化')
        before = len(logtext())
        r = apply_items('camera', {'camera.rec.hold_ms': 3000})
        chk(r.get('ok') is True, 'H7a config.apply 成功')
        text = quiet(before, 1.0)
        chk('rec sources reloaded' not in text,
            'H7b 无回调也就不可能重建（第一道防线是配置层的变化判定）')

        print('H9 6 路各自按品牌选 OID 表（camera.ch.N.vendor）')
        # 用户口径「6 路各自按品牌采」：同一份 camera.snmp.* 参数下，每一路用哪套
        # OID 由**该路自己的 vendor** 决定。这是 `cfg_snmp_targets()` 里唯一的
        # 品牌分支，也是最容易只对一路生效的地方 —— 写成 `camera.ch.0.vendor`
        # 常量，第 2~6 路会集体错用第 1 路的表，界面上一点异样都看不出来。
        #
        # ⚠ **必须断言非 0 路**：只测 cam0 的话，写成常量那版照样全绿
        #   （cam0 用的就是 ch.0 的 vendor）。H9c 是这条分支唯一的守卫。
        #
        # 判据用**槽位数**而不是 OID 值：三套表里标准 MIB-II 那三项的 OID 一模一样，
        # 只有私有节点不同 —— 海康私有节点出厂有值（8 槽），大华留空（只剩 3 槽）。
        chk(len(cam_slots('cam0')) == 8, 'H9a 第 1 路未配 vendor ⇒ 回落海康（8 槽）')
        r = apply_items('camera', {'camera.ch.2.vendor': 'dahua'})
        chk(r.get('ok') is True, 'H9b config.apply 成功（第 3 路改大华）')
        chk(wait_log(r'snmp cam2 reloaded') is not None,
            'H9c 改第 3 路 vendor 触发的是 cam2 作业（不是 cam0）')
        chk(len(cam_slots('cam2')) == 3,
            'H9d 第 3 路走大华表 ⇒ 只剩标准 MIB-II 三项（私有节点出厂空＝不采）')
        chk(len(cam_slots('cam0')) == 8, 'H9e 第 1 路不受影响（还是海康 8 槽）')
        r = apply_items('camera', {'camera.ch.0.vendor': 'dahua'})
        chk(r.get('ok') is True, 'H9f config.apply 成功（第 1 路也改大华）')
        chk(wait_log(r'snmp cam0 reloaded') is not None, 'H9g 触发 cam0 重装')
        chk(len(cam_slots('cam0')) == 3, 'H9h 第 1 路也变成 3 槽')
        r = apply_items('camera', {'camera.ch.0.vendor': '', 'camera.ch.2.vendor': ''})
        chk(r.get('ok') is True, 'H9i config.apply 成功（换回默认＝海康）')
        chk(len(cam_slots('cam0')) == 8 and len(cam_slots('cam2')) == 8,
            'H9j 两路都回到 8 槽')

        print('H8 收尾恢复默认')
        r = apply_items('camera', {'camera.ch.0.url': '', 'camera.ch.0.record': False,
                                   'camera.rec.hold_ms': 10000,
                                   'camera.rec.debounce_ms': 60000,
                                   'camera.snmp.hik.sysName': DEFAULT_SYSNAME_OID,
                                   'camera.snmp.community': 'public',
                                   'camera.detect.rtt_ms': 200})
        chk(r.get('ok') is True, 'H8a 恢复 camera.* 默认')
        chk(wait_log(r'rec sources reloaded: urls=0/0 hold=10000ms debounce=60000ms')
            is not None, 'H8b 录像源表清空（urls=0/0）')
        r = apply_items('switch', {'switch.snmp.community': 'public'})
        chk(r.get('ok') is True, 'H8c 恢复 switch.snmp.community 默认')
        slots = cam_slots()
        chk(len(slots) > 2 and slots[2].get('oid') == DEFAULT_SYSNAME_OID,
            'H8d sysName 槽位回默认 OID')
    finally:
        # 收尾：把本轮可能留下的改动恢复（best-effort，别因为清理失败掩盖真问题）
        try:
            if wait_ctl(0.1):
                apply_items('camera', {'camera.ch.0.url': '', 'camera.ch.0.record': False,
                                       'camera.rec.hold_ms': 10000,
                                       'camera.rec.debounce_ms': 60000,
                                       'camera.snmp.hik.sysName': DEFAULT_SYSNAME_OID,
                                       'camera.snmp.community': 'public',
                                       'camera.detect.rtt_ms': 200})
                apply_items('switch', {'switch.snmp.community': 'public'})
        except Exception:
            pass
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
    print('hotcfg e2e passed (%d assertions)' % total)
    sys.exit(rc)
