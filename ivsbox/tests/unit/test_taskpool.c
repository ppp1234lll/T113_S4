/*
 * iv_taskpool 单测（开发计划 M1-S5）
 *
 * 覆盖：
 *   1) 入参校验、create 参数夹取、NULL 全路径不崩；
 *   2) 正常完成：结果经槽位回传、完成回调次数与内容正确；
 *   3) 队列满：在飞数达上限时 submit 立即返回 IV_EBUSY（不排队不等待）；
 *   4) 取消未开工（QUEUED）任务：立即释放槽位、完成回调不执行；
 *   5) 取消已开工（RUNNING）任务：协作式，任务函数自行退出，回调仍执行且 rc 为
 *      IV_ECANCELED；
 *   6) 取消不存在的 id -> IV_ENOENT；
 *   7) 截止时间：任务在队列里放到过期，取出时不再执行，rc 为 IV_ETIMEDOUT；
 *   8) 空闲不误判：无任务时进度号仍持续递增（S6 判活的前提）；
 *   9) process(max) 限流；
 *  10) on_done 里再 submit（回调锁外执行，不得死锁）；
 *  11) destroy 无死锁（队列非空时直接销毁）；
 *  12) fd 不泄漏：create/destroy 循环后 /proc/self/fd 数量不增长；
 *  13) 与 Reactor 联调：慢任务运行期间主循环进度号持续递增（Reactor 未被阻塞）。
 *
 * 说明：
 *   - 本测试不调用 iv_log_init，并显式关闭落盘出口，避免在宿主机 mkdir 默认
 *     /opt/log（沿用 test_basic / test_reactor 的口径）。
 *   - 第 8、13 例是真实等待，不是 sleep 凑数：它们验证的正是"空闲时进度号仍在走"
 *     与"慢任务不阻塞主循环"，零耗时无法验证。
 *   - 跨线程读进度号一律用 iv_taskpool_worker_progress()（内部 relaxed 原子读），
 *     任务里检查取消一律用 iv_task_canceled()，这样 `make tsan` 才干净。
 */
#include <dirent.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_taskpool.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* 主线程轮询收取完成结果，直到计数达标或超时 */
static int drain_until(iv_taskpool_t *pool, int *counter, int target, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        (void)iv_taskpool_process(pool, 0);
        if (*counter >= target)
            return 0;
        sleep_ms(5);
        waited += 5;
    }
    (void)iv_taskpool_process(pool, 0);
    return (*counter >= target) ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * 任务函数
 * ------------------------------------------------------------------------- */

/* 下面两个标志由 worker 线程写、主线程轮询读，**没有锁提供 happens-before**，
 * 所以声明为 _Atomic 而不是 int —— 否则 TSan 会如实报成数据竞争，而且它会
 * 明确指出"你不是在同步，你只是在 sleep"（As if synchronized via sleep）。
 * 用原子类型后，`a->ran = 1;` / `a->ran == 0` 的写法不用改，语义已是原子访问。 */
struct slow_arg {
    int        ms;
    _Atomic int ran;      /* 任务函数是否真的执行过 */
    _Atomic int canceled; /* 是否观察到取消标志 */
};

static int fn_mark(iv_task_ctx_t *ctx)
{
    struct slow_arg *a = (struct slow_arg *)ctx->arg;

    a->ran = 1;
    return IV_OK;
}

static int fn_sleep(iv_task_ctx_t *ctx)
{
    struct slow_arg *a = (struct slow_arg *)ctx->arg;

    a->ran = 1;
    sleep_ms(a->ms);
    return IV_OK;
}

/* 可取消的长任务：最多等 2s，周期检查取消标志 */
static int fn_sleep_cancelable(iv_task_ctx_t *ctx)
{
    struct slow_arg *a = (struct slow_arg *)ctx->arg;
    int i;

    a->ran = 1;
    for (i = 0; i < 2000; i++) {
        if (iv_task_canceled(ctx)) {
            a->canceled = 1;
            return IV_ECANCELED;
        }
        sleep_ms(1);
    }
    return IV_OK;
}

/* 把 token 写进结果缓冲 */
struct result_arg {
    int        token;
    _Atomic int ran; /* 同上：worker 写、主线程读 */
};

static int fn_write_result(iv_task_ctx_t *ctx)
{
    struct result_arg *a = (struct result_arg *)ctx->arg;

    a->ran = 1;
    if (ctx->result_cap < sizeof(a->token))
        return IV_ERANGE;
    *(int *)ctx->result = a->token;
    ctx->result_len     = sizeof(a->token);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 完成回调
 * ------------------------------------------------------------------------- */

struct cbstat {
    int      n;         /* 回调次数 */
    int      ok;        /* 结果内容核对通过次数 */
    int      bad_len;   /* 结果长度不符次数 */
    int      bad_val;   /* 结果内容不符次数 */
    int      last_rc;
    uint64_t last_id;
    int      expect_tokens;   /* 非 0 时逐条核对 result 是否等于该 token */
};

static void cb_count(uint64_t id, int rc, const void *result, size_t len, void *arg)
{
    struct cbstat *st = (struct cbstat *)arg;

    st->n++;
    st->last_rc = rc;
    st->last_id = id;

    if (st->expect_tokens != 0) {
        if (len != sizeof(int)) {
            st->bad_len++;
        } else if (*(const int *)result != st->expect_tokens) {
            st->bad_val++;
        } else {
            st->ok++;
        }
    }
}

/* ---------------------------------------------------------------------------
 * 1) 入参校验与 create 参数夹取
 * ------------------------------------------------------------------------- */

static void test_validation(void)
{
    iv_taskpool_t *pool;
    iv_task_req_t  req;

    chk(iv_taskpool_submit(NULL, NULL, NULL) == IV_EINVAL, "submit(NULL,NULL)");
    chk(iv_taskpool_cancel(NULL, 1u) == IV_EINVAL, "cancel(NULL)");
    chk(iv_taskpool_process(NULL, 0) == IV_EINVAL, "process(NULL)");
    chk(iv_taskpool_worker_progress(NULL, 0) == 0u, "worker_progress(NULL)");
    chk(iv_taskpool_worker_count(NULL) == 0, "worker_count(NULL)");
    chk(iv_taskpool_pending(NULL) == 0, "pending(NULL)");
    chk(iv_taskpool_eventfd(NULL) == -1, "eventfd(NULL)");
    iv_taskpool_destroy(NULL); /* 不崩即可 */

    /* 参数 <= 0 取默认：2 worker / 队列 16 */
    pool = iv_taskpool_create(0, 0);
    chk(pool != NULL, "create(0,0) uses defaults");
    if (pool == NULL)
        return;
    chk(iv_taskpool_worker_count(pool) == IV_TASKPOOL_WORKERS_DEFAULT, "default worker count");
    chk(iv_taskpool_eventfd(pool) >= 0, "eventfd valid");
    chk(iv_taskpool_pending(pool) == 0, "fresh pool is empty");
    chk(iv_taskpool_worker_progress(pool, 0) == 0u, "fresh worker progress == 0");
    chk(iv_taskpool_worker_progress(pool, 99) == 0u, "out-of-range worker idx -> 0");

    /* 非法提交 */
    req.name = NULL;
    req.fn = NULL;
    req.arg = NULL;
    req.timeout_ms = 0u;
    req.on_done = NULL;
    req.on_done_arg = NULL;
    chk(iv_taskpool_submit(pool, NULL, NULL) == IV_EINVAL, "submit(req=NULL)");
    chk(iv_taskpool_submit(pool, &req, NULL) == IV_EINVAL, "submit(fn=NULL)");

    chk(iv_taskpool_cancel(pool, 0u) == IV_EINVAL, "cancel(id=0)");

    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 2) 正常完成：结果回传 + 回调次数
 * ------------------------------------------------------------------------- */

static void test_complete_and_result(void)
{
    enum { N = 6 };
    iv_taskpool_t      *pool;
    struct result_arg   args[N];
    struct cbstat       st;
    iv_task_req_t       req;
    uint64_t            id;
    int                 i;

    pool = iv_taskpool_create(2, 8);
    chk(pool != NULL, "create for completion case");
    if (pool == NULL)
        return;

    st.n = 0;
    st.ok = 0;
    st.bad_len = 0;
    st.bad_val = 0;
    st.last_rc = 0;
    st.last_id = 0u;

    for (i = 0; i < N; i++) {
        args[i].token = 1000 + i;
        args[i].ran   = 0;

        req.name        = "write-result";
        req.fn          = fn_write_result;
        req.arg         = &args[i];
        req.timeout_ms  = 0u;
        req.on_done     = cb_count;
        req.on_done_arg = &st;

        st.expect_tokens = 0; /* 结果内容在下面逐条单独核对 */
        chk(iv_taskpool_submit(pool, &req, &id) == IV_OK, "submit ok");
        chk(id != 0u, "task id is non-zero");
    }

    chk(drain_until(pool, &st.n, N, 2000) == 0, "all tasks completed in time");
    chk(st.n == N, "completion callback fired N times");
    for (i = 0; i < N; i++)
        chk(args[i].ran == 1, "task body ran");

    /* 逐个核对结果内容：每次重跑一个任务并单独核对 */
    for (i = 0; i < N; i++) {
        struct cbstat one;

        one.n = 0;
        one.ok = 0;
        one.bad_len = 0;
        one.bad_val = 0;
        one.last_rc = 0;
        one.last_id = 0u;
        one.expect_tokens = 2000 + i;

        args[i].token = 2000 + i;
        args[i].ran   = 0;

        req.name        = "verify-result";
        req.fn          = fn_write_result;
        req.arg         = &args[i];
        req.timeout_ms  = 0u;
        req.on_done     = cb_count;
        req.on_done_arg = &one;

        chk(iv_taskpool_submit(pool, &req, &id) == IV_OK, "resubmit for verify");
        chk(drain_until(pool, &one.n, 1, 1000) == 0, "verify task completed");
        chk(one.bad_len == 0, "result length matches");
        chk(one.bad_val == 0, "result content matches token");
        chk(one.ok == 1, "result verified once");
        chk(one.last_rc == IV_OK, "task rc == IV_OK");
        chk(one.last_id == id, "callback id matches submit id");
    }

    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 3) 队列满 -> IV_EBUSY
 * ------------------------------------------------------------------------- */

static void test_queue_full(void)
{
    enum { CAP = 4 };
    iv_taskpool_t    *pool;
    struct slow_arg   args[CAP + 1];
    iv_task_req_t     req;
    uint64_t          id;
    int               i;
    int               rejected = 0;

    /* 单 worker + 小队列：先占满，再验证第 CAP+1 个被拒 */
    pool = iv_taskpool_create(1, CAP);
    chk(pool != NULL, "create for full-queue case");
    if (pool == NULL)
        return;

    for (i = 0; i <= CAP; i++) {
        args[i].ms  = 200;
        args[i].ran = 0;

        req.name        = "blocker";
        req.fn          = fn_sleep;
        req.arg         = &args[i];
        req.timeout_ms  = 0u;
        req.on_done     = NULL;
        req.on_done_arg = NULL;

        if (iv_taskpool_submit(pool, &req, &id) == IV_EBUSY)
            rejected++;
    }

    chk(rejected == 1, "exactly one submit rejected with IV_EBUSY");
    chk(iv_taskpool_pending(pool) == CAP, "in-flight count saturates at queue size");

    /* 队列非空时直接销毁：不得死锁 */
    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 4) 取消未开工任务 + 5) 取消已开工任务 + 6) id 不存在
 * ------------------------------------------------------------------------- */

static void test_cancel(void)
{
    iv_taskpool_t    *pool;
    struct slow_arg   blocker;
    struct slow_arg   queued;
    struct slow_arg   running;
    struct cbstat     st;
    iv_task_req_t     req;
    uint64_t          id_blocker = 0u;
    uint64_t          id_queued  = 0u;
    uint64_t          id_running = 0u;

    /* 单 worker：排在第一位的会占住 worker，后面的才能稳定停在 QUEUED */
    pool = iv_taskpool_create(1, 8);
    chk(pool != NULL, "create for cancel case");
    if (pool == NULL)
        return;

    st.n = 0;
    st.ok = 0;
    st.bad_len = 0;
    st.bad_val = 0;
    st.last_rc = 0;
    st.last_id = 0u;
    st.expect_tokens = 0;

    blocker.ms = 300;
    blocker.ran = 0;
    blocker.canceled = 0;

    req.name = "blocker";
    req.fn = fn_sleep;
    req.arg = &blocker;
    req.timeout_ms = 0u;
    req.on_done = cb_count;
    req.on_done_arg = &st;
    chk(iv_taskpool_submit(pool, &req, &id_blocker) == IV_OK, "submit blocker");

    sleep_ms(50); /* 让 blocker 真正开工 */

    queued.ms = 10;
    queued.ran = 0;
    queued.canceled = 0;
    req.name = "queued";
    req.fn = fn_sleep;
    req.arg = &queued;
    chk(iv_taskpool_submit(pool, &req, &id_queued) == IV_OK, "submit queued task");

    /* 未开工：立即释放，回调不执行 */
    chk(iv_taskpool_cancel(pool, id_queued) == IV_OK, "cancel queued task");
    chk(queued.ran == 0, "canceled queued task never ran");

    /* 不存在的 id */
    chk(iv_taskpool_cancel(pool, 987654321ull) == IV_ENOENT, "cancel unknown id -> ENOENT");
    /* 重复取消：已释放，找不到 -> ENOENT */
    chk(iv_taskpool_cancel(pool, id_queued) == IV_ENOENT, "cancel twice -> ENOENT");

    /* 已开工：协作式取消，回调仍执行且 rc 为 IV_ECANCELED */
    running.ms = 0;
    running.ran = 0;
    running.canceled = 0;
    req.name = "running";
    req.fn = fn_sleep_cancelable;
    req.arg = &running;
    chk(iv_taskpool_submit(pool, &req, &id_running) == IV_OK, "submit cancelable task");

    /* 等 blocker 收尾，再等 running 真正开工 —— 否则本次取消会落到 QUEUED 分支
     * （那条路径上面刚验过），而这里要验的是 RUNNING 分支 */
    chk(drain_until(pool, &st.n, 1, 1000) == 0, "blocker completed");
    {
        int w;
        for (w = 0; w < 200 && running.ran == 0; w++)
            sleep_ms(5);
    }
    chk(running.ran == 1, "cancelable task started (RUNNING path)");

    chk(iv_taskpool_cancel(pool, id_running) == IV_OK, "cancel running task");

    chk(drain_until(pool, &st.n, 2, 3000) == 0, "running task completed after cancel");
    chk(running.canceled == 1, "running task observed the cancel flag");
    chk(st.n == 2, "exactly 2 callbacks: blocker + canceled-running, queued suppressed");
    chk(st.last_id == id_running, "last callback belongs to the canceled running task");
    chk(st.last_rc == IV_ECANCELED, "canceled running task reports IV_ECANCELED");

    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 7) 截止时间：排队放到过期，取出时不再执行
 * ------------------------------------------------------------------------- */

static void test_deadline_expired(void)
{
    iv_taskpool_t   *pool;
    struct slow_arg  blocker;
    struct slow_arg  late;
    struct cbstat    st;
    iv_task_req_t    req;
    uint64_t         id = 0u;

    pool = iv_taskpool_create(1, 4);
    chk(pool != NULL, "create for deadline case");
    if (pool == NULL)
        return;

    st.n = 0;
    st.ok = 0;
    st.bad_len = 0;
    st.bad_val = 0;
    st.last_rc = 0;
    st.last_id = 0u;
    st.expect_tokens = 0;

    blocker.ms = 300;
    blocker.ran = 0;
    blocker.canceled = 0;
    req.name = "blocker";
    req.fn = fn_sleep;
    req.arg = &blocker;
    req.timeout_ms = 0u;
    req.on_done = cb_count;
    req.on_done_arg = &st;
    chk(iv_taskpool_submit(pool, &req, &id) == IV_OK, "submit blocker");

    /* 1ms 截止：worker 忙着 300ms，取出时必然已过期 */
    late.ms = 0;
    late.ran = 0;
    late.canceled = 0;
    req.name = "late";
    req.fn = fn_mark;
    req.arg = &late;
    req.timeout_ms = 1u;
    chk(iv_taskpool_submit(pool, &req, &id) == IV_OK, "submit late task");

    chk(drain_until(pool, &st.n, 2, 2000) == 0, "both tasks reported");
    chk(late.ran == 0, "expired task body was NOT executed");
    chk(st.last_rc == IV_ETIMEDOUT, "expired task reports IV_ETIMEDOUT");

    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 8) 空闲时进度号仍递增（S6 判活前提）
 * ------------------------------------------------------------------------- */

static void test_idle_progress(void)
{
    iv_taskpool_t *pool;
    uint32_t       before;
    uint32_t       after;

    pool = iv_taskpool_create(2, 4);
    chk(pool != NULL, "create for idle-progress case");
    if (pool == NULL)
        return;

    before = iv_taskpool_worker_progress(pool, 0);
    sleep_ms(1200); /* 跨过一次空闲超时轮 */
    after = iv_taskpool_worker_progress(pool, 0);

    /* 差值语义：不得写 after > before（32 位回绕） */
    chk((uint32_t)(after - before) != 0u, "worker progress advances while idle");
    chk(iv_taskpool_pending(pool) == 0, "idle pool has nothing in flight");

    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 9) process(max) 限流
 * ------------------------------------------------------------------------- */

static void test_process_max(void)
{
    enum { N = 5 };
    iv_taskpool_t    *pool;
    struct slow_arg   args[N];
    struct cbstat     st;
    iv_task_req_t     req;
    uint64_t          id;
    int               i;
    int               got;

    pool = iv_taskpool_create(2, 8);
    chk(pool != NULL, "create for process-max case");
    if (pool == NULL)
        return;

    st.n = 0;
    st.ok = 0;
    st.bad_len = 0;
    st.bad_val = 0;
    st.last_rc = 0;
    st.last_id = 0u;
    st.expect_tokens = 0;

    for (i = 0; i < N; i++) {
        args[i].ms  = 0;
        args[i].ran = 0;
        args[i].canceled = 0;
        req.name = "quick";
        req.fn = fn_mark;
        req.arg = &args[i];
        req.timeout_ms = 0u;
        req.on_done = cb_count;
        req.on_done_arg = &st;
        chk(iv_taskpool_submit(pool, &req, &id) == IV_OK, "submit quick task");
    }

    /* 等全部进入 DONE（只等不取） */
    for (i = 0; i < 200; i++) {
        if (iv_taskpool_pending(pool) == N) {
            /* 再给一点时间让 worker 把状态推到 DONE */
            sleep_ms(50);
            break;
        }
        sleep_ms(5);
    }
    chk(iv_taskpool_pending(pool) == N, "all tasks still held in DONE slots");

    got = iv_taskpool_process(pool, 2);
    chk(got == 2, "process(max=2) returns 2");
    got = iv_taskpool_process(pool, 2);
    chk(got == 2, "process(max=2) returns 2 again");
    got = iv_taskpool_process(pool, 2);
    chk(got == 1, "process(max=2) returns remaining 1");
    chk(st.n == N, "all callbacks eventually fired");
    chk(iv_taskpool_pending(pool) == 0, "slots released after process");

    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 10) on_done 里再 submit（回调锁外执行，不得死锁）
 * ------------------------------------------------------------------------- */

struct chain {
    iv_taskpool_t *pool;
    int            submitted;
    int            n;
};

static void cb_chain(uint64_t id, int rc, const void *result, size_t len, void *arg)
{
    struct chain *c = (struct chain *)arg;
    iv_task_req_t req;
    struct slow_arg *a;
    uint64_t newid;

    (void)id;
    (void)rc;
    (void)result;
    (void)len;

    c->n++;

    if (c->submitted)
        return;
    c->submitted = 1;

    /* 静态变量而不是栈变量：任务可能在回调返回后才执行 */
    static struct slow_arg chain_arg;
    a = &chain_arg;
    a->ms = 0;
    a->ran = 0;
    a->canceled = 0;

    req.name = "chained";
    req.fn = fn_mark;
    req.arg = a;
    req.timeout_ms = 0u;
    req.on_done = cb_chain;
    req.on_done_arg = c;

    chk(iv_taskpool_submit(c->pool, &req, &newid) == IV_OK, "submit from on_done");
}

static void test_resubmit_in_callback(void)
{
    iv_taskpool_t   *pool;
    struct chain     c;
    struct slow_arg  first;
    iv_task_req_t    req;
    uint64_t         id;

    pool = iv_taskpool_create(2, 8);
    chk(pool != NULL, "create for resubmit case");
    if (pool == NULL)
        return;

    c.pool = pool;
    c.submitted = 0;
    c.n = 0;

    first.ms = 0;
    first.ran = 0;
    first.canceled = 0;

    req.name = "first";
    req.fn = fn_mark;
    req.arg = &first;
    req.timeout_ms = 0u;
    req.on_done = cb_chain;
    req.on_done_arg = &c;

    chk(iv_taskpool_submit(pool, &req, &id) == IV_OK, "submit first of chain");
    chk(drain_until(pool, &c.n, 2, 2000) == 0, "chained resubmit completed (no deadlock)");
    chk(c.submitted == 1, "resubmit happened exactly once");

    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 11) destroy 无死锁（队列非空）
 * ------------------------------------------------------------------------- */

static void test_destroy_no_deadlock(void)
{
    iv_taskpool_t   *pool;
    struct slow_arg  args[4];
    iv_task_req_t    req;
    uint64_t         id;
    int              i;

    pool = iv_taskpool_create(2, 8);
    chk(pool != NULL, "create for destroy case");
    if (pool == NULL)
        return;

    for (i = 0; i < 4; i++) {
        args[i].ms = 100;
        args[i].ran = 0;
        args[i].canceled = 0;
        req.name = "pending";
        req.fn = fn_sleep;
        req.arg = &args[i];
        req.timeout_ms = 0u;
        req.on_done = NULL;
        req.on_done_arg = NULL;
        chk(iv_taskpool_submit(pool, &req, &id) == IV_OK, "submit before destroy");
    }

    iv_taskpool_destroy(pool); /* 能返回即通过；未取走的结果被丢弃 */
    chk(1, "destroy returned without deadlock");
}

/* ---------------------------------------------------------------------------
 * 12) fd 不泄漏
 * ------------------------------------------------------------------------- */

static int count_fds(void)
{
    DIR           *d;
    struct dirent *e;
    int            n = 0;

    d = opendir("/proc/self/fd");
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL)
        n++;
    closedir(d);
    return n;
}

static void test_fd_leak(void)
{
    int before;
    int after;
    int i;

    before = count_fds();
    if (before < 0) {
        chk(0, "cannot count fds: /proc/self/fd unavailable");
        return;
    }

    for (i = 0; i < 200; i++) {
        iv_taskpool_t *pool = iv_taskpool_create(2, 4);
        chk(pool != NULL, "create in fd-leak loop");
        if (pool == NULL)
            return;
        iv_taskpool_destroy(pool);
    }

    after = count_fds();
    chk(after == before, "fd count stable across 200 create/destroy cycles");
}

/* ---------------------------------------------------------------------------
 * 13) 与 Reactor 联调：慢任务不阻塞主循环
 * ------------------------------------------------------------------------- */

struct integ {
    iv_reactor_t  *r;
    iv_taskpool_t *pool;
    int            done;
    int            target;
    int            ticks;
    uint32_t       p_first;
    uint32_t       p_last;
};

static void integ_on_done(uint64_t id, int rc, const void *result, size_t len, void *arg)
{
    (void)id;
    (void)rc;
    (void)result;
    (void)len;
    (void)arg;
}

static void integ_eventfd(int fd, uint32_t events, void *arg)
{
    struct integ *it = (struct integ *)arg;

    (void)fd;
    (void)events;

    it->done += iv_taskpool_process(it->pool, 0);
    if (it->done >= it->target)
        iv_reactor_stop(it->r);
}

static void integ_tick(void *arg)
{
    struct integ *it = (struct integ *)arg;

    it->ticks++;
    if (it->ticks == 1)
        it->p_first = iv_reactor_progress(it->r);
    it->p_last = iv_reactor_progress(it->r);

    if (it->done < it->target)
        (void)iv_timer_add(it->r, 50u, integ_tick, it);
}

static void test_reactor_integration(void)
{
    enum { N = 3 };
    iv_reactor_t    *r;
    iv_taskpool_t   *pool;
    struct integ     it;
    struct slow_arg  args[N];
    iv_task_req_t    req;
    uint64_t         id;
    int              rc;
    int              i;

    pool = iv_taskpool_create(2, 8);
    chk(pool != NULL, "create pool for reactor integration");
    if (pool == NULL)
        return;

    r = iv_reactor_create(8);
    chk(r != NULL, "create reactor for integration");
    if (r == NULL) {
        iv_taskpool_destroy(pool);
        return;
    }

    it.r = r;
    it.pool = pool;
    it.done = 0;
    it.target = N;
    it.ticks = 0;
    it.p_first = 0u;
    it.p_last = 0u;

    chk(iv_reactor_add(r, iv_taskpool_eventfd(pool), IV_EV_READ, integ_eventfd, &it) != NULL,
        "register taskpool eventfd on reactor");

    chk(iv_timer_add(r, 50u, integ_tick, &it) != NULL, "arm periodic tick timer");

    for (i = 0; i < N; i++) {
        args[i].ms = 120; /* 明显长于 tick 周期：主循环必须在此期间继续转 */
        args[i].ran = 0;
        args[i].canceled = 0;
        req.name = "slow";
        req.fn = fn_sleep;
        req.arg = &args[i];
        req.timeout_ms = 0u;
        req.on_done = integ_on_done;
        req.on_done_arg = NULL;
        chk(iv_taskpool_submit(pool, &req, &id) == IV_OK, "submit slow task");
    }

    rc = iv_reactor_run(r);
    chk(rc == IV_OK, "reactor run returns IV_OK");
    chk(it.done == N, "reactor侧 collected all slow-task completions");
    chk(it.ticks >= 2, "tick timer fired multiple times while workers were busy");
    /* 主循环在慢任务运行期间持续推进：这正是 S6 判活要的结论 */
    chk((uint32_t)(it.p_last - it.p_first) >= 2u, "reactor progress advanced while slow tasks ran");

    iv_reactor_destroy(r);
    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    /* 落盘出口显式关闭：本测试不初始化日志，但模块失败路径会写日志，
     * 不关会在宿主机 mkdir 默认 /opt/log（口径同 test_basic / test_reactor） */
    iv_log_set_root(NULL);

    test_validation();
    test_complete_and_result();
    test_queue_full();
    test_cancel();
    test_deadline_expired();
    test_idle_progress();
    test_process_max();
    test_resubmit_in_callback();
    test_destroy_no_deadlock();
    test_fd_leak();
    test_reactor_integration();

    if (g_fail) {
        fprintf(stderr, "test_taskpool failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_taskpool passed (queue full, cancel, deadline, idle progress, reactor)\n");
    return 0;
}
