/*
 * IVSBox 慢任务池实现（计划 M1-S5）
 *
 * 设计要点与取舍（细节理由见 iv_taskpool.h 顶部注释）：
 *
 * 1) 槽位式队列，不用紧凑环形缓冲。
 *    槽位数组 + 每槽独立状态（FREE/QUEUED/RUNNING/DONE/DISPATCHING）。
 *    这样"取消"能在 O(n) 内找到任意在飞任务（n <= 64，可忽略），
 *    而紧凑队列一旦元素被 worker 取走就脱离可见范围，cancel 会找不到
 *    正在执行的任务、只能返回 ENOENT —— 那是错的。
 *
 * 2) 完成的任务**不立即释放槽位**，而是停在 DONE 等 process() 取走。
 *    好处一：结果缓冲就在槽位里，省掉一份结果拷贝；
 *    好处二：如果调用方忘了挂 eventfd / 忘调 process，表现是槽位耗尽后
 *    submit 一律 EBUSY —— 有界失败，不泄漏、不崩溃，问题立刻可见。
 *
 * 3) worker 空闲时用 pthread_cond_timedwait 带 1s 超时，**超时轮同样推进
 *    进度号**。这与 iv_reactor 的 epoll_wait 有限超时是同一条口径：不这样做，
 *    S6 健康线程会把"两个 worker 都闲着"误判成"worker 死锁"并停喂看门狗。
 *
 * 4) cond 绑定 CLOCK_MONOTONIC（pthread_condattr_setclock）。默认是
 *    CLOCK_REALTIME，而 S2.5 的 GPS 对时会 settimeofday，系统时间跳变会让
 *    等待时长凭空变长或变短，空闲轮询因此失去意义。
 *
 * 5) 时间基准直接用 iv_clock_monotonic_ms()（libivhal）。这是本模块放在
 *    libivmodules 而非 libivcore 换来的便利：core 在链接顺序最后，反向引用
 *    hal 会当场链接失败，S4 的 iv_reactor 就只能自己调 libc clock_gettime。
 *
 * 6) 信号掩码在**建线程之前**由本模块自己阻塞（pthread_sigmask），且不恢复。
 *    按计划 §S10 的装配顺序，taskpool 先建、Reactor 后建，而 iv_reactor_create()
 *    也要求"必须在建任何线程之前"阻塞信号 —— 两个要求直接冲突。由本模块
 *    自己负责，无论先后顺序，worker 继承的掩码都是正确的。
 *    （此前这里写的是"架构 §4.2 的装配顺序"，属**错误归属**：§4.2 只画并发模型的
 *    数据流，没有规定任何创建次序。2026-09-29 更正为归到计划 §S10。）
 *
 * 7) 链接期需要 -lpthread：板端 glibc 2.25 的 pthread 符号在独立 libpthread
 *    里，漏加板上直接链接失败；而新 glibc（>=2.34）已把 pthread 并入 libc，
 *     VM 上不加也能过 —— 典型的"VM 绿、板端挂"。
 *
 * 8) 两处跨线程共享量（worker 进度号、任务 cancel 标志）走 __atomic_* +
 *    memory_order_relaxed。它们没有锁提供 happens-before，裸读写会被 TSan
 *    判成数据竞争；说"良性"没用，`make tsan` 照样报。见 iv_taskpool.h 的
 *    「原子访问纪律」一节。
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_taskpool.h"

#define TASKPOOL_MOD "taskpool"

/* ---------------------------------------------------------------------------
 * 数据结构
 * ------------------------------------------------------------------------- */

enum slot_state {
    SLOT_FREE = 0,   /* 可复用 */
    SLOT_QUEUED,     /* 已提交，等 worker 取 */
    SLOT_RUNNING,    /* worker 正在执行（槽位刻意保留，cancel 要能找到它） */
    SLOT_DONE,       /* 执行完毕，等 process 取走结果 */
    SLOT_DISPATCHING /* 正在被 process 调用 on_done（防递归 process 重复回调） */
};

struct iv_task_slot {
    int              state;
    uint64_t         id;
    iv_task_fn       fn;
    void            *arg;
    uint64_t         deadline_ms;              /* 绝对到期时刻；0 = 不限 */
    iv_task_done_fn  on_done;
    void            *on_done_arg;
    char             name[IV_TASKPOOL_NAME_MAX];
    int              rc;
    size_t           result_len;
    volatile int     cancel;                   /* 协作式取消标志：仅通过 __atomic_* 访问 */
    uint8_t          result[IV_TASKPOOL_RESULT_MAX];
};

struct iv_worker {
    pthread_t         tid;
    iv_taskpool_t    *pool;
    int               idx;
    volatile uint32_t progress;                /* 每完成一个任务或每次空闲超时轮 +1；
                                                * 仅通过 __atomic_* 访问 */
};

struct iv_taskpool {
    pthread_mutex_t      lock;
    pthread_cond_t       cond;                 /* 绑 CLOCK_MONOTONIC */
    struct iv_task_slot *slots;
    int                  queue_size;           /* 槽位总数 == 在飞任务上限 */
    int                  occupied;             /* QUEUED + RUNNING + DONE（锁内维护） */
    struct iv_worker    *workers;
    int                  nworkers;
    int                  efd;                  /* eventfd：worker 完成通知 */
    int                  stopping;
    uint64_t             next_id;
};

/* ---------------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------------- */

/* 把 ms 毫秒后的绝对时刻写进 ts（CLOCK_MONOTONIC 基准，供 cond_timedwait 用） */
static void mono_deadline(struct timespec *ts, uint32_t ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += (time_t)(ms / 1000u);
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* 调用方需自行持锁 */
static int find_state(const iv_taskpool_t *pool, int state)
{
    int i;

    for (i = 0; i < pool->queue_size; i++) {
        if (pool->slots[i].state == state)
            return i;
    }
    return -1;
}

/* 取任务名做日志；name 可能为空串 */
static const char *slot_name(const struct iv_task_slot *s)
{
    return (s->name[0] != '\0') ? s->name : "-";
}

/* ---------------------------------------------------------------------------
 * worker 主循环
 * ------------------------------------------------------------------------- */

static void *worker_main(void *arg)
{
    struct iv_worker *w = (struct iv_worker *)arg;
    iv_taskpool_t    *pool = w->pool;

    for (;;) {
        int idx;
        struct iv_task_slot *s;
        iv_task_fn fn;
        void *targ;
        uint64_t deadline;
        int rc;
        size_t out_len = 0;
        uint64_t one = 1u;

        pthread_mutex_lock(&pool->lock);

        /* 找活干；没有就带 1s 超时地等，超时轮同样推进进度号 */
        for (;;) {
            if (pool->stopping != 0) {
                idx = -1;
                break;
            }
            idx = find_state(pool, SLOT_QUEUED);
            if (idx >= 0)
                break;

            struct timespec ts;
            mono_deadline(&ts, IV_TASKPOOL_IDLE_POLL_MS);
            if (pthread_cond_timedwait(&pool->cond, &pool->lock, &ts) == ETIMEDOUT) {
                /* 空闲轮也前进：S6 才能区分"空闲"与"死锁" */
                (void)__atomic_add_fetch(&w->progress, 1u, __ATOMIC_RELAXED);
            }
        }

        if (idx < 0) { /* stopping */
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        s = &pool->slots[idx];
        s->state = SLOT_RUNNING;

        /* 快照执行所需字段，执行期间不持锁 */
        fn       = s->fn;
        targ     = s->arg;
        deadline = s->deadline_ms;

        pthread_mutex_unlock(&pool->lock);

        /* 开工准入：任一时间点后提交方都可能置 cancel，这里只做"开工前"判定 */
        if (deadline != 0u && iv_clock_monotonic_ms() > deadline) {
            rc = IV_ETIMEDOUT;
            IV_LOG_W(TASKPOOL_MOD, "task '%s' id=%llu expired before start",
                     slot_name(s), (unsigned long long)s->id);
        } else if (__atomic_load_n(&s->cancel, __ATOMIC_RELAXED) != 0) {
            rc = IV_ECANCELED;
        } else {
            iv_task_ctx_t ctx;

            ctx.arg        = targ;
            ctx.cancel     = &s->cancel;
            ctx.result     = s->result;
            ctx.result_cap = IV_TASKPOOL_RESULT_MAX;
            ctx.result_len = 0;

            rc = fn(&ctx);

            out_len = ctx.result_len;
            if (out_len > IV_TASKPOOL_RESULT_MAX) /* 防御：任务函数越写就截断 */
                out_len = IV_TASKPOOL_RESULT_MAX;
        }

        pthread_mutex_lock(&pool->lock);
        s->rc         = rc;
        s->result_len = out_len;
        s->state      = SLOT_DONE;
        pthread_mutex_unlock(&pool->lock);

        /* 先改状态、后通知：保证 reactor 被唤醒时结果已经就绪 */
        if (write(pool->efd, &one, sizeof one) != (ssize_t)sizeof one)
            IV_LOG_W(TASKPOOL_MOD, "completion notify failed on eventfd");

        (void)__atomic_add_fetch(&w->progress, 1u, __ATOMIC_RELAXED);
    }

    return NULL;
}

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

iv_taskpool_t *iv_taskpool_create(int nworkers, int queue_size)
{
    iv_taskpool_t *pool;
    pthread_condattr_t cattr;
    sigset_t set;
    int i;

    if (nworkers <= 0)
        nworkers = IV_TASKPOOL_WORKERS_DEFAULT;
    if (nworkers > IV_TASKPOOL_WORKERS_MAX)
        nworkers = IV_TASKPOOL_WORKERS_MAX;
    if (queue_size <= 0)
        queue_size = IV_TASKPOOL_QUEUE_DEFAULT;
    if (queue_size > IV_TASKPOOL_QUEUE_MAX)
        queue_size = IV_TASKPOOL_QUEUE_MAX;

    pool = (iv_taskpool_t *)calloc(1u, sizeof(*pool));
    if (pool == NULL) {
        IV_LOG_E(TASKPOOL_MOD, "out of memory");
        return NULL;
    }

    pool->queue_size = queue_size;
    pool->nworkers   = nworkers;
    pool->next_id    = 1u;
    pool->efd        = -1;

    pool->slots = (struct iv_task_slot *)calloc((size_t)queue_size, sizeof(*pool->slots));
    if (pool->slots == NULL) {
        IV_LOG_E(TASKPOOL_MOD, "out of memory for %d slots", queue_size);
        free(pool);
        return NULL;
    }

    pool->workers = (struct iv_worker *)calloc((size_t)nworkers, sizeof(*pool->workers));
    if (pool->workers == NULL) {
        IV_LOG_E(TASKPOOL_MOD, "out of memory for %d workers", nworkers);
        free(pool->slots);
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->lock, NULL);

    /* 绑 CLOCK_MONOTONIC：默认的 CLOCK_REALTIME 会被 GPS 对时跳变打乱空闲轮询 */
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&pool->cond, &cattr);
    pthread_condattr_destroy(&cattr);

    pool->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (pool->efd < 0) {
        IV_LOG_E(TASKPOOL_MOD, "eventfd failed: %s", strerror(errno));
        goto fail;
    }

    /* 必须在建线程之前：worker 创建时继承掩码。不恢复 —— 一旦跑多线程就保持
     * 统一掩码最简单，iv_reactor_create() 之后再做一遍是幂等的。 */
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    for (i = 0; i < nworkers; i++) {
        pool->workers[i].pool     = pool;
        pool->workers[i].idx      = i;
        pool->workers[i].progress = 0u;
        if (pthread_create(&pool->workers[i].tid, NULL, worker_main,
                           &pool->workers[i]) != 0) {
            IV_LOG_E(TASKPOOL_MOD, "pthread_create failed at worker %d", i);
            /* 已建线程先停掉，nworkers 保持在 i 以便统一回收 */
            pool->nworkers = i;
            goto fail;
        }
    }

    IV_LOG_I(TASKPOOL_MOD, "taskpool ready: %d worker(s), queue %d, result max %d",
             nworkers, queue_size, (int)IV_TASKPOOL_RESULT_MAX);
    return pool;

fail:
    pthread_mutex_lock(&pool->lock);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    for (i = 0; i < pool->nworkers; i++)
        pthread_join(pool->workers[i].tid, NULL);

    if (pool->efd >= 0)
        close(pool->efd);
    pthread_cond_destroy(&pool->cond);
    pthread_mutex_destroy(&pool->lock);
    free(pool->workers);
    free(pool->slots);
    free(pool);
    return NULL;
}

void iv_taskpool_destroy(iv_taskpool_t *pool)
{
    int i;

    if (pool == NULL)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->stopping = 1;

    /* 让正在执行的任务也有机会收手：只置标志、不打断（协作式）。
     * 计划 §S5 的「destroy 先置取消标志再 join」要的就是这一步 —— 否则关机
     * 路径撞上一个 30s 的 SNMP 超时任务就得干等 30s，procd 的重启窗口未必
     * 等得起。任务函数若不检查 iv_task_canceled()，仍会跑完，这是协作式的边界。 */
    for (i = 0; i < pool->queue_size; i++) {
        if (pool->slots[i].state == SLOT_RUNNING)
            __atomic_store_n(&pool->slots[i].cancel, 1, __ATOMIC_RELAXED);
    }

    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    for (i = 0; i < pool->nworkers; i++)
        pthread_join(pool->workers[i].tid, NULL);

    /* 丢弃未开工任务与未被取走的结果：回调一律不执行（头文件已约定） */
    pthread_mutex_lock(&pool->lock);
    for (i = 0; i < pool->queue_size; i++)
        pool->slots[i].state = SLOT_FREE;
    pool->occupied = 0;
    pthread_mutex_unlock(&pool->lock);

    close(pool->efd);
    pthread_cond_destroy(&pool->cond);
    pthread_mutex_destroy(&pool->lock);
    free(pool->workers);
    free(pool->slots);
    free(pool);

    IV_LOG_I(TASKPOOL_MOD, "taskpool destroyed");
}

/* ---------------------------------------------------------------------------
 * 提交与取消
 * ------------------------------------------------------------------------- */

int iv_taskpool_submit(iv_taskpool_t *pool, const iv_task_req_t *req, uint64_t *out_id)
{
    int idx;
    uint64_t id;
    struct iv_task_slot *s;

    if (pool == NULL || req == NULL || req->fn == NULL)
        return IV_EINVAL;

    pthread_mutex_lock(&pool->lock);

    idx = find_state(pool, SLOT_FREE);
    if (idx < 0) {
        int in_flight = pool->occupied;

        pthread_mutex_unlock(&pool->lock);
        /* 满队列立即返回，不排队不等待（架构 §4.2） */
        IV_LOG_D(TASKPOOL_MOD, "submit rejected: queue full (%d in flight)", in_flight);
        return IV_EBUSY;
    }

    s = &pool->slots[idx];

    id = pool->next_id++;
    if (pool->next_id == 0u) /* 回绕：跳过无效 id 0 */
        pool->next_id = 1u;

    s->id          = id;
    s->fn          = req->fn;
    s->arg         = req->arg;
    s->deadline_ms = (req->timeout_ms != 0u)
                         ? iv_clock_monotonic_ms() + (uint64_t)req->timeout_ms
                         : 0u;
    s->on_done     = req->on_done;
    s->on_done_arg = req->on_done_arg;
    s->rc          = IV_OK;
    s->result_len  = 0u;
    s->name[0]     = '\0';
    if (req->name != NULL) {
        strncpy(s->name, req->name, sizeof(s->name) - 1u);
        s->name[sizeof(s->name) - 1u] = '\0';
    }
    __atomic_store_n(&s->cancel, 0, __ATOMIC_RELAXED);
    s->state = SLOT_QUEUED;
    pool->occupied++;

    pthread_mutex_unlock(&pool->lock);

    /* 用 broadcast 而非 signal：两个 worker 都空闲时，多次 signal 可能落到同一个
     * 线程上，导致第二个任务要等到空闲超时（1s）才被捡走。worker 只有两个，
     * 惊群成本可忽略。刻意放在解锁之后，避免被唤醒者立刻阻塞在锁上。 */
    pthread_cond_broadcast(&pool->cond);

    if (out_id != NULL)
        *out_id = id;
    return IV_OK;
}

int iv_taskpool_cancel(iv_taskpool_t *pool, uint64_t id)
{
    int i;
    int rc = IV_ENOENT;

    if (pool == NULL || id == 0u)
        return IV_EINVAL;

    pthread_mutex_lock(&pool->lock);

    for (i = 0; i < pool->queue_size; i++) {
        struct iv_task_slot *s = &pool->slots[i];

        if (s->state != SLOT_QUEUED && s->state != SLOT_RUNNING)
            continue;
        if (s->id != id)
            continue;

        if (s->state == SLOT_QUEUED) {
            /* 未开工：直接释放槽位，完成回调不执行 */
            s->state = SLOT_FREE;
            pool->occupied--;
            rc = IV_OK;
        } else {
            /* 已开工：只能请求，任务函数自己决定何时退出；回调仍会执行 */
            __atomic_store_n(&s->cancel, 1, __ATOMIC_RELAXED);
            rc = IV_OK;
        }
        break;
    }

    pthread_mutex_unlock(&pool->lock);
    return rc;
}

/* ---------------------------------------------------------------------------
 * 结果收取
 * ------------------------------------------------------------------------- */

int iv_taskpool_eventfd(const iv_taskpool_t *pool)
{
    return (pool != NULL) ? pool->efd : -1;
}

int iv_taskpool_process(iv_taskpool_t *pool, int max)
{
    int done = 0;

    if (pool == NULL)
        return IV_EINVAL;

    /* 电平触发必须读干，否则 eventfd 一直可读、回调反复上报 */
    {
        uint64_t v;
        while (read(pool->efd, &v, sizeof(v)) == (ssize_t)sizeof(v))
            ;
    }

    for (;;) {
        int idx;
        struct iv_task_slot *s;

        if (max > 0 && done >= max)
            break;

        /* 标 DISPATCHING 再解锁：递归调用 process 时不会再捡到同一条 */
        pthread_mutex_lock(&pool->lock);
        idx = find_state(pool, SLOT_DONE);
        if (idx >= 0)
            pool->slots[idx].state = SLOT_DISPATCHING;
        pthread_mutex_unlock(&pool->lock);

        if (idx < 0)
            break;

        s = &pool->slots[idx];

        /* 锁外回调：on_done 里再调 submit / cancel 都不会死锁 */
        if (s->on_done != NULL)
            s->on_done(s->id, s->rc, s->result, s->result_len, s->on_done_arg);

        pthread_mutex_lock(&pool->lock);
        s->state = SLOT_FREE;
        pool->occupied--;
        pthread_mutex_unlock(&pool->lock);

        done++;
    }

    /* 因 max 提前收手时 eventfd 已经被读干，残留的 DONE 槽就没有唤醒源了 ——
     * 若此后没有新任务完成，它们会一直占着槽位直到 destroy。补写一次，
     * 让下一轮 reactor 必然再来取。max <= 0（不限）不会有残留，不必补。 */
    if (max > 0) {
        int remain;

        pthread_mutex_lock(&pool->lock);
        remain = (find_state(pool, SLOT_DONE) >= 0);
        pthread_mutex_unlock(&pool->lock);

        if (remain) {
            uint64_t one = 1u;
            (void)write(pool->efd, &one, sizeof(one));
        }
    }

    return done;
}

/* ---------------------------------------------------------------------------
 * 只读诊断
 * ------------------------------------------------------------------------- */

uint32_t iv_taskpool_worker_progress(const iv_taskpool_t *pool, int idx)
{
    if (pool == NULL || idx < 0 || idx >= pool->nworkers)
        return 0u;
    return __atomic_load_n(&pool->workers[idx].progress, __ATOMIC_RELAXED);
}

int iv_taskpool_worker_count(const iv_taskpool_t *pool)
{
    return (pool != NULL) ? pool->nworkers : 0;
}

int iv_taskpool_pending(iv_taskpool_t *pool)
{
    int n;

    if (pool == NULL)
        return 0;

    pthread_mutex_lock(&pool->lock);
    n = pool->occupied;
    pthread_mutex_unlock(&pool->lock);
    return n;
}
