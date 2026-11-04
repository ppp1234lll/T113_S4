#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""列出设备上已配置的摄像机。

输出每行： <序号> <IP> <品牌> <码流>
只输出 ip 非空的通道。访问端的 cam.bat 用它动态生成菜单和端口映射，
这样在 Web 上改过 camera.ch[].ip、或者增删通道之后，PC 那边不用改脚本。

配置文件是 JSONC（带 // 整行注释），标准 json 解析不了，所以先剥注释；
万一格式再变就用正则兜底，保证总能给出一个列表。
"""
import json
import re
import sys

CONF = '/mnt/UDISK/ivsbox/config/ivsbox.json'


def strip_line_comments(text):
    keep = []
    for line in text.splitlines():
        if line.strip().startswith('//'):
            continue
        keep.append(line)
    return '\n'.join(keep)


def main():
    try:
        raw = open(CONF, encoding='utf-8').read()
    except Exception as exc:
        sys.stderr.write('open %s: %s\n' % (CONF, exc))
        return 1

    rows = []
    try:
        doc = json.loads(strip_line_comments(raw))
        chs = (doc.get('camera') or {}).get('ch') or []
        idx = 0
        for ch in chs:
            idx += 1
            if not isinstance(ch, dict):
                continue
            ip = (ch.get('ip') or '').strip()
            if not ip:
                continue
            rows.append((idx, ip, ch.get('vendor') or '-', ch.get('stream') or '-'))
    except Exception as exc:
        sys.stderr.write('json parse failed (%s), fallback to regex\n' % exc)
        for idx, ip in enumerate(
                re.findall(r'"ip"\s*:\s*"(\d+\.\d+\.\d+\.\d+)"', raw), 1):
            rows.append((idx, ip, '-', '-'))

    for idx, ip, vendor, stream in rows:
        print('%d %s %s %s' % (idx, ip, vendor, stream))
    return 0


if __name__ == '__main__':
    sys.exit(main())
