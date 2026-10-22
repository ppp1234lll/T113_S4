#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""摄像机检测方式热生效 ＋ 单路重测（camera.test）的装配层端到端取证

为什么必须有这一层：`iv_camdet_probe_now()` / `iv_camdet_set_mode()` 的单测只证明
"给它这些东西它能干对活"。**"网页改配置会不会走到它"** 取决于 `main.c` 的订阅掩码
和 `on_cfg_committed()` 的分支 —— 掩码漏了 class，分支写得再对也永远不触发，
编译、单测、集成用例全绿，运行期零反应（`probe.*` 在 2026-10-21 就是这么踩的）。
只有真拉起进程、真下发 `config.apply` 才看得见。

判据分两路：
  - **行为**：`sys.overview` 的 `cameras.mode` 与 `camera.test` 的返回值；
  - **日志**：`ivsboxd` 不往 stdout 打日志，只看 `/mnt/UDISK/log` 的追加部分
    （日志按小时分文件、跨轮复用 ⇒ 必须记 BASE 偏移，否则上一轮的
    `mode -> rtsp` 会被当成这一轮的证据）。

  C1  记下初始检测方式（出厂 1=PING）
  C2  改 camera.detect.mode=3 ⇒ overview 的 cameras.mode 立刻变 3，日志出 mode -> rtsp
  C3  改 camera.detect.mode=2 ⇒ 立刻变 2，日志出 mode -> port
  C4  camera.test 参数校验：ch 越界 / 缺 ch 都是 IV_EINVAL(-2)
  C5  没配 IP 的那一路 ⇒ IV_ENOENT(-10)，前端据此提示"先配 IP"
  C6  改 camera.ch.0.ip ⇒ 日志出 channel table reloaded（**IP 也热生效**）；
      随后 camera.test ch=1 变成"已排上"（ok / queued / ch=1）
  C7  RTSP 方式下 camera.test 的 mode_name=rtsp、expect_ms=1500（不是 PING 的 20 s）
  C8  还原配置后再测 ⇒ 又回到 IV_ENOENT（证明确实是"配了 IP 才测得动"）

用法（必须先 `make build`；VM 上到 `~/wbnet/ivsbox` 跑，见 tests/integration/readme.txt）：
    cd <repo>/ivsbox
    sudo python3 tests/integration/camtest_e2e.py
前置：`/mnt/UDISK/log` 与 `/mnt/UDISK/ivsbox` 可写（root）；当前**没有**别的
ivsboxd 在跑（本脚本自己拉起 build/host/ivsboxd，结束时收掉）。
**副作用（如实记账）**：C6 会按需建出 `camera.ch.0.*` 这一槽的 8 个键（与网页上
点「建立该路通道」同一路径），结束时把 `camera.ch.0.ip` 置回空串、检测方式置回
初始值 —— 行为等同出厂，但键本身留在配置里。想要绝对干净就在临时目录里跑。
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

IV_EINVAL = -2
IV_ENOENT = -10

fails = []
rid = 0
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


def wait_log(pattern, deadline=8.0):
    """等某条日志出现（只认 BASE 之后的追加内容）；返回最后匹配行，None = 超时。"""
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


def cam_mode():
    """camdet 的**真实**检测方式（不是配置里那个值 —— 两者不一致正是本用例要抓的）。"""
    r = ctl('sys.overview')
    cams = (r.get('d') or {}).get('cameras') or {}
    return cams.get('mode')


def apply_items(items, cls='camera'):
    r = ctl('config.class', {'cls': cls})
    ver = (r.get('d') or {}).get('version')
    return ctl('config.apply', {'ver': ver, 'items': items})


def main():
    global BASE

    if wait_ctl(0.1):
        print('FAIL: 已经有 ivsboxd 在跑，换个环境再测')
        return 1

    p = subprocess.Popen([BIN], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    init_mode = None
    try:
        if not wait_ctl():
            print('FAIL: ivsboxd 没起来')
            return 1

        BASE = len(logtext())

        print('C1 记下初始检测方式')
        init_mode = cam_mode()
        chk(init_mode in (1, 2, 3), 'C1a overview 给出了检测方式，实得 ' + repr(init_mode))

        print('C2 切 RTSP拉流测试（camera.detect.mode=3）')
        r = apply_items({'camera.detect.mode': 3})
        chk(r.get('ok') is True, 'C2a config.apply 成功')
        time.sleep(0.5)
        chk(cam_mode() == 3,
            'C2b **不重启**就生效：overview 的 cameras.mode 变 3（订阅掩码含 CAMERA），实得 '
            + repr(cam_mode()))
        chk(wait_log(r'mode -> rtsp') is not None, 'C2c 日志出现 mode -> rtsp')

        print('C3 切端口扫描（camera.detect.mode=2）')
        r = apply_items({'camera.detect.mode': 2})
        chk(r.get('ok') is True, 'C3a config.apply 成功')
        time.sleep(0.5)
        chk(cam_mode() == 2, 'C3b 再切同样立刻生效，实得 ' + repr(cam_mode()))
        chk(wait_log(r'mode -> port') is not None, 'C3c 日志出现 mode -> port')

        print('C4 camera.test 参数校验')
        r = ctl('camera.test', {'ch': 7})
        chk(r.get('ok') is False and r.get('e') == IV_EINVAL,
            'C4a ch=7 越界 ⇒ IV_EINVAL(%d)，实得 %r' % (IV_EINVAL, r.get('e')))
        r = ctl('camera.test', {})
        chk(r.get('ok') is False and r.get('e') == IV_EINVAL,
            'C4b 缺 ch ⇒ IV_EINVAL(%d)' % IV_EINVAL)

        print('C5 没配 IP 的那一路')
        r = ctl('camera.test', {'ch': 1})
        chk(r.get('ok') is False and r.get('e') == IV_ENOENT,
            'C5a 未配 IP ⇒ IV_ENOENT(%d)（前端据此提示"先配 IP"），实得 %r'
            % (IV_ENOENT, r.get('e')))

        print('C6 配 IP 也热生效（camera.ch.0.ip）')
        r = apply_items({'camera.ch.0.ip': '10.99.99.1'})
        chk(r.get('ok') is True, 'C6a config.apply 成功')
        line = wait_log(r'camdet channel table reloaded')
        chk(line is not None, 'C6b 日志出现 channel table reloaded（IP 不再是"重启才生效"）')
        r = ctl('camera.test', {'ch': 1})
        chk(r.get('ok') is True, 'C6c 现在能测了（ok）')
        d = r.get('d') or {}
        chk(d.get('queued') is True, 'C6d queued=true（接口不等结果，等会把 Reactor 卡住）')
        chk(d.get('ch') == 1, 'C6e 回显的 ch 是 1（1 基，与页面「摄像机1」同序）')

        print('C7 RTSP 方式的返回值')
        apply_items({'camera.detect.mode': 3})
        time.sleep(0.5)
        d = (ctl('camera.test', {'ch': 1}).get('d') or {})
        chk(d.get('mode') == 3 and d.get('mode_name') == 'rtsp',
            'C7a mode=3 / mode_name=rtsp，实得 %r/%r' % (d.get('mode'), d.get('mode_name')))
        chk(d.get('expect_ms') == 1500,
            'C7b expect_ms=1500（RTSP 单次 OPTIONS 1 s；不是 PING 的 20 s），实得 %r'
            % d.get('expect_ms'))

        print('C8 还原：清 IP、复位检测方式')
        r = apply_items({'camera.ch.0.ip': ''})
        chk(r.get('ok') is True, 'C8a 清 IP 成功')
        if init_mode in (1, 2, 3):
            apply_items({'camera.detect.mode': init_mode})
            time.sleep(0.3)
            chk(cam_mode() == init_mode,
                'C8b 检测方式复位为 %r，实得 %r' % (init_mode, cam_mode()))
        r = ctl('camera.test', {'ch': 1})
        chk(r.get('ok') is False and r.get('e') == IV_ENOENT,
            'C8c 清掉 IP 后又回到 IV_ENOENT（证明"能测"确实来自刚配的 IP）')
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
    print('camtest e2e passed (19 assertions)')
    sys.exit(rc)
