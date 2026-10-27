# -*- coding: utf-8 -*-
"""板端专用：按 `通道=IP` 批量写 `camera.ch.N.ip`（通道号 **1 基**，同平台口径）。

为什么不用 Web / ctl 手搓：板端没有 curl，`config.apply` 又要求 `ver` 与键类型
完全对得上，一条一条手打极易写错。本脚本从 ctl 读当前 `ver` 再提交，写完读回
并用 `system.snapshot` 确认主控没被卡死。

用法（board 上）：
    python3 /tmp/board_cam_set.py 2=192.168.2.31 3=        # 第 2 路设 .31、第 3 路清空

⚠ `config.apply` 会把当前 SSH 中转会话重置 ⇒ 请用 `nohup ... &` 跑，
输出重定向到 `/mnt/UDISK/*.log` 再回读（`/tmp` 是 tmpfs，复位即清）。
"""
import json
import socket
import struct
import sys

CTL = '/var/run/ivsbox/ctl'
HDR = struct.Struct('<IHHIIQII')
rid = [0]


def ctl(method, params=None, timeout=8.0):
    rid[0] += 1
    payload = json.dumps({'m': method, 'p': params or {}}).encode()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.settimeout(timeout)
    s.connect(CTL)
    s.sendall(HDR.pack(0x42535649, 1, 1, 0, 0, rid[0], len(payload), 0) + payload)
    buf = s.recv(200000)
    s.close()
    n = HDR.unpack(buf[:32])[6]
    return json.loads(buf[32:32 + n].decode())


def dump():
    d = ctl('config.class', {'cls': 'camera'}).get('d') or {}
    it = d.get('items') or {}
    return [it.get('camera.ch.%d.ip' % i) for i in range(8)]


def main():
    kv = {}
    for a in sys.argv[1:]:
        ch, _, ip = a.partition('=')
        n = int(ch)
        if n < 1 or n > 8:
            print('channel %d out of range (1..8)' % n, flush=True)
            return 1
        kv['camera.ch.%d.ip' % (n - 1)] = ip
    if not kv:
        print('usage: board_cam_set.py 2=192.168.2.31 3=', flush=True)
        return 1

    ver = (ctl('config.class', {'cls': 'camera'}).get('d') or {}).get('version')
    print('改前 8 路 =', dump(), 'ver=%s' % ver, flush=True)
    r = ctl('config.apply', {'ver': ver, 'items': kv})
    print('apply %s -> ok=%s e=%s' % (kv, r.get('ok'), r.get('e')), flush=True)
    after = dump()
    print('改后 8 路 =', after, flush=True)
    try:
        ctl('system.snapshot')
        print('ctl 仍应答 = True', flush=True)
    except Exception as e:                      # noqa: BLE001
        print('ctl 无应答 <<< 卡死: %r' % (e,), flush=True)
        return 2
    ok = all((after[int(k.split('.')[2])] or '') == v for k, v in kv.items())
    print('一致 =', ok, flush=True)
    return 0 if ok else 3


if __name__ == '__main__':
    sys.exit(main())
