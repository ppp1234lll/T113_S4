# -*- coding: utf-8 -*-
"""板端：受控远程通道开关（ctl 的 tunnel.*）。在设备上跑，不在 PC 上跑。

**2026-10-30 按 BIRD（GO 版 bird_20260623）收口**：设备侧只有**一条** stcp 代理，
转发本机 sshd（127.0.0.1:22）。"看摄像机 / 取 SD 卡文件 / 传固件"全部由**访问端**
用 `ssh -L` 按需建立，设备侧不再逐服务开代理（`--svcs` 已被忽略）。

    pscp tun.py root@<设备>:/opt/ivsbox/scripts/
    plink root@<设备> "python3 /opt/ivsbox/scripts/tun.py status"
    plink root@<设备> "python3 /opt/ivsbox/scripts/tun.py on"            # 不过期（= BIRD 行为）
    plink root@<设备> "python3 /opt/ivsbox/scripts/tun.py on --ttl 1800"  # 30 分钟后自动关
    plink root@<设备> "python3 /opt/ivsbox/scripts/tun.py off"

访问端（PC）拿到通道后，跑 _deploy/frpwin/cam.bat 选一路即可（按需单路 + ssh -C 压缩）：

    ssh -C -i ~/.ssh/ivsbox_ed25519 -p 16000 -L 9091:192.168.1.106:80 root@127.0.0.1 -N

**-C 必须带**：摄像机 Web 首屏要拉 8.3 MB 明文 JS/CSS，而摄像机自己不做 gzip；
SSH 协议层压缩后链路只走约 1 MB，首屏从 33 s 降到 6 s（2026-11-02 实测）。

为什么要有它：`tunnel.enable` 目前只挂在本地通道（ctl）上，Web 界面没有按钮，
平台上也没有对应命令。运维要开口子就只能走这里 —— 手工 `touch /tmp/frp.enabled`
看似等价，但那样拿不到"到期时刻"，通道会一直开着（BIRD 就是这个行为；本工程支持
显式 TTL，不传 TTL 时才回到 BIRD 语义）。

开之前设备上要满足三件事（缺一条 `tunnel.enable` 会回 `ENOSERVER` / 拒绝）：
  1. 配置 `sys.tunnel.server.host` 非空（frps 地址）；
  2. `<secure>/tunnel.token` 存在（平台下发的调试令牌，路径 /mnt/UDISK/ivsbox/secure/）；
  3. 设备 SN 拿得到（eth0 MAC 派生，见 tunnel_read_sn）。
"""
import json
import os
import socket
import struct
import sys
import time

HDR = struct.Struct('<IHHIIQII')
SOCK = '/var/run/ivsbox/ctl'
# 与 iv_tunnel.h 的 IV_TUNNEL_FLAG_DEFAULT、scripts/frp.sh 的 ENABLED_FLAG 同名。
FLAG = '/tmp/frp.enabled'


def ctl(m, p=None, timeout=10):
    req = {'m': m}
    if p is not None:
        req['p'] = p
    b = json.dumps(req).encode()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.send(HDR.pack(0x42535649, 1, 1, 0, 0, 1, len(b), 0) + b)
    d = s.recv(65536 + 32)
    s.close()
    n = HDR.unpack(d[:32])[6]
    return json.loads(d[32:32 + n].decode())


def show(tag):
    r = ctl('tunnel.status')
    d = r.get('d', {})
    print('%s on=%s remain=%ss ttl=%ss local_port=%s sn=%s server=%s:%s' % (
        tag, d.get('on'), d.get('remain_s'), d.get('ttl_s'),
        d.get('local_port', 22), d.get('sn'),
        d.get('server'), d.get('server_port')))
    if d.get('on'):
        print('   标记 %s  配置 %s' % (d.get('flag', FLAG), d.get('conf')))
        print('   访问端：双击 _deploy/frpwin/cam.bat，按需选一路'
              '（1/2/3 摄像机，W 设备 Web，D 下 SD 卡文件）')
    return d


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        return 1
    cmd = argv[0]

    if cmd == 'status':
        show('STATUS')
        return 0

    if cmd == 'off':
        print(json.dumps(ctl('tunnel.disable'), ensure_ascii=False))
        time.sleep(1)
        show('AFTER-OFF')
        return 0

    if cmd == 'on':
        ttl = 0          # 0 ＝ 不过期，与 BIRD 一致；要限时显式给 --ttl
        host = None
        i = 1
        while i < len(argv):
            if argv[i] == '--ttl' and i + 1 < len(argv):
                ttl = int(argv[i + 1]); i += 2
            elif argv[i] == '--svcs' and i + 1 < len(argv):
                # 兼容老命令行：BIRD 模型下设备侧只有 ssh 一条代理，此项忽略。
                print('NOTE --svcs 已忽略（BIRD 模型：只转发本机 22，其余走 ssh -L）')
                i += 2
            elif argv[i] == '--host' and i + 1 < len(argv):
                host = argv[i + 1]; i += 2
            else:
                print('unknown arg: %s' % argv[i])
                return 1
        if host:
            a = ctl('config.all')['d']
            r = ctl('config.apply', {'ver': a['version'], 'items': {
                'sys.tunnel.server.host': host, 'sys.tunnel.server.port': 7000}})
            print('CONFIG %s' % json.dumps(r, ensure_ascii=False)[:200])
        # tunnel.enable 非幂等：已开着再 enable 会回 ESTATE。开机自恢复脚本会被
        # 重复调用（人工 start、上电、monitor 等），这里先判一次，已开就直通。
        cur = ctl('tunnel.status').get('d', {})
        if cur.get('on') and cur.get('ttl_s') in (0, None) and not host:
            print('ALREADY-ON (ttl=0, 不做重复 enable)')
            show('STATUS')
            return 0
        print(json.dumps(ctl('tunnel.enable', {'ttl': ttl}), ensure_ascii=False))
        # monitor.sh 每 5 s 一拍，等它把 frpc 拉起来（标记由 iv_tunnel 直接写）
        for _ in range(12):
            time.sleep(1)
            if os.path.exists(FLAG):
                break
        time.sleep(5)
        show('AFTER-ON')
        return 0

    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main())
