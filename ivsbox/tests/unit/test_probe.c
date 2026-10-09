/*
 * iv_probe 单测（libivmodules，M3-S3.2）
 *
 * ============================ 测法：注入 iops，零真实网络 ============================
 * 全部 socket 操作经 `iv_probe_iops_t` 注入（见 iv_probe.h），因此**不需要
 * CAP_NET_RAW、不需要真网络、不需要 root**：ICMP 回包用构造的 rtnetlink 式字节
 * 喂进 mock recv，TCP 结果用 mock connect/getsockopt 控制。任何环境结果一致。
 *
 * ============================ 反向证伪点（每条都对应一个真缺陷） ============================
 *   c03 连续失败达 fail_n 才 DOWN、连续成功达 ok_n 才 UP —— 去掉迟滞会抖动；
 *   c04 avg RTT 超阈值必判 DEGRADED —— 丢这条判定，劣化永远看不见；
 *   c05 绑定接口链路 down 立即判死 —— 若还要等计数，拔网线不是秒级可见；
 *   c05 链路恢复清 LINK_DOWN 覆盖 —— 不清则链路恢复后目标永远挂 DOWN；
 *   c06 icmp_disabled 时 ICMP 项不判死 —— 判死则无 CAP 的板子整个网络判黑；
 *   c08 ICMP 回包按 (id,seq) 匹配 —— 不匹配会把别人的回包算成自己的成功；
 *   c09 TCP 立即失败要计失败 —— 吞掉则拒绝服务永远不算失败；
 *   c10 超时以调用方 now 为准 —— 若自己取时，单测无法断言时间轴；
 *   c12 ICMP 报文校验和字节序 —— 漏 htons 则真内核丢包、回包全丢（联调实证）。
 *
 * 临时产物：无（全部内存内构造）。AGENTS.md 规则 6。
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>

#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * mock iops
 * ------------------------------------------------------------------------- */
static int      g_nextfd       = 3;   /* 每次 socket() 递增发号 */
static int      g_sock_errno   = 0;   /* !=0 时，仅 SOCK_RAW 的 socket() 失败（模拟无 CAP_NET_RAW） */
static int      g_connect_ret  = -1;
static int      g_connect_errno = EINPROGRESS;
static int      g_soerror      = 0;   /* getsockopt(SO_ERROR) 报的值 */
static ssize_t  g_sendto_ret   = 0;   /* <=0 视作失败（0 = 用包长） */
static int      g_sendto_errno = 0;
static int      g_close_count  = 0;
static uint64_t g_now          = 1000;

/* 待喂入的回包（一次性）：g_pend_len>0 时 recv 返回它，之后 EAGAIN */
static uint8_t  g_pend[512];
static size_t   g_pend_len     = 0;

/* 最近一次 sendto 发出的原始字节（用于字节级校验 ICMP 报文，尤其校验和字节序） */
static uint8_t  g_sent[512];
static size_t   g_sent_len     = 0;

static int mk_socket(int d, int t, int p)
{
    (void)d;
    (void)p;
    /* g_sock_errno 只作用于 raw socket：模拟"无 CAP_NET_RAW 时普通 socket 仍可用"。
     * 注意 SOCK_STREAM=1 / SOCK_RAW=3 是枚举值不是位标志，必须按低位比较，
     * 不能写成 (t & SOCK_RAW) != 0 —— 那样流式 socket 也会被误判成 raw。 */
    if (g_sock_errno != 0 && (t & 0x0f) == SOCK_RAW) {
        errno = g_sock_errno;
        return -1;
    }
    return g_nextfd++;
}

static int mk_connect(int fd, const struct sockaddr *a, socklen_t l)
{
    (void)fd;
    (void)a;
    (void)l;
    if (g_connect_ret < 0)
        errno = g_connect_errno;
    return g_connect_ret;
}

static ssize_t mk_sendto(int fd, const void *buf, size_t len, int flags,
                         const struct sockaddr *a, socklen_t al)
{
    (void)fd;
    (void)flags;
    (void)a;
    (void)al;
    if (g_sendto_errno != 0) {
        errno = g_sendto_errno;
        return -1;
    }
    if (buf != NULL && len <= sizeof(g_sent)) {
        memcpy(g_sent, buf, len); /* 留底：供字节级校验 */
        g_sent_len = len;
    }
    return g_sendto_ret > 0 ? g_sendto_ret : (ssize_t)len;
}

static ssize_t mk_recv(int fd, void *buf, size_t len, int flags)
{
    (void)fd;
    (void)flags;
    if (g_pend_len == 0) {
        errno = EAGAIN;
        return -1;
    }
    if (g_pend_len > len)
        return -1;
    memcpy(buf, g_pend, g_pend_len);
    {
        size_t n = g_pend_len;

        g_pend_len = 0;
        return (ssize_t)n;
    }
}

static int mk_getsockopt(int fd, int level, int optname, void *val, socklen_t *vlen)
{
    (void)fd;
    (void)level;
    (void)optname;
    if (vlen != NULL && *vlen >= (socklen_t)sizeof(int)) {
        *(int *)val = g_soerror;
        *vlen = (socklen_t)sizeof(int);
    }
    return 0;
}

static int mk_close(int fd)
{
    (void)fd;
    g_close_count++;
    return 0;
}

static uint64_t mk_now(void)
{
    return g_now;
}

static const iv_probe_iops_t g_mock = {
    mk_socket, mk_connect, mk_sendto, mk_recv, mk_getsockopt, mk_close, mk_now
};

static iv_probe_cfg_t mk_cfg(void)
{
    iv_probe_cfg_t c;

    iv_probe_cfg_default(&c);
    c.iops = &g_mock;
    return c;
}

static void mock_reset(void)
{
    g_nextfd        = 3;
    g_sock_errno    = 0;
    g_connect_ret   = -1;
    g_connect_errno = EINPROGRESS;
    g_soerror       = 0;
    g_sendto_ret    = 0;
    g_sendto_errno  = 0;
    g_close_count   = 0;
    g_now           = 1000;
    g_pend_len      = 0;
    g_sent_len      = 0;
}

/* 构造 ICMP echo reply 字节流（IP 头 + ICMP 头），返回总长 */
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

#define DST 0x0102A8C0u /* 192.168.2.1 网络序（值本身不影响逻辑） */

/* ---------------------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------------------- */
static void c01_args_and_guards(void)
{
    iv_probe_t     p;
    iv_probe_stat_t st;

    chk(iv_probe_init(NULL, NULL) == IV_EINVAL, "c01 init(NULL)");
    chk(iv_probe_init(&p, NULL) == IV_OK, "c01 init default");
    chk(iv_probe_count(&p) == 0, "c01 empty");
    chk(iv_probe_count(NULL) == 0, "c01 count(NULL)");

    chk(iv_probe_add_link(&p, 0, "bad") == IV_EINVAL, "c01 add_link(ifindex=0)");
    chk(iv_probe_add_tcp(&p, DST, 0, 0, "bad") == IV_EINVAL, "c01 add_tcp(port=0)");

    chk(iv_probe_add_tcp(&p, DST, 80, 0, "t1") == 0, "c01 add_tcp idx0");
    chk(iv_probe_add_tcp(&p, DST, 80, 0, "dup") == IV_EEXIST, "c01 dup tcp");
    chk(iv_probe_add_tcp(&p, DST, 443, 0, "t2") == 1, "c01 add_tcp idx1");

    /* 未 start 就 tick => ESTATE */
    chk(iv_probe_tick(&p, g_now) == IV_ESTATE, "c01 tick before start");

    chk(iv_probe_stat_get(&p, 99, &st) == IV_EINVAL, "c01 stat idx oob");
    chk(iv_probe_state(&p, 99) == IV_PROBE_UNKNOWN, "c01 state idx oob");
    chk(iv_probe_note_result(&p, 99, 1, 1, g_now) == IV_EINVAL, "c01 note idx oob");

    /* 填满项表 */
    {
        int i;

        for (i = 2; i < IV_PROBE_MAX_ITEMS; i++)
            chk(iv_probe_add_tcp(&p, DST, (uint16_t)(1000 + i), 0, "f") == i,
                "c01 fill");
        chk(iv_probe_add_tcp(&p, DST, 9999, 0, "over") == IV_EFULL, "c01 full");
    }
}

static void c02_stat_snapshot(void)
{
    iv_probe_t      p;
    iv_probe_cfg_t  c = mk_cfg();
    iv_probe_stat_t st;

    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_app(&p, 0, "app");

    (void)iv_probe_note_result(&p, 0, 1, 100, 2000);
    (void)iv_probe_note_result(&p, 0, 1, 200, 3000);
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.state == IV_PROBE_UP, "c02 two ok -> UP (ok_n=2)");
    chk(st.avg_rtt_ms == 150, "c02 avg rtt");
    chk(st.last_rtt_ms == 200, "c02 last rtt");
    chk(st.last_ok_ms == 3000, "c02 last ok time");
    chk(st.ok_streak == 2, "c02 ok streak");
    chk(st.sent == 2 && st.lost == 0, "c02 sent/lost");

    /* 三次失败 -> DOWN，丢包率与 streak 同步 */
    (void)iv_probe_note_result(&p, 0, 0, 0, 4000);
    (void)iv_probe_note_result(&p, 0, 0, 0, 5000);
    (void)iv_probe_note_result(&p, 0, 0, 0, 6000);
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.state == IV_PROBE_DOWN, "c02 three fail -> DOWN (fail_n=3)");
    chk(st.fail_streak == 3 && st.ok_streak == 0, "c02 fail streak");
    chk(st.sent == 5 && st.lost == 3, "c02 sent/lost after fails");
    /* 窗口 5 样本，2 成功 => 丢包 600‰ */
    chk(st.loss_permille == 600, "c02 loss permille");
    chk(st.last_ok_ms == 3000, "c02 last ok unchanged after fails");
}

static void c03_hysteresis(void)
{
    iv_probe_t p;
    iv_probe_cfg_t c = mk_cfg(); /* fail_n=3 ok_n=2 */

    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_app(&p, 0, "app");

    /* 不足 ok_n：仍 UNKNOWN（不能一见成功就 UP） */
    (void)iv_probe_note_result(&p, 0, 1, 10, 100);
    chk(iv_probe_state(&p, 0) == IV_PROBE_UNKNOWN, "c03 1 ok < ok_n -> UNKNOWN");
    (void)iv_probe_note_result(&p, 0, 1, 10, 200);
    chk(iv_probe_state(&p, 0) == IV_PROBE_UP, "c03 2 ok -> UP");

    /* 不足 fail_n：保持 UP（迟滞，不抖动） */
    (void)iv_probe_note_result(&p, 0, 0, 0, 300);
    (void)iv_probe_note_result(&p, 0, 0, 0, 400);
    chk(iv_probe_state(&p, 0) == IV_PROBE_UP, "c03 2 fail < fail_n -> stay UP");
    (void)iv_probe_note_result(&p, 0, 0, 0, 500);
    chk(iv_probe_state(&p, 0) == IV_PROBE_DOWN, "c03 3 fail -> DOWN");
}

static void c04_degrade(void)
{
    iv_probe_t     p;
    iv_probe_cfg_t c = mk_cfg();
    iv_probe_stat_t st;

    c.degrade_rtt_ms        = 100;
    c.degrade_loss_permille = 0; /* 只测 RTT 维度 */
    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_app(&p, 0, "app");

    (void)iv_probe_note_result(&p, 0, 1, 50, 100);
    (void)iv_probe_note_result(&p, 0, 1, 50, 200);
    chk(iv_probe_state(&p, 0) == IV_PROBE_UP, "c04 fast rtt -> UP");

    /* 拉高 RTT：avg 超阈值 -> DEGRADED（仍算可用） */
    (void)iv_probe_note_result(&p, 0, 1, 300, 300);
    (void)iv_probe_note_result(&p, 0, 1, 300, 400);
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.avg_rtt_ms > 100, "c04 avg risen");
    chk(st.state == IV_PROBE_DEGRADED, "c04 high rtt -> DEGRADED");
}

static void c05_link_event(void)
{
    iv_probe_t p;
    iv_probe_cfg_t c = mk_cfg();
    int i_link, i_tcp;

    (void)iv_probe_init(&p, &c);
    i_link = iv_probe_add_link(&p, 7, "wan0");
    i_tcp  = iv_probe_add_tcp(&p, DST, 80, 7, "svc-on-wan0");
    chk(i_link == 0 && i_tcp == 1, "c05 idx");

    /* 先让 TCP 项判 UP */
    (void)iv_probe_note_result(&p, i_tcp, 1, 20, 100);
    (void)iv_probe_note_result(&p, i_tcp, 1, 20, 200);
    chk(iv_probe_state(&p, i_tcp) == IV_PROBE_UP, "c05 tcp up");

    /* 链路断：LINK 项与绑定项**立即** DOWN（不等计数） */
    chk(iv_probe_set_link(&p, 7, 0, 300) >= 0, "c05 set_link down rc");
    chk(iv_probe_state(&p, i_link) == IV_PROBE_DOWN, "c05 LINK item down");
    chk(iv_probe_state(&p, i_tcp) == IV_PROBE_DOWN, "c05 bound item down at once");
    {
        iv_probe_stat_t st;

        (void)iv_probe_stat_get(&p, i_tcp, &st);
        chk(st.reason == IV_PROBE_R_LINK_DOWN, "c05 reason link_down");
    }

    /* 链路恢复：清覆盖，回到按统计判（此前 UP -> 立即复 UP） */
    (void)iv_probe_set_link(&p, 7, 1, 400);
    chk(iv_probe_state(&p, i_link) == IV_PROBE_UP, "c05 LINK item up");
    chk(iv_probe_state(&p, i_tcp) == IV_PROBE_UP, "c05 bound item back up");

    /* 从未探过的绑定项：链路恢复后应回 UNKNOWN，而不是一直挂 DOWN */
    {
        int i2 = iv_probe_add_tcp(&p, DST, 8080, 7, "fresh");

        (void)iv_probe_set_link(&p, 7, 0, 500);
        chk(iv_probe_state(&p, i2) == IV_PROBE_DOWN, "c05 fresh down by link");
        (void)iv_probe_set_link(&p, 7, 1, 600);
        chk(iv_probe_state(&p, i2) == IV_PROBE_UNKNOWN, "c05 fresh -> UNKNOWN on recover");
    }
}

static void c06_icmp_disabled(void)
{
    iv_probe_t p;
    iv_probe_cfg_t c = mk_cfg();
    int i_icmp, i_tcp;

    mock_reset();
    g_sock_errno = EPERM; /* raw socket 被拒 */
    (void)iv_probe_init(&p, &c);
    i_icmp = iv_probe_add_icmp(&p, DST, 0, "icmp");
    i_tcp  = iv_probe_add_tcp(&p, DST, 80, 0, "tcp");
    chk(iv_probe_start(&p, g_now) == IV_OK, "c06 start");
    chk(iv_probe_icmp_available(&p) == 0, "c06 icmp disabled flag");

    (void)iv_probe_tick(&p, g_now);
    chk(iv_probe_state(&p, i_icmp) == IV_PROBE_UNKNOWN, "c06 icmp not judged DOWN");

    /* ICMP 被禁，但 TCP 成功仍判可用 */
    (void)iv_probe_note_result(&p, i_tcp, 1, 30, 2000);
    (void)iv_probe_note_result(&p, i_tcp, 1, 30, 3000);
    chk(iv_probe_state(&p, i_tcp) == IV_PROBE_UP, "c06 tcp still UP w/o icmp");
    (void)iv_probe_stop(&p);
}

static void c07_tick_timeout(void)
{
    iv_probe_t p;
    iv_probe_cfg_t c = mk_cfg();
    int i;

    c.timeout_ms  = 1000;
    c.interval_ms = 500;
    mock_reset();
    (void)iv_probe_init(&p, &c);
    i = iv_probe_add_tcp(&p, DST, 80, 0, "tcp");
    (void)iv_probe_start(&p, 1000);

    (void)iv_probe_tick(&p, 1000); /* 发起，connect EINPROGRESS */
    chk(iv_probe_fd_count(&p) == 1, "c07 one inflight fd");

    (void)iv_probe_tick(&p, 1500); /* 未超时 */
    {
        iv_probe_stat_t st;

        (void)iv_probe_stat_get(&p, i, &st);
        chk(st.sent == 0, "c07 no result yet");
    }
    /* 超时：判失败并关闭 socket */
    (void)iv_probe_tick(&p, 2000);
    {
        iv_probe_stat_t st;

        (void)iv_probe_stat_get(&p, i, &st);
        chk(st.sent == 1 && st.lost == 1, "c07 timeout counts as lost");
        chk(st.reason == IV_PROBE_R_TIMEOUT, "c07 reason timeout");
    }
    chk(g_close_count >= 1, "c07 socket closed on timeout");
    chk(iv_probe_fd_count(&p) == 0, "c07 no inflight after timeout");

    /* 连超 fail_n 次 -> DOWN（每轮推进 interval+timeout 保证再次到期） */
    g_now = 2000;
    (void)iv_probe_tick(&p, 2000 + 500);  /* 发起 */
    (void)iv_probe_tick(&p, 2000 + 500 + 1000); /* 超时 */
    (void)iv_probe_tick(&p, 2000 + 500 + 1000 + 500); /* 发起 */
    (void)iv_probe_tick(&p, 2000 + 500 + 1000 + 500 + 1000); /* 超时 */
    chk(iv_probe_state(&p, i) == IV_PROBE_DOWN, "c07 three timeouts -> DOWN");
}

static void c08_icmp_reply(void)
{
    iv_probe_t p;
    iv_probe_cfg_t c = mk_cfg();
    int i;

    mock_reset();
    (void)iv_probe_init(&p, &c);
    i = iv_probe_add_icmp(&p, DST, 0, "icmp");
    (void)iv_probe_start(&p, 5000);
    chk(iv_probe_fd_count(&p) == 1, "c08 icmp fd");

    (void)iv_probe_tick(&p, 5000); /* 发 echo，seq=1 */
    chk(iv_probe_fd_count(&p) == 1, "c08 still one fd (shared)");

    /* 匹配的 reply */
    g_pend_len = mk_icmp_reply(g_pend, 0x4950u, 1u);
    chk(iv_probe_on_readable(&p, iv_probe_fd_at(&p, 0), 5050) == 1, "c08 reply handled");
    {
        iv_probe_stat_t st;

        (void)iv_probe_stat_get(&p, i, &st);
        chk(st.sent == 1 && st.lost == 0, "c08 reply success");
        chk(st.last_rtt_ms == 50, "c08 rtt from now delta");
    }
    (void)iv_probe_note_result(&p, i, 1, 50, 5060); /* 凑 ok_n=2 */
    chk(iv_probe_state(&p, i) == IV_PROBE_UP, "c08 UP");

    /* 不匹配的 reply（别的 id）：不得计入 */
    (void)iv_probe_tick(&p, 6000);
    g_pend_len = mk_icmp_reply(g_pend, 0x1234u, 2u);
    chk(iv_probe_on_readable(&p, iv_probe_fd_at(&p, 0), 6050) == 0, "c08 foreign id ignored");

    /* 不认识的 fd => ESTATE */
    chk(iv_probe_on_readable(&p, 999, 6050) == IV_ESTATE, "c08 unknown fd");
    (void)iv_probe_stop(&p);
}

static void c09_tcp_connect_result(void)
{
    iv_probe_t p;
    iv_probe_cfg_t c = mk_cfg();
    int i;

    /* 成功路径 */
    mock_reset();
    (void)iv_probe_init(&p, &c);
    i = iv_probe_add_tcp(&p, DST, 80, 0, "tcp");
    (void)iv_probe_start(&p, 100);
    (void)iv_probe_tick(&p, 100);           /* connect EINPROGRESS */
    g_soerror = 0;
    chk(iv_probe_on_writable(&p, iv_probe_fd_at(&p, 0), 130) == 1, "c09 writable handled");
    {
        iv_probe_stat_t st;

        (void)iv_probe_stat_get(&p, i, &st);
        chk(st.last_rtt_ms == 30, "c09 connect rtt");
    }

    /* 立即失败（ECONNREFUSED）：要计失败 */
    {
        iv_probe_t p2;
        int        j;

        mock_reset();
        g_connect_ret   = -1;
        g_connect_errno = ECONNREFUSED;
        (void)iv_probe_init(&p2, &c);
        j = iv_probe_add_tcp(&p2, DST, 81, 0, "tcp2");
        (void)iv_probe_start(&p2, 100);
        (void)iv_probe_tick(&p2, 100); /* connect 立即 -1 */
        {
            iv_probe_stat_t st;

            (void)iv_probe_stat_get(&p2, j, &st);
            chk(st.sent == 1 && st.lost == 1, "c09 immediate fail counted");
            chk(st.reason == IV_PROBE_R_REFUSED, "c09 refused reason");
        }
        /* connect 立即成功（同机）：也要计成功 */
        mock_reset();
        g_connect_ret = 0;
        (void)iv_probe_init(&p2, &c);
        j = iv_probe_add_tcp(&p2, DST, 82, 0, "tcp3");
        (void)iv_probe_start(&p2, 100);
        (void)iv_probe_tick(&p2, 100);
        {
            iv_probe_stat_t st;

            (void)iv_probe_stat_get(&p2, j, &st);
            chk(st.sent == 1 && st.lost == 0, "c09 immediate success counted");
        }
    }
    (void)iv_probe_stop(&p);
}

static void c10_now_is_caller_clock(void)
{
    iv_probe_t p;
    iv_probe_cfg_t c = mk_cfg();
    int i;

    c.timeout_ms = 500;
    mock_reset();
    (void)iv_probe_init(&p, &c);
    i = iv_probe_add_tcp(&p, DST, 80, 0, "tcp");
    (void)iv_probe_start(&p, 1u << 30); /* 大起点，验证不依赖引擎自取时 */
    (void)iv_probe_tick(&p, 1u << 30);
    chk(iv_probe_fd_count(&p) == 1, "c10 inflight");

    (void)iv_probe_tick(&p, (1u << 30) + 499); /* 未到 500 */
    {
        iv_probe_stat_t st;

        (void)iv_probe_stat_get(&p, i, &st);
        chk(st.sent == 0, "c10 not yet timeout at 499ms");
    }
    (void)iv_probe_tick(&p, (1u << 30) + 500); /* 到点 */
    {
        iv_probe_stat_t st;

        (void)iv_probe_stat_get(&p, i, &st);
        chk(st.reason == IV_PROBE_R_TIMEOUT, "c10 timeout exactly at 500ms");
    }
    (void)iv_probe_stop(&p);
}

static void c11_lifecycle_guards(void)
{
    iv_probe_t p;
    iv_probe_cfg_t c = mk_cfg();

    mock_reset();
    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_tcp(&p, DST, 80, 0, "tcp");

    chk(iv_probe_stop(&p) == IV_ESTATE, "c11 stop before start");
    chk(iv_probe_start(&p, 1) == IV_OK, "c11 start");
    chk(iv_probe_start(&p, 1) == IV_ESTATE, "c11 double start");
    chk(iv_probe_stop(&p) == IV_OK, "c11 stop");
    chk(iv_probe_fd_count(&p) == 0, "c11 no fd after stop");
    chk(iv_probe_start(&p, 2) == IV_OK, "c11 restart");
    (void)iv_probe_stop(&p);

    chk(iv_probe_state_name(IV_PROBE_UP) != NULL, "c11 state name");
    chk(iv_probe_reason_name(IV_PROBE_R_TIMEOUT) != NULL, "c11 reason name");
    (void)iv_probe_reset(&p);
    chk(iv_probe_count(&p) == 0, "c11 reset clears items");
}

/* 按 16 位大端字求 ones-complement 和（与主机端序无关）。 */
static uint16_t pkt_ones_sum(const uint8_t *b, size_t n)
{
    uint32_t s = 0;

    while (n > 1u) {
        s += (uint32_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
        b += 2;
        n -= 2u;
    }
    if (n == 1u)
        s += (uint32_t)((uint16_t)b[0] << 8);
    while ((s >> 16) != 0u)
        s = (s & 0xffffu) + (s >> 16);
    return (uint16_t)s;
}

/* c12：字节级校验引擎**发出**的 ICMP echo 报文。
 * 专门钉住「校验和字节序」——c08 只解析回包、mock sendto 也不看内容，曾因此让
 * 一个漏写的 htons 逃到真实内核联调才暴露（内核以 InCsumErrors 丢包、回包全丢）。
 * 判据与端序无关：含校验和字段在内的整包按 16 位大端字求和，合法校验和必使
 * ones-complement 和为全 1（0xFFFF）。**只看 seq=1 有区分力盲区**（其校验和恰为
 * 回文 0xAEAE，写错字节序也会"看起来正确"）⇒ 必须再验 seq=2。 */
static void c12_icmp_packet_bytes(void)
{
    iv_probe_t            p;
    iv_probe_cfg_t        c = mk_cfg();
    const struct icmphdr *ih;
    const size_t          pkt_len = sizeof(struct icmphdr) + IV_PROBE_ICMP_PAYLOAD;

    mock_reset();
    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_icmp(&p, DST, 0, "icmp");
    (void)iv_probe_start(&p, 5000);
    (void)iv_probe_tick(&p, 5000); /* 发第 1 个 echo，seq=1 */

    chk(g_sent_len == pkt_len, "c12 icmp pkt length");
    ih = (const struct icmphdr *)g_sent;
    chk(ih->type == ICMP_ECHO, "c12 type echo");
    chk(ih->code == 0, "c12 code 0");
    chk(ntohs(ih->un.echo.id) == 0x4950u, "c12 echo id");
    chk(ntohs(ih->un.echo.sequence) == 1u, "c12 seq starts at 1");
    chk(pkt_ones_sum(g_sent, g_sent_len) == 0xFFFFu, "c12 checksum valid (seq=1)");

    /* 喂回包让 inflight 归零，好排下一轮；再发第 2 包验证 seq=2 校验和仍有效
     * （排除"seq=1 恰好回文"的巧合通过）——这是区分字节序正误的关键断言。 */
    g_pend_len = mk_icmp_reply(g_pend, 0x4950u, 1u);
    (void)iv_probe_on_readable(&p, iv_probe_fd_at(&p, 0), 5050);
    (void)iv_probe_tick(&p, 5050u + 100000u);
    ih = (const struct icmphdr *)g_sent;
    chk(ntohs(ih->un.echo.sequence) == 2u, "c12 seq increments");
    chk(pkt_ones_sum(g_sent, g_sent_len) == 0xFFFFu, "c12 checksum valid (seq=2)");
    (void)iv_probe_stop(&p);
}

int main(void)
{
    c01_args_and_guards();
    c02_stat_snapshot();
    c03_hysteresis();
    c04_degrade();
    c05_link_event();
    c06_icmp_disabled();
    c07_tick_timeout();
    c08_icmp_reply();
    c09_tcp_connect_result();
    c10_now_is_caller_clock();
    c11_lifecycle_guards();
    c12_icmp_packet_bytes();

    if (g_fail != 0) {
        fprintf(stderr, "test_probe FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_probe passed (args, stat window, hysteresis, degrade, link event, "
           "icmp disable/reply/packet-bytes, tcp result, timeout, lifecycle)\n");
    return 0;
}
