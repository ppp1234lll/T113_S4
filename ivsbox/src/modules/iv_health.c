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
 *   - 喂狗失败不静默（S6-01）：连续 IV_HEALTH_WD_FAIL_MAX 次失败（EINTR 也按失败
 *     计数）即 fail-stop 关 fd、此后不再尝试；失败状态走快照的 wd_fail_streak /
 *     wd_io_fault，fault_code 语义不变（只反映进度判定）。
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

/* 编译期不变量：健康判据的最坏结论时延（stuck_ms + interval_ms）必须严格小于看门狗
 * 超时窗口，否则"判据还没得出故障结论、看门狗就已经咬了"，门控形同虚设。
 * 这里比的是**请求值**：板端 sunxi-wdt 的 max_timeout 恰为 16s，而内核只对越界值
 * 返回 -EINVAL（不 clamp），所以"请求 16s 就被原样接受" ⇒ 请求值 = 实际值。
 * 换板子/换驱动时：先实测该驱动的 max_timeout，再改 IV_WATCHDOG_DEFAULT_TIMEOUT_SEC
 * 或这两个默认值 —— 顺序反了会被下面这个 #error 当场拦下。 */
#if (IV_HEALTH_STUCK_MS_DEFAULT + IV_HEALTH_INTERVAL_MS_DEFAULT) >= \
    (IV_WATCHDOG_DEFAULT_TIMEOUT_SEC * 1000u)
#error "health: stuck_ms + interval_ms must stay strictly below the watchdog timeout"
#endif

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
        uint64_t deadline = iv_taskpool_worker_deadline(h->pool, i);
        uint32_t cur      = iv_taskpool_worker_progress(h->pool, i);

        /* 有界阻塞任务在自己的端到端截止时间内拥有健康租约：DNS/SNMP/ONVIF
         * 这类单次阻塞调用无法主动 heartbeat，但“仍在调用方声明的预算内”不等于
         * worker 死锁。租约每轮刷新探针；越过 deadline 后恢复 stuck_ms 判据。
         * deadline 先读、progress 后读，与 taskpool 的“先 +1、后撤运行槽”顺序配对，
         * 避免任务恰好完成时看到“无租约 + 旧进度”的误判窗口。 */
        if (deadline != 0u && now_ms <= deadline) {
            worker_probes[i].primed = 1;
            worker_probes[i].last = cur;
            worker_probes[i].changed_ms = now_ms;
            continue;
        }

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

static void fill_snapshot(struct iv_health *h, int fault,
                          uint32_t wd_streak, int wd_io_fault)
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
    h->snap.wd_fail_streak = wd_streak;   /* 喂狗失败状态与 fault_code 分开走（S6-01） */
    h->snap.wd_io_fault = wd_io_fault;

    pthread_mutex_unlock(&h->lock);
}

/* 健康线程主循环 */
static void *health_main(void *arg)
{
    struct iv_health *h = (struct iv_health *)arg;
    iv_probe_t        reactor_probe;
    iv_probe_t        worker_probes[IV_HEALTH_WORKERS_MAX];
    int               last_fault = IV_HEALTH_OK;
    uint32_t          wd_streak = 0u;    /* 连续喂狗失败次数，仅本线程访问（S6-01） */
    int               wd_io_fault = 0;

    memset(&reactor_probe, 0, sizeof(reactor_probe));
    memset(worker_probes, 0, sizeof(worker_probes));

    /* 启动即喂一次：进程刚起来时主循环可能还没推进过一轮进度号，不能让它在
     * 首个判据窗口内就撞上看门狗超时。*/
    if (h->wd_fd >= 0 && iv_watchdog_keepalive(h->wd_fd) != 0) {
        wd_streak = 1u; /* 启动首喂失败同样计入连续失败计数 */
        wd_io_fault = 1;
        fprintf(stderr, HEALTH_TAG " initial keepalive failed: %s\n", strerror(errno));
    }

    while (__atomic_load_n(&h->stopping, __ATOMIC_RELAXED) == 0) {
        uint64_t now_ms;
        int      fault;

        /* 睡一个检查周期。信号已被阻塞（装配顺序保证），usleep 不会被打断，
         * 因此 destroy 最多等一个周期。*/
        usleep(h->interval_ms * 1000u);
        now_ms = iv_clock_monotonic_ms();

        fault = health_check(h, now_ms, &reactor_probe, worker_probes);

        if (fault == IV_HEALTH_OK) {
            if (h->wd_fd >= 0) {
                /* 喂狗失败（含 EINTR，最坏多花一个 tick 才进入 fail-stop）按失败
                 * 计数（S6-01）：只打日志不改状态的话，"fd 一直在但写不进去"
                 * 会静默耗完看门狗窗口，快照却还显示一切正常。*/
                if (iv_watchdog_keepalive(h->wd_fd) == 0) {
                    wd_streak = 0u;
                    wd_io_fault = 0;
                } else {
                    wd_streak++;
                    wd_io_fault = 1;
                    fprintf(stderr, HEALTH_TAG " keepalive failed (%u consecutive): %s\n",
                            (unsigned)wd_streak, strerror(errno));

                    if (wd_streak >= IV_HEALTH_WD_FAIL_MAX) {
                        /* fail-stop：关 fd 即停喂。此后 fd < 0，上面的守卫自然
                         * 跳过、不再尝试；wd_io_fault 在快照里保持 1。 */
                        fprintf(stderr, HEALTH_TAG
                                " watchdog keepalive failed %u times, closing watchdog fd\n",
                                (unsigned)wd_streak);
                        (void)iv_watchdog_close(h->wd_fd);
                        h->wd_fd = -1;
                    }
                }
            }
        } else {
            if (fault != last_fault)
                report_fault(h, fault);

            /* 停喂：**故意**只 close，不写 'V'。本工程 keepalive 写的是 '\0'，
             * 而 sunxi-wdt 声明了 WDIOF_MAGICCLOSE —— 内核 release 路径只在
             * "写过 magic 'V'"或"驱动未声明 MAGICCLOSE"时才停狗，所以这里 close
             * 之后狗会继续倒计时、约 timeout 秒后整机复位，正是门控要的结果。
             * **判死路径绝不写 'V'**：写 'V' 是"正常停机"的动作，
             * 见 iv_watchdog_disable() 的纪律说明。 */
            if (h->wd_fd >= 0) {
                (void)iv_watchdog_close(h->wd_fd);
                h->wd_fd = -1;
            }
        }

        last_fault = fault;
        fill_snapshot(h, fault, wd_streak, wd_io_fault);
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

    /* 运行期不变量（与文件顶部的编译期 #error 同一口径，这里兜住**自定义值**）：
     * 真喂狗时，判据最坏结论时延（stuck + interval）必须严格小于看门狗超时，
     * 否则"判据还没判死、狗就先咬了"，门控被静默绕过。fd < 0（不真喂狗的
     * 纯逻辑单测）不受此约束。*/
    if (h->wd_fd >= 0 && (uint64_t)h->stuck_ms + h->interval_ms >=
                             IV_WATCHDOG_DEFAULT_TIMEOUT_SEC * 1000u) {
        free(h);
        return NULL;
    }

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
