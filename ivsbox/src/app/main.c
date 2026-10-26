/*
 * ivsboxd 主控进程入口（M1-S10 骨架装配 + M3 业务装配）
 *
 * 启动顺序（已钉死）：
 *   日志 → 配置 → SQLite → 慢任务池 → Reactor → 本地通道服务 → 看门狗
 *   → M3 业务装配（netlink/probe/netmgr/report）→ 健康线程
 * 然后 iv_reactor_run() 进入主循环，直到收到 SIGTERM/SIGINT 后干净退出。
 *
 * 本文件只负责装配与生命周期，不含业务逻辑。M3 的四条链（S3.1 netlink /
 * S3.2 probe / S3.3 netmgr / S3.6 report）的接线封装在 `iv_agent`
 * （libivmodules）里；本文件只从配置读参数、拉起它、并在退出时按逆序关停。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netpacket/packet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/ivsbox.h"
#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_config.h"
#include "ivsbox/iv_ctl.h"
#include "ivsbox/iv_db.h"
#include "ivsbox/iv_health.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_netcfg.h"
#include "ivsbox/iv_provision.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_taskpool.h"
#include "ivsbox/iv_tunnel.h"
#include "ivsbox/iv_upd.h"
#include "ivsbox/iv_version.h"
#include "ivsbox/iv_watchdog.h"

#define APP_MOD "app"

/* 摄像机 6 路 IP 配置键（S4.1；空串＝未配置） */
static const char *const CAM_IP_KEY[IV_CAMDET_CH_N] = {
    "camera.ch.0.ip", "camera.ch.1.ip", "camera.ch.2.ip",
    "camera.ch.3.ip", "camera.ch.4.ip", "camera.ch.5.ip"
};

/* 运行期全局句柄 */
static iv_taskpool_t *g_pool;
static iv_reactor_t  *g_reactor;
static iv_health_t   *g_health;
static iv_config_t   *g_cfg;
static iv_db_t       *g_db;
static iv_store_t    *g_store;
static int            g_wd_fd = -1;

/* M3 业务装配（约 35 KB，必须放静态区/堆，勿放栈） */
static iv_agent_t     g_agent;

/* E3 应答的 `tsn`（芯片唯一号）：装配期读一次，指针交给 agent 长持。
 * 放静态区而不是栈上临时量 —— `iv_report_cfg_t` 里存的是指针，栈变量出作用域就悬空。 */
static char           g_tsn[IV_PROV_TSN_MAX];

/* 本地通道（M5-S5.1 起改为**多客户端** ＋ 方法路由）：
 * 原来只支持一个客户端、只认 system.snapshot。Web 进程会长期占一条连接，
 * 调试工具/平台探活可能同时连进来，单客户端槽会让后来者被直接踢掉。 */
#define CHAN_MAX_CLIENTS 4
typedef struct {
    int         fd;
    iv_event_t *ev;
} chan_cli_t;

static int         g_listen_fd = -1;
static iv_event_t *g_listen_ev;
static chan_cli_t  g_cli[CHAN_MAX_CLIENTS];

/* iv_ctl 的上下文与方法路由结果缓冲（均在装配层单线程内使用）。
 * 请求缓冲给通道最大包，响应缓冲给 iv_ctl 自己的上限。 */
static iv_ctl_ctx_t g_ctl_ctx;
static char         g_rx_buf[IV_CHAN_RECV_CAP_MIN];
static char         g_rsp_buf[IV_CTL_RSP_MAX];

/* 通道函数在下面定义，这里先声明：关停路径要先于定义使用它。 */
static void chan_client_drop(int slot);

/* ---------------------------------------------------------------------------
 * M8-S8.3 受控远程通道 —— 静态句柄（函数体在 cfg 辅助函数之后；app_shutdown 与
 * on_config_event 都先于那些定义用到它们，故声明必须提在这里）。
 * ------------------------------------------------------------------------- */
static iv_tunnel_t  g_tunnel_store;  /* 句柄（约 2 KB，静态区；不占栈） */
static iv_tunnel_t *g_tunnel = &g_tunnel_store;
static iv_timer_t  *g_tunnel_timer;
/* resolver 的目标端口与开关（装配时从配置读入；resolver 是回调，不便再读配置） */
static uint16_t     g_tun_port_web   = 80u;
static uint16_t     g_tun_port_media = 18000u;
static uint16_t     g_tun_port_cam   = 8081u;
static int          g_tun_cam_enable;

static void tunnel_apply_cfg(void); /* 配置热更新回调要用 */

/* ---------------------------------------------------------------------------
 * 干净退出
 * ------------------------------------------------------------------------- */
static void app_shutdown(void)
{
    IV_LOG_I(APP_MOD, "shutting down ivsboxd");

    {
        int i;

        for (i = 0; i < CHAN_MAX_CLIENTS; i++)
            chan_client_drop(i);
    }
    if (g_listen_fd >= 0) {
        (void)iv_chan_listen_close(g_listen_fd, IV_CHAN_PATH_DEFAULT);
        g_listen_fd = -1;
    }

    /* M8-S8.3：主控**干净退出**即关通道。有效期闸门跑在本进程里，进程不在了就不该
     * 再留一条没人管的远程入口。代价是"运维远程重启主控会断线"—— 但重开只需平台
     * 再发一条 tunnel.enable，比"重启后通道永久开着"安全。主控被 SIGKILL 时这条
     * 走不到，兜底交给标记文件里的到期时刻（scripts/frp.sh 独立判过期）。 */
    if (iv_tunnel_state(g_tunnel) == IV_TUNNEL_ON) {
        (void)iv_tunnel_disable(g_tunnel, (uint64_t)time(NULL), IV_TUNNEL_STOP_LOCAL);
        IV_LOG_I(APP_MOD, "tunnel: closed on shutdown");
    }
    if (g_tunnel_timer != NULL && g_reactor != NULL) {
        (void)iv_timer_cancel(g_reactor, g_tunnel_timer);
        g_tunnel_timer = NULL;
    }

    /* 健康线程必须在 taskpool/reactor 销毁前停掉 */
    if (g_health != NULL) {
        iv_health_destroy(g_health);
        g_health = NULL;
    }

    /* M3 装配层：必须在 reactor 销毁前摘除其定时器/事件并关闭串口与平台连接 */
    iv_agent_close(&g_agent);

    if (g_pool != NULL) {
        iv_taskpool_destroy(g_pool);
        g_pool = NULL;
    }
    if (g_reactor != NULL) {
        iv_reactor_destroy(g_reactor);
        g_reactor = NULL;
    }

    if (g_cfg != NULL) {
        iv_config_close(g_cfg);
        g_cfg = NULL;
    }
    if (g_store != NULL) {
        iv_store_close(g_store); /* 只清句柄；db 的关闭在下面 */
        g_store = NULL;
    }
    if (g_db != NULL) {
        iv_db_close(g_db);
        g_db = NULL;
    }

    /* 正常停机必须显式停狗，否则整机会在约 16s 后被复位 */
    if (g_wd_fd >= 0) {
        (void)iv_watchdog_disable(g_wd_fd);
        g_wd_fd = -1;
    }

    iv_log_set_sink(NULL, NULL);
}

/* ---------------------------------------------------------------------------
 * 看门狗初始化：请求 16s；若驱动拒绝越界值，读回实际值并校验不变量
 * ------------------------------------------------------------------------- */
static int watchdog_setup(void)
{
    int actual_sec;
    int need;

    g_wd_fd = iv_watchdog_open(NULL);
    if (g_wd_fd < 0) {
        if (errno == ENOENT) {
            IV_LOG_W(APP_MOD, "watchdog device not found, running without hardware watchdog");
            return 0;
        }
        IV_LOG_E(APP_MOD, "watchdog open failed: %s", strerror(errno));
        return -1;
    }

    if (iv_watchdog_set_timeout(g_wd_fd, IV_WATCHDOG_DEFAULT_TIMEOUT_SEC) >= 0) {
        actual_sec = IV_WATCHDOG_DEFAULT_TIMEOUT_SEC;
    } else {
        actual_sec = iv_watchdog_get_timeout(g_wd_fd);
        if (actual_sec <= 0) {
            IV_LOG_E(APP_MOD, "watchdog set timeout failed and cannot read actual timeout");
            return -1;
        }
        IV_LOG_W(APP_MOD, "watchdog set timeout rejected, using actual timeout %ds", actual_sec);
    }

    /* 健康判据最坏结论时延必须严格小于看门狗超时窗口 */
    need = (int)((IV_HEALTH_STUCK_MS_DEFAULT + IV_HEALTH_INTERVAL_MS_DEFAULT + 999u) / 1000u);
    if (actual_sec <= need) {
        IV_LOG_E(APP_MOD, "watchdog timeout %ds too small (need > %ds)", actual_sec, need);
        return -1;
    }

    IV_LOG_I(APP_MOD, "watchdog armed, timeout=%ds", actual_sec);
    return 0;
}

/* ---------------------------------------------------------------------------
 * 本地通道：方法路由（M5-S5.1）
 *
 * 载荷解析、方法分发、响应拼装全部交给 `iv_ctl`（libivmodules，可单测）；
 * 本文件只负责 fd 生命周期与收发。**未知方法静默不回**（`IV_ENOTSUP`）——
 * S10 既有口径，且被 tests/integration/test_ctl_snapshot.c 钉死。
 * ------------------------------------------------------------------------- */

/* 找一个空闲客户端槽；满返回 -1。 */
static int chan_slot_free(void)
{
    int i;

    for (i = 0; i < CHAN_MAX_CLIENTS; i++) {
        if (g_cli[i].fd < 0)
            return i;
    }
    return -1;
}

static void chan_client_drop(int slot)
{
    if (slot < 0 || slot >= CHAN_MAX_CLIENTS)
        return;
    if (g_cli[slot].ev != NULL && g_reactor != NULL) {
        (void)iv_reactor_del(g_reactor, g_cli[slot].ev);
        g_cli[slot].ev = NULL;
    }
    if (g_cli[slot].fd >= 0) {
        (void)close(g_cli[slot].fd);
        g_cli[slot].fd = -1;
    }
}

static void on_chan_client(int fd, uint32_t events, void *arg)
{
    int            slot = (int)(intptr_t)arg;
    ivs_chan_hdr_t hdr;
    size_t         payload_len = 0;
    size_t         out_len = 0;
    int            rc;
    int            crc;

    if (events & (IV_EV_ERR | IV_EV_HUP | IV_EV_RDHUP)) {
        chan_client_drop(slot);
        return;
    }

    rc = iv_chan_recv(fd, g_rx_buf, sizeof(g_rx_buf), &hdr, &payload_len);
    if (rc == IV_EAGAIN)
        return;
    if (rc != IV_OK) {
        IV_LOG_I(APP_MOD, "channel client (slot=%d) closed/error, rc=%d", slot, rc);
        chan_client_drop(slot);
        return;
    }
    if (hdr.type != IV_CHAN_TYPE_REQ)
        return; /* 这个方向只会来 REQ */

    crc = iv_ctl_handle(&g_ctl_ctx,
                        (const char *)g_rx_buf + IV_CHAN_HDR_SIZE, payload_len,
                        g_rsp_buf, sizeof(g_rsp_buf), &out_len);
    if (crc != IV_OK) {
        /* IV_ENOTSUP = 未知方法 ⇒ **静默丢弃**（S10 口径）。
         * 其余错误（缓冲太小、内存不足）属装配问题，同样不回，但留一条日志。 */
        if (crc != IV_ENOTSUP)
            IV_LOG_W(APP_MOD, "channel request handling failed: rc=%d", crc);
        return;
    }

    {
        ivs_chan_hdr_t rh;
        int            snd;

        if (iv_chan_hdr_init(&rh, IV_CHAN_TYPE_RSP, hdr.request_id, 0u,
                             (uint32_t)out_len) != IV_OK)
            return;
        snd = iv_chan_send(fd, &rh, g_rsp_buf, out_len);
        if (snd != IV_OK)
            IV_LOG_I(APP_MOD, "channel send failed (slot=%d, rc=%d)", slot, snd);
    }
}

static void on_chan_listen(int fd, uint32_t events, void *arg)
{
    iv_chan_peer_t peer;
    int            cfd;
    int            slot;

    (void)events;
    (void)arg;

    cfd = iv_chan_accept(fd, NULL, &peer);
    if (cfd == IV_EAGAIN)
        return;
    if (cfd < 0) {
        IV_LOG_E(APP_MOD, "channel accept failed: %d", cfd);
        return;
    }

    slot = chan_slot_free();
    if (slot < 0) {
        IV_LOG_W(APP_MOD, "channel clients full (%d), dropping new one",
                 CHAN_MAX_CLIENTS);
        (void)close(cfd);
        return;
    }

    g_cli[slot].fd = cfd;
    g_cli[slot].ev = iv_reactor_add(g_reactor, cfd, IV_EV_READ, on_chan_client,
                                    (void *)(intptr_t)slot);
    if (g_cli[slot].ev == NULL) {
        IV_LOG_E(APP_MOD, "channel client add to reactor failed");
        (void)close(cfd);
        g_cli[slot].fd = -1;
        return;
    }
    IV_LOG_I(APP_MOD, "channel client connected (slot=%d, uid=%d)", slot, (int)peer.uid);
}

/* ---------------------------------------------------------------------------
 * Reactor 回调：慢任务池完成事件
 * ------------------------------------------------------------------------- */
static void on_taskpool_event(int fd, uint32_t events, void *arg)
{
    (void)fd;
    (void)events;
    (void)arg;
    (void)iv_taskpool_process(g_pool, 0);
}

/* ---------------------------------------------------------------------------
 * Reactor 回调：配置热更新
 * ------------------------------------------------------------------------- */
static void on_config_event(int fd, uint32_t events, void *arg)
{
    (void)fd;
    (void)events;
    (void)arg;
    (void)iv_config_watch_poll(g_cfg, NULL);
    /* M8-S8.3：配置热更新后重取通道的服务端落点与代理端口（服务端一变，
     * 正在开的会话被作废，见 tunnel_apply_cfg）。 */
    tunnel_apply_cfg();
}

/* ---------------------------------------------------------------------------
 * 通道接听点父目录（init 脚本负责创建；这里仅在缺失时兜底）
 * ------------------------------------------------------------------------- */
static int mkdir_for_channel(void)
{
    const char *p = IV_CHAN_PATH_DEFAULT;
    const char *slash = strrchr(p, '/');
    char        dir[128];
    size_t      len;

    if (slash == NULL || slash == p)
        return 0;

    len = (size_t)(slash - p);
    if (len >= sizeof(dir))
        return -1;
    memcpy(dir, p, len);
    dir[len] = '\0';

    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 本地通道系统操作钩子（M5-S5.2）
 *
 * 只有这两个动作**必须由 ivsboxd 本体**执行（Web 进程做不了）：
 *   - 重启：`reboot(2)` 要 root，且必须先把文件系统落盘（sync）再走，否则
 *     UDISK 上的配置/队列可能停在 page cache 里没写下去。
 *   - 对时：`clock_settime(CLOCK_REALTIME)` 影响全机，只有主控该做。
 * 两个钩子都在**装配层单线程**内同步执行，故都得够快（架构 §4.2）。
 * ------------------------------------------------------------------------- */
static int ctl_do_reboot(void *arg)
{
    (void)arg;
    (void)sync();
    /* 正常停机前先停狗：否则 reboot 期间看门狗可能先咬、把复位做得更难看。
     * 这里不 disable 也能重启成功，但保留顺序与 app_shutdown 一致。 */
    if (g_wd_fd >= 0) {
        (void)iv_watchdog_disable(g_wd_fd);
        g_wd_fd = -1;
    }
    if (reboot(RB_AUTOBOOT) != 0)
        return IV_EIO;
    return IV_OK;
}

static int ctl_do_settime(int64_t wall_sec, void *arg)
{
    struct timespec ts;

    (void)arg;
    ts.tv_sec  = (time_t)wall_sec;
    ts.tv_nsec = 0;
    if (clock_settime(CLOCK_REALTIME, &ts) != 0)
        return IV_EIO;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * M6 媒体方法的 ctl 绑定（S6.1）：四个 ops 全部转发到 agent 内嵌的 iv_media
 * 桥接实例。arg＝&g_agent（iv_agent_t），经 iv_agent_media() 取桥接句柄。
 * 语义与失败码由 iv_media 定义（capture 异步入队、record 同步回环 HTTP）。
 * ------------------------------------------------------------------------- */
static int media_op_capture(const char *path, char *out_path, size_t cap,
                            void *arg)
{
    return iv_media_capture_async(iv_agent_media((iv_agent_t *)arg), path,
                                  out_path, cap);
}

static int media_op_record_start(int id, const char *url, void *arg)
{
    return iv_media_record_start(iv_agent_media((iv_agent_t *)arg), id, url);
}

static int media_op_record_stop(int id, void *arg)
{
    return iv_media_record_stop(iv_agent_media((iv_agent_t *)arg), id);
}

static int media_op_record_status(int id, int *recording, void *arg)
{
    return iv_media_record_status(iv_agent_media((iv_agent_t *)arg), id,
                                  recording);
}

static const iv_ctl_media_ops_t s_media_ops = {
    media_op_capture,
    media_op_record_start,
    media_op_record_stop,
    media_op_record_status,
    NULL /* arg 在 app_init 里回填为 &g_agent（agent 此时才存在） */
};

/* ---------------------------------------------------------------------------
 * M3 业务装配：从配置读取参数，拉起网络侧与上报链
 * ------------------------------------------------------------------------- */
/* 配置里的点分十进制 IPv4 → 网络序；空串/非法返回 0（＝未配） */
static uint32_t cfg_ip_be(const char *key)
{
    const char      *s = NULL;
    struct in_addr   in;

    if (iv_config_get_str(g_cfg, key, &s) != IV_OK || s == NULL || s[0] == '\0')
        return 0u;
    if (inet_pton(AF_INET, s, &in) != 1)
        return 0u;
    return in.s_addr; /* 已是网络序 */
}

/* 配置里的字符串：空/未配 → NULL（指针指向配置模块内部缓冲，配置存活期内有效） */
static const char *cfg_str(const char *key)
{
    const char *s = NULL;

    if (iv_config_get_str(g_cfg, key, &s) != IV_OK || s == NULL || s[0] == '\0')
        return NULL;
    return s;
}

/* 硬件型号（E3 应答 `mod`）的**值拷贝**。
 *
 * 2026-10-21 板端实测踩到：原先直接存 `cfg_str("sys.device.model")` 返回的指针，
 * 而那是配置模块 item 数组内部的定长缓冲。`config.apply` 会触发 inotify reload，
 * reload 时 item 数组被重建/清零 ⇒ 那根指针指向的条目没了 ⇒ E3 的 `mod` 变空串。
 * `net.server.*` 有 `on_cfg_committed()` 在配置事件里重新取值所以能自愈，
 * `mod` 没有订阅者，就一直空着。
 *
 * 改法：固定指向本文件的 static 缓冲，配置事件里**原地改写内容**（指针不变）
 * ⇒ 既保住"网页改完下一帧就生效"，又永不悬空。 */
static char g_mod[IV_REPORT_MOD_MAX + 1u];

static void model_refresh(void)
{
    const char *s = cfg_str("sys.device.model");

    (void)snprintf(g_mod, sizeof(g_mod), "%s", (s != NULL) ? s : "");
}

/* 配置里的端口：未配/越界 → 0（0 ＝ 该条地址无效） */
static uint16_t cfg_port(const char *key)
{
    int64_t v = 0;

    if (iv_config_get_int(g_cfg, key, &v) != IV_OK || v <= 0 || v > 65535)
        return 0u;
    return (uint16_t)v;
}

/* ---------------------------------------------------------------------------
 * 平台地址热重载（2026-10-19）
 *
 * ============================ 解决什么 ============================
 * Web 上改了「有线/无线平台服务器」地址后，配置确实落盘了、界面也显示新值，
 * **但报文还发往旧地址**，一直到重启主控为止。原因不在配置层：`iv_report`
 * 在 open 时就把地址拷成自己的副本，之后只有 netmgr 换出口才重推一次。
 * 现场表现就是"改了端口，平台那边一直等不到这台设备"。
 *
 * ============================ 为什么缓存一份 ============================
 * `net` 分类下还有 `net.ip` / `net.mask` / `net.gateway` 这些与平台**无关**的键。
 * 只要分类里有一个值变了通知就会来，不比对的话"改个静态 IP"也会把平台连接
 * 拆一次。所以这里存一份上次推送的值，只有地址或端口真的变了才重连。
 *
 * 2026-10-22 起缓存的是 **`iv_agent_pick_host()` 的解析结果**（每个出口一份），
 * 不再是四个输入值本身。理由：加了备份地址之后，"输入变了"不等于"要用的地址
 * 变了"—— 主地址在用、只改备份地址时解析结果一模一样，却会把在跑的 TCP 会话
 * 白拆一次。两个出口各解析一次即可覆盖"当前活动口是哪一个"两种可能。
 * ------------------------------------------------------------------------- */
static char     s_pick_host[IV_WAN_N][64];
static uint16_t s_pick_port[IV_WAN_N];

/* 返回 1 = **会被用到的**地址/端口有变化。`force`=0 时只对齐缓存、不重连
 * （启动时用：那时 agent 刚按同一份配置 open 过，再重连一次纯属多余）。 */
static int agent_sync_server_cfg(int force)
{
    const char *raw;
    const char *bak;
    uint16_t    port;
    uint16_t    bport;
    int         i;
    int         changed = 0;

    /* 1) 先把四个值写进 agent 的配置快照（它是 `iv_agent_pick_host()` 的输入） */
    for (i = 0; i < IV_WAN_N; i++) {
        raw   = (i == IV_WAN_WIRELESS) ? cfg_str("net.server.wireless.host")
                                       : cfg_str("net.server.wired.host");
        port  = (i == IV_WAN_WIRELESS) ? cfg_port("net.server.wireless.port")
                                       : cfg_port("net.server.wired.port");
        bak   = (i == IV_WAN_WIRELESS) ? cfg_str("net.server.wireless.host_bak")
                                       : cfg_str("net.server.wired.host_bak");
        bport = (i == IV_WAN_WIRELESS) ? cfg_port("net.server.wireless.port_bak")
                                       : cfg_port("net.server.wired.port_bak");
        /* host 直接沿用配置内部指针：配置的字符串存在定长缓冲里（`union{char s[]}`），
         * 原地改写、指针一直有效。**端口是值类型，必须重写**（agent 存的是拷贝）。 */
        g_agent.cfg.srv_host[i]     = raw;
        g_agent.cfg.srv_port[i]     = port;
        g_agent.cfg.srv_host_bak[i] = bak;
        g_agent.cfg.srv_port_bak[i] = bport;
    }

    /* 2) 再比"解析出来到底连谁"。备份地址只在主地址没配时才顶上，所以
     *    "只改了当前没在用的备份"⇒ 解析结果不变 ⇒ 不重连。 */
    for (i = 0; i < IV_WAN_N; i++) {
        uint16_t    p  = 0u;
        const char *h  = iv_agent_pick_host(&g_agent.cfg, i, &p);
        const char *hs = (h != NULL) ? h : "";

        if (strcmp(s_pick_host[i], hs) != 0 || s_pick_port[i] != p)
            changed = 1;
        (void)snprintf(s_pick_host[i], sizeof(s_pick_host[i]), "%s", hs);
        s_pick_port[i] = p;
    }
    if (changed && force != 0) {
        int rc = iv_agent_reload_server(&g_agent);

        IV_LOG_I(APP_MOD, "net.server.* changed -> reload rc=%d", rc);
    }
    return changed;
}

/* 配置 → 模块参数的只读装配（定义在下面，`on_cfg_committed()` 要用）：
 * 开机装配与热更共用同一段，避免"开机一套、改配置另一套"。 */
static void cfg_snmp_targets(iv_agent_snmp_cfg_t *sc);
static int  cfg_rec_url_table(char (*buf)[IV_CFG_STR_MAX], int cap);
/* 本机地址热生效（定义在 `apply_saved_local_net()` 旁边，那里才有完整的取值/校验
 * 逻辑）。`on_cfg_committed()` 只负责"触发"，不重复一遍判定。 */
static void schedule_local_net_apply(void);

/* 配置事务通知（F7）。这里只接"改完必须马上生效"的：
 *   - `net` 分类：平台地址/端口（与出口网卡成对刷新）→ 断开旧连接、连新平台；
 *   - `sys` 分类里的 `sys.report.interval_s`：周期上报间隔 → 换节拍，**不断链**；
 *     `sys.sample.period_ms`：采集（0xE1 轮询）周期 → 只换轮询节拍，同样不断链；
 *     `sys.transport.mode`：传输模式 → 重跑一次状态机判定（活动口真变才重连）；
 *   - `probe` 分类：`probe.*` 五个键 → 探活参数热生效（目标变才重建探测表）；
 *   - `netthr` 分类：切换门限（`loss_count`）与自愈限次/冷却 → 各改各的模块，
 *     同值早返回（`iv_recovery_set_limits()` 会重置 credit，不能白调）。
 * 其余分类的热更新各有归属（隧道见 on_config_event）。 */
static void on_cfg_committed(iv_config_t *c, const char *name, uint32_t mask, void *user)
{
    (void)c;
    (void)name;
    (void)user;
    if ((mask & IV_CFG_CLASS_MASK(IV_CFG_CLS_NET)) != 0u) {
        (void)agent_sync_server_cfg(1);
        /* 本机地址（`net.ip`/`net.mask`/`net.gateway`）热生效 —— 这三个键此前是
         * "改配置不生效"清单里的**最后一条**：只有平台 `0xF4` 与开机
         * `apply_saved_local_net()` 两个消费者，网页上改完必须重启才应用。
         *
         * 这里只**触发**，真正判断（留空／非法／与内核现值相同）全在
         * `apply_saved_local_net()` 里 —— 与开机走同一段代码，不另写一份判定。
         * 触发是**延迟**的（见函数内注释）：本次回调发生在 `config.apply` 的
         * 处理过程中，HTTP 响应还没回到浏览器。 */
        schedule_local_net_apply();
    }
    if ((mask & IV_CFG_CLASS_MASK(IV_CFG_CLS_SYS)) != 0u) {
        /* `sys.report.interval_s` 原先只在 app_init 里读一次，网页上改了等于没改
         * （必须重启进程）—— 现场"设了 5 分钟、平台上还是十分钟一条"正是这个。 */
        int64_t v = 0;

        /* 硬件型号（E3 的 `mod`）同理：它存的是本文件的 static 缓冲，这里必须
         * 重新读一次，否则网页改完要重启才生效。 */
        model_refresh();
        if (iv_config_get_int(g_cfg, "sys.report.interval_s", &v) == IV_OK && v > 0)
            (void)iv_report_set_report_period(&g_agent.rp, (uint32_t)v * 1000u);
        /* 采集周期同理（`sys.sample.period_ms`）：只换 0xE1 轮询节拍，不动上报节拍、
         * 不断链、不清队列。 */
        if (iv_config_get_int(g_cfg, "sys.sample.period_ms", &v) == IV_OK && v > 0)
            (void)iv_report_set_poll_period(&g_agent.rp, (uint32_t)v);
        /* 传输模式：原先只在 agent_setup() 读一次、交给 iv_agent_open() 定初始
         * 活动口，之后**必须重启主控**才生效（D 类"改配置不生效"清单里的一条）。
         * 这里补上订阅；iv_agent_set_transport_mode() 内部对同值早返回，
         * 所以改上报间隔/采集周期不会顺带把平台链路拆一次。 */
        if (iv_config_get_int(g_cfg, "sys.transport.mode", &v) == IV_OK) {
            int mrc = iv_agent_set_transport_mode(&g_agent, (iv_netmgr_mode_t)v,
                                                  iv_clock_monotonic_ms());

            if (mrc != IV_OK)
                IV_LOG_W(APP_MOD, "transport mode %d not applied (%d)", (int)v, mrc);
        }
    }
    if ((mask & IV_CFG_CLASS_MASK(IV_CFG_CLS_PROBE)) != 0u) {
        /* `probe.*` 五个键原先只在 `agent_setup()` 里读一次 ⇒ 网页上改探测目标/
         * 节拍、平台下发 `0xF2`（ping 间隔）/`0xF8`（主网检测 IP）都要重启主控。
         * 归一化（0＝取默认）在 `iv_agent_reload_probe()` 内部统一做，
         * 与 `iv_agent_open()` 共用同一份 `probe_cfg_of()`，不在这里二次翻译。 */
        int64_t  pv   = 0;
        uint32_t i_ms = 0;
        uint32_t t_ms = 0;
        uint32_t f_n  = 0;
        int      prc;

        if (iv_config_get_int(g_cfg, "probe.interval_ms", &pv) == IV_OK)
            i_ms = (uint32_t)pv;
        if (iv_config_get_int(g_cfg, "probe.timeout_ms", &pv) == IV_OK)
            t_ms = (uint32_t)pv;
        if (iv_config_get_int(g_cfg, "probe.max_fail", &pv) == IV_OK)
            f_n = (uint32_t)pv;
        prc = iv_agent_reload_probe(&g_agent, cfg_ip_be("probe.target1"),
                                    cfg_ip_be("probe.target2"), i_ms, t_ms, f_n,
                                    iv_clock_monotonic_ms());
        if (prc != IV_OK)
            IV_LOG_W(APP_MOD, "probe.* not applied (%d)", prc);
    }
    if ((mask & (IV_CFG_CLASS_MASK(IV_CFG_CLS_ELEC) |
                 IV_CFG_CLASS_MASK(IV_CFG_CLS_SENSOR))) != 0u) {
        /* 阈值九个键（`elec.*` 4 项 ＋ `sensor.*` 5 项）此前**全工程没有消费者**：
         * 网页能改、能存，采集板一无所知。这里补上"改完就下发 0xF1"。
         * 值 0 ＝ 该项不设置（不下发该键），口径见 `iv_report_build_thresh()`；
         * 真发帧由 `thresh_drive()` 每拍重试（撞上在飞的 0xE1 是常态）。 */
        iv_thresh_t th;
        double      dv = 0.0;
        int         trc;

        (void)memset(&th, 0, sizeof(th));
        if (iv_config_get_dbl(g_cfg, "elec.volt_high", &dv) == IV_OK)
            th.hv = (int32_t)dv;
        if (iv_config_get_dbl(g_cfg, "elec.volt_low", &dv) == IV_OK)
            th.lv = (int32_t)dv;
        if (iv_config_get_dbl(g_cfg, "elec.over_current", &dv) == IV_OK)
            th.ov = (int32_t)dv;
        if (iv_config_get_dbl(g_cfg, "elec.leak_current", &dv) == IV_OK)
            th.ld = (int32_t)dv;
        if (iv_config_get_dbl(g_cfg, "sensor.tilt", &dv) == IV_OK)
            th.sl = (int32_t)dv;
        if (iv_config_get_dbl(g_cfg, "sensor.temp_high", &dv) == IV_OK)
            th.tu = (int32_t)dv;
        if (iv_config_get_dbl(g_cfg, "sensor.temp_low", &dv) == IV_OK)
            th.tl = (int32_t)dv;
        if (iv_config_get_dbl(g_cfg, "sensor.humi_high", &dv) == IV_OK)
            th.hu = (int32_t)dv;
        if (iv_config_get_dbl(g_cfg, "sensor.humi_low", &dv) == IV_OK)
            th.hl = (int32_t)dv;

        trc = iv_agent_apply_thresh(&g_agent, &th, iv_clock_monotonic_ms());
        /* IV_ESTATE 在这里的含义是"九项全为 0，没什么可发"——正常情形（出厂态
         * 或用户把阈值全清空），不该按告警打。 */
        if (trc != IV_OK && trc != IV_ESTATE)
            IV_LOG_W(APP_MOD, "thresh.* not applied (%d)", trc);
    }
    if ((mask & IV_CFG_CLASS_MASK(IV_CFG_CLS_CAMERA)) != 0u) {
        /* `camera.detect.mode` / `camera.detect.rtt_ms` / `camera.ch.N.ip` 原先只在
         * `agent_setup()` 里读一次、交给 `iv_agent_open()` 定初始状态 —— 网页上改
         * 「检测方式」或摄像机 IP 都必须重启主控才生效（参考 web 那个下拉是保存即
         * 生效的，本机等于摆设）。现场最典型的动作就是"换个 IP 再测一下"。
         * 0 一律跳过不翻译：0 是 open 语境下"取默认"的哨兵，运行期当"用户没配"。 */
        uint32_t cips[IV_CAMDET_CH_N];
        int64_t  cv = 0;
        int      cmode = 0;
        uint32_t crtt = 0u;
        int      ci;
        int      crc;

        for (ci = 0; ci < IV_CAMDET_CH_N; ci++)
            cips[ci] = cfg_ip_be(CAM_IP_KEY[ci]);
        if (iv_config_get_int(g_cfg, "camera.detect.mode", &cv) == IV_OK && cv > 0)
            cmode = (int)cv;
        if (iv_config_get_int(g_cfg, "camera.detect.rtt_ms", &cv) == IV_OK && cv > 0)
            crtt = (uint32_t)cv;
        crc = iv_agent_reload_camdet(&g_agent, cmode, crtt, cips,
                                     iv_clock_monotonic_ms());
        if (crc != IV_OK)
            IV_LOG_W(APP_MOD, "camera.* not applied (%d)", crc);

        /* 录像源表与时间参数（2026-10-22）：`camera.ch.N.url` / `.record` /
         * `camera.rec.hold_ms` / `.debounce_ms` 原先只在 `iv_agent_open()` 读一次。
         * 同一张 `camera.ch` 表里 `ip` 11:20 就热了、相邻几列却要重启，现场最容易踩。
         * 判据与早返回都在 agent 侧（整类提交是常态，改别的键必须空转）。 */
        {
            char        ubuf[IV_RECCTL_CH_N][IV_CFG_STR_MAX];
            const char *urls[IV_RECCTL_CH_N];
            int64_t     rv = 0;
            uint32_t    rh = 0u;
            uint32_t    rd = 0u;
            int         rn;
            int         ri;
            int         rrc;

            rn = cfg_rec_url_table(ubuf, IV_RECCTL_CH_N);
            for (ri = 0; ri < IV_RECCTL_CH_N; ri++)
                urls[ri] = ubuf[ri];
            if (iv_config_get_int(g_cfg, "camera.rec.hold_ms", &rv) == IV_OK && rv > 0)
                rh = (uint32_t)rv;
            if (iv_config_get_int(g_cfg, "camera.rec.debounce_ms", &rv) == IV_OK &&
                rv > 0)
                rd = (uint32_t)rv;
            rrc = iv_agent_reload_rec(&g_agent, urls, rn, rh, rd);
            /* `IV_ESTATE` ＝ media 桥接没起来（此时配置已落盘，下次 open 生效），
             * 与 camdet 那半边同口径，不按告警刷屏。 */
            if (rrc != IV_OK && rrc != IV_ESTATE)
                IV_LOG_W(APP_MOD, "camera rec sources not applied (%d)", rrc);
        }

        /* SNMP 目标（`camera.snmp.*`）：与 `switch.snmp.*` 一起送，
         * 由 agent 按目标逐项比对（没变的那个一个字节都不动）。 */
        {
            iv_agent_snmp_cfg_t traps[IV_AGENT_SNMP_TARGET_N];
            int                 src;

            memset(traps, 0, sizeof(traps));
            cfg_snmp_targets(traps);
            src = iv_agent_reload_snmp(&g_agent, traps);
            if (src != IV_OK && src != IV_ESTATE)
                IV_LOG_W(APP_MOD, "snmp.* not applied (%d)", src);
        }
    }
    if ((mask & IV_CFG_CLASS_MASK(IV_CFG_CLS_SWITCH)) != 0u) {
        /* `switch.ip` 与 `switch.snmp.*`：前一版全无消费者（交换机页只读结果），
         * 改完只能重启。这里与 `camera.snmp.*` 走同一条热更（按目标逐项比对，
         * 摄像机那一份没变就跳过）。 */
        iv_agent_snmp_cfg_t traps[IV_AGENT_SNMP_TARGET_N];
        int                 src;

        memset(traps, 0, sizeof(traps));
        cfg_snmp_targets(traps);
        src = iv_agent_reload_snmp(&g_agent, traps);
        if (src != IV_OK && src != IV_ESTATE)
            IV_LOG_W(APP_MOD, "switch.snmp.* not applied (%d)", src);
    }
    if ((mask & IV_CFG_CLASS_MASK(IV_CFG_CLS_NETTHR)) != 0u) {
        /* `netthr.reboot_max` / `.reboot_interval_s` 原先只在 `agent_setup()` 里
         * 读一次、交给 `iv_agent_open()` 初始化 `iv_recovery` ⇒ 网页改完必须重启；
         * `netthr.loss_count` 更是**零消费方**的死键。
         *
         * `-1` / `0` 在这里表示"这一项这次没给"（0 是 `reboot_max` 的合法值＝禁用），
         * 与 `iv_agent_reload_camdet()` 同口径；归一化与"同值早返回"都在 agent 侧
         * 一处做 —— 那里早返回是硬要求，因为 `iv_recovery_set_limits()` 会重置
         * 每通道 credit，改个 `loss_count` 不该把自愈次数补满。 */
        int64_t nv       = 0;
        int     reb_max  = -1;
        uint32_t cd_s    = 0u;
        uint32_t loss_n  = 0u;
        int     nrc;

        if (iv_config_get_int(g_cfg, "netthr.reboot_max", &nv) == IV_OK && nv >= 0)
            reb_max = (int)nv;
        if (iv_config_get_int(g_cfg, "netthr.reboot_interval_s", &nv) == IV_OK && nv > 0)
            cd_s = (uint32_t)nv;
        if (iv_config_get_int(g_cfg, "netthr.loss_count", &nv) == IV_OK && nv > 0)
            loss_n = (uint32_t)nv;
        nrc = iv_agent_reload_netthr(&g_agent, reb_max, cd_s, loss_n);
        if (nrc != IV_OK)
            IV_LOG_W(APP_MOD, "netthr.* not applied (%d)", nrc);
    }
}

/* ---------------------------------------------------------------------------
 * M8-S8.3 受控远程通道的装配（模块本体见 src/modules/tunnel/iv_tunnel.c）
 * ---------------------------------------------------------------------------
 * 这一层只做"只有装配层才知道的事"，四件：
 *   ① **身份与令牌的来源**：SN 读工厂身份分区（架构钉死"身份不进配置"），令牌读
 *      `<secure>/tunnel.token`（**也不进配置** —— 配置是明文 JSON 且 `config.all`
 *      会把它整份导出给前端）。两者任一缺 ⇒ 通道开不了（fail-closed），
 *      而不是编一个"看起来合理"的默认值。
 *   ② **服务名 → 本机代理入口**的映射（resolver）。一律 127.0.0.1，模块侧另有环回
 *      硬闸门兜底；摄像机 IP 因此永远进不了 `frpc.toml`。
 *   ③ 配置键 `sys.tunnel.*` 的读取，以及**热更新**时把在开的会话作废。
 *   ④ 周期 `tick()`（到期自动关）。**第二道兜底在 scripts/frp.sh**：标记文件内容
 *      就是到期时刻，主控死了脚本也能判过期（见 iv_tunnel.h 头文件说明）。
 * ------------------------------------------------------------------------- */

/* 令牌落点：与 iv_prov 的 SECURE_DIR_DEFAULT 同源（/mnt/UDISK/ivsbox/secure）。
 * ⚠ F8 密钥分离未实现，这个目录目前**不是**真正的机密存储（与 `camera.ch.N.pass`、
 *   `camera.snmp.community` 同类已知欠账）；但它至少不会随 `config.all` 被导出、
 *   也不会出现在日志里 —— 比塞进普通配置强。 */
#define TUNNEL_TOKEN_FILE_DEFAULT "/mnt/UDISK/ivsbox/secure/tunnel.token"

/* 到期巡检周期：有效期以小时计，1 s 级没必要，5 s 足够及时且不吵。 */
#define TUNNEL_TICK_MS 5000

/* 句柄与 resolver 的目标端口在文件头声明（app_shutdown / on_config_event 先于
 * 本块用到它们），这里只放只与本块相关的常量与函数。 */

/* 服务名 → 本机代理入口。**只可能返回环回地址**：frp 的 `localIP` 必须是本机可达
 * 地址，所以摄像机 IP 永远进不了 `frpc.toml` —— 要访问摄像机只能过设备上的受控代理
 * （架构 §9.3 / S8.5）。非环回由 iv_tunnel 再拦一次（EHOST）。 */
static int tunnel_resolve(const iv_tunnel_svc_t *svc, uint32_t *ip_be, uint16_t *port,
                          void *arg)
{
    (void)arg;
    if (svc == NULL || ip_be == NULL || port == NULL)
        return IV_EINVAL;

    /* ip_be 的约定＝"数值本身按大端读"（见 test_tunnel 的 rig 与 iv_tunnel 的渲染：
     * 按字节取位输出 localIP），不要 htonl —— htonl 过的值在小端机上 >>24 是 0x01，
     * 会被环回闸门当成非环回拒掉。 */
    *ip_be = 0x7f000001u; /* 127.0.0.1 */
    switch (svc->kind) {
    case IV_TUNNEL_SVC_WEB:
        *port = g_tun_port_web;
        return IV_OK;
    case IV_TUNNEL_SVC_MEDIA:
        *port = g_tun_port_media;
        return IV_OK;
    case IV_TUNNEL_SVC_CAMERA:
        /* S8.5 落地前本机没有这个监听口。报"不可达"（EUNMAPPED 的语义）比把它映射到
         * 一个没人听的端口好：后者会让 frpc 起得来、连得上、但一访问就断，
         * 现场排查会从"通道"一路查到"摄像机"，方向全错。 */
        if (!g_tun_cam_enable)
            return IV_ENOENT;
        *port = g_tun_port_cam;
        return IV_OK;
    default:
        return IV_EINVAL;
    }
}

static const iv_tunnel_resolver_t s_tunnel_resolver = { tunnel_resolve, NULL };

/* 读令牌：取文件第一行、去掉 CR/LF（平台下发多半带换行），其余忽略。
 * 文件不在/为空**不算错误**：出厂未下发令牌的设备本来就该开不了通道。 */
static void tunnel_read_token(char *out, size_t cap)
{
    const char *env  = getenv("IVSBOX_TUNNEL_TOKEN");
    const char *path = getenv("IVSBOX_TUNNEL_TOKEN_FILE");
    char        buf[IV_TUNNEL_TOKEN_MAX];
    ssize_t     n;
    size_t      i;
    int         fd;

    out[0] = '\0';
    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, cap, "%s", env);
        return;
    }
    if (path == NULL || path[0] == '\0')
        path = TUNNEL_TOKEN_FILE_DEFAULT;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        IV_LOG_W(APP_MOD, "tunnel: token file absent (%s) - channel cannot be opened", path);
        return;
    }
    n = read(fd, buf, sizeof buf - 1u);
    (void)close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    for (i = 0; i < (size_t)n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            buf[i] = '\0';
            break;
        }
    }
    if (buf[0] != '\0')
        (void)snprintf(out, cap, "%s", buf);
}

/* 读设备 SN：工厂身份分区里的 `devid`（＝协议里的 `tid`，6 位大写 HEX）—— 这是本工程
 * 对应 BIRD 板卡 SN 的那个"设备固有标识"。
 * 读不到就留空；**不退回 MAC / 随机值**：钥匙派生式两侧必须一致，编一个"看起来对"的
 * SN 只会让运维侧算不出同一把钥匙，症状是"连不上但谁都没报错"。 */
static void tunnel_read_sn(char *out, size_t cap)
{
    const char  *env = getenv("IVSBOX_TUNNEL_SN");
    iv_prov_id_t id;
    char         tid[IV_PROV_TID_MAX];
    int          rc;

    out[0] = '\0';
    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, cap, "%s", env);
        return;
    }
    rc = iv_prov_private_read(NULL, &id);
    if (rc != IV_OK) {
        IV_LOG_W(APP_MOD, "tunnel: no device identity (private partition rc=%d) - "
                          "channel cannot be opened", rc);
        return;
    }
    iv_prov_tid_from_u32(id.devid, tid);
    if (strcmp(tid, IV_PROV_TID_BROADCAST) == 0) {
        IV_LOG_W(APP_MOD, "tunnel: device id is the broadcast placeholder (%s)", tid);
        return;
    }
    (void)snprintf(out, cap, "%s", tid);
}

/* 读配置：服务端落点 ＋ 本机三个代理端口 ＋ 摄像机代理开关。
 * 单独成函数是为了**配置热更新**也走同一条路（on_config_event）。 */
static void tunnel_apply_cfg(void)
{
    const char *host = cfg_str("sys.tunnel.server.host");
    uint16_t    port = cfg_port("sys.tunnel.server.port");
    uint16_t    p;
    int         bv = 0;

    p = cfg_port("sys.tunnel.web.port");
    if (p != 0u) g_tun_port_web = p;
    p = cfg_port("sys.tunnel.media.port");
    if (p != 0u) g_tun_port_media = p;
    p = cfg_port("sys.tunnel.cam.port");
    if (p != 0u) g_tun_port_cam = p;
    if (iv_config_get_bool(g_cfg, "sys.tunnel.cam.enable", &bv) == IV_OK)
        g_tun_cam_enable = (bv != 0);

    if (port == 0u)
        port = (uint16_t)IV_TUNNEL_PORT_DEF;

    /* 服务端变了 ⇒ 正在开的会话立刻作废：钥匙是按旧服务端场景配的，留着它只会让运维
     * "连得上但连不通"。身份与令牌不由配置改，故不在此列。 */
    if (host != NULL && g_tunnel->server_addr[0] != '\0' &&
        (strcmp(host, g_tunnel->server_addr) != 0 || port != g_tunnel->server_port)) {
        (void)iv_tunnel_invalidate(g_tunnel);
        IV_LOG_I(APP_MOD, "tunnel: frps -> %s:%u, any open session invalidated", host,
                 (unsigned)port);
    }
    if (host != NULL)
        (void)iv_tunnel_set_server(g_tunnel, host, port);
}

/* 周期巡检：到期自关。**回调内重挂**（iv_timer_add 是一次性语义，见 iv_reactor.h）。 */
static void on_tunnel_tick(void *arg)
{
    (void)arg;
    if (iv_tunnel_tick(g_tunnel, (uint64_t)time(NULL)) != 0)
        IV_LOG_I(APP_MOD, "tunnel: ttl expired, channel closed and flag cleared");
    g_tunnel_timer = iv_timer_add(g_reactor, TUNNEL_TICK_MS, on_tunnel_tick, NULL);
}

static void tunnel_setup(void)
{
    const char *env;
    char        sn[IV_TUNNEL_SN_MAX];
    char        token[IV_TUNNEL_TOKEN_MAX];

    iv_tunnel_init(g_tunnel);

    tunnel_read_sn(sn, sizeof sn);
    tunnel_read_token(token, sizeof token);
    if (sn[0] != '\0' && token[0] != '\0')
        (void)iv_tunnel_set_identity(g_tunnel, sn, token);

    /* 落点可被环境变量覆盖，验证时指到临时目录（与 IVSBOX_READY_MARK / IVSBOX_SD_ROOT
     * 同口径）。生产不设这两个变量，走 iv_tunnel 的编译期默认。 */
    env = getenv("IVSBOX_TUNNEL_CONF");
    if (env != NULL || getenv("IVSBOX_TUNNEL_FLAG") != NULL) {
        if (iv_tunnel_set_paths(g_tunnel, env, getenv("IVSBOX_TUNNEL_FLAG")) != IV_OK)
            IV_LOG_W(APP_MOD, "tunnel: bad path override in environment");
    }

    (void)iv_tunnel_set_resolver(g_tunnel, &s_tunnel_resolver);
    tunnel_apply_cfg();
    g_ctl_ctx.tunnel = g_tunnel;

    if (iv_tunnel_caps(g_tunnel) == 0u) {
        IV_LOG_W(APP_MOD, "tunnel: capability unavailable (needs sn + token + frps host) - "
                          "tunnel.enable will be refused");
    }
    g_tunnel_timer = iv_timer_add(g_reactor, TUNNEL_TICK_MS, on_tunnel_tick, NULL);
    if (g_tunnel_timer == NULL)
        IV_LOG_W(APP_MOD, "tunnel: tick timer not armed; ttl gate degraded to the shell side");
}

/* ---------------------------------------------------------------------------
 * M4-S4.2：本机网口的 IPv4 与 MAC（SADP Inquiry 字段）
 * 取不到就保持 0 / 全 0（iv_device 会填占位值，不影响其余三路发现）。
 * ------------------------------------------------------------------------- */
static void local_link_info(const char *ifname, uint32_t *ip_be, uint8_t mac[6])
{
    struct ifaddrs *head = NULL;
    struct ifaddrs *ifa;

    *ip_be = 0u;
    memset(mac, 0, 6u);
    if (ifname == NULL || ifname[0] == '\0')
        return;
    if (getifaddrs(&head) != 0)
        return;
    for (ifa = head; ifa != NULL; ifa = ifa->ifa_next) {
        const struct sockaddr *sa = ifa->ifa_addr;

        if (sa == NULL || strcmp(ifa->ifa_name, ifname) != 0)
            continue;
        if (sa->sa_family == AF_INET && *ip_be == 0u) {
            *ip_be = ((const struct sockaddr_in *)(const void *)sa)
                         ->sin_addr.s_addr;
        } else if (sa->sa_family == AF_PACKET) {
            const struct sockaddr_ll *sll =
                (const struct sockaddr_ll *)(const void *)sa;

            if (sll->sll_halen == 6u)
                memcpy(mac, sll->sll_addr, 6u);
        }
    }
    freeifaddrs(head);
}

/* ---------------------------------------------------------------------------
 * 平台 0xE1（查询设备当前参数设置）的 `data` 段
 * ---------------------------------------------------------------------------
 * 参考实现 `com.c:com_query_configuration_function()` 有 30 余个键，这里只送
 * **我们真有的**：配置键、芯片序列号、4G 快照、eth0 实取地址。
 * 本板没有对应外设/数据的（风扇温湿度 `ft`/`fh`、网络延时 `pt`、加热与漏电阈值、
 * 网络电源次数 `lannum`/`lantime`）一律送 0 —— 不编造，但**键一个都不能少**：
 * 平台是按 key 取的，缺键和值为 0 在界面上是两种表现。
 * 所有值都是**字符串**，与参考实现 `my_cjson_join_string_function()` 一致
 * （`"rt":"180,0,0"` 而不是 `"rt":180`）。
 * ------------------------------------------------------------------------- */

/* "host:port" —— 任一半缺失就整条空串（参考实现 sprintf("%s:%d") 会出 ":0"，
 * 平台会把它当合法地址显示，比空串更误导）。 */
static const char *endpoint_of(const char *hk, const char *pk,
                               char *out, size_t cap)
{
    const char *h = cfg_str(hk);
    uint16_t    p = cfg_port(pk);

    if (h == NULL || p == 0u) {
        out[0] = '\0';
        return out;
    }
    (void)snprintf(out, cap, "%s:%u", h, (unsigned)p);
    return out;
}

/* 网口的 IPv4 / 掩码 / MAC ＋ 默认网关。
 * 网关从 `/proc/net/route` 取：Destination=00000000 且 Iface 匹配的那条。
 * ⚠ 该文件打印的是 `s_addr` 的**内存值**（小端），`strtoul` 的结果直接赋给
 * `s_addr` 即可，不要再 `htonl`。 */
static void if_facts(const char *ifname, char ip[20], char mask[20],
                     char gw[20], char mac[20])
{
    struct ifaddrs *head = NULL;
    struct ifaddrs *ifa;
    FILE           *fp;

    ip[0] = mask[0] = gw[0] = mac[0] = '\0';
    if (ifname == NULL || ifname[0] == '\0')
        return;

    if (getifaddrs(&head) == 0) {
        for (ifa = head; ifa != NULL; ifa = ifa->ifa_next) {
            const struct sockaddr *sa = ifa->ifa_addr;

            if (sa == NULL || strcmp(ifa->ifa_name, ifname) != 0)
                continue;
            if (sa->sa_family == AF_INET) {
                const struct sockaddr_in *in =
                    (const struct sockaddr_in *)(const void *)sa;

                if (ip[0] == '\0')
                    (void)inet_ntop(AF_INET, &in->sin_addr, ip, 20);
                if (ifa->ifa_netmask != NULL)
                    (void)inet_ntop(AF_INET,
                                    &((const struct sockaddr_in *)
                                          (const void *)ifa->ifa_netmask)->sin_addr,
                                    mask, 20);
            } else if (sa->sa_family == AF_PACKET) {
                const struct sockaddr_ll *sll =
                    (const struct sockaddr_ll *)(const void *)sa;

                if (sll->sll_halen == 6u)
                    (void)snprintf(mac, 20, "%02x:%02x:%02x:%02x:%02x:%02x",
                                   sll->sll_addr[0], sll->sll_addr[1],
                                   sll->sll_addr[2], sll->sll_addr[3],
                                   sll->sll_addr[4], sll->sll_addr[5]);
            }
        }
        freeifaddrs(head);
    }

    fp = fopen("/proc/net/route", "r");
    if (fp != NULL) {
        char line[256];

        (void)fgets(line, sizeof(line), fp); /* 表头 */
        while (fgets(line, sizeof(line), fp) != NULL) {
            char          name[32];
            char          dest[16];
            char          gate[16];
            unsigned long flags = 0ul;

            if (sscanf(line, "%31s %15s %15s %lx", name, dest, gate, &flags) != 4)
                continue;
            if (strcmp(name, ifname) != 0)
                continue;
            if (strtoul(dest, NULL, 16) != 0ul)
                continue;
            if (strtoul(gate, NULL, 16) == 0ul)
                continue;
            {
                struct in_addr a;

                a.s_addr = (uint32_t)strtoul(gate, NULL, 16);
                (void)inet_ntop(AF_INET, &a, gw, 20);
            }
            break;
        }
        (void)fclose(fp);
    }
}

struct cfg_json {
    char  *p;
    size_t cap;
    size_t n;
    int    ovf;
};

static void cj_str(struct cfg_json *j, const char *key, const char *val)
{
    int w;

    if (j->ovf)
        return;
    w = snprintf(j->p + j->n, j->cap - j->n, "%s\"%s\":\"%s\"",
                 (j->n > 0u) ? "," : "", key, (val != NULL) ? val : "");
    if (w < 0 || (size_t)w >= j->cap - j->n) {
        j->ovf = 1;
        return;
    }
    j->n += (size_t)w;
}

static void cj_int(struct cfg_json *j, const char *key, long long v)
{
    char b[24];

    (void)snprintf(b, sizeof(b), "%lld", v);
    cj_str(j, key, b);
}

static int query_cfg_json(char *out, size_t cap, void *arg)
{
    struct cfg_json       j;
    const iv_cell_snap_t *cs;
    char                  ep[96];
    char                  ip[20];
    char                  mask[20];
    char                  gw[20];
    char                  mac[20];
    char                  key[40];
    char                  pair[96];
    const char           *t1;
    const char           *t2;
    int64_t               v = 0;
    int                   i;

    (void)arg;
    if (out == NULL || cap == 0u)
        return IV_EINVAL;
    j.p = out;
    j.cap = cap;
    j.n = 0u;
    j.ovf = 0;

    cs = &g_agent.cell.snap;
    /* 参考实现的 `local->ip/nm/gw` 是 lwIP 的**本机网卡**（有线口），对应本板 eth0。 */
    if_facts("eth0", ip, mask, gw, mac);

    cj_str(&j, "tn", ""); /* 本板无"设备名称"配置项，宁缺不编 */
    cj_str(&j, "tsn", g_tsn);
    cj_str(&j, "isi", endpoint_of("net.server.wired.host",
                                  "net.server.wired.port", ep, sizeof(ep)));
    cj_str(&j, "osi", endpoint_of("net.server.wireless.host",
                                  "net.server.wireless.port", ep, sizeof(ep)));
    cj_str(&j, "usi", endpoint_of("net.server.upgrade.host",
                                  "net.server.upgrade.port", ep, sizeof(ep)));
    cj_str(&j, "iccid", cs->iccid);
    /* IV_CELL 只读 ICCID/IMSI，没有 IMEI —— 不拿 IMSI 冒充。 */
    cj_str(&j, "imei", "");
    /* 信号：参考实现送 +CSQ 原始值 0..31；未知（99/未读）送 -1，别送 0
     * （0 是合法的"极弱信号"，和"没读到时"是两件事）。 */
    cj_int(&j, "dbm", (cs->csq >= 0 && cs->csq <= 31) ? (long long)cs->csq : -1);

    if (iv_config_get_int(g_cfg, "sys.transport.mode", &v) != IV_OK)
        v = 0;
    cj_int(&j, "tm", (long long)v);
    cj_str(&j, "mac", mac);
    cj_str(&j, "ip", ip);
    cj_str(&j, "nm", mask);
    cj_str(&j, "gw", gw);

    /* 主网检测 IP：参考实现是 "主,副" 两个 IPv4。
     * 两个都没配时**给空串**，不能发孤零零的 `","` —— 平台会把它当"两个空 IP"
     * 显示成一行空白，比空串更容易被误判成"设备回了但内容坏了"。 */
    t1 = cfg_str("probe.target1");
    t2 = cfg_str("probe.target2");
    if (t1 == NULL && t2 == NULL) {
        cj_str(&j, "mip", "");
    } else {
        (void)snprintf(pair, sizeof(pair), "%s,%s",
                       (t1 != NULL) ? t1 : "", (t2 != NULL) ? t2 : "");
        cj_str(&j, "mip", pair);
    }

    /* 上报间隔：参考实现发 "秒,0,0"（后两档是给多段间隔预留的） */
    if (iv_config_get_int(g_cfg, "sys.report.interval_s", &v) != IV_OK || v < 0)
        v = 0;
    (void)snprintf(pair, sizeof(pair), "%lld,0,0", (long long)v);
    cj_str(&j, "rt", pair);

    /* ping 每轮间隔 / 每个设备间隔 —— 两个字段**量纲不同**，别再一起 /1000：
     *   `pl`（每轮间隔）仍是**秒**：同一批"间隔"类字段里 `rt`（定时上报间隔）就是秒，
     *        继续 `v / 1000`；
     *   `pn`（每设备超时）是**毫秒**，原样送。三条证据：
     *        ① 平台参数设置页那一项自己就标着「有线网络超时时间(ms)」；
     *        ② 10-26 15:51 板端实抓 `0xFE … 01 64`（界面填 100）⇒ 下发就是毫秒、
     *           不换算（见 `on_plat_net_timeout()`）；
     *        ③ 16:09 用户明确该单位是毫秒。
     * 读写量纲必须一致：写下去 100（ms）、读回来 0（秒）⇒ 平台设完再打开页面显示 0，
     * 现场会以为"没保存上"，然后再点一次。 */
    if (iv_config_get_int(g_cfg, "probe.interval_ms", &v) != IV_OK)
        v = 0;
    cj_int(&j, "pl", (long long)(v / 1000));
    if (iv_config_get_int(g_cfg, "probe.timeout_ms", &v) != IV_OK)
        v = 0;
    cj_int(&j, "pn", (long long)v);

    /* 风扇温湿度 / 网络延时：本板无风扇、无该配置项 ⇒ 0，不编 */
    cj_str(&j, "ft", "0,0");
    cj_str(&j, "fh", "0,0");
    cj_int(&j, "pt", 0);

    /* 六路摄像机 IP（c1..c6） */
    for (i = 0; i < IV_REPORT_CNS_N; i++) {
        const char *camip = NULL;

        (void)snprintf(key, sizeof(key), "camera.ch.%d.ip", i);
        if (iv_config_get_str(g_cfg, key, &camip) != IV_OK || camip == NULL)
            camip = "";
        (void)snprintf(key, sizeof(key), "c%d", i + 1);
        cj_str(&j, key, camip);
    }

    /* 电压/电流/漏电/倾斜阈值与网络电源次数：本板无对应采集 ⇒ 0 */
    cj_int(&j, "ov", 0);
    cj_int(&j, "uv", 0);
    cj_int(&j, "oc", 0);
    cj_int(&j, "angle", 0);
    cj_int(&j, "miu", 0);
    cj_int(&j, "lannum", 0);
    cj_int(&j, "lantime", 0);

    return j.ovf ? IV_ERANGE : (int)j.n;
}

/* ---------------------------------------------------------------------------
 * M4-S4.3：读一个 SNMP 目标的配置（**不新增配置键**，全部复用 S4.3 已冻结的
 * `camera.snmp.*` / `switch.snmp.*`）。
 * OID 槽位按 `slots[]`（规范表 `iv_snmp_cam_slots` / `iv_snmp_sw_slots`）顺序读；
 * **位置保持一一对应**：中间某槽没配就留空串、不压缩下标，否则
 * `iv_agent_snmp_get()` 回报的槽位名会错位。n_oid 取"最后一个非空槽 + 1"。
 * ------------------------------------------------------------------------- */
static void cfg_snmp_target(iv_agent_snmp_cfg_t *sc, const char *pfx,
                            const char *const *slots, int n_slot,
                            uint32_t ip_be)
{
    char    key[96];
    int64_t v = 0;
    int     b = 0;
    int     i;

    memset(sc, 0, sizeof(*sc));
    sc->ip_be = ip_be;

    if (snprintf(key, sizeof(key), "%s.enable", pfx) < (int)sizeof(key) &&
        iv_config_get_bool(g_cfg, key, &b) == IV_OK)
        sc->enable = b;
    if (snprintf(key, sizeof(key), "%s.community", pfx) < (int)sizeof(key)) {
        const char *s = NULL;

        if (iv_config_get_str(g_cfg, key, &s) == IV_OK && s != NULL)
            (void)snprintf(sc->community, sizeof(sc->community), "%.*s",
                           (int)sizeof(sc->community) - 1, s);
    }
    if (snprintf(key, sizeof(key), "%s.timeout_ms", pfx) < (int)sizeof(key) &&
        iv_config_get_int(g_cfg, key, &v) == IV_OK && v > 0)
        sc->timeout_ms = (uint32_t)v;
    if (snprintf(key, sizeof(key), "%s.retries", pfx) < (int)sizeof(key) &&
        iv_config_get_int(g_cfg, key, &v) == IV_OK && v >= 0)
        sc->retries = (uint32_t)v;

    for (i = 0; i < n_slot && i < (int)IV_AGENT_SNMP_SLOT_MAX; i++) {
        const char *s = NULL;

        sc->oid[i][0] = '\0';
        if (snprintf(key, sizeof(key), "%s.oid.%s", pfx, slots[i]) >=
            (int)sizeof(key))
            continue;
        if (iv_config_get_str(g_cfg, key, &s) != IV_OK || s == NULL)
            continue;
        (void)snprintf(sc->oid[i], IV_SNMP_OID_STR_MAX, "%.*s",
                       (int)IV_SNMP_OID_STR_MAX - 1, s);
        if (sc->oid[i][0] != '\0')
            sc->n_oid = i + 1; /* 位置语义：下标＝规范槽位表下标 */
    }
}

/* 两个目标一次填好：`agent_setup()` 与"改完热更"（`on_cfg_committed`）共用同一段，
 * 否则开机一套、改配置另一套，迟早漂移。 */
static void cfg_snmp_targets(iv_agent_snmp_cfg_t *sc)
{
    cfg_snmp_target(&sc[IV_AGENT_SNMP_CAM], "camera.snmp", iv_snmp_cam_slots,
                    (int)IV_SNMP_CAM_SLOT_N, cfg_ip_be("camera.ch.0.ip"));
    cfg_snmp_target(&sc[IV_AGENT_SNMP_SW], "switch.snmp", iv_snmp_sw_slots,
                    (int)IV_SNMP_SW_SLOT_N, cfg_ip_be("switch.ip"));
}

/* ---------------------------------------------------------------------------
 * S6.5 前置：**断网录像的接线**（键早在默认表里，本步只是把没人读的键接上）
 *
 *   - 路级：`camera.ch.N.url`（录像源 RTSP 地址，**含账密**）＋ `camera.ch.N.record`
 *     （该路是否录像）。**两者同时满足**该路才进表；`n_url` 取"最后一个进表的路 ＋ 1"，
 *     中间未进表的路**留空串** —— `iv_recctl` 对空串路直接跳过（`url[i][0] == '\0'`），
 *     所以空洞不会对不存在的地址发 start。对齐用户 2026-10-14 拍板：
 *     「配置完成后才进行录制，参数错误、未配置不录像」。
 *   - 全局：`camera.rec.hold_ms` / `camera.rec.debounce_ms`（原为 recctl 编译期常量）。
 *
 * ⚠ 路数**两个域**：本表是**录像侧 8 路**（`IV_RECCTL_CH_N`）；只有**前 6 路**会被
 *   网络探测（平台上报帧 `CNS` 段固定 6 宽），第 7/8 路**只录不测**。
 * ⚠ 地址长度受 `IV_CFG_STR_MAX`（160 含 NUL）限制：超长的 URL 在 `set` 时就被拒。
 * ------------------------------------------------------------------------- */
/* 录像源表的**只读数**：把 `camera.ch.N.url`（含账密）与 `.record` 合成一张表，
 * 未进表（没地址 / 开关没开）的路写空串。返回表长（最后一个进表的路 ＋ 1）。
 * 抽出来给"开机装配"与"热更"共用 —— 两处各写一遍迟早不一致。 */
static int cfg_rec_url_table(char (*buf)[IV_CFG_STR_MAX], int cap)
{
    char    key[IV_CFG_KEY_MAX];
    int     n = 0;
    int     i;

    for (i = 0; i < cap; i++) {
        const char *url = NULL;
        int         rec = 0;

        buf[i][0] = '\0';
        if (snprintf(key, sizeof(key), "camera.ch.%d.url", i) >= (int)sizeof(key))
            continue;
        if (iv_config_get_str(g_cfg, key, &url) != IV_OK || url == NULL ||
            url[0] == '\0')
            continue; /* 未配地址 ⇒ 该路不录 */
        if (snprintf(key, sizeof(key), "camera.ch.%d.record", i) >= (int)sizeof(key))
            continue;
        if (iv_config_get_bool(g_cfg, key, &rec) != IV_OK || rec == 0)
            continue; /* 录像开关未开 */
        (void)snprintf(buf[i], IV_CFG_STR_MAX, "%s", url);
        n = i + 1;
    }
    return n;
}

static void cfg_rec_sources(iv_agent_cfg_t *ac)
{
    int64_t v = 0;
    int     i;
    char    ubuf[IV_RECCTL_CH_N][IV_CFG_STR_MAX];
    int     un;

    ac->rec_n_url = 0;
    for (i = 0; i < IV_RECCTL_CH_N; i++)
        ac->rec_url[i][0] = '\0';

    un = cfg_rec_url_table(ubuf, IV_RECCTL_CH_N);
    for (i = 0; i < IV_RECCTL_CH_N; i++)
        (void)snprintf(ac->rec_url[i], sizeof(ac->rec_url[i]), "%s", ubuf[i]);
    ac->rec_n_url = un;

    if (iv_config_get_int(g_cfg, "camera.rec.hold_ms", &v) == IV_OK && v > 0)
        ac->rec_hold_ms = (uint32_t)v;
    if (iv_config_get_int(g_cfg, "camera.rec.debounce_ms", &v) == IV_OK && v > 0)
        ac->rec_debounce_ms = (uint32_t)v;
}

/* ---------------------------------------------------------------------------
 * 平台下发：0xF5「配置定时上报间隔」
 * ---------------------------------------------------------------------------
 * 语义逐条对齐参考实现 `com.c:com_set_next_report_time()`（2026-10-20 核对）：
 *   `DATA = sel(1) + 秒数(大端 u16)`，共 3 字节；
 *   - `秒 == 0`  ⇒ 不采纳、回 `0x74`；
 *   - `sel != 1` ⇒ 参考实现只在 `sel == 1` 时真正写入，其余选择子照样回 `0x01`；
 *   - 其余       ⇒ 写入（**先改内存、再落盘**）并回 `0x01`。
 *
 * 为什么要补：原先把 0xF5 落到"未注册命令一律回 OK"的默认分支 —— 平台收到
 * `0x01` 就认定"设备已按新间隔上报"，而设备的周期一分没动，两边对"多久一条"
 * 的认知从此不一致。这是平台**唯一**能改我们上报节拍的入口，不能只回话不干活。
 *
 * 顺序照抄参考实现：先让新周期在运行期生效，再落盘（落盘失败不影响本次生效，
 * 只影响下次启动）。保存成功后配置层还会通知 `on_cfg_committed(SYS)` 再调一次
 * `iv_report_set_report_period()` —— 同值早返回，不会抖。
 * ------------------------------------------------------------------------- */
static void on_plat_report_intv(const iv_proto_frame_t *f, void *arg)
{
    uint8_t  sel = 0;
    uint16_t sec = 0;
    int64_t  cur = 0;
    int      rc;

    (void)arg;
    if (f == NULL || g_cfg == NULL)
        return;

    if (iv_report_parse_cfg_intv(f->data, f->len, &sel, &sec) != IV_OK) {
        IV_LOG_W(APP_MOD, "0xF5 payload too short (%u bytes), ack 0x74",
                 (unsigned)f->len);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    if (sec == 0u) {
        IV_LOG_W(APP_MOD, "0xF5 sel=%u interval=0s rejected, ack 0x74", (unsigned)sel);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    if (sel != 1u) {
        /* 参考实现只在 sel==1 时写入，但**照样回 0x01**（= “已处理”） */
        IV_LOG_I(APP_MOD, "0xF5 sel=%u interval=%us ignored (only sel=1 applied)",
                 (unsigned)sel, (unsigned)sec);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
        return;
    }
    if (iv_config_get_int(g_cfg, "sys.report.interval_s", &cur) == IV_OK &&
        cur == (int64_t)sec) {
        /* 同值：不重复落盘（平台可能反复下发同一配置，别白写 flash） */
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
        return;
    }

    rc = iv_config_set_int(g_cfg, "sys.report.interval_s", (int64_t)sec);
    if (rc != IV_OK) {
        /* 超出默认值表范围（5..3600 s）：如实回失败，而不是假装设成功 */
        IV_LOG_W(APP_MOD, "0xF5 interval=%us out of range (%d), ack 0x74",
                 (unsigned)sec, rc);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    (void)iv_report_set_report_period(&g_agent.rp, (uint32_t)sec * 1000u);
    rc = iv_config_save(g_cfg, NULL, 0);
    IV_LOG_I(APP_MOD, "platform set report interval -> %u s (save rc=%d)",
             (unsigned)sec, rc);
    (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
}

/* ---------------------------------------------------------------------------
 * 平台下行 0xF3「配置传输模式」
 *
 * 载荷 1 字节：1=有线 2=无线 3=有线+无线 4=自动选择（与 iv_netmgr_mode_t 同值）。
 * 参考实现存进 local->server_mode 后回 0x01；我们多一步"立刻生效" —— 原先这个键
 * 只在启动时读一次，改了要重启，那等于平台点了个没反应的按钮。
 *
 * 生效会**拆一次现有平台连接**（活动出口换了就得按新出口重连，见
 * iv_agent_set_transport_mode 的说明），所以同值下发直接回 0x01、不重复落盘、
 * 不重复重连。
 *
 * ==================== ACK 必须在切链路**之前**发出去 ====================
 * 板端实测（2026-10-26 14:38:30）：平台下发 `f3 … 01 45 ffff`（改成有线），设备侧
 * 一路都对 —— `transport mode -> 1 (active wan 0 -> 1)`、`bind=eth0`、落盘 version=90、
 * 重连后 `transport state -> 2`（链路恢复、心跳照收）—— 但平台界面"点了没反应"。
 * 原因是**应答被自己掐掉了**：
 *
 *   ① `iv_agent_set_transport_mode()` → `iv_agent_reload_server()` → 重连；
 *   ② 重连走 `iv_transport_open()`，那里把 `t->tx_len` **清零**；
 *   ③ 而 `iv_transport_send()` 只是把帧 memcpy 进 `tbuf`，真正的 `write()` 在
 *      `iv_transport_step()` 里按 reactor 节拍（200 ms）才刷；
 *   ④ 于是"先切网、后 ACK" ⇒ ACK 进来时 `state` 已不是 `UP`，`iv_transport_send()`
 *      直接回 `IV_ESTATE`（它**刻意不积压**："宁可让调用方丢弃，也不积压过期 ACK"）；
 *      就算抢在切网前进了 `tbuf`，也会被 ② 清掉。
 *      **两条路都是帧永久消失**，而 `(void)` 把返回码吞了，日志里一个字都没有。
 *
 * 这与 `0xF4` 是同一个坑的两个变体（那边是 `ip addr flush` 拆链路）。区别是那边
 * 还能用 `nanosleep(100 ms)` 等 `send()` 的字节出内核 —— **这里不行**：帧根本还没
 * `write()`，睡多久都躺在用户态暂存里。所以只能**把切网本身延后**到下一轮 reactor
 * （600 ms ≈ 3 个节拍），让 ACK 先被正常刷出去。
 *
 * 代价：落盘被挪进延后段，它失败时就只能记日志、没法再回 `0x74`（平台已经收到
 * `0x01`）。这是"应答要真实"与"链路已经不在了"之间躲不开的取舍，与 `0xF4` 的
 * `flush` 失败同理 —— 好在 `iv_config_save()` 只会在整表校验不过时失败，而我们
 * 刚写进去的只是一个 `1..4` 的枚举。
 * ------------------------------------------------------------------------- */

/* 1＝"落盘＋切网"已排队一次。重发的同一条 0xF3 会走上面的同值早返回，进不到这里。 */
static int g_tmode_apply_queued;

/* `0xF3` 的"落盘 ＋ 切网"延后到这里执行，理由见 `on_plat_mode()` 上方。
 *
 * 延后段读**配置当前值**而不是把 mode 存进全局：期间若又来一条不同的 0xF3，
 * 配置里的值已经是新的，直接按最新值生效即可（幂等，不需要"最后一次谁赢"的判定）。 */
static void on_transport_mode_apply_timer(void *arg)
{
    int64_t v = 0;
    int     mode;
    int     arc;
    int     src;

    (void)arg;
    g_tmode_apply_queued = 0;
    if (g_cfg == NULL)
        return;
    if (iv_config_get_int(g_cfg, "sys.transport.mode", &v) != IV_OK)
        return;
    mode = (int)v;

    /* 先落盘：它成功时会通过 `on_cfg_committed(SYS)` 顺带把模式切过去。 */
    src = iv_config_save(g_cfg, NULL, 0);
    if (src != IV_OK)
        IV_LOG_E(APP_MOD, "0xF3 mode=%d applied but NOT saved (rc=%d): it will "
                 "revert on the next boot", mode, src);

    /* 再显式调一次兜底：`save` 的通知若因故障没派发（或掩码里没有 SYS），
     * 这里是唯一的生效入口。同值早返回，重复调用无害。 */
    arc = iv_agent_set_transport_mode(&g_agent, (iv_netmgr_mode_t)mode,
                                      iv_clock_monotonic_ms());
    IV_LOG_I(APP_MOD, "platform set transport mode -> %d applied (apply rc=%d, save rc=%d)",
             mode, arc, src);
}

static void schedule_transport_mode_apply(void)
{
    if (g_reactor == NULL)
        return;
    if (g_tmode_apply_queued)
        return; /* 已排一次队；定时器按配置当前值生效，再排只会多存一次盘 */
    if (iv_timer_add(g_reactor, 600u, on_transport_mode_apply_timer, NULL) == NULL) {
        IV_LOG_E(APP_MOD, "transport mode timer full -- 0xF3 was acked but will NOT "
                 "be applied until the next boot");
        return;
    }
    g_tmode_apply_queued = 1;
}

static void on_plat_mode(const iv_proto_frame_t *f, void *arg)
{
    int64_t cur = 0;
    int     mode;
    int     rc;

    (void)arg;
    if (f == NULL || g_cfg == NULL)
        return;

    if (f->len < 1u) {
        IV_LOG_W(APP_MOD, "0xF3 payload empty (0 bytes), ack 0x74");
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    mode = (int)f->data[0];
    if (mode < (int)IV_NETMGR_MODE_WIRED_ONLY || mode > (int)IV_NETMGR_MODE_AUTO) {
        /* 参考实现对非法 mode 也是 0x74；这里同样不猜"你是不是想设自动" */
        IV_LOG_W(APP_MOD, "0xF3 mode=%d out of range (1..4), ack 0x74", mode);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    if (iv_config_get_int(g_cfg, "sys.transport.mode", &cur) == IV_OK &&
        cur == (int64_t)mode) {
        /* **同值也要留一行**（2026-10-26 补）：原来这里完全静默，于是平台点
         * 「配置」后，若下发值与当前相同，设备侧一个字都没有 —— 与"平台压根没发"
         * 在日志里长得一模一样，现场无法区分（15:32:19 那条 `f3 … 01` 就是这么
         * 凭空消失的）。同值早返回本身是对的（不重连、不写 flash），只是要留痕。 */
        IV_LOG_I(APP_MOD, "0xF3 mode=%d unchanged (already active), ack 0x01, "
                 "link untouched", mode);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
        return;
    }

    /* 只改内存里的值，**不动链路、不落盘** —— `iv_config_set_int()` 不派发提交
     * 通知，所以这一步是安全的。写进去之后重发的同一条 0xF3 会走上面的同值早返回，
     * 不会重复挂定时器。 */
    rc = iv_config_set_int(g_cfg, "sys.transport.mode", (int64_t)mode);
    if (rc != IV_OK) {
        IV_LOG_W(APP_MOD, "0xF3 mode=%d rejected by config layer (%d), ack 0x74",
                 mode, rc);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }

    /* ACK 先走。返回码**不能丢** —— 丢了就等于"平台收不到 0x01 而我们不知道"，
     * 正是这次要修的那种情形。现在丢不了：`iv_proto_ack()` 内部对发送失败统一
     * 记一条 `ack 0xXX/0xXX could not be sent`（见 `iv_proto.c`，判定收在一处，
     * 不靠 30 多个调用点各自记得判）。 */
    (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);

    schedule_transport_mode_apply();
}

/* ---------------------------------------------------------------------------
 * 平台下行 0xF4「配置设备 IP、网关」
 *
 * 载荷 12 字节：4 B IP ＋ 4 B 掩码 ＋ 4 B 网关（**全部大端/网络序**），与参考实现
 * `com.c:com_set_local_net()` 一致。板端实测抓到过真帧：
 *   `f0f0110a000b2cc1 f4 ffffffffffffffff 0c c0a80279 ffffff00 c0a80201 a3 ff...`
 *                                    ↑len      ↑.121      ↑/24       ↑.1
 *
 * ============================ 顺序是这条命令的全部难点 ============================
 * 改本机地址会**立刻断开**平台上那条 TCP。所以顺序只能是：
 *   ① 校验并解析 ⇒ ② 落盘（失败还能如实回 0x74）⇒ ③ **先把 0x01 发出去**
 *   ⇒ ④ 等一个往返的工夫 ⇒ ⑤ 才真正 flush/加地址。
 * 把 ⑤ 提到 ③ 前面，平台永远收不到应答 —— 界面上照样显示"执行失败"，
 * 而我们连原因都换掉了，比现在更难查。
 * ④ 那 100 ms 不是随手 sleep：`send()` 只是把字节交给内核，紧跟其后的 `addr flush`
 * 会把这条连接 RST 掉，缓冲里没来得及发的字节一起丢。
 * ------------------------------------------------------------------------- */
static void app_sleep_ms(unsigned ms)
{
    struct timespec ts;

    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static void on_plat_local_net(const iv_proto_frame_t *f, void *arg)
{
    static const char *const keys[3] = {"net.ip", "net.mask", "net.gateway"};
    iv_netcfg_t c;
    char        ipbuf[INET_ADDRSTRLEN]   = "";
    char        maskbuf[INET_ADDRSTRLEN] = "";
    char        gwbuf[INET_ADDRSTRLEN]   = "";
    char        oldv[3][INET_ADDRSTRLEN];
    const char *cur = NULL;
    int         rc;
    int         i;

    (void)arg;
    if (f == NULL || g_cfg == NULL)
        return;

    rc = iv_netcfg_parse(f->data, f->len, &c);
    if (rc != IV_OK) {
        IV_LOG_W(APP_MOD, "0xF4 payload rejected (rc=%d, %u bytes), ack 0x74",
                 rc, (unsigned)f->len);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    if (inet_ntop(AF_INET, &c.ip, ipbuf, sizeof ipbuf) == NULL ||
        inet_ntop(AF_INET, &c.mask, maskbuf, sizeof maskbuf) == NULL ||
        inet_ntop(AF_INET, &c.gw, gwbuf, sizeof gwbuf) == NULL) {
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }

    /* 同值早返回：每应用一次就要拆一次链路，不能为"什么都没变"付这个代价。
     * 三个键都比，不能只比 IP —— 只改网关也是合法的下发。 */
    if (iv_config_get_str(g_cfg, keys[0], &cur) == IV_OK && cur != NULL &&
        strcmp(cur, ipbuf) == 0) {
        if (iv_config_get_str(g_cfg, keys[1], &cur) == IV_OK && cur != NULL &&
            strcmp(cur, maskbuf) == 0) {
            if (iv_config_get_str(g_cfg, keys[2], &cur) == IV_OK && cur != NULL &&
                strcmp(cur, gwbuf) == 0) {
                IV_LOG_I(APP_MOD, "0xF4 unchanged (%s/%s gw %s), ack 0x01, link untouched",
                         ipbuf, maskbuf, gwbuf);
                (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
                return;
            }
        }
    }

    /* ---- 落盘：写不进去就如实回 0x74，**不动网络** ----
     * 反过来（先改网络再落盘）会留下"地址改了但存不住"的状态：重启又回旧地址，
     * 现场看到的是"改过、自己变回去了"，比直接失败难查十倍。 */
    (void)memset(oldv, 0, sizeof oldv);
    for (i = 0; i < 3; i++) {
        const char *o = NULL;
        int         j;

        if (iv_config_get_str(g_cfg, keys[i], &o) == IV_OK && o != NULL)
            (void)snprintf(oldv[i], sizeof oldv[i], "%s", o);
        if (iv_config_set_str(g_cfg, keys[i], (i == 0) ? ipbuf
                                : (i == 1) ? maskbuf : gwbuf) == IV_OK)
            continue;
        for (j = 0; j < i; j++) /* 已写进去的逐个退回旧值，不留半新半旧 */
            (void)iv_config_set_str(g_cfg, keys[j], oldv[j]);
        IV_LOG_W(APP_MOD, "0xF4 set %s failed, rolled back, ack 0x74", keys[i]);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    {
        int save_rc = iv_config_save(g_cfg, NULL, 0);

        if (save_rc != IV_OK) {
            for (i = 0; i < 3; i++)
                (void)iv_config_set_str(g_cfg, keys[i], oldv[i]);
            IV_LOG_W(APP_MOD, "0xF4 save failed (%d), rolled back, ack 0x74", save_rc);
            (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
            return;
        }
    }

    IV_LOG_I(APP_MOD, "platform set local net -> %s/%s gw %s (acked first, applying next)",
             ipbuf, maskbuf, gwbuf);
    (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);

    /* 先把应答送出去，再动网口 —— 见本节开头的 ④。 */
    app_sleep_ms(100u);
    rc = iv_netcfg_apply(&c, NULL);
    if (rc != IV_OK)
        IV_LOG_E(APP_MOD, "0xF4 apply failed (%d): config saved but %s NOT changed",
                 rc, IV_NETCFG_IFNAME_DEFAULT);
    else
        IV_LOG_I(APP_MOD, "0xF4 applied: this host is now %s/%s gw %s",
                 ipbuf, maskbuf, gwbuf);
}

/* ---------------------------------------------------------------------------
 * 平台下行 0xF8「主网检测 IP」
 *
 * 载荷 8 字节：主网 4 B ＋ 副网 4 B（**大端**）。板端实抓真帧（11:40:46）：
 *   `f0f0110a000b2cc1 f8 ffffffffffffffff 08 c0a80201 c0a80202 68 ffff`
 *                                    ↑len      ↑192.168.2.1 ↑192.168.2.2
 * 界面「主网检测 IP 1 / IP 2」正是这两个值，落到 `probe.target1` / `probe.target2`
 * （配置项注释就写着"主网检测 IP 1/2"，不是新造键）。
 *
 * ============================ 为什么不回 0x74 ============================
 * 这条此前落"未注册 ⇒ 0x74"，平台点「配置主网检测IP」一直失败。但**数据我们
 * 全都有**：`probe.*` 五个键早在 `iv_agent_reload_probe()` 里接好了热更，缺的
 * 只是"把平台给的 8 字节翻译成那两个键"这一层。属于"该补"，不是"该回 0x74"。
 *
 * 与 `0xF4` 的关键区别：**不碰网口**，所以没有"断链"那一串顺序约束 ——
 * 落盘成功 → 生效 → 回 0x01 即可，不需要先 ack 再动手。
 * ------------------------------------------------------------------------- */
static void on_plat_ping_target(const iv_proto_frame_t *f, void *arg)
{
    static const char *const keys[2] = {"probe.target1", "probe.target2"};
    uint32_t tgt[2];
    char     val[2][INET_ADDRSTRLEN];
    char     oldv[2][INET_ADDRSTRLEN];
    const char *cur0 = NULL;
    const char *cur1 = NULL;
    int      rc;
    int      i;

    (void)arg;
    if (f == NULL || g_cfg == NULL)
        return;

    rc = iv_netcfg_parse_targets(f->data, f->len, &tgt[0], &tgt[1]);
    if (rc != IV_OK) {
        IV_LOG_W(APP_MOD, "0xF8 payload rejected (rc=%d, %u bytes), ack 0x74",
                 rc, (unsigned)f->len);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    for (i = 0; i < 2; i++) {
        if (inet_ntop(AF_INET, &tgt[i], val[i], sizeof val[i]) == NULL) {
            (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
            return;
        }
    }

    /* 同值早返回：`probe.*` 的热更会重建探测条目表，等于把"已经攒了多久没丢包"
     * 一起清掉。平台重复点同一个值不该白清一次。**两个目标都要比** ——
     * 只比主网会让"只改副网"变成静默不生效。 */
    if (iv_config_get_str(g_cfg, keys[0], &cur0) == IV_OK && cur0 != NULL &&
        iv_config_get_str(g_cfg, keys[1], &cur1) == IV_OK && cur1 != NULL &&
        strcmp(cur0, val[0]) == 0 && strcmp(cur1, val[1]) == 0) {
        IV_LOG_I(APP_MOD, "0xF8 unchanged (%s / %s), ack 0x01, probe untouched",
                 val[0], val[1]);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
        return;
    }

    /* ---- 落盘：先写两个键，任何一个写不进去就把已写的退回旧值 ---- */
    (void)memset(oldv, 0, sizeof oldv);
    for (i = 0; i < 2; i++) {
        const char *o = NULL;
        int         j;

        if (iv_config_get_str(g_cfg, keys[i], &o) == IV_OK && o != NULL)
            (void)snprintf(oldv[i], sizeof oldv[i], "%s", o);
        if (iv_config_set_str(g_cfg, keys[i], val[i]) == IV_OK)
            continue;
        for (j = 0; j < i; j++)
            (void)iv_config_set_str(g_cfg, keys[j], oldv[j]);
        IV_LOG_W(APP_MOD, "0xF8 set %s failed, rolled back, ack 0x74", keys[i]);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    {
        int save_rc = iv_config_save(g_cfg, NULL, 0);

        if (save_rc != IV_OK) {
            for (i = 0; i < 2; i++)
                (void)iv_config_set_str(g_cfg, keys[i], oldv[i]);
            IV_LOG_W(APP_MOD, "0xF8 save failed (%d), rolled back, ack 0x74", save_rc);
            (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
            return;
        }
    }

    /* ---- 生效：与 `on_cfg_committed` 的 PROBE 分支**同一个入口**。
     * `iv_config_save()` 内部已经触发过一次 reload 事件（订阅里含 PROBE），订阅
     * 路径会走到这里；仍显式再调一次，口径与 `on_plat_mode()` 一致：
     * 应答必须真的对应"已经生效"，不能把生效挂在"订阅有没有订上"这种装配层
     * 细节上 —— 那正是本项目栽过五次的坑。函数内部对同值早返回，重复调用无害。 */
    {
        int64_t  pv   = 0;
        uint32_t i_ms = 0;
        uint32_t t_ms = 0;
        uint32_t f_n  = 0;
        int      prc;

        if (iv_config_get_int(g_cfg, "probe.interval_ms", &pv) == IV_OK)
            i_ms = (uint32_t)pv;
        if (iv_config_get_int(g_cfg, "probe.timeout_ms", &pv) == IV_OK)
            t_ms = (uint32_t)pv;
        if (iv_config_get_int(g_cfg, "probe.max_fail", &pv) == IV_OK)
            f_n = (uint32_t)pv;
        prc = iv_agent_reload_probe(&g_agent, tgt[0], tgt[1], i_ms, t_ms, f_n,
                                    iv_clock_monotonic_ms());
        if (prc != IV_OK) {
            /* 配置已落盘但没生效：如实回失败，别让平台以为成功（下次重启会生效，
             * 这条日志是现场唯一能看出"存了没生效"的地方）。 */
            IV_LOG_E(APP_MOD, "0xF8 saved but probe reload failed (%d), ack 0x74", prc);
            (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
            return;
        }
    }

    IV_LOG_I(APP_MOD, "platform set ping targets -> %s / %s (saved + applied)",
             val[0], val[1]);
    (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
}

/* ---------------------------------------------------------------------------
 * 平台下行 0xA5「配置搜索方式」
 *
 * 载荷 1 字节：1=PING 2=端口扫描 3=RTSP OPTIONS。与参考实现
 * `com_set_carema_search_mode_function()`（取 `buff[0]`）同口径，也与本机配置键
 * `camera.detect.mode`（1..3，见 `iv_config.c` 默认值表）**就是同一个东西**。
 *
 * ============================ 修的是什么 ============================
 * 原实现挂在 `iv_agent` 层（`on_cfg_search_mode`），只调 `iv_camdet_set_mode()`：
 * 改了**运行期**、从不落盘。后果两条：
 *   ① 平台收到 `0x01`（设置成功），设备一重启又回配置里的旧方式；
 *   ② 配置键一直是旧值 ⇒ 平台接着点「查询设备配置」(0xE1) 看到的是旧值，
 *      两边对"当前是哪种搜索方式"的认知从此不一致。
 * 这不是"少打了一行日志"，是**回了 OK 只干了一半活** —— 与 `0xF3` 之前那一批
 * "回了 0x01 没干活"的指令同一类。规矩就是本项目反复栽出来的那条：
 * **凡是要写配置的下行命令，一律在装配层注册**（本层才有 `g_cfg`），
 * 库层不许自行"改内存当生效"。
 *
 * 顺序：**不碰链路**，所以可以"先落盘、再生效、最后 ACK"（与 `0xF8` 同构）。
 * `iv_config_save()` 会派发 `on_cfg_committed(CAMERA)` → `iv_agent_reload_camdet()`
 * 顺带把运行期切过去；下面仍显式再调一次兜底，口径同 `0xF8`：应答必须真的对应
 * "已经生效"，不能把生效挂在"订阅有没有订上"这种装配层细节上。
 * ------------------------------------------------------------------------- */
static void on_plat_search_mode(const iv_proto_frame_t *f, void *arg)
{
    int64_t cur = -1;
    int     mode;
    int     rc;
    int     arc;

    (void)arg;
    if (f == NULL || g_cfg == NULL)
        return;

    if (f->data == NULL || f->len < 1u) {
        IV_LOG_W(APP_MOD, "0xA5 payload empty (0 bytes), ack 0x74");
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    mode = (int)f->data[0];
    if (mode < (int)IV_CAMDET_MODE_PING || mode > (int)IV_CAMDET_MODE_RTSP) {
        /* 参考实现对非法值同样是"不采纳"；这里如实回 0x74，不猜"你是不是想设 PING" */
        IV_LOG_W(APP_MOD, "0xA5 mode=%d out of range (1..3), ack 0x74", mode);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    /* 同值早返回：`iv_camdet_set_mode()` 会关掉在途探测 fd、清扫描轮、重建 PING
     * 引擎 —— 平台重复点同一个值不该白拆一轮（那段"空窗"里这一路会暂时没有状态）。 */
    if (iv_config_get_int(g_cfg, "camera.detect.mode", &cur) == IV_OK &&
        cur == (int64_t)mode) {
        IV_LOG_I(APP_MOD, "0xA5 unchanged (mode=%d %s), ack 0x01, detect untouched",
                 mode, iv_camdet_mode_name(mode));
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
        return;
    }
    rc = iv_config_set_int(g_cfg, "camera.detect.mode", (int64_t)mode);
    if (rc != IV_OK) {
        IV_LOG_W(APP_MOD, "0xA5 mode=%d rejected by config layer (%d), ack 0x74",
                 mode, rc);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    rc = iv_config_save(g_cfg, NULL, 0);
    if (rc != IV_OK) {
        /* 退回旧值：不留"内存新、盘上旧"的中间态（重启后自己变回去最难查） */
        if (cur >= 1)
            (void)iv_config_set_int(g_cfg, "camera.detect.mode", cur);
        IV_LOG_W(APP_MOD, "0xA5 save failed (%d), rolled back, ack 0x74", rc);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    arc = iv_agent_reload_camdet(&g_agent, mode, 0u, NULL, iv_clock_monotonic_ms());
    if (arc != IV_OK) {
        /* 已落盘但没生效：如实回失败（下次重启会生效，这条日志是现场唯一能看出
         * "存了没生效"的地方）。 */
        IV_LOG_E(APP_MOD, "0xA5 mode=%d saved but hot apply failed (%d), ack 0x74",
                 mode, arc);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }

    IV_LOG_I(APP_MOD, "platform set detect mode -> %d (%s) (saved + applied)",
             mode, iv_camdet_mode_name(mode));
    (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
}

/* ---------------------------------------------------------------------------
 * 平台下行 0xFE「有线网络超时时间(ms)」
 *
 * 板端实抓真帧（2026-10-26 15:51:18，界面填 100）：
 *   `f0f0110a000b2cc1 fe ffffffffffffffff 01 64 e2 ffff`
 *     head ver devtype devid  cmd  qn1       qn2  len data crc tail
 * ⇒ **载荷 1 字节、量纲＝毫秒**（`0x64` = 100，与界面上的 (ms) 一致）。
 *
 * 这一条拖了很久，原因是参考实现的注释写的是"1 字节**秒**"，而界面标的是 (ms)，
 * 差一个 x1000，两种解释都能自圆其说 —— 所以一直标着"量纲未知**不猜**"。
 * 现在真帧到手，按**实测**取值：100 就是 100 ms，不做任何换算。
 *
 * 落到 `probe.timeout_ms`（单次探测超时，毫秒，区间 100..60000，且必须 < 探测周期）。
 * 为什么不落 `netthr.timeout_ms`：那个键在默认值表里写着"保留键：**无消费方**，
 * 改后不生效（探测超时见 `probe.timeout_ms`）"，用户 10-22 已拍板不接。
 * 写进去就等于"回了 0x01 但什么都没变"——正是本轮在清的那类病。
 *
 * 顺序：**不碰平台链路**（只重建探测表），所以可以"落盘 → 生效 → ACK"，
 * 与 `0xF8` 同构；真实失败（超范围/热更失败）如实回 `0x74`。
 * ------------------------------------------------------------------------- */
static void on_plat_net_timeout(const iv_proto_frame_t *f, void *arg)
{
    int64_t  cur = -1;
    int64_t  pv  = 0;
    int      ms;
    int      rc;
    int      prc;
    uint32_t i_ms = 0u;
    uint32_t f_n  = 0u;

    (void)arg;
    if (f == NULL || g_cfg == NULL)
        return;

    if (f->data == NULL || f->len < 1u) {
        IV_LOG_W(APP_MOD, "0xFE payload empty (0 bytes), ack 0x74");
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    ms = (int)f->data[0];
    if (ms == 0) {
        /* 参考实现同样把 0 当非法：0 ms 超时没有意义，别悄悄改成"取默认" */
        IV_LOG_W(APP_MOD, "0xFE timeout=0ms rejected, ack 0x74");
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    if (iv_config_get_int(g_cfg, "probe.timeout_ms", &cur) == IV_OK &&
        cur == (int64_t)ms) {
        IV_LOG_I(APP_MOD, "0xFE timeout=%dms unchanged, ack 0x01, probe untouched", ms);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
        return;
    }

    rc = iv_config_set_int(g_cfg, "probe.timeout_ms", (int64_t)ms);
    if (rc != IV_OK) {
        /* 1 字节最大 255，区间下界是 100 ⇒ 只有 1..99 会被拒（如实回失败，
         * 不 clamp —— 悄悄改成 100 会让平台以为它就是 100） */
        IV_LOG_W(APP_MOD, "0xFE timeout=%dms out of range [100,60000] (%d), ack 0x74",
                 ms, rc);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    rc = iv_config_save(g_cfg, NULL, 0);
    if (rc != IV_OK) {
        if (cur >= 0)
            (void)iv_config_set_int(g_cfg, "probe.timeout_ms", cur);
        IV_LOG_W(APP_MOD, "0xFE save failed (%d), rolled back, ack 0x74", rc);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }

    /* 显式兜底生效（口径同 0xF8）：`iv_agent_reload_probe()` 内部对同值早返回，
     * 周期/失败次数按配置现值传，别因为"这次只改超时"就把它们清成 0。 */
    if (iv_config_get_int(g_cfg, "probe.interval_ms", &pv) == IV_OK && pv > 0)
        i_ms = (uint32_t)pv;
    if (iv_config_get_int(g_cfg, "probe.max_fail", &pv) == IV_OK && pv > 0)
        f_n = (uint32_t)pv;
    prc = iv_agent_reload_probe(&g_agent, cfg_ip_be("probe.target1"),
                                cfg_ip_be("probe.target2"), i_ms, (uint32_t)ms, f_n,
                                iv_clock_monotonic_ms());
    if (prc != IV_OK) {
        IV_LOG_E(APP_MOD, "0xFE saved but probe reload failed (%d), ack 0x74", prc);
        (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }

    IV_LOG_I(APP_MOD, "platform set wired net timeout -> %d ms (saved + applied)", ms);
    (void)iv_proto_ack(&g_agent.rp.pf, f->cmd, IV_PROTO_ACK_OK);
}

/* 把配置里存的静态地址（`net.ip`/`net.mask`/`net.gateway`）真正应用到网口。
 *
 * 两个调用点**共用同一段判定**（这是刻意的，不另写一份）：
 *   - 开机第 1b 步，见 `main()`；
 *   - Web 改完 `net.*` 之后，由 `schedule_local_net_apply()` 延迟 800 ms 触发。
 *
 * ============================ 为什么必须有这一段 ============================
 * "落盘"的全部意义就是**重启后还在**。没有这一步，0xF4 改完地址、一重启又回到
 * 出厂那套，平台会看到设备"自己变回去"，比改动不生效更难查。
 *
 * 出厂默认 `net.ip` 是空串（"由 DHCP 分配"）⇒ **什么都不做**，绝不覆盖现场
 * 由 rootfs/DHCP 配出来的地址。这一条是安全底线：写错了也不能让设备一开机就没网。
 *
 * 「IP ＋ 前缀长度」已经等于目标时直接跳过：否则每次开机都要 flush 一遍地址，
 * 平台那条 TCP 刚建好就被自己踹掉，日志上表现为"开机后平台反复重连"。
 */
static void apply_saved_local_net(void)
{
    iv_netcfg_t c;
    const char *ip = NULL;
    const char *mask = NULL;
    const char *gw = NULL;
    char        addrbuf[INET_ADDRSTRLEN + 8];

    if (g_cfg == NULL)
        return;
    if (iv_config_get_str(g_cfg, "net.ip", &ip) != IV_OK || ip == NULL || ip[0] == '\0')
        return;
    if (iv_config_get_str(g_cfg, "net.mask", &mask) != IV_OK || mask == NULL)
        return;
    if (iv_config_get_str(g_cfg, "net.gateway", &gw) != IV_OK || gw == NULL)
        return;

    (void)memset(&c, 0, sizeof c);
    if (inet_pton(AF_INET, ip, &c.ip) != 1 ||
        inet_pton(AF_INET, mask, &c.mask) != 1 ||
        inet_pton(AF_INET, gw, &c.gw) != 1) {
        IV_LOG_W(APP_MOD, "saved net.ip/mask/gateway not parseable (%s/%s/%s), skipped",
                 ip, mask, gw);
        return;
    }
    /* 与下发路径共用同一套校验：配置是人手改文件也能写进去的，不能假定它合法。 */
    {
        uint8_t raw[IV_NETCFG_PAYLOAD_LEN];

        (void)memcpy(raw, &c.ip, 4u);
        (void)memcpy(raw + 4, &c.mask, 4u);
        (void)memcpy(raw + 8, &c.gw, 4u);
        if (iv_netcfg_parse(raw, sizeof raw, &c) != IV_OK) {
            IV_LOG_W(APP_MOD, "saved local net rejected by the same guards, skipped");
            return;
        }
    }

    (void)snprintf(addrbuf, sizeof addrbuf, "%s/%d", ip, iv_netcfg_prefix(c.mask));
    /* 比到**前缀长度**这一级，不能只比 IP —— 网页上允许只改掩码（IP 不动），
     * 只比 IP 就会判"已在位、不动链路"，改了个寂寞而界面显示已保存。 */
    if (iv_netcfg_if_addr_equals(NULL, c.ip, c.mask) == 1) {
        IV_LOG_I(APP_MOD, "saved local net %s already in place, not touching the link",
                 addrbuf);
        return;
    }
    if (iv_netcfg_apply(&c, NULL) == IV_OK)
        IV_LOG_I(APP_MOD, "saved local net applied: %s gw %s", addrbuf, gw);
    else
        IV_LOG_E(APP_MOD, "saved local net %s could NOT be applied", addrbuf);
}

/* ---------------------------------------------------------------------------
 * 本机地址热生效（Web 改 `net.ip` / `net.mask` / `net.gateway` 之后）
 *
 * ============================ 为什么要延迟 ============================
 * 触发点 `on_cfg_committed()` 是在 `config.apply` 的**处理过程中**被回调的 ——
 * 此刻 ivsbox-web 还没拿到 ctl 应答，浏览器更没拿到 HTTP 响应。此时立刻
 * `ip addr flush`，浏览器看到的是"请求失败"，而且这个失败是**真的**（连接被拆了），
 * 现场会以为"保存没成功、再点一次"，实际配置已经存进去了。
 *
 * 800 ms 是给"ctl 应答 → ivsbox-web 写 HTTP 响应 → 浏览器收到并渲染 toast"留的
 * 余量。局域网里这段通常是几毫秒，800 ms 已经很宽松，而人对"点完 1 秒后断线"
 * 完全可以接受 —— 本来改 IP 就要断。
 *
 * ============================ 为什么用 reactor 定时器 ============================
 * 不能用 `nanosleep` 睡在回调里：那会**卡住整个 reactor**（平台链路、串口、
 * 通道全停 800 ms）。`iv_timer_add()` 是一次性定时器，回调结束即自动释放，
 * 不需要任何收尾。
 *
 * 同一时刻可能有多次提交（连点两次「配置 IP 和网关」）⇒ 会挂多个定时器，
 * 但 `apply_saved_local_net()` 自带"与内核现值相同就跳过"，第二次是空转，
 * 不会把刚设好的地址再拆一遍。为这点额外挂一个"已排队"标志不值当。
 * ------------------------------------------------------------------------- */
static void on_local_net_apply_timer(void *arg)
{
    (void)arg;
    apply_saved_local_net();
}

static void schedule_local_net_apply(void)
{
    if (g_reactor == NULL)
        return;
    if (iv_timer_add(g_reactor, 800u, on_local_net_apply_timer, NULL) == NULL)
        IV_LOG_W(APP_MOD, "local net apply timer full -- net.* saved but NOT applied");
}

static int agent_setup(void)
{
    iv_agent_cfg_t ac;
    int64_t        v;
    int            i;

    iv_agent_cfg_default(&ac);

    /* 平台地址：**按活动出口取**（下标＝iv_wan_t）—— 每条链路主/备各一份，活动
     * 出口由 sys.transport.mode ＋ netmgr 状态机决定。选地址的优先级链在
     * `iv_agent.c` 的 `iv_agent_pick_host()`：
     *     本口主 → 本口备 → 另一口主 → 另一口备
     * 四条全空 ⇒ 不启平台链路（iv_report 内部 idle）。
     * ⚠ 备份地址**不做失败切换**（用户 2026-10-22 拍板）：它只在主地址"没配"
     *   时才顶上，与"主地址连不上就换一条"是两件不同的事。 */
    ac.srv_host[IV_WAN_WIRELESS] = cfg_str("net.server.wireless.host");
    ac.srv_port[IV_WAN_WIRELESS] = cfg_port("net.server.wireless.port");
    ac.srv_host[IV_WAN_WIRED]    = cfg_str("net.server.wired.host");
    ac.srv_port[IV_WAN_WIRED]    = cfg_port("net.server.wired.port");
    ac.srv_host_bak[IV_WAN_WIRELESS] = cfg_str("net.server.wireless.host_bak");
    ac.srv_port_bak[IV_WAN_WIRELESS] = cfg_port("net.server.wireless.port_bak");
    ac.srv_host_bak[IV_WAN_WIRED]    = cfg_str("net.server.wired.host_bak");
    ac.srv_port_bak[IV_WAN_WIRED]    = cfg_port("net.server.wired.port_bak");

    /* 定时上报间隔（配置单位秒 → 毫秒） */
    if (iv_config_get_int(g_cfg, "sys.report.interval_s", &v) == IV_OK && v > 0)
        ac.report_ms = (uint32_t)v * 1000u;

    /* 传输模式：1=有线 2=无线 3=双 4=自动（与 iv_netmgr_mode_t 同值） */
    if (iv_config_get_int(g_cfg, "sys.transport.mode", &v) == IV_OK)
        ac.mode = (iv_netmgr_mode_t)v;

    /* 探活目标与节拍（探活失败阈值同时作为状态机判失败阈值，同源） */
    ac.probe_target_be[0] = cfg_ip_be("probe.target1");
    ac.probe_target_be[1] = cfg_ip_be("probe.target2");
    if (iv_config_get_int(g_cfg, "probe.interval_ms", &v) == IV_OK)
        ac.probe_interval_ms = (uint32_t)v;
    if (iv_config_get_int(g_cfg, "probe.timeout_ms", &v) == IV_OK)
        ac.probe_timeout_ms = (uint32_t)v;
    if (iv_config_get_int(g_cfg, "probe.max_fail", &v) == IV_OK)
        ac.probe_fail_n = (uint32_t)v;
    /* ⚠ 这里**只有** `probe_fail_n`。切换门限（`nm_fail_n`）的来源是
     * `netthr.loss_count`（架构 §10.2「切换门限由 netthr.* 承担」），见下方
     * netthr 段。2026-10-22 之前两处同源 ⇒ 界面上「连续失败次数上限」和
     * 「丢包次数上限」两个键实际只有一个算数，改另一个没反应。 */

    /* devid（TID）暂无配置键来源：待工厂身份分区（/dev/by-name/private）读取落地 */
    /* 摄像机网络状态检测（S4.1）：6 路 IP 与检测方式 */
    for (i = 0; i < IV_CAMDET_CH_N; i++)
        ac.camera_ip_be[i] = cfg_ip_be(CAM_IP_KEY[i]);
    if (iv_config_get_int(g_cfg, "camera.detect.mode", &v) == IV_OK && v > 0)
        ac.camera_detect_mode = (uint32_t)v;
    if (iv_config_get_int(g_cfg, "camera.detect.rtt_ms", &v) == IV_OK && v > 0)
        ac.camera_detect_rtt_ms = (uint32_t)v;

    /* 自愈（S4.4）：netthr.reboot_max（**0 = 禁用自愈**，对齐现网 net_reload=0）
     * 与 netthr.reboot_interval_s（动作后冷却秒数，现网 NETWAOK_RELOAD_TIME=60）。 */
    if (iv_config_get_int(g_cfg, "netthr.reboot_max", &v) == IV_OK && v >= 0)
        ac.netthr_reboot_max = (uint32_t)v;
    if (iv_config_get_int(g_cfg, "netthr.reboot_interval_s", &v) == IV_OK && v > 0)
        ac.netthr_reboot_interval_s = (uint32_t)v;
    /* 切换门限（`nm_fail_n`）的**唯一**来源：`netthr.loss_count`。
     * 它此前是零消费方的死键（门限由 `probe.max_fail` 喂），而架构 §10.2 明写
     * 「切换门限由 netthr.* 承担」⇒ 网页上「丢包次数上限」改多少次都不影响切换。 */
    if (iv_config_get_int(g_cfg, "netthr.loss_count", &v) == IV_OK && v > 0)
        ac.nm_fail_n = (uint32_t)v;

    /* 摄像机信息探测（S4.2）：开机一次自动搜索；SADP 走有线网口，报文字段取本机
     * 网口的 IPv4/MAC（取不到就留空，其余三路发现不受影响）。 */
    ac.dev_enable = 1;
    ac.dev_ifname[0] = '\0'; /* 空 ⇒ iv_agent 用 wan_name[IV_WAN_WIRED] */
    local_link_info(ac.wan_name[IV_WAN_WIRED], &ac.dev_local_ip_be,
                    ac.dev_local_mac);

    /* SNMP 采集（S4.3）：**收发必须落在慢任务池**（iv_snmp 是阻塞式）。
     * 目标 IP 复用既有键：摄像机＝`camera.ch.0.ip`（现网"查摄像机1"），
     * 交换机＝`switch.ip`；OID 槽位键名即语义（见 iv_snmp.h 规范表）。 */
    ac.pool = g_pool;
    /* S9 延伸：档案/SNMP/网络历史落 control.db（store 在第 2b 步就绪）。 */
    ac.store = g_store;
    cfg_snmp_targets(ac.snmp);

    /* 断网录像（S6.3）的源表与时间参数（S6.5 前置）：`camera.ch.N.url`/`.record`
     * ＋ `camera.rec.hold_ms`/`.debounce_ms`。未配的路留空、状态机自然跳过。 */
    cfg_rec_sources(&ac);

    /* SD 卡策略（S6.5）：默认启用。根目录**刻意不接配置键**（§18.5 拍板：
     * 容量与水位都是运行时事实/工程常量）—— 留空 ⇒ 环境变量 ⇒ 默认 /mnt/sdcard，
     * 与 media 抓拍目录同源。板端实际挂载点仍是待确认事实（登记在 docs）。 */
    ac.storage_enable = 1;
    ac.storage_root[0] = '\0';

    /* GPS（S2.5）：默认 /dev/ttySAC1、9600 8N1，解析器恒装、**不校时**。
     * 环境变量只为联调/回放留口子（与 IVSBOX_SD_ROOT 同口径，产线不设）。 */
    {
        const char *gdev = getenv("IVSBOX_GPS_DEV");

        if (gdev != NULL)
            ac.gps_path = gdev;
    }

    /* 4G 模组 AT 口：默认 `/dev/ttyUSB0`（出厂镜像固定），**不接配置键** ——
     * 与 GPS 同口径：AT 口是硬件事实，配错的症状是"一条命令都不回"，
     * 与"模块没插"长得一模一样，属现场最难查的一类。环境变量只为联调留口子。 */
    {
        const char *cdev = getenv("IVSBOX_CELL_DEV");

        if (cdev != NULL)
            ac.cell_path = cdev;
    }
    /* 自动重拨用的出厂脚本。空串 = 关闭自动重拨；不配 = 默认
     * `/usr/bin/usb-EC200G-inserted.sh`。与上面同口径：只为联调/换模组留口子，
     * 不进配置键 —— 现场不需要改它，需要改就是脚本路径本身不对。 */
    {
        const char *dcmd = getenv("IVSBOX_DIAL_CMD");

        if (dcmd != NULL)
            ac.cell_dial_cmd = dcmd;
    }

    /* 设备 ID（TID）＝工厂身份分区里的 `devid`，**不进配置**（架构 §10.2）。
     *
     * 2026-10-19 修：这里原先硬编码 `0`，于是每一条上报报文里的 devid 都是 0，
     * 平台按设备 ID 归档时自然对不上这台机器 —— 现场看到的现象就是"链路明明
     * ESTABLISHED，平台侧却一直是未接入"。`iv_prov_private_read()` 与
     * `sys.overview.device.sn` 读的是同一条记录（`iv_prov_tid_from_u32(devid)`
     * 就是页面上那个 SN），两处必须同源。
     * 读不到就给 0：平台会当作"未登记设备"，这比编一个看起来合法的假 ID 更容易查。 */
    ac.devid = 0u;
    {
        iv_prov_id_t pid;

        if (iv_prov_private_read(NULL, &pid) == IV_OK)
            ac.devid = pid.devid;
        else
            IV_LOG_W(APP_MOD, "no factory identity; reporting devid=0 (platform cannot match)");
    }

    /* 硬件型号（E3 应答 `mod`）：配置键 `sys.device.model`，Web 配置页可改
     * （用户 2026-10-20 明确要求）。
     * **送的是本文件 static 缓冲的地址，不是配置内部指针** —— 原因见
     * `model_refresh()` 上方注释（存指针会在配置 reload 后变空串）。
     * 缓冲内容由 `model_refresh()` 在启动时与每次 SYS 类配置提交后原地改写，
     * 所以网页改完下一帧 E3 应答就是新值，不用重启。 */
    model_refresh();
    ac.mod = g_mod;

    /* 平台 0xE1（查询设备当前参数设置）：data 段由本层拼（配置键、4G 快照、
     * eth0 实取地址都在这里），report 层只管 JSON 外壳与发送。 */
    ac.on_query_cfg     = query_cfg_json;
    ac.on_query_cfg_arg = NULL;

    /* 终端序列号（E3 应答 `tsn`）：芯片唯一号，**不派生自 devid** —— 见
     * iv_provision.h 里 iv_prov_tsn() 的说明。读不到就空串（宁缺勿编）。 */
    if (iv_prov_tsn(g_tsn) == 0)
        IV_LOG_W(APP_MOD, "chip serial unavailable; E3 tsn will be empty");
    ac.tsn = g_tsn;

    /* 采集周期（0xE1 轮询间隔）：`sys.sample.period_ms` 的落点。
     * 这个键在 2026-10-21 之前是**死键**（全仓零消费方，节拍被 IV_REPORT_POLL_MS
     * 写死成 30 s），网页上改成 1 s 也没用 —— 板端实测 30.9 s 一轮。现在配置即真值：
     * 这里取一次做初值，之后每次 SYS 类配置提交由 on_cfg_committed() 热更。 */
    {
        int64_t v = 0;

        if (iv_config_get_int(g_cfg, "sys.sample.period_ms", &v) == IV_OK && v > 0)
            ac.poll_ms = (uint32_t)v;
        /* 现场排查用：日志里一眼能看到「采集周期到底取了多少」。这个键配错/被越界
         * 丢弃时只会静默回落到 30 s，没有这行日志就只能靠数端口帧。 */
        IV_LOG_I(APP_MOD, "sample period %u ms (0xE1 polling)",
                 (unsigned)ac.poll_ms);
    }

    if (iv_agent_open(&g_agent, &ac, g_reactor, iv_clock_monotonic_ms()) != IV_OK) {
        IV_LOG_E(APP_MOD, "agent open failed");
        return -1;
    }

    /* 平台下行 0xF5「配置定时上报间隔」——**建链之后**注册（路由表挂在 rp.pf 上，
     * 而 pf 是 iv_agent_open → iv_report_open 里才初始化的）。查询类的 0xE2/0xE3
     * 在 iv_report_open 内部注册；这条要写配置，只有装配层够得着 g_cfg。 */
    (void)iv_proto_route(&g_agent.rp.pf, IV_PROTO_CMD_CFG_REPORT_INTV,
                         on_plat_report_intv, NULL);
    /* 平台下行 0xF3「配置传输模式」：同样要写配置 ＋ 立刻生效，
     * 只有装配层够得着 g_cfg 与 agent 句柄（2026-10-21 补）。 */
    (void)iv_proto_route(&g_agent.rp.pf, IV_PROTO_CMD_CFG_MODE, on_plat_mode, NULL);
    /* 平台下行 0xF4「配置设备 IP、网关」：本条**会改本机网口地址**，只有装配层
     * 够得着 g_cfg，且 `iv_netcfg_apply()` 属于"改系统"的动作，不该由协议层代劳
     * （2026-10-26 补；此前落默认分支回 0x74，平台点「配置IP和网关」一直失败）。 */
    (void)iv_proto_route(&g_agent.rp.pf, IV_PROTO_CMD_CFG_LOCAL_NET,
                         on_plat_local_net, NULL);
    /* 平台下行 0xF8「主网检测 IP」：8 B → `probe.target1/2` ＋ 立刻热更。
     * 数据我们本来就有（`probe.*` 五键已接好），此前只是没人把它翻译过去
     * （2026-10-26 补；此前落默认分支回 0x74，平台点「配置主网检测IP」一直失败）。 */
    (void)iv_proto_route(&g_agent.rp.pf, IV_PROTO_CMD_CFG_MAIN_NET,
                         on_plat_ping_target, NULL);
    /* 平台下行 0xA5「配置搜索方式」：`camera.detect.mode` 的另一个写入口。
     * 2026-10-26 从 `iv_agent` 层搬到这里 —— 那边没有配置句柄，只能改运行期、
     * 落不了盘（平台点了成功、设备重启回旧值）。规矩同 0xF3/0xF4/0xF5/0xF8：
     * 写配置的下行命令只有装配层能接。 */
    (void)iv_proto_route(&g_agent.rp.pf, IV_PROTO_CMD_CFG_SEARCH,
                         on_plat_search_mode, NULL);
    /* 平台下行 0xFE「有线网络超时时间(ms)」：1 字节毫秒（板端 10-26 15:51 实抓
     * `fe … 01 64 …` ⇒ 界面填 100 就发 100），落 `probe.timeout_ms`。
     * 此前落"未注册 ⇒ 0x74"，平台点「配置」一直失败。 */
    (void)iv_proto_route(&g_agent.rp.pf, IV_PROTO_CMD_CFG_NET_DELAY,
                         on_plat_net_timeout, NULL);
    return 0;
}

/* ---------------------------------------------------------------------------
 * 骨架初始化
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * 非正常退出取证（R-08）
 *
 * 背景：2026-10-22 板端 17:15:42 出现一次**无预兆重启** —— 日志尾部有 763 个
 * NUL 字节、没有任何 shutdown/signal 记录、`/tmp` 被整片清空（那是 tmpfs），
 * 但 `boot_id` 没变。事后两次定向复现（重跑坏包 install、只跑 updater status）
 * 都没重现，**根因未定位**。没定位就不能说"修好了"，能做的只有一件事：
 * 让下一次发生时**当场留下判据**。
 *
 * 做法＝经典的双态标记（不需要额外硬件、掉电也不撒谎）：
 *   - 启动最早期读 `run.state`：内容是 `clean` ⇒ 上次是**自己走完 app_shutdown()
 *     退出的**（SIGTERM / init 停止）；是 `dirty` 或空 ⇒ 上次**没走完正常退出**
 *     —— 掉电、硬复位、内核 panic、`kill -9`、看门狗复位都会落到这一支；
 *   - 读完立刻改写成 `dirty`，正常退出时再改成 `clean`；
 *   - 同时把 `/proc/sys/kernel/random/boot_id` 打进日志：**boot_id 变** ⇒ 真重启；
 *     **boot_id 不变** ⇒ 只是进程被重新拉起（那一夜的现象正是这一种）。
 *
 * 文件放 UDISK（跨掉电保留）；UDSIK 不可写时退到 /tmp —— 那只能判"同一次开机
 * 内的进程重启"，日志里会体现（路径就在日志里）。路径可用 IVSBOX_RUN_STATE
 * 覆盖，测试/工装用。
 * ------------------------------------------------------------------------- */
#define APP_RUN_STATE_UDISK "/mnt/UDISK/ivsbox/run.state"
#define APP_RUN_STATE_TMP "/tmp/ivsbox-run.state"

static const char *run_state_path(void)
{
    const char *p = getenv("IVSBOX_RUN_STATE");

    if (p != NULL && p[0] != '\0')
        return p;
    if (access("/mnt/UDISK/ivsbox", W_OK) == 0)
        return APP_RUN_STATE_UDISK;
    return APP_RUN_STATE_TMP;
}

static void run_state_write(const char *what)
{
    const char *path = run_state_path();
    int         fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        IV_LOG_W(APP_MOD, "run state write failed: %s (%s)", path, strerror(errno));
        return;
    }
    if (write(fd, what, strlen(what)) != (ssize_t)strlen(what))
        IV_LOG_W(APP_MOD, "run state short write: %s", path);
    else
        (void)fsync(fd); /* 掉电判据必须落盘，不能停在页缓存里 */
    (void)close(fd);
}

/* 把 boot_id 打进日志：区分"整机重启"与"进程被重新拉起"的唯一廉价判据。 */
static void boot_id_log(void)
{
    char    buf[80];
    int     fd;
    ssize_t n;

    fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        IV_LOG_W(APP_MOD, "boot_id unavailable (%s)", strerror(errno));
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1u);
    (void)close(fd);
    if (n <= 0) {
        IV_LOG_W(APP_MOD, "boot_id read failed (%s)", strerror(errno));
        return;
    }
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    IV_LOG_I(APP_MOD, "boot_id=%s", buf);
}

/* 启动最早期调用一次（日志起来之后、其余装配之前）。 */
static void run_state_check(void)
{
    const char *path = run_state_path();
    char        buf[32];
    int         fd;
    ssize_t     n;

    boot_id_log();

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        IV_LOG_I(APP_MOD, "run state: no previous marker (%s) -- first run or fresh flash",
                 path);
        run_state_write("dirty\n");
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1u);
    (void)close(fd);
    if (n < 0)
        n = 0;
    buf[n] = '\0';

    if (strncmp(buf, "clean", 5u) == 0) {
        IV_LOG_I(APP_MOD, "previous shutdown was clean (%s)", path);
    } else {
        /* 这一行是给"下一次无预兆重启"用的：它证明上一个进程没走完退出路径。 */
        IV_LOG_W(APP_MOD,
                 "unclean shutdown detected: prev state='%s' (%s) -- power loss / "
                 "hard reset / kill -9 / watchdog reset 都会落到这里", buf, path);
    }
    run_state_write("dirty\n");
}

static int app_init(void)
{
    int cfg_wfd;
    int tp_efd;

    if (iv_log_init("ivsboxd") != 0)
        return -1;

    /* 启动第一行就把"这版可不可追溯"说清楚（D-11）：
     * `git=nogit valid=0` 表示这次构建没注入哈希 —— 不是出错，但现场排查时
     * 一定要知道，否则"版本串与实际一致"永远对不上账。有注入时是
     * `git=<sha> valid=1`。两个值都直接来自编译期常量，读一次零成本。 */
    IV_LOG_I(APP_MOD, "ivsboxd starting, version %s (git=%s valid=%d)",
            iv_version_string(), iv_version_git(), iv_version_git_valid());
    run_state_check();

    /* 1. 配置 */
    g_cfg = iv_config_open(NULL, "ivsbox");
    if (g_cfg == NULL) {
        IV_LOG_E(APP_MOD, "config open failed");
        return -1;
    }

    /* 1b. 把配置里存的静态地址（`net.ip`/`net.mask`/`net.gateway`）应用到网口。
     *     放在这里而不是后面，有两个原因：
     *     ① 越早越好 —— 后面每一步（DB、taskpool、reactor、agent）都可能自己
     *        用网络；地址晚一步就位，早起的那些连接全走旧地址，日志上看起来
     *        像"偶尔连不上"，其实是自己把网口换掉了。
     *     ② 此刻还是单线程（taskpool/reactor 都没建），`iv_netcfg_apply()` 里
     *        的 fork 是教科书意义上安全的；等到多线程再 fork，子进程只允许调
     *        async-signal-safe 函数，而 execv 之前的每一步都得反复证明。
     *     出厂默认 `net.ip` 是空串 ⇒ 直接返回，不动现场由 rootfs/DHCP 配出来的
     *     地址（这条是安全底线，见函数注释）。 */
    apply_saved_local_net();

    /* 2. SQLite */
    if (iv_db_open(NULL, &g_db) != IV_OK) {
        IV_LOG_E(APP_MOD, "db open failed");
        return -1;
    }

    /* 2b. control.db 表结构（S9 延伸）。fail-open：建表失败（磁盘满/只读）只
     * 告警，档案/审计/网络历史降级为"只写日志"，主控照常跑 —— 这与上面
     * "db 打不开即退出"不同：那是主数据，这是辅助数据。 */
    if (iv_store_open(g_db, &g_store) != IV_OK) {
        IV_LOG_W(APP_MOD, "control.db schema ensure failed -- persist off");
        g_store = NULL;
    }

    /* 3. 慢任务池（必须在 reactor 创建前，worker 继承信号掩码） */
    g_pool = iv_taskpool_create(0, 0);
    if (g_pool == NULL) {
        IV_LOG_E(APP_MOD, "taskpool create failed");
        return -1;
    }

    /* 4. Reactor（阻塞 SIGTERM/SIGINT/SIGPIPE 并建 signalfd）
     *    顺序由计划 §S10 钉死：taskpool 先创建，reactor 随后。iv_taskpool_create()
     *    已把上述信号阻塞且不恢复，worker 继承该掩码；reactor_create() 在调用线程
     *    再阻塞一次，确保 signalfd 能收到进程级信号。 */
    g_reactor = iv_reactor_create(0);
    if (g_reactor == NULL) {
        IV_LOG_E(APP_MOD, "reactor create failed");
        return -1;
    }

    /* 5. 本地通道服务 ＋ 方法路由上下文（M5-S5.1）
     * 客户端槽必须显式置 -1：静态数组默认全 0，而 0 是合法 fd（stdin），
     * 不置 -1 会让"空闲槽"判定把 stdin 当成已连接客户端。 */
    {
        int i;

        for (i = 0; i < CHAN_MAX_CLIENTS; i++) {
            g_cli[i].fd = -1;
            g_cli[i].ev = NULL;
        }
    }
    memset(&g_ctl_ctx, 0, sizeof(g_ctl_ctx));
    g_ctl_ctx.cfg         = g_cfg;
    g_ctl_ctx.db          = g_db;
    g_ctl_ctx.store       = g_store;
    g_ctl_ctx.do_reboot   = ctl_do_reboot;
    g_ctl_ctx.do_settime  = ctl_do_settime;
    /* 恢复出厂（2026-10-22）：擦除目标的两个根。
     *   数据根＝`/mnt/UDISK/ivsbox`（日志在它下面的 `log/`）；
     *   媒体根走 `iv_storage_root_resolve("")`，与 SD 卡策略、抓拍目录**同源**
     *   （`IVSBOX_SD_ROOT` 覆盖对它同样有效——单测/回放才设，产线不设）。
     * 两个串都存进 static 缓冲：ctx 只借指针，函数返回后局部量就没了。 */
    {
        static char s_fac_data_root[96];
        static char s_fac_media_root[96];

        snprintf(s_fac_data_root, sizeof(s_fac_data_root), "%s", IV_UPD_DATA_ROOT_DEFAULT);
        iv_storage_root_resolve("", s_fac_media_root, sizeof(s_fac_media_root));
        g_ctl_ctx.factory_data_root  = s_fac_data_root;
        g_ctl_ctx.factory_media_root = s_fac_media_root;
    }
    /* g_ctl_ctx.agent 在 agent_setup() 之后回填（见第 9 步）。 */

    if (mkdir_for_channel() != 0)
        IV_LOG_W(APP_MOD, "channel parent dir create failed: %s", strerror(errno));

    g_listen_fd = iv_chan_listen(IV_CHAN_PATH_DEFAULT, 0, 0);
    if (g_listen_fd < 0) {
        IV_LOG_E(APP_MOD, "channel listen failed: %d", g_listen_fd);
        /* S10 骨架允许通道服务缺失时继续启动，由板端 init 脚本重建目录后恢复 */
    } else {
        g_listen_ev = iv_reactor_add(g_reactor, g_listen_fd, IV_EV_READ,
                                     on_chan_listen, NULL);
        if (g_listen_ev == NULL) {
            IV_LOG_E(APP_MOD, "channel listen add to reactor failed");
            return -1;
        }
    }

    /* 6. 把 taskpool 完成事件挂进 reactor */
    tp_efd = iv_taskpool_eventfd(g_pool);
    if (tp_efd >= 0) {
        if (iv_reactor_add(g_reactor, tp_efd, IV_EV_READ,
                           on_taskpool_event, NULL) == NULL) {
            IV_LOG_E(APP_MOD, "taskpool eventfd add to reactor failed");
            return -1;
        }
    }

    /* 7. 把配置热更新挂进 reactor */
    cfg_wfd = iv_config_watch_fd(g_cfg);
    if (cfg_wfd >= 0) {
        if (iv_reactor_add(g_reactor, cfg_wfd, IV_EV_READ,
                           on_config_event, NULL) == NULL) {
            IV_LOG_E(APP_MOD, "config watch add to reactor failed");
            return -1;
        }
    }

    /* 8. 看门狗 */
    if (watchdog_setup() != 0)
        return -1;

    /* 9. M3 业务装配：网络侧（netlink/probe/netmgr）+ 控制板上报链（report）
     *    必须在 reactor 创建之后（其定时器/fd 都要挂到 reactor）。 */
    if (agent_setup() != 0)
        return -1;

    /* 9b. agent 就绪后再回填路由上下文的 agent 指针：
     * iv_ctl 的 sys.overview / device.list / snmp.* / camera.search 都读它，
     * 而第 5 步建通道时 agent 还没装配。M6：媒体方法经 ops 表绑到 agent 内嵌
     * 的 iv_media 实例（agent_open 已起桥接线程；失败时 inited=0，方法按
     * IV_ENOTSUP/IV_ECONN 应答，不致命）。 */
    g_ctl_ctx.agent     = &g_agent;
    g_ctl_ctx.media     = &s_media_ops;
    g_ctl_ctx.media_arg = &g_agent;

    /* 9c. 平台地址热重载（2026-10-19）：Web 改 `net.server.*` 后不必重启主控。
     * 走配置层的 F7 事务通知 —— `config.apply`（内存提交）与文件被外部改写
     * （inotify 重载）两条路径都会回调；只挂 inotify 会漏掉前者（提交时内存
     * 已是新值，重载比对等于"没变化"，压根不会触发）。
     * 先 force=0 对齐一次缓存，避免刚装配完就白重连一次。
     *
     * ⚠ `probe` 分类必须一起订上：`on_cfg_committed()` 里的分支写好了、但掩码
     * 没订等于**回调永远不触发**，编译、单测全绿、运行期毫无反应（"接线在装配层"
     * 这一类坑，单测抓不到，只有真拉进程的集成用例才看得见）。`elec`/`sensor`
     * （阈值下发）同理，一处都不能漏；`camera`（检测方式热生效）是同一条坑的第
     * 三次 —— 见 `on_cfg_committed()` 里 CAMERA 分支；`netthr`（切换门限＋
     * 自愈限次）是第四次。**第五次就在这一轮，而且是集成用例 H5 抓出来的**：
     * `switch.snmp.*` 的热更分支写好了、掩码里却没有 SWITCH ⇒ 改 community 一点
     * 反应都没有，而"摄像机那半边顺手把两个目标一起送"会让它拖到下一次 camera
     * 提交才生效（日志上正好隔一整步，看着像慢半拍、其实是漏订阅）。 */
    (void)agent_sync_server_cfg(0);
    if (iv_config_subscribe(g_cfg,
                            IV_CFG_CLASS_MASK(IV_CFG_CLS_NET) |
                                IV_CFG_CLASS_MASK(IV_CFG_CLS_SYS) |
                                IV_CFG_CLASS_MASK(IV_CFG_CLS_PROBE) |
                                IV_CFG_CLASS_MASK(IV_CFG_CLS_ELEC) |
                                IV_CFG_CLASS_MASK(IV_CFG_CLS_SENSOR) |
                                IV_CFG_CLASS_MASK(IV_CFG_CLS_CAMERA) |
                                IV_CFG_CLASS_MASK(IV_CFG_CLS_SWITCH) |
                                IV_CFG_CLASS_MASK(IV_CFG_CLS_NETTHR),
                            on_cfg_committed, NULL) != IV_OK)
        IV_LOG_W(APP_MOD,
                 "subscribe(net+sys+probe+elec+sensor+camera+switch+netthr) failed -- "
                 "server addr / report interval / probe params / thresholds / camera "
                 "detect mode / snmp targets / net thresholds change needs a restart");

    /* 9d. M8-S8.3 受控远程通道装配（身份/令牌/服务端/落点/巡检定时器）。
     *    在 9b 之后：与 agent 装配同级，都在 reactor 建好之后。 */
    tunnel_setup();

    /* 10. 健康线程（必须在 reactor 创建之后、run 之前） */
    g_health = iv_health_start_default(g_reactor, g_pool, g_wd_fd);
    if (g_health == NULL) {
        IV_LOG_E(APP_MOD, "health start failed");
        return -1;
    }

    IV_LOG_I(APP_MOD, "ivsboxd initialized, entering reactor");
    return 0;
}

/* ---------------------------------------------------------------------------
 * 就绪标记（M7-S7.3：升级自检的判据）
 * ------------------------------------------------------------------------- */
/* 装配全部走完之后落一个标记，启动包装脚本与 monitor 据此判定"这次启动真的起来了"：
 * 有标记 ⇒ boot-ok（pending → confirmed）；超过自检窗口仍没标记 ⇒ boot-fail（回滚）。
 *
 * 口径（写清楚，不夸大）：判据覆盖 **ivsboxd 自身**的装配 —— 日志 / 配置 / SQLite /
 * 慢任务池 / reactor / 本地通道 / 看门狗 / M3 业务装配 / 健康线程，即 app_init()
 * 走到 `return 0` 的那一刻。**不含**独立的 ivsbox-media、ivsbox-web 进程（它们由各自
 * 的 init 脚本守护，生命周期与主控无关），也不代表平台侧链路真的通 —— 那些属于
 * §16.2 的联调场景，不该由一个开机标记冒充。
 *
 * 路径可用环境变量 IVSBOX_READY_MARK 覆盖（验证时指到临时目录）。写失败**不算致命**：
 * 不写标记的后果是升级自检超时后回滚，那比"带病确认"安全。 */
static void ready_mark_write(void)
{
    const char *path = getenv("IVSBOX_READY_MARK");
    int         fd;

    if (path == NULL || path[0] == '\0')
        path = IV_UPD_READY_MARK_DEFAULT;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        IV_LOG_W(APP_MOD, "ready mark create failed: %s (%s)", path, strerror(errno));
        return;
    }
    if (write(fd, "1\n", 2u) != 2) {
        IV_LOG_W(APP_MOD, "ready mark write failed: %s (%s)", path, strerror(errno));
    } else {
        (void)fsync(fd);
        IV_LOG_I(APP_MOD, "ready mark written: %s", path);
    }
    (void)close(fd);
}

/* ---------------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("ivsboxd %s\n", iv_version_string());
            return 0;
        }
    }

    if (app_init() != 0) {
        app_shutdown();
        fprintf(stderr, "ivsboxd init failed\n");
        return 1;
    }

    /* 装配完成 ⇒ 落就绪标记（M7-S7.3 升级自检的判据，见函数注释） */
    ready_mark_write();

    (void)iv_reactor_run(g_reactor);

    app_shutdown();
    /* 走到这里＝**收到 SIGTERM/SIGINT 并走完了退出路径**，把标记改成 clean；
     * 没走到这里（掉电/硬复位/被 SIGKILL）标记会停在 dirty ⇒ 下次开机日志里
     * 会出现 "unclean shutdown detected"（见 run_state_check()，R-08）。 */
    run_state_write("clean\n");
    return 0;
}
