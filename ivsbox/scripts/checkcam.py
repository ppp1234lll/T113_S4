#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从设备侧探测一台摄像机是否可用（PC 端 cam.bat 建隧道前调用）。

用法:
    checkcam.py <ip> [port]        port 默认 80

输出（单行，给 bat 判断）:
    OK         80 端口能连上，可以转发
    NG:PING    网络不通（连 ping 都不通）
    NG:PORT    ping 通但 80 连不上（多半是端口/服务问题）
    NG:ARG     参数不对

为什么先测端口再 ping：内网 TCP connect 是毫秒级，通的摄像机几乎没有等待；
ping 不通不代表不能用（有些摄像机默认禁 ICMP），所以以端口为准。
"""
import socket
import subprocess
import sys


def ping_ok(ip):
    try:
        return subprocess.call(['ping', '-c', '2', '-W', '1', ip],
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL) == 0
    except Exception:
        return False


def port_ok(ip, port, timeout=3):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        return s.connect_ex((ip, port)) == 0
    except Exception:
        return False
    finally:
        s.close()


def main():
    if len(sys.argv) < 2:
        print('NG:ARG')
        return 1
    ip = sys.argv[1]
    try:
        port = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    except ValueError:
        print('NG:ARG')
        return 1

    if port_ok(ip, port):
        print('OK')
        return 0
    if ping_ok(ip):
        print('NG:PORT')
        return 3
    print('NG:PING')
    return 2


if __name__ == '__main__':
    sys.exit(main())
