#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mkota.py —— 造 IVSOTA01 升级包（联调/产线自用）

包格式见 `include/ivsbox/iv_ota.h`（**以那份头文件为准**，本脚本只是照它写字节）：

    偏移 0                magic[8]  "IVSOTA01"
    偏移 8                u32 LE    manifest_len
    偏移 12               u32 LE    payload_len
    16                    manifest  行式 ASCII（\n 结尾，末行也要有）
    16+mlen               payload   各文件内容按清单顺序首尾相接
    16+mlen+plen          sig[64]   Ed25519，覆盖**它之前的全部字节**

三条容易错的地方（本脚本已按规则处理）：
  1. 清单 `files=N` 必须与 `file=` 行数**精确相等**，`size` 合计必须**等于**
     `payload_len`；sha256 必须是 **64 位小写 hex**（大写不接受）。
  2. `version` 是 `M.m.p`（三段，每段 <= 65535），**不是** `iv_version_sysver()`
     那种四段串；它必须**严格大于**设备当前版本，且设备当前版本 >= `minrollback`。
  3. 路径围栏：不以 `/` 开头、不含 `..` 分量、不含 `:`、不含反斜杠、不以 `/` 结尾。

用法：见 `--help` 或文件末尾的示例。
"""
import argparse
import hashlib
import os
import struct
import sys

MAGIC = b"IVSOTA01"
MANIFEST_MAX = 8192
FILES_MAX = 32
PATH_MAX = 95  # 含 NUL 是 96


def die(msg):
    sys.stderr.write("mkota: %s\n" % msg)
    sys.exit(2)


# ---------------------------------------------------------------------------
# 校验（与设备侧同一套规则，早失败早报错）
# ---------------------------------------------------------------------------

def check_rel_path(p):
    if not p:
        die("空的文件路径")
    if len(p) > PATH_MAX:
        die("路径过长（> %d）：%s" % (PATH_MAX, p))
    if p.startswith("/"):
        die("路径不能以 / 开头（禁止绝对路径）：%s" % p)
    if p.endswith("/"):
        die("路径不能以 / 结尾：%s" % p)
    if ":" in p:
        die("路径里不能有 ':'（清单用 ':' 分段）：%s" % p)
    if "\\" in p:
        die("路径里不能有反斜杠：%s" % p)
    if ".." in p.split("/"):
        die("路径里不能有 '..' 分量：%s" % p)
    for ch in p:
        o = ord(ch)
        if o < 0x20 or o == 0x7F:
            die("路径里有控制字符：%r" % p)


def check_version(v):
    parts = v.split(".")
    if len(parts) != 3:
        die("version 必须是 M.m.p 三段（不是 %r）" % v)
    for x in parts:
        if not x.isdigit():
            die("version 段必须全数字：%r" % v)
        if int(x) > 65535:
            die("version 段超出 65535：%r" % v)


# ---------------------------------------------------------------------------
# 造包
# ---------------------------------------------------------------------------

def cmd_build(a):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    check_version(a.version)
    check_version(a.minrollback)

    # ---- 读文件 + 拼清单 ----
    lines = []
    payload = bytearray()
    lines.append("pkgfmt=1")
    lines.append("model=%s" % a.model)
    lines.append("hwmin=%d" % a.hwmin)
    lines.append("hwmax=%d" % a.hwmax)
    lines.append("version=%s" % a.version)
    lines.append("minrollback=%s" % a.minrollback)
    lines.append("files=%d" % len(a.file))

    if len(a.file) == 0:
        die("至少要给一个 --file")
    if len(a.file) > FILES_MAX:
        die("文件数超过 %d" % FILES_MAX)

    for spec in a.file:
        if "=" not in spec:
            die("--file 的写法是 <包内相对路径>=<本地文件>：%r" % spec)
        dest, src = spec.split("=", 1)
        check_rel_path(dest)
        if dest.startswith("bin/") is False and a.strict_bin:
            die("这个包只装 %s，路径必须在 bin/ 下：%s" % ("程序", dest))
        try:
            with open(src, "rb") as fh:
                blob = fh.read()
        except OSError as e:
            die("读不到 %s：%s" % (src, e))
        # 程序要能被准入检查认出来：必须是非空的 ELF
        if dest.startswith("bin/") and not blob.startswith(b"\x7fELF"):
            die("%s 不是 ELF（准入检查会拒）" % src)
        digest = hashlib.sha256(blob).hexdigest()
        lines.append("file=%s:%d:%s" % (dest, len(blob), digest))
        payload += blob
        print("  %-24s %8d B  %s" % (dest, len(blob), digest[:16] + "…"))

    manifest = ("\n".join(lines) + "\n").encode("ascii")
    if len(manifest) > MANIFEST_MAX:
        die("清单 %d B 超过上限 %d" % (len(manifest), MANIFEST_MAX))

    header = MAGIC + struct.pack("<II", len(manifest), len(payload))
    base = header + manifest + bytes(payload)

    # ---- 签名（Ed25519，覆盖 header+manifest+payload）----
    try:
        with open(a.key, "rb") as fh:
            key = serialization.load_pem_private_key(fh.read(), password=None)
    except OSError as e:
        die("读不到私钥 %s：%s" % (a.key, e))
    if not isinstance(key, Ed25519PrivateKey):
        die("%s 不是 Ed25519 私钥" % a.key)
    sig = key.sign(base)
    if len(sig) != 64:
        die("签名长度异常：%d" % len(sig))

    pkg = base + sig
    out_dir = os.path.dirname(os.path.abspath(a.out))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    with open(a.out, "wb") as fh:
        fh.write(pkg)
        fh.flush()
        os.fsync(fh.fileno())

    pub = key.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw)
    print("")
    print("包：      %s（%d B）" % (a.out, len(pkg)))
    print("版本：    %s  (minrollback %s)" % (a.version, a.minrollback))
    print("型号：    %s  hw %d..%d" % (a.model, a.hwmin, a.hwmax))
    print("公钥 hex：%s" % pub.hex())
    print("")
    print("设备侧验签命令（联调用，量产要把公钥写进 iv_ota_pubkey.c）：")
    print("  ivsbox-updater install %s --pubkey-hex %s" % (a.out, pub.hex()))
    return 0


def cmd_genkey(a):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    if not os.path.isdir(a.out_dir):
        os.makedirs(a.out_dir)
    key = Ed25519PrivateKey.generate()
    priv_pem = key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption())
    pub_raw = key.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw)
    p = os.path.join(a.out_dir, "ed25519.pem")
    with open(p, "wb") as fh:
        fh.write(priv_pem)
    os.chmod(p, 0o600)
    with open(os.path.join(a.out_dir, "ed25519.pub.hex"), "w") as fh:
        fh.write(pub_raw.hex() + "\n")
    print("私钥：%s（0600，**不要入库**）" % p)
    print("公钥：%s" % pub_raw.hex())
    return 0


def cmd_info(a):
    """写 info.txt（设备据此判断"有没有新版本"）。"""
    # 设备期望的 version 是 `<硬件名>-<iv_version_sysver()>`，四段日期串，
    # 与包清单里的 M.m.p **不是同一个概念**，两者都别写错。
    text = '{"version":"%s-%s","url":"%s"}\n' % (a.hw, a.version, a.url)
    # 设备取的路径是 `GET /<硬件名>/info.txt` ⇒ 输出的父目录通常还不存在，自己铺。
    parent = os.path.dirname(os.path.abspath(a.out))
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    with open(a.out, "w") as fh:
        fh.write(text)
    print("info.txt -> %s" % a.out)
    print(text.rstrip())
    return 0


def main():
    ap = argparse.ArgumentParser(description="造 IVSOTA01 升级包 / 测试密钥 / info.txt")
    sub = ap.add_subparsers(dest="cmd")

    b = sub.add_parser("build", help="造包")
    b.add_argument("--out", required=True, help="输出 .pkg 路径")
    b.add_argument("--version", required=True, help="包版本 M.m.p（须 > 设备当前版本）")
    b.add_argument("--minrollback", default="1.0.0", help="允许升级的本机版本下限")
    b.add_argument("--model", default="IVSBOX-T113", help="产品型号（必须与设备一致）")
    b.add_argument("--hwmin", type=int, default=1)
    b.add_argument("--hwmax", type=int, default=2)
    b.add_argument("--key", required=True, help="Ed25519 私钥 PEM")
    b.add_argument("--file", action="append", default=[],
                   help="<包内相对路径>=<本地文件>，可重复")
    b.add_argument("--any-path", dest="strict_bin", action="store_false",
                   help="允许非 bin/ 路径（默认只允许 bin/，与运行期换版范围一致）")
    b.set_defaults(func=cmd_build)

    g = sub.add_parser("genkey", help="生成 Ed25519 测试密钥对")
    g.add_argument("--out-dir", default=".", help="输出目录")
    g.set_defaults(func=cmd_genkey)

    i = sub.add_parser("info", help="写 info.txt")
    i.add_argument("--hw", default="IVSBOX-T113")
    i.add_argument("--version", required=True, help="设备本机 <sysver>（四段日期串）")
    i.add_argument("--url", required=True, help="包的可下载 URL")
    i.add_argument("--out", default="info.txt")
    i.set_defaults(func=cmd_info)

    a = ap.parse_args()
    if not getattr(a, "func", None):
        ap.print_help()
        return 1
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
