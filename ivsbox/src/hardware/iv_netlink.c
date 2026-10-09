/*
 * rtnetlink 网络事件监听（libivhal，功能开发计划 M3-S3.1）
 *
 * 职责、取舍与全部契约见 `include/ivsbox/iv_netlink.h` 的文件头；这里只补
 * 四条实现层面的说明：
 *
 * 1) **不用 `iv_log`**（同 `iv_serial`）：本层是 syscall 薄包装 ＋ 报文解析，
 *    错误一律返回码 ＋ errno 表达；单测里日志未初始化也能安全调用。
 *
 * 2) **解析全程防御**：每条消息先过长度闸（`nlmsg_len` 既要 >= 头长、也要
 *    <= 剩余字节数），属性用 `RTA_OK` 逐个验长，属性取值再验长度够不够 4 字节。
 *    善意内核之外，本函数必须能吃下任意垃圾字节而不越界（fuzz/单测钉死）。
 *
 * 3) **超长报文按 MSG_TRUNC 丢弃而不是硬解析**：netlink 是数据报语义，
 *    半条报文没有任何可用价值；8 KiB 缓冲在路由 socket 上远超实际报文，
 *    真超长说明对端在灌异常数据，丢弃并按溢出口径上报（IV_EFULL）最诚实。
 *
 * 4) **`NLMSG_ERROR` 只关心 ENOBUFS**：error==0 是 ACK、负值是单条请求的
 *    失败回执 —— 本模块从不发请求，收到它们说明流里有别人的消息（多播组
 *    有重叠），静默跳过；唯独 `-ENOBUFS` 是内核在说"我丢了你的消息"，
 *    必须升级成 IV_EFULL 逼上层重同步。
 */
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <net/if.h>          /* IFF_UP / IFF_RUNNING / IFF_LOWER_UP(兜底) */
#include <linux/netlink.h>   /* sockaddr_nl / NETLINK_ROUTE / NLMSG_* */
#include <linux/rtnetlink.h> /* ifinfomsg / ifaddrmsg / rtmsg / RTA_* / RTMGRP_* */

#include "ivsbox/iv_netlink.h"
#include "ivsbox/iv_ret.h"

/* ---------------------------------------------------------------------------
 * 内核私有 flags 位（linux/if.h 口径，**刻意不用 glibc 的 IFF_* 名**）：
 *   LOWER_UP = 0x10000（driver signals L1 up）；注意 0x40000 是 IFF_ECHO，
 *   两者挨得很近，且各版本 glibc <net/if.h> 对这两个位的暴露不一致
 *   （2026-10-09 实测：VM gcc11/glibc2.35 即使 -D_GNU_SOURCE 也没有定义，
 *   兜底值一旦写错，单测自洽、真实内核事件全丢 —— 本轮联调抓到的真缺陷）。
 *   自持常量是唯一不依赖工具链的写法。
 * ------------------------------------------------------------------------- */
#define IVNL_KF_UP       ((uint32_t)0x00000001u) /* = IFF_UP */
#define IVNL_KF_LOWER_UP ((uint32_t)0x00010000u) /* = IFF_LOWER_UP */

/* ---------------------------------------------------------------------------
 * 属性查找（RTA_OK 逐个验长，恶意/截断字节流安全）
 * ------------------------------------------------------------------------- */
static const struct rtattr *find_attr(const struct nlmsghdr *nh, size_t hdrsz,
                                      int type)
{
    const struct rtattr *rta;
    int rlen;

    rta  = (const struct rtattr *)((const char *)nh +
                                   NLMSG_ALIGN(hdrsz) + NLMSG_HDRLEN);
    rlen = (int)NLMSG_PAYLOAD(nh, (int)hdrsz);

    while (RTA_OK(rta, rlen)) {
        if (rta->rta_type == type)
            return rta;
        rta = RTA_NEXT(rta, rlen);
    }
    return NULL;
}

/* 取 u32 属性；不存在或长度不足返回 dflt（不猜不编） */
static uint32_t attr_u32(const struct nlmsghdr *nh, size_t hdrsz, int type,
                         uint32_t dflt)
{
    const struct rtattr *a = find_attr(nh, hdrsz, type);

    if (a != NULL && RTA_PAYLOAD(a) == (int)sizeof(uint32_t))
        return *(const uint32_t *)RTA_DATA(a);
    return dflt;
}

/* 取 4 字节地址属性（网络序原样拷贝）；不存在或长度不足写全 0 */
static void attr_addr4(const struct nlmsghdr *nh, size_t hdrsz, int type,
                       uint8_t out[4])
{
    const struct rtattr *a = find_attr(nh, hdrsz, type);

    if (a != NULL && RTA_PAYLOAD(a) >= (int)sizeof(uint32_t))
        memcpy(out, RTA_DATA(a), 4);
    else
        memset(out, 0, 4);
}

/* ---------------------------------------------------------------------------
 * 去重表
 * ------------------------------------------------------------------------- */
static int seen_find(const iv_netlink_t *nl, int ifindex)
{
    int i;

    for (i = 0; i < IV_NETLINK_IFACE_MAX; i++)
        if (nl->seen[i].ifindex == ifindex)
            return i;
    return -1;
}

/* 空槽则占用；表满返回 -1（降级：该接口的 LINK 事件不再去重，见头文件） */
static int seen_claim(iv_netlink_t *nl, int ifindex)
{
    int i = seen_find(nl, ifindex);

    if (i >= 0)
        return i;
    for (i = 0; i < IV_NETLINK_IFACE_MAX; i++) {
        if (nl->seen[i].ifindex == 0) {
            nl->seen[i].ifindex = ifindex;
            nl->seen[i].up      = 0u;
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * 消息分发
 * ------------------------------------------------------------------------- */
static void emit(iv_netlink_t *nl, const iv_netlink_evt_t *evt)
{
    nl->evt_count++;
    nl->on_evt(evt, nl->arg);
}

static void handle_link(iv_netlink_t *nl, const struct nlmsghdr *nh)
{
    const struct ifinfomsg *ifi;
    const struct rtattr    *name_a;
    iv_netlink_evt_t        evt;
    int                     up, slot;

    if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(*ifi)))
        return;
    ifi = (const struct ifinfomsg *)NLMSG_DATA(nh);

    memset(&evt, 0, sizeof(evt));
    evt.kind      = IV_NLEVT_LINK_DOWN;
    evt.ifindex   = ifi->ifi_index;
    evt.link_flags = ifi->ifi_flags;

    name_a = find_attr(nh, sizeof(*ifi), IFLA_IFNAME);
    if (name_a != NULL) {
        size_t n = (size_t)RTA_PAYLOAD(name_a);

        if (n > IV_NETLINK_IFNAME_LEN - 1u)
            n = IV_NETLINK_IFNAME_LEN - 1u;
        memcpy(evt.ifname, RTA_DATA(name_a), n);
        evt.ifname[n] = '\0'; /* 内核保证带 NUL，这里不依赖它 */
    }

    if (nh->nlmsg_type == RTM_DELLINK) {
        /* 接口被移除：上次是 UP 才补一条 DOWN（从未见过的接口直接无声清表） */
        int i = seen_find(nl, evt.ifindex);

        if (i >= 0) {
            if (nl->seen[i].up)
                emit(nl, &evt);
            nl->seen[i].ifindex = 0;
        }
        return;
    }

    /* RTM_NEWLINK：carrier = IFF_UP && IFF_LOWER_UP（取舍理由见头文件）。
     * 去重只对"已见过且状态没变"生效；未见过必报（初见即报＝初始状态同步，
     * 含第一面就是 DOWN 的接口 —— 不能因去重表初值是 0 而把 DOWN 吞掉）。 */
    up = (int)((ifi->ifi_flags & IVNL_KF_UP) &&
               (ifi->ifi_flags & IVNL_KF_LOWER_UP));
    slot = seen_find(nl, evt.ifindex);
    if (slot >= 0 && nl->seen[slot].up == (uint8_t)up)
        return; /* 与上次相同：qdisc/MTU 类噪音，去重 */

    evt.kind = up ? IV_NLEVT_LINK_UP : IV_NLEVT_LINK_DOWN;
    emit(nl, &evt);
    if (slot < 0)
        slot = seen_claim(nl, evt.ifindex); /* 表满 = -1：降级为不再去重 */
    if (slot >= 0)
        nl->seen[slot].up = (uint8_t)up;
}

static void handle_addr(iv_netlink_t *nl, const struct nlmsghdr *nh)
{
    const struct ifaddrmsg *ifa;
    iv_netlink_evt_t        evt;

    if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(*ifa)))
        return;
    ifa = (const struct ifaddrmsg *)NLMSG_DATA(nh);
    if (ifa->ifa_family != AF_INET)
        return; /* 只订阅 IPv4 组，双保险 */

    /* IFA_LOCAL 优先（PPP/点对点上 ADDRESS 是对端地址）；普通接口两者相同 */
    memset(&evt, 0, sizeof(evt));
    evt.kind     = (nh->nlmsg_type == RTM_NEWADDR) ? IV_NLEVT_ADDR_ADD
                                                   : IV_NLEVT_ADDR_DEL;
    evt.ifindex  = (int)ifa->ifa_index;
    evt.family   = AF_INET;
    evt.prefixlen = ifa->ifa_prefixlen;
    if (find_attr(nh, sizeof(*ifa), IFA_LOCAL) != NULL)
        attr_addr4(nh, sizeof(*ifa), IFA_LOCAL, evt.addr);
    else
        attr_addr4(nh, sizeof(*ifa), IFA_ADDRESS, evt.addr);
    emit(nl, &evt);
}

static void handle_route(iv_netlink_t *nl, const struct nlmsghdr *nh)
{
    const struct rtmsg *rt;
    iv_netlink_evt_t    evt;

    if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(*rt)))
        return;
    rt = (const struct rtmsg *)NLMSG_DATA(nh);
    /* 默认路由 = IPv4 + dst_len==0 + 单播。blackhole/prohibit 等特殊类型
     * 以及普通网段路由都不进事件流（S3.3 只关心"往哪走默认出口"）。 */
    if (rt->rtm_family != AF_INET || rt->rtm_dst_len != 0 ||
        rt->rtm_type != RTN_UNICAST)
        return;

    memset(&evt, 0, sizeof(evt));
    evt.kind     = (nh->nlmsg_type == RTM_NEWROUTE) ? IV_NLEVT_ROUTE_ADD
                                                    : IV_NLEVT_ROUTE_DEL;
    evt.ifindex  = (int)attr_u32(nh, sizeof(*rt), RTA_OIF, 0);
    evt.family   = AF_INET;
    evt.prefixlen = 0;
    attr_addr4(nh, sizeof(*rt), RTA_GATEWAY, evt.addr);
    evt.metric = attr_u32(nh, sizeof(*rt), RTA_PRIORITY, 0);
    evt.table  = attr_u32(nh, sizeof(*rt), RTA_TABLE, rt->rtm_table);
    emit(nl, &evt);
}

/* ---------------------------------------------------------------------------
 * 公开接口
 * ------------------------------------------------------------------------- */
int iv_netlink_init(iv_netlink_t *nl, iv_netlink_cb on_evt, void *arg)
{
    if (nl == NULL || on_evt == NULL)
        return IV_EINVAL;

    memset(nl, 0, sizeof(*nl));
    nl->on_evt = on_evt;
    nl->arg    = arg;
    nl->fd     = -1;
    return IV_OK;
}

void iv_netlink_reset(iv_netlink_t *nl)
{
    if (nl == NULL)
        return;
    memset(nl->seen, 0, sizeof(nl->seen));
}

static int socket_err_map(int e)
{
    switch (e) {
    case EACCES:
    case EPERM:
        return IV_EAUTH;
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
        return IV_ENOMEM;
    default:
        return IV_EIO; /* errno 保留原值，调用方可读 */
    }
}

int iv_netlink_attach(iv_netlink_t *nl)
{
    int                fd;
    struct sockaddr_nl sa;

    if (nl == NULL || nl->on_evt == NULL)
        return IV_EINVAL;
    if (nl->fd >= 0)
        return IV_ESTATE;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
                NETLINK_ROUTE);
    if (fd < 0)
        return socket_err_map(errno);

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int e = errno;

        (void)close(fd);
        errno = e;
        return socket_err_map(e);
    }

    nl->fd = fd;
    iv_netlink_reset(nl); /* 新订阅 = 新起点：初见即报充当初始状态同步 */
    return IV_OK;
}

int iv_netlink_fd(const iv_netlink_t *nl)
{
    if (nl == NULL)
        return -1;
    return nl->fd;
}

int iv_netlink_close(iv_netlink_t *nl)
{
    int rc, fd;

    if (nl == NULL)
        return IV_EINVAL;
    if (nl->fd < 0)
        return IV_ESTATE; /* 重复关闭是调用方 bug，不掩盖（同 iv_serial_close） */

    fd = nl->fd;
    nl->fd = -1;
    rc = close(fd);
    return (rc == 0) ? IV_OK : IV_EIO;
}

int iv_netlink_feed(iv_netlink_t *nl, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         off = 0;
    int            overflow = 0;

    if (nl == NULL || nl->on_evt == NULL)
        return IV_EINVAL;
    if (buf == NULL && len > 0)
        return IV_EINVAL;
    if (len == 0)
        return 0;

    nl->evt_count = 0;
    while (len - off >= NLMSG_HDRLEN) {
        const struct nlmsghdr *nh = (const struct nlmsghdr *)(p + off);

        if (nh->nlmsg_len < NLMSG_HDRLEN || nh->nlmsg_len > len - off)
            break; /* 截断/坏长度：余下字节整体作废（数据报语义，无半条报文） */

        switch (nh->nlmsg_type) {
        case RTM_NEWLINK:
        case RTM_DELLINK:
            handle_link(nl, nh);
            break;
        case RTM_NEWADDR:
        case RTM_DELADDR:
            handle_addr(nl, nh);
            break;
        case RTM_NEWROUTE:
        case RTM_DELROUTE:
            handle_route(nl, nh);
            break;
        case NLMSG_ERROR: {
            const struct nlmsgerr *e;

            if (nh->nlmsg_len >= NLMSG_LENGTH(sizeof(*e))) {
                e = (const struct nlmsgerr *)NLMSG_DATA(nh);
                if (e->error == -ENOBUFS)
                    overflow = 1; /* 内核显式告知丢消息：必须重同步 */
            }
            break; /* 其余 ACK/错误回执与订阅无关，静默跳过 */
        }
        default:
            break; /* NLMSG_DONE / 未知类型：跳过 */
        }

        off += NLMSG_ALIGN(nh->nlmsg_len);
    }

    if (overflow)
        return IV_EFULL;
    return nl->evt_count;
}

int iv_netlink_poll(iv_netlink_t *nl)
{
    uint8_t buf[IV_NETLINK_BUF_SIZE];
    int     total = 0, overflow = 0;

    if (nl == NULL || nl->on_evt == NULL)
        return IV_EINVAL;
    if (nl->fd < 0)
        return IV_ESTATE;

    for (;;) {
        struct msghdr mh;
        struct iovec  iov;
        ssize_t       n;
        int           r;

        memset(&mh, 0, sizeof(mh));
        iov.iov_base    = buf;
        iov.iov_len     = sizeof(buf);
        mh.msg_iov      = &iov;
        mh.msg_iovlen   = 1;

        n = recvmsg(nl->fd, &mh, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == ENOBUFS) {
                overflow = 1; /* 内核接收队列溢出：本轮消息不完整 */
                break;
            }
            return IV_EIO; /* errno 保留；已交付事件仍有效 */
        }
        if (n == 0)
            break; /* 理论不至（RAW netlink 无对端关闭语义），防御 */
        if (mh.msg_flags & MSG_TRUNC) {
            overflow = 1; /* 单条报文超过缓冲：无法安全解析，按溢出上报 */
            continue;
        }

        r = iv_netlink_feed(nl, buf, (size_t)n);
        if (r == IV_EFULL) {
            overflow = 1;
            continue;
        }
        if (r < 0)
            return r;
        total += r;
    }

    if (overflow)
        return IV_EFULL;
    return total;
}
