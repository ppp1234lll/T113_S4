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
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_taskpool.h"
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

    ac.devid = 0u;

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

    (void)iv_reactor_run(g_reactor);

    app_shutdown();
    return 0;
}
