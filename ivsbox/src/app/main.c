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
 * ------------------------------------------------------------------------- */
static char     s_srv_host[IV_WAN_N][64];
static uint16_t s_srv_port[IV_WAN_N];

/* 返回 1 = 地址/端口有变化。`force`=0 时只对齐缓存、不重连（启动时用：
 * 那时 agent 刚按同一份配置 open 过，再重连一次纯属多余）。 */
static int agent_sync_server_cfg(int force)
{
    const char *host;
    const char *raw;
    uint16_t    port;
    int         i;
    int         changed = 0;

    for (i = 0; i < IV_WAN_N; i++) {
        raw  = (i == IV_WAN_WIRELESS) ? cfg_str("net.server.wireless.host")
                                      : cfg_str("net.server.wired.host");
        port = (i == IV_WAN_WIRELESS) ? cfg_port("net.server.wireless.port")
                                      : cfg_port("net.server.wired.port");
        host = (raw != NULL) ? raw : "";
        if (strcmp(s_srv_host[i], host) != 0 || s_srv_port[i] != port)
            changed = 1;
        (void)snprintf(s_srv_host[i], sizeof(s_srv_host[i]), "%s", host);
        s_srv_port[i] = port;
        /* host 直接沿用配置内部指针：配置的字符串存在定长缓冲里（`union{char s[]}`），
         * 原地改写、指针一直有效。**端口是值类型，必须重写**（agent 存的是拷贝）。 */
        g_agent.cfg.srv_host[i] = raw;
        g_agent.cfg.srv_port[i] = port;
    }
    if (changed && force != 0) {
        int rc = iv_agent_reload_server(&g_agent);

        IV_LOG_I(APP_MOD, "net.server.* changed -> reload rc=%d", rc);
    }
    return changed;
}

/* 配置事务通知（F7）。这里只接两条"改完必须马上生效"的：
 *   - `net` 分类：平台地址/端口（与出口网卡成对刷新）→ 断开旧连接、连新平台；
 *   - `sys` 分类里的 `sys.report.interval_s`：周期上报间隔 → 换节拍，**不断链**。
 * 其余分类的热更新各有归属（隧道见 on_config_event）。 */
static void on_cfg_committed(iv_config_t *c, const char *name, uint32_t mask, void *user)
{
    (void)c;
    (void)name;
    (void)user;
    if ((mask & IV_CFG_CLASS_MASK(IV_CFG_CLS_NET)) != 0u)
        (void)agent_sync_server_cfg(1);
    if ((mask & IV_CFG_CLASS_MASK(IV_CFG_CLS_SYS)) != 0u) {
        /* `sys.report.interval_s` 原先只在 app_init 里读一次，网页上改了等于没改
         * （必须重启进程）—— 现场"设了 5 分钟、平台上还是十分钟一条"正是这个。 */
        int64_t v = 0;

        if (iv_config_get_int(g_cfg, "sys.report.interval_s", &v) == IV_OK && v > 0)
            (void)iv_report_set_report_period(&g_agent.rp, (uint32_t)v * 1000u);
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
static void cfg_rec_sources(iv_agent_cfg_t *ac)
{
    char    key[IV_CFG_KEY_MAX];
    int64_t v = 0;
    int     i;

    ac->rec_n_url = 0;
    for (i = 0; i < IV_RECCTL_CH_N; i++) {
        const char *url = NULL;
        int         rec = 0;

        ac->rec_url[i][0] = '\0';
        if (snprintf(key, sizeof(key), "camera.ch.%d.url", i) >= (int)sizeof(key))
            continue;
        if (iv_config_get_str(g_cfg, key, &url) != IV_OK || url == NULL || url[0] == '\0')
            continue; /* 未配地址 ⇒ 该路不录 */
        if (snprintf(key, sizeof(key), "camera.ch.%d.record", i) >= (int)sizeof(key))
            continue;
        if (iv_config_get_bool(g_cfg, key, &rec) != IV_OK || rec == 0)
            continue; /* 录像开关未开 */
        (void)snprintf(ac->rec_url[i], sizeof(ac->rec_url[i]), "%s", url);
        ac->rec_n_url = i + 1;
    }

    if (iv_config_get_int(g_cfg, "camera.rec.hold_ms", &v) == IV_OK && v > 0)
        ac->rec_hold_ms = (uint32_t)v;
    if (iv_config_get_int(g_cfg, "camera.rec.debounce_ms", &v) == IV_OK && v > 0)
        ac->rec_debounce_ms = (uint32_t)v;
}

static int agent_setup(void)
{
    iv_agent_cfg_t ac;
    int64_t        v;
    int            i;

    iv_agent_cfg_default(&ac);

    /* 平台地址：**按活动出口取**（下标＝iv_wan_t）—— 两条分开配置，活动出口由
     * sys.transport.mode ＋ netmgr 状态机决定；本口没配则回退另一口（iv_agent 内）。
     * 两条都为空 ⇒ 不启平台链路（iv_report 内部 idle）。 */
    ac.srv_host[IV_WAN_WIRELESS] = cfg_str("net.server.wireless.host");
    ac.srv_port[IV_WAN_WIRELESS] = cfg_port("net.server.wireless.port");
    ac.srv_host[IV_WAN_WIRED]    = cfg_str("net.server.wired.host");
    ac.srv_port[IV_WAN_WIRED]    = cfg_port("net.server.wired.port");

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
    if (iv_config_get_int(g_cfg, "probe.max_fail", &v) == IV_OK) {
        ac.probe_fail_n = (uint32_t)v;
        ac.nm_fail_n    = (uint32_t)v;
    }

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
    cfg_snmp_target(&ac.snmp[IV_AGENT_SNMP_CAM], "camera.snmp",
                    iv_snmp_cam_slots, (int)IV_SNMP_CAM_SLOT_N,
                    cfg_ip_be("camera.ch.0.ip"));
    cfg_snmp_target(&ac.snmp[IV_AGENT_SNMP_SW], "switch.snmp",
                    iv_snmp_sw_slots, (int)IV_SNMP_SW_SLOT_N,
                    cfg_ip_be("switch.ip"));

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

    if (iv_agent_open(&g_agent, &ac, g_reactor, iv_clock_monotonic_ms()) != IV_OK) {
        IV_LOG_E(APP_MOD, "agent open failed");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 骨架初始化
 * ------------------------------------------------------------------------- */
static int app_init(void)
{
    int cfg_wfd;
    int tp_efd;

    if (iv_log_init("ivsboxd") != 0)
        return -1;

    IV_LOG_I(APP_MOD, "ivsboxd starting, version %s", iv_version_string());

    /* 1. 配置 */
    g_cfg = iv_config_open(NULL, "ivsbox");
    if (g_cfg == NULL) {
        IV_LOG_E(APP_MOD, "config open failed");
        return -1;
    }

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
     * 先 force=0 对齐一次缓存，避免刚装配完就白重连一次。 */
    (void)agent_sync_server_cfg(0);
    if (iv_config_subscribe(g_cfg,
                            IV_CFG_CLASS_MASK(IV_CFG_CLS_NET) |
                                IV_CFG_CLASS_MASK(IV_CFG_CLS_SYS),
                            on_cfg_committed, NULL) != IV_OK)
        IV_LOG_W(APP_MOD,
                 "subscribe(net+sys) failed -- server addr / report interval "
                 "change needs a restart");

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
    return 0;
}
