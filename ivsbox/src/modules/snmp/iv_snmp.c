/*
 * SNMP v2c 最小客户端实现（libivmodules，功能开发计划 M4-S4.3）
 *
 * 结构：① 纯 BER 编解码；② 请求构造/应答解析；③ 阻塞事务 + 重试；④ GET/GETNEXT/walk。
 * 不引任何第三方库；全部系统调用经注入点，单测零网络。
 */
#include "ivsbox/iv_snmp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "ivsbox/iv_clock.h"

/* ---------------------------------------------------------------------------
 * 默认注入点
 * ------------------------------------------------------------------------- */
static const iv_snmp_iops_t g_default_iops = {
    .socket = socket,
    .setsockopt = setsockopt,
    .sendto = sendto,
    .recvfrom = recvfrom,
    .close = close,
    .now_ms = iv_clock_monotonic_ms,
};

const iv_snmp_iops_t *iv_snmp_default_iops(void) { return &g_default_iops; }

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */
/* 经 io 注入的 now_ms */
static uint64_t io_now(const iv_snmp_target_t *t) {
    return t->iops->now_ms ? t->iops->now_ms() : 0u;
}

/* BER 长度域写入（短式 / 长式）。返回写入字节数。 */
static size_t put_len(uint8_t *p, size_t len) {
    if (len < 0x80u) {
        p[0] = (uint8_t)len;
        return 1;
    }
    if (len <= 0xFFu) {
        p[0] = 0x81;
        p[1] = (uint8_t)len;
        return 2;
    }
    p[0] = 0x82;
    p[1] = (uint8_t)(len >> 8);
    p[2] = (uint8_t)(len & 0xFF);
    return 3;
}

/* 游标式写入器 */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   n;
    int      err;
} wr_t;

static void wr_bytes(wr_t *w, const void *p, size_t n) {
    if (w->err || n == 0) return;
    if (w->n + n > w->cap) {
        w->err = IV_ERANGE;
        return;
    }
    memcpy(w->buf + w->n, p, n);
    w->n += n;
}
static void wr_byte(wr_t *w, uint8_t b) { wr_bytes(w, &b, 1); }
static void wr_tlv(wr_t *w, uint8_t tag, const void *p, size_t n) {
    uint8_t lb[3];
    size_t ln;
    if (w->err) return;
    ln = put_len(lb, n);
    if (w->n + 1 + ln + n > w->cap) {
        w->err = IV_ERANGE;
        return;
    }
    wr_byte(w, tag);
    wr_bytes(w, lb, ln);
    wr_bytes(w, p, n);
}
/* 预留一个 TLV 头：记下起点，返回内容起点（后续回填长度）。 */
static size_t wr_open(wr_t *w, uint8_t tag) {
    size_t start;
    if (w->err) return w->n;
    start = w->n;
    wr_byte(w, tag);
    wr_byte(w, 0); /* 占位，稍后回填 */
    return start;
}
/* 回填长度（short/long 自适应；仅在内容长度 < 0x80 时用占位足够）。
 * 为保证 >127 也正确，这里采用"整体平移"：先算出真实长度，重新排布。
 *
 * ⚠ **必须从后往前搬**：目标地址 `start+1+ln+i` 恒在源地址 `start+2+i` 的**右边**
 *   （相差 ln-1 ≥ 1），正向搬会把刚写入的首字节沿路复制到整段内容上
 *   —— 表现为"包长度对、内容全同一个字节"。此坑由 S4.3 反向证伪实测抓到
 *   （当时的用例只覆盖了**读**方向的长式长度，写方向无人断言）。 */
static void wr_close(wr_t *w, size_t start) {
    size_t content;
    uint8_t lb[3];
    size_t ln, i;
    if (w->err) return;
    /* start 指向 tag；start+1 是占位长度字节 */
    content = w->n - (start + 2);
    ln = put_len(lb, content);
    if (ln == 1) {
        w->buf[start + 1] = lb[0];
        return;
    }
    /* 需要更长长度域：把内容整体右移 (ln-1) 字节（**倒序**，见上） */
    if (w->n + (ln - 1) > w->cap) {
        w->err = IV_ERANGE;
        return;
    }
    for (i = content; i > 0u; i--)
        w->buf[start + 1 + ln + i - 1u] = w->buf[start + 2 + i - 1u];
    for (i = 0; i < ln; i++)
        w->buf[start + 1 + i] = lb[i];
    w->n += (ln - 1);
}

/* TLV 读取器 */
typedef struct {
    const uint8_t *p;
    size_t         n;
} rd_t;

static int rd_tlv(rd_t *r, uint8_t *tag, const uint8_t **val, size_t *vlen) {
    size_t l = 0, off;
    if (r->n < 2) return IV_EPROTO;
    if (r->p[1] < 0x80u) {
        l = r->p[1];
        off = 2;
    } else if (r->p[1] == 0x81u) {
        if (r->n < 3) return IV_EPROTO;
        l = r->p[2];
        off = 3;
    } else if (r->p[1] == 0x82u) {
        if (r->n < 4) return IV_EPROTO;
        l = ((size_t)r->p[2] << 8) | r->p[3];
        off = 4;
    } else {
        return IV_EPROTO;
    }
    if (r->n < off + l) return IV_EPROTO;
    *tag = r->p[0];
    *val = r->p + off;
    *vlen = l;
    r->p += off + l;
    r->n -= off + l;
    return IV_OK;
}

/* 读一个指定 tag 的构造体并进入其中 */
static int rd_enter(rd_t *r, uint8_t want) {
    uint8_t t;
    const uint8_t *v;
    size_t l;
    int rc = rd_tlv(r, &t, &v, &l);
    if (rc != IV_OK) return rc;
    if (t != want) return IV_EPROTO;
    r->p = v;
    r->n = l;
    return IV_OK;
}

/* 读一个整数 TLV（最多 8 字节，带符号/无符号）→ int64。 */
static int rd_int(rd_t *r, uint8_t want_tag, int64_t *out) {
    uint8_t t;
    const uint8_t *v;
    size_t l, i;
    int rc = rd_tlv(r, &t, &v, &l);
    int64_t acc = 0;
    if (rc != IV_OK) return rc;
    if (t != want_tag) return IV_EPROTO;
    if (l == 0 || l > 8) return IV_EPROTO;
    for (i = 0; i < l; i++) acc = (acc << 8) | v[i];
    *out = acc;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * OID 编解码
 * ------------------------------------------------------------------------- */
int iv_snmp_oid_encode(const char *dotted, uint8_t *out, size_t cap) {
    uint32_t arc[32];
    int n = 0;
    const char *p;
    size_t idx = 0;
    int i;
    if (dotted == NULL || out == NULL) return IV_EINVAL;
    p = dotted;
    while (*p != '\0') {
        uint32_t v = 0;
        int digits = 0;
        while (*p == '.') p++;
        if (*p == '\0') break;
        while (*p >= '0' && *p <= '9') {
            v = v * 10u + (uint32_t)(*p - '0');
            p++;
            digits++;
            if (v > 0xFFFFFFFFu) return IV_EINVAL;
        }
        if (digits == 0) return IV_EINVAL; /* 非数字 */
        if (n >= 32) return IV_ERANGE;
        arc[n++] = v;
    }
    if (n < 2) return IV_EINVAL;
    if (arc[0] > 2u) return IV_EINVAL;
    if (idx >= cap) return IV_ERANGE;
    out[idx++] = (uint8_t)(40u * arc[0] + arc[1]);
    for (i = 2; i < n; i++) {
        uint32_t v = arc[i];
        uint8_t tmp[5];
        int k = 0;
        if (v < 128u) {
            if (idx >= cap) return IV_ERANGE;
            out[idx++] = (uint8_t)v;
            continue;
        }
        do {
            tmp[k++] = (uint8_t)(v & 0x7Fu);
            v >>= 7;
        } while (v > 0 && k < 5);
        while (k > 0) {
            uint8_t b = tmp[--k];
            if (k > 0) b |= 0x80u;
            if (idx >= cap) return IV_ERANGE;
            out[idx++] = b;
        }
    }
    return (int)idx;
}

int iv_snmp_oid_decode(const uint8_t *in, size_t inlen, char *out, size_t cap) {
    char tmp[IV_SNMP_OID_STR_MAX];
    size_t n = 0, i;
    uint32_t acc = 0;
    int have = 0;
    if (in == NULL || out == NULL || inlen == 0) return IV_EINVAL;
    {
        uint32_t a = in[0] / 40u;
        uint32_t b = in[0] % 40u;
        if (a > 2u) {
            a = 2u;
            b = (uint32_t)in[0] - 80u;
        }
        n = (size_t)snprintf(tmp, sizeof(tmp), ".%u.%u", a, b);
    }
    for (i = 1; i < inlen; i++) {
        acc = (acc << 7) | (uint32_t)(in[i] & 0x7Fu);
        if (in[i] & 0x80u) {
            have = 1;
            continue;
        }
        have = 0;
        if (n + 16 >= sizeof(tmp)) return IV_ERANGE;
        n += (size_t)snprintf(tmp + n, sizeof(tmp) - n, ".%u", acc);
        acc = 0;
    }
    if (have) return IV_EPROTO; /* 截断的多字节 arc */
    if (cap <= n) return IV_ERANGE;
    memcpy(out, tmp, n + 1);
    return (int)n;
}

int iv_snmp_oid_in_subtree(const char *base, const char *oid) {
    size_t n;
    if (base == NULL || oid == NULL) return 0;
    if ((base[0] == '.') != (oid[0] == '.')) return 0;
    n = strlen(base);
    if (strncmp(base, oid, n) != 0) return 0;
    return (oid[n] == '.' || oid[n] == '\0');
}

/* ---------------------------------------------------------------------------
 * 构造请求
 * ------------------------------------------------------------------------- */
int iv_snmp_build_request(uint8_t pdu_tag, const char *community,
                          uint32_t reqid, const char *oid, uint8_t *out,
                          size_t cap) {
    uint8_t oidb[IV_SNMP_OID_STR_MAX];
    int oidlen;
    wr_t w;
    size_t clen;
    if (community == NULL || oid == NULL || out == NULL) return IV_EINVAL;
    if (pdu_tag != IV_SNMP_PDU_GET && pdu_tag != IV_SNMP_PDU_GETNEXT)
        return IV_EINVAL;
    oidlen = iv_snmp_oid_encode(oid, oidb, sizeof(oidb));
    if (oidlen < 0) return oidlen;
    clen = strlen(community);
    if (clen > 255u) return IV_ERANGE;

    memset(&w, 0, sizeof(w));
    w.buf = out;
    w.cap = cap;

    /* 这里手写嵌套：外层 */
    {
        size_t outer = wr_open(&w, 0x30);
        /* version INTEGER 1 */
        {
            uint8_t one = 0x01;
            wr_tlv(&w, IV_SNMP_T_INTEGER, &one, 1);
        }
        /* community OCTET STRING */
        wr_tlv(&w, IV_SNMP_T_OCTETSTR, community, clen);
        /* PDU */
        {
            size_t pdu = wr_open(&w, pdu_tag);
            /* request-id INTEGER 4 字节无符号 */
            {
                uint8_t rid[4];
                rid[0] = (uint8_t)(reqid >> 24);
                rid[1] = (uint8_t)(reqid >> 16);
                rid[2] = (uint8_t)(reqid >> 8);
                rid[3] = (uint8_t)reqid;
                wr_tlv(&w, IV_SNMP_T_INTEGER, rid, 4);
            }
            /* error-status / error-index = 0 */
            {
                uint8_t z = 0x00;
                wr_tlv(&w, IV_SNMP_T_INTEGER, &z, 1);
                wr_tlv(&w, IV_SNMP_T_INTEGER, &z, 1);
            }
            /* variable-bindings */
            {
                size_t vbs = wr_open(&w, 0x30);
                size_t vb = wr_open(&w, 0x30);
                wr_tlv(&w, IV_SNMP_T_OID, oidb, (size_t)oidlen);
                /* NULL 值：tag 0x05、长度 0 */
                wr_tlv(&w, IV_SNMP_T_NULL, NULL, 0);
                wr_close(&w, vb);
                wr_close(&w, vbs);
            }
            wr_close(&w, pdu);
        }
        wr_close(&w, outer);
    }
    if (w.err) return w.err;
    return (int)w.n;
}

/* ---------------------------------------------------------------------------
 * 解析应答
 * ------------------------------------------------------------------------- */
static int parse_value(const uint8_t *v, size_t l, uint8_t tag,
                       iv_snmp_value_t *val) {
    val->type = tag;
    val->num = 0;
    switch (tag) {
        case IV_SNMP_T_INTEGER: {
            size_t i;
            int64_t acc = 0;
            if (l == 0 || l > 8) return IV_EPROTO;
            for (i = 0; i < l; i++) acc = (acc << 8) | v[i];
            /* 符号扩展（对无符号掩码左移，避免"左移负值"未定义行为） */
            if (l < 8 && (v[0] & 0x80u)) {
                uint64_t mask = ~0ULL << (l * 8);
                acc |= (int64_t)mask;
            }
            val->num = acc;
            return IV_OK;
        }
        case IV_SNMP_T_COUNTER32:
        case IV_SNMP_T_GAUGE32:
        case IV_SNMP_T_TIMETICKS:
        case IV_SNMP_T_COUNTER64: {
            size_t i;
            int64_t acc = 0;
            if (l == 0 || l > 8) return IV_EPROTO;
            for (i = 0; i < l; i++) acc = (acc << 8) | v[i];
            val->num = acc;
            return IV_OK;
        }
        case IV_SNMP_T_OCTETSTR: {
            size_t n = l < (sizeof(val->str) - 1) ? l : (sizeof(val->str) - 1);
            memcpy(val->str, v, n);
            val->str[n] = '\0';
            return IV_OK;
        }
        case IV_SNMP_T_IPADDR: {
            if (l != 4) return IV_EPROTO;
            snprintf(val->str, sizeof(val->str), "%u.%u.%u.%u", v[0], v[1], v[2],
                     v[3]);
            return IV_OK;
        }
        case IV_SNMP_T_OID: {
            int rc = iv_snmp_oid_decode(v, l, val->str, sizeof(val->str));
            return rc < 0 ? rc : IV_OK;
        }
        case IV_SNMP_T_NOSUCHOBJECT:
        case IV_SNMP_T_NOSUCHINSTANCE:
        case IV_SNMP_T_ENDOFMIBVIEW:
            return IV_OK; /* 无值（哨兵） */
        case IV_SNMP_T_NULL:
            return IV_OK;
        default:
            return IV_EPROTO;
    }
}

int iv_snmp_parse_response(const uint8_t *pkt, size_t len, uint32_t want_reqid,
                           iv_snmp_value_t *val, int *err_status) {
    rd_t outer, pdu, vbs, vb;
    uint8_t t;
    const uint8_t *v;
    size_t l;
    int64_t reqid, es, ei;
    int rc;
    if (pkt == NULL || val == NULL) return IV_EINVAL;
    memset(val, 0, sizeof(*val));
    if (err_status) *err_status = 0;

    outer.p = pkt;
    outer.n = len;
    rc = rd_enter(&outer, 0x30);
    if (rc != IV_OK) return rc;
    /* version */
    rc = rd_int(&outer, IV_SNMP_T_INTEGER, &reqid);
    if (rc != IV_OK) return rc;
    /* community（值不校验） */
    rc = rd_tlv(&outer, &t, &v, &l);
    if (rc != IV_OK) return rc;
    if (t != IV_SNMP_T_OCTETSTR) return IV_EPROTO;
    /* PDU */
    rc = rd_enter(&outer, IV_SNMP_PDU_GETRESP);
    if (rc != IV_OK) return rc;
    pdu = outer;
    /* request-id */
    rc = rd_int(&pdu, IV_SNMP_T_INTEGER, &reqid);
    if (rc != IV_OK) return rc;
    if ((uint32_t)reqid != want_reqid) return IV_EPROTO; /* 不是本事务的应答 */
    /* error-status / error-index */
    rc = rd_int(&pdu, IV_SNMP_T_INTEGER, &es);
    if (rc != IV_OK) return rc;
    rc = rd_int(&pdu, IV_SNMP_T_INTEGER, &ei);
    if (rc != IV_OK) return rc;
    if (err_status) *err_status = (int)es;
    /* var-bindings */
    rc = rd_enter(&pdu, 0x30);
    if (rc != IV_OK) return rc;
    vbs = pdu;
    /* 只取第一条 varbind */
    rc = rd_enter(&vbs, 0x30);
    if (rc != IV_OK) return rc;
    vb = vbs;
    /* name (OID) */
    rc = rd_tlv(&vb, &t, &v, &l);
    if (rc != IV_OK) return rc;
    if (t != IV_SNMP_T_OID) return IV_EPROTO;
    rc = iv_snmp_oid_decode(v, l, val->oid, sizeof(val->oid));
    if (rc < 0) return rc;
    /* value（任意 tag） */
    rc = rd_tlv(&vb, &t, &v, &l);
    if (rc != IV_OK) return rc;
    return parse_value(v, l, t, val);
}

/* ---------------------------------------------------------------------------
 * 事务
 * ------------------------------------------------------------------------- */
int iv_snmp_transact(const iv_snmp_target_t *t, uint8_t pdu_tag, uint32_t reqid,
                     const char *oid, uint8_t *resp, size_t respcap) {
    uint8_t req[IV_SNMP_PKT_MAX];
    struct sockaddr_in dst;
    struct timeval tv;
    int fd, rc, attempt;
    uint32_t tries;
    int reqlen;
    if (t == NULL || t->iops == NULL || resp == NULL) return IV_EINVAL;
    reqlen = iv_snmp_build_request(pdu_tag, t->community, reqid, oid, req,
                                   sizeof(req));
    if (reqlen < 0) return reqlen;

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(t->port ? t->port : IV_SNMP_PORT_DEFAULT);
    if (inet_pton(AF_INET, t->host, &dst.sin_addr) != 1) return IV_EINVAL;

    fd = t->iops->socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return IV_EIO;
    tv.tv_sec = (time_t)((t->timeout_ms ? t->timeout_ms : IV_SNMP_TIMEOUT_MS_DEFAULT) / 1000u);
    tv.tv_usec = (suseconds_t)(((t->timeout_ms ? t->timeout_ms : IV_SNMP_TIMEOUT_MS_DEFAULT) % 1000u) * 1000u);
    if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_usec = 100000;
    t->iops->setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    tries = (t->retries ? t->retries : IV_SNMP_RETRIES_DEFAULT) + 1u;
    rc = IV_ETIMEDOUT;
    for (attempt = 0; (uint32_t)attempt < tries; attempt++) {
        ssize_t s = t->iops->sendto(fd, req, (size_t)reqlen, 0,
                                    (struct sockaddr *)&dst, sizeof(dst));
        if (s < 0) {
            rc = IV_EIO;
            break;
        }
        for (;;) {
            struct sockaddr_storage from;
            socklen_t fl = sizeof(from);
            ssize_t r = t->iops->recvfrom(fd, resp, respcap, 0,
                                          (struct sockaddr *)&from, &fl);
            if (r < 0) {
                if (errno == EINTR) continue; /* 被信号打断，继续等 */
                break;                         /* EAGAIN/EWOULDBLOCK = 本轮到点 */
            }
            if (r == 0) break;
            /* reqid 匹配才认；不匹配的包丢弃继续等 */
            {
                iv_snmp_value_t tmp;
                int es = 0;
                if (iv_snmp_parse_response(resp, (size_t)r, reqid, &tmp, &es) ==
                    IV_OK) {
                    rc = (int)r;
                    goto done;
                }
            }
        }
        rc = IV_ETIMEDOUT;
    }
done:
    t->iops->close(fd);
    (void)io_now(t);
    return rc;
}

int iv_snmp_get(const iv_snmp_target_t *t, const char *oid,
                iv_snmp_value_t *out) {
    uint8_t resp[IV_SNMP_PKT_MAX];
    uint32_t reqid;
    int n, es = 0;
    if (t == NULL || oid == NULL || out == NULL) return IV_EINVAL;
    reqid = (t->reqid_seed ? t->reqid_seed : 1u) + 1u;
    n = iv_snmp_transact(t, IV_SNMP_PDU_GET, reqid, oid, resp, sizeof(resp));
    if (n < 0) return n;
    if (iv_snmp_parse_response(resp, (size_t)n, reqid, out, &es) != IV_OK)
        return IV_EPROTO;
    if (es != 0) return IV_EPROTO;
    return iv_snmp_value_valid(out->type) ? IV_OK : IV_ENOENT;
}

int iv_snmp_getnext(const iv_snmp_target_t *t, const char *oid,
                    iv_snmp_value_t *out) {
    uint8_t resp[IV_SNMP_PKT_MAX];
    uint32_t reqid;
    int n, es = 0;
    if (t == NULL || oid == NULL || out == NULL) return IV_EINVAL;
    reqid = (t->reqid_seed ? t->reqid_seed : 1u) + 2u;
    n = iv_snmp_transact(t, IV_SNMP_PDU_GETNEXT, reqid, oid, resp, sizeof(resp));
    if (n < 0) return n;
    if (iv_snmp_parse_response(resp, (size_t)n, reqid, out, &es) != IV_OK)
        return IV_EPROTO;
    if (es != 0) return IV_EPROTO;
    return IV_OK;
}

int iv_snmp_walk(const iv_snmp_target_t *t, const char *column_oid,
                 iv_snmp_value_t *rows, int max_rows, int *n_out) {
    char cur[IV_SNMP_OID_STR_MAX];
    int i, cnt = 0;
    uint32_t cap;
    if (t == NULL || column_oid == NULL || rows == NULL || n_out == NULL)
        return IV_EINVAL;
    *n_out = 0;
    cap = t->max_rows ? t->max_rows : IV_SNMP_MAX_ROWS_DEFAULT;
    if (max_rows <= 0) return IV_EINVAL;
    if ((uint32_t)max_rows < cap) cap = (uint32_t)max_rows;

    if (strlen(column_oid) >= sizeof(cur)) return IV_ERANGE;
    strncpy(cur, column_oid, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';

    for (i = 0; i < (int)cap; i++) {
        iv_snmp_value_t v;
        int rc = iv_snmp_getnext(t, cur, &v);
        if (rc != IV_OK) return (cnt > 0) ? IV_OK : rc;
        if (!iv_snmp_value_valid(v.type)) break;            /* 无值/到底 */
        if (!iv_snmp_oid_in_subtree(column_oid, v.oid)) break; /* 走出本列 */
        rows[cnt++] = v;
        if (strcmp(cur, v.oid) == 0) break; /* 无进展（对端原地返回）→ 记完本行即止 */
        strncpy(cur, v.oid, sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = '\0';
    }
    *n_out = cnt;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 值/类型工具
 * ------------------------------------------------------------------------- */
int iv_snmp_value_valid(const uint8_t type) {
    if (type == 0) return 0;
    if (type == IV_SNMP_T_NOSUCHOBJECT || type == IV_SNMP_T_NOSUCHINSTANCE ||
        type == IV_SNMP_T_ENDOFMIBVIEW)
        return 0;
    if (type == IV_SNMP_T_NULL) return 0;
    return 1;
}

const char *iv_snmp_type_name(uint8_t type) {
    switch (type) {
        case IV_SNMP_T_INTEGER: return "integer";
        case IV_SNMP_T_OCTETSTR: return "octet-string";
        case IV_SNMP_T_NULL: return "null";
        case IV_SNMP_T_OID: return "oid";
        case IV_SNMP_T_IPADDR: return "ip-address";
        case IV_SNMP_T_COUNTER32: return "counter32";
        case IV_SNMP_T_GAUGE32: return "gauge32";
        case IV_SNMP_T_TIMETICKS: return "timeticks";
        case IV_SNMP_T_COUNTER64: return "counter64";
        case IV_SNMP_T_NOSUCHOBJECT: return "no-such-object";
        case IV_SNMP_T_NOSUCHINSTANCE: return "no-such-instance";
        case IV_SNMP_T_ENDOFMIBVIEW: return "end-of-mib";
        default: return "none";
    }
}

void iv_snmp_target_default(iv_snmp_target_t *t) {
    if (t == NULL) return;
    memset(t, 0, sizeof(*t));
    t->port = IV_SNMP_PORT_DEFAULT;
    t->timeout_ms = IV_SNMP_TIMEOUT_MS_DEFAULT;
    t->retries = IV_SNMP_RETRIES_DEFAULT;
    t->max_rows = IV_SNMP_MAX_ROWS_DEFAULT;
    t->reqid_seed = 1;
    t->iops = NULL;
}
