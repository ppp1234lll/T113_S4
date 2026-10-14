/*
 * iv_ota 单测（libivmodules，功能开发计划 M7-S7.1）
 *
 * ============================ 靶子是什么 ============================
 * 靶子是**拒绝条件**，不是"能跑通"：§16.2 场景 9 要求"未签名或型号不匹配的升级包
 * 必须被拒绝"，所以每组用例都问同一个问题 —— **这包该不该被拒、拒的理由对不对**。
 *
 * 签名侧：设备侧只验签，**没有签名代码**；本测试用 OpenSSL 现场造两把测试密钥，
 * 用其中一把签真包（正例），另一把用来验"签名来自别的密钥也要拒"。
 * 测试私钥写在文件里是安全的：它只在这一套用例内有效，与量产密钥无关，
 * 且量产私钥绝不入库（规则 3 第 6 条）。
 *
 * ============================ 覆盖 ============================
 *   t01 版本号：解析 / 比较 / 渲染，非法形式（缺段、多段、非数字、越界、空白）
 *   t02 清单解析：合法往返；10 类非法（未知键、重复键、缺键、行数不符、大写 hex、
 *       无尾换行、空行、hwmin>hwmax、files=0、超长）
 *   t03 正例：正常包（内存版 + 文件版）通过，清单字段逐个核对
 *   t04 篡改：负载单字节 / 清单单字节 / 签名单字节 / magic / 长度字段 → 各自被拒
 *   t05 型号不匹配 → EMODEL
 *   t06 硬件版本不在区间 → EHWVER
 *   t07 版本：同版本 / 降级 / 本机低于 minrollback → EVER；边界放行
 *   t08 完整性：清单摘要错 / 清单尺寸错 / 负载比清单合计长（后两者分别单独钉住
 *       第 3 步与第 9 步，避免"删掉检查也测不出来"的死检查）→ EFILE
 *   t09 路径围栏：`../`、绝对路径、`a/../../b`、`etc/./x` → EPATH（解析层 + 验签层）
 *   t10 签名缺失：全零签名、截断丢签名、尾部夹带、长度不足 → ESIG / EBADFMT
 *   t11 密钥：全零/ NULL 公钥 → ENOKEY；另一把密钥 → ESIG；参数非法不改 why
 *   t12 文件版：不存在 / 目录 / 空文件 → EIOFAIL；NULL 路径 → EINVAL
 *   t13 内置公钥占位：返回 ENOTSUP 且输出全零（fail-closed 的前提）+ reason 名
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "ivsbox/iv_ota.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_sha256.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 测试侧材料
 * ------------------------------------------------------------------------- */

#define PKG_CAP 65536u

typedef struct {
    uint8_t b[PKG_CAP];
    size_t  len;
} pkg_t;

/* 两把测试密钥的种子（32 字节）。A 为"发布密钥"，B 为"别人的密钥"。 */
static const uint8_t SEED_A[IV_OTA_SEED_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0
};
static const uint8_t SEED_B[IV_OTA_SEED_LEN] = {
    0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87, 0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f,
    0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11
};

static const iv_ota_self_t SELF = { "IVSBOX-T113", 1u, { 1u, 0u, 0u } };

static const uint8_t BODY_A[] = "ivsboxd-application-binary-payload-v1.0.1........";
static const uint8_t BODY_B[] = "{\"sys\":{\"name\":\"demo\"},\"camera\":{\"ch\":[]}}";

/* 清单注入点（单独触发某一层拒绝，别把多层原因混在一起）。 */
#define FAULT_HASH0 1u /* 第一个文件的摘要写假值 → 完整性层 */
#define FAULT_SIZE0 2u /* 第一个文件的尺寸写 +1 → 负载长度层 */

typedef struct {
    const char    *path;
    const uint8_t *data;
    size_t         len;
} tfile_t;

/* ---------------------------------------------------------------------------
 * 造包工具（只用 OpenSSL 签名，不碰被测代码）
 * ------------------------------------------------------------------------- */

static void wr_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* 由种子导出 Ed25519 公钥（32 字节）。 */
static int pubkey_from_seed(const uint8_t seed[IV_OTA_SEED_LEN],
                            uint8_t out[IV_OTA_PUBKEY_LEN])
{
    EVP_PKEY *k;
    size_t    n  = IV_OTA_PUBKEY_LEN;
    int       ok = 0;

    k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, IV_OTA_SEED_LEN);
    if (k != NULL) {
        ok = EVP_PKEY_get_raw_public_key(k, out, &n) == 1 && n == IV_OTA_PUBKEY_LEN;
        EVP_PKEY_free(k);
    }
    return ok;
}

static int sign_msg(const uint8_t seed[IV_OTA_SEED_LEN], const uint8_t *msg, size_t mlen,
                    uint8_t sig[IV_OTA_SIG_LEN])
{
    EVP_PKEY   *k;
    EVP_MD_CTX *ctx;
    size_t      sl = IV_OTA_SIG_LEN;
    int         ok = 0;

    k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, IV_OTA_SEED_LEN);
    if (k == NULL)
        return 0;
    ctx = EVP_MD_CTX_new();
    if (ctx != NULL) {
        if (EVP_DigestSignInit(ctx, NULL, NULL, NULL, k) == 1 &&
            EVP_DigestSign(ctx, sig, &sl, msg, mlen) == 1 && sl == IV_OTA_SIG_LEN)
            ok = 1;
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(k);
    return ok;
}

/* 生成清单文本（faults 见 FAULT_* 宏）。 */
static size_t mk_manifest(char *out, size_t cap, const char *model, unsigned hwmin,
                          unsigned hwmax, const char *ver, const char *minrb,
                          const tfile_t *f, size_t nf, unsigned faults)
{
    size_t used = 0u;
    size_t i;

    used += (size_t)snprintf(out + used, cap - used,
                             "pkgfmt=1\nmodel=%s\nhwmin=%u\nhwmax=%u\nversion=%s\n"
                             "minrollback=%s\nfiles=%u\n",
                             model, hwmin, hwmax, ver, minrb, (unsigned)nf);
    for (i = 0u; i < nf; i++) {
        uint8_t     d[IV_OTA_SHA256_LEN];
        iv_sha256_t c;
        char        hex[IV_OTA_SHA256_HEX_LEN + 1u];
        size_t      size = f[i].len;
        size_t      j;

        iv_sha256_init(&c);
        iv_sha256_update(&c, f[i].data, f[i].len);
        iv_sha256_final(&c, d);
        iv_sha256_hex(d, hex, sizeof(hex));

        if ((faults & FAULT_HASH0) != 0u && i == 0u) {
            for (j = 0u; j < IV_OTA_SHA256_HEX_LEN; j++)
                hex[j] = 'a';
            hex[IV_OTA_SHA256_HEX_LEN] = '\0';
        }
        if ((faults & FAULT_SIZE0) != 0u && i == 0u)
            size += 1u;
        used += (size_t)snprintf(out + used, cap - used, "file=%s:%u:%s\n", f[i].path,
                                 (unsigned)size, hex);
    }
    return used;
}

/* 负载 = 各文件首尾相接。 */
static size_t mk_payload(uint8_t *out, const tfile_t *f, size_t nf)
{
    size_t used = 0u;
    size_t i;

    for (i = 0u; i < nf; i++) {
        memcpy(out + used, f[i].data, f[i].len);
        used += f[i].len;
    }
    return used;
}

/* 装配完整包：头 + 清单 + 负载 + 签名（seed=NULL 时签名段留全零）。 */
static size_t build_pkg(pkg_t *pk, const char *manifest, size_t mlen, const uint8_t *payload,
                        size_t plen, const uint8_t seed[IV_OTA_SEED_LEN])
{
    size_t off;

    memcpy(pk->b, IV_OTA_MAGIC, IV_OTA_MAGIC_LEN);
    wr_u32_le(pk->b + 8, (uint32_t)mlen);
    wr_u32_le(pk->b + 12, (uint32_t)plen);
    memcpy(pk->b + IV_OTA_HEADER_LEN, manifest, mlen);
    memcpy(pk->b + IV_OTA_HEADER_LEN + mlen, payload, plen);
    off = IV_OTA_HEADER_LEN + mlen + plen;
    if (seed != NULL)
        (void)sign_msg(seed, pk->b, off, pk->b + off);
    else
        memset(pk->b + off, 0, IV_OTA_SIG_LEN);
    pk->len = off + IV_OTA_SIG_LEN;
    return pk->len;
}

static void files_default(tfile_t f[2])
{
    f[0].path = "bin/ivsboxd";
    f[0].data = BODY_A;
    f[0].len  = sizeof(BODY_A) - 1u;
    f[1].path = "etc/ivsbox.json";
    f[1].data = BODY_B;
    f[1].len  = sizeof(BODY_B) - 1u;
}

/* 正常装配（可调 model / 硬件区间 / 版本 / minrollback / 注入 / 签名密钥）。 */
static void mk_good_pkg(pkg_t *pk, const char *model, unsigned hwmin, unsigned hwmax,
                        const char *ver, const char *minrb, unsigned faults,
                        const uint8_t seed[IV_OTA_SEED_LEN])
{
    char    man[2048];
    uint8_t payload[1024];
    tfile_t f[2];
    size_t  mlen;
    size_t  plen;

    files_default(f);
    mlen = mk_manifest(man, sizeof(man), model, hwmin, hwmax, ver, minrb, f, 2u, faults);
    plen = mk_payload(payload, f, 2u);
    (void)build_pkg(pk, man, mlen, payload, plen, seed);
}

/* 负载区起始偏移（便于精确篡改负载而不动清单）。 */
static size_t payload_off(const pkg_t *pk)
{
    return (size_t)IV_OTA_HEADER_LEN + (size_t)rd_u32_le(pk->b + 8);
}

/* 在负载尾部插入 1 个填充字节并令头里的 payload_len 涨 1，然后**重新签名**：
 * 清单里声明的尺寸之和仍是原值 ⇒ 逐文件摘要全部对得上、签名也是真签，
 * 唯一能抓住它的就是"负载长度 vs 清单合计"（第 3 步）这一层。
 * 没有这条用例，第 3 步就是"删掉也测不出来"的死检查。 */
static void add_payload_pad_and_resign(pkg_t *pk, const uint8_t seed[IV_OTA_SEED_LEN])
{
    size_t mlen    = (size_t)rd_u32_le(pk->b + 8);
    size_t plen    = (size_t)rd_u32_le(pk->b + 12);
    size_t off_end = (size_t)IV_OTA_HEADER_LEN + mlen + plen; /* 签名起点 */

    memmove(pk->b + off_end + 1u, pk->b + off_end, IV_OTA_SIG_LEN);
    pk->b[off_end] = 0x00u;
    wr_u32_le(pk->b + 12, (uint32_t)(plen + 1u));
    pk->len += 1u;
    (void)sign_msg(seed, pk->b, pk->len - IV_OTA_SIG_LEN, pk->b + pk->len - IV_OTA_SIG_LEN);
}

/* 期望被判非法：核对返回码非 0 且 reason 精确命中。 */
static void expect_reject(const pkg_t *pk, const iv_ota_self_t *self,
                          const uint8_t pub[IV_OTA_PUBKEY_LEN], iv_ota_reason_t want,
                          const char *what)
{
    iv_ota_manifest_t m;
    iv_ota_reason_t   why = IV_OTA_OK;
    int               rc;

    memset(&m, 0, sizeof(m));
    rc = iv_ota_verify_mem(pk->b, pk->len, self, pub, &m, &why);
    if (rc == IV_OK) {
        fprintf(stderr, "FAIL: %s -- expected reject, got OK\n", what);
        g_fail++;
        return;
    }
    if (why != want) {
        fprintf(stderr, "FAIL: %s -- reason %s (%d), want %s (%d)\n", what,
                iv_ota_reason_name(why), (int)why, iv_ota_reason_name(want), (int)want);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * t01 版本号
 * ------------------------------------------------------------------------- */

static void t01_version(void)
{
    iv_ota_ver_t a;
    iv_ota_ver_t b;
    char         s[IV_OTA_VER_STR_MAX];
    char         big[IV_OTA_VER_STR_MAX + 4u];

    chk(iv_ota_ver_parse("1.2.3", 5u, &a) == IV_OK && a.major == 1u && a.minor == 2u &&
            a.patch == 3u,
        "t01: parse 1.2.3");
    chk(iv_ota_ver_parse("65535.65535.65535", 17u, &a) == IV_OK && a.major == 65535u,
        "t01: parse max values");
    chk(iv_ota_ver_parse("1.2", 3u, &a) == IV_EINVAL, "t01: '1.2' rejected");
    chk(iv_ota_ver_parse("1.2.3.4", 7u, &a) == IV_EINVAL, "t01: '1.2.3.4' rejected");
    chk(iv_ota_ver_parse("1..2", 4u, &a) == IV_EINVAL, "t01: '1..2' rejected");
    chk(iv_ota_ver_parse("1.2.", 4u, &a) == IV_EINVAL, "t01: '1.2.' rejected");
    chk(iv_ota_ver_parse("a.b.c", 5u, &a) == IV_EINVAL, "t01: 'a.b.c' rejected");
    chk(iv_ota_ver_parse("65536.0.0", 9u, &a) == IV_EINVAL, "t01: 65536 rejected");
    chk(iv_ota_ver_parse(" 1.2.3", 6u, &a) == IV_EINVAL, "t01: leading space rejected");
    chk(iv_ota_ver_parse("1.2.3 ", 6u, &a) == IV_EINVAL, "t01: trailing space rejected");
    chk(iv_ota_ver_parse("", 0u, &a) == IV_EINVAL, "t01: empty rejected");
    chk(iv_ota_ver_parse(NULL, 5u, &a) == IV_EINVAL, "t01: NULL rejected");

    a.major = 1u; a.minor = 0u; a.patch = 0u;
    b.major = 1u; b.minor = 0u; b.patch = 1u;
    chk(iv_ota_ver_cmp(a, b) < 0 && iv_ota_ver_cmp(b, a) > 0 && iv_ota_ver_cmp(a, a) == 0,
        "t01: compare ordering");
    a.major = 2u; b.major = 1u; b.minor = 65535u; b.patch = 65535u;
    chk(iv_ota_ver_cmp(a, b) > 0, "t01: major dominates");

    iv_ota_ver_format((iv_ota_ver_t){ 10u, 0u, 65535u }, s, sizeof(s));
    chk(strcmp(s, "10.0.65535") == 0, "t01: format round trip");
    chk(iv_ota_ver_parse(s, strlen(s), &b) == IV_OK && b.minor == 0u && b.patch == 65535u,
        "t01: parse(format(x)) == x");

    /* 容量不足：可以截断，但**必须 NUL 终止**、且不得越界写 */
    memset(big, 'Z', sizeof(big));
    iv_ota_ver_format((iv_ota_ver_t){ 1u, 2u, 3u }, big, 3u);
    chk(big[2] == '\0', "t01: short cap stays NUL-terminated");
    chk(big[3] == 'Z', "t01: short cap does not overrun");
    chk(big[0] == '1' && big[1] == '.', "t01: short cap keeps prefix");
    iv_ota_ver_format((iv_ota_ver_t){ 1u, 2u, 3u }, NULL, 10u);
    iv_ota_ver_format((iv_ota_ver_t){ 1u, 2u, 3u }, big, 0u);
}

/* ---------------------------------------------------------------------------
 * t02 清单解析
 * ------------------------------------------------------------------------- */

static void t02_manifest(void)
{
    iv_ota_manifest_t m;
    iv_ota_reason_t   why = IV_OTA_OK;
    tfile_t           f[2];
    char              man[2048];
    char              bad[2048];
    size_t            mlen;

    files_default(f);
    mlen = mk_manifest(man, sizeof(man), "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", f, 2u, 0u);

    chk(iv_ota_manifest_parse(man, mlen, &m, &why) == IV_OK, "t02: legal manifest parsed");
    chk(m.pkgfmt == 1u && m.n_files == 2u, "t02: pkgfmt/files");
    chk(strcmp(m.model, "IVSBOX-T113") == 0, "t02: model");
    chk(m.hw_min == 1u && m.hw_max == 2u, "t02: hw range");
    chk(m.version.major == 1u && m.version.minor == 0u && m.version.patch == 1u, "t02: version");
    chk(m.min_rollback.patch == 0u, "t02: min rollback");
    chk(strcmp(m.files[0].path, "bin/ivsboxd") == 0 && m.files[0].size == f[0].len,
        "t02: file 0 path/size");
    chk(strcmp(m.files[1].path, "etc/ivsbox.json") == 0 && m.files[1].size == f[1].len,
        "t02: file 1 path/size (dot in name allowed)");

    /* 未知键 */
    {
        size_t n = (size_t)snprintf(bad, sizeof(bad), "%soops=1\n", man);

        chk(iv_ota_manifest_parse(bad, n, &m, &why) == IV_EPROTO && why == IV_OTA_EBADFMT,
            "t02: unknown key rejected");
    }
    /* 重复单值键 */
    {
        size_t n = (size_t)snprintf(bad, sizeof(bad), "%smodel=IVSBOX-T113\n", man);

        chk(iv_ota_manifest_parse(bad, n, &m, &why) == IV_EPROTO && why == IV_OTA_EBADFMT,
            "t02: duplicated single-value key rejected");
    }
    /* 缺必需键（去掉 minrollback 行） */
    {
        const char *drop = "minrollback=1.0.0\n";
        const char *p    = strstr(man, drop);
        size_t      n;

        chk(p != NULL, "t02: locate minrollback line");
        if (p != NULL) {
            n = (size_t)(p - man);
            memcpy(bad, man, n);
            memcpy(bad + n, p + strlen(drop), mlen - n - strlen(drop));
            bad[mlen - strlen(drop)] = '\0';
            chk(iv_ota_manifest_parse(bad, mlen - strlen(drop), &m, &why) == IV_EPROTO &&
                    why == IV_OTA_EBADFMT,
                "t02: missing required key rejected");
        }
    }
    /* files= 与 file= 行数不符 */
    {
        size_t n  = mk_manifest(bad, sizeof(bad), "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", f, 2u,
                                0u);
        char  *p  = memmem(bad, n, "files=2", 7u);

        chk(p != NULL, "t02: locate files=2");
        if (p != NULL) {
            p[6] = '3';
            chk(iv_ota_manifest_parse(bad, n, &m, &why) == IV_EPROTO && why == IV_OTA_EBADFMT,
                "t02: files count mismatch rejected");
        }
    }
    /* 大写 hex */
    {
        size_t n = mk_manifest(bad, sizeof(bad), "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", f, 2u,
                               0u);
        char  *h = strchr(bad, ':');

        chk(h != NULL, "t02: locate first ':'");
        if (h != NULL) {
            h = strchr(h + 1, ':');
            chk(h != NULL, "t02: locate size separator");
            if (h != NULL) {
                h[1] = 'A';
                chk(iv_ota_manifest_parse(bad, n, &m, &why) == IV_EPROTO &&
                        why == IV_OTA_EBADFMT,
                    "t02: uppercase hex rejected");
            }
        }
    }
    /* 无尾换行 */
    chk(iv_ota_manifest_parse(man, mlen - 1u, &m, &why) == IV_EPROTO && why == IV_OTA_EBADFMT,
        "t02: missing final newline rejected");
    /* 空行 */
    {
        size_t n = (size_t)snprintf(bad, sizeof(bad), "%s\n", man);

        chk(iv_ota_manifest_parse(bad, n, &m, &why) == IV_EPROTO && why == IV_OTA_EBADFMT,
            "t02: empty line rejected");
    }
    /* 空键 */
    {
        size_t n = (size_t)snprintf(bad, sizeof(bad), "=1\n%s", man);

        chk(iv_ota_manifest_parse(bad, n, &m, &why) == IV_EPROTO && why == IV_OTA_EBADFMT,
            "t02: empty key rejected");
    }
    /* hwmin > hwmax */
    {
        size_t n = mk_manifest(bad, sizeof(bad), "IVSBOX-T113", 9u, 2u, "1.0.1", "1.0.0", f, 2u,
                               0u);

        chk(iv_ota_manifest_parse(bad, n, &m, &why) == IV_EPROTO && why == IV_OTA_EBADFMT,
            "t02: hwmin>hwmax rejected");
    }
    /* files=0 */
    {
        size_t n = (size_t)snprintf(bad, sizeof(bad),
                                    "pkgfmt=1\nmodel=IVSBOX-T113\nhwmin=1\nhwmax=2\n"
                                    "version=1.0.1\nminrollback=1.0.0\nfiles=0\n");

        chk(iv_ota_manifest_parse(bad, n, &m, &why) == IV_EPROTO && why == IV_OTA_EBADFMT,
            "t02: files=0 rejected");
    }
    /* pkgfmt 不是 1 */
    {
        size_t n = (size_t)snprintf(bad, sizeof(bad),
                                    "pkgfmt=2\nmodel=IVSBOX-T113\nhwmin=1\nhwmax=2\n"
                                    "version=1.0.1\nminrollback=1.0.0\nfiles=1\n"
                                    "file=bin/x:1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");

        chk(iv_ota_manifest_parse(bad, n, &m, &why) == IV_EPROTO && why == IV_OTA_EBADFMT,
            "t02: unknown pkgfmt rejected");
    }
    /* NULL / 超长 */
    chk(iv_ota_manifest_parse(NULL, 10u, &m, &why) == IV_EINVAL, "t02: NULL buf EINVAL");
    chk(iv_ota_manifest_parse(man, IV_OTA_MANIFEST_MAX + 1u, &m, &why) == IV_EINVAL,
        "t02: oversize manifest EINVAL");
}

/* ---------------------------------------------------------------------------
 * t03 正例（内存版 + 文件版）
 * ------------------------------------------------------------------------- */

static void t03_accept(void)
{
    uint8_t           pub[IV_OTA_PUBKEY_LEN];
    pkg_t             pk;
    iv_ota_manifest_t m;
    iv_ota_reason_t   why = IV_OTA_OK;
    char              tmpl[] = "/tmp/ivotapkg_XXXXXX";
    int               fd;

    chk(pubkey_from_seed(SEED_A, pub) == 1, "t03: derive public key A");
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);

    memset(&m, 0, sizeof(m));
    chk(iv_ota_verify_mem(pk.b, pk.len, &SELF, pub, &m, &why) == IV_OK, "t03: good pkg accepted");
    chk(why == IV_OTA_OK, "t03: reason ok");
    chk(m.n_files == 2u && strcmp(m.model, "IVSBOX-T113") == 0, "t03: manifest filled");
    chk(strcmp(m.files[0].path, "bin/ivsboxd") == 0, "t03: manifest file path filled");

    fd = mkstemp(tmpl);
    chk(fd >= 0, "t03: mkstemp");
    if (fd >= 0) {
        ssize_t w = write(fd, pk.b, pk.len);

        chk(w == (ssize_t)pk.len, "t03: write pkg file");
        (void)close(fd);

        memset(&m, 0, sizeof(m));
        why = IV_OTA_ESIG;
        chk(iv_ota_verify_file(tmpl, &SELF, pub, &m, &why) == IV_OK,
            "t03: file variant accepted (mmap path)");
        chk(why == IV_OTA_OK, "t03: file variant reason ok");
        chk(m.n_files == 2u, "t03: file variant manifest filled");
        chk(unlink(tmpl) == 0, "t03: cleanup pkg file");
    }
}

/* ---------------------------------------------------------------------------
 * t04 篡改
 * ------------------------------------------------------------------------- */

static void t04_tamper(void)
{
    uint8_t pub[IV_OTA_PUBKEY_LEN];
    pkg_t   pk;

    chk(pubkey_from_seed(SEED_A, pub) == 1, "t04: derive public key A");

    /* 负载单字节：签名覆盖全包，所以这必须由签名层抓住（不是完整性层） */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    pk.b[payload_off(&pk)] ^= 0x01u;
    expect_reject(&pk, &SELF, pub, IV_OTA_ESIG, "t04: payload byte flipped");

    /* 清单单字节：把 version 的 1 改成 2（清单仍合法，只是内容变了） */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    {
        char *v = memmem(pk.b + IV_OTA_HEADER_LEN, 64u, "version=1.0.1", 13u);

        chk(v != NULL, "t04: locate version in manifest");
        if (v != NULL) {
            v[8] = '2';
            expect_reject(&pk, &SELF, pub, IV_OTA_ESIG, "t04: manifest byte flipped");
        }
    }

    /* 签名单字节 */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    pk.b[pk.len - 1u] ^= 0x80u;
    expect_reject(&pk, &SELF, pub, IV_OTA_ESIG, "t04: signature byte flipped");

    /* magic 改动 → 结构层就拒 */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    pk.b[3] = 'X';
    expect_reject(&pk, &SELF, pub, IV_OTA_EBADFMT, "t04: magic changed");

    /* 头长度字段被改 → 总长不匹配 */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    pk.b[12] = (uint8_t)(pk.b[12] + 1u);
    expect_reject(&pk, &SELF, pub, IV_OTA_EBADFMT, "t04: payload_len edited");
}

/* ---------------------------------------------------------------------------
 * t05 型号 / t06 硬件版本 / t07 版本
 * ------------------------------------------------------------------------- */

static void t05_model(void)
{
    uint8_t pub[IV_OTA_PUBKEY_LEN];
    pkg_t   pk;

    chk(pubkey_from_seed(SEED_A, pub) == 1, "t05: derive public key A");
    mk_good_pkg(&pk, "IVSBOX-OTHER", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EMODEL, "t05: other model rejected");

    /* 型号大小写不同也算不匹配（不做大小写折叠） */
    mk_good_pkg(&pk, "ivsbox-t113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EMODEL, "t05: case-different model rejected");
}

static void t06_hwver(void)
{
    uint8_t pub[IV_OTA_PUBKEY_LEN];
    pkg_t   pk;

    chk(pubkey_from_seed(SEED_A, pub) == 1, "t06: derive public key A");
    mk_good_pkg(&pk, "IVSBOX-T113", 2u, 3u, "1.0.1", "1.0.0", 0u, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EHWVER, "t06: hw below range rejected");

    mk_good_pkg(&pk, "IVSBOX-T113", 0u, 0u, "1.0.1", "1.0.0", 0u, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EHWVER, "t06: hw above range rejected");
}

static void t07_version(void)
{
    uint8_t           pub[IV_OTA_PUBKEY_LEN];
    pkg_t             pk;
    iv_ota_manifest_t m;
    iv_ota_reason_t   why = IV_OTA_OK;

    chk(pubkey_from_seed(SEED_A, pub) == 1, "t07: derive public key A");

    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.0", "1.0.0", 0u, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EVER, "t07: same version rejected");

    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "0.9.9", "0.1.0", 0u, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EVER, "t07: downgrade rejected");

    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "2.0.0", 0u, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EVER, "t07: local below minrollback rejected");

    /* 边界：minrollback == 本机版本、硬件区间端点 == 本机版本，都应放行 */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 1u, "1.0.1", "1.0.0", 0u, SEED_A);
    chk(iv_ota_verify_mem(pk.b, pk.len, &SELF, pub, &m, &why) == IV_OK,
        "t07: boundary values accepted");
}

/* ---------------------------------------------------------------------------
 * t08 完整性（签名正确但清单与负载不符）
 * ------------------------------------------------------------------------- */

static void t08_integrity(void)
{
    uint8_t pub[IV_OTA_PUBKEY_LEN];
    pkg_t   pk;

    chk(pubkey_from_seed(SEED_A, pub) == 1, "t08: derive public key A");

    /* 摘要写错（签名覆盖的是错摘要，签名本身是"对的"）→ 必须由第 9 步抓住 */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", FAULT_HASH0, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EFILE, "t08: manifest digest mismatch rejected");

    /* 尺寸写错（合计 != payload_len）→ 由第 3 步抓住 */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", FAULT_SIZE0, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EFILE, "t08: size/length mismatch rejected");

    /* 负载尾部夹带 1 字节且重新签名：清单尺寸之和 < 头里的 payload_len，
     * 逐文件摘要与签名都"对"，只有第 3 步的长度核对能拦下它 */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    add_payload_pad_and_resign(&pk, SEED_A);
    expect_reject(&pk, &SELF, pub, IV_OTA_EFILE, "t08: payload longer than manifest sum rejected");
}

/* ---------------------------------------------------------------------------
 * t09 路径围栏（解析层 + 验签层都要拦）
 * ------------------------------------------------------------------------- */

static void t09_path(void)
{
    uint8_t pub[IV_OTA_PUBKEY_LEN];
    static const char *bad_paths[] = { "../etc/passwd", "/etc/passwd", "a/../../b", "etc/./x",
                                       "bin/", "bin//x", "./bin/x", "bin\\x", "bin\\:x" };
    size_t i;

    chk(pubkey_from_seed(SEED_A, pub) == 1, "t09: derive public key A");

    for (i = 0u; i < sizeof(bad_paths) / sizeof(bad_paths[0]); i++) {
        tfile_t           f[2];
        char              man[2048];
        iv_ota_manifest_t m;
        iv_ota_reason_t   why = IV_OTA_OK;
        size_t            mlen;

        files_default(f);
        f[0].path = bad_paths[i];
        mlen      = mk_manifest(man, sizeof(man), "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", f, 2u,
                                0u);
        if (iv_ota_manifest_parse(man, mlen, &m, &why) != IV_EPROTO || why != IV_OTA_EPATH) {
            fprintf(stderr, "FAIL: t09: parser accepted bad path '%s' (%s)\n", bad_paths[i],
                    iv_ota_reason_name(why));
            g_fail++;
        }
    }

    /* 验签层：真装配一个"路径穿越"的包并用正确密钥签名 —— 仍然必须被拒（顺序上
     * 路径检查在签名校验之前，所以理由固定是 EPATH，不是 ESIG） */
    {
        tfile_t  f[2];
        char     man[2048];
        uint8_t  payload[1024];
        pkg_t    pk;
        size_t   mlen;
        size_t   plen;

        files_default(f);
        f[0].path = "../bin/ivsboxd";
        mlen      = mk_manifest(man, sizeof(man), "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", f, 2u,
                                0u);
        plen = mk_payload(payload, f, 2u);
        (void)build_pkg(&pk, man, mlen, payload, plen, SEED_A);
        expect_reject(&pk, &SELF, pub, IV_OTA_EPATH, "t09: traversal package rejected");
    }
}

/* ---------------------------------------------------------------------------
 * t10 签名缺失 / 结构不完整
 * ------------------------------------------------------------------------- */

static void t10_missing_sig(void)
{
    uint8_t           pub[IV_OTA_PUBKEY_LEN];
    pkg_t             pk;
    iv_ota_manifest_t m;
    iv_ota_reason_t   why = IV_OTA_OK;

    chk(pubkey_from_seed(SEED_A, pub) == 1, "t10: derive public key A");

    /* 全零签名（装配时 seed=NULL） */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, NULL);
    expect_reject(&pk, &SELF, pub, IV_OTA_ESIG, "t10: zero signature rejected");

    /* 截断掉 64 字节签名段 */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    pk.len -= IV_OTA_SIG_LEN;
    expect_reject(&pk, &SELF, pub, IV_OTA_EBADFMT, "t10: truncated signature rejected");

    /* 尾部夹带 1 字节 */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);
    pk.b[pk.len] = 0x5au;
    pk.len += 1u;
    expect_reject(&pk, &SELF, pub, IV_OTA_EBADFMT, "t10: trailing byte rejected");

    /* 长度连头都不到 */
    chk(iv_ota_verify_mem(pk.b, 8u, &SELF, pub, &m, &why) == IV_EPROTO &&
            why == IV_OTA_EBADFMT,
        "t10: too short rejected");
}

/* ---------------------------------------------------------------------------
 * t11 密钥
 * ------------------------------------------------------------------------- */

static void t11_keys(void)
{
    uint8_t           pub_a[IV_OTA_PUBKEY_LEN];
    uint8_t           pub_b[IV_OTA_PUBKEY_LEN];
    uint8_t           zero[IV_OTA_PUBKEY_LEN];
    pkg_t             pk;
    iv_ota_manifest_t m;
    iv_ota_reason_t   why = IV_OTA_OK;

    chk(pubkey_from_seed(SEED_A, pub_a) == 1 && pubkey_from_seed(SEED_B, pub_b) == 1,
        "t11: derive keys");
    chk(memcmp(pub_a, pub_b, IV_OTA_PUBKEY_LEN) != 0, "t11: two distinct keys");

    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_A);

    /* 用 B 签的包拿 A 的公钥验 → 拒 */
    expect_reject(&pk, &SELF, pub_b, IV_OTA_ESIG, "t11: signed by other key rejected");

    /* 全零公钥 = 未配置 → ENOKEY（与"签名坏"区分开，现场能分辨"没部署密钥"） */
    memset(zero, 0, sizeof(zero));
    expect_reject(&pk, &SELF, zero, IV_OTA_ENOKEY, "t11: zero pubkey rejected");
    expect_reject(&pk, &SELF, NULL, IV_OTA_ENOKEY, "t11: NULL pubkey rejected");

    /* 用 B 重新签同一个包，拿 B 的公钥验 → 通过（证明拒的是密钥不匹配，不是包本身坏） */
    mk_good_pkg(&pk, "IVSBOX-T113", 1u, 2u, "1.0.1", "1.0.0", 0u, SEED_B);
    chk(iv_ota_verify_mem(pk.b, pk.len, &SELF, pub_b, &m, &why) == IV_OK,
        "t11: same pkg accepted with its own key");

    /* 参数非法：返回 EINVAL 且不改 why */
    why = IV_OTA_ESIG;
    chk(iv_ota_verify_mem(NULL, 10u, &SELF, pub_a, &m, &why) == IV_EINVAL && why == IV_OTA_ESIG,
        "t11: NULL buf -> EINVAL, why untouched");
    chk(iv_ota_verify_mem(pk.b, pk.len, NULL, pub_a, &m, &why) == IV_EINVAL,
        "t11: NULL self -> EINVAL");
    {
        iv_ota_self_t no_model = { NULL, 1u, { 1u, 0u, 0u } };

        chk(iv_ota_verify_mem(pk.b, pk.len, &no_model, pub_a, &m, &why) == IV_EINVAL,
            "t11: NULL self model -> EINVAL");
    }
}

/* ---------------------------------------------------------------------------
 * t12 文件版错误路径
 * ------------------------------------------------------------------------- */

static void t12_file(void)
{
    uint8_t           pub[IV_OTA_PUBKEY_LEN];
    iv_ota_manifest_t m;
    iv_ota_reason_t   why = IV_OTA_OK;
    char              tmpl[] = "/tmp/ivotaempty_XXXXXX";
    int               fd;

    chk(pubkey_from_seed(SEED_A, pub) == 1, "t12: derive public key A");

    chk(iv_ota_verify_file("/tmp/ivota_no_such_pkg_xyz", &SELF, pub, &m, &why) == IV_EIO &&
            why == IV_OTA_EIOFAIL,
        "t12: missing file -> EIOFAIL/EIO");
    chk(iv_ota_verify_file("/tmp", &SELF, pub, &m, &why) == IV_EIO && why == IV_OTA_EIOFAIL,
        "t12: directory -> EIOFAIL/EIO");
    chk(iv_ota_verify_file(NULL, &SELF, pub, &m, &why) == IV_EINVAL, "t12: NULL path EINVAL");

    fd = mkstemp(tmpl);
    chk(fd >= 0, "t12: mkstemp");
    if (fd >= 0) {
        (void)close(fd);
        chk(iv_ota_verify_file(tmpl, &SELF, pub, &m, &why) == IV_EIO && why == IV_OTA_EIOFAIL,
            "t12: empty file -> EIOFAIL/EIO");
        chk(unlink(tmpl) == 0, "t12: cleanup");
    }
}

/* ---------------------------------------------------------------------------
 * t13 内置公钥占位
 * ------------------------------------------------------------------------- */

static void t13_builtin(void)
{
    uint8_t pub[IV_OTA_PUBKEY_LEN];
    uint8_t acc = 0u;
    size_t  i;

    memset(pub, 0xaa, sizeof(pub));
    chk(iv_ota_builtin_pubkey(pub) == IV_ENOTSUP, "t13: builtin pubkey not configured");
    for (i = 0u; i < IV_OTA_PUBKEY_LEN; i++)
        acc |= pub[i];
    chk(acc == 0u, "t13: builtin pubkey zeroed (no fake key)");
    chk(iv_ota_builtin_pubkey(NULL) == IV_EINVAL, "t13: NULL out EINVAL");
    chk(strcmp(iv_ota_reason_name(IV_OTA_ESIG), "bad-signature") == 0, "t13: reason name");
    chk(strcmp(iv_ota_reason_name((iv_ota_reason_t)999), "unknown") == 0, "t13: unknown reason");
}

int main(void)
{
    t01_version();
    t02_manifest();
    t03_accept();
    t04_tamper();
    t05_model();
    t06_hwver();
    t07_version();
    t08_integrity();
    t09_path();
    t10_missing_sig();
    t11_keys();
    t12_file();
    t13_builtin();

    if (g_fail != 0) {
        fprintf(stderr, "test_ota FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_ota passed (ver, manifest parse, accept, tamper, model, hw, version, "
           "integrity, path fence, missing sig, keys, file io, builtin pubkey)\n");
    return 0;
}
