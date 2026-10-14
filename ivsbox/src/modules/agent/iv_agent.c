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
static void env_feed(iv_agent_t *a, uint64_t now_ms);
static void on_cfg_search_mode(const iv_proto_frame_t *f, void *arg);
static void on_query_ipc(const iv_proto_frame_t *f, void *arg);
static void on_query_snmp(const iv_proto_frame_t *f, void *arg);
static int  agent_send_power(const void *data, uint16_t len, void *arg);
static void on_camdet_change(int ch, iv_camdet_status_t old_st,
                             iv_camdet_status_t new_st, void *arg);
static void on_door_open(int ds, void *arg);
static void on_dev_found(const iv_dev_desc_t *d, void *arg);
static int  snmp_task_fn(iv_task_ctx_t *ctx);
static void on_snmp_done(uint64_t id, int rc, const void *result,
                         size_t result_len, void *arg);

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
    int                        t;
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

    /* ---- 4b. M4-S4.2 设备发现（不依赖 Reactor：iv_device 全非阻塞、tick 内排空）----
     * `dev_enable=0` 时连 socket 都不建（配置里没开就零开销，也让既有用例不受影响）。 */
    iv_device_init(&a->dev, NULL);
    if (cfg->dev_enable) {
        iv_device_cfg_t dcfg;

        iv_device_cfg_default(&dcfg);
        dcfg.ifname      = (cfg->dev_ifname[0] != '\0') ? cfg->dev_ifname
                                                       : cfg->wan_name[IV_WAN_WIRED];
        dcfg.local_ip_be = cfg->dev_local_ip_be;
        memcpy(dcfg.local_mac, cfg->dev_local_mac, sizeof(dcfg.local_mac));
        dcfg.on_found    = on_dev_found;
        dcfg.cb_arg      = a;
        if (iv_device_open(&a->dev, &dcfg, now_ms) != IV_OK) {
            IV_LOG_W(AG_MOD, "device discovery open failed");
        } else {
            IV_LOG_I(AG_MOD, "device discovery up: ifname=%s sadp=%d",
                     (dcfg.ifname != NULL) ? dcfg.ifname : "(none)",
                     iv_device_sadp_available(&a->dev));
            /* 开机一次自动搜索（现网由平台 0xE4 驱动；本产品另加"开机自检一轮"）。 */
            (void)iv_device_start_search(&a->dev, now_ms);
        }
    }

    /* ---- 4c. M4-S4.3 SNMP 采集作业（**收发在慢任务池**，这里是纯参数装配）---- */
    for (t = 0; t < IV_AGENT_SNMP_TARGET_N; t++) {
        const iv_agent_snmp_cfg_t *sc = &cfg->snmp[t];
        iv_agent_snmp_job_t       *jb = &a->snmp_job[t];
        int                        k;

        memset(jb, 0, sizeof(*jb));
        iv_snmp_target_default(&jb->t);
        jb->t.iops = iv_snmp_default_iops();
        jb->owner  = a;
        jb->which  = t;
        if (sc->port != 0u)
            jb->t.port = sc->port;
        if (sc->timeout_ms != 0u)
            jb->t.timeout_ms = sc->timeout_ms;
        if (sc->retries != 0u)
            jb->t.retries = sc->retries;
        if (sc->max_rows != 0u)
            jb->t.max_rows = sc->max_rows;
        (void)snprintf(jb->t.community, sizeof(jb->t.community), "%s",
                       sc->community);
        if (sc->ip_be != 0u) {
            struct in_addr in;

            in.s_addr = sc->ip_be;
            (void)inet_ntop(AF_INET, &in, jb->t.host, sizeof(jb->t.host));
        }
        jb->n_slot = (sc->n_oid > 0) ? sc->n_oid : 0;
        if (jb->n_slot > (int)IV_AGENT_SNMP_SLOT_MAX)
            jb->n_slot = (int)IV_AGENT_SNMP_SLOT_MAX;
        for (k = 0; k < jb->n_slot; k++)
            (void)snprintf(jb->slot[k].oid, sizeof(jb->slot[k].oid), "%s",
                           sc->oid[k]);
        a->snmp_reqid[t] = 1u + (uint32_t)t * 0x1000u;
        a->snmp_state[t] = IV_AGENT_SNMP_IDLE;
    }
    if (cfg->pool != NULL)
        IV_LOG_I(AG_MOD, "snmp: cam(enable=%d n=%d host=%s) sw(enable=%d n=%d host=%s)",
                 cfg->snmp[IV_AGENT_SNMP_CAM].enable, a->snmp_job[IV_AGENT_SNMP_CAM].n_slot,
                 a->snmp_job[IV_AGENT_SNMP_CAM].t.host,
                 cfg->snmp[IV_AGENT_SNMP_SW].enable, a->snmp_job[IV_AGENT_SNMP_SW].n_slot,
                 a->snmp_job[IV_AGENT_SNMP_SW].t.host);

    /* ---- 4d. 平台下行命令路由（0xE4 搜索摄像机 / 0xE7 查 SNMP）----
     * 与 0xA5 同在**装配层**注册：这三条都只影响装配层持有的模块（dev/snmp/cd），
     * 与 0xE1~0xE3/E2 那批"读上报链"的默认路由（iv_report 内注册）互不重叠。 */
    (void)iv_proto_route(&a->rp.pf, IV_PROTO_CMD_QUERY_IPC_IP, on_query_ipc, a);
    (void)iv_proto_route(&a->rp.pf, IV_PROTO_CMD_QUERY_SNMP, on_query_snmp, a);

    /* ---- 5. 摄像机网络状态探测（S4.1）＋ 自愈（S4.4）＋ 0xA5 路由 ----
     * **这三项都不依赖 Reactor**：camdet 是相位机、recovery 是纯逻辑，两者都只由
     * `iv_agent_step()`（或 Reactor 的 tick）推进 ⇒ 必须放在 `r != NULL` **之外**。
     * 留在里面会让「调用方按节拍调 iv_agent_step」的模式**静默丢掉 S4.1/S4.4/0xA5**：
     * `a->rec` 从不 init ⇒ `opened == 0` ⇒ 主网/摄像机的故障沿永远不动作。
     * （2026-10-13 实测：test_agent_switch 走 r == NULL，recovery 的 credit 恒 0，
     *   主网 1→2 沿收不到任何继电器动作。） */
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

    /* ---- 6. 本层定时器（仅 Reactor 模式；r == NULL 时由调用方按节拍推进）---- */
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
    if (a->cfg.pool != NULL)
        (void)iv_taskpool_process(a->cfg.pool, 0); /* 收 SNMP 完成结果（同线程） */
    probe_drive(a, now_ms);
    netmgr_feed(a, now_ms);
    (void)iv_netmgr_tick(&a->nm, now_ms);
    if (iv_camdet_is_open(&a->cd))
        (void)iv_camdet_tick(&a->cd, now_ms);
    if (iv_device_is_open(&a->dev))
        (void)iv_device_tick(&a->dev, now_ms);
    env_feed(a, now_ms);
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

    iv_camdet_close(&a->cd);  /* 停在飞 socket + 关引擎（未开时幂等） */
    iv_device_close(&a->dev); /* 关 UDP / AF_PACKET 两个 socket（未开时幂等） */
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

/* 把本层掌握的"非采集板事实"注入上报层：CNS（六路摄像机网络状态）＋ MN（主网两路）。
 * 只改这两个字段，其余外部事实（CSQ/定位/故障码）原样保留。
 *
 * MN **同时带出 S4.4 的主网故障沿**：判据用 `iv_netmgr_wan_is_up()` 的**迟滞结论**
 * （连续 fail_n 次失败才判死），**不是**裸 `iv_probe_state()` —— 后者单次探测超时
 * 就会把主网电切掉。该出口是 S4.4 唯一缺的那一环（iv_report_env_t::mn[] 的生产者）。
 * 未绑定接口的那一路按 MN=0（未指定 IP）上报，且不参与故障沿。 */
static void env_feed(iv_agent_t *a, uint64_t now_ms)
{
    iv_report_env_t env;
    int             i;

    env = a->rp.env;

    if (iv_camdet_is_open(&a->cd)) {
        int cns[IV_CAMDET_CH_N];

        if (iv_camdet_cns(&a->cd, cns) == IV_OK) {
            for (i = 0; i < IV_REPORT_CNS_N && i < IV_CAMDET_CH_N; i++)
                env.cns[i] = cns[i];
        }
    }

    for (i = 0; i < (int)IV_RECOVERY_MASTER_N && i < (int)IV_WAN_N; i++) {
        int st;

        if (a->wan_ifindex[i] == 0) {
            st = 0; /* 未绑定接口 ⇒ 未指定 IP */
        } else {
            st = iv_netmgr_wan_is_up(&a->nm, (iv_wan_t)i) ? 1 : 2;
        }
        env.mn[i] = st;
        if (a->mn_last[i] == 1u && st == 2)
            iv_recovery_on_master_change(&a->rec, i, 1, 2, now_ms);
        a->mn_last[i] = (uint8_t)st;
    }

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

/* 平台 0xE4（搜索摄像机信息）：重开一轮多品牌发现，与现网"摄像机搜索"按钮等价。
 * 发现是**异步**的（非阻塞相位机，约 6 相位 × 3 轮），故这里只回 ACK；
 * 结果由 0xE4 的应答体另出（**其 JSON schema 尚未冻结**，见计划 §S4.2 未做条）。 */
static void on_query_ipc(const iv_proto_frame_t *f, void *arg)
{
    iv_agent_t *a = (iv_agent_t *)arg;
    int         rc;

    if (f == NULL || a == NULL)
        return;
    rc = iv_agent_device_search(a, a->now_ms);
    (void)iv_proto_ack(&a->rp.pf, f->cmd,
                       (rc == IV_OK) ? IV_PROTO_ACK_OK : IV_PROTO_ERR_EXEC);
    IV_LOG_I(AG_MOD, "platform 0xE4 camera search -> rc=%d", rc);
}

/* 平台 0xE7（查询 SNMP 信息）：把采集任务丢进慢任务池，回 ACK；
 * 取值结果留在本层结果表（`iv_agent_snmp_get()`），应答 JSON 同 0xE4 待 schema。 */
static void on_query_snmp(const iv_proto_frame_t *f, void *arg)
{
    iv_agent_t *a = (iv_agent_t *)arg;
    int         which;
    int         rc;

    if (f == NULL || a == NULL)
        return;
    /* DATA[0] 选目标：0/缺省 = 摄像机（现网 snmp_dev_type 0~5），7 = 交换机。
     * 与现网 com/app 里 snmp_dev_type 的取值口径一致（0~5 摄像机、7 交换机）。 */
    which = IV_AGENT_SNMP_CAM;
    if (f->data != NULL && f->len >= 1u && f->data[0] >= 7u)
        which = IV_AGENT_SNMP_SW;
    rc = iv_agent_snmp_start(a, which, a->now_ms);
    (void)iv_proto_ack(&a->rp.pf, f->cmd,
                       (rc == IV_OK) ? IV_PROTO_ACK_OK : IV_PROTO_ERR_EXEC);
    IV_LOG_I(AG_MOD, "platform 0xE7 snmp query target=%d -> rc=%d", which, rc);
}

/* S4.2 发现回调：每新增/更新一条档案调一次（worker/装配层同线程，无阻塞动作）。 */
static void on_dev_found(const iv_dev_desc_t *d, void *arg)
{
    iv_agent_t *a = arg;

    if (a == NULL || d == NULL)
        return;
    IV_LOG_I(AG_MOD, "device found: brand=%s model=%s by=%s (%d total)",
             iv_dev_brand_name(d->brand),
             (d->model[0] != '\0') ? d->model : "?",
             iv_dev_phase_name(d->found_by), iv_device_count(&a->dev));
}

/* ---------------------------------------------------------------------------
 * S4.3 SNMP：慢任务池任务函数（**worker 线程**）与完成回调（装配层线程）
 * ------------------------------------------------------------------------- */
/* 逐槽位取值：标量（OID 以 `.0` 结尾）走 GET；表列走 GETNEXT 连读，
 * 只留存前 IV_AGENT_SNMP_ROWS_MAX 行（完整遍历上限由 t.max_rows 管）。
 * **只写 job 结构、不碰 agent 业务状态**；结果由 on_snmp_done 落账。 */
static int snmp_task_fn(iv_task_ctx_t *ctx)
{
    iv_agent_snmp_job_t *jb = ctx->arg;
    int                  i;

    if (jb == NULL)
        return IV_EINVAL;

    for (i = 0; i < jb->n_slot; i++) {
        iv_agent_snmp_slot_t *s  = &jb->slot[i];
        const char           *oid = s->oid;
        size_t                ol;

        s->n_rows = 0;
        s->rc     = IV_ESTATE;
        if (oid[0] == '\0') {
            s->rc = IV_ENOENT; /* 该槽位未配 OID：不采、不填值 */
            continue;
        }
        if (iv_task_canceled(ctx)) {
            s->rc = IV_ECANCELED;
            continue;
        }
        /* 逐槽位错开 request-id，便于排障时区分在飞事务（收发本身是串行的）。 */
        jb->t.reqid_seed = 1u + (uint32_t)i * 4u;

        ol = strlen(oid);
        if (ol >= 2u && oid[ol - 2] == '.' && oid[ol - 1] == '0') {
            iv_snmp_value_t v;

            s->rc = iv_snmp_get(&jb->t, oid, &v);
            if (s->rc == IV_OK) {
                s->row[0].type = v.type;
                s->row[0].num  = v.num;
                (void)snprintf(s->row[0].str, sizeof(s->row[0].str), "%s", v.str);
                s->n_rows = 1;
            }
        } else {
            iv_snmp_value_t rows[IV_AGENT_SNMP_ROWS_MAX];
            int             nr = 0;
            int             k;

            s->rc = iv_snmp_walk(&jb->t, oid, rows, (int)IV_AGENT_SNMP_ROWS_MAX,
                                 &nr);
            if (s->rc == IV_OK) {
                for (k = 0; k < nr && k < (int)IV_AGENT_SNMP_ROWS_MAX; k++) {
                    s->row[k].type = rows[k].type;
                    s->row[k].num  = rows[k].num;
                    (void)snprintf(s->row[k].str, sizeof(s->row[k].str), "%s",
                                   rows[k].str);
                }
                s->n_rows = nr;
            }
        }
    }
    return IV_OK;
}

/* 采集完成（**装配层线程**）：把状态置为"结果就绪"并记账。id 只用于日志。 */
static void on_snmp_done(uint64_t id, int rc, const void *result,
                         size_t result_len, void *arg)
{
    iv_agent_snmp_job_t *jb = arg;
    iv_agent_t          *a  = (jb != NULL) ? (iv_agent_t *)jb->owner : NULL;
    int                  i;

    (void)id;
    (void)result;
    (void)result_len;
    if (jb == NULL || a == NULL)
        return;

    for (i = 0; i < jb->n_slot; i++) {
        if (jb->slot[i].n_rows == 0)
            a->snmp_slot_noval++;
    }
    a->snmp_state[jb->which] = IV_AGENT_SNMP_DONE;
    a->snmp_runs++;
    IV_LOG_I(AG_MOD, "snmp collect done: target=%d rc=%d slots=%d",
             jb->which, rc, jb->n_slot);
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
    if (a->cfg.pool != NULL)
        (void)iv_taskpool_process(a->cfg.pool, 0);
    probe_drive(a, now);
    if (iv_camdet_is_open(&a->cd))
        (void)iv_camdet_tick(&a->cd, now);
    if (iv_device_is_open(&a->dev))
        (void)iv_device_tick(&a->dev, now);
    env_feed(a, now);
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
    env_feed(a, now); /* 主网 MN 更新 ＋ 故障沿（1 s 粒度足够） */
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

/* M5-S5.1：把内嵌的 camdet 句柄暴露给 Web 只读（总览页要读 CNS 六路与检测方式）。
 * 返回的是**内部句柄**，调用方只读；未装配（r == NULL 且未 open）时它仍是
 * 结构化的零值句柄，读 CNS 得到全 0（＝"未配置该路 IP"），语义正确。 */
const iv_camdet_t *iv_agent_camdet(const iv_agent_t *a)
{
    return (a != NULL) ? &a->cd : NULL;
}

uint64_t iv_agent_door_events(const iv_agent_t *a)
{
    return (a != NULL) ? a->door_events : 0u;
}

uint8_t iv_agent_mn(const iv_agent_t *a, int w)
{
    if (a == NULL || w < 0 || w >= (int)IV_RECOVERY_MASTER_N ||
        w >= (int)IV_WAN_N)
        return 0u;
    return a->mn_last[w];
}

/* ---------------------------------------------------------------------------
 * S4.2 设备发现出口
 * ------------------------------------------------------------------------- */
int iv_agent_device_search(iv_agent_t *a, uint64_t now_ms)
{
    if (a == NULL)
        return IV_EINVAL;
    if (!iv_device_is_open(&a->dev))
        return IV_ESTATE; /* 未装配（dev_enable=0）或 open 失败 */
    iv_device_clear(&a->dev);
    return iv_device_start_search(&a->dev, now_ms);
}

int iv_agent_device_count(const iv_agent_t *a)
{
    if (a == NULL)
        return IV_EINVAL;
    return iv_device_count(&a->dev);
}

int iv_agent_device_get(const iv_agent_t *a, int i, iv_dev_desc_t *out)
{
    if (a == NULL || out == NULL)
        return IV_EINVAL;
    return iv_device_get(&a->dev, i, out);
}

iv_device_t *iv_agent_device(iv_agent_t *a)
{
    return (a != NULL) ? &a->dev : NULL;
}

/* ---------------------------------------------------------------------------
 * S4.3 SNMP 采集出口
 * ------------------------------------------------------------------------- */
int iv_agent_snmp_start(iv_agent_t *a, int which, uint64_t now_ms)
{
    iv_agent_snmp_job_t  *jb;
    iv_agent_snmp_cfg_t  *sc;
    iv_task_req_t         req;
    uint64_t              id = 0u;
    int                   rc;

    if (a == NULL || which < 0 || which >= IV_AGENT_SNMP_TARGET_N)
        return IV_EINVAL;
    if (a->cfg.pool == NULL)
        return IV_ESTATE; /* 无慢任务池：阻塞式收发绝不上 Reactor 线程 */
    sc = &a->cfg.snmp[which];
    if (!sc->enable || sc->ip_be == 0u || a->snmp_job[which].n_slot <= 0)
        return IV_ESTATE;
    if (a->snmp_state[which] == IV_AGENT_SNMP_RUN)
        return IV_EBUSY;

    jb = &a->snmp_job[which];
    memset(&req, 0, sizeof(req));
    req.name        = (which == IV_AGENT_SNMP_CAM) ? "snmp-cam" : "snmp-sw";
    req.fn          = snmp_task_fn;
    req.arg         = jb;
    /* 端到端截止时间：每槽位最坏 = (retries+1) × timeout；表列还会多行。
     * 给足余量（不设限反而会让卡死的槽位拖住 worker）。 */
    req.timeout_ms  = (uint32_t)(jb->n_slot + 1) *
                      ((jb->t.retries + 1u) *
                       (jb->t.timeout_ms ? jb->t.timeout_ms
                                         : IV_SNMP_TIMEOUT_MS_DEFAULT)) *
                      (uint32_t)IV_AGENT_SNMP_ROWS_MAX + 1000u;
    req.on_done     = on_snmp_done;
    req.on_done_arg = jb;

    rc = iv_taskpool_submit(a->cfg.pool, &req, &id);
    if (rc != IV_OK) {
        IV_LOG_W(AG_MOD, "snmp submit failed: %d", rc);
        return rc;
    }
    a->snmp_state[which] = IV_AGENT_SNMP_RUN;
    a->now_ms            = now_ms;
    IV_LOG_I(AG_MOD, "snmp collect started: target=%d slots=%d id=%llu",
             which, jb->n_slot, (unsigned long long)id);
    return IV_OK;
}

int iv_agent_snmp_state(const iv_agent_t *a, int which)
{
    if (a == NULL || which < 0 || which >= IV_AGENT_SNMP_TARGET_N)
        return IV_EINVAL;
    return (int)a->snmp_state[which];
}

int iv_agent_snmp_get(const iv_agent_t *a, int which, int i,
                      const char **slot_name, const char **oid, int *n_rows,
                      iv_agent_snmp_row_t *rows, int rowcap)
{
    const iv_agent_snmp_job_t  *jb;
    const char *const          *names;
    const iv_agent_snmp_slot_t *s;
    int                         k;
    int                         n;

    if (a == NULL || which < 0 || which >= IV_AGENT_SNMP_TARGET_N)
        return IV_EINVAL;
    jb = &a->snmp_job[which];
    if (i < 0 || i >= jb->n_slot)
        return IV_EINVAL;

    names = (which == IV_AGENT_SNMP_CAM) ? iv_snmp_cam_slots : iv_snmp_sw_slots;
    s     = &jb->slot[i];
    if (slot_name != NULL)
        *slot_name = names[i];
    if (oid != NULL)
        *oid = s->oid;
    if (n_rows != NULL)
        *n_rows = s->n_rows;
    if (rows != NULL && rowcap > 0) {
        n = (s->n_rows < rowcap) ? s->n_rows : rowcap;
        for (k = 0; k < n; k++)
            rows[k] = s->row[k];
    }
    return IV_OK;
}
