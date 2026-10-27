/*
 * iv_agent 装配层 M4 集成测试（功能开发计划 M4-S4.2 设备发现 / M4-S4.3 SNMP 采集）
 *
 * ============================ 它验的是什么 ============================
 * 单测（tests/unit/test_device.c / test_snmp.c）各自证明「模块本身对不对」；
 * 本用例证明 **iv_agent 把它们接进装配层之后，整条链真的会转**：
 *
 *   S4.2 设备发现
 *     dev_enable=1 ──► iv_device_open ──► 开机自动 search ──► iv_device_tick
 *     平台 0xE4（经 iv_report_feed_platform 注入）──► on_query_ipc ──► 重开一轮
 *   S4.3 SNMP 采集
 *     iv_agent_snmp_start / 平台 0xE7 ──► iv_taskpool（**worker 线程阻塞收发**）
 *          └─ 真 UDP 请求 ──► 用例内的假 SNMP 应答器 ──► 真 GetResponse
 *               └─ on_snmp_done（装配层线程）──► 结果表 ──► iv_agent_snmp_get
 *
 * ============================ 为什么要有"假 SNMP 应答器" ============================
 * `iv_agent_open()` 把作业目标写成 `jb->t.iops = iv_snmp_default_iops()`
 * （**真 socket**）——装配层不提供注入点，这是刻意的（生产路径不留测试后门）。
 * 所以本用例在**回环上开一个真 UDP socket**，按收到的请求现造标准 GetResponse：
 *   GET     → 字典命中则回值，未命中回 `noSuchObject`（→ 客户端记"无值"）；
 *   GETNEXT → 回归"字典里比请求 OID 大的最小那条"（**按 OID 数字逐段比较**，
 *             不是字符串比较——`.8.3` 与 `.10.1` 的字符串序是反的）。
 * 这样走的是**真报文、真线程、真超时/重试**，只有"对端是谁"是假的。
 *
 * ============================ 反向证伪点（每条都对应一个真缺陷） ============================
 *   m04 未配 OID 的槽位必须**不发包、不填值**（请求计数恰为 4，槽位 n_rows=0）；
 *   m04 字典未命中的标量必须回"无值"而**不拿相邻槽位的值乱填**（slot8 = 0 行）；
 *   m06 表列必须走 GETNEXT 走表并在**走出子树时停**（3 行，不是 4 行）；
 *   m01 `pool == NULL` 必须拒绝采集（IV_ESTATE）—— SNMP 是阻塞式收发，
 *       放进 Reactor 线程就违反架构 §4.2；
 *   m03/m07 0xE4 / 0xE7 平台命令必须真的路由到本层（只 stop 搜索/只置空闲后喂
 *       一帧即恢复，**没有别的路径能让它们恢复**）。
 *
 * ============================ 已知取舍（登记） ============================
 *   - `dev_ifname` 刻意用**不存在的网口名**（`ivnull0`）：本用例验的是**接线**
 *     （open / tick / search / 0xE4），不验二层抓包（那是板端实测的事）。
 *     用真网口会让用例依赖"跑测者有没有 CAP_NET_RAW"，结论随环境漂移。
 *   - `cfg.snmp[].oid[]` 直接填槽位（**位置一一对应，空串＝该槽不采**）。
 *     app 层"从配置键读进这些槽位"的那段（`src/app/main.c` 的 `cfg_snmp_target()`）
 *     因 **app 层 .c 不进静态库**、单测链不到符号 ⇒ 只能在整机冒烟覆盖。
 *
 * ============================ 临时产物 ============================
 * pty（虚拟串口）／UDP socket／$TMPDIR 下一临时目录，用例结束前全部释放。
 * AGENTS.md 规则 6。
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_db.h"
#include "ivsbox/iv_device.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_proto.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_snmp.h"
#include "ivsbox/iv_store.h"
#include "ivsbox/iv_taskpool.h"

#define STEP_MS 50u

/* iv_agent_t 约 35 KB ⇒ 必须静态区 */
static iv_agent_t g_agent;
static int        g_fail;
static uint64_t   g_now;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void log_discard(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

/* ---------------------------------------------------------------------------
 * 夹具
 * ------------------------------------------------------------------------- */
static int pty_pair(char *slave_path, size_t cap)
{
    int m;

    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;
    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        (void)close(m);
        return -1;
    }
    if (ptsname_r(m, slave_path, cap) != 0) {
        (void)close(m);
        return -1;
    }
    if (fcntl(m, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(m);
        return -1;
    }
    return m;
}

static int mk_tmpdir(char *out, size_t cap)
{
    const char *base = getenv("TMPDIR");
    char        tpl[256];

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    if (snprintf(tpl, sizeof tpl, "%s/ivsbox-m4-XXXXXX", base) >= (int)sizeof tpl)
        return -1;
    if (mkdtemp(tpl) == NULL)
        return -1;
    if (strlen(tpl) >= cap)
        return -1;
    (void)strcpy(out, tpl);
    return 0;
}

static void rm_tmpdir(const char *dir)
{
    DIR *d;

    d = opendir(dir);
    if (d == NULL)
        return;
    for (;;) {
        struct dirent *e = readdir(d);
        char           p[512];

        if (e == NULL)
            break;
        if (e->d_name[0] == '.')
            continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) < (int)sizeof p)
            (void)unlink(p);
    }
    (void)closedir(d);
    (void)rmdir(dir);
}

/* ---------------------------------------------------------------------------
 * 平台下行帧（0xF0F0 二进制帧）—— 口径同 test_e2e_report 的 build_down。
 * 注入通道用 iv_report_feed_platform()（等价于传输层收到平台字节），
 * 因此本用例**不需要**真的建一条平台 TCP 连接。
 * ------------------------------------------------------------------------- */
static size_t build_down(uint8_t *out, uint16_t devtype, uint32_t devid,
                         uint8_t cmd, uint32_t qn1, uint32_t qn2,
                         const void *data, uint16_t len)
{
    size_t i;

    out[0] = 0xF0u;
    out[1] = 0xF0u;
    out[2] = (uint8_t)IV_PROTO_VER;
    out[3] = (uint8_t)(devtype >> 8);
    out[4] = (uint8_t)devtype;
    out[5] = (uint8_t)(devid >> 16);
    out[6] = (uint8_t)(devid >> 8);
    out[7] = (uint8_t)devid;
    out[8] = cmd;
    for (i = 0; i < 4u; i++) {
        out[9 + i]  = (uint8_t)(qn1 >> (24u - 8u * i));
        out[13 + i] = (uint8_t)(qn2 >> (24u - 8u * i));
    }
    out[17] = (uint8_t)len;
    if (len != 0u && data != NULL)
        memcpy(&out[18], data, len);
    out[18 + len] = iv_crc8(&out[2], 16u + (size_t)len, IV_CRC8_SEED_INIT);
    out[19 + len] = 0xFFu;
    out[20 + len] = 0xFFu;
    return 18u + (size_t)len + 3u;
}

/* ---------------------------------------------------------------------------
 * 假 SNMP 应答器
 * ------------------------------------------------------------------------- */

/* 最小 BER 游标：只够读懂本工程自己造的请求 */
typedef struct {
    const uint8_t *p;
    size_t         n;
} cur_t;

/* 读一个 TLV 头，把游标推进到**内容**起点、n 收窄为内容长度。
 * want 非 0 时校验 tag；got 回填实际 tag。 */
static int cur_enter(cur_t *c, uint8_t want, uint8_t *got)
{
    uint8_t t;
    size_t  len, hl;

    if (c->n < 2u)
        return 0;
    t = c->p[0];
    if (c->p[1] < 0x80u) {
        len = c->p[1];
        hl  = 2u;
    } else if (c->p[1] == 0x81u) {
        if (c->n < 3u)
            return 0;
        len = c->p[2];
        hl  = 3u;
    } else if (c->p[1] == 0x82u) {
        if (c->n < 4u)
            return 0;
        len = ((size_t)c->p[2] << 8) | (size_t)c->p[3];
        hl  = 4u;
    } else {
        return 0;
    }
    if (c->n < hl + len)
        return 0;
    if (want != 0u && t != want)
        return 0;
    if (got != NULL)
        *got = t;
    c->p += hl;
    c->n  = len;
    return 1;
}

/* 读一个完整 TLV（tag + len + 内容），游标越过它 */
static int cur_value(cur_t *c, uint8_t *tag, const uint8_t **val, size_t *len)
{
    uint8_t t;
    size_t  l, hl;

    if (c->n < 2u)
        return 0;
    t = c->p[0];
    if (c->p[1] < 0x80u) {
        l  = c->p[1];
        hl = 2u;
    } else if (c->p[1] == 0x81u) {
        if (c->n < 3u)
            return 0;
        l  = c->p[2];
        hl = 3u;
    } else if (c->p[1] == 0x82u) {
        if (c->n < 4u)
            return 0;
        l  = ((size_t)c->p[2] << 8) | (size_t)c->p[3];
        hl = 4u;
    } else {
        return 0;
    }
    if (c->n < hl + l)
        return 0;
    *tag = t;
    *val = c->p + hl;
    *len = l;
    c->p += hl + l;
    c->n -= hl + l;
    return 1;
}

/* OID 点分文本 → 数字段数组 */
#define OID_ARCS_MAX 48
static int oid_arcs(const char *s, long *out, int cap)
{
    int         n = 0;
    const char *p = s;

    while (*p != '\0') {
        long v = 0;

        if (*p == '.') {
            p++;
            continue;
        }
        if (*p < '0' || *p > '9')
            return -1;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (long)(*p - '0');
            p++;
        }
        if (n >= cap)
            return -1;
        out[n++] = v;
    }
    return n;
}

/* OID 数字逐段字典序（**不是字符串比较**：`.8.3` < `.10.1`） */
static int oid_cmp(const char *a, const char *b)
{
    long A[OID_ARCS_MAX], B[OID_ARCS_MAX];
    int  na = oid_arcs(a, A, OID_ARCS_MAX);
    int  nb = oid_arcs(b, B, OID_ARCS_MAX);
    int  i, m;

    if (na < 0 || nb < 0)
        return 0;
    m = (na < nb) ? na : nb;
    for (i = 0; i < m; i++) {
        if (A[i] != B[i])
            return (A[i] < B[i]) ? -1 : 1;
    }
    if (na == nb)
        return 0;
    return (na < nb) ? -1 : 1;
}

typedef struct {
    const char *oid;
    uint8_t     tag;
    const char *str; /* OCTET STRING 值；NULL → 用 num */
    int64_t     num;
} resp_ent_t;

/* 假设备 MIB。**刻意与槽位表语义对齐**：
 *   —— 摄像机（HIK 私有子树 .39165.1）三条标量
 *   —— 交换机 sysName 一条标量 + IF-MIB ifOperStatus 一列表列（含"邻居列"用于验子树边界）
 */
static const resp_ent_t g_dict[] = {
    { ".1.3.6.1.4.1.39165.1.1.0", IV_SNMP_T_OCTETSTR, "CAM-DESC", 0 },
    { ".1.3.6.1.4.1.39165.1.3.0", IV_SNMP_T_OCTETSTR, "CAM-NAME", 0 },
    { ".1.3.6.1.4.1.39165.1.7.0", IV_SNMP_T_INTEGER, NULL, 42 },
    { ".1.3.6.1.2.1.1.5.0", IV_SNMP_T_OCTETSTR, "SW-NAME", 0 },
    { ".1.3.6.1.2.1.2.2.1.8.1", IV_SNMP_T_INTEGER, NULL, 1 },
    { ".1.3.6.1.2.1.2.2.1.8.2", IV_SNMP_T_INTEGER, NULL, 2 },
    { ".1.3.6.1.2.1.2.2.1.8.3", IV_SNMP_T_INTEGER, NULL, 1 },
    { ".1.3.6.1.2.1.2.2.1.10.1", IV_SNMP_T_COUNTER32, NULL, 5 },
};
#define DICT_N (sizeof(g_dict) / sizeof(g_dict[0]))

static const resp_ent_t *dict_find(const char *oid)
{
    size_t i;

    for (i = 0; i < DICT_N; i++) {
        if (strcmp(g_dict[i].oid, oid) == 0)
            return &g_dict[i];
    }
    return NULL;
}

static const resp_ent_t *dict_next(const char *oid)
{
    const resp_ent_t *best = NULL;
    size_t            i;

    for (i = 0; i < DICT_N; i++) {
        if (oid_cmp(g_dict[i].oid, oid) <= 0)
            continue;
        if (best == NULL || oid_cmp(g_dict[i].oid, best->oid) < 0)
            best = &g_dict[i];
    }
    return best;
}

/* 小整数 → BER 内容字节（本用例量级：0..2^31-1） */
static size_t enc_int(uint8_t *out, int64_t v)
{
    if (v >= 0 && v <= 127) {
        out[0] = (uint8_t)v;
        return 1u;
    }
    out[0] = (uint8_t)((v >> 24) & 0xFF);
    out[1] = (uint8_t)((v >> 16) & 0xFF);
    out[2] = (uint8_t)((v >> 8) & 0xFF);
    out[3] = (uint8_t)(v & 0xFF);
    return 4u;
}

/* TLV 包装（单/双字节长式），口径同 tests/unit/test_snmp.c */
static size_t wrap(uint8_t *out, uint8_t tag, const uint8_t *in, size_t n)
{
    size_t  hl;
    uint8_t h[4];

    if (n < 0x80u) {
        h[0] = (uint8_t)n;
        hl   = 1u;
    } else if (n <= 0xFFu) {
        h[0] = 0x81u;
        h[1] = (uint8_t)n;
        hl   = 2u;
    } else {
        h[0] = 0x82u;
        h[1] = (uint8_t)(n >> 8);
        h[2] = (uint8_t)n;
        hl   = 3u;
    }
    out[0] = tag;
    memcpy(out + 1, h, hl);
    if (n > 0u)
        memcpy(out + 1 + hl, in, n);
    return 1u + hl + n;
}

/* 造一条完整 GetResponse（单 varbind）。返回总长度，失败 0。 */
static size_t mk_response(uint8_t *out, size_t cap, uint32_t reqid,
                          const char *oid, uint8_t vtag, const uint8_t *vval,
                          size_t vlen)
{
    static const uint8_t z[3]   = { 0x02, 0x01, 0x00 };
    static const uint8_t ver[3] = { 0x02, 0x01, 0x01 };
    uint8_t oidb[64], oidtlv[96], vtlv[400];
    uint8_t vbcontent[512], varbind[560], vblist[600];
    uint8_t pducontent[700], pdutlv[800], msgcontent[880];
    uint8_t rid[4];
    const char *comm = "public";
    size_t  ol, vl, vc, vbn, vbln, pc, pn, mcn, cl, total;
    int     oidl = iv_snmp_oid_encode(oid, oidb, sizeof oidb);

    if (oidl < 0)
        return 0u;
    ol = wrap(oidtlv, IV_SNMP_T_OID, oidb, (size_t)oidl);
    vl = wrap(vtlv, vtag, vval, vlen);

    vc = 0u;
    memcpy(vbcontent + vc, oidtlv, ol);
    vc += ol;
    memcpy(vbcontent + vc, vtlv, vl);
    vc += vl;
    vbn  = wrap(varbind, 0x30, vbcontent, vc);
    vbln = wrap(vblist, 0x30, varbind, vbn);

    rid[0] = (uint8_t)(reqid >> 24);
    rid[1] = (uint8_t)(reqid >> 16);
    rid[2] = (uint8_t)(reqid >> 8);
    rid[3] = (uint8_t)reqid;
    pc = wrap(pducontent, 0x02, rid, 4u);
    memcpy(pducontent + pc, z, 3u);
    pc += 3u;
    memcpy(pducontent + pc, z, 3u);
    pc += 3u;
    memcpy(pducontent + pc, vblist, vbln);
    pc += vbln;
    pn = wrap(pdutlv, IV_SNMP_PDU_GETRESP, pducontent, pc);

    memcpy(msgcontent, ver, 3u);
    mcn = 3u;
    cl  = wrap(msgcontent + mcn, IV_SNMP_T_OCTETSTR, (const uint8_t *)comm,
               strlen(comm));
    mcn += cl;
    memcpy(msgcontent + mcn, pdutlv, pn);
    mcn += pn;

    total = wrap(out, 0x30, msgcontent, mcn);
    return (total > cap) ? 0u : total;
}

static size_t g_req_n;

/* 收一条请求 → 回一条应答。返回 1 = 处理了一条，0 = 无请求。 */
static int responder_serve(int fd)
{
    uint8_t            req[IV_SNMP_PKT_MAX * 2u];
    uint8_t            resp[900];
    uint8_t            vbuf[8];
    uint8_t            pdu_tag = 0u;
    uint8_t            t;
    const uint8_t     *v   = NULL;
    size_t             vl  = 0u;
    uint32_t           reqid = 0u;
    char               oid[IV_SNMP_OID_STR_MAX];
    struct sockaddr_in from;
    socklen_t          fl = sizeof from;
    ssize_t            r;
    const resp_ent_t  *hit = NULL;
    const char        *rsp_oid;
    uint8_t            rsp_tag = IV_SNMP_T_NOSUCHOBJECT;
    size_t             vlen = 0u;
    size_t             n;

    r = recvfrom(fd, req, sizeof req, 0, (struct sockaddr *)&from, &fl);
    if (r <= 0)
        return 0;
    g_req_n++;

    /* ---- 解析请求：PDU 类型 + reqid + 目标 OID ---- */
    {
        cur_t c;

        c.p    = req;
        c.n    = (size_t)r;
        oid[0] = '\0';
        if (!cur_enter(&c, 0x30u, NULL))
            return 1;
        if (!cur_value(&c, &t, &v, &vl))
            return 1; /* version INTEGER */
        if (!cur_value(&c, &t, &v, &vl))
            return 1; /* community OCTET STRING */
        if (!cur_enter(&c, 0u, &pdu_tag))
            return 1;
        if (!cur_value(&c, &t, &v, &vl))
            return 1; /* request-id */
        {
            size_t i;

            for (i = 0; i < vl && i < 4u; i++)
                reqid = (reqid << 8) | (uint32_t)v[i];
        }
        if (!cur_value(&c, &t, &v, &vl))
            return 1; /* error-status */
        if (!cur_value(&c, &t, &v, &vl))
            return 1; /* error-index */
        if (!cur_enter(&c, 0x30u, NULL))
            return 1; /* varbind list */
        if (!cur_enter(&c, 0x30u, NULL))
            return 1; /* varbind */
        if (!cur_value(&c, &t, &v, &vl) || t != IV_SNMP_T_OID)
            return 1;
        if (iv_snmp_oid_decode(v, vl, oid, sizeof oid) < 0)
            return 1;
    }

    /* ---- 造应答 ---- */
    rsp_oid = oid;
    if (pdu_tag == IV_SNMP_PDU_GET) {
        hit = dict_find(oid);
    } else {
        hit = dict_next(oid);
        if (hit != NULL)
            rsp_oid = hit->oid; /* GETNEXT 应答的名字是"下一个"实例 */
        else
            rsp_tag = IV_SNMP_T_ENDOFMIBVIEW;
    }
    if (hit == NULL) {
        vlen = 0u; /* noSuchObject / endOfMibView：只有名字、没有值 */
    } else if (hit->str != NULL) {
        rsp_tag = hit->tag;
        v       = (const uint8_t *)hit->str;
        vlen    = strlen(hit->str);
    } else {
        rsp_tag = hit->tag;
        vlen    = enc_int(vbuf, hit->num);
        v       = vbuf;
    }

    n = mk_response(resp, sizeof resp, reqid, rsp_oid, rsp_tag, v, vlen);
    if (n == 0u)
        return 1;
    (void)sendto(fd, resp, n, 0, (struct sockaddr *)&from, fl);
    return 1;
}

/* 在 ms 内收干请求；已处理过则不阻塞（尽快回到 iv_agent_step） */
static int responder_drain(int fd, int ms)
{
    int served = 0;

    for (;;) {
        struct pollfd p;
        int           pr;

        p.fd      = fd;
        p.events  = POLLIN;
        p.revents = 0;
        pr        = poll(&p, 1, (served == 0) ? ms : 0);
        if (pr <= 0)
            break;
        if (responder_serve(fd) == 0)
            break;
        served++;
    }
    return served;
}

/* ---------------------------------------------------------------------------
 * 驱动
 * ------------------------------------------------------------------------- */
/* 应答 + 推进，直到指定目标 DONE（或超时）。返回 1 = DONE。 */
static int pump_until(int fd, int which, int max_iter)
{
    int i;

    for (i = 0; i < max_iter; i++) {
        if (iv_agent_snmp_state(&g_agent, which) == IV_AGENT_SNMP_DONE)
            return 1;
        (void)responder_drain(fd, 40);
        g_now += STEP_MS;
        (void)iv_agent_step(&g_agent, g_now);
    }
    return iv_agent_snmp_state(&g_agent, which) == IV_AGENT_SNMP_DONE;
}

/* 两个目标都在飞时用：跑到"两个都 DONE"（不做请求计数断言） */
static int pump_until_both(int fd, int max_iter)
{
    int i;

    for (i = 0; i < max_iter; i++) {
        if (iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_CAM) == IV_AGENT_SNMP_DONE &&
            iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_SW) == IV_AGENT_SNMP_DONE)
            return 1;
        (void)responder_drain(fd, 40);
        g_now += STEP_MS;
        (void)iv_agent_step(&g_agent, g_now);
    }
    return (iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_CAM) == IV_AGENT_SNMP_DONE &&
            iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_SW) == IV_AGENT_SNMP_DONE);
}

/* ---------------------------------------------------------------------------
 * 主流程
 * ------------------------------------------------------------------------- */
static int scenario(void)
{
    char                slave_path[128];
    char                qdir[256];
    int                 master = -1;
    int                 usrv   = -1;
    uint16_t            uport  = 0;
    iv_agent_cfg_t      cfg;
    iv_taskpool_t      *pool = NULL;
    iv_db_t            *tdb = NULL;
    iv_store_t         *tstore = NULL;
    char                dbdir[128] = "";
    iv_dev_desc_t       desc;
    const char         *name = NULL;
    const char         *oid  = NULL;
    iv_agent_snmp_row_t rows[IV_AGENT_SNMP_ROWS_MAX];
    int                 nrows = 0;

    iv_log_set_sink(log_discard, NULL);

    master = pty_pair(slave_path, sizeof slave_path);
    if (master < 0) {
        printf("SKIP: cannot create pty pair\n");
        return 0;
    }
    if (mk_tmpdir(qdir, sizeof qdir) != 0) {
        printf("SKIP: cannot create tmpdir\n");
        (void)close(master);
        return 0;
    }

    /* 假 SNMP 应答器：回环上的真 UDP socket */
    usrv = socket(AF_INET, SOCK_DGRAM, 0);
    if (usrv < 0) {
        printf("SKIP: cannot create UDP socket\n");
        (void)close(master);
        rm_tmpdir(qdir);
        return 0;
    }
    {
        struct sockaddr_in sa;
        socklen_t          sl = sizeof sa;

        memset(&sa, 0, sizeof sa);
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port        = 0;
        if (bind(usrv, (struct sockaddr *)&sa, sizeof sa) != 0 ||
            getsockname(usrv, (struct sockaddr *)&sa, &sl) != 0) {
            printf("SKIP: cannot bind UDP responder\n");
            (void)close(usrv);
            (void)close(master);
            rm_tmpdir(qdir);
            return 0;
        }
        uport = ntohs(sa.sin_port);
    }
    (void)fcntl(usrv, F_SETFL, O_NONBLOCK);

    /* ---- 公共配置 ---- */
    iv_agent_cfg_default(&cfg);
    cfg.serial_path  = slave_path;
    cfg.queue_dir    = qdir;
    cfg.devid        = 0x101u;
    cfg.mod          = "M4-MOD";
    cfg.sv           = "M4-SV";
    cfg.report_ms    = 3600u * 1000u;
    cfg.heartbeat_ms = 3600u * 1000u;
    cfg.poll_ms      = 3600u * 1000u;
    cfg.nm_fail_n    = 1u;
    cfg.nm_ok_n      = 1u;
    cfg.nm_hold_s    = 1u;
    cfg.nm_stable_s  = 0u;

    /* =======================================================================
     * m01 两个"没做该步装配"的门必须真的关上
     *   —— 这是**接线本身**的语义，模块单测覆盖不到（模块内没有这两个门）。
     * ======================================================================= */
    cfg.dev_enable = 0;
    cfg.pool       = NULL;
    g_now          = 0u;
    iv_agent_init(&g_agent);
    chk(iv_agent_open(&g_agent, &cfg, NULL, g_now) == IV_OK,
        "m01 open (no dev / no pool)");
    chk(iv_device_is_open(iv_agent_device(&g_agent)) == 0,
        "m01 dev_enable=0 -> iv_device NOT opened (zero fds)");
    chk(iv_agent_device_search(&g_agent, g_now) == IV_ESTATE,
        "m01 dev_enable=0 -> search refused with IV_ESTATE");
    chk(iv_agent_device_count(&g_agent) == 0, "m01 dev_enable=0 -> empty archive");
    chk(iv_agent_device_get(&g_agent, 0, &desc) == IV_ENOENT,
        "m01 dev_enable=0 -> get(0) out of range");
    chk(iv_agent_snmp_start(&g_agent, IV_AGENT_SNMP_CAM, g_now) == IV_ESTATE,
        "m01 pool=NULL -> snmp refused (blocking IO must not run on the reactor)");
    iv_agent_close(&g_agent);
    chk(iv_agent_is_open(&g_agent) == 0, "m01 closed");

    /* =======================================================================
     * m02 装配：dev_enable=1 + 真慢任务池 + 两个 SNMP 目标
     * ======================================================================= */
    pool = iv_taskpool_create(2, 8);
    chk(pool != NULL, "m02 taskpool created");
    if (pool == NULL) {
        (void)close(usrv);
        (void)close(master);
        rm_tmpdir(qdir);
        return 1;
    }

    /* 刻意用不存在的网口名：本用例验接线，不验二层抓包（见文件头取舍） */
    (void)snprintf(cfg.dev_ifname, sizeof cfg.dev_ifname, "%s", "ivnull0");
    cfg.dev_enable       = 1;
    cfg.dev_local_ip_be  = htonl(0x0A000001u); /* 10.0.0.1，仅填充字段 */
    cfg.dev_local_mac[0] = 0x02;
    cfg.dev_local_mac[5] = 0x01;
    cfg.pool             = pool;

    /* S9 延伸：装配 control.db（真实磁盘文件）。m01 段 cfg.store 仍为 NULL
     * （cfg_default 清零），顺带验过 NULL 守卫不炸。 */
    {
        char dbpath[256];

        chk(mk_tmpdir(dbdir, sizeof dbdir) == 0, "m09 store tmpdir");
        if (dbdir[0] == '\0') {
            (void)close(usrv);
            (void)close(master);
            rm_tmpdir(qdir);
            return 1;
        }
        (void)snprintf(dbpath, sizeof dbpath, "%s/control.db", dbdir);
        chk(iv_db_open(dbpath, &tdb) == IV_OK && tdb != NULL, "m09 iv_db_open");
        chk(iv_store_open(tdb, &tstore) == IV_OK && tstore != NULL,
            "m09 iv_store_open (schema ensured)");
        cfg.store = tstore;
    }

    /* 摄像机目标：9 槽位**位置一一对应**；空串＝该槽不采 */
    {
        iv_agent_snmp_cfg_t *sc = &cfg.snmp[IV_AGENT_SNMP_CAM];

        memset(sc, 0, sizeof *sc);
        sc->enable     = 1;
        sc->ip_be      = htonl(INADDR_LOOPBACK);
        sc->port       = uport;
        sc->timeout_ms = 400u;
        sc->retries    = 1u;
        sc->max_rows   = 8u;
        (void)snprintf(sc->community, sizeof sc->community, "%s", "public");
        sc->n_oid = (int)IV_SNMP_CAM_SLOT_N; /* 9 */
        (void)snprintf(sc->oid[0], sizeof sc->oid[0], "%s", ".1.3.6.1.4.1.39165.1.1.0");
        /* 1 空 */
        (void)snprintf(sc->oid[2], sizeof sc->oid[2], "%s", ".1.3.6.1.4.1.39165.1.3.0");
        /* 3/4/5 空 */
        (void)snprintf(sc->oid[6], sizeof sc->oid[6], "%s", ".1.3.6.1.4.1.39165.1.7.0");
        /* 7 空 */
        (void)snprintf(sc->oid[8], sizeof sc->oid[8], "%s", ".1.3.6.1.4.1.39165.1.99.0");
    }
    /* 交换机目标：0 标量 / 1~4 空 / 5 表列 */
    {
        iv_agent_snmp_cfg_t *sc = &cfg.snmp[IV_AGENT_SNMP_SW];

        memset(sc, 0, sizeof *sc);
        sc->enable     = 1;
        sc->ip_be      = htonl(INADDR_LOOPBACK);
        sc->port       = uport;
        sc->timeout_ms = 400u;
        sc->retries    = 1u;
        sc->max_rows   = 8u;
        (void)snprintf(sc->community, sizeof sc->community, "%s", "public");
        sc->n_oid = 6;
        (void)snprintf(sc->oid[0], sizeof sc->oid[0], "%s", ".1.3.6.1.2.1.1.5.0");
        (void)snprintf(sc->oid[5], sizeof sc->oid[5], "%s", ".1.3.6.1.2.1.2.2.1.8");
    }

    g_now = 0u;
    iv_agent_init(&g_agent);
    chk(iv_agent_open(&g_agent, &cfg, NULL, g_now) == IV_OK, "m02 open (dev + pool + snmp)");

    /* =======================================================================
     * m03 S4.2 设备发现：装配 → 开机自检一轮 → 平台 0xE4 触发
     * ======================================================================= */
    chk(iv_device_is_open(iv_agent_device(&g_agent)) == 1,
        "m03 dev_enable=1 -> iv_device opened");
    chk(iv_device_is_searching(iv_agent_device(&g_agent)) == 1,
        "m03 open() kicked off one search round (开机一次)");
    chk(iv_agent_device_count(&g_agent) == 0,
        "m03 no camera on the test network -> empty archive");
    chk(iv_agent_device_get(&g_agent, -1, &desc) == IV_ENOENT, "m03 get(-1) rejected");

    /* 0xE4 路由：只 stop 搜索 → 喂一帧 → 必须重新开始（没有别的路径能让它恢复） */
    iv_device_stop_search(iv_agent_device(&g_agent));
    chk(iv_device_is_searching(iv_agent_device(&g_agent)) == 0, "m03 search stopped by test");
    {
        uint8_t q[64];
        size_t  qn;

        qn = build_down(q, IV_PROTO_DEVTYPE_DEFAULT, 0x101u,
                        IV_PROTO_CMD_QUERY_IPC_IP, 1u, 1u, NULL, 0u);
        iv_report_feed_platform(&g_agent.rp, q, qn);
        (void)iv_agent_step(&g_agent, g_now);
    }
    chk(iv_device_is_searching(iv_agent_device(&g_agent)) == 1,
        "m03 platform 0xE4 routed to on_query_ipc -> search restarted");
    chk(g_agent.ipc_pending == 1u, "m03 0xE4 armed the delayed JSON reply");

    /* 0xE4 的清单必须是"搜完再发"：这里不依赖真网卡，直接把假档案塞进发现句柄，
     * 只验拼出来的 JSON（字段名/0 基/封套/crc/不截断）。口径来源：
     * `指令-通用版 20260623.xlsx`「查询摄像机信息」应答模板 ＋
     * 参考固件 `com_ipc_device_information()`。真链路收发由 `platcmd_e2e.py` C35 钉住。 */
    {
        iv_device_t *dv = iv_agent_device(&g_agent);
        char         js[512];
        int          jn;

        memset(dv->item, 0, sizeof dv->item);
        dv->item[0].brand   = IV_DEV_BRAND_HIKVISION;
        dv->item[0].ip_be   = htonl(0xC0A80240u); /* 192.168.2.64 */
        dv->item[0].mac[0]  = 0x28u;
        dv->item[0].mac[1]  = 0x57u;
        dv->item[0].mac[2]  = 0xbeu;
        dv->item[0].mac[3]  = 0x43u;
        dv->item[0].mac[4]  = 0x27u;
        dv->item[0].mac[5]  = 0x86u;
        (void)snprintf(dv->item[0].model, sizeof dv->item[0].model, "%s", "DS-2CD3T46");
        dv->item[1].brand   = IV_DEV_BRAND_DAHUA;
        dv->item[1].ip_be   = htonl(0xC0A80241u); /* 192.168.2.65 */
        (void)snprintf(dv->item[1].model, sizeof dv->item[1].model, "%s", "DH-IPC-HFW1433");
        dv->n_items = 2;

        jn = iv_agent_build_ipc_json(&g_agent, "00000001000000002", js, sizeof js);
        chk(jn > 0, "m03 ipc json built");
        chk((int)strlen(js) == jn, "m03 ipc json length matches strlen");
        chk(strstr(js, "\"num\":2") != NULL, "m03 ipc json num=2");
        /* **0 基**：平台按 ip0 起取（表里示例就是 ip0/ip1/ip2），
         * 发成 1 基 ⇒ 平台找 ip0 找不到，一路"0 台"（现场就是这症状）。 */
        chk(strstr(js, "\"ip0\":\"192.168.2.64\"") != NULL, "m03 ipc json ip0（0 基）");
        chk(strstr(js, "\"mac0\":\"28:57:be:43:27:86\"") != NULL, "m03 ipc json mac0");
        chk(strstr(js, "\"mod1\":\"DH-IPC-HFW1433\"") != NULL, "m03 ipc json mod1（0 基，第 2 台）");
        chk(strstr(js, "\"ip1\":\"192.168.2.65\"") != NULL, "m03 ipc json ip1 = 第 2 台");
        /* 品牌是**整数码**：0=海康 1=大华 2=宇视（不是英文词，也不是我们的枚举值） */
        chk(strstr(js, "\"bd0\":0") != NULL, "m03 ipc json bd0=0 海康（整数码）");
        chk(strstr(js, "\"bd1\":1") != NULL, "m03 ipc json bd1=1 大华（整数码）");
        chk(strstr(js, "\"bd0\":\"hikvision\"") == NULL, "m03 ipc json 品牌不是英文词");
        chk(strstr(js, "\"qn\":\"00000001000000002\"") != NULL,
            "m03 ipc json echoes the request QN");
        /* 封套要素：ver/devtype/tid/cmd/crc 一样不能少（少了平台整帧丢，症状同样是"0 台"） */
        chk(strstr(js, "\"ver\":\"11\"") != NULL, "m03 ipc json 带 ver");
        chk(strstr(js, "\"devtype\":\"") != NULL, "m03 ipc json 带 devtype");
        chk(strstr(js, "\"tid\":\"") != NULL, "m03 ipc json 带 tid");
        chk(strstr(js, "\"cmd\":\"E4\"") != NULL, "m03 ipc json 带 cmd=E4");
        chk(strstr(js, "\"crc\":\"") != NULL, "m03 ipc json 带 crc");
        chk(iv_agent_build_ipc_json(&g_agent, "0", js, 200) == IV_ERANGE,
            "m03 ipc json refuses to truncate (IV_ERANGE, 不发半截)");

        dv->n_items = 0; /* 归还：后面的用例不该看见这两台假摄像机 */
    }

    /* =======================================================================
     * m04 S4.3 SNMP：启动 / 在飞唯一 / 未配槽位不发包 / 无值不乱填
     * ======================================================================= */
    chk(iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_CAM) == IV_AGENT_SNMP_IDLE,
        "m04 CAM starts IDLE");
    chk(iv_agent_snmp_start(&g_agent, IV_AGENT_SNMP_CAM, g_now) == IV_OK,
        "m04 CAM start accepted");
    chk(iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_CAM) == IV_AGENT_SNMP_RUN,
        "m04 CAM now RUN");
    chk(iv_agent_snmp_start(&g_agent, IV_AGENT_SNMP_CAM, g_now) == IV_EBUSY,
        "m04 second start while RUN -> IV_EBUSY (one round at a time)");
    chk(iv_agent_snmp_start(&g_agent, 99, g_now) == IV_EINVAL, "m04 bad target rejected");
    chk(iv_agent_snmp_state(&g_agent, -1) == IV_EINVAL, "m04 bad target state rejected");

    {
        size_t before = g_req_n;

        chk(pump_until(usrv, IV_AGENT_SNMP_CAM, 200), "m04 CAM round completed");
        /* 4 个非空槽位 ⇒ 恰 4 条请求（**空槽位不发包**，否则计数会大于 4） */
        chk(g_req_n - before == 4u,
            "m04 CAM issued exactly 4 requests (empty slots send nothing)");
    }
    chk(g_agent.snmp_runs == 1u, "m04 one completed round counted");
    chk(g_agent.snmp_slot_noval == 6u,
        "m04 6 slots report no value (5 unconfigured + 1 noSuchObject)");

    /* 槽位名 ↔ 配置下标一一对应（错位是这里最典型的缺陷） */
    chk(iv_agent_snmp_get(&g_agent, IV_AGENT_SNMP_CAM, 0, &name, &oid, &nrows,
                          rows, IV_AGENT_SNMP_ROWS_MAX) == IV_OK,
        "m04 CAM slot0 readable");
    chk(name != NULL && strcmp(name, iv_snmp_cam_slots[0]) == 0,
        "m04 CAM slot0 name == spec table[0]");
    chk(nrows == 1 && rows[0].type == IV_SNMP_T_OCTETSTR &&
            strcmp(rows[0].str, "CAM-DESC") == 0,
        "m04 CAM slot0 scalar GET value");

    chk(iv_agent_snmp_get(&g_agent, IV_AGENT_SNMP_CAM, 2, &name, &oid, &nrows,
                          rows, IV_AGENT_SNMP_ROWS_MAX) == IV_OK,
        "m04 CAM slot2 readable");
    chk(name != NULL && strcmp(name, iv_snmp_cam_slots[2]) == 0, "m04 CAM slot2 name");
    chk(oid != NULL && strcmp(oid, ".1.3.6.1.4.1.39165.1.3.0") == 0,
        "m04 CAM slot2 keeps its configured OID (positions are not compacted)");
    chk(nrows == 1 && strcmp(rows[0].str, "CAM-NAME") == 0, "m04 CAM slot2 value");

    chk(iv_agent_snmp_get(&g_agent, IV_AGENT_SNMP_CAM, 6, &name, &oid, &nrows,
                          rows, IV_AGENT_SNMP_ROWS_MAX) == IV_OK,
        "m04 CAM slot6 readable");
    chk(name != NULL && strcmp(name, iv_snmp_cam_slots[6]) == 0, "m04 CAM slot6 name");
    chk(nrows == 1 && rows[0].type == IV_SNMP_T_INTEGER && rows[0].num == 42,
        "m04 CAM slot6 integer value 42");

    /* 未配槽位：不发包、无值 */
    chk(iv_agent_snmp_get(&g_agent, IV_AGENT_SNMP_CAM, 1, &name, &oid, &nrows,
                          rows, IV_AGENT_SNMP_ROWS_MAX) == IV_OK,
        "m04 CAM slot1 readable");
    chk(nrows == 0 && g_agent.snmp_job[IV_AGENT_SNMP_CAM].slot[1].rc == IV_ENOENT,
        "m04 CAM slot1 (unconfigured) -> no value");

    /* 字典未命中：回 noSuchObject ⇒ 无值，**不拿邻值乱填** */
    chk(iv_agent_snmp_get(&g_agent, IV_AGENT_SNMP_CAM, 8, &name, &oid, &nrows,
                          rows, IV_AGENT_SNMP_ROWS_MAX) == IV_OK,
        "m04 CAM slot8 readable");
    chk(nrows == 0, "m04 CAM slot8 (noSuchObject) -> no value, not a neighbour's");
    chk(g_agent.snmp_job[IV_AGENT_SNMP_CAM].slot[8].rc == IV_ENOENT,
        "m04 CAM slot8 rc == IV_ENOENT");

    chk(iv_agent_snmp_get(&g_agent, IV_AGENT_SNMP_CAM, 9, &name, &oid, &nrows,
                          rows, IV_AGENT_SNMP_ROWS_MAX) == IV_EINVAL,
        "m04 slot index out of range rejected");

    /* =======================================================================
     * m05 两个目标**各自独立**：可重开一轮，也可同时在飞
     * ======================================================================= */
    chk(iv_agent_snmp_start(&g_agent, IV_AGENT_SNMP_CAM, g_now) == IV_OK,
        "m05 CAM re-armed after DONE");
    chk(iv_agent_snmp_start(&g_agent, IV_AGENT_SNMP_SW, g_now) == IV_OK,
        "m05 a different target is accepted while CAM is RUN (per-target state)");
    chk(iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_CAM) == IV_AGENT_SNMP_RUN &&
            iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_SW) == IV_AGENT_SNMP_RUN,
        "m05 both targets RUN concurrently");
    chk(pump_until_both(usrv, 200), "m05 both rounds completed");
    chk(g_agent.snmp_runs == 3u, "m05 three rounds counted (1 + cam + sw)");

    /* =======================================================================
     * m06 S4.3 表列：GETNEXT 走表 + 子树边界停
     * ======================================================================= */
    {
        size_t before = g_req_n;

        chk(iv_agent_snmp_start(&g_agent, IV_AGENT_SNMP_SW, g_now) == IV_OK,
            "m06 SW re-armed");
        chk(pump_until(usrv, IV_AGENT_SNMP_SW, 200), "m06 SW round completed");
        /* 1 标量 + 表列 4 步（3 行 + 1 步走出子树） */
        chk(g_req_n - before == 5u, "m06 SW issued 5 requests (1 scalar + 4 getnext)");
    }
    chk(iv_agent_snmp_get(&g_agent, IV_AGENT_SNMP_SW, 0, &name, &oid, &nrows,
                          rows, IV_AGENT_SNMP_ROWS_MAX) == IV_OK,
        "m06 SW slot0 readable");
    chk(name != NULL && strcmp(name, iv_snmp_sw_slots[0]) == 0, "m06 SW slot0 name");
    chk(nrows == 1 && strcmp(rows[0].str, "SW-NAME") == 0, "m06 SW slot0 value");

    chk(iv_agent_snmp_get(&g_agent, IV_AGENT_SNMP_SW, 5, &name, &oid, &nrows,
                          rows, IV_AGENT_SNMP_ROWS_MAX) == IV_OK,
        "m06 SW slot5 readable");
    chk(name != NULL && strcmp(name, iv_snmp_sw_slots[5]) == 0, "m06 SW slot5 name");
    chk(nrows == 3, "m06 SW slot5 walk stopped at the subtree boundary (3 rows)");
    chk(nrows == 3 && rows[0].num == 1 && rows[1].num == 2 && rows[2].num == 1,
        "m06 SW slot5 walked values are the column's, in row order");

    chk(iv_agent_snmp_get(&g_agent, IV_AGENT_SNMP_SW, 1, &name, &oid, &nrows,
                          rows, IV_AGENT_SNMP_ROWS_MAX) == IV_OK,
        "m06 SW slot1 readable");
    chk(nrows == 0, "m06 SW slot1 (unconfigured) -> no value");

    /* =======================================================================
     * m07 平台 0xE7 路由（目标选择：DATA[0] < 7 → 摄像机；>= 7 → 交换机）
     * ======================================================================= */
    {
        uint8_t q[64];
        uint8_t sel_cam = 0x00u;
        uint8_t sel_sw  = 0x07u;
        size_t  qn;

        qn = build_down(q, IV_PROTO_DEVTYPE_DEFAULT, 0x101u,
                        IV_PROTO_CMD_QUERY_SNMP, 2u, 2u, &sel_cam, 1u);
        iv_report_feed_platform(&g_agent.rp, q, qn);
        (void)iv_agent_step(&g_agent, g_now);
        chk(iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_CAM) == IV_AGENT_SNMP_RUN,
            "m07 platform 0xE7 (sel 0) started the CAMERA collect");
        chk(pump_until(usrv, IV_AGENT_SNMP_CAM, 200), "m07 0xE7 cam round completed");

        qn = build_down(q, IV_PROTO_DEVTYPE_DEFAULT, 0x101u,
                        IV_PROTO_CMD_QUERY_SNMP, 3u, 3u, &sel_sw, 1u);
        iv_report_feed_platform(&g_agent.rp, q, qn);
        (void)iv_agent_step(&g_agent, g_now);
        chk(iv_agent_snmp_state(&g_agent, IV_AGENT_SNMP_SW) == IV_AGENT_SNMP_RUN,
            "m07 platform 0xE7 (sel 7) started the SWITCH collect");
        chk(pump_until(usrv, IV_AGENT_SNMP_SW, 200), "m07 0xE7 sw round completed");
    }

    /* =======================================================================
     * m09 S9 延伸：SNMP 结果落 control.db（装配层接线：on_snmp_done → iv_store）
     *   行集口径：job 覆盖全部已配槽位数（cam 9 + sw 6 = 15 行），未配/无应答
     *   槽位落 ok=0 行（S4.3"不乱填"）⇒ 读侧表现为 ENOENT。
     *   device 表：store 行数必须等于档案条数（每条新档案 upsert 一次，MAC
     *   主键去重后一一对应）。**不断言 ==0**：ifname 不存在时 if_nametoindex
     *   返回 0，AF_PACKET 套接字不绑定 ⇒ 测试机会收到宿主机真实网段流量，
     *   档案非空是环境产物而非接线缺陷（一致性才是接线的真不变量）。
     * ======================================================================= */
    {
        char    v[128];
        int64_t n = 0;

        chk(iv_store_snmp_value(tstore, "switch", iv_snmp_sw_slots[0],
                                v, sizeof v) == IV_OK &&
                strcmp(v, "SW-NAME") == 0,
            "m09 SW slot0 landed in snmp_latest (recent-value)");
        chk(iv_store_snmp_value(tstore, "camera", iv_snmp_cam_slots[6],
                                v, sizeof v) == IV_OK &&
                strcmp(v, "42") == 0,
            "m09 CAM slot6 integer landed as \"42\" (num path)");
        chk(iv_store_snmp_value(tstore, "switch", iv_snmp_sw_slots[5],
                                v, sizeof v) == IV_OK &&
                strcmp(v, "1") == 0,
            "m09 SW slot5 walk stores first row only (num=1)");
        chk(iv_store_snmp_value(tstore, "switch", iv_snmp_sw_slots[1],
                                v, sizeof v) == IV_ENOENT,
            "m09 SW slot1 (unconfigured, ok=0 row) reads back ENOENT");
        chk(iv_store_snmp_value(tstore, "camera", iv_snmp_cam_slots[8],
                                v, sizeof v) == IV_ENOENT,
            "m09 CAM slot8 (no response, ok=0) reads back ENOENT (不乱填)");
        chk(iv_store_rows(tstore, "snmp_latest", &n) == IV_OK && n == 15,
            "m09 snmp_latest has 15 rows (cam 9 + sw 6, all slots landed)");
        if (n != 15)
            printf("m09 DEBUG snmp_latest rows=%lld\n", (long long)n);
        chk(iv_store_rows(tstore, "device", &n) == IV_OK &&
                n == (int64_t)iv_agent_device_count(&g_agent),
            "m09 device rows == archive count (store mirrors archive)");
        if (n != (int64_t)iv_agent_device_count(&g_agent))
            printf("m09 DEBUG device rows=%lld archive=%d\n", (long long)n,
                   iv_agent_device_count(&g_agent));
    }

    /* =======================================================================
     * m08 收口
     * ======================================================================= */
    iv_agent_close(&g_agent);
    chk(iv_agent_is_open(&g_agent) == 0, "m08 agent closed");
    chk(iv_device_is_open(iv_agent_device(&g_agent)) == 0,
        "m08 device discovery closed (UDP + AF_PACKET released)");

    iv_store_close(tstore);
    iv_db_close(tdb);
    rm_tmpdir(dbdir);
    iv_taskpool_destroy(pool);
    (void)close(usrv);
    (void)close(master);
    rm_tmpdir(qdir);

    if (g_fail == 0)
        printf("test_agent_m4 passed (device discovery wiring + 0xE4 route + "
               "0xE4 delayed JSON; snmp "
               "collect through the taskpool: scalar GET / table GETNEXT / "
               "no-value slots; 0xE7 route; snmp results landed to control.db)\n");
    else
        printf("test_agent_m4 FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}

int main(void)
{
    return scenario();
}
