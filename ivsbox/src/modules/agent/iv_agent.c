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
#include "ivsbox/iv_proto.h"
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
static void camdet_feed(iv_agent_t *a);
static void on_cfg_search_mode(const iv_proto_frame_t *f, void *arg);
static int  agent_send_power(const void *data, uint16_t len, void *arg);
static void on_camdet_change(int ch, iv_camdet_status_t old_st,
                             iv_camdet_status_t new_st, void *arg);
static void on_door_open(int ds, void *arg);

/* 接口名 → 内核 ifindex；未解析到返回 0（该 WAN 视为未绑定） */
static int wan_ifindex_of(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    return (int)if_nametoindex(name);
}

/* 初始活动出口：与 iv_netmgr 的起始态对齐 —— 只有"仅有线"模式从有线起步，
 * 其余（自动 / 仅无线 / 双 WAN）都以无线为初始活动口（netmgr 初始态 WIRELESS_UP）。 */
static iv_wan_t agent_initial_wan(iv_netmgr_mode_t mode)
{
    return (mode == IV_NETMGR_MODE_WIRED_ONLY) ? IV_WAN_WIRED : IV_WAN_WIRELESS;
}

/* 按指定出口取平台地址：**本口没配则回退另一口**；两口都没配 → NULL（不启平台链路）。
 * port 出参同步置为选中那条的端口（未选中时置 0）。 */
static const char *agent_pick_host(const iv_agent_cfg_t *cfg, int w, uint16_t *port)
{
    int i;

    for (i = 0; i < IV_WAN_N; i++) {
        int         x = (i == 0)
                            ? w
                            : ((w == IV_WAN_WIRELESS) ? IV_WAN_WIRED : IV_WAN_WIRELESS);
        const char *h = cfg->srv_host[x];

        if (h != NULL && h[0] != '\0' && cfg->srv_port[x] != 0u) {
            if (port != NULL)
                *port = cfg->srv_port[x];
            return h;
        }
    }
    if (port != NULL)
        *port = 0u;
    return NULL;
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
    iv_netmgr_mode_t           mode;
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
    mode = (cfg->mode != 0) ? (iv_netmgr_mode_t)cfg->mode : IV_NETMGR_MODE_AUTO;
    iv_report_cfg_default(&rc);
    rc.serial_path  = (cfg->serial_path != NULL) ? cfg->serial_path : IV_REPORT_SERIAL_DEFAULT;
    /* 平台地址按**初始活动出口**取（本口没配则回退另一口；两口都空 ⇒ 不启平台链路） */
    rc.server_host  = agent_pick_host(cfg, agent_initial_wan(mode), &rc.server_port);
    rc.queue_dir    = cfg->queue_dir;
    rc.mod          = cfg->mod;
    rc.sv           = (cfg->sv != NULL) ? cfg->sv : iv_version_string();
    rc.devid        = cfg->devid;
    rc.devtype      = cfg->devtype;
    rc.report_ms    = cfg->report_ms;
    rc.heartbeat_ms = cfg->heartbeat_ms;
    rc.poll_ms      = cfg->poll_ms;
    rc.on_door      = on_door_open; /* S4.5：门开 → 抓拍钩子（M4 阶段空实现） */
    rc.on_door_arg  = a;
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

    /* ---- 3b. 开局摸底（必须）----
     * netlink 只报"变化"、不回放现状（见 iv_netlink.h 取舍 2），而 netmgr_feed()
     * 对 IV_PROBE_UNKNOWN 是"不喂"。若本进程启动时两条 WAN 已经 UP（板端开机就是
     * 如此：udev 拨 4G、eth0 早已 UP），probe 的 LINK 项会一直停在 UNKNOWN，状态机
     * 便"看不见"任何可用 WAN —— 4G 掉线时切不到本来可用的 eth0（2026-10-12 板端
     * 真链路验收查出）。这里在装配完成时同步探一次链路可用性播进去
     * （判据 IFF_UP && IFF_RUNNING；为何不是事件路径的 LOWER_UP，见
     * iv_netlink.h 里 iv_netlink_link_up() 的说明）。 */
    for (w = 0; w < IV_WAN_N; w++) {
        int up = 0;

        if (a->wan_probe_idx[w] < 0)
            continue;
        if (iv_netlink_link_up(cfg->wan_name[w], &up) != IV_OK) {
            IV_LOG_W(AG_MOD, "link seed failed for wan %s", cfg->wan_name[w]);
            continue;
        }
        (void)iv_probe_set_link(&a->pb, a->wan_ifindex[w], up, now_ms);
        IV_LOG_I(AG_MOD, "link seed: %s %s", cfg->wan_name[w], up ? "up" : "down");
    }

    /* ---- 4. netmgr（switch_route 复用默认；reconnect 接上报链）---- */
    iv_netmgr_cfg_default(&mc);
    mc.mode = mode;
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
        /* ---- 摄像机网络状态探测（S4.1）---- */
        {
            iv_camdet_cfg_t ccfg;

            iv_camdet_cfg_default(&ccfg);
            if (cfg->camera_detect_mode != 0u)
                ccfg.mode = (int)cfg->camera_detect_mode;
            if (cfg->camera_detect_rtt_ms != 0u)
                ccfg.rtt_thr_ms = cfg->camera_detect_rtt_ms;
            /* S4.4：把 "正常 → 故障" 的沿直接接进自愈模块。回调里只做
             * "判断 ＋ 入队"，不做任何 I/O；真正的 0xD1 由下面 step/timer
             * 里的 iv_recovery_tick 每拍最多发 1 条（同 cmd 在飞唯一）。 */
            ccfg.on_change = on_camdet_change;
            ccfg.cb_arg    = a;
            iv_camdet_init(&a->cd, &ccfg);
            if (iv_camdet_open(&a->cd, NULL, now_ms) != IV_OK) {
                IV_LOG_W(AG_MOD, "camdet open failed");
            } else if (iv_camdet_set_channels(&a->cd, cfg->camera_ip_be,
                                              now_ms) < 0) {
                IV_LOG_W(AG_MOD, "camdet channel table rejected");
            }
            /* 摄像机在有线侧：开局同步一次 carrier（现网：网线断开 ⇒ 已配 IP 的一律报 2） */
            {
                int cup = 0;

                if (cfg->wan_name[IV_WAN_WIRED][0] != 0 &&
                    iv_netlink_link_up(cfg->wan_name[IV_WAN_WIRED], &cup) == IV_OK) {
                    (void)iv_camdet_set_link(&a->cd, cup, now_ms);
                    IV_LOG_I(AG_MOD, "camdet link seed: %s %s",
                             cfg->wan_name[IV_WAN_WIRED], cup ? "up" : "down");
                }
            }
            /* S4.4 自愈：出厂默认**禁用**（reboot_max=0 ⇒ 对齐现网
             * net_reload=0）；打开需配 `netthr.reboot_max` >= 1。动作经
             * iv_report_send_cmd 走本层自持的 iv_link（0xD1 属控制命令，
             * iv_link 只发一次、不重传）。 */
            {
                iv_recovery_cfg_t rcfg;

                iv_recovery_cfg_default(&rcfg);
                rcfg.reboot_max  = (int)cfg->netthr_reboot_max;
                rcfg.cooldown_ms = (cfg->netthr_reboot_interval_s != 0u)
                                       ? cfg->netthr_reboot_interval_s * 1000u
                                       : 0u;
                rcfg.tx     = agent_send_power;
                rcfg.tx_arg = a;
                iv_recovery_init(&a->rec, &rcfg);
                IV_LOG_I(AG_MOD, "recovery: reboot_max=%d cooldown=%us",
                         (int)cfg->netthr_reboot_max,
                         (unsigned)cfg->netthr_reboot_interval_s);
            }

            /* 平台命令 0xA5（CONFIGURE_SEARCH_MODE）切换检测方式 */
            (void)iv_proto_route(&a->rp.pf, IV_PROTO_CMD_CFG_SEARCH,
                                 on_cfg_search_mode, a);
        }

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
             (rc.server_host != NULL) ? rc.server_host : "(none)",
             (unsigned)rc.server_port);
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
    if (iv_camdet_is_open(&a->cd))
        (void)iv_camdet_tick(&a->cd, now_ms);
    camdet_feed(a);
    (void)iv_recovery_tick(&a->rec, now_ms);
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

        iv_camdet_close(&a->cd);  /* 停在飞 socket + 关引擎 */
    iv_recovery_close(&a->rec);
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
/* S4.4 动作出口：把 iv_recovery 造好的 0xD1 载荷交给本层自持的 iv_link。
 * IV_EBUSY（同 cmd 在飞）原样上抛 —— iv_recovery 会保留队头、下一拍再试。 */
static int agent_send_power(const void *data, uint16_t len, void *arg)
{
    iv_agent_t *a = arg;

    return iv_report_send_cmd(&a->rp, IV_FRAME_CMD_POWER, data, len, a->now_ms);
}

/* S4.1 状态沿 → S4.4：把该路摄像机的 old/new 原样交下去；"是否动作"
 * （单沿/冷却/限次/禁用）由 iv_recovery 一处判定，这里不长第二套策略。 */
static void on_camdet_change(int ch, iv_camdet_status_t old_st,
                             iv_camdet_status_t new_st, void *arg)
{
    iv_agent_t *a = arg;

    if (a == NULL)
        return;
    iv_recovery_on_camera_change(&a->rec, ch, (int)old_st, (int)new_st,
                                 a->now_ms);
}

/* S4.5 门开事件出口：`0xC1`（箱门开启）到达时由 iv_status 同步触发。
 * **M4 阶段 ＝ 空实现 ＋ 日志 ＋ 计数**：抓图本体在 M6-S6.2（经本地通道
 * `snapshot.take` 交 `ivsbox-media` 抠图），到时把这里换成派发即可；
 * 无鉴权 / 无审计 / 无去重表（S4.5 口径）。
 * 回调在装配层单线程内同步执行 ⇒ 这里**只允许无阻塞动作**（架构 §4.2）。 */
static void on_door_open(int ds, void *arg)
{
    iv_agent_t *a = arg;

    if (a == NULL)
        return;
    a->door_events++;
    IV_LOG_I(AG_MOD,
             "door open event #%llu (DS=%d): snapshot hook not wired (M6-S6.2)",
             (unsigned long long)a->door_events, ds);
}

/* 把摄像机五态编码注入上报层的 CNS 字段（闭合 S3.6 的"CNS 恒 0"遗留）：
 * 只改 cns[]，其余外部事实（CSQ/定位/故障码）原样保留。 */
static void camdet_feed(iv_agent_t *a)
{
    iv_report_env_t env;
    int             cns[IV_CAMDET_CH_N];
    int             i;

    if (!iv_camdet_is_open(&a->cd))
        return;
    if (iv_camdet_cns(&a->cd, cns) != IV_OK)
        return;
    env = a->rp.env;
    for (i = 0; i < IV_REPORT_CNS_N && i < IV_CAMDET_CH_N; i++)
        env.cns[i] = cns[i];
    iv_report_set_env(&a->rp, &env);
}

/* 平台 0xA5（配置搜索方式）：DATA[0] = 1/2/3，非法回 0x74。
 * 与现网 com_set_carema_search_mode_function() 同口径（那儿也是取 buff[0]）。 */
static void on_cfg_search_mode(const iv_proto_frame_t *f, void *arg)
{
    iv_agent_t *a = (iv_agent_t *)arg;
    int         rc;

    if (f == NULL || a == NULL)
        return;
    if (f->data == NULL || f->len < 1u) {
        (void)iv_proto_ack(&a->rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    rc = iv_camdet_set_mode(&a->cd, (int)f->data[0], a->now_ms);
    (void)iv_proto_ack(&a->rp.pf, f->cmd,
                       (rc >= 0) ? IV_PROTO_ACK_OK : IV_PROTO_ERR_EXEC);
    IV_LOG_I(AG_MOD, "platform set detect mode %u -> rc=%d",
             (unsigned)f->data[0], rc);
}

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
        /* 网线状态直接覆盖摄像机结论（现网口径） */
        if (a->wan_ifindex[IV_WAN_WIRED] != 0 &&
            a->wan_ifindex[IV_WAN_WIRED] == e->ifindex)
            (void)iv_camdet_set_link(&a->cd, up, a->now_ms);
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
    if (iv_camdet_is_open(&a->cd))
        (void)iv_camdet_tick(&a->cd, now);
    camdet_feed(a);
    (void)iv_recovery_tick(&a->rec, now);
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

/* netmgr 切换事务的 reconnect 步：按新出口换平台地址 → 断旧 TCP → 按新址重开
 * （队列未确认数据全部保留，由取件泵补传）。 */
static int agent_reconnect(void *arg, int new_ifindex)
{
    iv_agent_t *a = arg;
    const char *h;
    uint16_t    port = 0u;
    int         w    = IV_WAN_WIRELESS; /* 兜底：接口识别不出时按无线 */
    int         i;

    if (a == NULL)
        return IV_EINVAL;

    /* 目标出口由 netmgr 传入的 new_ifindex **反推**，不依赖 active 的更新时机
     * （本回调在切换事务内执行，active 何时翻转属 netmgr 内部实现）。 */
    for (i = 0; i < IV_WAN_N; i++) {
        if (a->wan_ifindex[i] != 0 && a->wan_ifindex[i] == new_ifindex) {
            w = i;
            break;
        }
    }

    h = agent_pick_host(&a->cfg, w, &port);
    if (iv_report_set_server(&a->rp, h, port) != IV_OK)
        IV_LOG_W(AG_MOD, "server addr switch rejected, keep old (%s)",
                 a->cfg.wan_name[w]);
    else
        IV_LOG_I(AG_MOD, "server addr -> %s:%u (wan=%s)",
                 (h != NULL) ? h : "(none)", (unsigned)port, a->cfg.wan_name[w]);

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

uint64_t iv_agent_door_events(const iv_agent_t *a)
{
    return (a != NULL) ? a->door_events : 0u;
}
