/*
 * iv_netlink 单测（libivhal，M3-S3.1）
 *
 * ============================ 测法：字节级注入 ＋ 真实订阅 ============================
 * 解析逻辑全部经 `iv_netlink_feed()` 驱动 —— 用构造的 rtnetlink 报文（与
 * `ip monitor` 同款字节流）逐条注入，不开 socket、不碰真实网络，任何环境
 * （本机 / VM / 板端）结果一致。真实内核事件（ip link / ip addr / ip route）
 * 属集成联调，在编译 VM 上按计划 §S3.1 验证句单独做。
 * 唯一碰真实内核的是 c02：attach 一条真 NETLINK_ROUTE socket，用
 * `getsockname` 断言订阅组播组精确等于 RTMGRP_LINK|IPv4_IFADDR|IPv4_ROUTE
 * —— "订阅了什么"是本模块最核心的行为，必须钉死。
 *
 * ============================ 反向证伪点（每条都对应一个真缺陷） ============================
 *   c04 重复 NEWLINK 期望 0 事件 —— 去掉去重表必红（qdisc/MTU 噪音会灌爆上层）；
 *   c04 LOWER_UP 清位必须判 DOWN —— carrier 判据错成 IFF_RUNNING 时，dummy/lo
 *        这类 operstate=UNKNOWN 的接口永远报不出事件；
 *   c06 DELLINK 只在"曾见过且为 UP"时补发 DOWN —— 去掉 seen 查找会多报幽灵 DOWN；
 *   c07 地址不去重 —— 若误用 LINK 的去重表处理地址，同接口第二地址会丢；
 *   c08 非 UNICAST/非默认路由被过滤 —— 放行会灌进网段路由风暴；
 *   c10 -ENOBUFS 必须升级 IV_EFULL —— 吞掉它上层永远不知道丢过消息。
 *
 * 临时产物：无（全部内存内构造，无文件、无 fork）。AGENTS.md 规则 6。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <net/if.h>          /* IFF_* */
#include <linux/netlink.h>   /* NLMSG_* / struct nlmsgerr */
#include <linux/rtnetlink.h> /* RTM_* / RTMGRP_* / RTA_* / RTN_* / RT_TABLE_* */

#include "ivsbox/iv_netlink.h"
#include "ivsbox/iv_ret.h"

#ifndef IFF_LOWER_UP
/* 内核私有位 0x10000（见 iv_netlink.c 的 IVNL_KF_* 说明）；0x40000 是 IFF_ECHO，
 * 曾因误用该值导致单测自洽而真实内核事件全丢，这里必须与实现同源。 */
#define IFF_LOWER_UP 0x10000u
#endif

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* /proc/self/fd 条目数：attach/close 循环不泄漏 fd（同 test_serial 的口径） */
static int count_open_fds(void)
{
    DIR           *d;
    struct dirent *e;
    int            n = 0;

    d = opendir("/proc/self/fd");
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.')
            n++;
    }
    (void)closedir(d);
    return n;
}

/* ---------------------------------------------------------------------------
 * 事件录制
 * ------------------------------------------------------------------------- */
#define REC_MAX 16

static iv_netlink_evt_t g_evts[REC_MAX];
static int              g_n;

static void rec_cb(const iv_netlink_evt_t *e, void *arg)
{
    (void)arg;
    if (g_n < REC_MAX)
        g_evts[g_n] = *e;
    g_n++;
}

static void rec_reset(void)
{
    g_n = 0;
    memset(g_evts, 0, sizeof(g_evts));
}

/* ---------------------------------------------------------------------------
 * rtnetlink 报文构造（对齐内核字节流，驱动与 poll 同一条解析路径）
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t *b;
    size_t   cap;
    size_t   len;
} mbuf_t;

static void mb_init(mbuf_t *m, uint8_t *buf, size_t cap)
{
    m->b   = buf;
    m->cap = cap;
    m->len = 0;
}

/* 追加一条消息（fixed 为协议定长头），返回 nlmsghdr 供继续挂属性 */
static struct nlmsghdr *mb_msg(mbuf_t *m, uint16_t type, const void *fixed,
                               size_t flen)
{
    struct nlmsghdr *nh = (struct nlmsghdr *)(m->b + m->len);
    size_t           need = NLMSG_LENGTH(flen);

    chk(m->len + NLMSG_ALIGN(need) <= m->cap, "mb_msg: cap overflow");
    memset(m->b + m->len, 0, NLMSG_ALIGN(need));
    nh->nlmsg_len   = (uint32_t)need;
    nh->nlmsg_type  = type;
    nh->nlmsg_flags = NLM_F_REQUEST;
    nh->nlmsg_seq   = 1;
    if (flen != 0)
        memcpy(NLMSG_DATA(nh), fixed, flen);
    m->len += NLMSG_ALIGN(need);
    return nh;
}

/* 给消息追加一个 rtattr（自动对齐与回写 nlmsg_len） */
static void mb_attr(mbuf_t *m, struct nlmsghdr *nh, int type,
                    const void *data, size_t dlen)
{
    struct rtattr *rta = (struct rtattr *)((uint8_t *)nh +
                                           NLMSG_ALIGN(nh->nlmsg_len));
    size_t need = RTA_LENGTH(dlen);

    chk(m->len + RTA_ALIGN(need) <= m->cap, "mb_attr: cap overflow");
    memset(rta, 0, RTA_ALIGN(need));
    rta->rta_type = type;
    rta->rta_len  = (uint16_t)need;
    memcpy(RTA_DATA(rta), data, dlen);
    nh->nlmsg_len += (uint32_t)RTA_ALIGN(need);
    m->len += RTA_ALIGN(need); /* 同步推进缓冲长度，否则喂给 feed 时会被截断 */
}

/* RTM_NEWLINK/DELLINK；name==NULL 时不带 IFLA_IFNAME */
static size_t mk_link(uint8_t *b, size_t cap, uint16_t type, int ifindex,
                      uint32_t flags, const char *name)
{
    struct ifinfomsg ifi;
    struct nlmsghdr *nh;
    mbuf_t           m;

    memset(&ifi, 0, sizeof(ifi));
    ifi.ifi_family = AF_UNSPEC;
    ifi.ifi_type   = 1; /* ARPHRD_ETHER，取值不影响解析 */
    ifi.ifi_index  = ifindex;
    ifi.ifi_flags  = flags;

    mb_init(&m, b, cap);
    nh = mb_msg(&m, type, &ifi, sizeof(ifi));
    if (name != NULL)
        mb_attr(&m, nh, IFLA_IFNAME, name, strlen(name) + 1);
    return m.len;
}

/* RTM_NEWADDR/DELADDR；with_local 控制带 IFA_LOCAL 还是只带 IFA_ADDRESS */
static size_t mk_addr(uint8_t *b, size_t cap, uint16_t type, uint8_t family,
                      uint8_t prefixlen, int ifindex, const uint8_t addr[4],
                      int with_local)
{
    struct ifaddrmsg ifa;
    struct nlmsghdr *nh;
    mbuf_t           m;

    memset(&ifa, 0, sizeof(ifa));
    ifa.ifa_family   = family;
    ifa.ifa_prefixlen = prefixlen;
    ifa.ifa_index    = (uint32_t)ifindex;

    mb_init(&m, b, cap);
    nh = mb_msg(&m, type, &ifa, sizeof(ifa));
    mb_attr(&m, nh, with_local ? IFA_LOCAL : IFA_ADDRESS, addr, 4);
    return m.len;
}

/* RTM_NEWROUTE/DELROUTE（默认路由口径：dst_len==0）；attrs 按位拼装 */
#define RT_ATTR_OIF     0x01u
#define RT_ATTR_GW      0x02u
#define RT_ATTR_PRIO    0x04u
#define RT_ATTR_TABLE   0x08u

static size_t mk_route(uint8_t *b, size_t cap, uint16_t type, uint8_t family,
                       uint8_t dst_len, uint8_t rtm_type, uint8_t rtm_table,
                       int oif, const uint8_t gw[4], uint32_t prio,
                       uint32_t table, unsigned attrs)
{
    struct rtmsg     rt;
    struct nlmsghdr *nh;
    mbuf_t           m;
    uint32_t         u;

    memset(&rt, 0, sizeof(rt));
    rt.rtm_family = family;
    rt.rtm_dst_len = dst_len;
    rt.rtm_table  = rtm_table;
    rt.rtm_scope  = RT_SCOPE_UNIVERSE;
    rt.rtm_type   = rtm_type;

    mb_init(&m, b, cap);
    nh = mb_msg(&m, type, &rt, sizeof(rt));
    if (attrs & RT_ATTR_OIF) {
        u = (uint32_t)oif;
        mb_attr(&m, nh, RTA_OIF, &u, sizeof(u));
    }
    if ((attrs & RT_ATTR_GW) && gw != NULL)
        mb_attr(&m, nh, RTA_GATEWAY, gw, 4);
    if (attrs & RT_ATTR_PRIO) {
        mb_attr(&m, nh, RTA_PRIORITY, &prio, sizeof(prio));
    }
    if (attrs & RT_ATTR_TABLE) {
        mb_attr(&m, nh, RTA_TABLE, &table, sizeof(table));
    }
    return m.len;
}

/* NLMSG_ERROR：error 任意（-ENOBUFS 走溢出路径，0/-EPERM 应被跳过） */
static size_t mk_error(uint8_t *b, size_t cap, int err)
{
    struct nlmsgerr  ne;
    mbuf_t           m;

    memset(&ne, 0, sizeof(ne));
    ne.error = err;
    mb_init(&m, b, cap);
    (void)mb_msg(&m, NLMSG_ERROR, &ne, sizeof(ne));
    return m.len;
}

/* 确定性伪随机（xorshift64*，同 fuzz_frame 的种子纪律） */
static uint64_t g_rs = 0x243F6A8885A308D3ULL;

static uint64_t rs_next(void)
{
    g_rs ^= g_rs >> 12;
    g_rs ^= g_rs << 25;
    g_rs ^= g_rs >> 27;
    return g_rs * 2685821657736338717ULL;
}

/* ---------------------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------------------- */
static void c01_args_and_guards(void)
{
    iv_netlink_t nl;
    uint8_t      buf[32] = { 0 };

    chk(iv_netlink_init(NULL, rec_cb, NULL) == IV_EINVAL, "c01 init(NULL)");
    chk(iv_netlink_init(&nl, NULL, NULL) == IV_EINVAL, "c01 init(on_evt=NULL)");
    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c01 init ok");
    chk(iv_netlink_fd(&nl) == -1, "c01 fresh fd == -1");
    chk(iv_netlink_fd(NULL) == -1, "c01 fd(NULL) == -1");
    chk(iv_netlink_poll(&nl) == IV_ESTATE, "c01 poll unattached");
    chk(iv_netlink_close(&nl) == IV_ESTATE, "c01 close unattached");
    chk(iv_netlink_feed(NULL, buf, sizeof(buf)) == IV_EINVAL, "c01 feed(NULL nl)");
    chk(iv_netlink_feed(&nl, NULL, 5) == IV_EINVAL, "c01 feed(NULL buf)");
    chk(iv_netlink_feed(&nl, NULL, 0) == 0, "c01 feed(NULL buf, len=0) == 0");
    chk(iv_netlink_feed(&nl, buf, 0) == 0, "c01 feed(len=0) == 0");
    chk(iv_netlink_attach(&nl) == IV_OK, "c01 attach ok");
    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c01 re-init");
    chk(iv_netlink_fd(&nl) == -1, "c01 re-init resets fd");
    iv_netlink_reset(NULL); /* 只要不崩 */
}

static void c02_attach_subscription(void)
{
    iv_netlink_t        nl;
    struct sockaddr_nl  sa;
    socklen_t           sl;
    int                 fd, fl, before, i;

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c02 init");
    chk(iv_netlink_attach(&nl) == IV_OK, "c02 attach");
    fd = iv_netlink_fd(&nl);
    chk(fd >= 0, "c02 fd >= 0");

    fl = fcntl(fd, F_GETFL);
    chk(fl >= 0 && (fl & O_NONBLOCK) != 0, "c02 socket is O_NONBLOCK");

    /* 订阅组必须精确等于计划 §S3.1 的三组：LINK / IPv4_IFADDR / IPv4_ROUTE */
    memset(&sa, 0, sizeof(sa));
    sl = sizeof(sa);
    chk(getsockname(fd, (struct sockaddr *)&sa, &sl) == 0, "c02 getsockname");
    chk(sa.nl_family == AF_NETLINK, "c02 family == AF_NETLINK");
    chk(sa.nl_groups == (uint32_t)(RTMGRP_LINK | RTMGRP_IPV4_IFADDR |
                                   RTMGRP_IPV4_ROUTE),
        "c02 groups exact");

    chk(iv_netlink_attach(&nl) == IV_ESTATE, "c02 double attach");
    chk(iv_netlink_poll(&nl) >= 0, "c02 quiet poll >= 0"); /* 系统可能恰好有事件 */
    chk(iv_netlink_close(&nl) == IV_OK, "c02 close");
    chk(iv_netlink_close(&nl) == IV_ESTATE, "c02 double close");
    chk(iv_netlink_fd(&nl) == -1, "c02 fd reset after close");
    chk(iv_netlink_attach(&nl) == IV_OK, "c02 re-attach after close");
    chk(iv_netlink_close(&nl) == IV_OK, "c02 close again");

    before = count_open_fds();
    for (i = 0; i < 8; i++) {
        chk(iv_netlink_attach(&nl) == IV_OK, "c02 loop attach");
        chk(iv_netlink_close(&nl) == IV_OK, "c02 loop close");
    }
    chk(count_open_fds() == before, "c02 no fd leak in attach/close cycle");
}

static void c03_truncated_and_garbage(void)
{
    iv_netlink_t nl;
    uint8_t      buf[1024];
    size_t       i;
    int          r;

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c03 init");
    rec_reset();

    memset(buf, 0xA5, 8); /* 不足一个 nlmsghdr（16B） */
    chk(iv_netlink_feed(&nl, buf, 8) == 0, "c03 short buffer");

    /* 头部声称 1000B 但只剩 16B：坏长度整体作废，不得越界读 */
    memset(buf, 0, 16);
    ((struct nlmsghdr *)buf)->nlmsg_len = 1000;
    ((struct nlmsghdr *)buf)->nlmsg_type = RTM_NEWLINK;
    chk(iv_netlink_feed(&nl, buf, 16) == 0, "c03 bad length claims 1000");

    ((struct nlmsghdr *)buf)->nlmsg_len = 0; /* nlmsg_len < NLMSG_HDRLEN */
    chk(iv_netlink_feed(&nl, buf, 16) == 0, "c03 zero nlmsg_len");

    /* 1KB 确定性伪随机：任何垃圾都不许崩、不许越界（asan 钉） */
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)rs_next();
    r = iv_netlink_feed(&nl, buf, sizeof(buf));
    chk(r >= 0 || r == IV_EFULL, "c03 garbage no crash");

}

static void c04_link_updown_and_dedupe(void)
{
    iv_netlink_t nl;
    uint8_t      b[256];
    size_t       n;
    const uint32_t up_flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_LOWER_UP;

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c04 init");
    rec_reset();

    n = mk_link(b, sizeof(b), RTM_NEWLINK, 2, up_flags, "eth0");
    chk(iv_netlink_feed(&nl, b, n) == 1, "c04 first NEWLINK delivers 1");
    chk(g_n == 1, "c04 rec count");
    chk(g_evts[0].kind == IV_NLEVT_LINK_UP, "c04 kind LINK_UP");
    chk(g_evts[0].ifindex == 2, "c04 ifindex");
    chk(strcmp(g_evts[0].ifname, "eth0") == 0, "c04 ifname");
    chk(g_evts[0].link_flags == up_flags, "c04 raw flags");

    /* 同 flags 重发（qdisc/MTU 类内核噪音）必须被去重 —— 去重表缺陷的证伪点 */
    n = mk_link(b, sizeof(b), RTM_NEWLINK, 2, up_flags, "eth0");
    chk(iv_netlink_feed(&nl, b, n) == 0, "c04 duplicate NEWLINK deduped");

    /* 拔线：LOWER_UP 清位判 DOWN（判据错成 RUNNING 时此处必红） */
    rec_reset();
    n = mk_link(b, sizeof(b), RTM_NEWLINK, 2, IFF_UP | IFF_BROADCAST, "eth0");
    chk(iv_netlink_feed(&nl, b, n) == 1, "c04 carrier loss delivers 1");
    chk(g_evts[0].kind == IV_NLEVT_LINK_DOWN, "c04 carrier loss -> LINK_DOWN");

    /* 恢复 */
    rec_reset();
    n = mk_link(b, sizeof(b), RTM_NEWLINK, 2, up_flags, "eth0");
    chk(iv_netlink_feed(&nl, b, n) == 1, "c04 carrier back delivers 1");
    chk(g_evts[0].kind == IV_NLEVT_LINK_UP, "c04 carrier back -> LINK_UP");

    /* 初见即 DOWN（开机时接口就是 administratively down）也必须报出 */
    rec_reset();
    n = mk_link(b, sizeof(b), RTM_NEWLINK, 7, 0, "wwan0");
    chk(iv_netlink_feed(&nl, b, n) == 1, "c04 first-seen down delivers 1");
    chk(g_evts[0].kind == IV_NLEVT_LINK_DOWN, "c04 first-seen down kind");
    chk(strcmp(g_evts[0].ifname, "wwan0") == 0, "c04 first-seen down ifname");

}

static void c05_link_without_ifname(void)
{
    iv_netlink_t nl;
    uint8_t      b[256];
    size_t       n;

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c05 init");
    rec_reset();

    n = mk_link(b, sizeof(b), RTM_NEWLINK, 9, IFF_UP | IFF_LOWER_UP, NULL);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c05 delivers 1");
    chk(g_evts[0].kind == IV_NLEVT_LINK_UP, "c05 kind");
    chk(g_evts[0].ifname[0] == '\0', "c05 ifname empty (not garbage)");
    chk(g_evts[0].ifindex == 9, "c05 ifindex");

    /* 无名接口同样进去重表：重复不再报 */
    n = mk_link(b, sizeof(b), RTM_NEWLINK, 9, IFF_UP | IFF_LOWER_UP, NULL);
    chk(iv_netlink_feed(&nl, b, n) == 0, "c05 nameless iface deduped too");

}

static void c06_dellink(void)
{
    iv_netlink_t nl;
    uint8_t      b[256];
    size_t       n;

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c06 init");
    rec_reset();

    n = mk_link(b, sizeof(b), RTM_NEWLINK, 5, IFF_UP | IFF_LOWER_UP, "wan0");
    (void)iv_netlink_feed(&nl, b, n);

    rec_reset();
    n = mk_link(b, sizeof(b), RTM_DELLINK, 5, IFF_UP | IFF_LOWER_UP, "wan0");
    chk(iv_netlink_feed(&nl, b, n) == 1, "c06 DELLINK on seen up iface -> 1");
    chk(g_evts[0].kind == IV_NLEVT_LINK_DOWN, "c06 DELLINK -> LINK_DOWN");
    chk(strcmp(g_evts[0].ifname, "wan0") == 0, "c06 DELLINK carries ifname");

    /* 移除后重新出现：初见语义恢复 */
    rec_reset();
    n = mk_link(b, sizeof(b), RTM_NEWLINK, 5, IFF_UP | IFF_LOWER_UP, "wan0");
    chk(iv_netlink_feed(&nl, b, n) == 1, "c06 reappear after DEL -> 1");
    chk(g_evts[0].kind == IV_NLEVT_LINK_UP, "c06 reappear kind");

    /* 从未见过的接口被移除：不补发幽灵 DOWN —— seen 查找缺失时此处必红 */
    rec_reset();
    n = mk_link(b, sizeof(b), RTM_DELLINK, 42, 0, "ghost0");
    chk(iv_netlink_feed(&nl, b, n) == 0, "c06 DELLINK on unknown iface silent");

}

static void c07_addr_add_del(void)
{
    iv_netlink_t nl;
    uint8_t      b[256];
    size_t       n;
    /* 192.168.2.120 网络序原样（内核给什么就透什么，不做主机序转换） */
    static const uint8_t a120[4] = { 0xC0u, 0xA8u, 0x02u, 0x78u };
    static const uint8_t a1[4]   = { 0xC0u, 0xA8u, 0x02u, 0x01u };

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c07 init");
    rec_reset();

    n = mk_addr(b, sizeof(b), RTM_NEWADDR, AF_INET, 24, 2, a120, 1);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c07 ADDR_ADD delivers 1");
    chk(g_evts[0].kind == IV_NLEVT_ADDR_ADD, "c07 kind");
    chk(g_evts[0].ifindex == 2, "c07 ifindex");
    chk(g_evts[0].prefixlen == 24, "c07 prefixlen");
    chk(memcmp(g_evts[0].addr, a120, 4) == 0, "c07 addr bytes (IFA_LOCAL)");

    /* 地址**不去重**：同接口第二地址、重发都必须到达 —— 误用 LINK 去重时必红 */
    n = mk_addr(b, sizeof(b), RTM_NEWADDR, AF_INET, 24, 2, a120, 1);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c07 addr NOT deduped");
    n = mk_addr(b, sizeof(b), RTM_NEWADDR, AF_INET, 24, 2, a1, 1);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c07 second addr on same iface");

    rec_reset();
    n = mk_addr(b, sizeof(b), RTM_DELADDR, AF_INET, 24, 2, a120, 1);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c07 DELADDR delivers 1");
    chk(g_evts[0].kind == IV_NLEVT_ADDR_DEL, "c07 DEL kind");

    /* 只有 IFA_ADDRESS（无 LOCAL）也必须取到地址 */
    rec_reset();
    n = mk_addr(b, sizeof(b), RTM_NEWADDR, AF_INET, 28, 3, a1, 0);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c07 ADDRESS-only delivers 1");
    chk(memcmp(g_evts[0].addr, a1, 4) == 0, "c07 addr bytes (IFA_ADDRESS)");
    chk(g_evts[0].prefixlen == 28, "c07 prefixlen 28");

    /* 非 IPv4 家族：AF_PACKET 同布局字节流必须被过滤 */
    n = mk_addr(b, sizeof(b), RTM_NEWADDR, AF_PACKET, 24, 2, a120, 1);
    chk(iv_netlink_feed(&nl, b, n) == 0, "c07 non-AF_INET filtered");

}

static void c08_route_default_only(void)
{
    iv_netlink_t nl;
    uint8_t      b[256];
    size_t       n;
    static const uint8_t gw[4] = { 0xC0u, 0xA8u, 0x02u, 0x01u }; /* 192.168.2.1 */

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c08 init");
    rec_reset();

    n = mk_route(b, sizeof(b), RTM_NEWROUTE, AF_INET, 0, RTN_UNICAST,
                 RT_TABLE_MAIN, 2, gw, 100, RT_TABLE_MAIN,
                 RT_ATTR_OIF | RT_ATTR_GW | RT_ATTR_PRIO);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c08 ROUTE_ADD delivers 1");
    chk(g_evts[0].kind == IV_NLEVT_ROUTE_ADD, "c08 kind");
    chk(g_evts[0].ifindex == 2, "c08 oif -> ifindex");
    chk(memcmp(g_evts[0].addr, gw, 4) == 0, "c08 gateway bytes");
    chk(g_evts[0].metric == 100, "c08 metric");
    chk(g_evts[0].table == RT_TABLE_MAIN, "c08 table falls back to rtm_table");
    chk(g_evts[0].prefixlen == 0, "c08 prefixlen 0");

    /* 路由不去重：同目的多指标默认路由（主备）都必须到达 */
    n = mk_route(b, sizeof(b), RTM_NEWROUTE, AF_INET, 0, RTN_UNICAST,
                 RT_TABLE_MAIN, 2, gw, 100, RT_TABLE_MAIN,
                 RT_ATTR_OIF | RT_ATTR_GW | RT_ATTR_PRIO);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c08 route NOT deduped");

    /* DEL：无属性也要给出安全零值 */
    rec_reset();
    n = mk_route(b, sizeof(b), RTM_DELROUTE, AF_INET, 0, RTN_UNICAST,
                 RT_TABLE_MAIN, 0, NULL, 0, RT_TABLE_MAIN, 0);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c08 ROUTE_DEL delivers 1");
    chk(g_evts[0].kind == IV_NLEVT_ROUTE_DEL, "c08 DEL kind");
    chk(g_evts[0].ifindex == 0 && g_evts[0].metric == 0, "c08 DEL zero fields");
    chk(g_evts[0].addr[0] == 0 && g_evts[0].addr[3] == 0, "c08 DEL no gateway");

    /* RTA_TABLE 属性覆盖 rtm_table（策略路由表 > 255 的编码路径） */
    rec_reset();
    n = mk_route(b, sizeof(b), RTM_NEWROUTE, AF_INET, 0, RTN_UNICAST,
                 RT_TABLE_MAIN, 2, gw, 0, 100, RT_ATTR_OIF | RT_ATTR_TABLE);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c08 RTA_TABLE delivers 1");
    chk(g_evts[0].table == 100, "c08 table from RTA_TABLE");

    /* 过滤：网段路由 / 非单播类型 / 非 IPv4 都不进事件流 */
    rec_reset();
    n = mk_route(b, sizeof(b), RTM_NEWROUTE, AF_INET, 24, RTN_UNICAST,
                 RT_TABLE_MAIN, 2, gw, 0, RT_TABLE_MAIN, RT_ATTR_OIF);
    chk(iv_netlink_feed(&nl, b, n) == 0, "c08 non-default dst_len filtered");
    n = mk_route(b, sizeof(b), RTM_NEWROUTE, AF_INET, 0, RTN_BLACKHOLE,
                 RT_TABLE_MAIN, 0, NULL, 0, RT_TABLE_MAIN, 0);
    chk(iv_netlink_feed(&nl, b, n) == 0, "c08 blackhole default filtered");
    n = mk_route(b, sizeof(b), RTM_NEWROUTE, AF_INET6, 0, RTN_UNICAST,
                 RT_TABLE_MAIN, 2, gw, 0, RT_TABLE_MAIN, RT_ATTR_OIF);
    chk(iv_netlink_feed(&nl, b, n) == 0, "c08 non-AF_INET filtered");
    chk(g_n == 0, "c08 nothing leaked through filters");

}

static void c09_multi_message_and_done(void)
{
    iv_netlink_t nl;
    uint8_t      b[512];
    size_t       n, off;
    const uint32_t up_flags = IFF_UP | IFF_LOWER_UP;
    static const uint8_t a120[4] = { 0xC0u, 0xA8u, 0x02u, 0x78u };

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c09 init");
    rec_reset();

    /* 一包三消息：NEWLINK + NEWADDR + NLMSG_DONE（multipart 收尾） */
    off = 0;
    n = mk_link(b + off, sizeof(b) - off, RTM_NEWLINK, 4, up_flags, "eth1");
    off += n;
    n = mk_addr(b + off, sizeof(b) - off, RTM_NEWADDR, AF_INET, 24, 4, a120, 1);
    off += n;
    n = mk_link(b + off, sizeof(b) - off, NLMSG_DONE, 0, 0, NULL); /* DONE 当空消息结尾 */
    off += n;

    chk(iv_netlink_feed(&nl, b, off) == 2, "c09 two events out of three msgs");
    chk(g_n == 2, "c09 rec count 2");
    chk(g_evts[0].kind == IV_NLEVT_LINK_UP && g_evts[0].ifindex == 4,
        "c09 order: link first");
    chk(g_evts[1].kind == IV_NLEVT_ADDR_ADD && g_evts[1].ifindex == 4,
        "c09 order: addr second");

}

static void c10_nlmsg_error_enobufs(void)
{
    iv_netlink_t nl;
    uint8_t      b[256];
    size_t       n;

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c10 init");
    rec_reset();

    /* -ENOBUFS：内核显式告知丢消息 —— 吞掉它上层永远不会重同步，必须 IV_EFULL */
    n = mk_error(b, sizeof(b), -ENOBUFS);
    chk(iv_netlink_feed(&nl, b, n) == IV_EFULL, "c10 ENOBUFS -> IV_EFULL");

    /* 普通 ACK / 请求级错误：与订阅无关，静默跳过 */
    n = mk_error(b, sizeof(b), 0);
    chk(iv_netlink_feed(&nl, b, n) == 0, "c10 ACK skipped");
    n = mk_error(b, sizeof(b), -EPERM);
    chk(iv_netlink_feed(&nl, b, n) == 0, "c10 error reply skipped");
    chk(g_n == 0, "c10 no events from error msgs");

}

static void c11_reset_resync(void)
{
    iv_netlink_t nl;
    uint8_t      b[256];
    size_t       n;

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c11 init");
    rec_reset();

    n = mk_link(b, sizeof(b), RTM_NEWLINK, 3, IFF_UP | IFF_LOWER_UP, "eth2");
    (void)iv_netlink_feed(&nl, b, n);
    chk(g_n == 1, "c11 first delivery");

    /* 全量重同步后：同一状态要重新按"初见"上报一遍（否则上层摸不到底） */
    iv_netlink_reset(&nl);
    rec_reset();
    n = mk_link(b, sizeof(b), RTM_NEWLINK, 3, IFF_UP | IFF_LOWER_UP, "eth2");
    chk(iv_netlink_feed(&nl, b, n) == 1, "c11 after reset re-delivers");
    chk(g_evts[0].kind == IV_NLEVT_LINK_UP, "c11 re-sync kind");

}

static void c12_ifname_truncation(void)
{
    iv_netlink_t nl;
    uint8_t      b[256];
    size_t       n;
    const char  *longname = "verylonginterfacex"; /* 18 字符 + NUL，超出 16 槽 */

    chk(iv_netlink_init(&nl, rec_cb, NULL) == IV_OK, "c12 init");
    rec_reset();

    n = mk_link(b, sizeof(b), RTM_NEWLINK, 6, IFF_UP | IFF_LOWER_UP, longname);
    chk(iv_netlink_feed(&nl, b, n) == 1, "c12 delivers 1");
    chk(strlen(g_evts[0].ifname) == IV_NETLINK_IFNAME_LEN - 1,
        "c12 ifname truncated to 15 chars");
    chk(g_evts[0].ifname[IV_NETLINK_IFNAME_LEN - 1] == '\0',
        "c12 ifname NUL-terminated");
    chk(memcmp(g_evts[0].ifname, longname, IV_NETLINK_IFNAME_LEN - 1) == 0,
        "c12 truncated prefix matches");

}

int main(void)
{
    c01_args_and_guards();
    c02_attach_subscription();
    c03_truncated_and_garbage();
    c04_link_updown_and_dedupe();
    c05_link_without_ifname();
    c06_dellink();
    c07_addr_add_del();
    c08_route_default_only();
    c09_multi_message_and_done();
    c10_nlmsg_error_enobufs();
    c11_reset_resync();
    c12_ifname_truncation();

    if (g_fail != 0) {
        fprintf(stderr, "test_netlink FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_netlink passed (args, attach/groups, garbage, link updown/"
           "dedupe, dellink, addr, route, multi-msg, ENOBUFS, reset, trunc)\n");
    return 0;
}
