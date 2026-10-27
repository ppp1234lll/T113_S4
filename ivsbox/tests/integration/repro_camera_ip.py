#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""复现：平台连配**多路**「配置摄像机 IP」(0xF7) 后主控是否还活着。

2026-10-27 08:51 板端真实现场：平台先配第 1 路（成功，日志有 applied），
31 s 后再配第 2 路 —— 设备日志在这一行之后就**彻底停了 31 秒**（连每 30 s 的
心跳都没有），随后 08:52:03 一次 `unclean shutdown`（看门狗复位）。
平台因此收不到应答 ⇒ 界面报「配置第二路摄像机失败」。

本脚本只回答一个问题：**连续配置多路**之后主控还响不响应 ctl。
每配一路就做一次 `system.snapshot`（走同一条 reactor 线程），超时即判"卡死"。
卡死时用 gdb 抓一次线程栈（host 构建是 -O0 -g）。

用法（必须先 `make host`）：
    cd <repo>/ivsbox
    sudo python3 tests/integration/repro_camera_ip.py
前置：当前没有别的 ivsboxd 在跑（本脚本自己拉起，结束时收掉并还原配置）。
"""
import os
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import platcmd_e2e as E  # noqa: E402


def alive(tag, timeout_note=''):
    """ctl 还能不能应答 —— 这是"主控没卡死"最直接的判据。"""
    t0 = time.time()
    try:
        E.ctl('system.snapshot')
        print('  ALIVE %-46s (%.2fs) %s' % (tag, time.time() - t0, timeout_note))
        return True
    except Exception as e:                      # noqa: BLE001
        print('  DEAD  %-46s (%s) %s' % (tag, e, timeout_note))
        return False


def gdb_dump(pid):
    print('--- gdb thread apply all bt (pid %d) ---' % pid)
    try:
        out = subprocess.check_output(
            ['gdb', '-p', str(pid), '-batch', '-ex', 'thread apply all bt'],
            stderr=subprocess.STDOUT, timeout=120).decode('utf-8', 'replace')
    except Exception as e:                      # noqa: BLE001
        out = 'gdb failed: %r' % (e,)
    print(out[-6000:])


def main():
    if E.wait_ctl(0.1):
        print('FAIL: 已经有 ivsboxd 在跑')
        return 1

    ifn, ip, plen = E.local_addr()
    if ip is None:
        print('FAIL: 取不到本机出口地址')
        return 1
    print('冒充平台 %s:%d' % (ip, E.PLAT_PORT))

    plat = E.Plat(E.PLAT_PORT)
    p = subprocess.Popen([E.BIN], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
    pid = p.pid
    rc = 0
    try:
        if not E.wait_ctl():
            print('FAIL: ivsboxd 没起来')
            return 1
        E.BASE = len(E.logtext())

        orig_cam = E.cls_items('camera')
        orig_net = E.cls_items('net')

        # 关自愈 ＋ 平台地址指到本机（同 platcmd_e2e 的安全前置）
        ver = (E.ctl('config.class', {'cls': 'netthr'}).get('d') or {}).get('version')
        E.ctl('config.apply', {'ver': ver, 'items': {'netthr.reboot_max': 0}})
        E.apply_cls('net', {
            'net.server.wired.host': ip, 'net.server.wired.port': E.PLAT_PORT,
            'net.server.wired.host_bak': ip, 'net.server.wired.port_bak': E.PLAT_PORT,
            'net.server.wireless.host': ip, 'net.server.wireless.port': E.PLAT_PORT,
            'net.server.wireless.host_bak': ip,
            'net.server.wireless.port_bak': E.PLAT_PORT,
        })
        t0 = time.time()
        while time.time() - t0 < 25.0 and not plat.online():
            time.sleep(0.2)
        if not plat.online():
            print('FAIL: 设备没连进来')
            return 1
        time.sleep(1.0)
        print('平台已连上，开始连配 6 路')
        print('  原始 camera.ch.N.ip = %s'
              % [orig_cam.get('camera.ch.%d.ip' % i) for i in range(6)])

        # 目标 IP 可用 argv 指定：板端那次卡死时配的是**真实可达的摄像机**
        # （192.168.2.30 / .31，PING 有回包），而 host 上 192.168.9.x 无路由、
        # 永远没回包。两种情形的差别就在这里，所以默认值改成"本机可达"的几个。
        targets = sys.argv[1:] or ['192.168.9.201', '192.168.9.202',
                                   '192.168.9.203', '192.168.9.204',
                                   '192.168.9.205', '192.168.9.206']

        for ch in range(1, len(targets) + 1):
            tgt = targets[ch - 1]
            payload = bytes([ch]) + socket.inet_aton(tgt)
            plat.clear()
            t_start = len(E.logtext())
            ok = plat.send(E.build_downlink(0xF7, payload))
            f = plat.wait(0xF7, kind='bin', timeout=6.0)
            print('ch%d %s -> ack=%s (send=%s)'
                  % (ch, tgt, ('0x%02X' % f['code']) if f else 'NONE', ok))
            time.sleep(0.3)
            if not alive('ch%d 之后 ctl 响应' % ch):
                rc = 2
                gdb_dump(pid)
                break
            tail = E.logtext()[t_start:]
            for ln in tail.splitlines():
                if 'camera' in ln or '0xF7' in ln or 'camdet' in ln:
                    print('    | ' + ln)
            plat.clear()
    finally:
        try:
            plat.close()
        except Exception:                       # noqa: BLE001
            pass
        try:
            if pid and os.path.exists('/proc/%d' % pid):
                os.kill(pid, signal.SIGKILL)
        except Exception:                       # noqa: BLE001
            pass
        # 还原现场配置（按原值，不按出厂值猜）
        try:
            if E.wait_ctl(5.0):
                keep = {'camera.ch.%d.ip' % i: orig_cam.get('camera.ch.%d.ip' % i, '')
                        for i in range(6)}
                keep['camera.detect.mode'] = orig_cam.get('camera.detect.mode', 1)
                E.apply_cls('camera', keep)
                net_keep = {k: orig_net.get(k) for k in (
                    'net.server.wired.host', 'net.server.wired.port',
                    'net.server.wired.host_bak', 'net.server.wired.port_bak',
                    'net.server.wireless.host', 'net.server.wireless.port',
                    'net.server.wireless.host_bak', 'net.server.wireless.port_bak')
                    if k in orig_net}
                E.apply_cls('net', net_keep)
                print('现场配置已还原')
        except Exception as e:                  # noqa: BLE001
            print('还原失败（需手工核对）：%r' % (e,))
    print('退出码 %d' % rc)
    return rc


if __name__ == '__main__':
    sys.exit(main())
