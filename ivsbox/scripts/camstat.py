#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""设备侧状态汇总，给 PC 端 cam.bat 的 S（状态）用。

一次 ssh 就把要看的都取回来：设备主机名、4G 地址、有线地址、隧道(frpc)在不在、
以及每一路摄像机的连通性。以前是让 ssh 在远端跑一长串 shell（含 $( ) 和 |），
在 cmd.exe 里引号会被解释坏，报"系统找不到指定的路径"，所以改成脚本。

注意：**输出全部是 ASCII**。Windows 控制台是 936 代码页，设备这边打印中文过去
会变成乱码（固件里也是这么处理的：日志标签一律英文）。
"""
import json
import re
import socket
import subprocess
import sys

CONF = '/mnt/UDISK/ivsbox/config/ivsbox.json'
CAM_PORT = 80


def strip_line_comments(text):
    return '\n'.join(l for l in text.splitlines() if not l.strip().startswith('//'))


def load_cams():
    """返回 [(序号, ip)]，只含 ip 非空的通道。"""
    try:
        raw = open(CONF, encoding='utf-8').read()
    except Exception as exc:
        sys.stderr.write('open %s: %s\n' % (CONF, exc))
        return []
    try:
        doc = json.loads(strip_line_comments(raw))
        rows, idx = [], 0
        for ch in ((doc.get('camera') or {}).get('ch') or []):
            idx += 1
            if not isinstance(ch, dict):
                continue
            ip = (ch.get('ip') or '').strip()
            if ip:
                rows.append((idx, ip))
        return rows
    except Exception:
        return [(i, ip) for i, ip in enumerate(
            re.findall(r'"ip"\s*:\s*"(\d+\.\d+\.\d+\.\d+)"', raw), 1)]


def iface_ip(name):
    try:
        out = subprocess.check_output(['ip', '-4', 'addr', 'show', name],
                                      stderr=subprocess.DEVNULL).decode()
    except Exception:
        return '-'
    m = re.search(r'inet\s+(\d+\.\d+\.\d+\.\d+/\d+)', out)
    return m.group(1) if m else '-'


def tunnel_on():
    for pid in ('frpc', 'frpc_arm'):
        try:
            if subprocess.call(['pidof', pid],
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL) == 0:
                return 'ON'
        except Exception:
            pass
    return 'OFF'


def port_ok(ip, port=CAM_PORT, timeout=2):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        return s.connect_ex((ip, port)) == 0
    except Exception:
        return False
    finally:
        s.close()


def ping_ok(ip):
    try:
        return subprocess.call(['ping', '-c', '2', '-W', '1', ip],
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL) == 0
    except Exception:
        return False


def cam_state(ip):
    if port_ok(ip):
        return 'OK'
    if ping_ok(ip):
        return 'NG:PORT'
    return 'NG:PING'


def main():
    print('host = %s' % socket.gethostname())
    print('lan  = %s' % iface_ip('eth0'))
    print('4g   = %s' % iface_ip('usb0'))
    print('tun  = %s' % tunnel_on())
    print('----')
    for idx, ip in load_cams():
        print('cam%-2d %-15s %s' % (idx, ip, cam_state(ip)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
