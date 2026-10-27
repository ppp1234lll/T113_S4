# -*- coding: utf-8 -*-
"""板端取证：运行期新增一路摄像机 IP 之后，ivsboxd 是"自旋"还是"阻塞在哪个系统调用"。

背景：2026-10-27 08:51:32 平台配第 2 路成功后，主控 31 s 内一条日志都没写，随后
08:52:03 一次 unclean shutdown（16 s 看门狗复位）。host 上连配 6 路（含可达 IP）
都不复现 ⇒ 板端特有路径。板端实测：**已有 1 路再加一路卡死（0→1 那一次不卡）**。

⚠ 取证本身会被看门狗复位打断，而硬复位会丢掉页缓存里的日志（上一版写
`/mnt/UDISK/hang.log` 回来是 0 字节）。所以证据**当场发到 VM 的 TCP 端口**，
VM 侧收到即落盘 —— 复位不会把已送达的数据抹掉。

判据：
  - utime+stime 每拍暴涨 + State=R ⇒ 自旋（死循环）
  - State=S/D + `/proc/pid/syscall` 有 syscall 号 ⇒ 阻塞在哪个调用
  - `/proc/pid/stack` 给内核栈（有 CONFIG_STACKTRACE 才有内容）

用法：board 上 `EVI=192.168.2.115:16098 python3 -u board_hang_probe.py <ip> <ch>`
"""
import json
import os
import socket
import struct
import sys
import time

CTL = '/var/run/ivsbox/ctl'
HDR = struct.Struct('<IHHIIQII')
rid = [0]
_evi = [None]

# 常见 syscall 号（arm32 EABI）：阻塞点看不懂时靠这张表
_SC = {
    3: 'read', 4: 'write', 5: 'open', 6: 'close', 54: 'ioctl', 145: 'readv',
    146: 'writev', 168: 'poll', 183: 'getcwd', 192: 'mmap2', 240: 'futex',
    248: 'exit_group', 252: 'exit', 263: 'clock_nanosleep', 281: 'socket',
    283: 'connect', 285: 'recv', 290: 'sendto', 291: 'recvfrom', 292: 'recvmsg',
    293: 'sendmsg', 338: 'epoll_wait', 345: 'sendmmsg', 354: 'epoll_pwait',
    355: 'wait4', 384: 'getrandom',
}


def say(msg):
    print(msg, flush=True)
    if _evi[0] is not None:
        try:
            _evi[0].sendall((msg + '\n').encode('utf-8'))
        except OSError:
            pass


def ctl(method, params=None, timeout=4.0):
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


def pid_of(name='ivsboxd'):
    for d in os.listdir('/proc'):
        if not d.isdigit():
            continue
        try:
            with open('/proc/%s/comm' % d) as fh:
                if fh.read().strip() == name:
                    return int(d)
        except OSError:
            pass
    return None


def cpu_ticks(pid):
    with open('/proc/%d/stat' % pid) as fh:
        f = fh.read().rsplit(') ', 1)[1].split()
    return int(f[11]), int(f[12])


def snapshot(pid):
    out = {}
    try:
        with open('/proc/%d/status' % pid) as fh:
            for ln in fh.read().splitlines():
                for key in ('State:', 'voluntary_ctxt_switches:',
                            'nonvoluntary_ctxt_switches:', 'Threads:'):
                    if ln.startswith(key):
                        out[key[:-1]] = ln[len(key):].strip()
    except OSError as e:
        out['State'] = 'gone(%s)' % e
        return out
    for nm, path in (('wchan', '/proc/%d/wchan' % pid),
                     ('syscall', '/proc/%d/syscall' % pid)):
        try:
            with open(path) as fh:
                v = fh.read().strip()
        except OSError:
            v = '?'
        if nm == 'syscall' and v and v[0].isdigit():
            nr = int(v.split()[0])
            v = '%s(%d) %s' % (_SC.get(nr, 'unknown'), nr, v)
        out[nm] = v or '(empty)'
    try:
        with open('/proc/%d/stack' % pid) as fh:
            st = fh.read().strip().splitlines()
        out['kstack'] = ' | '.join(x.strip() for x in st[:4]) if st else '(empty)'
    except OSError:
        out['kstack'] = 'n/a'
    return out


def main():
    evi = os.environ.get('EVI')
    if evi:
        host, _, port = evi.partition(':')
        try:
            _evi[0] = socket.create_connection((host, int(port)), timeout=5)
            _evi[0].setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            say('EVI connected to %s' % evi)
        except OSError as e:
            say('EVI connect failed: %r（继续，只打印）' % (e,))

    pid = pid_of()
    if pid is None:
        say('FAIL: 找不到 ivsboxd')
        return 1
    say('ivsboxd pid=%d' % pid)

    d = ctl('config.class', {'cls': 'camera'}).get('d') or {}
    v, items = d.get('version'), d.get('items') or {}
    say('camera version=%s, 现 8 路 = %s' % (v, [items.get('camera.ch.%d.ip' % i)
                                                for i in range(8)]))
    tgt = sys.argv[1] if len(sys.argv) > 1 else '192.168.2.1'
    ch = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    key = 'camera.ch.%d.ip' % (ch - 1)
    say('将把 %s 设成 %s（与平台 0xF7 同一条热更路径）' % (key, tgt))

    u0, s0 = cpu_ticks(pid)
    say('基线 cpu utime=%d stime=%d' % (u0, s0))
    t0 = time.time()

    try:
        r = ctl('config.apply', {'ver': v, 'items': {key: tgt}}, timeout=4.0)
        say('config.apply 返回 ok=%s (%.2fs)' % (r.get('ok'), time.time() - t0))
    except Exception as e:                      # noqa: BLE001
        say('config.apply 没回（可能已经卡住）：%r' % (e,))

    last = u0 + s0
    for _ in range(14):
        time.sleep(1.0)
        try:
            u, s2 = cpu_ticks(pid)
        except OSError:
            say('  t+%.1fs 进程没了（崩溃/被复位）' % (time.time() - t0))
            return 3
        st = snapshot(pid)
        say('  t+%5.1fs cpu=+%3d %s' % (time.time() - t0, (u + s2) - last, st))
        last = u + s2
        try:
            ctl('system.snapshot', timeout=2.0)
            say('            ctl 仍应答')
        except Exception:                       # noqa: BLE001
            say('            ctl 无应答 <<< 卡死')
    return 0


if __name__ == '__main__':
    sys.exit(main())
