/*
 * SNMP v2c 最小客户端单测（功能开发计划 M4-S4.3）
 *
 * 覆盖：OID BER 编解码（含 39165 多字节 arc 黄金字节）、请求报文布局、
 *       应答解析（INTEGER/OCTET STRING/Counter32/Gauge32/TimeTicks/异常哨兵）、
 *       request-id 失配丢弃、超时与重试次数、GET/GETNEXT、表列 walk 的子树边界
 *       与行数上限、报文过长时 BER 长式长度域。
 * 零网络：全部 socket 走注入点。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_snmp.h"

static int g_fail;
static void chk(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 注入点
 * ------------------------------------------------------------------------- */
static int g_sock_fd = 200;
static int g_n_send;
static uint8_t g_queue[16][IV_SNMP_PKT_MAX];
static size_t g_queue_len[16];
static int g_queue_n, g_queue_i;

static int io_socket(int d, int t, int p) {
    (void)d;
    (void)t;
    (void)p;
    return g_sock_fd++;
}
static int io_setsockopt(int fd, int lv, int on, const void *v, socklen_t l) {
    (void)fd;
    (void)lv;
    (void)on;
    (void)v;
    (void)l;
    return 0;
}
static ssize_t io_sendto(int fd, const void *b, size_t n, int f,
                         const struct sockaddr *a, socklen_t al) {
    (void)fd;
    (void)b;
    (void)f;
    (void)a;
    (void)al;
    g_n_send++;
    return (ssize_t)n;
}
static ssize_t io_recvfrom(int fd, void *b, size_t n, int f, struct sockaddr *a,
                           socklen_t *al) {
    (void)fd;
    (void)f;
    (void)a;
    (void)al;
    if (g_queue_i >= g_queue_n) {
        errno = EAGAIN;
        return -1;
    }
    if (g_queue_len[g_queue_i] > n) {
        errno = EAGAIN;
        return -1;
    }
    memcpy(b, g_queue[g_queue_i], g_queue_len[g_queue_i]);
    return (ssize_t)g_queue_len[g_queue_i++];
}
static int io_close(int fd) {
    (void)fd;
    return 0;
}
static uint64_t io_now(void) { return 0; }

static const iv_snmp_iops_t g_io = {
    .socket = io_socket,
    .setsockopt = io_setsockopt,
    .sendto = io_sendto,
    .recvfrom = io_recvfrom,
    .close = io_close,
    .now_ms = io_now,
};

static void io_reset(void) {
    g_n_send = 0;
    g_queue_n = 0;
    g_queue_i = 0;
}
static void io_push(const uint8_t *p, size_t n) {
    if (g_queue_n < 16) {
        memcpy(g_queue[g_queue_n], p, n);
        g_queue_len[g_queue_n] = n;
        g_queue_n++;
    }
}

static iv_snmp_target_t mk_target(void) {
    iv_snmp_target_t t;
    iv_snmp_target_default(&t);
    t.iops = &g_io;
    strcpy(t.host, "192.168.1.200");
    strcpy(t.community, "public");
    t.timeout_ms = 50;
    t.retries = 0;
    return t;
}

/* ---------------------------------------------------------------------------
 * 造应答报文的小工具（TLV 包装）
 * ------------------------------------------------------------------------- */
static size_t wrap(uint8_t *out, uint8_t tag, const uint8_t *in, size_t n) {
    size_t hl;
    uint8_t h[4];
    if (n < 0x80u) {
        h[0] = (uint8_t)n;
        hl = 1;
    } else if (n <= 0xFFu) {
        h[0] = 0x81;
        h[1] = (uint8_t)n;
        hl = 2;
    } else {
        h[0] = 0x82;
        h[1] = (uint8_t)(n >> 8);
        h[2] = (uint8_t)n;
        hl = 3;
    }
    out[0] = tag;
    memcpy(out + 1, h, hl);
    if (n > 0) memcpy(out + 1 + hl, in, n);
    return 1 + hl + n;
}

/* 造一条完整 GetResponse（单 varbind）。返回总长度；失败返回 0。 */
static size_t mk_response(uint8_t *out, size_t cap, uint32_t reqid,
                          const char *oid, uint8_t vtag, const uint8_t *vval,
                          size_t vlen) {
    uint8_t oidb[64], oidtlv[96], vtlv[400];
    uint8_t vbcontent[512], varbind[560], vblist[600];
    uint8_t pducontent[700], pdutlv[760], msgcontent[820];
    size_t ol, vl, vc, vbn, vbln, pc, pn, mcn, cl, total;
    uint8_t rid[4];
    const char *comm = "public";
    uint8_t z[3] = {0x02, 0x01, 0x00};
    int oidlen = iv_snmp_oid_encode(oid, oidb, sizeof(oidb));
    if (oidlen < 0) return 0;
    ol = wrap(oidtlv, 0x06, oidb, (size_t)oidlen);
    vl = wrap(vtlv, vtag, vval, vlen);

    vc = 0;
    memcpy(vbcontent + vc, oidtlv, ol);
    vc += ol;
    memcpy(vbcontent + vc, vtlv, vl);
    vc += vl;
    vbn = wrap(varbind, 0x30, vbcontent, vc);
    vbln = wrap(vblist, 0x30, varbind, vbn);

    rid[0] = (uint8_t)(reqid >> 24);
    rid[1] = (uint8_t)(reqid >> 16);
    rid[2] = (uint8_t)(reqid >> 8);
    rid[3] = (uint8_t)reqid;
    pc = wrap(pducontent, 0x02, rid, 4);
    memcpy(pducontent + pc, z, 3);
    pc += 3;
    memcpy(pducontent + pc, z, 3);
    pc += 3;
    memcpy(pducontent + pc, vblist, vbln);
    pc += vbln;
    pn = wrap(pdutlv, IV_SNMP_PDU_GETRESP, pducontent, pc);

    {
        uint8_t ver[3] = {0x02, 0x01, 0x01};
        memcpy(msgcontent, ver, 3);
        mcn = 3;
        cl = wrap(msgcontent + mcn, 0x04, (const uint8_t *)comm, strlen(comm));
        mcn += cl;
        memcpy(msgcontent + mcn, pdutlv, pn);
        mcn += pn;
    }
    total = wrap(out, 0x30, msgcontent, mcn);
    if (total > cap) return 0;
    return total;
}

/* ---------------------------------------------------------------------------
 * s01：OID 编解码
 * ------------------------------------------------------------------------- */
static void s01_oid(void) {
    uint8_t b[64];
    char s[IV_SNMP_OID_STR_MAX];
    int n = iv_snmp_oid_encode(".1.3.6.1.4.1.39165.1.7.0", b, sizeof(b));
    chk(n == 11, "s01 oid encoded len 11");
    /* 黄金字节：首字节 40*1+3=0x2B；39165 = 0x82 0xB1 0x7D */
    {
        const uint8_t want[11] = {0x2B, 0x06, 0x01, 0x04, 0x01,
                                  0x82, 0xB1, 0x7D, 0x01, 0x07, 0x00};
        chk(n == 11 && memcmp(b, want, 11) == 0, "s01 golden bytes");
    }
    chk(iv_snmp_oid_decode(b, (size_t)n, s, sizeof(s)) > 0, "s01 decode ok");
    chk(strcmp(s, ".1.3.6.1.4.1.39165.1.7.0") == 0, "s01 roundtrip");
    /* 不带前导点也接受 */
    n = iv_snmp_oid_encode("1.3.6.1.2.1.1.1.0", b, sizeof(b));
    chk(n == 8, "s01 no-leading-dot len 8");
    /* 非法：只有一级 / 首级 >2 */
    chk(iv_snmp_oid_encode(".1", b, sizeof(b)) < 0, "s01 reject single arc");
    chk(iv_snmp_oid_encode(".3.1", b, sizeof(b)) < 0, "s01 reject first>2");
    chk(iv_snmp_oid_encode("..1.3.x", b, sizeof(b)) < 0, "s01 reject non-numeric");
}

/* ---------------------------------------------------------------------------
 * s02：请求报文布局
 * ------------------------------------------------------------------------- */
static void s02_request(void) {
    uint8_t pkt[IV_SNMP_PKT_MAX];
    int n = iv_snmp_build_request(IV_SNMP_PDU_GET, "public", 0x01020304u,
                                  ".1.3.6.1.2.1.1.1.0", pkt, sizeof(pkt));
    chk(n > 0, "s02 built");
    chk(pkt[0] == 0x30, "s02 outer SEQUENCE");
    /* version INTEGER 1 */
    chk(pkt[2] == 0x02 && pkt[3] == 0x01 && pkt[4] == 0x01, "s02 version=1");
    /* community */
    chk(pkt[5] == 0x04 && pkt[6] == 6 && memcmp(&pkt[7], "public", 6) == 0,
        "s02 community");
    /* PDU tag 0xA0 + reqid 4 字节 01 02 03 04 */
    chk(pkt[13] == IV_SNMP_PDU_GET, "s02 GetRequest tag");
    /* PDU 起于 13（tag）、14（len），reqid INTEGER 起于 15 */
    chk(pkt[15] == 0x02 && pkt[16] == 0x04, "s02 reqid INTEGER len4");
    chk(pkt[17] == 0x01 && pkt[18] == 0x02 && pkt[19] == 0x03 && pkt[20] == 0x04,
        "s02 reqid bytes");
    /* error-status/index = 0 */
    chk(pkt[21] == 0x02 && pkt[22] == 0x01 && pkt[23] == 0x00, "s02 err-status 0");
    /* 以 NULL(0x05 0x00) 结尾 */
    chk(pkt[n - 2] == 0x05 && pkt[n - 1] == 0x00, "s02 ends with NULL");
    /* GetNext tag */
    n = iv_snmp_build_request(IV_SNMP_PDU_GETNEXT, "public", 1u, ".1.3.6", pkt,
                              sizeof(pkt));
    chk(n > 0 && pkt[13] == IV_SNMP_PDU_GETNEXT, "s02 GetNext tag");
    /* 缓冲太小 */
    chk(iv_snmp_build_request(IV_SNMP_PDU_GET, "public", 1u, ".1.3.6", pkt, 5) <
            0,
        "s02 small cap error");
}

/* ---------------------------------------------------------------------------
 * s03：应答解析各种类型
 * ------------------------------------------------------------------------- */
static void s03_parse_types(void) {
    uint8_t pkt[640];
    iv_snmp_value_t v;
    int es = 0;
    size_t n;
    const char *oid = ".1.3.6.1.2.1.1.1.0";

    /* INTEGER 42 */
    {
        uint8_t val[1] = {42};
        n = mk_response(pkt, sizeof(pkt), 7, oid, IV_SNMP_T_INTEGER, val, 1);
        chk(n > 0, "s03 int built");
        chk(iv_snmp_parse_response(pkt, n, 7, &v, &es) == IV_OK, "s03 int parse");
        chk(v.type == IV_SNMP_T_INTEGER && v.num == 42, "s03 int value 42");
        chk(strcmp(v.oid, oid) == 0, "s03 oid echoed");
    }
    /* 负 INTEGER -1 */
    {
        uint8_t val[1] = {0xFF};
        n = mk_response(pkt, sizeof(pkt), 7, oid, IV_SNMP_T_INTEGER, val, 1);
        chk(iv_snmp_parse_response(pkt, n, 7, &v, &es) == IV_OK, "s03 neg parse");
        chk(v.num == -1, "s03 neg value -1");
    }
    /* OCTET STRING */
    {
        const char *s = "DS-2CD1234";
        n = mk_response(pkt, sizeof(pkt), 7, oid, IV_SNMP_T_OCTETSTR,
                        (const uint8_t *)s, strlen(s));
        chk(iv_snmp_parse_response(pkt, n, 7, &v, &es) == IV_OK, "s03 str parse");
        chk(strcmp(v.str, "DS-2CD1234") == 0, "s03 str value");
    }
    /* Counter32 */
    {
        uint8_t val[4] = {0x00, 0x01, 0x00, 0x00}; /* 65536 */
        n = mk_response(pkt, sizeof(pkt), 7, oid, IV_SNMP_T_COUNTER32, val, 4);
        chk(iv_snmp_parse_response(pkt, n, 7, &v, &es) == IV_OK, "s03 c32 parse");
        chk(v.type == IV_SNMP_T_COUNTER32 && v.num == 65536, "s03 c32 value");
    }
    /* Gauge32 */
    {
        uint8_t val[4] = {0x00, 0x00, 0x03, 0xE8}; /* 1000 */
        n = mk_response(pkt, sizeof(pkt), 7, oid, IV_SNMP_T_GAUGE32, val, 4);
        chk(iv_snmp_parse_response(pkt, n, 7, &v, &es) == IV_OK, "s03 g32 parse");
        chk(v.num == 1000 && iv_snmp_value_valid(v.type), "s03 g32 value");
    }
    /* TimeTicks */
    {
        uint8_t val[4] = {0x00, 0x00, 0x27, 0x10}; /* 10000 */
        n = mk_response(pkt, sizeof(pkt), 7, oid, IV_SNMP_T_TIMETICKS, val, 4);
        chk(iv_snmp_parse_response(pkt, n, 7, &v, &es) == IV_OK, "s03 ticks parse");
        chk(v.num == 10000, "s03 ticks value");
    }
    /* noSuchObject（异常哨兵 → 无值） */
    {
        n = mk_response(pkt, sizeof(pkt), 7, oid, IV_SNMP_T_NOSUCHOBJECT, NULL, 0);
        chk(iv_snmp_parse_response(pkt, n, 7, &v, &es) == IV_OK, "s03 nosuch parse");
        chk(!iv_snmp_value_valid(v.type), "s03 nosuch invalid");
    }
    /* endOfMibView */
    {
        n = mk_response(pkt, sizeof(pkt), 7, oid, IV_SNMP_T_ENDOFMIBVIEW, NULL, 0);
        chk(iv_snmp_parse_response(pkt, n, 7, &v, &es) == IV_OK, "s03 eom parse");
        chk(!iv_snmp_value_valid(v.type), "s03 eom invalid");
    }
    /* reqid 失配 */
    {
        uint8_t val[1] = {1};
        n = mk_response(pkt, sizeof(pkt), 99, oid, IV_SNMP_T_INTEGER, val, 1);
        chk(iv_snmp_parse_response(pkt, n, 7, &v, &es) == IV_EPROTO,
            "s03 reqid mismatch -> EPROTO");
    }
}

/* ---------------------------------------------------------------------------
 * s04：长报文（>127 字节）的 BER 长式长度域
 * ------------------------------------------------------------------------- */
static void s04_long_form(void) {
    uint8_t pkt[IV_SNMP_PKT_MAX];
    iv_snmp_value_t v;
    int es = 0;
    /* community 很长 → 整包 > 127 字节，逼迫外层用长式长度。
     * 注意：本用例只覆盖**读**方向（自造报文 → parse_response）；
     *       写方向的长度域由 s10 覆盖。 */
    uint8_t sval[200];
    size_t n;
    memset(sval, 'A', sizeof(sval));
    n = mk_response(pkt, sizeof(pkt), 3, ".1.3.6.1.2.1.1.1.0", IV_SNMP_T_OCTETSTR,
                    sval, sizeof(sval));
    chk(n > 127, "s04 packet > 127 bytes");
    chk(pkt[0] == 0x30 && pkt[1] == 0x81, "s04 outer uses long-form length");
    chk(iv_snmp_parse_response(pkt, n, 3, &v, &es) == IV_OK, "s04 long parse ok");
    chk(v.type == IV_SNMP_T_OCTETSTR && strlen(v.str) > 0, "s04 value read");
}

/* ---------------------------------------------------------------------------
 * s05：转述层——reqid 失配包被丢弃，后续匹配包被采纳
 * ------------------------------------------------------------------------- */
static void s05_transact_discard(void) {
    iv_snmp_target_t t = mk_target();
    uint8_t pkt[640], resp[IV_SNMP_PKT_MAX];
    uint8_t val[1] = {7};
    size_t n1, n2;
    int rc;
    io_reset();
    n1 = mk_response(pkt, sizeof(pkt), 99, ".1.3.6.1.2.1.1.1.0",
                     IV_SNMP_T_INTEGER, val, 1);
    io_push(pkt, n1);
    n2 = mk_response(pkt, sizeof(pkt), 2 /* get 用 seed+1 */, ".1.3.6.1.2.1.1.1.0",
                     IV_SNMP_T_INTEGER, val, 1);
    io_push(pkt, n2);
    rc = iv_snmp_transact(&t, IV_SNMP_PDU_GET, 2u, ".1.3.6.1.2.1.1.1.0", resp,
                          sizeof(resp));
    chk(rc > 0, "s05 transact ok past mismatch");
    chk((size_t)rc == n2, "s05 returned the matching packet");
    chk(g_n_send == 1, "s05 sent once");
}

/* ---------------------------------------------------------------------------
 * s06：超时与重试次数
 * ------------------------------------------------------------------------- */
static void s06_timeout_retries(void) {
    iv_snmp_target_t t = mk_target();
    uint8_t resp[IV_SNMP_PKT_MAX];
    int rc;
    io_reset(); /* 队列空 → 每次都超时 */
    t.retries = 2;
    rc = iv_snmp_transact(&t, IV_SNMP_PDU_GET, 2u, ".1.3.6.1.2.1.1.1.0", resp,
                          sizeof(resp));
    chk(rc == IV_ETIMEDOUT, "s06 timeout code");
    chk(g_n_send == 3, "s06 attempts == retries+1");
}

/* ---------------------------------------------------------------------------
 * s07：GET 取值（含 noSuchObject → IV_ENOENT）
 * ------------------------------------------------------------------------- */
static void s07_get(void) {
    iv_snmp_target_t t = mk_target();
    iv_snmp_value_t v;
    uint8_t pkt[640];
    uint8_t val[1] = {25};
    size_t n;
    io_reset();
    n = mk_response(pkt, sizeof(pkt), 2, ".1.3.6.1.4.1.39165.1.7.0",
                    IV_SNMP_T_INTEGER, val, 1);
    io_push(pkt, n);
    chk(iv_snmp_get(&t, ".1.3.6.1.4.1.39165.1.7.0", &v) == IV_OK, "s07 get ok");
    chk(v.num == 25, "s07 get value");

    io_reset();
    n = mk_response(pkt, sizeof(pkt), 2, ".1.3.6.1.4.1.39165.1.7.0",
                    IV_SNMP_T_NOSUCHOBJECT, NULL, 0);
    io_push(pkt, n);
    chk(iv_snmp_get(&t, ".1.3.6.1.4.1.39165.1.7.0", &v) == IV_ENOENT,
        "s07 nosuch -> ENOENT");
}

/* ---------------------------------------------------------------------------
 * s08：GETNEXT + walk 的子树边界与行数
 * ------------------------------------------------------------------------- */
static void s08_walk(void) {
    iv_snmp_target_t t = mk_target();
    iv_snmp_value_t rows[8];
    uint8_t pkt[640];
    int nrows = 0;
    const char *col = ".1.3.6.1.2.1.2.2.1.8";
    io_reset();
    /* 三行都在列内，第四步走出（.10 相邻列） */
    {
        uint8_t v1[1] = {1};
        uint8_t v2[1] = {2};
        uint8_t v3[1] = {1};
        uint8_t v4[1] = {5};
        size_t n;
        n = mk_response(pkt, sizeof(pkt), 3, ".1.3.6.1.2.1.2.2.1.8.1",
                        IV_SNMP_T_INTEGER, v1, 1);
        io_push(pkt, n);
        n = mk_response(pkt, sizeof(pkt), 3, ".1.3.6.1.2.1.2.2.1.8.2",
                        IV_SNMP_T_INTEGER, v2, 1);
        io_push(pkt, n);
        n = mk_response(pkt, sizeof(pkt), 3, ".1.3.6.1.2.1.2.2.1.8.3",
                        IV_SNMP_T_INTEGER, v3, 1);
        io_push(pkt, n);
        n = mk_response(pkt, sizeof(pkt), 3, ".1.3.6.1.2.1.2.2.1.10.1",
                        IV_SNMP_T_COUNTER32, v4, 1);
        io_push(pkt, n);
    }
    chk(iv_snmp_walk(&t, col, rows, 8, &nrows) == IV_OK, "s08 walk ok");
    chk(nrows == 3, "s08 stops at subtree boundary (3 rows)");
    chk(strcmp(rows[0].oid, ".1.3.6.1.2.1.2.2.1.8.1") == 0, "s08 row0 oid");
    chk(rows[2].num == 1, "s08 row2 value");

    /* 行数上限：只取 2 行 */
    io_reset();
    {
        uint8_t v1[1] = {1};
        size_t n;
        n = mk_response(pkt, sizeof(pkt), 3, ".1.3.6.1.2.1.2.2.1.8.1",
                        IV_SNMP_T_INTEGER, v1, 1);
        io_push(pkt, n);
        n = mk_response(pkt, sizeof(pkt), 3, ".1.3.6.1.2.1.2.2.1.8.2",
                        IV_SNMP_T_INTEGER, v1, 1);
        io_push(pkt, n);
        n = mk_response(pkt, sizeof(pkt), 3, ".1.3.6.1.2.1.2.2.1.8.3",
                        IV_SNMP_T_INTEGER, v1, 1);
        io_push(pkt, n);
    }
    t.max_rows = 2;
    chk(iv_snmp_walk(&t, col, rows, 8, &nrows) == IV_OK, "s08 capped walk ok");
    chk(nrows == 2, "s08 max_rows cap respected");

    /* 无进展保护：同一 OID 反复返回应终止 */
    io_reset();
    {
        uint8_t v1[1] = {1};
        size_t n;
        int k;
        for (k = 0; k < 5; k++) {
            n = mk_response(pkt, sizeof(pkt), 3, col, IV_SNMP_T_INTEGER, v1, 1);
            io_push(pkt, n);
        }
    }
    t.max_rows = 8;
    chk(iv_snmp_walk(&t, col, rows, 8, &nrows) == IV_OK, "s08 noprogress walk ok");
    /* 第一行 OID == 列起点，子树判定为真；第二行同 OID → 无进展终止 */
    chk(nrows == 1, "s08 no-progress guard stops at 1");

    /* 行数上限（**函数参数**侧）：上面那组测的是 t.max_rows 侧，两者是 `min`；
     * 这里把目标侧放宽到 64，让**函数参数**成为唯一约束（否则这条分支无人覆盖
     * —— 反向证伪实测：删掉 min() 那一行，本用例原本照样全绿）。 */
    io_reset();
    {
        uint8_t v1[1] = {1};
        size_t n;
        int k;

        for (k = 0; k < 5; k++) {
            char o[64];

            (void)snprintf(o, sizeof(o), "%s.%d", col, k + 1);
            n = mk_response(pkt, sizeof(pkt), 3, o, IV_SNMP_T_INTEGER, v1, 1);
            io_push(pkt, n);
        }
    }
    t.max_rows = 64;
    chk(iv_snmp_walk(&t, col, rows, 2, &nrows) == IV_OK, "s08 arg-capped walk ok");
    chk(nrows == 2, "s08 the max_rows ARGUMENT also caps the walk");
}

/* ---------------------------------------------------------------------------
 * s09：子树边界判定
 * ------------------------------------------------------------------------- */
static void s09_subtree(void) {
    chk(iv_snmp_oid_in_subtree(".1.3.6", ".1.3.6.1") == 1, "s09 child");
    chk(iv_snmp_oid_in_subtree(".1.3.6", ".1.3.6") == 1, "s09 self");
    chk(iv_snmp_oid_in_subtree(".1.3.6", ".1.3.60") == 0, "s09 not a prefix");
    chk(iv_snmp_oid_in_subtree(".1.3.6", ".1.3.5.1") == 0, "s09 sibling");
    chk(iv_snmp_oid_in_subtree(".1.3.6.1.2.1.2.2.1.8", ".1.3.6.1.2.1.2.2.1.10.0") ==
            0,
        "s09 adjacent column");
}

/* ---------------------------------------------------------------------------
 * s10：**写出方向**的 BER 长式长度域（`put_len` / `wr_close` 的长度回填与内容右移）
 *
 * s04 用的是测试自造报文（mk_response）+ parse_response ⇒ 只覆盖**读**方向。
 * 写方向由 wr_close 负责：先占 1 字节长度，内容写完后若超 127 再把内容整体
 * 右移、补上长式长度字节。这段**s04 覆盖不到**——反向证伪实测：把 put_len 改成
 * "一律写单字节"，s04 照样全绿（该漏洞由本用例补上）；更进一步，本用例第一次
 * 运行就抓出 wr_close 的右移被写成了**正向拷贝**（内容被首字节刷满）。
 * 两条黄金：community 150 字符 ⇒ 外层 0x81 一级长式；255 字符（`build_request`
 * 的上限）⇒ 外层 0x82 两级长式且内容右移 2 字节。
 * 反向证伪：put_len 只写单字节 / wr_close 正向拷贝 ⇒ 本用例立刻失败。
 * ------------------------------------------------------------------------- */
static void s10_write_long_form(void) {
    uint8_t  req[IV_SNMP_PKT_MAX];
    char     comm[256];
    unsigned commlen[2] = {150u, 255u};
    uint8_t  tag[2]     = {IV_SNMP_PDU_GET, IV_SNMP_PDU_GETNEXT};
    const char *oid[2]  = {".1.3.6.1.2.1.1.1.0", ".1.3.6.1.2.1.1.5.0"};
    int      i;

    for (i = 0; i < 2; i++) {
        size_t   h, cl, cop, clen;
        int      n;
        char     fill = (i == 0) ? 'c' : 'd';

        memset(comm, fill, commlen[i]);
        comm[commlen[i]] = '\0';
        n = iv_snmp_build_request(tag[i], comm, 7u, oid[i], req, sizeof(req));
        chk(n > 127, "s10 request is longer than 127 bytes");
        if (n <= 0)
            continue;

        /* 外层长度域：短式放不下 ⇒ 必须长式，且长度值要和实际包长对上 */
        chk(req[0] == 0x30, "s10 outer SEQUENCE tag");
        if (i == 0) {
            chk(req[1] == 0x81, "s10 150-char community forces 0x81 outer length");
            chk((int)req[2] == n - 3, "s10 1-byte long-form length matches body");
            h = 3u;
        } else {
            chk(req[1] == 0x82, "s10 300-char community forces 0x82 outer length");
            chk((((int)req[2] << 8) | (int)req[3]) == n - 4,
                "s10 2-byte long-form length matches body");
            h = 4u;
        }

        /* 长度占位被回填成正确值 ⇒ 其后紧跟 version INTEGER(1)（内容确实被右移了） */
        chk(req[h] == 0x02 && req[h + 1] == 0x01 && req[h + 2] == 0x01,
            "s10 version INTEGER 1 follows the outer length field");
        chk(req[h + 3] == 0x04, "s10 community is an OCTET STRING");
        cl = (req[h + 4] == 0x81u) ? 2u : 3u; /* 长度域自身字节数（1 或 2） */
        clen = (cl == 2u) ? (size_t)req[h + 5]
                          : (((size_t)req[h + 5] << 8) | (size_t)req[h + 6]);
        chk(clen == (size_t)commlen[i], "s10 community length value is exact");
        chk(req[h + 4] == ((commlen[i] > 0xFFu) ? 0x82u : 0x81u),
            "s10 community uses the right long-form width");
        cop = h + 4u + cl; /* community 内容起点 */
        chk(cop + commlen[i] < (size_t)n, "s10 community fits inside the packet");
        chk(memcmp(req + cop, comm, commlen[i]) == 0,
            "s10 community bytes survive the long-form shift");
        chk(req[cop + commlen[i]] == tag[i], "s10 PDU tag follows the community");
    }
}

int main(void) {
    s01_oid();
    s02_request();
    s03_parse_types();
    s04_long_form();
    s05_transact_discard();
    s06_timeout_retries();
    s07_get();
    s08_walk();
    s09_subtree();
    s10_write_long_form();
    if (g_fail != 0) {
        fprintf(stderr, "test_snmp FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_snmp passed (oid ber, request layout, response types, long-form "
           "len read/write, transact discard/timeout-retry, get, walk "
           "subtree/limits, subtree)\n");
    return 0;
}
