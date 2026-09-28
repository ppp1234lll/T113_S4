/*
 * 健康线程 + 看门狗门控（libivmodules）。
 *
 * 设计口径（判据、落点、线程与信号、日志出口）全部写在 iv_health.h 的文件头，
 * 这里只留实现要点：
 *   - 每个观察对象一个 iv_probe_t = 「上次进度号」+「该进度号最近一次变化的时刻」，
 *     两者缺一不可；把进度号本身当时间戳用是错的（量纲不同）。
 *   - 跨线程量 stopping 一律走 __atomic_* + relaxed（与 S5 iv_taskpool 同一纪律，
 *     裸读写会被 ThreadSanitizer 判成数据竞争）；快照走内部互斥锁。
 *   - 故障时"关 fd 即停喂"，不做补救：卡死由整机复位兜底，这是设计不是遗漏。
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_health.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_watchdog.h"

#define HEALTH_TAG "[health]"

/* 一个被观察对象的活跃度探针 */
typedef struct iv_probe {
    uint32_t last;       /* 上次见到的进度号 */
    uint64_t changed_ms; /* 该进度号最近一次发生变化时的单调时刻 */
    int      primed;     /* 是否已播种首个样本 */
} iv_probe_t;

struct iv_health {
    pthread_t            tid;
    const iv_reactor_t  *reactor;
    const iv_taskpool_t *pool;
    int                  wd_fd;      /* < 0 = 不喂 / 已停喂 */
    uint32_t             stuck_ms;
    uint32_t             interval_ms;

    volatile int         stopping;   /* 跨线程，一律 __atomic_* */
    pthread_mutex_t      lock;       /* 保护 snap */
    iv_health_snapshot_t snap;
};

/* ---------------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------------- */

/* 进度号推进性判定。返回 1 = 已超预算（判死），0 = 正常。
 * 首次观察只播种不判死 —— 否则启动瞬间必然误报故障。 */
static int probe_check(iv_probe_t *p, uint32_t cur, uint64_t now_ms, uint32_t stuck_ms)
{
    if (!p->primed) {
        p->primed = 1;
        p->last = cur;
        p->changed_ms = now_ms;
        return 0;
    }

    if (cur != p->last) {
        p->last = cur;
        p->changed_ms = now_ms;
        return 0;
    }

    /* 进度号未变：距最近一次变化是否已超窗口 */
    return (now_ms - p->changed_ms) >= (uint64_t)stuck_ms;
}

/* 实际要观察的 worker 数（夹到探针数组上限） */
static int worker_span(const struct iv_health *h)
{
    int n;

    if (h->pool == NULL)
        return 0;

    n = iv_taskpool_worker_count(h->pool);
    if (n < 0)
        n = 0;
    if (n > IV_HEALTH_WORKERS_MAX)
        n = IV_HEALTH_WORKERS_MAX;
    return n;
}

/* 一轮判据：返回故障码（0 = 健康） */
static int health_check(struct iv_health *h, uint64_t now_ms,
                        iv_probe_t *reactor_probe, iv_probe_t *worker_probes)
{
    int fault = IV_HEALTH_OK;
    int i;
    int n;

    if (h->reactor != NULL) {
        uint32_t cur = iv_reactor_progress(h->reactor);

        if (probe_check(reactor_probe, cur, now_ms, h->stuck_ms))
            fault |= IV_HEALTH_REACTOR_DEAD;
    }

    n = worker_span(h);
    for (i = 0; i < n; i++) {
        uint32_t cur = iv_taskpool_worker_progress(h->pool, i);

        if (probe_check(&worker_probes[i], cur, now_ms, h->stuck_ms))
            fault |= IV_HEALTH_WORKER_DEAD;
    }

    return fault;
}

/* 最小现场：模块名 + 故障位 + 进度号快照。只在"首次进入该故障"时打一次。 */
static void report_fault(struct iv_health *h, int fault)
{
    int i;
    int n;

    fprintf(stderr,
            HEALTH_TAG " fault=%d reactor_dead=%d worker_dead=%d stuck_ms=%u "
                       "reactor_progress=%u workers=[",
            fault,
            (fault & IV_HEALTH_REACTOR_DEAD) ? 1 : 0,
            (fault & IV_HEALTH_WORKER_DEAD) ? 1 : 0,
            (unsigned)h->stuck_ms,
            (unsigned)(h->reactor != NULL ? iv_reactor_progress(h->reactor) : 0u));

    n = worker_span(h);
    for (i = 0; i < n; i++)
        fprintf(stderr, "%u%s", (unsigned)iv_taskpool_worker_progress(h->pool, i),
                (i + 1 < n) ? "," : "");
    fprintf(stderr, "]\n");
}

static void fill_snapshot(struct iv_health *h, int fault)
{
    int i;
    int n;

    pthread_mutex_lock(&h->lock);

    h->snap.reactor_progress =
        (h->reactor != NULL) ? iv_reactor_progress(h->reactor) : 0u;

    n = worker_span(h);
    for (i = 0; i < n; i++)
        h->snap.worker_progress[i] = iv_taskpool_worker_progress(h->pool, i);
    h->snap.worker_count = n;
    h->snap.fault_code = fault;
    h->snap.watchdog_fd = h->wd_fd;
    h->snap.tick_count++;

    pthread_mutex_unlock(&h->lock);
}

/* 健康线程主循环 */
static void *health_main(void *arg)
{
    struct iv_health *h = (struct iv_health *)arg;
    iv_probe_t        reactor_probe;
    iv_probe_t        worker_probes[IV_HEALTH_WORKERS_MAX];
    int               last_fault = IV_HEALTH_OK;

    memset(&reactor_probe, 0, sizeof(reactor_probe));
    memset(worker_probes, 0, sizeof(worker_probes));

    /* 启动即喂一次：进程刚起来时主循环可能还没推进过一轮进度号，不能让它在
     * 首个判据窗口内就撞上看门狗超时。*/
    if (h->wd_fd >= 0 && iv_watchdog_keepalive(h->wd_fd) != 0)
        fprintf(stderr, HEALTH_TAG " initial keepalive failed: %s\n", strerror(errno));

    while (__atomic_load_n(&h->stopping, __ATOMIC_RELAXED) == 0) {
        uint64_t now_ms;
        int      fault;

        /* 睡一个检查周期。信号已被阻塞（装配顺序保证），usleep 不会被打断，
         * 因此 destroy 最多等一个周期。*/
        usleep(h->interval_ms * 1000u);
        now_ms = iv_clock_monotonic_ms();

        fault = health_check(h, now_ms, &reactor_probe, worker_probes);

        if (fault == IV_HEALTH_OK) {
            if (h->wd_fd >= 0 && iv_watchdog_keepalive(h->wd_fd) != 0)
                fprintf(stderr, HEALTH_TAG " keepalive failed: %s\n", strerror(errno));
        } else {
            if (fault != last_fault)
                report_fault(h, fault);

            /* 停喂：关掉 fd（nowayout=0 时内核随之停狗），此后不再写。 */
            if (h->wd_fd >= 0) {
                (void)iv_watchdog_close(h->wd_fd);
                h->wd_fd = -1;
            }
        }

        last_fault = fault;
        fill_snapshot(h, fault);
    }

    return NULL;
}

/* ---------------------------------------------------------------------------
 * 公开 API
 * ------------------------------------------------------------------------- */

iv_health_t *iv_health_start_default(const iv_reactor_t *reactor,
                                     const iv_taskpool_t *pool,
                                     int watchdog_fd)
{
    return iv_health_start(reactor, pool, watchdog_fd, 0u, 0u);
}

iv_health_t *iv_health_start(const iv_reactor_t *reactor,
                             const iv_taskpool_t *pool,
                             int watchdog_fd,
                             uint32_t stuck_ms,
                             uint32_t interval_ms)
{
    struct iv_health *h;

    h = (struct iv_health *)calloc(1u, sizeof(*h));
    if (h == NULL)
        return NULL;

    h->reactor     = reactor;
    h->pool        = pool;
    h->wd_fd       = watchdog_fd;
    h->stuck_ms    = (stuck_ms != 0u) ? stuck_ms : IV_HEALTH_STUCK_MS_DEFAULT;
    h->interval_ms = (interval_ms != 0u) ? interval_ms : IV_HEALTH_INTERVAL_MS_DEFAULT;
    h->snap.watchdog_fd = watchdog_fd;

    if (pthread_mutex_init(&h->lock, NULL) != 0) {
        free(h);
        return NULL;
    }

    if (pthread_create(&h->tid, NULL, health_main, h) != 0) {
        pthread_mutex_destroy(&h->lock);
        free(h);
        return NULL;
    }

    return h;
}

int iv_health_snapshot(iv_health_t *h, iv_health_snapshot_t *out)
{
    if (h == NULL || out == NULL)
        return IV_EINVAL;

    pthread_mutex_lock(&h->lock);
    *out = h->snap;
    pthread_mutex_unlock(&h->lock);

    return IV_OK;
}

void iv_health_destroy(iv_health_t *h)
{
    if (h == NULL)
        return;

    __atomic_store_n(&h->stopping, 1, __ATOMIC_RELAXED);
    pthread_join(h->tid, NULL);
    pthread_mutex_destroy(&h->lock);
    free(h);
}
