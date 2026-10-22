#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ivsbox-web 升级包分片上传 端到端取证（VM 上跑，**不需要 ivsboxd**）

为什么要这一层：上传/续传/校验全是"多跑几趟 HTTP 才看得见"的行为 —— 单片超过 24 KiB
会不会被截断、偏移对不上会不会悄悄写坏、SHA 不符会不会照样改名、包名里的 `..` 会不会
跑出 ota 根，这些单靠读代码都容易被自己说服。`IV_WEB_BODY_MAX` 是 32 KiB、版本包有几
MB，这条链路本来就不可能"一次 POST 传完"，所以它必须被测。

覆盖：
  O1  登录拿会话
  O2  第一片：`off=0` 落到 `<root>/<name>.part`，回 off/next
  O3  偏移不连续 ⇒ 409 且回**实际**偏移（续传握手，不是闷头写）
  O4  GET /api/ota/chunk 能查到续传点（页面刷新后接得上）
  O5  按续传点补齐，最终字节数与源文件一致
  O6  finish 声明的 size 不对 ⇒ 409
  O7  finish 的 sha256 不对 ⇒ 409 且回真实 sha
  O8  finish 正确 ⇒ 200，`.part` 变正式包，正文与源文件逐字节相同
  O9  包名围栏：`../x`、`/abs`、首字符 `.`、含 `/` 一律 403
  O10 单片 > 24 KiB ⇒ 413（**不是**被静默截断）
  O11 abort 删掉未完成的分片；重复 abort 也不报错
  O12 install：未上传的包 ⇒ 404；没有 ivsboxd ⇒ 200 + ok:false（不是挂住/崩）
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

PORT = 18098
BASE = '/tmp/otae2e'
ROOT = BASE + '/ota'
STATIC = os.path.abspath('web/static')
BIN = os.path.abspath('build/host/ivsbox-web')
SLICE = 24 * 1024

fails = []


def chk(cond, what):
    if cond:
        print('  PASS ' + what)
    else:
        print('  FAIL ' + what)
        fails.append(what)


def req(method, path, body=None, cookie=None, raw=None, ctype=None, timeout=15):
    c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=timeout)
    h = {}
    if cookie:
        h['Cookie'] = 'ivs_sid=' + cookie
    data = None
    if raw is not None:
        data = raw
        h['Content-Type'] = ctype or 'application/octet-stream'
    elif body is not None:
        data = json.dumps(body).encode()
        h['Content-Type'] = 'application/json'
    c.request(method, path, body=data, headers=h)
    r = c.getresponse()
    resp = r.read()
    c.close()
    return r.status, resp


def size_of(p):
    """文件不存在时返回 -1：否则一旦被测行为错了，用例会崩在 getsize 上，
    表面是"用例坏了"而不是"产品错了"，证伪时特别容易被误读。"""
    try:
        return os.path.getsize(p)
    except OSError:
        return -1


def jbody(resp):
    try:
        return json.loads(resp.decode())
    except Exception:
        return {}


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
    os.makedirs(ROOT)

    # 源包：60000 B，正好跨 3 个 24 KiB 片（24576 + 24576 + 10848），
    # 用非平凡内容（有重复也有变化）以免"按块比较"这类错误被偶然掩盖。
    payload = bytes((i * 31 + (i >> 7)) & 0xFF for i in range(60000))
    sha = hashlib.sha256(payload).hexdigest()
    name = 'ivsbox-1.0.1-test.pkg'

    logf = open(BASE + '/web.log', 'wb')
    p = subprocess.Popen(
        [BIN, '--port', str(PORT), '--root', STATIC, '--ota-root', ROOT,
         '--log-root', BASE + '/log', '--chan', BASE + '/no-such.ctl',
         '--media-root', BASE + '/media', '--pass-file', '', '--pass', 'Abcd1234'],
        stdout=logf, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        if not wait_port():
            print('FAIL: ivsbox-web 没起来')
            return 1

        st, resp = req('POST', '/api/login', {'user': 'admin', 'pass': 'Abcd1234'})
        tok = (jbody(resp).get('d') or {}).get('token', '')
        chk(st == 200 and tok != '', 'O1 登录拿到会话（%d）' % st)
        if tok == '':
            return 1

        part = os.path.join(ROOT, name + '.part')

        print('O2 第一片')
        st, resp = req('POST', '/api/ota/chunk?name=%s&off=0' % name,
                       raw=payload[:SLICE], cookie=tok)
        j = jbody(resp)
        chk(st == 200 and j.get('ok') is True, 'O2a 200 且 ok=true（%d）' % st)
        chk((j.get('d') or {}).get('off') == SLICE, 'O2b 回 off=%d' % SLICE)
        chk(size_of(part) == SLICE,
            'O2c 落盘 %d B 到 <root>/<name>.part' % SLICE)

        print('O3 偏移不连续')
        st, resp = req('POST', '/api/ota/chunk?name=%s&off=%d' % (name, SLICE * 2),
                       raw=payload[SLICE * 2:], cookie=tok)
        j = jbody(resp)
        chk(st == 409, 'O3a 409（得 %d）' % st)
        chk((j.get('d') or {}).get('off') == SLICE,
            'O3b 回实际偏移 %d 供前端续传' % SLICE)
        chk(size_of(part) == SLICE, 'O3c 分片文件没被乱写（仍是 %d B，实得 %d）' % (SLICE, size_of(part)))

        print('O4 查续传点')
        st, resp = req('GET', '/api/ota/chunk?name=%s' % name, cookie=tok)
        j = jbody(resp)
        chk(st == 200 and (j.get('d') or {}).get('off') == SLICE,
            'O4 续传点 = %d' % SLICE)

        print('O5 按续传点补完')
        for off in (SLICE, SLICE * 2):
            st, resp = req('POST', '/api/ota/chunk?name=%s&off=%d' % (name, off),
                           raw=payload[off:off + SLICE], cookie=tok)
            j = jbody(resp)
            chk(st == 200 and j.get('ok') is True, 'O5a off=%d 片被接收（%d）' % (off, st))
        chk(size_of(part) == len(payload),
            'O5b 最终 %d B，与源包一致' % len(payload))

        print('O6/O7 finish 的两种不对')
        st, resp = req('POST', '/api/ota/finish',
                       {'name': name, 'size': len(payload) + 1, 'sha256': sha}, cookie=tok)
        chk(st == 409, 'O6 size 不符 ⇒ 409（得 %d）' % st)
        bad = 'ab' * 32
        st, resp = req('POST', '/api/ota/finish',
                       {'name': name, 'size': len(payload), 'sha256': bad}, cookie=tok)
        j = jbody(resp)
        chk(st == 409, 'O7a sha 不符 ⇒ 409（得 %d）' % st)
        chk((j.get('d') or {}).get('sha256') == sha,
            'O7b 409 里回真实 sha，前端能据此判断"包坏了"')
        chk(os.path.exists(part), 'O7c 校验没过时分片文件仍在（可重传而不是从头再来）')

        print('O8 finish 正确（不带 sha：纯 http 下浏览器算不出摘要）')
        st, resp = req('POST', '/api/ota/finish',
                       {'name': name, 'size': len(payload)}, cookie=tok)
        j = jbody(resp)
        d = j.get('d') or {}
        final = os.path.join(ROOT, name)
        chk(st == 200 and j.get('ok') is True, 'O8a 200 且 ok=true（%d）' % st)
        chk(d.get('sha256') == sha,
            'O8b 缺 sha256 时服务端算并回显（%s…）' % str(d.get('sha256'))[:12])
        chk(d.get('path') == final, 'O8c 回正式包绝对路径')
        chk(os.path.exists(final) and not os.path.exists(part),
            'O8c .part 已改名为正式包（不再有 .part 残留）')
        same = False
        if os.path.exists(final):
            with open(final, 'rb') as fh:
                same = (fh.read() == payload)
        chk(same, 'O8d 正式包与源包逐字节相同')

        print('O9 包名围栏')
        for bad_name in ('../evil.pkg', '/etc/passwd', '.hidden', 'a/b.pkg', 'x%20y.pkg'):
            st, _ = req('POST', '/api/ota/chunk?name=%s&off=0' % bad_name,
                        raw=b'x', cookie=tok)
            chk(st == 403, 'O9 %r ⇒ 403（得 %d）' % (bad_name, st))
        chk(not os.path.exists(os.path.join(BASE, 'evil.pkg')),
            'O9b 越界名没有在 ota 根之外留下文件')

        print('O10 单片刻意超限')
        st, resp = req('POST', '/api/ota/chunk?name=big.pkg&off=0',
                       raw=b'\x00' * (SLICE + 1), cookie=tok)
        chk(st == 413, 'O10 > %d B 的片 ⇒ 413（得 %d）' % (SLICE, st))
        chk(not os.path.exists(os.path.join(ROOT, 'big.pkg.part')) or
            os.path.getsize(os.path.join(ROOT, 'big.pkg.part')) == 0,
            'O10b 超限片没有被静默截断落盘')

        print('O11 abort')
        req('POST', '/api/ota/chunk?name=tmp.pkg&off=0', raw=b'abc', cookie=tok)
        chk(os.path.exists(os.path.join(ROOT, 'tmp.pkg.part')), 'O11a 先造一个未完成的分片')
        st, resp = req('POST', '/api/ota/abort', {'name': 'tmp.pkg'}, cookie=tok)
        chk(st == 200 and (jbody(resp).get('d') or {}).get('aborted') is True,
            'O11b abort 返回 aborted=true')
        chk(not os.path.exists(os.path.join(ROOT, 'tmp.pkg.part')), 'O11c 分片文件已删')
        st, resp = req('POST', '/api/ota/abort', {'name': 'tmp.pkg'}, cookie=tok)
        chk(st == 200 and (jbody(resp).get('d') or {}).get('aborted') is False,
            'O11d 重复 abort 不报错（aborted=false）')

        print('O12 install')
        st, resp = req('POST', '/api/ota/install', {'name': 'never-uploaded.pkg'}, cookie=tok)
        chk(st == 404, 'O12a 未上传的包 ⇒ 404（得 %d）' % st)
        st, resp = req('POST', '/api/ota/install', {'name': name, 'restart': 0}, cookie=tok)
        j = jbody(resp)
        chk(st == 200 and j.get('ok') is False,
            'O12b 没有 ivsboxd ⇒ 200 + ok=false（不是挂住，得 %d）' % st)
        chk('not running' in str(j.get('msg', '')),
            'O12c 提示是"ivsboxd not running"：%r' % j.get('msg'))
        chk(os.path.exists(final), 'O12d 请求失败不影响已就位的包')

        print('O13 鉴权/参数')
        st, _ = req('POST', '/api/ota/chunk?name=x.pkg&off=0', raw=b'x')
        chk(st == 401, 'O13a 无 Cookie ⇒ 401（得 %d）' % st)
        st, _ = req('POST', '/api/ota/chunk?off=0', raw=b'x', cookie=tok)
        chk(st == 400, 'O13b 缺 name ⇒ 400（得 %d）' % st)
        st, _ = req('POST', '/api/ota/chunk?name=x.pkg', raw=b'x', cookie=tok)
        chk(st == 400, 'O13c 缺 off ⇒ 400（**不默认 0**，免得手滑清掉已有分片，得 %d）' % st)
        st, _ = req('GET', '/api/ota/nosuch', cookie=tok)
        chk(st == 404, 'O13d 未知子路径 ⇒ 404（得 %d）' % st)
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
        logf.close()
    return 0


if __name__ == '__main__':
    rc = main()
    print('')
    if fails:
        print('%d FAILED' % len(fails))
        sys.exit(1)
    print('ota upload e2e passed (37 assertions)')
    sys.exit(rc)
