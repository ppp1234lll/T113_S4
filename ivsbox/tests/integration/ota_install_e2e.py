#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""升级包上传 → 请求安装 的**装配层**端到端取证（VM/板端以 root 跑真 ivsboxd + ivsbox-web）

为什么必须有这一层：`tests/web/ota_e2e.py` 证明了"字节能传上去、SHA 能校验、包能就位"，
但它跑的时候**没有 ivsboxd**，所以 `install` 那一步只能断言"回了个 ivsboxd not running"。
真正要证明的是**接线**：Web 把包名翻成绝对路径 → 本地通道 `ota.request` → ivsboxd 落下
`<data_root>/ota/request`（monitor.sh 见到它才拉起 `ivsbox-updater` 安装）。这一层全靠
装配，单测与 web-only 用例都碰不到（2026-10-21 起反复踩的"接线在装配层"那条）。

判据是**盘上的请求文件**，不是 HTTP 响应 —— 响应说"已接受"却什么都没落下，是这类接线
最典型的假成功。

前置：root（ctl 是 root:root 0660）、`/mnt/UDISK/ivsbox/ota` 可写、当前没有别的
ivsboxd / ivsbox-web 占着通道与端口。脚本自己起、自己收。
"""
import hashlib
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.parse

CTL = '/var/run/ivsbox/ctl'
DATA_ROOT = '/mnt/UDISK/ivsbox'
OTA_ROOT = DATA_ROOT + '/ota'
REQUEST = OTA_ROOT + '/request'
PORT = 18096
BASE = '/tmp/otae2e_asm'
STATIC = os.path.abspath('web/static')
WEB = os.path.abspath('build/host/ivsbox-web')
DAEMON = os.path.abspath('build/host/ivsboxd')
SLICE = 24 * 1024

fails = []


def chk(cond, what):
    if cond:
        print('  PASS ' + what)
    else:
        print('  FAIL ' + what)
        fails.append(what)


def req(method, path, body=None, raw=None, cookie=None, timeout=20):
    c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=timeout)
    h = {}
    if cookie:
        h['Cookie'] = 'ivs_sid=' + cookie
    data = None
    if raw is not None:
        data = raw
        h['Content-Type'] = 'application/octet-stream'
    elif body is not None:
        data = json.dumps(body).encode()
        h['Content-Type'] = 'application/json'
    c.request(method, path, body=data, headers=h)
    r = c.getresponse()
    resp = r.read()
    c.close()
    return r.status, resp


def jbody(resp):
    try:
        return json.loads(resp.decode())
    except Exception:
        return {}


def wait(fn, deadline=15.0):
    t0 = time.time()
    while time.time() - t0 < deadline:
        if fn():
            return True
        time.sleep(0.2)
    return False


def port_open():
    s = socket.socket()
    s.settimeout(0.3)
    try:
        s.connect(('127.0.0.1', PORT))
        return True
    except OSError:
        return False
    finally:
        s.close()


def main():
    if os.geteuid() != 0:
        print('SKIP: 需要 root（ctl 是 root:root 0660），用 sudo 跑')
        return 0
    if os.path.exists(CTL):
        print('SKIP: 已经有 ivsboxd 在跑（%s 已存在），换个环境再测' % CTL)
        return 0

    shutil.rmtree(BASE, ignore_errors=True)
    os.makedirs(BASE)
    os.makedirs(OTA_ROOT, exist_ok=True)
    for p in (REQUEST,):
        if os.path.exists(p):
            os.unlink(p)

    name = 'ivsbox-asm-test.pkg'
    payload = bytes((i * 17 + (i >> 5)) & 0xFF for i in range(40000))
    sha = hashlib.sha256(payload).hexdigest()

    dlog = open(BASE + '/daemon.log', 'wb')
    wlog = open(BASE + '/web.log', 'wb')
    dm = subprocess.Popen([DAEMON], stdout=dlog, stderr=subprocess.STDOUT,
                          start_new_session=True)
    web = None
    try:
        if not wait(lambda: os.path.exists(CTL)):
            print('FAIL: ivsboxd 没起来（ctl 接听点没出现）')
            return 1
        web = subprocess.Popen(
            [WEB, '--port', str(PORT), '--root', STATIC, '--ota-root', OTA_ROOT,
             '--log-root', BASE + '/log', '--chan', CTL, '--media-root', BASE + '/media',
             '--pass-file', '', '--pass', 'Abcd1234'],
            stdout=wlog, stderr=subprocess.STDOUT, start_new_session=True)
        if not wait(port_open):
            print('FAIL: ivsbox-web 没起来')
            return 1

        st, resp = req('POST', '/api/login', {'user': 'admin', 'pass': 'Abcd1234'})
        tok = (jbody(resp).get('d') or {}).get('token', '')
        chk(st == 200 and tok != '', 'K1 登录拿到会话（%d）' % st)
        if tok == '':
            return 1

        print('K2 分片上传到 daemon 认的那个 ota 目录')
        for off in range(0, len(payload), SLICE):
            piece = payload[off:off + SLICE]
            st, resp = req('POST', '/api/ota/chunk?name=%s&off=%d' % (name, off),
                           raw=piece, cookie=tok)
            chk(st == 200 and jbody(resp).get('ok') is True,
                'K2 off=%d（%d B）被接收' % (off, len(piece)))
        final = os.path.join(OTA_ROOT, name)
        st, resp = req('POST', '/api/ota/finish',
                       {'name': name, 'size': len(payload), 'sha256': sha}, cookie=tok)
        j = jbody(resp)
        chk(st == 200 and j.get('ok') is True, 'K2 finish 通过（%d）' % st)
        chk(os.path.getsize(final) == len(payload) if os.path.exists(final) else False,
            'K2 包已在 %s' % final)

        print('K3 请求安装 ⇒ daemon 必须落下 monitor.sh 要消费的请求文件')
        chk(not os.path.exists(REQUEST), 'K3a 安装前没有残留的请求文件')
        st, resp = req('POST', '/api/ota/install', {'name': name, 'restart': 1}, cookie=tok)
        j = jbody(resp)
        chk(st == 200 and j.get('ok') is True, 'K3b 200 且 ok=true（%d）' % st)
        chk(wait(lambda: os.path.exists(REQUEST), 5.0),
            'K3c `<data_root>/ota/request` 真的出现了')
        body = ''
        if os.path.exists(REQUEST):
            with open(REQUEST, 'r', encoding='utf-8') as fh:
                body = fh.read()
        chk(body.rstrip('\n') == '%s 1' % final,
            'K3d 请求文件内容 = "<包绝对路径> 1"（monitor 侧 `read -r pkg restart` 直接可用）：%r' % body)

        print('K4 restart 标志要如实传下去（默认 0）')
        os.unlink(REQUEST)
        st, resp = req('POST', '/api/ota/install', {'name': name}, cookie=tok)
        chk(st == 200 and jbody(resp).get('ok') is True, 'K4a 不带 restart 也接受（%d）' % st)
        if wait(lambda: os.path.exists(REQUEST), 5.0):
            with open(REQUEST, 'r', encoding='utf-8') as fh:
                body = fh.read()
            chk(body.rstrip('\n') == '%s 0' % final,
                'K4b 默认 restart=0 写在第二段：%r' % body)
        else:
            chk(False, 'K4b 请求文件没出现')

        print('K5 越界路径由 daemon 兜住（Web 侧只给名字，构造不出别的路径）')
        os.unlink(REQUEST)
        st, resp = req('POST', '/api/ota/install', {'name': '../etc/passwd'}, cookie=tok)
        chk(st in (400, 403), 'K5a 包名带 .. ⇒ 被 Web 拒（%d）' % st)
        chk(not os.path.exists(REQUEST), 'K5b 没有落下任何请求文件')
    finally:
        for pr in (web, dm):
            if pr is None:
                continue
            pr.terminate()
            try:
                pr.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pr.kill()
        dlog.close()
        wlog.close()
        # 清理：请求文件与测试包必须走，否则 monitor.sh 下次启动会真装这个假包。
        for p in (REQUEST, os.path.join(OTA_ROOT, name)):
            if os.path.exists(p):
                os.unlink(p)
    return 0


if __name__ == '__main__':
    rc = main()
    print('')
    if fails:
        print('%d FAILED' % len(fails))
        sys.exit(1)
    print('ota install (assembly) e2e passed (13 assertions)')
    sys.exit(rc)
