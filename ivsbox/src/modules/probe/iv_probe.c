/*
 * 统一探活引擎实现（libivmodules，功能开发计划 M3-S3.2）
 *
 * 职责、四层语义、统一输出与全部契约见 `include/ivsbox/iv_probe.h` 文件头；
 * 这里只补四条实现层面的说明：
 *
 * 1) **不引 pthread / Reactor**：本层是 socket 薄包装 ＋ 状态机，全部 socket 走
 *    `iv_probe_iops_t` 注入（生产默认 libc），非阻塞。fd 经 `iv_probe_fd_at()`
 *    交出，由调用方挂自己的事件循环。时间一律由调用方传入（`now_ms`）。
 *
 * 2) **ICMP 是引擎共享的一条 raw socket**：多个 ICMP 项共用一个 fd，靠
 *    `(id, seq)` 匹配回包；`id` 取固定值 `IV_PROBE_ICMP_ID`，`seq` 每项各自递增。
 *    开不出 raw socket（EPERM/EACCES）时不报错，置 `icmp_disabled` 降级。
 *
 * 3) **TCP 是"一问一拆"**：每发起一次 TCP 探测开一条非阻塞 socket、connect，
 *    无论成功失败**立即 close**（只验证路通端口开，不做应用层交互）。因此 TCP
 *    fd 只在探测进行中存在，`iv_probe_fd_count()` 随之动态变化。
 *
 * 4) **判定只在一处**（`recompute`）：链路判死 / ICMP 降级 / 连续失败 / 连续成功 /
 *    劣化，全部归口到它，`note()` 与 `set_link()` 只负责改统计再调它。避免多处
 *    各判一套导致口径漂移。
 */
#include <errno.h>
#include <string.h>

#include <arpa/inet.h>       /* htons / ntohs */
#include <netinet/in.h>
#include <netinet/ip.h>      /* struct iphdr（raw ICMP 收包带 IP 头） */
#include <netinet/ip_icmp.h> /* struct icmphdr / ICMP_ECHO */
#include <sys/socket.h>
#include <unistd.h>

#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_ret.h"

/* ---------------------------------------------------------------------------
 * 默认注入点（生产路径）
 * ------------------------------------------------------------------------- */
static int real_socket(int domain, int type, int proto)
{
    return socket(domain, type, proto);
}

static int real_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    return connect(fd, addr, len);
}

static ssize_t real_sendto(int fd, const void *buf, size_t len, int flags,
                           const struct sockaddr *addr, socklen_t alen)
{
    return sendto(fd, buf, len, flags, addr, alen);
}

static ssize_t real_recv(int fd, void *buf, size_t len, int flags)
{
    return recv(fd, buf, len, flags);
}

static int real_getsockopt(int fd, int level, int optname, void *optval,
                           socklen_t *optlen)
{
    return getsockopt(fd, level, optname, optval, optlen);
}

static int real_close(int fd)
{
    return close(fd);
}

static uint64_t real_now_ms(void)
{
    return iv_clock_monotonic_ms();
}

static const iv_probe_iops_t g_default_iops = {
    real_socket, real_connect, real_sendto, real_recv,
    real_getsockopt, real_close, real_now_ms
};

const iv_probe_iops_t *iv_probe_default_iops(void)
{
    return &g_default_iops;
}

/* ---------------------------------------------------------------------------
 * 配置默认值
 * ------------------------------------------------------------------------- */
void iv_probe_cfg_default(iv_probe_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->interval_ms           = 5000u;
    cfg->timeout_ms            = 1000u;
    cfg->fail_n                = 3u;
    cfg->ok_n                  = 2u;
    cfg->degrade_rtt_ms        = 800u;
    cfg->degrade_loss_permille = 300u;
    cfg->iops                  = NULL;
}

/* ---------------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------------- */

/* 有界字符串拷贝：始终 NUL 结尾，不做字节级溢出（-Wstringop-truncation 安全） */
static void copy_name(char *dst, const char *src)
{
    size_t n = 0;

    if (src != NULL) {
        while (n < IV_PROBE_NAME_LEN - 1u && src[n] != '\0')
            n++;
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static int link_is_down(const iv_probe_t *p, int ifindex)
{
    int i;

    if (ifindex == 0)
        return 0; /* 未绑定接口：不受链路事件影响 */
    for (i = 0; i < IV_PROBE_MAX_LINKS; i++) {
        if (p->link_ifindex[i] == ifindex)
            return p->link_up[i] ? 0 : 1;
    }
    return 0; /* 未知接口不判死（防止一个没上报过的 ifindex 把目标冤枉成 DOWN） */
}

/* 滑窗写入（环形）。ok!=0 记成功与 RTT，否则记失败（RTT 槽清 0）。 */
static void win_push(iv_probe_item_t *it, int ok, uint32_t rtt)
{
    it->win_ok[it->win_pos]  = (uint8_t)(ok ? 1u : 0u);
    it->win_rtt[it->win_pos] = ok ? rtt : 0u;
    it->win_pos = (it->win_pos + 1) % IV_PROBE_WIN_LEN;
    if (it->win_cnt < IV_PROBE_WIN_LEN)
        it->win_cnt++;
}

/* 由滑窗重算 avg_rtt_ms 与 loss_permille（覆盖 win_cnt 个最新样本）。 */
static void win_recalc(iv_probe_item_t *it)
{
    int      i;
    int      nok = 0;
    uint64_t sum = 0;

    for (i = 0; i < it->win_cnt; i++) {
        int idx = (it->win_pos - it->win_cnt + i + IV_PROBE_WIN_LEN) % IV_PROBE_WIN_LEN;

        if (it->win_ok[idx]) {
            nok++;
            sum += it->win_rtt[idx];
        }
    }
    it->avg_rtt_ms = nok ? (uint32_t)(sum / (uint64_t)nok) : 0u;
    it->loss_permille = it->win_cnt
        ? (uint32_t)((uint32_t)(it->win_cnt - nok) * 1000u / (uint32_t)it->win_cnt)
        : 0u;
}

/* 唯一判定入口：由统计 + 链路 + ICMP 降级推出 state/reason。 */
static void recompute(iv_probe_t *p, iv_probe_item_t *it)
{
    /* LINK 项：状态就是它监听接口的 carrier 状态 */
    if (it->layer == IV_PROBE_LINK) {
        int i, found = 0, up = 0;

        for (i = 0; i < IV_PROBE_MAX_LINKS; i++) {
            if (it->ifindex != 0 && p->link_ifindex[i] == it->ifindex) {
                found = 1;
                up    = p->link_up[i];
                break;
            }
        }
        if (!found) {
            it->state  = IV_PROBE_UNKNOWN;
            it->reason = IV_PROBE_R_NONE;
        } else {
            it->state  = up ? IV_PROBE_UP : IV_PROBE_DOWN;
            it->reason = up ? IV_PROBE_R_OK : IV_PROBE_R_LINK_DOWN;
        }
        return;
    }

    /* 绑定接口链路 down：立即判死，不等计数 */
    if (link_is_down(p, it->ifindex)) {
        it->state  = IV_PROBE_DOWN;
        it->reason = IV_PROBE_R_LINK_DOWN;
        return;
    }
    /* 链路恢复：清掉 LINK_DOWN 覆盖，回到按统计判（此前若已是 UP，立即复 UP；
     * 从未探过则回 UNKNOWN，而不是把 LINK_DOWN 这个原因一直挂着）。 */
    if (it->reason == IV_PROBE_R_LINK_DOWN) {
        it->reason = IV_PROBE_R_NONE;
        it->state  = IV_PROBE_UNKNOWN;
    }

    /* ICMP 层被内核拒绝：本层不参与判定，保持 UNKNOWN（绝不算 DOWN） */
    if (it->layer == IV_PROBE_ICMP && p->icmp_disabled) {
        it->state  = IV_PROBE_UNKNOWN;
        it->reason = IV_PROBE_R_NOPERM;
        return;
    }

    if (it->fail_streak >= p->cfg.fail_n) {
        it->state = IV_PROBE_DOWN; /* reason 保留最后一次失败原因 */
        return;
    }
    if (it->ok_streak >= p->cfg.ok_n) {
        int bad = 0;

        if (p->cfg.degrade_rtt_ms != 0u && it->avg_rtt_ms > p->cfg.degrade_rtt_ms)
            bad = 1;
        if (p->cfg.degrade_loss_permille != 0u &&
            it->loss_permille > p->cfg.degrade_loss_permille)
            bad = 1;
        it->state  = bad ? IV_PROBE_DEGRADED : IV_PROBE_UP;
        it->reason = IV_PROBE_R_OK;
        return;
    }

    /* 样本不足以判 UP/DOWN：保持既有状态（首次即 UNKNOWN），reason 不动。 */
}

/* 记录一次探测结果并重判。fail_reason==NONE 时保留上一次失败原因。 */
static void note(iv_probe_t *p, iv_probe_item_t *it, int ok, uint32_t rtt,
                 iv_probe_reason_t fail_reason, uint64_t now)
{
    if (it->layer != IV_PROBE_LINK)
        it->sent++;
    win_push(it, ok, rtt);
    win_recalc(it);

    if (ok) {
        it->last_rtt_ms = rtt;
        it->last_ok_ms  = now;
        it->ok_streak++;
        it->fail_streak = 0;
        it->reason      = IV_PROBE_R_OK;
    } else {
        it->lost++;
        it->fail_streak++;
        it->ok_streak = 0;
        if (fail_reason != IV_PROBE_R_NONE)
            it->reason = fail_reason;
    }
    recompute(p, it);
}

/* errno -> 原因码（TCP 连接失败 / sendto 失败共用） */
static iv_probe_reason_t errno_reason(int e)
{
    switch (e) {
    case ECONNREFUSED:
        return IV_PROBE_R_REFUSED;
    case ENETUNREACH:
    case EHOSTUNREACH:
        return IV_PROBE_R_UNREACH;
    case EPERM:
    case EACCES:
        return IV_PROBE_R_NOPERM;
    case ETIMEDOUT:
        return IV_PROBE_R_TIMEOUT;
    default:
        return IV_PROBE_R_IOERR;
    }
}

/* 结束一次进行中的 TCP 探测：关掉 socket、清 inflight。 */
static void tcp_finish(const iv_probe_iops_t *io, iv_probe_item_t *it)
{
    if (it->fd >= 0) {
        (void)io->close(it->fd);
        it->fd = -1;
    }
    it->inflight = 0;
}

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */
int iv_probe_init(iv_probe_t *p, const iv_probe_cfg_t *cfg)
{
    int i;

    if (p == NULL)
        return IV_EINVAL;

    memset(p, 0, sizeof(*p));
    if (cfg != NULL)
        p->cfg = *cfg;
    else
        iv_probe_cfg_default(&p->cfg);

    /* 归一化：0 视为默认；iops==NULL 取默认 */
    if (p->cfg.interval_ms == 0u)
        p->cfg.interval_ms = 5000u;
    if (p->cfg.timeout_ms == 0u)
        p->cfg.timeout_ms = 1000u;
    if (p->cfg.fail_n == 0u)
        p->cfg.fail_n = 3u;
    if (p->cfg.ok_n == 0u)
        p->cfg.ok_n = 2u;
    if (p->cfg.iops == NULL)
        p->cfg.iops = iv_probe_default_iops();

    p->icmp_fd = -1;
    for (i = 0; i < IV_PROBE_MAX_ITEMS; i++)
        p->item[i].fd = -1;
    return IV_OK;
}

void iv_probe_reset(iv_probe_t *p)
{
    int i;

    if (p == NULL)
        return;
    for (i = 0; i < IV_PROBE_MAX_ITEMS; i++) {
        iv_probe_item_t *it = &p->item[i];

        if (it->fd >= 0 && p->cfg.iops != NULL)
            (void)p->cfg.iops->close(it->fd);
        it->fd = -1;
    }
    p->n_items        = 0;
    p->started        = 0;
    p->icmp_disabled  = 0;
    if (p->icmp_fd >= 0 && p->cfg.iops != NULL)
        (void)p->cfg.iops->close(p->icmp_fd);
    p->icmp_fd = -1;
    memset(p->link_ifindex, 0, sizeof(p->link_ifindex));
    memset(p->link_up, 0, sizeof(p->link_up));
}

/* ---------------------------------------------------------------------------
 * 项管理
 * ------------------------------------------------------------------------- */
static int item_alloc(iv_probe_t *p, iv_probe_layer_t layer, int ifindex,
                      const char *name)
{
    iv_probe_item_t *it;

    if (p->n_items >= IV_PROBE_MAX_ITEMS)
        return IV_EFULL;
    it = &p->item[p->n_items];
    memset(it, 0, sizeof(*it));
    it->layer       = layer;
    it->ifindex     = ifindex;
    it->fd          = -1;
    it->interval_ms = p->cfg.interval_ms;
    it->state       = IV_PROBE_UNKNOWN;
    it->reason      = IV_PROBE_R_NONE;
    copy_name(it->name, name);
    p->n_items++;
    return p->n_items - 1;
}

/* 重复检测：同层且同目标（ICMP/TCP）视为重复 */
static int find_dup(const iv_probe_t *p, iv_probe_layer_t layer, uint32_t dst_be,
                    uint16_t port)
{
    int i;

    for (i = 0; i < p->n_items; i++) {
        const iv_probe_item_t *it = &p->item[i];

        if (it->layer != layer)
            continue;
        if (layer == IV_PROBE_ICMP && it->dst_be == dst_be)
            return i;
        if (layer == IV_PROBE_TCP && it->dst_be == dst_be && it->port == port)
            return i;
    }
    return -1;
}

int iv_probe_add_link(iv_probe_t *p, int ifindex, const char *name)
{
    if (p == NULL)
        return IV_EINVAL;
    if (ifindex <= 0)
        return IV_EINVAL;
    return item_alloc(p, IV_PROBE_LINK, ifindex, name);
}

int iv_probe_add_icmp(iv_probe_t *p, uint32_t dst_be, int ifindex, const char *name)
{
    int idx;

    if (p == NULL)
        return IV_EINVAL;
    if (find_dup(p, IV_PROBE_ICMP, dst_be, 0) >= 0)
        return IV_EEXIST;
    idx = item_alloc(p, IV_PROBE_ICMP, ifindex, name);
    if (idx < 0)
        return idx;
    p->item[idx].dst_be = dst_be;
    return idx;
}

int iv_probe_add_tcp(iv_probe_t *p, uint32_t dst_be, uint16_t port, int ifindex,
                     const char *name)
{
    int idx;

    if (p == NULL)
        return IV_EINVAL;
    if (port == 0u)
        return IV_EINVAL;
    if (find_dup(p, IV_PROBE_TCP, dst_be, port) >= 0)
        return IV_EEXIST;
    idx = item_alloc(p, IV_PROBE_TCP, ifindex, name);
    if (idx < 0)
        return idx;
    p->item[idx].dst_be = dst_be;
    p->item[idx].port   = port;
    return idx;
}

int iv_probe_add_app(iv_probe_t *p, int ifindex, const char *name)
{
    if (p == NULL)
        return IV_EINVAL;
    return item_alloc(p, IV_PROBE_APP, ifindex, name);
}

/* ---------------------------------------------------------------------------
 * 启停
 * ------------------------------------------------------------------------- */
int iv_probe_start(iv_probe_t *p, uint64_t now_ms)
{
    int i, has_icmp = 0;

    if (p == NULL)
        return IV_EINVAL;
    if (p->started)
        return IV_ESTATE;

    for (i = 0; i < p->n_items; i++) {
        if (p->item[i].layer == IV_PROBE_ICMP)
            has_icmp = 1;
    }

    if (has_icmp && !p->icmp_disabled) {
        int fd = p->cfg.iops->socket(AF_INET,
                                     SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                     IPPROTO_ICMP);
        if (fd < 0)
            p->icmp_disabled = 1; /* 无权限/不支持：降级，不报错（见文件头） */
        else
            p->icmp_fd = fd;
    }

    /* 首轮立即探测（next_due = now），随后按 interval 周期 */
    for (i = 0; i < p->n_items; i++) {
        iv_probe_item_t *it = &p->item[i];

        it->next_due_ms = now_ms;
        it->inflight    = 0;
        if (it->layer == IV_PROBE_LINK)
            recompute(p, it); /* LINK 项：先按当前链路表定位 */
    }
    p->started = 1;
    return IV_OK;
}

int iv_probe_stop(iv_probe_t *p)
{
    int i;

    if (p == NULL)
        return IV_EINVAL;
    if (!p->started)
        return IV_ESTATE;

    if (p->icmp_fd >= 0) {
        (void)p->cfg.iops->close(p->icmp_fd);
        p->icmp_fd = -1;
    }
    for (i = 0; i < p->n_items; i++) {
        iv_probe_item_t *it = &p->item[i];

        if (it->fd >= 0)
            (void)p->cfg.iops->close(it->fd);
        it->fd       = -1;
        it->inflight = 0;
    }
    p->started = 0;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * fd 交出
 * ------------------------------------------------------------------------- */
int iv_probe_fd_count(const iv_probe_t *p)
{
    int i, n = 0;

    if (p == NULL)
        return 0;
    if (p->icmp_fd >= 0)
        n++;
    for (i = 0; i < p->n_items; i++) {
        if (p->item[i].fd >= 0)
            n++;
    }
    return n;
}

int iv_probe_fd_at(const iv_probe_t *p, int i)
{
    int k = 0, j;

    if (p == NULL || i < 0)
        return -1;
    if (p->icmp_fd >= 0) {
        if (k == i)
            return p->icmp_fd;
        k++;
    }
    for (j = 0; j < p->n_items; j++) {
        if (p->item[j].fd >= 0) {
            if (k == i)
                return p->item[j].fd;
            k++;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * ICMP 报文与收发
 * ------------------------------------------------------------------------- */
#define IV_PROBE_ICMP_ID ((uint16_t)0x4950u) /* "IP"：本引擎专属 echo id */

typedef struct {
    struct icmphdr hdr;
    uint8_t        payload[IV_PROBE_ICMP_PAYLOAD];
} icmp_pkt_t;

static uint16_t icmp_checksum(const void *data, size_t len)
{
    const uint8_t *p   = (const uint8_t *)data;
    uint32_t       sum = 0;

    while (len > 1u) {
        sum += (uint32_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
        p += 2;
        len -= 2u;
    }
    if (len == 1u)
        sum += (uint32_t)((uint16_t)p[0] << 8);
    while ((sum >> 16) != 0u)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

/* 找绑定某 ifindex 或该 ICMP 项（按 seq）。回包匹配用。 */
static iv_probe_item_t *icmp_find_by_seq(iv_probe_t *p, uint16_t seq)
{
    int i;

    for (i = 0; i < p->n_items; i++) {
        iv_probe_item_t *it = &p->item[i];

        if (it->layer == IV_PROBE_ICMP && it->inflight && it->icmp_echo_seq == seq)
            return it;
    }
    return NULL;
}

static void start_icmp(iv_probe_t *p, iv_probe_item_t *it, uint64_t now)
{
    icmp_pkt_t          pkt;
    struct sockaddr_in  sa;
    ssize_t             n;

    if (p->icmp_disabled || p->icmp_fd < 0) {
        recompute(p, it); /* 走 icmp_disabled 分支 -> UNKNOWN，不判死 */
        it->next_due_ms = now + it->interval_ms;
        return;
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.type             = ICMP_ECHO;
    pkt.hdr.code             = 0;
    pkt.hdr.un.echo.id       = htons(IV_PROBE_ICMP_ID);
    it->icmp_echo_seq        = (uint16_t)(it->icmp_echo_seq + 1u);
    pkt.hdr.un.echo.sequence = htons(it->icmp_echo_seq);
    /* icmp_checksum() 按大端字求得的 16 位校验和是主机序整数，而
     * icmphdr.checksum 是 __be16（网络序字段）=> 必须 htons 后再落字段。
     * 漏掉 htons 时只有校验和恰为回文（如 0xAEAE）的报文才偶然正确，
     * 其余被内核以 InCsumErrors 丢弃、回包全丢。 */
    pkt.hdr.checksum         = htons(icmp_checksum(&pkt, sizeof(pkt)));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = it->dst_be;

    n = p->cfg.iops->sendto(p->icmp_fd, &pkt, sizeof(pkt), 0,
                            (struct sockaddr *)&sa, sizeof(sa));
    if (n < 0) {
        if (errno == EPERM || errno == EACCES) {
            p->icmp_disabled = 1; /* 运行时才发现被拒：同样降级 */
            recompute(p, it);
        }
        /* 发送失败不占 inflight：记一次失败并等下一轮 */
        note(p, it, 0, 0u, errno_reason(errno), now);
        it->next_due_ms = now + it->interval_ms;
        return;
    }
    it->sent_at_ms = now;
    it->inflight   = 1;
}

static void start_tcp(iv_probe_t *p, iv_probe_item_t *it, uint64_t now)
{
    struct sockaddr_in sa;
    int                fd, rc;

    fd = p->cfg.iops->socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        note(p, it, 0, 0u, errno_reason(errno), now);
        it->next_due_ms = now + it->interval_ms;
        return;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(it->port);
    sa.sin_addr.s_addr = it->dst_be;

    rc = p->cfg.iops->connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0) {
        /* 立刻连通（同机/极近端）：RTT 记 0，立即成功 */
        (void)p->cfg.iops->close(fd);
        note(p, it, 1, 0u, IV_PROBE_R_NONE, now);
        it->next_due_ms = now + it->interval_ms;
        return;
    }
    if (errno == EINPROGRESS || errno == EALREADY) {
        it->fd         = fd;
        it->sent_at_ms = now;
        it->inflight   = 1;
        return;
    }

    /* 立即失败（拒绝/不可达） */
    {
        int e = errno;

        (void)p->cfg.iops->close(fd);
        note(p, it, 0, 0u, errno_reason(e), now);
    }
    it->next_due_ms = now + it->interval_ms;
}

/* ---------------------------------------------------------------------------
 * 事件驱动
 * ------------------------------------------------------------------------- */
int iv_probe_on_readable(iv_probe_t *p, int fd, uint64_t now_ms)
{
    uint8_t rbuf[512];
    int     handled = 0;

    if (p == NULL)
        return IV_EINVAL;
    if (fd < 0 || fd != p->icmp_fd)
        return IV_ESTATE;

    for (;;) {
        ssize_t n = p->cfg.iops->recv(fd, rbuf, sizeof(rbuf), 0);
        const struct iphdr   *iph;
        const struct icmphdr *ih;
        size_t                ihl;
        uint16_t              id, seq;
        iv_probe_item_t      *it;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return IV_EIO; /* 已处理的回包仍有效 */
        }
        if (n == 0)
            break;
        if ((size_t)n < sizeof(struct iphdr))
            continue;
        iph = (const struct iphdr *)rbuf;
        ihl = (size_t)iph->ihl * 4u;
        if (iph->ihl < 5u || (size_t)n < ihl + sizeof(struct icmphdr))
            continue;
        ih = (const struct icmphdr *)(rbuf + ihl);
        if (ih->type != ICMP_ECHOREPLY)
            continue;
        id = ntohs(ih->un.echo.id);
        if (id != IV_PROBE_ICMP_ID)
            continue; /* 不是本引擎发的（raw socket 收全部 ICMP） */
        seq = ntohs(ih->un.echo.sequence);
        it  = icmp_find_by_seq(p, seq);
        if (it == NULL)
            continue; /* 迟到的回包（已超时判负） */
        it->inflight = 0;
        note(p, it, 1, (uint32_t)(now_ms - it->sent_at_ms), IV_PROBE_R_NONE, now_ms);
        it->next_due_ms = now_ms + it->interval_ms;
        handled++;
    }
    return handled;
}

int iv_probe_on_writable(iv_probe_t *p, int fd, uint64_t now_ms)
{
    int i, sockerr = 0;
    socklen_t slen = sizeof(sockerr);

    if (p == NULL)
        return IV_EINVAL;

    for (i = 0; i < p->n_items; i++) {
        iv_probe_item_t *it = &p->item[i];

        if (it->fd != fd || !it->inflight)
            continue;

        if (p->cfg.iops->getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &slen) != 0)
            sockerr = errno;
        tcp_finish(p->cfg.iops, it);
        if (sockerr == 0)
            note(p, it, 1, (uint32_t)(now_ms - it->sent_at_ms), IV_PROBE_R_NONE, now_ms);
        else
            note(p, it, 0, 0u, errno_reason(sockerr), now_ms);
        it->next_due_ms = now_ms + it->interval_ms;
        return 1;
    }
    return 0; /* fd 不是本引擎在探测的（可能已超时判负并 close） */
}

int iv_probe_tick(iv_probe_t *p, uint64_t now_ms)
{
    int i, changed = 0;

    if (p == NULL)
        return IV_EINVAL;
    if (!p->started)
        return IV_ESTATE;

    for (i = 0; i < p->n_items; i++) {
        iv_probe_item_t *it = &p->item[i];
        iv_probe_state_t before;

        if (it->layer == IV_PROBE_LINK || it->layer == IV_PROBE_APP)
            continue; /* 被动层不主动探测 */

        before = it->state;

        if (it->inflight && now_ms >= it->sent_at_ms &&
            (now_ms - it->sent_at_ms) >= p->cfg.timeout_ms) {
            tcp_finish(p->cfg.iops, it); /* ICMP 无 fd，tcp_finish 是 no-op */
            note(p, it, 0, 0u, IV_PROBE_R_TIMEOUT, now_ms);
            it->next_due_ms = now_ms + it->interval_ms;
        }

        if (!it->inflight && now_ms >= it->next_due_ms) {
            if (it->layer == IV_PROBE_ICMP)
                start_icmp(p, it, now_ms);
            else
                start_tcp(p, it, now_ms);
        }

        if (it->state != before)
            changed++;
    }
    return changed;
}

/* ---------------------------------------------------------------------------
 * 结果 / 链路事件注入
 * ------------------------------------------------------------------------- */
int iv_probe_note_result(iv_probe_t *p, int idx, int ok, uint32_t rtt_ms,
                         uint64_t now_ms)
{
    iv_probe_item_t *it;

    if (p == NULL || idx < 0 || idx >= p->n_items)
        return IV_EINVAL;
    it = &p->item[idx];
    note(p, it, ok, rtt_ms, ok ? IV_PROBE_R_NONE : IV_PROBE_R_APP_FAIL, now_ms);
    return IV_OK;
}

int iv_probe_set_link(iv_probe_t *p, int ifindex, int up, uint64_t now_ms)
{
    int i, slot = -1, changed = 0;

    (void)now_ms;
    if (p == NULL)
        return IV_EINVAL;
    if (ifindex <= 0)
        return IV_EINVAL;

    for (i = 0; i < IV_PROBE_MAX_LINKS; i++) {
        if (p->link_ifindex[i] == ifindex) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (i = 0; i < IV_PROBE_MAX_LINKS; i++) {
            if (p->link_ifindex[i] == 0) {
                slot = i;
                p->link_ifindex[i] = ifindex;
                break;
            }
        }
    }
    if (slot < 0)
        return IV_EFULL; /* 链路表满：不误判，交由上层扩展 */

    p->link_up[slot] = up ? 1u : 0u;

    /* 即时重判：绑定该接口的项 + 监听该接口的 LINK 项 */
    for (i = 0; i < p->n_items; i++) {
        iv_probe_item_t *it = &p->item[i];
        iv_probe_state_t before;

        if (it->ifindex != ifindex)
            continue;
        before = it->state;
        if (!up) {
            /* 链路断：作废进行中的探测，免得到期后又报成功 */
            tcp_finish(p->cfg.iops, it);
        }
        recompute(p, it);
        if (it->state != before)
            changed++;
    }
    return changed;
}

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */
int iv_probe_stat_get(const iv_probe_t *p, int idx, iv_probe_stat_t *out)
{
    const iv_probe_item_t *it;

    if (p == NULL || out == NULL || idx < 0 || idx >= p->n_items)
        return IV_EINVAL;
    it = &p->item[idx];

    memset(out, 0, sizeof(*out));
    out->state       = it->state;
    out->reason      = it->reason;
    out->last_rtt_ms = it->last_rtt_ms;
    out->avg_rtt_ms  = it->avg_rtt_ms;
    out->loss_permille = it->loss_permille;
    out->ok_streak   = it->ok_streak;
    out->fail_streak = it->fail_streak;
    out->last_ok_ms  = it->last_ok_ms;
    out->sent        = it->sent;
    out->lost        = it->lost;
    return IV_OK;
}

iv_probe_state_t iv_probe_state(const iv_probe_t *p, int idx)
{
    if (p == NULL || idx < 0 || idx >= p->n_items)
        return IV_PROBE_UNKNOWN;
    return p->item[idx].state;
}

int iv_probe_count(const iv_probe_t *p)
{
    return (p == NULL) ? 0 : p->n_items;
}

int iv_probe_icmp_available(const iv_probe_t *p)
{
    return (p == NULL) ? 0 : (p->icmp_disabled ? 0 : 1);
}

const char *iv_probe_state_name(iv_probe_state_t s)
{
    switch (s) {
    case IV_PROBE_UP:
        return "up";
    case IV_PROBE_DOWN:
        return "down";
    case IV_PROBE_DEGRADED:
        return "degraded";
    case IV_PROBE_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *iv_probe_reason_name(iv_probe_reason_t r)
{
    switch (r) {
    case IV_PROBE_R_OK:
        return "ok";
    case IV_PROBE_R_TIMEOUT:
        return "timeout";
    case IV_PROBE_R_REFUSED:
        return "refused";
    case IV_PROBE_R_UNREACH:
        return "unreach";
    case IV_PROBE_R_NOPERM:
        return "noperm";
    case IV_PROBE_R_LINK_DOWN:
        return "link_down";
    case IV_PROBE_R_APP_FAIL:
        return "app_fail";
    case IV_PROBE_R_IOERR:
        return "ioerr";
    case IV_PROBE_R_NONE:
    default:
        return "none";
    }
}
