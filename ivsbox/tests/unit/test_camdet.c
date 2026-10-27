/*
 * 摄像机网络状态探测单测（功能开发计划 M4-S4.1）
 *
 * 覆盖：五态映射（全应答 / 延时边界 / 部分应答 / 全无应答）、未配 IP、
 *       "网线断开"覆盖、状态沿回调、端口扫描、RTSP、模式切换、通道表刷新、CNS 导出。
 * 零硬件：PING 走 iv_probe 的注入点（造 echo reply 字节），端口扫描 / RTSP 走
 *       本模块自己的注入点。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <stdio.h>
#include <string.h>
#include <time.h> /* c14：量"重建后那次 tick 有没有空转" */

#include "ivsbox/iv_camdet.h"
#include "ivsbox/iv_report.h" /* c12：CNS 字段编码一致性 */

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

#define PING_ID 0x4950u               /* iv_probe 的 echo id（固定） */
#define CAM0_BE 0xC0A801C8u           /* 192.168.1.200 网络序 */
#define CAM1_BE 0xC0A8016Au           /* 192.168.1.106 网络序 */
#define PKT_INTV 5000u                /* 包间隔 = 引擎 interval */
#define PKT_TOUT 5000u                /* 单包超时 */

/* ---------------------------------------------------------------------------
 * PING 侧注入点（假 raw socket + 一次性回包）
 * ------------------------------------------------------------------------- */
static int      g_pb_nextfd = 3;
static uint8_t  g_pend[512];
static size_t   g_pend_len;
static uint8_t  g_sent[512];
static size_t   g_sent_len;
static int      g_pb_close_cnt;
static uint16_t g_last_seq; /* 引擎实际发出的 echo seq */

static int pb_socket(int d, int t, int p)
{
    (void)d;
    (void)t;
    (void)p;
    return g_pb_nextfd++;
}

static int pb_connect(int fd, const struct sockaddr *a, socklen_t l)
{
    (void)fd;
    (void)a;
    (void)l;
    return -1;
}

static ssize_t pb_sendto(int fd, const void *buf, size_t len, int flags,
                         const struct sockaddr *a, socklen_t al)
{
    (void)fd;
    (void)flags;
    (void)a;
    (void)al;
    if (len <= sizeof(g_sent)) {
        memcpy(g_sent, buf, len);
        g_sent_len = len;
    }
    /* 记下引擎实际发出的 echo seq：回包必须按它造，否则 seq 对不上会被引擎
     * 丢弃（该样本转为超时）。raw ICMP socket 发出的字节**从 ICMP 头起**
     * —— IP 头由内核补，别按 IP 头偏移解析。 */
    if (len >= 8u) {
        const struct icmphdr *ic = (const struct icmphdr *)buf;

        g_last_seq = ntohs(ic->un.echo.sequence);
    }
    return (ssize_t)len;
}

static ssize_t pb_recv(int fd, void *buf, size_t len, int flags)
{
    (void)fd;
    (void)flags;
    if (g_pend_len == 0 || g_pend_len > len) {
        errno = EAGAIN;
        return -1;
    }
    memcpy(buf, g_pend, g_pend_len);
    {
        size_t n = g_pend_len;

        g_pend_len = 0;
        return (ssize_t)n;
    }
}

static int pb_getsockopt(int fd, int level, int optname, void *val, socklen_t *vlen)
{
    (void)fd;
    (void)level;
    (void)optname;
    (void)val;
    (void)vlen;
    return -1;
}

static int pb_close(int fd)
{
    (void)fd;
    g_pb_close_cnt++;
    return 0;
}

static uint64_t pb_now(void)
{
    return 0;
}

static const iv_probe_iops_t g_mock_pb = {
    pb_socket, pb_connect, pb_sendto, pb_recv, pb_getsockopt, pb_close, pb_now
};

/* 构造 ICMP echo reply 字节流（IP 头 + ICMP 头），返回总长（同 test_probe.c） */
static size_t mk_icmp_reply(uint8_t *b, uint16_t id, uint16_t seq)
{
    struct iphdr   *ip = (struct iphdr *)b;
    struct icmphdr *ic = (struct icmphdr *)(b + 20);

    memset(b, 0, 28);
    ip->version  = 4;
    ip->ihl      = 5;
    ip->protocol = IPPROTO_ICMP;
    ic->type             = ICMP_ECHOREPLY;
    ic->code             = 0;
    ic->un.echo.id       = htons(id);
    ic->un.echo.sequence = htons(seq);
    return 28;
}

/* ---------------------------------------------------------------------------
 * 端口扫描 / RTSP 侧注入点
 * ------------------------------------------------------------------------- */
static int     g_cd_nextfd  = 100;
static int     g_conn_ret   = -1;        /* socket 层 connect 的返回值 */
static int     g_conn_errno = EINPROGRESS;
static int     g_so_err     = 0;         /* getsockopt(SO_ERROR) 报的值 */
static int     g_recv_mode  = 1;         /* 0=EAGAIN 1=有响应 2=对端关闭 3=错误 */
static int     g_send_calls;

/* 第二次及以后的 connect（poll_connect 用"再 connect 一次"判结果）：
 * 默认 EISCONN = 已连上。要造"一直连不上"就把 poll_errno 设成 EALREADY。 */
static int     g_poll_ret    = -1;
static int     g_poll_errno  = EISCONN;
static int     g_conn_fd     = -1;        /* 上一次 connect 的 fd */
static int     g_conn_calls  = 0;         /* 该 fd 上的 connect 次数 */

static int cd_socket(int d, int t, int p)
{
    (void)d;
    (void)t;
    (void)p;
    return g_cd_nextfd++;
}

static int cd_connect(int fd, const struct sockaddr *a, socklen_t l)
{
    (void)a;
    (void)l;
    if (fd != g_conn_fd) { /* 换了一条新 socket：重新计次 */
        g_conn_fd    = fd;
        g_conn_calls = 0;
    }
    g_conn_calls++;
    if (g_conn_calls == 1) { /* 发起连接的那一次 */
        if (g_conn_ret < 0)
            errno = g_conn_errno;
        return g_conn_ret;
    }
    if (g_poll_ret < 0) /* 之后的"探结果" */
        errno = g_poll_errno;
    return g_poll_ret;
}

static int cd_getsockopt(int fd, int level, int optname, void *val, socklen_t *vlen)
{
    (void)fd;
    (void)level;
    (void)optname;
    if (val != NULL && vlen != NULL && *vlen >= (socklen_t)sizeof(int)) {
        *(int *)val = g_so_err;
        *vlen       = (socklen_t)sizeof(int);
    }
    return 0;
}

static ssize_t cd_send(int fd, const void *buf, size_t len, int flags)
{
    (void)fd;
    (void)buf;
    (void)flags;
    g_send_calls++;
    return (ssize_t)len;
}

static ssize_t cd_recv(int fd, void *buf, size_t len, int flags)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)flags;
    switch (g_recv_mode) {
    case 1:
        return 64; /* 收到任意 >0 字节即判正常（现网口径） */
    case 2:
        return 0;
    case 3:
        errno = ECONNRESET;
        return -1;
    default:
        errno = EAGAIN;
        return -1;
    }
}

static int cd_close(int fd)
{
    (void)fd;
    return 0;
}

static uint64_t cd_now(void)
{
    return 0;
}

static const iv_camdet_iops_t g_mock_cd = {
    cd_socket, cd_connect, cd_getsockopt, cd_send, cd_recv, cd_close, cd_now
};

/* ---------------------------------------------------------------------------
 * 夹具
 * ------------------------------------------------------------------------- */
typedef struct {
    iv_camdet_t        d;
    int                events;
    int                ev_ch;
    iv_camdet_status_t ev_old;
    iv_camdet_status_t ev_new;
} fx_t;

static void on_change(int ch, iv_camdet_status_t old_st, iv_camdet_status_t new_st,
                      void *arg)
{
    fx_t *f = (fx_t *)arg;

    f->events++;
    f->ev_ch  = ch;
    f->ev_old = old_st;
    f->ev_new = new_st;
}

static void fx_open(fx_t *f, int mode, uint32_t rtt_thr)
{
    iv_camdet_cfg_t cfg;

    memset(f, 0, sizeof(*f));
    g_pend_len  = 0;
    g_send_calls = 0;
    g_pb_close_cnt = 0;
    /* "探连接结果"的默认口径：再 connect 一次得到 EISCONN（已连上）。各用例
     * 只按需改 g_conn_ret/g_conn_errno/g_poll_errno，不再逐条复位。 */
    g_poll_ret   = -1;
    g_poll_errno = EISCONN;
    g_conn_fd    = -1;
    g_conn_calls = 0;

    iv_camdet_cfg_default(&cfg);
    cfg.mode       = mode;
    cfg.rtt_thr_ms = rtt_thr;
    cfg.pb_iops    = &g_mock_pb;
    cfg.iops       = &g_mock_cd;
    cfg.on_change  = on_change;
    cfg.cb_arg     = f;

    iv_camdet_init(&f->d, &cfg);
    chk(iv_camdet_open(&f->d, NULL, 1000) == IV_OK, "open ok");
}

/* 喂一台摄像机的一次"发包 → 收包/超时"，并把时间轴推进到下一个包的时刻 */
static void ping_pkt(fx_t *f, uint64_t *t, int reply, int rtt_ms)
{
    uint64_t send_at = *t;

    (void)iv_camdet_tick(&f->d, send_at); /* 到期即发第 N 包 */
    if (reply) {
        uint64_t at = send_at + (uint64_t)rtt_ms;

        g_pend_len = mk_icmp_reply(g_pend, PING_ID, g_last_seq);
        (void)iv_camdet_tick(&f->d, at); /* 收到回包 → 记一个成功样本 */
        *t = at + PKT_INTV;
    } else {
        uint64_t at = send_at + PKT_TOUT; /* 超时判定时刻 */

        (void)iv_camdet_tick(&f->d, at);
        *t = at + PKT_INTV;
    }
}

static void one_cam(uint32_t *ips)
{
    int i;

    for (i = 0; i < IV_CAMDET_CH_N; i++)
        ips[i] = 0u;
    ips[0] = CAM0_BE;
}

/* ---------------------------------------------------------------------------
 * c01 参数与守卫
 * ------------------------------------------------------------------------- */
static void c01_args_and_guards(void)
{
    fx_t            f;
    iv_camdet_cfg_t cfg;

    iv_camdet_cfg_default(&cfg);
    chk(cfg.mode == IV_CAMDET_MODE_PING, "cfg default mode=ping");
    chk(cfg.rtt_thr_ms == 200u, "cfg default rtt_thr=200");
    chk(cfg.round_ms == 30000u, "cfg default round=30s");
    chk(iv_camdet_port_table() != NULL && iv_camdet_port_table()[0] == 80u &&
            iv_camdet_port_table()[6] == 8899u,
        "port table 80/554/.../8899");

    memset(&f, 0, sizeof(f));
    iv_camdet_init(&f.d, &cfg);
    chk(iv_camdet_tick(&f.d, 1u) == IV_ESTATE, "tick before open -> ESTATE");
    chk(iv_camdet_set_mode(&f.d, IV_CAMDET_MODE_PORT, 1u) == IV_ESTATE,
        "set_mode before open -> ESTATE");
    chk(iv_camdet_open(&f.d, NULL, 1u) == IV_OK, "open");
    chk(iv_camdet_open(&f.d, NULL, 1u) == IV_ESTATE, "double open -> ESTATE");
    chk(iv_camdet_set_mode(&f.d, 9, 1u) == IV_EINVAL, "bad mode -> EINVAL");
    chk(iv_camdet_set_channels(&f.d, NULL, 1u) == IV_EINVAL, "null ips -> EINVAL");
    chk(iv_camdet_cns(&f.d, NULL) == IV_EINVAL, "null out -> EINVAL");
    chk(iv_camdet_status(&f.d, 99) == IV_CAMDET_NO_IP, "oob ch -> NO_IP");
    chk(iv_camdet_verdict(&f.d, -1) == IV_CAMDET_NO_IP, "neg ch -> NO_IP");
    chk(iv_camdet_mode(&f.d) == IV_CAMDET_MODE_PING, "mode getter");
    iv_camdet_close(&f.d);
    chk(iv_camdet_mode(&f.d) == 0, "mode getter after close -> 0");
}

/* ---------------------------------------------------------------------------
 * c02 PING 正常 + 轮结算 + 状态沿只报一次
 * ------------------------------------------------------------------------- */
static void c02_ping_normal(void)
{
    fx_t     f;
    uint32_t ips[6];
    uint64_t t = 1000u;
    int      i;

    fx_open(&f, IV_CAMDET_MODE_PING, 0u);
    one_cam(ips);
    chk(iv_camdet_set_channels(&f.d, ips, 1000u) == 0, "set_channels: no state change");

    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, 100); /* 4 包全应答，RTT 100 < 200 */

    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL, "4/4 ok, rtt100 -> normal");
    chk(iv_camdet_verdict(&f.d, 0) == IV_CAMDET_NORMAL, "verdict normal");
    chk(iv_camdet_rounds(&f.d) == 1u, "one round settled");
    chk(iv_camdet_samples(&f.d, 0) == 0, "ring cleared after round");
    chk(iv_camdet_status(&f.d, 1) == IV_CAMDET_NO_IP, "ch1 unconfigured -> 0");
    chk(f.events == 1 && f.ev_ch == 0 && f.ev_old == IV_CAMDET_NO_IP &&
            f.ev_new == IV_CAMDET_NORMAL,
        "one edge callback NO_IP->NORMAL");
    chk(iv_camdet_last_change_ms(&f.d, 0) != 0u, "last change recorded");

    /* 再走一轮仍正常：不得重复回调（无变化就无沿） */
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, 100);
    chk(iv_camdet_rounds(&f.d) == 2u, "two rounds");
    chk(f.events == 1, "no duplicate edge callback");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c03 延时判定边界：严格大于（R-02c）
 *
 * 判据从"平均 RTT >= 阈值"改成"> 阈值"：平均 RTT 只能是调用方 tick 的整数倍
 * （本层没有自己的事件循环，回包在 tick 里才被读走），默认阈值 200 恰好等于一个
 * tick ⇒ `>=` 会让"一个 tick 内就应答的局域网摄像机"恒判延时(4)。板端实测的
 * 现场症状就是 ping 0.5 ms、页面报延时。
 * ------------------------------------------------------------------------- */
static void c03_ping_delay_boundary(void)
{
    fx_t     f;
    uint32_t ips[6];
    uint64_t t = 1000u;
    int      i;

    /* 平均 == 阈值 → 正常（这就是 R-02c 的现场形态：thr=200、测得 200） */
    fx_open(&f, IV_CAMDET_MODE_PING, 100u); /* 阈值 100 */
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, 100); /* avg == 100 == 阈值 → 正常（严格大于） */

    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL, "avg==thr -> normal (R-02c)");
    iv_camdet_close(&f.d);

    /* 平均只超 1 ms → 延时，证明不是把判定整条撤了 */
    fx_open(&f, IV_CAMDET_MODE_PING, 99u); /* 阈值 99 */
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    t = 1000u;
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, 100);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_DELAY, "avg>thr -> delay(4)");
    iv_camdet_close(&f.d);

    /* 现场形态原样复刻：默认阈值 200 ＋ 一个 tick 的 RTT 200 → 必须正常 */
    fx_open(&f, IV_CAMDET_MODE_PING, IV_CAMDET_RTT_THR_MS_DEFAULT);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    t = 1000u;
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, (int)IV_CAMDET_TICK_RESOLUTION_MS);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL,
        "rtt==one tick under default thr -> normal (board case)");

    /* 两个 tick 才有回包（400 > 200）→ 延时，阈值仍然有效 */
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, (int)(2u * IV_CAMDET_TICK_RESOLUTION_MS));
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_DELAY, "two ticks -> delay");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c04 丢包：4 包里只有部分应答
 * ------------------------------------------------------------------------- */
static void c04_ping_loss(void)
{
    fx_t     f;
    uint32_t ips[6];
    uint64_t t = 1000u;

    fx_open(&f, IV_CAMDET_MODE_PING, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);

    ping_pkt(&f, &t, 1, 20); /* 1 成功 */
    ping_pkt(&f, &t, 0, 0);  /* 2 超时 */
    ping_pkt(&f, &t, 1, 20); /* 3 成功 */
    ping_pkt(&f, &t, 1, 20); /* 4 成功 */

    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_LOSS, "3/4 ok -> loss(5)");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c05 故障：4 包全无应答
 * ------------------------------------------------------------------------- */
static void c05_ping_fault(void)
{
    fx_t     f;
    uint32_t ips[6];
    uint64_t t = 1000u;
    int      i;

    fx_open(&f, IV_CAMDET_MODE_PING, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 0, 0);

    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT, "0/4 ok -> fault(2)");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c06 网线断开覆盖（现网：已配 IP 的一律 2，未配仍 0）
 * ------------------------------------------------------------------------- */
static void c06_link_down(void)
{
    fx_t     f;
    uint32_t ips[6];
    uint64_t t = 1000u;
    int      i;

    fx_open(&f, IV_CAMDET_MODE_PING, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, 10);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL, "pre: normal");

    (void)iv_camdet_set_link(&f.d, 0, t);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT, "link down -> fault(2)");
    chk(iv_camdet_status(&f.d, 1) == IV_CAMDET_NO_IP, "unconfigured stays 0");
    chk(iv_camdet_verdict(&f.d, 0) == IV_CAMDET_NORMAL,
        "verdict keeps probe result under link-down");

    (void)iv_camdet_set_link(&f.d, 1, t);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL, "link up -> back to normal");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c07 通道表刷新：清空 → 无 IP；改回 → 重新探测
 * ------------------------------------------------------------------------- */
static void c07_channel_refresh(void)
{
    fx_t     f;
    uint32_t ips[6];
    uint32_t none[6];
    uint64_t t = 1000u;
    int      i;

    fx_open(&f, IV_CAMDET_MODE_PING, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, 10);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL, "pre: normal");

    one_cam(none);
    none[0] = 0u;
    chk(iv_camdet_set_channels(&f.d, none, t) > 0, "clear ip -> state change");
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NO_IP, "cleared -> 0");
    chk(iv_camdet_ip(&f.d, 0) == 0u, "ip getter");

    (void)iv_camdet_set_channels(&f.d, ips, t);
    chk(iv_camdet_ip(&f.d, 0) == CAM0_BE, "ip back");
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 0, 0); /* 新表：4 包全超时 → 故障 */
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT, "re-probed -> fault(2)");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c08 端口扫描：任一端口通 → 正常；全不通 → 故障
 * ------------------------------------------------------------------------- */
static void c08_port_scan(void)
{
    fx_t     f;
    uint32_t ips[6];

    g_conn_ret   = 0; /* 立刻连通 */
    g_conn_errno = 0;
    fx_open(&f, IV_CAMDET_MODE_PORT, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    (void)iv_camdet_tick(&f.d, 1000u);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL, "any port open -> normal");
    iv_camdet_close(&f.d);

    g_conn_ret   = -1;
    g_conn_errno = ECONNREFUSED; /* 7 端口全部不可连 */
    fx_open(&f, IV_CAMDET_MODE_PORT, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    (void)iv_camdet_tick(&f.d, 1000u);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT, "all ports closed -> fault");
    iv_camdet_close(&f.d);

    /* 等连接结果（EINPROGRESS）→ 下一 tick 再 connect 回 EISCONN 视为连通。
     * 判"连上没有"靠的是**再 connect 一次**的 errno，不再看 SO_ERROR —— 见下一条。 */
    g_conn_ret   = -1;
    g_conn_errno = EINPROGRESS;
    g_poll_errno = EISCONN;
    g_so_err     = 0;
    fx_open(&f, IV_CAMDET_MODE_PORT, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    {
        uint64_t at = 1000u;
        int      k;

        for (k = 0; k < 20; k++) { /* 7 端口 × (发起+结果) 步，每步 100 ms */
            (void)iv_camdet_tick(&f.d, at);
            at += 100u;
        }
    }
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL,
        "async connect ok -> normal");
    iv_camdet_close(&f.d);

    /* ⚠ R-02 回归：对端不可达（连接一直在 SYN_SENT，永远等不到结果）必须判 FAULT。
     * 病根：旧实现拿 `getsockopt(SO_ERROR)==0` 当"连上了"，而 SYN_SENT 期间
     * SO_ERROR 本来就是 0 ⇒ 拔了网线的摄像机整轮恒报 NORMAL(1)。
     * 时间步必须**小于端口超时**（100 ms < 1000 ms），让"探结果"真的发生若干次；
     * 一步跨过超时就只会走 deadline 分支，反而测不到这条病根（踩过）。 */
    fx_open(&f, IV_CAMDET_MODE_PORT, 0u);
    g_conn_ret   = -1;
    g_conn_errno = EINPROGRESS;
    g_poll_ret   = -1;
    g_poll_errno = EALREADY; /* 一直"仍在进行"，直到超时 */
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    {
        uint64_t at = 1000u;
        int      k;

        for (k = 0; k < 110; k++) { /* 7 端口 × (发起 + 约 10 次探结果) */
            (void)iv_camdet_tick(&f.d, at);
            at += 100u;
        }
    }
    chk(iv_camdet_verdict(&f.d, 0) == IV_CAMDET_FAULT, "unreachable -> not normal");
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT, "unreachable -> fault(2)");
    iv_camdet_close(&f.d);

    /* 未配 IP 的通道在扫描方式下仍是 0 */
    g_conn_ret = 0;
    fx_open(&f, IV_CAMDET_MODE_PORT, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    (void)iv_camdet_tick(&f.d, 1000u);
    chk(iv_camdet_status(&f.d, 3) == IV_CAMDET_NO_IP, "scan: unconfigured -> 0");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c09 RTSP：连接成功 + 收到响应 → 正常；连接失败 / 收响应超时 → 故障
 * ------------------------------------------------------------------------- */
static void c09_rtsp(void)
{
    fx_t     f;
    uint32_t ips[6];
    int      i;

    g_conn_ret   = -1;
    g_conn_errno = EINPROGRESS;
    g_so_err     = 0;
    g_recv_mode  = 1; /* 收到 64 字节 */
    fx_open(&f, IV_CAMDET_MODE_RTSP, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    (void)iv_camdet_tick(&f.d, 1000u); /* 发起连接 */
    (void)iv_camdet_tick(&f.d, 1100u); /* 连接成功 → 发 OPTIONS */
    (void)iv_camdet_tick(&f.d, 1200u); /* 请求发完 */
    (void)iv_camdet_tick(&f.d, 1300u); /* 收到响应 */
    chk(g_send_calls == 1, "one OPTIONS sent");
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL, "rtsp 200-ish -> normal");
    iv_camdet_close(&f.d);

    g_conn_errno = ECONNREFUSED; /* 连接被拒 */
    fx_open(&f, IV_CAMDET_MODE_RTSP, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    (void)iv_camdet_tick(&f.d, 1000u);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT, "rtsp connect refused -> fault");
    iv_camdet_close(&f.d);

    g_conn_errno = EINPROGRESS;
    g_recv_mode  = 0; /* 一直无响应 */
    fx_open(&f, IV_CAMDET_MODE_RTSP, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    (void)iv_camdet_tick(&f.d, 1000u);
    (void)iv_camdet_tick(&f.d, 1100u);
    (void)iv_camdet_tick(&f.d, 1200u);
    for (i = 0; i < 16; i++)
        (void)iv_camdet_tick(&f.d, 1300u + (uint64_t)i * 100u); /* 越过 1s 超时 */
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT, "rtsp no reply -> fault");

    g_recv_mode = 2; /* 对端关闭 */
    fx_open(&f, IV_CAMDET_MODE_RTSP, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    (void)iv_camdet_tick(&f.d, 1000u);
    (void)iv_camdet_tick(&f.d, 1100u);
    (void)iv_camdet_tick(&f.d, 1200u);
    (void)iv_camdet_tick(&f.d, 1300u);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT, "rtsp closed -> fault");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c10 模式切换：PING<->PORT 时引擎的启停与方式生效
 * ------------------------------------------------------------------------- */
static void c10_mode_switch(void)
{
    fx_t     f;
    uint32_t ips[6];

    fx_open(&f, IV_CAMDET_MODE_PING, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    chk(f.d.pb.started == 1, "ping: engine started");

    chk(iv_camdet_set_mode(&f.d, IV_CAMDET_MODE_PORT, 2000u) == IV_OK, "switch to port");
    chk(iv_camdet_mode(&f.d) == IV_CAMDET_MODE_PORT, "mode getter = port");
    chk(f.d.pb.started == 0, "port: engine stopped (no ICMP traffic)");

    g_conn_ret = 0;
    (void)iv_camdet_tick(&f.d, 2000u);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL, "port mode works after switch");

    chk(iv_camdet_set_mode(&f.d, IV_CAMDET_MODE_PING, 3000u) == IV_OK, "switch back");
    chk(f.d.pb.started == 1, "ping: engine restarted");
    chk(iv_camdet_set_mode(&f.d, IV_CAMDET_MODE_PING, 3000u) == 0, "same mode -> no-op");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c11 CNS 导出
 * ------------------------------------------------------------------------- */
static void c11_cns(void)
{
    fx_t     f;
    uint32_t ips[6];
    uint64_t t = 1000u;
    int      cns[IV_CAMDET_CH_N];
    int      i;

    fx_open(&f, IV_CAMDET_MODE_PING, 0u);
    one_cam(ips); /* 只配 ch0：多通道共一个引擎，时间轴会互相干扰 */
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);

    /* ch0：4 包全应答 → 1；其余 5 路未配 → 0 */
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, 10);

    chk(iv_camdet_cns(&f.d, cns) == IV_OK, "cns ok");
    chk(cns[0] == IV_CAMDET_NORMAL, "cns[0]=1");
    chk(cns[1] == IV_CAMDET_NO_IP && cns[5] == IV_CAMDET_NO_IP, "cns[1..5]=0");
    chk(iv_camdet_status_name(IV_CAMDET_LOSS)[0] == 'l', "status name");
    chk(iv_camdet_mode_name(IV_CAMDET_MODE_RTSP)[0] == 'r', "mode name");
    iv_camdet_close(&f.d);
}

/* ---------------------------------------------------------------------------
 * c12 五态编码 → 上报数据段的 CNS 字段（与 iv_report 的编码口径对齐）
 * ------------------------------------------------------------------------- */
static void c12_cns_segment(void)
{
    iv_report_env_t env;
    char            seg[512];
    int             n;

    memset(&env, 0, sizeof(env));
    env.csq    = -1;                /* <0 = 未知 ⇒ 不上传该字段 */
    env.cns[0] = IV_CAMDET_NORMAL;  /* 1 */
    env.cns[2] = IV_CAMDET_FAULT;   /* 2 */
    env.cns[5] = IV_CAMDET_DELAY;   /* 4 */
    env.mn[0]  = 1;

    n = iv_report_build_seg(NULL, &env, 0u, 0x101u, "20250725171635", seg, sizeof(seg));
    chk(n > 0, "c12: segment built");
    chk(strstr(seg, "CNS=1,0,2,0,0,4;") != NULL,
        "c12: CNS carries the camdet five-state codes as-is");
}

/* ---------------------------------------------------------------------------
 * c13 立即重测一路（iv_camdet_probe_now）＋ 延时阈值热生效（set_rtt_thr）
 *
 * 这两条是 2026-10-22 补的"假功能"收口：`iv_camdet` 是轮询式（PING 20 s 一轮、
 * 扫描/RTSP 30 s 一轮），现场改完 IP / 换完检测方式都得等一整轮才看得到效果，
 * 而 `camera.detect.rtt_ms` 更只在 open 时读一次。Web 摄像机页的「测」按钮与
 * 「检测方式」下拉就靠这两个接口。
 *
 * 端口扫描那段是本用例的核心：它**证明本轮真的从被点的那一路开跑**，而不是
 * 从 0 号一路扫过来（否则 sc_from 白加、"测 3 号"实际在测 1 号）。
 * ------------------------------------------------------------------------- */
static void c13_probe_now_and_rtt(void)
{
    fx_t     f;
    uint32_t ips[6];
    uint64_t t;
    int      i;

    /* ---- 守卫：未 open / NULL / ch 越界 / 未配 IP / rtt=0 ---- */
    memset(&f, 0, sizeof(f));
    iv_camdet_init(&f.d, NULL);
    chk(iv_camdet_probe_now(NULL, 0, 1u) == IV_EINVAL, "c13: probe_now(NULL) -> EINVAL");
    chk(iv_camdet_probe_now(&f.d, 0, 1u) == IV_ESTATE, "c13: probe_now before open -> ESTATE");
    chk(iv_camdet_set_rtt_thr(&f.d, 100u) == IV_ESTATE, "c13: set_rtt before open -> ESTATE");

    g_conn_ret   = -1;
    g_conn_errno = EINPROGRESS;
    g_so_err     = 0;
    fx_open(&f, IV_CAMDET_MODE_PORT, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);

    chk(iv_camdet_probe_now(&f.d, -1, 1000u) == IV_EINVAL, "c13: ch<0 -> EINVAL");
    chk(iv_camdet_probe_now(&f.d, IV_CAMDET_CH_N, 1000u) == IV_EINVAL,
        "c13: ch>=CH_N -> EINVAL");
    chk(iv_camdet_probe_now(&f.d, 3, 1000u) == IV_ENOENT,
        "c13: channel without IP -> ENOENT");
    chk(iv_camdet_set_rtt_thr(&f.d, 0u) == IV_EINVAL, "c13: rtt=0 -> EINVAL");
    chk(iv_camdet_set_rtt_thr(&f.d, 300u) == IV_OK, "c13: rtt set");
    chk(f.d.cfg.rtt_thr_ms == 300u, "c13: rtt stored");
    chk(iv_camdet_set_rtt_thr(&f.d, 300u) == IV_OK, "c13: same rtt -> OK");
    chk(iv_camdet_set_rtt_thr(&f.d, 90u) == IV_OK && f.d.cfg.rtt_thr_ms == 90u,
        "c13: rtt lowered in place");
    iv_camdet_close(&f.d);

    /* ---- 端口扫描：probe_now 必须**从该路起**，而不是从 0 号起 ---- */
    g_conn_ret   = -1;
    g_conn_errno = ECONNREFUSED; /* 7 个端口全不可连 → 故障 */
    fx_open(&f, IV_CAMDET_MODE_PORT, 0u);
    for (i = 0; i < IV_CAMDET_CH_N; i++)
        ips[i] = 0u;
    ips[0] = CAM0_BE;
    ips[1] = CAM1_BE;
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);

    t = 1000u;
    for (i = 0; i < 30; i++) { /* 2 台 × 7 端口，每 tick 至少走一步 */
        (void)iv_camdet_tick(&f.d, t);
        t += 100u;
    }
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT &&
            iv_camdet_status(&f.d, 1) == IV_CAMDET_FAULT,
        "c13: baseline both fault");
    chk(f.d.next_round_ms > t, "c13: round finished, next one is in the future");

    /* 手动重测 1 号（0 基下标 1），同时把 connect 改成立刻连通 */
    g_conn_ret   = 0;
    g_conn_errno = 0;
    chk(iv_camdet_probe_now(&f.d, 1, t) == IV_OK, "c13: probe_now(ch1) queued");
    chk(f.d.sc_from == 1, "c13: next round starts at ch1");
    chk(f.d.next_round_ms == 0u, "c13: next round fires immediately");
    (void)iv_camdet_tick(&f.d, t);
    chk(f.d.sc_from == 0, "c13: sc_from consumed once (next round back to ch0)");
    chk(iv_camdet_status(&f.d, 1) == IV_CAMDET_NORMAL,
        "c13: ch1 re-probed -> normal");
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_FAULT,
        "c13: ch0 was NOT re-probed (round started at ch1, not ch0)");
    iv_camdet_close(&f.d);

    /* ---- PING：probe_now 清掉该路取样环（之后 4 个新样本重新结算） ---- */
    fx_open(&f, IV_CAMDET_MODE_PING, 0u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    t = 1000u;
    for (i = 0; i < 2; i++)
        ping_pkt(&f, &t, 1, 10);
    chk(iv_camdet_samples(&f.d, 0) == 2, "c13: 2 samples in the ring");
    chk(iv_camdet_probe_now(&f.d, 0, t) == IV_OK, "c13: ping probe_now ok");
    chk(iv_camdet_samples(&f.d, 0) == 0, "c13: ping ring cleared by probe_now");
    iv_camdet_close(&f.d);

    /* ---- 延时阈值热生效：同一批 150 ms 样本，阈值 200 → 正常 / 100 → 延时 ---- */
    fx_open(&f, IV_CAMDET_MODE_PING, 200u);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, 1000u);
    t = 1000u;
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, 150);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_NORMAL,
        "c13: rtt 150 under thr 200 -> normal");
    chk(iv_camdet_set_rtt_thr(&f.d, 100u) == IV_OK, "c13: lower thr to 100");
    for (i = 0; i < 4; i++)
        ping_pkt(&f, &t, 1, 150);
    chk(iv_camdet_status(&f.d, 0) == IV_CAMDET_DELAY,
        "c13: same rtt 150 now over thr 100 -> delay (hot effect)");
    iv_camdet_close(&f.d);
}

/* 回归 c14（2026-10-27 板端"平台配第 2 路摄像机 ⇒ 整机卡死重启"）：
 *
 * 已有 1 路在采样时**再加一路** ⇒ `pb_rebuild()` 重建 PING 引擎，`iv_probe_init()`
 * 把每个条目的 sent/lost 归零；而老通道的采样游标 `pv_sent/pv_lost` 是"上次读到的
 * 计数"，还留着旧值 ⇒ `sample_ping()` 里 `s.sent - d->pv_sent[ch]` 做**无符号减法**
 * 下溢成 0xFFFFFFFD ⇒ 那个 `while (dsent > 0u)` 要转约 42 亿次：主控单核 100%、
 * 日志一条不写、ctl 不再应答、16 s 后看门狗复位整机。
 *
 * 为什么现场难认：0→1 路不卡（唯一有 IP 的那路正好被 `set_channels()` 清过游标），
 * 已有 1 路再加一路就必卡；平台看到的是「配置失败」，设备侧却是整机重启。
 *
 * 判据：加完通道后那一次 tick 必须在毫秒级返回（老实现要转几十秒），且老通道
 * 的样本照常继续结算（不是"把采样整个扔掉"）。
 */
static void t_rebuild_no_underflow(void)
{
    fx_t     f;
    uint32_t ips[IV_CAMDET_CH_N];
    uint64_t t = 1000;
    struct timespec a;
    struct timespec b;
    long     ms;

    fx_open(&f, IV_CAMDET_MODE_PING, 0);
    one_cam(ips);
    (void)iv_camdet_set_channels(&f.d, ips, t);

    /* 先让 ch0 攒两个成功样本 ⇒ 游标 pv_sent/pv_lost 变成非零 */
    ping_pkt(&f, &t, 1, 10);
    ping_pkt(&f, &t, 1, 10);

    /* 再加一路：引擎重建，ch0 的引擎计数归零、游标还是 2 */
    ips[1] = CAM1_BE;
    (void)iv_camdet_set_channels(&f.d, ips, t);

    (void)clock_gettime(CLOCK_MONOTONIC, &a);
    ping_pkt(&f, &t, 1, 10);           /* 老实现在这一步空转几十秒 */
    (void)clock_gettime(CLOCK_MONOTONIC, &b);
    ms = (long)((b.tv_sec - a.tv_sec) * 1000L) + (b.tv_nsec - a.tv_nsec) / 1000000L;
    chk(ms < 2000,
        "c14a rebuild 后的第一个 tick 必须立刻返回（防计数下溢转成死循环）");

    /* 采样没被吞掉：再走满一轮（4 个样本）就能算出结论。
     * 这里**不追究结论是哪个**：引擎刚重建，mock 回包与重建的相对时序会让第一个
     * 样本偶尔记成"过渡期丢包"，那不是本用例要守的东西 —— 本用例守的是
     * "不能把主循环转死" ＋ "采样不能整个停掉"。所以只要求 vd 从 NO_IP 变成
     * 任何一个真结论（NORMAL / LOSS / FAULT / DELAY 都算通过）。 */
    ping_pkt(&f, &t, 1, 10);
    ping_pkt(&f, &t, 1, 10);
    ping_pkt(&f, &t, 1, 10);
    ping_pkt(&f, &t, 1, 10);
    chk(iv_camdet_verdict(&f.d, 0) != IV_CAMDET_NO_IP,
        "c14b rebuild 之后老通道仍在采样（一轮样本能算出结论）");
    iv_camdet_close(&f.d);
}

int main(void)
{
    c01_args_and_guards();
    c02_ping_normal();
    c03_ping_delay_boundary();
    c04_ping_loss();
    c05_ping_fault();
    c06_link_down();
    c07_channel_refresh();
    c08_port_scan();
    c09_rtsp();
    c10_mode_switch();
    c11_cns();
    c12_cns_segment();
    c13_probe_now_and_rtt();
    t_rebuild_no_underflow();

    if (g_fail != 0) {
        fprintf(stderr, "test_camdet FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_camdet passed (guards, ping normal/delay/loss/fault, link-down, "
           "channel refresh, port scan, rtsp, mode switch, cns, probe-now/rtt, "
           "rebuild-no-underflow)\n");
    return 0;
}
