/*
 * iv_netmgr.c —— 双 WAN 状态机实现（libivmodules，M3-S3.3）
 *
 * 依据：架构 §7.2（状态机 + 七步切换事务）、§18.3（无线=usb0 / 有线=eth0，板端实测）。
 * 设计：判定与编排在本模块；具体 I/O 全部经 `iv_netmgr_txn_ops_t` 注入。
 *       默认执行点的 `switch_route` 用 netlink 真改默认路由（见下）。
 * 零 malloc、不引 pthread/Reactor；时间由调用方以 now_ms 传入。
 */

#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ivsbox/iv_netmgr.h"

/* 本模块自己那条默认路由的优先级（metric）。刻意选一个低值（比 DHCP 的 204 小），
 * 使其在两条候选默认路由中胜出；切换时按 (oif, prio) 精确删除旧的那条，
 * 不依赖 DHCP/静态路由的 metric 具体值。 */
#define IV_NETMGR_ROUTE_PRIO 100u

/* 事务失败后的重试保护窗口（ms）：避免状态机每个 tick 反复打事务。 */
#define IV_NETMGR_RETRY_MS 5000u

/* ---------------------------------------------------------------------------
 * 默认值 / 生命周期
 * ------------------------------------------------------------------------- */

void iv_netmgr_cfg_default(iv_netmgr_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    (void)memset(cfg, 0, sizeof(*cfg));
    cfg->mode     = IV_NETMGR_MODE_AUTO;
    cfg->fail_n   = 3u;
    cfg->ok_n     = 2u;
    cfg->hold_s   = 30u;
    cfg->stable_s = 10u;
    cfg->ops      = NULL; /* -> iv_netmgr_ops_default() */
    /* 网卡名不预设：架构 §18.3 冻结为 usb0/eth0，但值由装配层按配置填入 */
}

void iv_netmgr_init(iv_netmgr_t *m)
{
    if (m == NULL) {
        return;
    }
    (void)memset(m, 0, sizeof(*m));
    m->state  = IV_NETMGR_WIRELESS_UP;
    m->active = (uint8_t)IV_WAN_WIRELESS;
}

/* 把 0 值在配置里都翻译成默认（避免 0 语义歧义） */
static void cfg_normalize(iv_netmgr_cfg_t *c)
{
    if ((int)c->mode < 1 || (int)c->mode > 4) {
        c->mode = IV_NETMGR_MODE_AUTO;
    }
    if (c->fail_n == 0u) { c->fail_n = 3u; }
    if (c->ok_n == 0u)   { c->ok_n = 2u; }
    if (c->hold_s == 0u) { c->hold_s = 30u; }
    /* stable_s == 0 合法（立即恢复媒体），不归一 */
    if (c->ops == NULL) {
        c->ops = iv_netmgr_ops_default();
    }
}

int iv_netmgr_open(iv_netmgr_t *m, const iv_netmgr_cfg_t *cfg, uint64_t now_ms)
{
    if (m == NULL || cfg == NULL) {
        return IV_EINVAL;
    }
    if (m->opened) {
        return IV_ESTATE;
    }

    m->cfg = *cfg;
    cfg_normalize(&m->cfg);

    /* 清零运行时状态 */
    m->wan_up[IV_WAN_WIRELESS] = 0;
    m->wan_up[IV_WAN_WIRED]    = 0;
    m->fail_streak[IV_WAN_WIRELESS] = m->fail_streak[IV_WAN_WIRED] = 0u;
    m->ok_streak[IV_WAN_WIRELESS]   = m->ok_streak[IV_WAN_WIRED]   = 0u;
    m->wan_up_since[IV_WAN_WIRELESS] = m->wan_up_since[IV_WAN_WIRED] = 0u;
    m->media_suspended = 0;
    m->media_resume_pending = 0;
    m->media_resume_at = 0;
    m->retry_at = 0;
    m->switches = 0;
    m->txn_fail = 0;
    m->opened = 1;

    /* 初始态：**假定无线可用**（"无线优先"，直到探活证明不是）。
     * 有线在收到第一次探活结论前保持"未知"，不抢先切换。 */
    m->state  = IV_NETMGR_WIRELESS_UP;
    m->active = (uint8_t)IV_WAN_WIRELESS;
    if (m->cfg.mode == IV_NETMGR_MODE_WIRED_ONLY) {
        m->state  = IV_NETMGR_WIRED_UP;
        m->active = (uint8_t)IV_WAN_WIRED;
    }
    if (m->cfg.wan[IV_WAN_WIRELESS].ifindex > 0) {
        m->wan_up[IV_WAN_WIRELESS] = 1;
        m->wan_up_since[IV_WAN_WIRELESS] = now_ms;
    }
    if (m->cfg.mode == IV_NETMGR_MODE_WIRED_ONLY &&
        m->cfg.wan[IV_WAN_WIRED].ifindex > 0) {
        m->wan_up[IV_WAN_WIRED] = 1;
        m->wan_up_since[IV_WAN_WIRED] = now_ms;
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 只读读数
 * ------------------------------------------------------------------------- */

iv_netmgr_state_t iv_netmgr_state(const iv_netmgr_t *m)
{
    return (m == NULL) ? IV_NETMGR_BOTH_DOWN : m->state;
}

iv_wan_t iv_netmgr_active_wan(const iv_netmgr_t *m)
{
    return (m == NULL) ? IV_WAN_WIRELESS : (iv_wan_t)m->active;
}

int iv_netmgr_active_ifindex(const iv_netmgr_t *m)
{
    if (m == NULL) {
        return 0;
    }
    return m->cfg.wan[m->active].ifindex;
}

int iv_netmgr_media_suspended(const iv_netmgr_t *m)
{
    return (m == NULL) ? 0 : (int)m->media_suspended;
}

int iv_netmgr_wan_is_up(const iv_netmgr_t *m, iv_wan_t wan)
{
    if (m == NULL || (int)wan < 0 || (int)wan >= IV_WAN_N) {
        return 0;
    }
    return (int)m->wan_up[wan];
}

uint64_t iv_netmgr_switches(const iv_netmgr_t *m)
{
    return (m == NULL) ? 0u : m->switches;
}

uint64_t iv_netmgr_txn_fail(const iv_netmgr_t *m)
{
    return (m == NULL) ? 0u : m->txn_fail;
}

const char *iv_netmgr_state_name(iv_netmgr_state_t s)
{
    switch (s) {
    case IV_NETMGR_WIRELESS_UP:       return "WIRELESS_UP";
    case IV_NETMGR_WIRELESS_DEGRADED: return "WIRELESS_DEGRADED";
    case IV_NETMGR_SWITCH_TO_WIRED:   return "SWITCH_TO_WIRED";
    case IV_NETMGR_WIRED_UP:          return "WIRED_UP";
    case IV_NETMGR_SWITCH_TO_WIRELESS:return "SWITCH_TO_WIRELESS";
    case IV_NETMGR_BOTH_DOWN:         return "BOTH_DOWN";
    default:                          return "?";
    }
}

/* ---------------------------------------------------------------------------
 * 七步事务（架构 §7.2）
 *   1 冻结低优先级发送 -> 2 切默认出口 -> 3 关旧接口 TCP -> 4~6 新接口重连
 *   （含重解析/建连/鉴权/补传）-> 7 稳定期后恢复媒体（排给 tick，不在此同步做）
 * ------------------------------------------------------------------------- */
static iv_netmgr_state_t steady_of(iv_wan_t w)
{
    return (w == IV_WAN_WIRED) ? IV_NETMGR_WIRED_UP : IV_NETMGR_WIRELESS_UP;
}

static int run_txn(iv_netmgr_t *m, iv_wan_t to, uint64_t now_ms)
{
    const iv_netmgr_txn_ops_t *o = m->cfg.ops;
    iv_wan_t from = (iv_wan_t)m->active;
    int from_if = m->cfg.wan[from].ifindex;
    int to_if   = m->cfg.wan[to].ifindex;
    int rc = IV_OK;

    m->state = (to == IV_WAN_WIRED) ? IV_NETMGR_SWITCH_TO_WIRED
                                    : IV_NETMGR_SWITCH_TO_WIRELESS;

    if (o != NULL) {
        /* 1 冻结低优先级（媒体）发送 */
        if (o->freeze_low_prio != NULL) {
            m->media_suspended = 1;
            if (o->freeze_low_prio(o->arg) != IV_OK) {
                rc = IV_EIO;
            }
        }
        /* 2 切默认出口 */
        if (rc == IV_OK && o->switch_route != NULL) {
            if (o->switch_route(o->arg, from_if, to_if,
                                m->cfg.wan[to].gateway_be) != IV_OK) {
                rc = IV_EIO;
            }
        }
        /* 3 关旧接口上的 TCP（含平台连接与远程流） */
        if (rc == IV_OK && o->close_old_tcp != NULL) {
            if (o->close_old_tcp(o->arg, from_if) != IV_OK) {
                rc = IV_EIO;
            }
        }
        /* 4~6 新接口重连（重解析地址 / 建连 / 鉴权与恢复 / 按优先级补传） */
        if (rc == IV_OK && o->reconnect != NULL) {
            if (o->reconnect(o->arg, to_if) != IV_OK) {
                rc = IV_EIO;
            }
        }
    }

    if (rc != IV_OK) {
        /* 事务失败：回滚冻结、保持原活动口，并设重试保护窗口 */
        m->media_suspended = 0;
        m->media_resume_pending = 0;
        m->txn_fail++;
        m->retry_at = now_ms + (uint64_t)IV_NETMGR_RETRY_MS;
        m->state = m->wan_up[from] ? steady_of(from) : IV_NETMGR_BOTH_DOWN;
        return rc;
    }

    /* 7 恢复媒体：排到稳定期后由 tick 触发 */
    m->media_resume_pending = m->media_suspended;
    m->media_resume_at = now_ms + (uint64_t)m->cfg.stable_s * 1000u;

    m->active = (uint8_t)to;
    m->switches++;
    m->state = steady_of(to);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 状态推进
 * ------------------------------------------------------------------------- */
static void evaluate(iv_netmgr_t *m, uint64_t now_ms)
{
    int wl = (int)m->wan_up[IV_WAN_WIRELESS];
    int wd = (int)m->wan_up[IV_WAN_WIRED];

    /* ---- 模式：不跑自动切换的三种 ---- */
    if (m->cfg.mode == IV_NETMGR_MODE_WIRED_ONLY) {
        m->active = (uint8_t)IV_WAN_WIRED;
        m->state = wd ? IV_NETMGR_WIRED_UP : IV_NETMGR_BOTH_DOWN;
        return;
    }
    if (m->cfg.mode == IV_NETMGR_MODE_WIRELESS_ONLY) {
        m->active = (uint8_t)IV_WAN_WIRELESS;
        if (wl) {
            m->state = IV_NETMGR_WIRELESS_UP;
        } else if (m->state == IV_NETMGR_WIRELESS_UP) {
            m->state = IV_NETMGR_WIRELESS_DEGRADED;
        } else {
            m->state = IV_NETMGR_BOTH_DOWN;
        }
        return;
    }
    if (m->cfg.mode == IV_NETMGR_MODE_BOTH) {
        /* 双链路启用、**不自动切换**：活动口＝无线优先，状态只反映可用性 */
        m->active = (uint8_t)IV_WAN_WIRELESS;
        m->state = wl ? IV_NETMGR_WIRELESS_UP
                      : (wd ? IV_NETMGR_WIRELESS_DEGRADED : IV_NETMGR_BOTH_DOWN);
        return;
    }

    /* ---- AUTO：§7.2 完整状态机 ---- */
    if (m->retry_at != 0u && now_ms < m->retry_at) {
        return; /* 事务失败后的重试保护窗口内不重复打事务 */
    }

    switch (m->state) {
    case IV_NETMGR_WIRELESS_UP:
        if (!wl) {
            if (wd) {
                (void)run_txn(m, IV_WAN_WIRED, now_ms);
            } else {
                m->state = IV_NETMGR_WIRELESS_DEGRADED;
            }
        }
        break;

    case IV_NETMGR_WIRELESS_DEGRADED:
        if (wl) {
            m->state = IV_NETMGR_WIRELESS_UP;         /* 无线自愈 */
        } else if (wd) {
            (void)run_txn(m, IV_WAN_WIRED, now_ms);   /* 业务仍失败 -> 切有线 */
        } else {
            m->state = IV_NETMGR_BOTH_DOWN;
        }
        break;

    case IV_NETMGR_WIRED_UP:
        if (!wd) {
            if (wl) {
                (void)run_txn(m, IV_WAN_WIRELESS, now_ms); /* 有线断、无线在 -> 立即回切 */
            } else {
                m->state = IV_NETMGR_BOTH_DOWN;
            }
        } else if (wl &&
                   (now_ms - m->wan_up_since[IV_WAN_WIRELESS]) >=
                       (uint64_t)m->cfg.hold_s * 1000u) {
            (void)run_txn(m, IV_WAN_WIRELESS, now_ms);     /* 无线稳定 hold_s -> 回切 */
        }
        break;

    case IV_NETMGR_BOTH_DOWN:
        if (wl) {
            (void)run_txn(m, IV_WAN_WIRELESS, now_ms);     /* 无线优先 */
        } else if (wd) {
            (void)run_txn(m, IV_WAN_WIRED, now_ms);
        }
        break;

    default:
        /* SWITCH_TO_* 只在 run_txn 内瞬时出现，不在此停留 */
        break;
    }
}

/* ---------------------------------------------------------------------------
 * 对外驱动
 * ------------------------------------------------------------------------- */

int iv_netmgr_note_probe(iv_netmgr_t *m, iv_wan_t wan, int ok, uint64_t now_ms)
{
    if (m == NULL || !m->opened || (int)wan < 0 || (int)wan >= IV_WAN_N) {
        return IV_EINVAL;
    }

    if (ok) {
        m->fail_streak[wan] = 0u;
        if (m->ok_streak[wan] != 0xFFFFFFFFu) {
            m->ok_streak[wan]++;
        }
        if (!m->wan_up[wan] && m->ok_streak[wan] >= m->cfg.ok_n) {
            m->wan_up[wan] = 1;
            m->wan_up_since[wan] = now_ms;
        }
    } else {
        m->ok_streak[wan] = 0u;
        if (m->fail_streak[wan] != 0xFFFFFFFFu) {
            m->fail_streak[wan]++;
        }
        if (m->wan_up[wan] && m->fail_streak[wan] >= m->cfg.fail_n) {
            m->wan_up[wan] = 0;
            m->wan_up_since[wan] = 0u;
        }
    }

    evaluate(m, now_ms);
    return IV_OK;
}

int iv_netmgr_tick(iv_netmgr_t *m, uint64_t now_ms)
{
    if (m == NULL || !m->opened) {
        return IV_EINVAL;
    }

    /* 稳定期到点 -> 恢复媒体上传/大文件下载 */
    if (m->media_resume_pending && now_ms >= m->media_resume_at) {
        const iv_netmgr_txn_ops_t *o = m->cfg.ops;
        if (o != NULL && o->resume_media != NULL) {
            (void)o->resume_media(o->arg, m->cfg.wan[m->active].ifindex);
        }
        m->media_suspended = 0;
        m->media_resume_pending = 0;
    }

    evaluate(m, now_ms);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 默认执行点：switch_route 走 netlink（RTM_NEWROUTE / RTM_DELROUTE）
 *
 * 口径：本模块自己那条默认路由固定用 IV_NETMGR_ROUTE_PRIO（metric 100），
 *       切换 = 删旧口那条(prio=100) + 建新口那条(prio=100)。
 *       不碰 DHCP/静态路由（它们的 metric 各异），也不需要知道它们的具体值。
 * ------------------------------------------------------------------------- */

struct nl_req {
    struct nlmsghdr nlh;
    struct rtmsg    rtm;
    char            buf[128];
};

static int nl_attr_add(struct nlmsghdr *nlh, size_t cap, int type,
                       const void *data, int len)
{
    int alen = RTA_LENGTH(len);
    int nlen = (int)NLMSG_ALIGN(nlh->nlmsg_len) + (int)RTA_ALIGN((size_t)alen);
    struct rtattr *rta;

    if ((size_t)nlen > cap) {
        return IV_ERANGE;
    }
    rta = (struct rtattr *)(void *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len  = (unsigned short)alen;
    (void)memcpy(RTA_DATA(rta), data, (size_t)len);
    nlh->nlmsg_len = (unsigned int)nlen;
    return IV_OK;
}

static int nl_route_one(int fd, int action, int ifindex, uint32_t gateway_be)
{
    struct nl_req req;
    struct sockaddr_nl dst;
    char ack[256];
    struct iovec iov;
    struct msghdr msg;
    ssize_t n;
    uint32_t prio = IV_NETMGR_ROUTE_PRIO;

    if (ifindex <= 0) {
        return IV_EINVAL;
    }

    (void)memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nlh.nlmsg_type  = (unsigned short)action;
    /* 必须带 NLM_F_ACK：netlink 约定"不带 ACK 只在出错时回消息"，而本函数随后
     * 做阻塞 recv —— 少了它，操作**成功**（正常路径）时内核不回复 ⇒ 永久阻塞。
     * 该缺陷只在 netns 内用真实网卡才暴露（假 ifindex 会在 sendmsg 前就返回）。 */
    req.nlh.nlmsg_flags = (unsigned short)((action == RTM_NEWROUTE)
                            ? (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE)
                            : (NLM_F_REQUEST | NLM_F_ACK));
    req.nlh.nlmsg_seq   = 1;
    req.rtm.rtm_family  = AF_INET;
    req.rtm.rtm_dst_len = 0;                       /* 默认路由 */
    req.rtm.rtm_table   = RT_TABLE_MAIN;
    req.rtm.rtm_protocol = RTPROT_BOOT;
    req.rtm.rtm_scope   = RT_SCOPE_UNIVERSE;
    req.rtm.rtm_type    = RTN_UNICAST;

    if (nl_attr_add(&req.nlh, sizeof(req), RTA_OIF, &ifindex, (int)sizeof(ifindex)) != IV_OK) {
        return IV_ERANGE;
    }
    if (nl_attr_add(&req.nlh, sizeof(req), RTA_PRIORITY, &prio, (int)sizeof(prio)) != IV_OK) {
        return IV_ERANGE;
    }
    if (action == RTM_NEWROUTE && gateway_be != 0u) {
        if (nl_attr_add(&req.nlh, sizeof(req), RTA_GATEWAY, &gateway_be,
                        (int)sizeof(gateway_be)) != IV_OK) {
            return IV_ERANGE;
        }
    }

    (void)memset(&dst, 0, sizeof(dst));
    dst.nl_family = AF_NETLINK;
    (void)memset(&iov, 0, sizeof(iov));
    iov.iov_base = &req;
    iov.iov_len  = req.nlh.nlmsg_len;
    (void)memset(&msg, 0, sizeof(msg));
    msg.msg_name    = &dst;
    msg.msg_namelen = (socklen_t)sizeof(dst);
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;

    n = sendmsg(fd, &msg, 0);
    if (n < 0) {
        return IV_EIO;
    }
    n = recv(fd, ack, sizeof(ack), 0);
    if (n < 0) {
        return IV_EIO;
    }
    {
        const struct nlmsghdr *rh = (const struct nlmsghdr *)(const void *)ack;
        if (rh->nlmsg_type == NLMSG_ERROR && rh->nlmsg_len >= NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
            const struct nlmsgerr *e = (const struct nlmsgerr *)NLMSG_DATA(rh);
            if (e->error != 0) {
                return (e->error == -ENOENT && action == RTM_DELROUTE) ? IV_OK : IV_EIO;
            }
        }
    }
    return IV_OK;
}

static int real_switch_route(void *arg, int from_ifindex, int to_ifindex,
                             uint32_t to_gateway_be)
{
    int fd;
    int rc;

    (void)arg;
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        return IV_EIO;
    }
    {
        /* 防御性收包超时：万一内核不回 ack，也不让本线程永久阻塞（1s 后退化为失败、
         * 由状态机在重试窗口后再来）。正常的 REQUEST|ACK 请求内核必定回包。 */
        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    /* 先删旧口那条（没有也算成功：ENOENT 被吞掉），再建新口那条 */
    (void)nl_route_one(fd, RTM_DELROUTE, from_ifindex, 0u);
    rc = nl_route_one(fd, RTM_NEWROUTE, to_ifindex, to_gateway_be);
    (void)close(fd);
    return rc;
}

static const iv_netmgr_txn_ops_t g_default_ops = {
    NULL,                /* freeze_low_prio：由装配层补 */
    real_switch_route,   /* switch_route：netlink 真改默认出口 */
    NULL,                /* close_old_tcp：由装配层补（S3.5 连接关闭） */
    NULL,                /* reconnect：由装配层补 */
    NULL,                /* resume_media：由装配层补 */
    NULL
};

const iv_netmgr_txn_ops_t *iv_netmgr_ops_default(void)
{
    return &g_default_ops;
}
