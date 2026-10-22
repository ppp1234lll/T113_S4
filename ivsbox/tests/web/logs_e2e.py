#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ivsbox-web 日志浏览/下载 端到端取证（VM 上跑，不需要 ivsboxd）

覆盖：
  L1  登录后 GET /api/logs 列出真实目录里的 <日期>/<小时>.log（非 .log / 非日期目录被忽略）
  L2  列表按 mtime 降序，最新的排第一
  L3  ?max=1 只回 1 条，但 total 仍是全量
  L4  GET /api/logs/file?f=<编码后的斜杠> 正文与磁盘内容逐字节相同
  L5  响应头带 Content-Disposition，文件名里的 / 换成 _
  L6  f 里带 ../ ⇒ 403
  L7  不存在的文件 ⇒ 404
  L8  无 Cookie ⇒ 401
  L9  POST ⇒ 405
  L10 /api/logsXYZ 不归本层（不是路径段前缀）
  L11 日志根不存在时回空列表（不是 500，也不是假条目）
"""
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.parse

PORT = 18099
BASE = '/tmp/loge2e'
ROOT = BASE + '/log'
STATIC = os.path.abspath('web/static')
BIN = os.path.abspath('build/host/ivsbox-web')

fails = []


def chk(cond, what):
    if cond:
        print('  PASS ' + what)
    else:
        print('  FAIL ' + what)
        fails.append(what)


def req(method, path, body=None, cookie=None, timeout=10):
    c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=timeout)
    h = {}
    if cookie:
        h['Cookie'] = 'ivs_sid=' + cookie
    if body is not None:
        h['Content-Type'] = 'application/json'
        body = json.dumps(body).encode()
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    hdrs = dict((k.lower(), v) for k, v in r.getheaders())
    c.close()
    return r.status, hdrs, data


def wait_port(deadline=10.0):
    t0 = time.time()
    while time.time() - t0 < deadline:
        s = socket.socket()
        s.settimeout(0.3)
        try:
            s.connect(('127.0.0.1', PORT))
            s.close()
            return True
        except OSError:
            s.close()
            time.sleep(0.1)
    return False


def main():
    shutil.rmtree(BASE, ignore_errors=True)
    os.makedirs(ROOT + '/2026-10-22')
    os.makedirs(ROOT + '/2026-10-21')
    os.makedirs(ROOT + '/not-a-date')          # 非日期目录：必须被忽略
    body_a = 'line1\nline2\n中文一行\n' * 3
    with open(ROOT + '/2026-10-21/23.log', 'w', encoding='utf-8') as f:
        f.write(body_a)
    os.utime(ROOT + '/2026-10-21/23.log', (1761000000, 1761000000))
    with open(ROOT + '/2026-10-22/08.log', 'w', encoding='utf-8') as f:
        f.write('fresh\n')
    os.utime(ROOT + '/2026-10-22/08.log', (1761100000, 1761100000))
    # 不是 .log ⇒ 不能出现在列表里
    with open(ROOT + '/2026-10-22/notes.txt', 'w', encoding='utf-8') as f:
        f.write('x')
    with open(ROOT + '/not-a-date/1.log', 'w', encoding='utf-8') as f:
        f.write('x')

    logf = open(BASE + '/web.log', 'wb')
    p = subprocess.Popen(
        [BIN, '--port', str(PORT), '--root', STATIC, '--log-root', ROOT,
         '--chan', BASE + '/no-such.ctl', '--media-root', BASE + '/media',
         '--pass-file', '', '--pass', 'Abcd1234'],
        stdout=logf, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        if not wait_port():
            print('FAIL: ivsbox-web 没起来')
            sys.exit(1)

        st, _, data = req('POST', '/api/login', {'user': 'admin', 'pass': 'Abcd1234'})
        j = json.loads(data.decode())
        tok = (j.get('d') or {}).get('token', '')
        chk(st == 200 and tok != '', 'L0 登录拿到会话（%d）' % st)
        if tok == '':
            return

        print('L1 列表')
        st, hd, data = req('GET', '/api/logs?max=20', cookie=tok)
        j = json.loads(data.decode())
        files = (j.get('d') or {}).get('files') or []
        names = [f['f'] for f in files]
        chk(st == 200 and j.get('ok') is True, 'L1a 200 且 ok=true（%d）' % st)
        chk(names == ['2026-10-22/08.log', '2026-10-21/23.log'],
            'L1b 只有两个 .log（.txt 与非日期目录都被忽略），实得 %s' % names)
        chk((j.get('d') or {}).get('total') == 2, 'L1c total=2')
        chk(files and files[0]['size'] == 6, 'L1d 文件大小取真实 st_size（6）')
        chk(files and files[0]['mtime'] == 1761100000, 'L1e mtime 是真实值')
        chk(files[0]['mtime'] > files[1]['mtime'], 'L2 按 mtime 降序（最新在前）')
        chk((j.get('d') or {}).get('root') == ROOT, 'L3a 回显日志根')

        print('L3 max 截断')
        st, _, data = req('GET', '/api/logs?max=1', cookie=tok)
        j = json.loads(data.decode())
        chk(len((j.get('d') or {}).get('files') or []) == 1, 'L3b max=1 只回 1 条')
        chk((j.get('d') or {}).get('total') == 2 and (j.get('d') or {}).get('shown') == 1,
            'L3c total 仍是全量 2、shown=1')

        print('L4 下载（斜杠做 % 编码）')
        q = urllib.parse.quote('2026-10-21/23.log', safe='')
        st, hd, data = req('GET', '/api/logs/file?f=' + q, cookie=tok)
        chk(st == 200, 'L4a 200（%d）' % st)
        chk(data.decode('utf-8') == body_a, 'L4b 正文与磁盘逐字节相同（%d B）' % len(data))
        cd = hd.get('content-disposition', '')
        chk('attachment' in cd and '2026-10-21_23.log' in cd,
            'L5 Content-Disposition 里 / 换成 _：%r' % cd)
        chk(hd.get('content-length') == str(len(body_a.encode())),
            'L5b Content-Length 与文件长度一致')

        print('L6..L10 负路径')
        st, _, _ = req('GET', '/api/logs/file?f=' + urllib.parse.quote('../../etc/passwd', safe=''),
                       cookie=tok)
        chk(st == 403, 'L6 穿越路径 ⇒ 403（得 %d）' % st)
        st, _, _ = req('GET', '/api/logs/file?f=2026-10-22/99.log', cookie=tok)
        chk(st == 404, 'L7 不存在的文件 ⇒ 404（得 %d）' % st)
        st, _, _ = req('GET', '/api/logs')
        chk(st == 401, 'L8 无 Cookie ⇒ 401（得 %d）' % st)
        st, _, _ = req('POST', '/api/logs', body={}, cookie=tok)
        chk(st == 405, 'L9 POST ⇒ 405（得 %d）' % st)
        st, _, data = req('GET', '/api/logsXYZ', cookie=tok)
        chk(st == 404, 'L10 /api/logsXYZ 不归本层 ⇒ 404（得 %d）' % st)

        print('L11 日志根不存在')
        st, _, data = req('GET', '/api/logs', cookie=tok)
        # 换一个不存在的根需要重启；这里改用"删掉根再查"验证同一条路径
        shutil.rmtree(ROOT)
        st, _, data = req('GET', '/api/logs', cookie=tok)
        j = json.loads(data.decode())
        chk(st == 200 and j.get('ok') is True, 'L11a 根不在时仍 200/ok（得 %d）' % st)
        chk(((j.get('d') or {}).get('files') or []) == [], 'L11b 回空列表，不造假条目')
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
        logf.close()


main()
print('')
if fails:
    print('%d FAILED' % len(fails))
    sys.exit(1)
print('log e2e passed (21 assertions)')
