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
 *   c12 ICMP 报文校验和字节序 —— 漏 htons 则真内核丢包、回包全丢（联调实证）；
 *   c16 ICMP 报文带 `IP_PKTINFO.ipi_ifindex` —— 不带则所有 WAN 都按默认路由探，
 *       2026-10-29 现场"4G 通、两条一起判 DOWN"就是这么来的。
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
/* 回包"从哪块网卡进来"（c17）：0 = 不带 cmsg，认领时跳过入口网卡校验 */
static int      g_pend_ifindex = 0;

/* 最近一次 sendto 发出的原始字节（用于字节级校验 ICMP 报文，尤其校验和字节序） */
static uint8_t  g_sent[512];
static size_t   g_sent_len     = 0;

/* 所有发出过的 ICMP echo 序号（c14 用：验证多项目序号不撞号） */
static uint16_t g_seqs[8];
static int      g_seq_n        = 0;

/* 出口绑定（c16）：setsockopt 放不放行 IP_PKTINFO、sendmsg 实际带的出口 ifindex */
static int      g_pktinfo_ok   = 1;   /* 0 = setsockopt 拒（模拟老/裁剪内核） */
static int      g_setsockopt_n = 0;   /* start 里开 IP_PKTINFO 的次数 */
static int      g_sendmsg_n    = 0;   /* 走 sendmsg（＝绑出口）的报文数 */
static int      g_sendmsg_einval = 0; /* 1 = sendmsg 报 EINVAL（模拟内核不认 control） */
static int      g_bind_ifindex[8];
static int      g_bind_n       = 0;   /* g_bind_ifindex 已填个数 */

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
    if (buf != NULL && len >= sizeof(struct icmphdr) && g_seq_n < (int)(sizeof(g_seqs) / sizeof(g_seqs[0]))) {
        const struct icmphdr *ih = (const struct icmphdr *)buf;

        if (ih->type == ICMP_ECHO)
            g_seqs[g_seq_n++] = ntohs(ih->un.echo.sequence);
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

/* 带控制消息收包（c17）：除了照常喂 `g_pend`，还按 `g_pend_ifindex` 挂一条
 * `IP_PKTINFO`，模拟"这个回包是从哪块网卡进来的"。`g_pend_ifindex==0` 时
 * 不挂 cmsg，等价于老内核 / 没开绑定 —— 认领时跳过入口网卡校验。 */
static ssize_t mk_recvmsg(int fd, struct msghdr *msg, int flags)
{
    ssize_t n;

    if (msg == NULL || msg->msg_iov == NULL || msg->msg_iovlen == 0)
        return -1;
    n = mk_recv(fd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags);
    if (n < 0)
        return n;
    if (g_pend_ifindex != 0 && msg->msg_control != NULL &&
        msg->msg_controllen >= CMSG_SPACE(sizeof(struct in_pktinfo))) {
        struct cmsghdr    *cm;
        struct in_pktinfo *pi;

        memset(msg->msg_control, 0, msg->msg_controllen);
        cm                  = CMSG_FIRSTHDR(msg);
        cm->cmsg_level      = IPPROTO_IP;
        cm->cmsg_type       = IP_PKTINFO;
        cm->cmsg_len        = CMSG_LEN(sizeof(struct in_pktinfo));
        pi                  = (struct in_pktinfo *)(void *)CMSG_DATA(cm);
        pi->ipi_ifindex     = g_pend_ifindex;
        msg->msg_controllen = CMSG_SPACE(sizeof(struct in_pktinfo));
    } else if (msg->msg_control != NULL) {
        msg->msg_controllen = 0;
    }
    return n;
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

/* IP_PKTINFO 开关（c16）：非 IP_PKTINFO 的 setsockopt 一律成功，不影响其它用例 */
static int mk_setsockopt(int fd, int level, int optname, const void *val, socklen_t len)
{
    (void)fd;
    (void)val;
    (void)len;
    if (level != IPPROTO_IP || optname != IP_PKTINFO)
        return 0;
    g_setsockopt_n++;
    if (!g_pktinfo_ok) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* 带控制消息发包（c16）：抓 ancillary 里那个 `ipi_ifindex`，它就是"这块 WAN"的证据 */
static ssize_t mk_sendmsg(int fd, const struct msghdr *msg, int flags)
{
    (void)fd;
    (void)flags;
    g_sendmsg_n++;
    if (g_sendmsg_einval) {
        errno = EINVAL;
        return -1;
    }
    {
        const struct cmsghdr *cm = CMSG_FIRSTHDR(msg);

        if (cm != NULL && cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
            const struct in_pktinfo *pi =
                (const struct in_pktinfo *)(const void *)CMSG_DATA(cm);

            if (g_bind_n < (int)(sizeof(g_bind_ifindex) / sizeof(g_bind_ifindex[0])))
                g_bind_ifindex[g_bind_n++] = pi->ipi_ifindex;
        }
    }
    if (msg->msg_iov != NULL && msg->msg_iovlen > 0)
        return (ssize_t)msg->msg_iov[0].iov_len;
    return 0;
}

static const iv_probe_iops_t g_mock = {
    .socket     = mk_socket,
    .connect    = mk_connect,
    .sendto     = mk_sendto,
    .recv       = mk_recv,
    .getsockopt = mk_getsockopt,
    .close      = mk_close,
    .now_ms     = mk_now,
    .sendmsg    = mk_sendmsg,
    .setsockopt = mk_setsockopt,
    .recvmsg    = mk_recvmsg
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
    g_pend_ifindex  = 0;
    g_sent_len      = 0;
    g_seq_n         = 0;
    g_pktinfo_ok    = 1;
    g_setsockopt_n  = 0;
    g_sendmsg_n     = 0;
    g_sendmsg_einval = 0;
    g_bind_n        = 0;
}

/* 构造 ICMP echo reply 字节流（IP 头 + ICMP 头），返回总长。
 * `saddr_be` 是回包的**源地址**：认领校验要求它等于该项的目标地址
 * （2026-10-29 加，见 `iv_probe.h` 的 `icmp_reject`）。 */
static size_t mk_icmp_reply_from(uint8_t *b, uint16_t id, uint16_t seq,
                                 uint32_t saddr_be)
{
    struct iphdr   *ip = (struct iphdr *)b;
    struct icmphdr *ic = (struct icmphdr *)(b + 20);

    memset(b, 0, 28);
    ip->version  = 4;
    ip->ihl      = 5;
    ip->protocol = IPPROTO_ICMP;
    ip->saddr    = saddr_be;
    ic->type             = ICMP_ECHOREPLY;
    ic->code             = 0;
    ic->un.echo.id       = htons(id);
    ic->un.echo.sequence = htons(seq);
    return 28;
}

static size_t mk_icmp_reply(uint8_t *b, uint16_t id, uint16_t seq)
{
    return mk_icmp_reply_from(b, id, seq, 0x0102A8C0u /* = DST，见下 */);
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
    /* 运行期调参入口（`retune` / `set_degrade`）都要求引擎已 start —— 只有 APP 项，
     * 不会去开 raw socket。 */
    chk(iv_probe_start(&p, 0) == IV_OK, "c04 start");

    (void)iv_probe_note_result(&p, 0, 1, 50, 100);
    (void)iv_probe_note_result(&p, 0, 1, 50, 200);
    chk(iv_probe_state(&p, 0) == IV_PROBE_UP, "c04 fast rtt -> UP");

    /* 拉高 RTT：avg 超阈值 -> DEGRADED（仍算可用） */
    (void)iv_probe_note_result(&p, 0, 1, 300, 300);
    (void)iv_probe_note_result(&p, 0, 1, 300, 400);
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.avg_rtt_ms > 100, "c04 avg risen");
    chk(st.state == IV_PROBE_DEGRADED, "c04 high rtt -> DEGRADED");

    /* ---- 运行期改门限（`probe.timeout_ms` 的热生效入口）----
     * 现场把它从 100 拉到 4000 ⇒ 同一条链路立刻**不再**算延时严重；拉回来又变回去。
     * 判据必须是"按现有证据立刻重判"，等下一拍会让引擎口径与上报口径打架。 */
    chk(iv_probe_set_degrade(&p, 4000, 0) == IV_OK, "c04 set_degrade -> OK");
    chk(iv_probe_state(&p, 0) == IV_PROBE_UP,
        "c04 raising the threshold clears DEGRADED at once");
    chk(iv_probe_set_degrade(&p, 4000, 0) == IV_OK, "c04 same value -> OK");
    chk(iv_probe_state(&p, 0) == IV_PROBE_UP, "c04 same value does not flap");
    chk(iv_probe_set_degrade(&p, 0, 0) == IV_OK, "c04 0 = do not judge");
    chk(iv_probe_state(&p, 0) == IV_PROBE_UP, "c04 threshold 0 never degrades");
    chk(iv_probe_set_degrade(&p, 100, 0) == IV_OK, "c04 threshold back to 100");
    chk(iv_probe_state(&p, 0) == IV_PROBE_DEGRADED,
        "c04 lowering the threshold re-degrades at once");
    {
        /* 未 init（started=0）⇒ ESTATE，不许"改内存当生效" */
        iv_probe_t fresh;

        memset(&fresh, 0, sizeof(fresh));
        chk(iv_probe_set_degrade(&fresh, 100, 0) == IV_ESTATE,
            "c04 set_degrade before start -> ESTATE");
        chk(iv_probe_set_degrade(NULL, 100, 0) == IV_EINVAL, "c04 set_degrade(NULL) -> EINVAL");
    }
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

/* 运行期调参（probe.interval_ms / timeout_ms / max_fail 的热生效入口）。
 * 重点不是"能不能改"，而是三件容易改错的事：
 *   ① 非法值必须拒（0 在 init 里是"取默认"，运行期只能是配错了）；
 *   ② 同值必须不抖 —— 尤其不能把 next_due 往后推，否则平台反复下发同一配置
 *      会让这条链路永远等不到下一拍；
 *   ③ 改阈值不能抹掉已累计的连败（那是"已经发生的事实"），也不能等下一拍才认。 */
static void c13_retune(void)
{
    iv_probe_t      p;
    iv_probe_cfg_t  c = mk_cfg();
    iv_probe_stat_t st;
    uint64_t        nd;

    c.interval_ms = 5000;
    c.timeout_ms  = 1000;
    c.fail_n      = 3;
    mock_reset();
    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_icmp(&p, DST, 0, "icmp");

    /* 未 start 一律拒：运行期入口不允许"先把参数塞进去、等 start 再生效" */
    chk(iv_probe_retune(&p, 10000, 2000, 3, 5000) == IV_ESTATE,
        "c13 retune before start -> ESTATE");

    (void)iv_probe_start(&p, 5000);

    chk(iv_probe_retune(&p, 0, 1000, 3, 5000) == IV_EINVAL, "c13 interval 0 rejected");
    chk(iv_probe_retune(&p, 10000, 0, 3, 5000) == IV_EINVAL, "c13 timeout 0 rejected");
    chk(iv_probe_retune(&p, 10000, 10000, 3, 5000) == IV_EINVAL,
        "c13 timeout >= interval rejected");
    chk(iv_probe_retune(&p, 10000, 2000, 0, 5000) == IV_EINVAL, "c13 fail_n 0 rejected");
    chk(p.cfg.interval_ms == 5000u && p.cfg.timeout_ms == 1000u && p.cfg.fail_n == 3u,
        "c13 rejections leave cfg untouched");

    /* 正常调参：cfg 与每项都跟着改，next_due 从 now 重排 */
    chk(iv_probe_retune(&p, 10000, 2000, 4, 5000) == IV_OK, "c13 retune ok");
    chk(p.cfg.interval_ms == 10000u && p.cfg.timeout_ms == 2000u && p.cfg.fail_n == 4u,
        "c13 cfg updated");
    chk(p.item[0].interval_ms == 10000u, "c13 item interval updated");
    chk(p.item[0].next_due_ms == 15000u, "c13 next_due rebased from now");

    /* 同值不抖：时钟不能被重新起算 */
    chk(iv_probe_retune(&p, 10000, 2000, 4, 9000) == IV_OK, "c13 same values ok");
    chk(p.item[0].next_due_ms == 15000u, "c13 same values do not restart the clock");

    /* 在飞的项不动：让它按原 timeout 收完这一次 */
    (void)iv_probe_tick(&p, 15000);
    chk(p.item[0].inflight == 1, "c13 item inflight after tick");
    nd = p.item[0].next_due_ms;
    chk(iv_probe_retune(&p, 20000, 3000, 4, 16000) == IV_OK, "c13 retune while inflight");
    chk(p.item[0].next_due_ms == nd, "c13 inflight item keeps its next_due");

    /* 阈值：连败累计不清零，降阈值立刻认 */
    (void)iv_probe_note_result(&p, 0, 0, 0, 17000);
    (void)iv_probe_note_result(&p, 0, 0, 0, 17000);
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.fail_streak == 2u, "c13 fail_streak accumulated");
    chk(st.state != IV_PROBE_DOWN, "c13 below threshold still not DOWN");

    chk(iv_probe_retune(&p, 20000, 3000, 2, 18000) == IV_OK, "c13 lower fail_n ok");
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.fail_streak == 2u, "c13 retune keeps fail_streak");
    chk(st.state == IV_PROBE_DOWN, "c13 lower threshold rejudged immediately");

    (void)iv_probe_stop(&p);
}

/* ---------------------------------------------------------------------------
 * c14 多路 ICMP：序号必须**全引擎唯一**（R-02）
 *
 * 病根（2026-10-22 板端 F-01 实测）：序号原先挂在**项**上、每项各自从 0 起，
 * 同一引擎的两路摄像机因此同拍发出**同号** echo。回包只能按 (id, seq) 认领，
 * 于是两路一旦错拍，A 的回包就被记到 B 名下；`now - B.sent_at` 还可能为负，
 * 转成 uint32 后是个天文数字 ⇒ 0.47 ms 的摄像机被判成「延时(4)」。
 * ------------------------------------------------------------------------- */
static void c14_icmp_seq_engine_unique(void)
{
    iv_probe_t      p;
    iv_probe_cfg_t  c = mk_cfg();
    iv_probe_stat_t st;
    int             a;
    int             b;

    mock_reset();
    (void)iv_probe_init(&p, &c);
    a = iv_probe_add_icmp(&p, DST, 0, "a");
    b = iv_probe_add_icmp(&p, DST + 1u, 0, "b");
    (void)iv_probe_start(&p, g_now);

    (void)iv_probe_tick(&p, 5000); /* 同一拍，两路各发一个 */
    chk(a == 0 && b == 1, "c14 two icmp items added");
    chk(g_seq_n == 2, "c14 both echoes sent in one tick");
    chk(g_seqs[0] != g_seqs[1], "c14 seq unique across items");

    /* 回**第二路**的包：只能记到 b 名下，a 一点都不能动。
     * 源地址必须是 b 自己的目标（DST+1），否则会被认领校验挡掉。 */
    g_pend_len = mk_icmp_reply_from(g_pend, 0x4950u, g_seqs[1], DST + 1u);
    chk(iv_probe_on_readable(&p, iv_probe_fd_at(&p, 0), 5100) == 1, "c14 b reply handled");
    (void)iv_probe_stat_get(&p, b, &st);
    chk(st.sent == 1 && st.lost == 0, "c14 reply credited to its own item");
    chk(st.last_rtt_ms == 100, "c14 rtt sane (no negative wrap)");
    (void)iv_probe_stat_get(&p, a, &st);
    chk(st.sent == 0 && st.lost == 0, "c14 other item untouched");

    /* a 一直没回：按自己的超时判负，且不能留一个来路不明的 RTT */
    (void)iv_probe_tick(&p, 12000);
    (void)iv_probe_stat_get(&p, a, &st);
    chk(st.lost == 1, "c14 a timed out on its own");
    chk(st.last_rtt_ms == 0, "c14 no bogus rtt on timeout");
    (void)iv_probe_stop(&p);
}

/* 同一探测目标、不同出口网口：必须能各建一项（2026-10-26）。
 *
 * 装配层 `agent_probe_build()` 是**逐 WAN 建项、所有 WAN 共用同一个 dst**
 * （`agent_probe_dst()` 取 target1，空则取 target2）。原来的去重只看
 * `(layer, dst[, port])`、不含 ifindex，于是第二个 WAN 必拿 `IV_EEXIST`
 * ⇒ `wan_probe_idx[wired] = -1` ⇒ `netmgr_feed()` 直接 `continue`
 * ⇒ **有线链路永远不参与判定**，`wan_up[WIRED]` 恒 0：
 * 界面「主网2」恒"断开"、状态永远到不了"有线已连接"、自动切换的"有线断→回切无线"
 * 永不成立。板端每次开机那条 `probe item add failed for wan eth0 (-9)` 就是它
 * （-9 = `IV_EEXIST`）——这功能从上线起就没真正跑起来过。 */
static void c15_dual_wan_shared_dst(void)
{
    iv_probe_t     p;
    iv_probe_cfg_t c = mk_cfg();

    (void)iv_probe_init(&p, &c);
    chk(iv_probe_add_icmp(&p, DST, 8, "usb0") == 0, "c15 wan0 icmp idx0");
    chk(iv_probe_add_icmp(&p, DST, 4, "eth0") == 1,
        "c15 同 dst 不同 ifindex 各建一项（否则有线链路永远不判定）");
    chk(iv_probe_add_icmp(&p, DST, 8, "dup") == IV_EEXIST,
        "c15 同 dst 同 ifindex 仍是重复");
    chk(iv_probe_add_tcp(&p, DST, 80, 8, "t-w0") == 2, "c15 wan0 tcp idx2");
    chk(iv_probe_add_tcp(&p, DST, 80, 4, "t-w1") == 3,
        "c15 tcp 同端口不同 ifindex 各建一项");
    chk(iv_probe_add_tcp(&p, DST, 80, 4, "t-dup") == IV_EEXIST,
        "c15 tcp 同 ifindex 同端口仍是重复");
    chk(iv_probe_count(&p) == 4, "c15 一共 4 项");
    (void)iv_probe_stop(&p);
}

/* ICMP 探测报文必须按项的 `ifindex` 指定出口网卡（2026-10-29）。
 *
 * 反向证伪点（每一条都对应一次真实误判）：
 *   1) 报文不带 `IP_PKTINFO.ipi_ifindex` ⇒ 两条 WAN 的探测全按**系统默认路由**
 *      出去，判出来的不是"这条链路通不通"。现场原样复现：4G `ping -I usb0` 3/3
 *      通，两块 WAN 却一起判 DOWN ⇒ netmgr 进 BOTH_DOWN ⇒ 按设计"双断只告警不
 *      动作" ⇒ 卡在没网的出口上，平台断联 4 小时；
 *   2) 两个项带的是同一个 ifindex（或者干脆写死 0）⇒ 等于没绑，同 1)；
 *   3) `IP_PKTINFO` 开不出来时要退回 `sendto()`，**不能**把开不出来算成探测失败
 *      —— 那会让"绑不上"直接变成"所有 WAN 都断"；
 *   4) `sendmsg` 报 EINVAL 时要当拍退回 `sendto()` 重发，且**不记失败** —— 同上，
 *      绑定失败不是目标不可达。 */
static void c16_icmp_egress_bind(void)
{
    iv_probe_t      p;
    iv_probe_cfg_t  c = mk_cfg();
    iv_probe_stat_t st;

    /* (1) 正常路径：两个项各带各的出口 */
    mock_reset();
    (void)iv_probe_init(&p, &c);
    chk(iv_probe_add_icmp(&p, DST, 8, "usb0") == 0, "c16 usb0 idx0");
    chk(iv_probe_add_icmp(&p, DST, 4, "eth0") == 1, "c16 eth0 idx1");
    chk(iv_probe_start(&p, g_now) == IV_OK, "c16 start");
    chk(g_setsockopt_n == 1, "c16 有绑定项时 start 开一次 IP_PKTINFO");
    (void)iv_probe_tick(&p, g_now);
    chk(g_sendmsg_n == 2, "c16 两个绑定接口的项都走 sendmsg");
    chk(g_bind_n == 2 && g_bind_ifindex[0] == 8 && g_bind_ifindex[1] == 4,
        "c16 每个探测报文带自己的出口 ifindex（否则两条 WAN 都走默认路由）");
    (void)iv_probe_stop(&p);

    /* (2) 没有绑定接口的项：走 sendto，且不该白开 IP_PKTINFO */
    mock_reset();
    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_icmp(&p, DST, 0, "any");
    (void)iv_probe_start(&p, g_now);
    chk(g_setsockopt_n == 0, "c16 无绑定项时不开 IP_PKTINFO");
    (void)iv_probe_tick(&p, g_now);
    chk(g_sendmsg_n == 0 && g_sent_len > 0, "c16 未绑接口的项仍走 sendto");
    (void)iv_probe_stop(&p);

    /* (3) IP_PKTINFO 开不出来：退回 sendto，不判死 */
    mock_reset();
    g_pktinfo_ok = 0;
    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_icmp(&p, DST, 8, "usb0");
    (void)iv_probe_start(&p, g_now);
    chk(p.icmp_bind_ok == 0, "c16 setsockopt 被拒 -> 绑定关闭");
    (void)iv_probe_tick(&p, g_now);
    chk(g_sendmsg_n == 0 && g_sent_len > 0, "c16 绑定关闭后走 sendto");
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.lost == 0, "c16 绑定失败不算目标不可达");
    (void)iv_probe_stop(&p);

    /* (4) sendmsg 报 EINVAL：当拍退回 sendto 重发，同样不判死 */
    mock_reset();
    g_sendmsg_einval = 1;
    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_icmp(&p, DST, 8, "usb0");
    (void)iv_probe_start(&p, g_now);
    (void)iv_probe_tick(&p, g_now);
    chk(p.icmp_bind_ok == 0, "c16 sendmsg EINVAL -> 绑定关闭");
    chk(g_sent_len > 0, "c16 EINVAL 当拍退回 sendto 重发");
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.lost == 0, "c16 EINVAL 不算目标不可达");
    (void)iv_probe_stop(&p);
}

/* 回包认领（2026-10-29 加）。raw ICMP socket 收**全部** ICMP，原先只按
 * `(id, seq) + inflight` 认领 —— 局域网里任意一台设备 ping 时的 seq 也是递增小整数，
 * 撞上就把这条 WAN 判成"通"。现场实测抓到 `192.168.1.200` 的 echo reply 顶替了平台
 * 回包：有线到平台 100% 丢包，界面却显示"主网1 正常"。
 * 这里钉死两道校验：① 源地址必须是该项的 dst；② 绑定开启时回包必须从该项的网卡进来
 * （两路探同一个平台地址时，光靠 ① 分不清是哪一路）。 */
static void c17_icmp_reply_owner_check(void)
{
    iv_probe_t      p;
    iv_probe_cfg_t  c = mk_cfg();
    iv_probe_stat_t st;
    int             fd;

    mock_reset();
    (void)iv_probe_init(&p, &c);
    (void)iv_probe_add_icmp(&p, DST, 8, "eth0");
    (void)iv_probe_start(&p, g_now);
    chk(p.icmp_bind_ok == 1, "c17 绑定开启（入口网卡校验才生效）");
    (void)iv_probe_tick(&p, g_now); /* 发 echo，seq=1 */
    fd = iv_probe_fd_at(&p, 0);

    /* ① 源地址不是本项目标：局域网别的机器撞了 seq —— 必须拒 */
    g_pend_ifindex = 8;
    g_pend_len     = mk_icmp_reply_from(g_pend, 0x4950u, 1u, DST + 7u);
    chk(iv_probe_on_readable(&p, fd, g_now + 10u) == 0, "c17 源地址不对 -> 拒收");
    chk(p.icmp_reject == 1u, "c17 拒收计数 +1");
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.sent == 0 && st.lost == 0, "c17 拒收不记成功也不记超时");
    chk(iv_probe_state(&p, 0) == IV_PROBE_UNKNOWN, "c17 拒收后状态不动");

    /* ② 源地址对、但回包从别的网卡进来：两路探同一目标时靠这条分开 */
    g_pend_ifindex = 9;
    g_pend_len     = mk_icmp_reply(g_pend, 0x4950u, 1u);
    chk(iv_probe_on_readable(&p, fd, g_now + 20u) == 0, "c17 入口网卡不对 -> 拒收");
    chk(p.icmp_reject == 2u, "c17 拒收计数 +2");

    /* ③ 两项都对：认领，RTT 按调用方传的 now 算 */
    g_pend_ifindex = 8;
    g_pend_len     = mk_icmp_reply(g_pend, 0x4950u, 1u);
    chk(iv_probe_on_readable(&p, fd, g_now + 30u) == 1, "c17 源地址＋入口都对 -> 认领");
    (void)iv_probe_stat_get(&p, 0, &st);
    chk(st.sent == 1 && st.lost == 0 && st.last_rtt_ms == 30u, "c17 认领后 RTT=30");

    /* ④ 绑定没开（老内核 / 无 IP_PKTINFO）：跳过入口网卡校验，只认源地址 */
    p.icmp_bind_ok = 0;
    (void)iv_probe_tick(&p, g_now + 100000u); /* 再发一个，seq=2 */
    g_pend_ifindex = 9;
    g_pend_len     = mk_icmp_reply(g_pend, 0x4950u, 2u);
    chk(iv_probe_on_readable(&p, fd, g_now + 100010u) == 1,
        "c17 绑定关闭时不校验入口网卡（退回老行为）");
    (void)iv_probe_stop(&p);
}

/* ---------------------------------------------------------------------------
 * c18 轮统计：一轮 ＝ 4 个包（2026-10-29 用户口径）
 *
 * "每 ping 一次发 4 个数据包，判断是否全部响应，计算 4 个数据包的平均响应时间，
 *  用于判断丢包、延时"。
 *
 * 上层的四态判定（通/不通/延时/丢包）只看 `round_*` 这组字段，所以它们必须：
 *   ① 不满 4 个包时 `round_seq` 不动 —— 拿半轮数据下结论，就是"刚开机就报故障"；
 *   ② 结算时给出成功/失败**个数**（不是只给一个布尔），否则分不清"丢包"和"故障"；
 *   ③ 平均 RTT 只对**成功包**求平均，全失败时记 0（不是"0 ms、很快"）。
 *
 * 反向证伪：把 `round_push()` 里的 `if (it->rnd_n < IV_PROBE_ROUND_SAMPLES) return;`
 * 去掉（每包都结算）⇒ ①③ 全红；把 `rnd_rtt_sum / rnd_ok` 改成 `/ rnd_n` ⇒
 * 有丢包那一轮的平均值立刻变小（20 → 15），第 2 条断言红。
 * ------------------------------------------------------------------------- */
static void c18_round_stats(void)
{
    iv_probe_t      p;
    iv_probe_cfg_t  c = mk_cfg();
    iv_probe_stat_t st;
    int             i;
    int             k;

    mock_reset();
    (void)iv_probe_init(&p, &c);
    i = iv_probe_add_icmp(&p, DST, 0, "round");
    (void)iv_probe_start(&p, 5000);

    /* 前 3 个包：还不算一轮 */
    (void)iv_probe_note_result(&p, i, 1, 10, 5100);
    (void)iv_probe_note_result(&p, i, 1, 20, 5200);
    (void)iv_probe_note_result(&p, i, 1, 30, 5300);
    (void)iv_probe_stat_get(&p, i, &st);
    chk(st.round_seq == 0u, "c18 no round settled before the 4th packet");

    /* 第 4 个包：3 成 1 败 ⇒ 平均只算成功的三个：(10+20+30)/3 = 20 */
    (void)iv_probe_note_result(&p, i, 0, 0, 5400);
    (void)iv_probe_stat_get(&p, i, &st);
    chk(st.round_seq == 1u, "c18 round settled exactly at the 4th packet");
    chk(st.round_ok == 3u && st.round_lost == 1u, "c18 round counts ok/lost");
    chk(st.round_avg_rtt_ms == 20u, "c18 average uses successful packets only");

    /* 全失败的一轮：平均 RTT 必须是 0，上层靠 round_ok==0 判"故障" */
    for (k = 0; k < 4; k++)
        (void)iv_probe_note_result(&p, i, 0, 0, 6100u + (uint64_t)k * 100u);
    (void)iv_probe_stat_get(&p, i, &st);
    chk(st.round_seq == 2u && st.round_ok == 0u && st.round_lost == 4u,
        "c18 an all-lost round");
    chk(st.round_avg_rtt_ms == 0u, "c18 all-lost round carries no average");

    /* 新一轮必须把上一轮的计数清掉（否则"4 成 + 4 败"会一直显示丢包） */
    for (k = 0; k < 4; k++)
        (void)iv_probe_note_result(&p, i, 1, 40, 7100u + (uint64_t)k * 100u);
    (void)iv_probe_stat_get(&p, i, &st);
    chk(st.round_seq == 3u && st.round_ok == 4u && st.round_lost == 0u,
        "c18 a fresh round resets the counters");
    chk(st.round_avg_rtt_ms == 40u, "c18 fresh round average");

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
    c13_retune();
    c14_icmp_seq_engine_unique();
    c15_dual_wan_shared_dst();
    c16_icmp_egress_bind();
    c17_icmp_reply_owner_check();
    c18_round_stats();

    if (g_fail != 0) {
        fprintf(stderr, "test_probe FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_probe passed (args, stat window, hysteresis, degrade, link event, "
           "icmp disable/reply/packet-bytes/seq-unique/egress-bind/reply-owner, tcp result, "
           "timeout, lifecycle, retune, dual-wan-shared-dst, round-4-packets)\n");
    return 0;
}
