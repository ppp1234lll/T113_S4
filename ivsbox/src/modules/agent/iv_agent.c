/*
 * M3 装配层实现。口径、取舍与数据流见 include/ivsbox/iv_agent.h。
 *
 * 只做三件事：按配置 open 四条链、把回调互相转接、按固定节拍驱动 tick。
 * 不含业务逻辑（策略都在 iv_probe / iv_netmgr / iv_report 内）。
 */
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_version.h"

#define AG_MOD "agent"

/* ---------------------------------------------------------------------------
 * 内部回调前向声明
 * ------------------------------------------------------------------------- */
static void on_nl_evt(const iv_netlink_evt_t *e, void *arg);
static void on_nl_fd(int fd, uint32_t events, void *arg);
static void on_pb_timer(void *arg);
static void on_nm_timer(void *arg);
static int  agent_reconnect(void *arg, int new_ifindex);

static void probe_drive(iv_agent_t *a, uint64_t now_ms);
static void netmgr_feed(iv_agent_t *a, uint64_t now_ms);

/* 接口名 → 内核 ifindex；未解析到返回 0（该 WAN 视为未绑定） */
static int wan_ifindex_of(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    return (int)if_nametoindex(name);
}

/* ---------------------------------------------------------------------------
 * 配置 / 生命周期
 * ------------------------------------------------------------------------- */
void iv_agent_cfg_default(iv_agent_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->serial_path  = IV_REPORT_SERIAL_DEFAULT;
    cfg->queue_dir    = IV_QUEUE_DIR_DEFAULT;
    cfg->devtype      = IV_PROTO_DEVTYPE_DEFAULT;
    cfg->report_ms    = IV_REPORT_REPORT_MS;
    cfg->heartbeat_ms = IV_REPORT_HEARTBEAT_MS;
    cfg->poll_ms      = IV_REPORT_POLL_MS;
    (void)snprintf(cfg->wan_name[IV_WAN_WIRELESS], IV_AGENT_WAN_NAME_MAX, "%s", "usb0");
    (void)snprintf(cfg->wan_name[IV_WAN_WIRED], IV_AGENT_WAN_NAME_MAX, "%s", "eth0");
    cfg->mode = IV_NETMGR_MODE_AUTO;
}

void iv_agent_init(iv_agent_t *a)
{
    int w;

    if (a == NULL)
        return;
    memset(a, 0, sizeof(*a));
    iv_report_init(&a->rp);
    for (w = 0; w < IV_WAN_N; w++)
        a->wan_probe_idx[w] = -1;
    a->opened = 0;
}

int iv_agent_open(iv_agent_t *a, const iv_agent_cfg_t *cfg, iv_reactor_t *r,
                  uint64_t now_ms)
{
    iv_report_cfg_t            rc;
    iv_probe_cfg_t             pc;
    iv_netmgr_cfg_t            mc;
    const iv_netmgr_txn_ops_t *dflt;
    uint32_t                   dst;
    int                        w;
    int                        arc;

    if (a == NULL || cfg == NULL)
        return IV_EINVAL;
    if (a->opened)
        return IV_ESTATE;

    iv_agent_init(a);
    a->cfg    = *cfg;
    a->r      = r;
    a->now_ms = now_ms;
    for (w = 0; w < IV_WAN_N; w++)
        a->wan_ifindex[w] = wan_ifindex_of(cfg->wan_name[w]);

    /* ---- 1. 控制板上报链（S3.6：内含 serial/frame/link/status/proto/queue/transport）---- */
    iv_report_cfg_default(&rc);
    rc.serial_path  = (cfg->serial_path != NULL) ? cfg->serial_path : IV_REPORT_SERIAL_DEFAULT;
    rc.server_host  = cfg->server_host;
    rc.server_port  = cfg->server_port;
    rc.queue_dir    = cfg->queue_dir;
    rc.mod          = cfg->mod;
    rc.sv           = (cfg->sv != NULL) ? cfg->sv : iv_version_string();
    rc.devid        = cfg->devid;
    rc.devtype      = cfg->devtype;
    rc.report_ms    = cfg->report_ms;
    rc.heartbeat_ms = cfg->heartbeat_ms;
    rc.poll_ms      = cfg->poll_ms;
    arc = iv_report_open(&a->rp, &rc, r, now_ms);
    if (arc != IV_OK) {
        IV_LOG_E(AG_MOD, "report open failed: %d", arc);
        return arc;
    }

    /* ---- 2. netlink（拿不到 socket 不致命：LINK 层降级为不驱动）---- */
    if (iv_netlink_init(&a->nl, on_nl_evt, a) != IV_OK) {
        IV_LOG_W(AG_MOD, "netlink init failed, link-layer probe disabled");
    } else if (iv_netlink_attach(&a->nl) != IV_OK) {
        IV_LOG_W(AG_MOD, "netlink attach failed, link-layer probe disabled");
    } else if (r != NULL) {
        a->nl_ev = iv_reactor_add(r, iv_netlink_fd(&a->nl), IV_EV_READ,
                                  on_nl_fd, a);
        if (a->nl_ev == NULL) {
            IV_LOG_W(AG_MOD, "netlink fd add to reactor failed");
            (void)iv_netlink_close(&a->nl);
        }
    }

    /* ---- 3. probe（每条 WAN 一项：有目标走 ICMP，否则被动 LINK）---- */
    iv_probe_cfg_default(&pc);
    if (cfg->probe_interval_ms != 0u)
        pc.interval_ms = cfg->probe_interval_ms;
    if (cfg->probe_timeout_ms != 0u)
        pc.timeout_ms = cfg->probe_timeout_ms;
    if (cfg->probe_fail_n != 0u)
        pc.fail_n = cfg->probe_fail_n;
    (void)iv_probe_init(&a->pb, &pc);
    dst = (cfg->probe_target_be[0] != 0u) ? cfg->probe_target_be[0]
                                          : cfg->probe_target_be[1];
    for (w = 0; w < IV_WAN_N; w++) {
        int idx;
        if (a->wan_ifindex[w] == 0)
            continue; /* 接口不存在：无项可建，该 WAN 恒 UNKNOWN */
        idx = (dst != 0u)
                  ? iv_probe_add_icmp(&a->pb, dst, a->wan_ifindex[w], cfg->wan_name[w])
                  : iv_probe_add_link(&a->pb, a->wan_ifindex[w], cfg->wan_name[w]);
        a->wan_probe_idx[w] = (idx >= 0) ? idx : -1;
        if (idx < 0)
            IV_LOG_W(AG_MOD, "probe item add failed for wan %s (%d)",
                     cfg->wan_name[w], idx);
    }
    if (iv_probe_start(&a->pb, now_ms) != IV_OK)
        IV_LOG_W(AG_MOD, "probe start failed (icmp may be unavailable)");

    /* ---- 4. netmgr（switch_route 复用默认；reconnect 接上报链）---- */
    iv_netmgr_cfg_default(&mc);
    mc.mode = (cfg->mode != 0) ? cfg->mode : IV_NETMGR_MODE_AUTO;
    for (w = 0; w < IV_WAN_N; w++) {
        mc.wan[w].ifindex = a->wan_ifindex[w];
        (void)snprintf(mc.wan[w].name, sizeof(mc.wan[w].name), "%s",
                       cfg->wan_name[w]);
        /* 默认网关待 netlink 路由事件补齐（见 on_nl_evt） */
    }
    mc.fail_n   = cfg->nm_fail_n;
    mc.ok_n     = cfg->nm_ok_n;
    mc.hold_s   = cfg->nm_hold_s;
    mc.stable_s = cfg->nm_stable_s;

    memset(&a->ops, 0, sizeof(a->ops));
    dflt = iv_netmgr_ops_default();
    if (dflt != NULL)
        a->ops.switch_route = dflt->switch_route;
    /* reconnect 已含"先关旧 TCP"（close_old_tcp 置空，避免重复断连） */
    a->ops.reconnect = agent_reconnect;
    a->ops.arg       = a;
    mc.ops           = &a->ops;

    (void)iv_netmgr_init(&a->nm);
    if (iv_netmgr_open(&a->nm, &mc, now_ms) != IV_OK)
        IV_LOG_W(AG_MOD, "netmgr open failed");

    /* ---- 5. 本层定时器 ---- */
    if (r != NULL) {
        a->pb_timer = iv_timer_add(r, IV_AGENT_PROBE_TICK_MS, on_pb_timer, a);
        a->nm_timer = iv_timer_add(r, IV_AGENT_NETMGR_TICK_MS, on_nm_timer, a);
        if (a->pb_timer == NULL || a->nm_timer == NULL)
            IV_LOG_W(AG_MOD, "agent timer add failed");
    }

    a->opened = 1;
    IV_LOG_I(AG_MOD,
             "agent up: wan0=%s(if%d) wan1=%s(if%d) mode=%d server=%s:%u",
             cfg->wan_name[IV_WAN_WIRELESS], a->wan_ifindex[IV_WAN_WIRELESS],
             cfg->wan_name[IV_WAN_WIRED], a->wan_ifindex[IV_WAN_WIRED],
             (int)mc.mode,
             (cfg->server_host != NULL) ? cfg->server_host : "(none)",
             (unsigned)cfg->server_port);
    return IV_OK;
}

int iv_agent_step(iv_agent_t *a, uint64_t now_ms)
{
    if (a == NULL)
        return IV_EINVAL;
    if (!a->opened)
        return IV_ESTATE;
    a->now_ms = now_ms;
    probe_drive(a, now_ms);
    netmgr_feed(a, now_ms);
    (void)iv_netmgr_tick(&a->nm, now_ms);
    (void)iv_report_step(&a->rp, now_ms);
    return IV_OK;
}

void iv_agent_close(iv_agent_t *a)
{
    if (a == NULL || !a->opened)
        return;

    if (a->r != NULL) {
        if (a->nm_timer != NULL) {
            (void)iv_timer_cancel(a->r, a->nm_timer);
            a->nm_timer = NULL;
        }
        if (a->pb_timer != NULL) {
            (void)iv_timer_cancel(a->r, a->pb_timer);
            a->pb_timer = NULL;
        }
        if (a->nl_ev != NULL) {
            (void)iv_reactor_del(a->r, a->nl_ev);
            a->nl_ev = NULL;
        }
    }

    iv_report_close(&a->rp); /* 摘 serial/tick 事件、关传输与队列 */
    iv_netmgr_init(&a->nm);  /* netmgr 无 close：init 复位 */
    if (a->pb.started)
        (void)iv_probe_stop(&a->pb);
    if (iv_netlink_fd(&a->nl) >= 0)
        (void)iv_netlink_close(&a->nl);

    a->opened = 0;
    a->r      = NULL;
}

/* ---------------------------------------------------------------------------
 * 驱动：probe（发探/收结果/超时）与 netmgr（喂结论 + 状态推进）
 * ------------------------------------------------------------------------- */
static void probe_drive(iv_agent_t *a, uint64_t now_ms)
{
    int n;
    int i;

    (void)iv_probe_tick(&a->pb, now_ms);

    /* 活动 fd 逐个补可读/可写处理；两者对"不认识的 fd"只返回错误码、不误处理，
     * 故循环调用安全（见 iv_agent.h 的取舍说明）。 */
    n = iv_probe_fd_count(&a->pb);
    for (i = 0; i < n; i++) {
        int fd = iv_probe_fd_at(&a->pb, i);
        if (fd < 0)
            continue;
        (void)iv_probe_on_readable(&a->pb, fd, now_ms);
        (void)iv_probe_on_writable(&a->pb, fd, now_ms);
    }
}

static void netmgr_feed(iv_agent_t *a, uint64_t now_ms)
{
    int w;

    for (w = 0; w < IV_WAN_N; w++) {
        iv_probe_state_t s;
        int              idx = a->wan_probe_idx[w];

        if (idx < 0)
            continue;
        s = iv_probe_state(&a->pb, idx);
        if (s == IV_PROBE_UNKNOWN)
            continue; /* 未探测过：不喂，避免把"未知"当"失败" */
        (void)iv_netmgr_note_probe(&a->nm, (iv_wan_t)w,
                                   (s == IV_PROBE_UP || s == IV_PROBE_DEGRADED)
                                       ? 1
                                       : 0,
                                   now_ms);
    }
}

/* ---------------------------------------------------------------------------
 * Reactor 回调
 * ------------------------------------------------------------------------- */
static void on_nl_fd(int fd, uint32_t events, void *arg)
{
    iv_agent_t *a = arg;
    int         n;

    (void)fd;
    if (events & (IV_EV_ERR | IV_EV_HUP | IV_EV_RDHUP)) {
        IV_LOG_W(AG_MOD, "netlink fd error/hup");
        return;
    }
    n = iv_netlink_poll(&a->nl); /* 事件在 poll 内经 on_nl_evt 逐个回调 */
    if (n == IV_EFULL)
        IV_LOG_W(AG_MOD, "netlink overflow: kernel may have dropped events");
}

static void on_nl_evt(const iv_netlink_evt_t *e, void *arg)
{
    iv_agent_t *a = arg;
    int         w;

    switch (e->kind) {
    case IV_NLEVT_LINK_UP:
    case IV_NLEVT_LINK_DOWN: {
        int up = (e->kind == IV_NLEVT_LINK_UP) ? 1 : 0;

        for (w = 0; w < IV_WAN_N; w++) {
            if (a->wan_ifindex[w] == e->ifindex)
                a->wan_link_up[w] = (uint8_t)up;
        }
        /* 即时重判绑定该接口的 probe 项（拔线要秒级可见） */
        (void)iv_probe_set_link(&a->pb, e->ifindex, up, a->now_ms);
        break;
    }
    case IV_NLEVT_ROUTE_ADD:
    case IV_NLEVT_ROUTE_DEL: {
        uint32_t gw = 0;

        if (e->kind == IV_NLEVT_ROUTE_ADD)
            memcpy(&gw, e->addr, 4u); /* addr = 网关（网络序原样） */
        for (w = 0; w < IV_WAN_N; w++) {
            if (a->wan_ifindex[w] != 0 && a->wan_ifindex[w] == e->ifindex)
                a->nm.cfg.wan[w].gateway_be = gw;
        }
        break;
    }
    default:
        /* ADDR_ADD/DEL：主网地址由 DHCP 管理，本步不消费 */
        break;
    }
}

static void on_pb_timer(void *arg)
{
    iv_agent_t *a   = arg;
    uint64_t    now = iv_clock_monotonic_ms();

    a->now_ms = now;
    probe_drive(a, now);
    if (a->r != NULL)
        a->pb_timer = iv_timer_add(a->r, IV_AGENT_PROBE_TICK_MS, on_pb_timer, a);
}

static void on_nm_timer(void *arg)
{
    iv_agent_t *a   = arg;
    uint64_t    now = iv_clock_monotonic_ms();

    a->now_ms = now;
    netmgr_feed(a, now);
    (void)iv_netmgr_tick(&a->nm, now);
    if (a->r != NULL)
        a->nm_timer = iv_timer_add(a->r, IV_AGENT_NETMGR_TICK_MS, on_nm_timer, a);
}

/* netmgr 切换事务的 reconnect 步：断旧 TCP → 用原址重开（队列数据保留） */
static int agent_reconnect(void *arg, int new_ifindex)
{
    iv_agent_t *a = arg;

    (void)new_ifindex; /* 按活动出口选平台地址待平台语义冻结后接入 */
    return iv_report_reconnect(&a->rp, a->now_ms);
}

/* ---------------------------------------------------------------------------
 * 只读读数
 * ------------------------------------------------------------------------- */
int iv_agent_is_open(const iv_agent_t *a)
{
    return (a != NULL) ? (int)a->opened : 0;
}

iv_netmgr_state_t iv_agent_netmgr_state(const iv_agent_t *a)
{
    return (a != NULL) ? iv_netmgr_state(&a->nm) : IV_NETMGR_BOTH_DOWN;
}

iv_wan_t iv_agent_active_wan(const iv_agent_t *a)
{
    return (a != NULL) ? iv_netmgr_active_wan(&a->nm) : IV_WAN_WIRELESS;
}

int iv_agent_active_ifindex(const iv_agent_t *a)
{
    return (a != NULL) ? iv_netmgr_active_ifindex(&a->nm) : 0;
}

iv_report_t *iv_agent_report(iv_agent_t *a)
{
    return (a != NULL) ? &a->rp : NULL;
}
