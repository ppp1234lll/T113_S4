/*
 * ivsboxd 主控进程入口（M1-S10 骨架装配）
 *
 * 启动顺序（已钉死）：
 *   日志 → 配置 → SQLite → 慢任务池 → Reactor → 本地通道服务 → 健康线程
 * 然后 iv_reactor_run() 进入主循环，直到收到 SIGTERM/SIGINT 后干净退出。
 *
 * 本文件只负责装配与生命周期，不含业务逻辑；业务逻辑后续按模块拆分。
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/ivsbox.h"
#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_config.h"
#include "ivsbox/iv_db.h"
#include "ivsbox/iv_health.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_taskpool.h"
#include "ivsbox/iv_version.h"
#include "ivsbox/iv_watchdog.h"

#define APP_MOD "app"
#define CHAN_SNAPSHOT "system.snapshot"

/* 运行期全局句柄 */
static iv_taskpool_t *g_pool;
static iv_reactor_t  *g_reactor;
static iv_health_t   *g_health;
static iv_config_t   *g_cfg;
static iv_db_t       *g_db;
static int            g_wd_fd = -1;

/* 本地通道 */
static int       g_listen_fd = -1;
static iv_event_t *g_listen_ev;
static int       g_client_fd = -1;
static iv_event_t *g_client_ev;

static char      g_rx_buf[IV_CHAN_RECV_CAP_MIN];

/* ---------------------------------------------------------------------------
 * 干净退出
 * ------------------------------------------------------------------------- */
static void app_shutdown(void)
{
    IV_LOG_I(APP_MOD, "shutting down ivsboxd");

    if (g_client_fd >= 0) {
        (void)close(g_client_fd);
        g_client_fd = -1;
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
 * 本地通道：对 system.snapshot 返回一个最小快照
 * ------------------------------------------------------------------------- */
static int snapshot_reply(int fd, const ivs_chan_hdr_t *req)
{
    ivs_chan_hdr_t hdr;
    char           body[128];
    int            n;

    n = snprintf(body, sizeof(body),
                 "{\"method\":\"system.snapshot\",\"version\":\"%s\",\"status\":\"ok\"}",
                 iv_version_string());
    if (n < 0 || n >= (int)sizeof(body))
        return -1;

    if (iv_chan_hdr_init(&hdr, IV_CHAN_TYPE_RSP, req->request_id, 0u, (uint32_t)n) != IV_OK)
        return -1;

    return iv_chan_send(fd, &hdr, body, (size_t)n);
}

static void on_chan_client(int fd, uint32_t events, void *arg)
{
    ivs_chan_hdr_t hdr;
    size_t         payload_len;
    int            rc;

    (void)arg;

    if (events & (IV_EV_ERR | IV_EV_HUP | IV_EV_RDHUP)) {
        rc = IV_ECONN;
    } else {
        rc = iv_chan_recv(fd, g_rx_buf, sizeof(g_rx_buf), &hdr, &payload_len);
    }

    if (rc == IV_EAGAIN)
        return;

    if (rc != IV_OK) {
        IV_LOG_I(APP_MOD, "channel client closed/error (fd=%d, rc=%d)", fd, rc);
        iv_reactor_del(g_reactor, g_client_ev);
        g_client_ev = NULL;
        (void)close(fd);
        g_client_fd = -1;
        return;
    }

    if (hdr.type == IV_CHAN_TYPE_REQ && payload_len > 0) {
        const char *payload = (const char *)g_rx_buf + IV_CHAN_HDR_SIZE;
        if (payload_len < sizeof(CHAN_SNAPSHOT) - 1u ||
            memcmp(payload, CHAN_SNAPSHOT, sizeof(CHAN_SNAPSHOT) - 1u) != 0) {
            /* 非 snapshot 请求：静默忽略（S10 只验证 snapshot 通路） */
        } else {
            (void)snapshot_reply(fd, &hdr);
        }
    }
}

static void on_chan_listen(int fd, uint32_t events, void *arg)
{
    iv_chan_peer_t peer;
    int            cfd;

    (void)events;
    (void)arg;

    cfd = iv_chan_accept(fd, NULL, &peer);
    if (cfd == IV_EAGAIN)
        return;
    if (cfd < 0) {
        IV_LOG_E(APP_MOD, "channel accept failed: %d", cfd);
        return;
    }

    if (g_client_fd >= 0) {
        /* S10 骨架只支持一个并发客户端，避免状态复杂 */
        IV_LOG_W(APP_MOD, "channel client already connected, dropping new one");
        (void)close(cfd);
        return;
    }

    g_client_fd = cfd;
    g_client_ev = iv_reactor_add(g_reactor, cfd, IV_EV_READ, on_chan_client, NULL);
    if (g_client_ev == NULL) {
        IV_LOG_E(APP_MOD, "channel client add to reactor failed");
        (void)close(cfd);
        g_client_fd = -1;
    } else {
        IV_LOG_I(APP_MOD, "channel client connected (fd=%d, uid=%d)", cfd, (int)peer.uid);
    }
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

    /* 5. 本地通道服务 */
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

    /* 9. 健康线程（必须在 reactor 创建之后、run 之前） */
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
