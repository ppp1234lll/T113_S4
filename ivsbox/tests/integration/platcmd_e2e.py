#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""平台**下行指令**全量回归：每一条已注册的命令真发一帧、看应答、看副作用

为什么必须有这一层：`main.c` / `iv_agent.c` / `iv_report.c` 里的 handler 都是
`static`，单测与 `test_e2e_report.c` 都够不到它们（后者只测 `iv_report` 自己的
路由，装配层注册的那几条根本不在场）。而 2026-10-26 一天之内连续在三条**配置**
指令上翻车（0xF4 改完网口 ack 被自己 flush 掉、0xF3 切网把 ack 清零、0xA5 只改
运行期不落盘）——共同点就是"**回了 0x01，平台看起来成功，实际没干全**"，
而这类问题在静态阅读里全都看不出来：代码每一步都"对"，错的是顺序与落点。

所以本脚本只做一件事：冒充平台，逐条下发，然后把三件事对账：
  ① 应答码（0x01 / 0x74）——平台界面上那个"成功/失败"就是它；
  ② **副作用**（配置键真的变了 / 日志真打了 applied）—— 光有 0x01 不算数；
  ③ 未实现的命令必须回 0x74（绝不能回 0x01 说谎）。

⚠ 安全约束（都是踩出来的）：
  - 开头先把 `netthr.reboot_max` 置 0（自愈禁用）：本脚本把平台地址指到本机，
    若上一轮留下 reboot_max>0，平台连不通时自愈会真把 VM 重启掉；
  - 平台地址用 **本机 eth0 的地址**而不是 127.0.0.1：`bind_ifname` +
    `SO_BINDTODEVICE` 下连 127.0.0.1 会因 oif 不匹配而永远 `EINPROGRESS`（D-01）；
  - **不发会改本机地址的 0xF4**（那会把 SSH 掐掉）：只发"同值"一份，走
    unchanged 早返回分支，仍然覆盖了解析＋应答＋早返回判据；
  - 收尾把动过的键全部还原（平台地址、上报间隔、检测目标、检测方式）。

用法（必须先 `make host`）：
    cd <repo>/ivsbox
    sudo python3 tests/integration/platcmd_e2e.py
前置：`/mnt/UDISK/log` 可写；当前**没有**别的 ivsboxd 在跑（本脚本自己拉起，
结束时收掉）。退出码：0 = 全部通过；1 = 有失败。
"""
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time

CTL = '/var/run/ivsbox/ctl'
LOGROOT = '/mnt/UDISK/log'
BIN = os.path.abspath('build/host/ivsboxd')
MAGIC = 0x42535649
HDR = struct.Struct('<IHHIIQII')

PLAT_PORT = 16099
DEFAULT_HOST = '47.104.250.225'
DEFAULT_PORT = 6012

fails = []
rid = 0
BASE = 0

# 下行帧布局（与 src/modules/proto/iv_proto.c 的 OFF_* 一致）
OFF_VER, OFF_TYPE, OFF_ID = 2, 3, 5
OFF_CMD, OFF_QN1, OFF_QN2, OFF_LEN, OFF_DATA = 8, 9, 13, 17, 18
VER = 0x11
DEVTYPE = 0x0A00


# ---------------------------------------------------------------------------
# CRC8（poly 0x07 / seed 0，覆盖 [ver, data 末]）—— 与 iv_crc8 同参
# ---------------------------------------------------------------------------
def crc8(buf):
    crc = 0
    for b in buf:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def build_downlink(cmd, data=b'', qn1=0x11223344, qn2=0x55667788, devid=0x0B2CC1):
    n = len(data)
    out = bytearray(21 + n)
    out[0] = 0xF0
    out[1] = 0xF0
    out[OFF_VER] = VER
    out[OFF_TYPE] = (DEVTYPE >> 8) & 0xFF
    out[OFF_TYPE + 1] = DEVTYPE & 0xFF
    out[OFF_ID] = (devid >> 16) & 0xFF
    out[OFF_ID + 1] = (devid >> 8) & 0xFF
    out[OFF_ID + 2] = devid & 0xFF
    out[OFF_CMD] = cmd
    struct.pack_into('>I', out, OFF_QN1, qn1)
    struct.pack_into('>I', out, OFF_QN2, qn2)
    out[OFF_LEN] = n & 0xFF
    out[OFF_DATA:OFF_DATA + n] = data
    out[OFF_DATA + n] = crc8(out[OFF_VER:OFF_DATA + n])
    out[OFF_DATA + n + 1] = 0xFF
    out[OFF_DATA + n + 2] = 0xFF
    return bytes(out)


def parse_bin(buf):
    """二进制帧：F0F0 … LEN@17 … DATA@18 … CRC … FFFF。返回记录 dict 或 None。"""
    if len(buf) < 21:
        return None
    n = buf[OFF_LEN]
    if len(buf) < 21 + n:
        return None
    data = bytes(buf[OFF_DATA:OFF_DATA + n])
    return {'kind': 'bin', 'cmd': buf[OFF_CMD],
            'code': (data[0] if n >= 1 else None), 'data': data}


def parse_json(blob, start):
    """JSON/文本帧：`##` + json + `##`（`iv_proto_json_frame()`，总长 4 + len）。

    从 start 开始找尾部的 `##`；返回 (记录, 消耗字节数) 或 (None, 0)。
    这类帧的"命令码"只在 JSON 里（`"cmd":"E3"`），没有二进制头。"""
    i = blob.find(b'##', start + 2)
    if i < 0:
        return None, 0
    body = blob[start + 2:i]
    cmd = None
    try:
        j = json.loads(body.decode('utf-8', 'replace'))
        d = j.get('data') if isinstance(j, dict) else None
        if isinstance(d, dict) and isinstance(d.get('cmd'), str):
            cmd = int(d['cmd'][:2], 16)
    except Exception:
        pass
    return {'kind': 'json', 'cmd': cmd, 'code': None, 'data': bytes(body)}, (i + 2 - start)


# ---------------------------------------------------------------------------
# 冒充平台的 TCP 服务（设备连进来，我们收发）
# ---------------------------------------------------------------------------
class Plat(object):
    def __init__(self, port):
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(('0.0.0.0', port))
        self.srv.listen(8)
        self.srv.settimeout(0.5)
        self.conns = []
        self.frames = []
        self.buf = {}
        self.stop = False
        self.nacc = 0      # 累计 accept 次数
        self.nread = 0     # 累计读到的字节数
        self.nparsed = 0   # 累计切出的帧数（clear() 不清）
        self.first = None  # 第一段原始字节（切不出帧时用它定位）
        self.err = None    # 读线程的异常（不给出来就永远是"没应答"）
        self.t = threading.Thread(target=self._run)
        self.t.daemon = True
        self.t.start()

    def _run(self):
        try:
            self._loop()
        except Exception as e:      # noqa: BLE001 —— 线程里任何异常都必须显式暴露
            import traceback
            self.err = traceback.format_exc()
            sys.stderr.write('PLAT READER DIED: %r\n' % (e,))

    def _loop(self):
        while not self.stop:
            try:
                c, _ = self.srv.accept()
                c.setblocking(False)
                with self.lock:
                    self.conns.append(c)
                    self.buf[id(c)] = b''
                    self.nacc += 1
            except socket.timeout:
                pass
            except OSError:
                break
            with self.lock:
                conns = list(self.conns)
            for c in conns:
                try:
                    d = c.recv(65535)
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError:
                    d = b''
                if not d:
                    with self.lock:
                        if c in self.conns:
                            self.conns.remove(c)
                        self.buf.pop(id(c), None)
                    try:
                        c.close()
                    except OSError:
                        pass
                    continue
                with self.lock:
                    self.buf[id(c)] = self.buf.get(id(c), b'') + d
                    self.nread += len(d)
                    if self.first is None:
                        self.first = bytes(d[:48])
                    blob = self.buf[id(c)]
                    # 两种帧混在一条流里：二进制（F0F0…）与 JSON/文本（##…##）。
                    # 只认其中一种 ⇒ 另一种会被当噪声、把后面的帧一起带偏
                    # （第一版就是这么 false-FAIL 掉全部应答的）。
                    while True:
                        if len(blob) >= 2 and blob[0] == 0x23 and blob[1] == 0x23:
                            rec, used = parse_json(blob, 0)
                            if rec is None:
                                break
                            blob = blob[used:]
                        elif len(blob) >= 2 and blob[0] == 0x0F and blob[1] == 0x0F:
                            # **设备→平台的二进制帧头是 0x0F0F**（`IV_PROTO_HEAD_UP`），
                            # 不是下行那个 0xF0F0（`IV_PROTO_HEAD_DOWN`）。第一版照
                            # 下行头去认，结果把所有 ACK/心跳都当噪声丢了 —— 症状是
                            # "每条指令都没应答"，而设备侧日志显示 ack 全部发出去了。
                            rec = parse_bin(blob)
                            if rec is None:
                                break
                            blob = blob[21 + blob[OFF_LEN]:]
                        elif len(blob) >= 2:
                            blob = blob[1:]
                            continue
                        else:
                            break
                        self.frames.append((time.time(), rec))
                        self.nparsed += 1
                    self.buf[id(c)] = blob

    def online(self):
        with self.lock:
            return len(self.conns) > 0

    def send(self, raw):
        with self.lock:
            conns = list(self.conns)
        for c in conns:
            try:
                c.sendall(raw)
                return True
            except OSError:
                pass
        return False

    def clear(self):
        with self.lock:
            self.frames = []

    def wait(self, cmd, timeout=6.0, after=0.0, kind=None):
        """等一个 cmd 匹配的上行帧，返回记录 dict 或 None。"""
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                for ts, rec in self.frames:
                    if ts >= after and rec['cmd'] == cmd and (kind is None or
                                                              rec['kind'] == kind):
                        return rec
            time.sleep(0.05)
        return None

    def close(self):
        self.stop = True
        with self.lock:
            for c in self.conns:
                try:
                    c.close()
                except OSError:
                    pass
            self.conns = []
        try:
            self.srv.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# ctl（本地通道，AF_UNIX + SEQPACKET，32 B 小端头）
# ---------------------------------------------------------------------------
def ctl(method, params=None):
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


def wait_ctl(deadline=20.0):
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


def wait_log(pattern, deadline=8.0, start=None):
    t0 = time.time()
    off = BASE if start is None else start
    last = None
    while time.time() - t0 < deadline:
        for line in logtext()[off:].splitlines():
            if re.search(pattern, line):
                last = line
        if last is not None:
            return last
        time.sleep(0.2)
    return last


def cls_items(cls):
    return (ctl('config.class', {'cls': cls}).get('d') or {}).get('items') or {}


def cls_ver(cls):
    return (ctl('config.class', {'cls': cls}).get('d') or {}).get('version')


def apply_cls(cls, items):
    return ctl('config.apply', {'ver': cls_ver(cls), 'items': items})


def local_addr():
    """本机"出口网卡"的 IPv4：平台地址必须指到**它自己网卡上的地址**（见 D-01）。

    VM 上的网卡名是 `ens33`/`enp0s3`，板端才是 `eth0` ⇒ 不写死名字，
    取第一条非 lo 的全局地址（也就是默认路由出口那一块）。"""
    try:
        out = subprocess.check_output(['ip', '-4', '-o', 'addr'],
                                      stderr=subprocess.DEVNULL).decode()
        for line in out.splitlines():
            m = re.search(r'^\d+:\s+(\S+)\s+inet (\d+\.\d+\.\d+\.\d+)/(\d+)', line)
            if m and m.group(1) != 'lo':
                return m.group(1), m.group(2), int(m.group(3))
    except Exception:
        pass
    return None, None, None


def prefix_to_mask(n):
    return (0xFFFFFFFF << (32 - n)) & 0xFFFFFFFF


def as_int(x):
    """`0xE1` 应答里所有数值都是**带引号的字符串**（`cj_int` 走 `cj_str`，
    照抄参考实现 `"pn":"2000"`）⇒ 断言前必须转一次，否则 `'2000' == 2000`
    恒假，报出来的消息却是"配置 2000，应答 2000"，看着像设备错了。"""
    try:
        return int(x)
    except (TypeError, ValueError):
        return None


def chk(cond, what):
    if cond:
        print('  PASS ' + what)
    else:
        print('  FAIL ' + what)
        fails.append(what)


def main():
    global BASE

    if wait_ctl(0.1):
        print('FAIL: 已经有 ivsboxd 在跑，换个环境再测')
        return 1

    ifn, ip, plen = local_addr()
    if ip is None:
        print('FAIL: 取不到本机出口网卡的地址')
        return 1
    print('平台冒充地址 = %s:%d（本机 %s = %s/%d）' % (ip, PLAT_PORT, ifn, ip, plen))

    plat = Plat(PLAT_PORT)
    p = subprocess.Popen([BIN], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    try:
        if not wait_ctl():
            print('FAIL: ivsboxd 没起来')
            return 1
        BASE = len(logtext())

        # 动过的键先记原值，收尾按原值还原（不是按"出厂值猜"）
        orig = {}
        for cls in ('net', 'sys', 'probe', 'camera'):
            orig[cls] = cls_items(cls)

        # ---- A0 安全前置：自愈置 0，平台地址指到本机 ----
        print('A0 前置：关自愈 ＋ 平台地址指到本机冒牌服务')
        ver = (ctl('config.class', {'cls': 'netthr'}).get('d') or {}).get('version')
        r = ctl('config.apply', {'ver': ver, 'items': {'netthr.reboot_max': 0}})
        chk(r.get('ok') is True, 'A0a netthr.reboot_max=0（平台连不通也不会真重启）')

        r = apply_cls('net', {
            'net.server.wired.host': ip, 'net.server.wired.port': PLAT_PORT,
            'net.server.wired.host_bak': ip, 'net.server.wired.port_bak': PLAT_PORT,
            'net.server.wireless.host': ip, 'net.server.wireless.port': PLAT_PORT,
            'net.server.wireless.host_bak': ip, 'net.server.wireless.port_bak': PLAT_PORT,
        })
        chk(r.get('ok') is True, 'A0b 平台地址指向本机冒牌服务')

        t0 = time.time()
        while time.time() - t0 < 25.0 and not plat.online():
            time.sleep(0.2)
        chk(plat.online(), 'A0c 设备真的连进来了（否则后面每条都会"没应答"）')
        if not plat.online():
            return 1
        time.sleep(1.0)  # 让心跳/首帧先过去，别把它的 cmd 当我们的应答

        # ---- B 查询类 ----
        print('B1 0xE3 查询版本 ⇒ 回 ##{...}## JSON，不是 ACK')
        plat.clear()
        plat.send(build_downlink(0xE3))
        f = plat.wait(0xE3, kind='json')
        chk(f is not None, 'B1a 收到 0xE3 的 JSON 应答帧')
        if f is not None:
            d = f['data']
            chk(b'QN=' in d or b'"qn":' in d, 'B1b 应答体是 JSON（带 qn）')
            chk(b'2874540201432778632' in d,
                'B1c qn 原样回显请求里的 QN（qn1qn2 十进制拼接）')

        print('B2 0xE1 查询参数 ⇒ 回 JSON，且 pn 的量纲必须与 0xFE 写下去的一致')
        plat.clear()
        plat.send(build_downlink(0xE1))
        f = plat.wait(0xE1, kind='json')
        chk(f is not None, 'B2a 0xE1 回了 JSON 帧')
        if f is not None:
            chk(b'{' in f['data'], 'B2b 应答体里是配置 JSON')
            # 平台参数设置页那一项写的是「有线网络超时时间(ms)」，`0xFE` 也是 1 字节
            # 毫秒原样写进 `probe.timeout_ms` ⇒ 读回来的 `pn` 必须同量纲。
            # 以前这里是 `v / 1000`（秒）：设 100 ms、读回 0，平台再打开页面显示 0，
            # 现场只会以为"没保存上"，然后再点一次。
            pr = cls_items('probe')
            want_pn = int(pr.get('probe.timeout_ms', 0))
            want_pl = int(pr.get('probe.interval_ms', 0)) // 1000
            try:
                bd = (json.loads(f['data'].decode('utf-8', 'replace')) or {}).get('data') or {}
            except Exception:
                bd = {}
            chk(as_int(bd.get('pn')) == want_pn,
                'B2c pn 送毫秒（配置 %d，应答 %s）' % (want_pn, bd.get('pn')))
            chk(as_int(bd.get('pl')) == want_pl,
                'B2d pl 仍送秒（配置 %d ms，应答 %s）' % (want_pl * 1000, bd.get('pl')))

        # ---- B2e~B2h 阈值/重启参数必须"读的是配置"，不是常量 ----
        # 2026-10-27 现场：平台参数设置页「温度阈值 / 湿度阈值 / 配置重启网络传输
        # 设备电源」三组全显示 0。根因是 `query_cfg_json()` 把 `ft`/`fh`/`lannum`/
        # `lantime` 写死成常量（"0,0"/0），而板端 `netthr.reboot_interval_s` 实际是
        # 60 s —— **值对不上**才是硬证据（不是"恰好都是 0"）。
        # 这里把三组配成互相可区分的非默认值（温度下限特意给负数），再查一次。
        se0 = cls_items('sensor')
        nt0 = cls_items('netthr')
        apply_cls('sensor', {'sensor.temp_high': 28.0, 'sensor.temp_low': -5.0,
                             'sensor.humi_high': 80.0, 'sensor.humi_low': 20.0})
        apply_cls('netthr', {'netthr.reboot_max': 2,
                             'netthr.reboot_interval_s': 45})
        plat.clear()
        plat.send(build_downlink(0xE1))
        f = plat.wait(0xE1, kind='json')
        bd2 = {}
        if f is not None:
            try:
                bd2 = (json.loads(f['data'].decode('utf-8', 'replace')) or {}).get('data') or {}
            except Exception:
                bd2 = {}
        chk(bd2.get('ft') == '28,-5',
            'B2e ft 送温度阈值"上限,下限"（含负温；应答 %r）' % bd2.get('ft'))
        chk(bd2.get('fh') == '80,20',
            'B2f fh 送湿度阈值"上限,下限"（应答 %r）' % bd2.get('fh'))
        chk(as_int(bd2.get('lannum')) == 2,
            'B2g lannum 送重启次数（配置 2，应答 %s）' % bd2.get('lannum'))
        chk(as_int(bd2.get('lantime')) == 45,
            'B2h lantime 送重启时间秒（配置 45，应答 %s）' % bd2.get('lantime'))
        # 立刻还原，不指望结尾的批量还原 —— 那条路径的"原值"可能是上一轮污染的。
        apply_cls('sensor', {k: se0[k] for k in
                             ('sensor.temp_high', 'sensor.temp_low',
                              'sensor.humi_high', 'sensor.humi_low') if k in se0})
        apply_cls('netthr', {k: nt0[k] for k in
                             ('netthr.reboot_max', 'netthr.reboot_interval_s')
                             if k in nt0})

        print('B3 0xE2 立即上报 ⇒ 回 ACK 0x01')
        plat.clear()
        plat.send(build_downlink(0xE2))
        f = plat.wait(0xE2, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'B3a 0xE2 ack=0x01')

        print('B4 0xE4 搜索摄像机 ⇒ 回 ACK 0x01（发现是异步的）')
        plat.clear()
        plat.send(build_downlink(0xE4))
        f = plat.wait(0xE4, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'B4a 0xE4 ack=0x01')

        print('B5 0xE7 查询 SNMP ⇒ 应答码必须与实际执行结果一致（不许谎报成功）')
        plat.clear()
        start = len(logtext())
        plat.send(build_downlink(0xE7, b'\x00'))
        f = plat.wait(0xE7, kind='bin')
        chk(f is not None, 'B5a 0xE7 有应答')
        # 判据不是"必须 0x01"：没配 SNMP 目标时 `iv_agent_snmp_start()` 回
        # `IV_ESTATE`，那时**该**回 0x74。撒谎（rc<0 却回 0x01）才是缺陷
        # —— 这正是 2026-10-20 那一批"二十多条没实现的指令全回 OK"的病根。
        ln = wait_log(r'0xE7 snmp query .*-> rc=(-?\d+)', start=start)
        rc = int(re.search(r'rc=(-?\d+)', ln).group(1)) if ln else None
        chk(ln is not None, 'B5b 日志记了查询结果（现场唯一的观测窗口）')
        chk(f is not None and rc is not None and
            f['code'] == (0x01 if rc == 0 else 0x74),
            'B5c 应答码与执行结果一致（rc=%s ⇒ 0x%02X）'
            % (rc, f['code'] if f is not None else -1))

        # ---- C 配置类：应答 + 副作用都要对 ----
        print('C1 0xF5 配置上报间隔 37 s ⇒ ack 0x01 ＋ 配置键真的变了')
        plat.clear()
        start = len(logtext())
        plat.send(build_downlink(0xF5, b'\x01\x00\x25'))  # sel=1, 37 s
        f = plat.wait(0xF5, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'C1a 0xF5 ack=0x01')
        chk(cls_items('sys').get('sys.report.interval_s') == 37,
            'C1b sys.report.interval_s == 37（落了盘，不是只回话）')

        print('C2 0xF8 配置主网检测 IP ⇒ ack 0x01 ＋ probe.target1/2 真的变了')
        plat.clear()
        start = len(logtext())
        plat.send(build_downlink(0xF8,
                                 bytes([192, 168, 2, 201, 192, 168, 2, 202])))
        f = plat.wait(0xF8, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'C2a 0xF8 ack=0x01')
        it = cls_items('probe')
        chk(it.get('probe.target1') == '192.168.2.201' and
            it.get('probe.target2') == '192.168.2.202', 'C2b probe.target1/2 已落盘')

        print('C3 0xA5 配置搜索方式=2 端口扫描 ⇒ ack 0x01 ＋ camera.detect.mode 落盘')
        plat.clear()
        start = len(logtext())
        plat.send(build_downlink(0xA5, b'\x02'))
        f = plat.wait(0xA5, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'C3a 0xA5 ack=0x01')
        chk(cls_items('camera').get('camera.detect.mode') == 2,
            'C3b camera.detect.mode == 2（**这是本次修的**：原来只改运行期不落盘）')
        chk(wait_log(r'detect mode -> 2 \(port\)', start=start) is not None,
            'C3c 日志有 "detect mode -> 2 (port)"（证明真的生效，不只是存了）')

        print('C4 0xA5 非法值 9 ⇒ 必须 0x74，且不采纳')
        plat.clear()
        plat.send(build_downlink(0xA5, b'\x09'))
        f = plat.wait(0xA5, kind='bin')
        chk(f is not None and f['code'] == 0x74, 'C4a 0xA5 mode=9 ⇒ ack=0x74')
        chk(cls_items('camera').get('camera.detect.mode') == 2,
            'C4b 非法值没有把配置改坏（仍是 2）')

        print('C5 0xA5 同值重发 ⇒ ack 0x01 且不动探测（unchanged 早返回）')
        plat.clear()
        start = len(logtext())
        plat.send(build_downlink(0xA5, b'\x02'))
        f = plat.wait(0xA5, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'C5a 0xA5 同值 ack=0x01')
        chk(wait_log(r'0xA5 unchanged', start=start) is not None,
            'C5b 走的是 unchanged 分支（没白拆一轮探测）')

        print('C6 0xF4 同值本机地址 ⇒ ack 0x01 且**不动网口**（真改地址会掐掉 SSH）')
        mask = prefix_to_mask(plen)
        mask_s = socket.inet_ntoa(struct.pack('>I', mask))
        # "同值"的判据是**三个键都比**（`on_plat_local_net()` 里就是这么写的），
        # 所以前置条件也得把三个键都摆成"与将要下发的完全一致"。原来只摆 ip/mask：
        # `net.gateway` 出厂默认是**空串**，于是 `strcmp("", "0.0.0.0")` 不等 ⇒
        # 走的其实是真改分支（真 flush 了一次 eth0），用例却还叫"同值"。
        # 这是**用例自己的前置条件不全**，与"设备没走 unchanged"长得一模一样。
        it = cls_items('net')
        same = (it.get('net.ip') == ip and it.get('net.mask') == mask_s and
                it.get('net.gateway') == '0.0.0.0')
        if not same:
            apply_cls('net', {'net.ip': ip, 'net.mask': mask_s,
                              'net.gateway': '0.0.0.0'})
        plat.clear()
        start = len(logtext())
        plat.send(build_downlink(0xF4, socket.inet_aton(ip) +
                                 struct.pack('>I', mask) + socket.inet_aton('0.0.0.0')))
        f = plat.wait(0xF4, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'C6a 0xF4 同值 ack=0x01')
        chk(wait_log(r'0xF4 unchanged', start=start) is not None,
            'C6b 走 unchanged 分支（链路没被拆）')
        chk(plat.online(), 'C6c 平台连接还在（证明真的没动网口）')

        print('C7 未实现命令必须回 0x74（回 0x01 就是骗平台）')
        for cmd in (0xE5, 0xF2, 0xA3, 0xA4):
            plat.clear()
            plat.send(build_downlink(cmd, b'\x00\x00\x00\x00'))
            f = plat.wait(cmd, kind='bin')
            chk(f is not None and f['code'] == 0x74,
                'C7 0x%02X ⇒ ack 0x%02X（期望 0x74）'
                % (cmd, f['code'] if f is not None else -1))

        print('C8 0xFE 有线网络超时时间(ms) ⇒ ack 0x01 ＋ probe.timeout_ms 落盘＋生效')
        plat.clear()
        start = len(logtext())
        plat.send(build_downlink(0xFE, b'\x64'))   # 100 ms（板端实抓过的真值）
        f = plat.wait(0xFE, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'C8a 0xFE 100ms ⇒ ack=0x01')
        chk(cls_items('probe').get('probe.timeout_ms') == 100,
            'C8b probe.timeout_ms == 100（落盘，不是只回话）')
        chk(wait_log(r'wired net delay threshold -> 100 ms', start=start) is not None,
            'C8c 日志有 "wired net delay threshold -> 100 ms"')
        # 落盘 ≠ 生效。这个键的消费者是 `iv_probe_cfg_t::degrade_rtt_ms`（延时门限），
        # 而它**曾经根本没被读过**（`probe_cfg_of()` 漏读）⇒ 现场设 100 从来没反应。
        # 只有 `netthr.status` 的 `probe_delay_ms`（模块真值）能证明真的换上了。
        st = ctl('netthr.status')
        chk(st.get('ok') is True and (st.get('d') or {}).get('probe_delay_ms') == 100,
            'C8d 探测引擎的延时门限真的变成 100（netthr.status.probe_delay_ms）')

        print('C9 0xFE 非法值 ⇒ 0x74，且不改坏配置')
        plat.clear()
        plat.send(build_downlink(0xFE, b'\x00'))   # 0 ms 无意义
        f = plat.wait(0xFE, kind='bin')
        chk(f is not None and f['code'] == 0x74, 'C9a 0xFE 0ms ⇒ ack=0x74')
        plat.clear()
        plat.send(build_downlink(0xFE, b'\x32'))   # 50 ms，低于区间下界 100
        f = plat.wait(0xFE, kind='bin')
        chk(f is not None and f['code'] == 0x74, 'C9b 0xFE 50ms 低于下界 ⇒ ack=0x74')
        chk(cls_items('probe').get('probe.timeout_ms') == 100,
            'C9c 非法值没有把配置改坏（仍是 100）')

        print('C10 0xFE 同值重发 ⇒ ack 0x01 且不动探测（unchanged 早返回）')
        plat.clear()
        start = len(logtext())
        plat.send(build_downlink(0xFE, b'\x64'))
        f = plat.wait(0xFE, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'C10a 0xFE 同值 ack=0x01')
        chk(wait_log(r'0xFE delay=100ms unchanged', start=start) is not None,
            'C10b 走的是 unchanged 分支（没白重建探测表）')

        # ---- C11~C15 0xF7「配置摄像机 IP 及连接位」----
        # 载荷 5 B：路号(**1 基**) ＋ IP 4 B 大端。参考实现 `com_deal_camera_config()`
        # 与 `tests/tools/fake_plat.py` 的桩帧 `01 c0 a8 02 64` 都是这个形状。
        # 这条命令 2026-10-26 之前**根本没有 handler**（`src/` 里 0 引用）⇒ 落默认
        # 分支回 0x74，平台点「配置」永远失败。所以这里既要用例覆盖正常路径，
        # 也要钉住"越界/重复必须回 0x74"这两条不许被写成"悄悄采纳"。
        cam0 = cls_items('camera')
        used = set((cam0.get('camera.ch.%d.ip' % i) or '') for i in range(6))
        tgt = None
        for cand in ('192.168.2.211', '192.168.2.212', '192.168.2.213',
                     '192.168.2.214'):
            if cand not in used:
                tgt = cand
                break
        chk(tgt is not None, 'C11a 找到未被占用的测试 IP（现场 6 路没占 211~214）')

        if tgt is not None:
            print('C11 0xF7 配置第 2 路 IP=%s ⇒ ack 0x01 ＋ camera.ch.1.ip 落盘 ＋ 生效'
                  % tgt)
            plat.clear()
            start = len(logtext())
            plat.send(build_downlink(0xF7, b'\x02' + socket.inet_aton(tgt)))
            f = plat.wait(0xF7, kind='bin')
            chk(f is not None and f['code'] == 0x01,
                'C11b 0xF7 ch2 ⇒ ack 0x01（**这是本次修的**：原来必回 0x74）')
            chk(cls_items('camera').get('camera.ch.1.ip') == tgt,
                'C11c camera.ch.1.ip == %s（路号 1 基 → 键 0 基必须翻对）' % tgt)
            chk(wait_log(r'camera ch2 ip -> %s \(saved \+ applied\)'
                         % re.escape(tgt), start=start) is not None,
                'C11d 日志有 "camera ch2 ip -> ... (saved + applied)"')

            print('C12 0xF7 同值重发 ⇒ ack 0x01 且不动探测（unchanged 早返回）')
            plat.clear()
            start = len(logtext())
            plat.send(build_downlink(0xF7, b'\x02' + socket.inet_aton(tgt)))
            f = plat.wait(0xF7, kind='bin')
            chk(f is not None and f['code'] == 0x01, 'C12a 同值 ack=0x01')
            chk(wait_log(r'0xF7 ch2 unchanged \(%s\)' % re.escape(tgt),
                         start=start) is not None,
                'C12b 走的是 unchanged 分支（没白重建通道表）')

            print('C13 0xF7 把第 3 路配成第 2 路已用的 IP ⇒ 必须 0x74，不覆盖别人')
            pre3 = cls_items('camera').get('camera.ch.2.ip') or ''
            plat.clear()
            start = len(logtext())
            plat.send(build_downlink(0xF7, b'\x03' + socket.inet_aton(tgt)))
            f = plat.wait(0xF7, kind='bin')
            chk(f is not None and f['code'] == 0x74,
                'C13a 重复 IP ⇒ ack=0x74（参考实现 app_match_local_camera_ip 同口径）')
            chk((cls_items('camera').get('camera.ch.2.ip') or '') == pre3,
                'C13b 第 3 路没被这条重复指令改掉（原值 %r）' % pre3)

        print('C14 0xF7 路号越界（0 和 7）⇒ 必须 0x74，不猜"你想配第 1 路"')
        for bad in (0x00, 0x07):
            plat.clear()
            plat.send(build_downlink(0xF7, bytes([bad, 192, 168, 2, 215])))
            f = plat.wait(0xF7, kind='bin')
            chk(f is not None and f['code'] == 0x74,
                'C14 ch=%d ⇒ ack=0x%02X（期望 0x74）'
                % (bad, f['code'] if f is not None else -1))
        chk('192.168.2.215' not in
            set((cls_items('camera').get('camera.ch.%d.ip' % i) or ''
                 for i in range(6))),
            'C14c 越界载荷一个字都没写进配置')

        if tgt is not None:
            print('C15 0xF7 IP 全 0 ⇒ 清除该路（写空串），不是"配成 0.0.0.0"')
            plat.clear()
            start = len(logtext())
            plat.send(build_downlink(0xF7, b'\x02\x00\x00\x00\x00'))
            f = plat.wait(0xF7, kind='bin')
            chk(f is not None and f['code'] == 0x01, 'C15a 清除 ⇒ ack=0x01')
            chk((cls_items('camera').get('camera.ch.1.ip') or '') == '',
                'C15b camera.ch.1.ip 已清空（全 0 ＝清除，与参考实现一致）')

        # C16 = 2026-10-27 现场"配第 2 路 ⇒ 整机卡死重启"的回归钉子。
        # 机理：再加一路时 `pb_rebuild()` 会把 PING 引擎每个条目的 sent/lost 归零，
        # 而**老通道**的采样游标 `pv_sent/pv_lost` 还留着重建前的值 ⇒ `uint32` 下溢
        # （0-3 = 0xFFFFFFFD）⇒ `sample_ping()` 的 while 要转约 42 亿次：主控单核
        # 100%、日志一条不写、ctl 不再应答，16 s 后看门狗复位整机。平台看到的是
        # 「配置第 N 路摄像机失败」，设备侧却是整机重启 —— 两者极难对上号。
        # 判据：**连配 6 路**每一条都要 ack 0x01，并且每一路之后 ctl 都得应答。
        print('C16 0xF7 连配 6 路 ⇒ 每路 ack 0x01 且主控始终响应（回归 10-27 卡死）')
        alive_n = 0
        for ch in range(1, 7):
            ip_ch = '192.168.2.%d' % (240 + ch)   # 现场不会撞的网段
            plat.clear()
            plat.send(build_downlink(0xF7, bytes([ch]) + socket.inet_aton(ip_ch)))
            f = plat.wait(0xF7, kind='bin')
            ok = (f is not None and f['code'] == 0x01)
            chk(ok, 'C16 ch%d=%s ⇒ ack=0x01' % (ch, ip_ch))
            try:
                ctl('system.snapshot')
                alive_n += 1
            except Exception:                     # noqa: BLE001
                chk(False, 'C16 ch%d 之后主控无应答（又把主循环转死了）' % ch)
                break
        chk(alive_n == 6, 'C16b 连配 6 路之后主控仍活着（ctl 6/6 应答）')

        # ---- C17~C20 0xA4「配置阈值」----
        # 真帧（2026-10-27 10:52:43 板端抓的，用户点「配置阈值」时）：
        #   f0f0 110a 000b 2cc1 a4 ffffffffffffffff 0a 012c 0050 03e8 001e 0019 43 ffff
        # 载荷 10 B ＝ 5 × u16 大端：高压300 低压80 电流1000 倾斜30 漏电25。
        # 界面上还有一格「功率(W)」，但**协议里没有这个字段** —— 那格永远收不到值。
        print('C17 0xA4 配置阈值（5×u16 大端）⇒ ack 0x01 ＋ 五个键落盘＋生效')
        el0 = cls_items('elec')
        se0 = cls_items('sensor')
        th_ok = bytes([0x01, 0x2c, 0x00, 0x50, 0x03, 0xe8, 0x00, 0x1e, 0x00, 0x19])
        start = len(logtext())
        plat.clear()
        plat.send(build_downlink(0xA4, th_ok))
        f = plat.wait(0xA4, kind='bin')
        chk(f is not None and f['code'] == 0x01,
            'C17a 0xA4 ⇒ ack=0x01（**本次修的**：原来必回 0x74）')
        el = cls_items('elec')
        chk(float(el.get('elec.volt_high') or -1) == 300.0,
            'C17b 高压 300 → elec.volt_high（实际 %r）' % el.get('elec.volt_high'))
        chk(float(el.get('elec.volt_low') or -1) == 80.0,
            'C17c 低压 80 → elec.volt_low（实际 %r）' % el.get('elec.volt_low'))
        chk(float(el.get('elec.over_current') or -1) == 1000.0,
            'C17d 电流 1000 → elec.over_current（**不换算成 A**：全链路同一量纲）')
        chk(float(el.get('elec.leak_current') or -1) == 25.0,
            'C17e 漏电 25 → elec.leak_current')
        chk(float(cls_items('sensor').get('sensor.tilt') or -1) == 30.0,
            'C17f 倾斜 30 → sensor.tilt')
        ln = wait_log(r'platform set threshold hv=300 lv=80 oc=1000 tilt=30 leak=25',
                      start=start)
        chk(ln is not None, 'C17g 日志记了 (saved + 0xF1 queued)（现场唯一的观测窗口）')
        chk(wait_log(r'thresh queued', start=start) is not None,
            'C17h 真的排了 0xF1（配了不下发＝没配）')

        print('C18 0xA4 同值重发 ⇒ ack 0x01 且走 unchanged（不重设采集板）')
        start = len(logtext())
        plat.clear()
        plat.send(build_downlink(0xA4, th_ok))
        f = plat.wait(0xA4, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'C18a 同值 ⇒ ack=0x01')
        chk(wait_log(r'0xA4 unchanged', start=start) is not None,
            'C18b 走的是 unchanged 分支')

        print('C19 0xA4 载荷不足 ⇒ 0x74，且不改坏配置')
        plat.clear()
        plat.send(build_downlink(0xA4, th_ok[:8]))
        f = plat.wait(0xA4, kind='bin')
        chk(f is not None and f['code'] == 0x74, 'C19a 8 B ⇒ ack=0x74')
        chk(float(cls_items('elec').get('elec.volt_high') or -1) == 300.0,
            'C19b 短载荷没把已存的值改坏')

        print('C20 0xA4 倾斜 91° 越界（上界 90）⇒ 0x74 且前几项一起回滚')
        th_bad = bytes([0x01, 0x2c, 0x00, 0x50, 0x03, 0xe8, 0x00, 0x5b, 0x00, 0x19])
        plat.clear()
        plat.send(build_downlink(0xA4, th_bad))
        f = plat.wait(0xA4, kind='bin')
        chk(f is not None and f['code'] == 0x74, 'C20a 倾斜 91 ⇒ ack=0x74')
        el = cls_items('elec')
        chk(float(el.get('elec.volt_high') or -1) == 300.0 and
            float(el.get('elec.over_current') or -1) == 1000.0,
            'C20b 被拒时已写的三项回滚了（high=%r oc=%r）'
            % (el.get('elec.volt_high'), el.get('elec.over_current')))

        # ---- C21~C27 0xBF/0xAD「配置重启网络传输设备电源」、0xF9/0xFA 温湿度阈值 ----
        # 真帧（2026-10-27 11:19:43 板端抓的，用户点「配置」、界面 6 次 / 60 秒）：
        #   f0f0 110a 000b 2cc1 bf ffffffffffffffff 02 06 3c 0e ffff
        # 载荷 2 B ＝ 重启次数(6) ＋ 重启时间(60 秒)，各 1 字节。
        # ⚠ 指令字典把这条记作 **0xAD**，平台实发 **0xBF** ⇒ 两个码都注册。
        print('C21 0xBF 配置重启网络传输设备电源（2 B）⇒ ack 0x01 ＋ 两个键落盘')
        nt0 = cls_items('netthr')
        start = len(logtext())
        plat.clear()
        plat.send(build_downlink(0xBF, bytes([6, 60])))
        f = plat.wait(0xBF, kind='bin')
        chk(f is not None and f['code'] == 0x01,
            'C21a 0xBF ⇒ ack=0x01（**本次修的**：原来必回 0x74）')
        nt = cls_items('netthr')
        chk(int(nt.get('netthr.reboot_max') or -1) == 6,
            'C21b 重启次数 6 → netthr.reboot_max（实际 %r）'
            % nt.get('netthr.reboot_max'))
        chk(int(nt.get('netthr.reboot_interval_s') or -1) == 60,
            'C21c 重启时间 60 → netthr.reboot_interval_s（实际 %r）'
            % nt.get('netthr.reboot_interval_s'))
        chk(wait_log(r'platform set reboot power max=6 interval=60s', start=start)
            is not None, 'C21d 日志记了 max=6 interval=60s（现场唯一观测窗口）')

        print('C22 0xBF 同值重发 ⇒ ack 0x01 且走 unchanged')
        start = len(logtext())
        plat.clear()
        plat.send(build_downlink(0xBF, bytes([6, 60])))
        f = plat.wait(0xBF, kind='bin')
        chk(f is not None and f['code'] == 0x01, 'C22a 同值 ⇒ ack=0x01')
        chk(wait_log(r'0xBF unchanged', start=start) is not None,
            'C22b 走的是 unchanged 分支')

        print('C23 0xAD（指令字典码，同一功能）也要接住')
        plat.clear()
        plat.send(build_downlink(0xAD, bytes([3, 30])))
        f = plat.wait(0xAD, kind='bin')
        chk(f is not None and f['code'] == 0x01,
            'C23a 0xAD ⇒ ack=0x01（字典与平台对不上，两个码都注册）')
        nt = cls_items('netthr')
        chk(int(nt.get('netthr.reboot_max') or -1) == 3 and
            int(nt.get('netthr.reboot_interval_s') or -1) == 30,
            'C23b 0xAD 也真的改到了（max=%r interval=%r）'
            % (nt.get('netthr.reboot_max'), nt.get('netthr.reboot_interval_s')))

        print('C24 0xBF 载荷 1 B ⇒ 0x74（既不是 2 也不是 4）')
        plat.clear()
        plat.send(build_downlink(0xBF, bytes([6])))
        f = plat.wait(0xBF, kind='bin')
        chk(f is not None and f['code'] == 0x74, 'C24a 1 B ⇒ ack=0x74')
        chk(int(cls_items('netthr').get('netthr.reboot_max') or -1) == 3,
            'C24b 短载荷没把已存的值改坏')

        print('C25 0xFA 配置温度阈值（1 字节有符号，50 / -2℃）⇒ 落 sensor.temp_*')
        start = len(logtext())
        plat.clear()
        plat.send(build_downlink(0xFA, bytes([50, 0xFE])))
        f = plat.wait(0xFA, kind='bin')
        chk(f is not None and f['code'] == 0x01,
            'C25a 0xFA ⇒ ack=0x01（**本次修的**：原来必回 0x74）')
        se = cls_items('sensor')
        chk(float(se.get('sensor.temp_high') or -1) == 50.0,
            'C25b 温度上限 50 → sensor.temp_high（实际 %r）'
            % se.get('sensor.temp_high'))
        chk(float(se.get('sensor.temp_low') or 999) == -2.0,
            'C25c 温度下限 **-2**（0xFE 按有符号解释）→ sensor.temp_low（实际 %r）'
            % se.get('sensor.temp_low'))
        chk(wait_log(r'platform set temperature high=50 low=-2', start=start)
            is not None, 'C25d 日志记了 high=50 low=-2')
        chk(wait_log(r'thresh queued', start=start) is not None,
            'C25e 改完真的重新排了 0xF1（否则采集板还是旧阈值）')

        print('C26 0xFA 4 字节大端（2 字节宽）也要接住：40 / 5℃')
        plat.clear()
        plat.send(build_downlink(0xFA, bytes([0x00, 40, 0x00, 5])))
        f = plat.wait(0xFA, kind='bin')
        chk(f is not None and f['code'] == 0x01,
            'C26a 4 B 载荷 ⇒ ack=0x01（宽度按实收字节数分派，不赌一种）')
        se = cls_items('sensor')
        chk(float(se.get('sensor.temp_high') or -1) == 40.0 and
            float(se.get('sensor.temp_low') or 999) == 5.0,
            'C26b 4 B 真的解成了 40/5（实际 %r/%r）'
            % (se.get('sensor.temp_high'), se.get('sensor.temp_low')))

        print('C27 0xF9 配置湿度阈值（1 字节无符号，80 / 20%）⇒ 落 sensor.humi_*')
        plat.clear()
        plat.send(build_downlink(0xF9, bytes([80, 20])))
        f = plat.wait(0xF9, kind='bin')
        chk(f is not None and f['code'] == 0x01,
            'C27a 0xF9 ⇒ ack=0x01（**本次修的**：原来必回 0x74）')
        se = cls_items('sensor')
        chk(float(se.get('sensor.humi_high') or -1) == 80.0 and
            float(se.get('sensor.humi_low') or -1) == 20.0,
            'C27b 湿度 80/20 → sensor.humi_high/low（实际 %r/%r）'
            % (se.get('sensor.humi_high'), se.get('sensor.humi_low')))

        print('C28 0xF9 上下限倒置（20 / 80）⇒ 0x74 且不改坏')
        plat.clear()
        plat.send(build_downlink(0xF9, bytes([20, 80])))
        f = plat.wait(0xF9, kind='bin')
        chk(f is not None and f['code'] == 0x74, 'C28a 倒置 ⇒ ack=0x74')
        se = cls_items('sensor')
        chk(float(se.get('sensor.humi_high') or -1) == 80.0 and
            float(se.get('sensor.humi_low') or -1) == 20.0,
            'C28b 倒置被拒后原值没被动（high=%r low=%r）'
            % (se.get('sensor.humi_high'), se.get('sensor.humi_low')))

        # 温湿度四键立刻还原（原值取自 C17 的 se0 快照）
        se_restore = {k: se0[k] for k in ('sensor.temp_high', 'sensor.temp_low',
                                          'sensor.humi_high', 'sensor.humi_low')
                      if k in se0}
        if se_restore:
            apply_cls('sensor', se_restore)
        # `netthr` 两键同理（reboot_max 原值可能是 0＝禁用自愈，别按出厂猜）
        if nt0:
            nt_restore = {k: nt0[k] for k in ('netthr.reboot_max',
                                              'netthr.reboot_interval_s')
                          if k in nt0}
            if nt_restore:
                apply_cls('netthr', nt_restore)
        nt = cls_items('netthr')
        chk(int(nt.get('netthr.reboot_max') or -1) ==
            int(nt0.get('netthr.reboot_max') or -1),
            'C29 netthr.reboot_max 已还原（%r → %r）'
            % (nt0.get('netthr.reboot_max'), nt.get('netthr.reboot_max')))
        se = cls_items('sensor')
        chk(float(se.get('sensor.temp_high') or -1) ==
            float(se0.get('sensor.temp_high') or -1),
            'C30 sensor.temp_high 已还原（%r → %r）'
            % (se0.get('sensor.temp_high'), se.get('sensor.temp_high')))

        # 立刻还原这五个键（别指望末尾的批量还原：那条路径的"原值"可能已被污染）
        for k, src in (('elec.volt_high', el0), ('elec.volt_low', el0),
                       ('elec.over_current', el0), ('elec.leak_current', el0),
                       ('sensor.tilt', se0)):
            if k in src:
                ctl('config.apply', {'ver': cls_ver('elec' if k.startswith('elec.') else 'sensor'),
                                     'items': {k: src[k]}})

        # ---- 收尾：把动过的键按**原值**还原 ----
        print('Z 收尾：还原现场配置')
        keep = ('net.ip', 'net.mask', 'net.gateway',
                'net.server.wired.host', 'net.server.wired.port',
                'net.server.wired.host_bak', 'net.server.wired.port_bak',
                'net.server.wireless.host', 'net.server.wireless.port',
                'net.server.wireless.host_bak', 'net.server.wireless.port_bak')
        apply_cls('net', {k: orig['net'][k] for k in keep if k in orig['net']})
        apply_cls('sys', {'sys.report.interval_s':
                          orig['sys'].get('sys.report.interval_s', 60)})
        apply_cls('probe', {'probe.target1': orig['probe'].get('probe.target1',
                                                              '192.168.2.1'),
                            'probe.target2': orig['probe'].get('probe.target2',
                                                              '192.168.2.2'),
                            'probe.timeout_ms': orig['probe'].get('probe.timeout_ms',
                                                                  2000)})
        # `camera.ch.N.ip` 同理：C11/C15 会改第 2 路（再清空），不还原就是
        # "跑一次用例把现场某一路摄像机 IP 抹掉"。
        cam_restore = {'camera.detect.mode': orig['camera'].get('camera.detect.mode', 1)}
        for i in range(6):
            k = 'camera.ch.%d.ip' % i
            if k in orig['camera']:
                cam_restore[k] = orig['camera'][k]
        apply_cls('camera', cam_restore)
        it = cls_items('net')
        chk(it.get('net.server.wired.host') == orig['net'].get('net.server.wired.host'),
            'Z1 平台地址已还原')
        chk(cls_items('camera').get('camera.detect.mode') ==
            orig['camera'].get('camera.detect.mode'), 'Z2 检测方式已还原')
        # `net.ip`/`mask`/`gateway` 原来**根本没进还原清单** —— C6 把它们改成
        # "本机地址 + 0.0.0.0"，于是每跑一次就固化一次，下次 C6 又读到"已污染的原值"。
        # 现场设备跑这条用例会把本机地址写进配置：必须还原。
        chk(it.get('net.ip') == orig['net'].get('net.ip'),
            'Z3 net.ip 已还原（本机 %r ← 原值 %r）'
            % (it.get('net.ip'), orig['net'].get('net.ip')))
        chk(it.get('net.gateway') == orig['net'].get('net.gateway'),
            'Z4 net.gateway 已还原（本机 %r ← 原值 %r）'
            % (it.get('net.gateway'), orig['net'].get('net.gateway')))
        camn = cls_items('camera')
        bad = [k for k in cam_restore if k.startswith('camera.ch.') and
               (camn.get(k) or '') != (orig['camera'].get(k) or '')]
        chk(not bad, 'Z5 六路摄像机 IP 已还原（不一致的键：%s）'
            % (', '.join(bad) if bad else '无'))
    finally:
        print('DIAG: accept=%d bytes_read=%d parsed=%d live=%d err=%s'
              % (plat.nacc, plat.nread, plat.nparsed, len(plat.frames),
                 'none' if plat.err is None else 'YES'))
        print('DIAG first bytes: %s' % (plat.first.hex() if plat.first else 'none'))
        if plat.err is not None:
            print(plat.err)
        plat.close()
        try:
            p.terminate()
            p.wait(timeout=8)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass

    print('')
    if fails:
        print('%d 项失败：' % len(fails))
        for f in fails:
            print('  - ' + f)
        return 1
    print('全部通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())
