/*
 * iv_health 单测（libivmodules）。
 *
 * 两条设计选择，先说清楚：
 *
 * 1) **不真喂狗，用 pipe 代替 watchdog fd**。health 把 watchdog_fd 当成"往里写
 *    1 字节"的对象，pipe 写端完全等价；测试读另一端就能数出"喂了几次"，并且
 *    能靠 EOF 检测到"health 已经 close 掉 fd（＝停喂）"。这正是 iv_watchdog 接口
 *    全部收 fd 的意义 —— 不需要任何测试专用钩子。
 *
 * 2) **不注入假进度号，只用真实对象**。判据跑在真实 iv_reactor / iv_taskpool
 *    的进度号上才有意义，否则测的是复制品。两种情形在单进程里都能造出来：
 *      - reactor 创建后**不 run** → 进度号恒 0 → 等价"主循环卡死"，覆盖判死路径；
 *      - taskpool 空转 → worker 的空闲 cond_timedwait 超时轮也推进进度号
 *        （S5 刻意埋的行为）→ 恒健康，覆盖"空闲不得误报"。
 *
 * 新增用例沿用"只用真实对象"的口径（S56-01 / S6-01）：
 *   - 用例 6/7 用**真实 taskpool 跑真长任务**验证任务心跳：不调心跳判死、
 *     调心跳判活；wd_fd=-1，现有运行期守卫对 fd<0 会跳过全部看门狗动作。
 *   - 用例 8 验证无法主动心跳的单次阻塞调用，在声明的 deadline 租约内
 *     不会被误报为 worker 死锁。
 *   - 用例 9 用"两端都关掉的 pipe 写端"伪造持续 I/O 失败，验证喂狗失败计数
 *     与 fail-stop；fault_code 语义不变。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_health.h"
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

static void sleep_ms(unsigned ms)
{
    usleep((useconds_t)ms * 1000u);
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl != -1)
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* 读干管道，返回读到的总字节数；读到 EOF 或 EAGAIN 即停。 */
static int drain(int fd)
{
    char    buf[128];
    ssize_t n;
    int     total = 0;

    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        total += (int)n;
    }
    return total;
}

/* 写端是否已全部关闭（EOF） */
static int at_eof(int fd)
{
    char c;

    return read(fd, &c, 1) == 0;
}

/* 轮询快照，直到"fault_code 是否非 0"符合 nonzero。成功返回 1，超时返回 0。 */
static int wait_for_fault(iv_health_t *h, int nonzero, unsigned timeout_ms)
{
    unsigned             waited = 0;
    iv_health_snapshot_t s;

    while (waited <= timeout_ms) {
        if (iv_health_snapshot(h, &s) == IV_OK) {
            if ((s.fault_code != 0) == (nonzero != 0))
                return 1;
        }
        sleep_ms(10);
        waited += 10;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 用例 1：主循环卡死 → 判死 → 关 fd 停喂
 * ------------------------------------------------------------------------- */
static void case_reactor_dead(void)
{
    int                  p[2];
    iv_reactor_t        *r;
    iv_health_t         *h;
    iv_health_snapshot_t s;
    int                  fed;

    chk(pipe(p) == 0, "case1: pipe()");
    set_nonblock(p[0]);

    r = iv_reactor_create(8);
    chk(r != NULL, "case1: reactor create");
    /* 刻意不调 iv_reactor_run()：进度号恒 0，等价主循环卡死 */

    h = iv_health_start(r, NULL, p[1], 300u, 50u);
    chk(h != NULL, "case1: health start");

    chk(wait_for_fault(h, 1, 3000u) == 1, "case1: reactor-dead fault detected");

    chk(iv_health_snapshot(h, &s) == IV_OK, "case1: snapshot");
    chk((s.fault_code & IV_HEALTH_REACTOR_DEAD) != 0,
        "case1: fault_code carries REACTOR_DEAD");
    chk(s.watchdog_fd < 0, "case1: watchdog_fd marked as stopped");

    /* 停喂的硬证据：health 已 close 写端 → 读端能吃干净并读到 EOF */
    fed = drain(p[0]);
    chk(fed >= 1, "case1: fed at least once before detecting the fault");
    chk(at_eof(p[0]) == 1, "case1: watchdog fd closed => feeding stopped");

    iv_health_destroy(h);
    iv_reactor_destroy(r);
    close(p[0]);
}

/* ---------------------------------------------------------------------------
 * 用例 2：taskpool 空转（空闲也推进进度号）→ 恒健康，不得误报
 *
 * stuck_ms 取 2000ms，**故意为 IV_TASKPOOL_IDLE_POLL_MS(1000ms) 的 2 倍**，
 * 观察 3.2s（> 3 个 idle 周期）。留 2 倍余量是因为宿主/VM 负载停顿若超过一个
 * idle 周期，进度号就会晚推一次；余量只有 1.5 倍时这条用例会偶发误报
 * （判死 → 停喂），那是测试自身脆弱，不是被测代码的问题。
 * 若 worker 空闲不推进进度号，这里必然误判；实测保持健康即证明 S5 埋的
 * "空闲轮推进进度号"确实生效。
 * ------------------------------------------------------------------------- */
static void case_healthy_worker_idle(void)
{
    int                  p[2];
    iv_taskpool_t       *pool;
    iv_health_t         *h;
    iv_health_snapshot_t s;
    int                  first;
    int                  second;

    chk(pipe(p) == 0, "case2: pipe()");
    set_nonblock(p[0]);

    pool = iv_taskpool_create(2, 8);
    chk(pool != NULL, "case2: taskpool create");

    h = iv_health_start(NULL, pool, p[1], 2000u, 50u);
    chk(h != NULL, "case2: health start");

    sleep_ms(1600);
    first = drain(p[0]);
    sleep_ms(1600);
    second = drain(p[0]);

    chk(first >= 1, "case2: feeding during first window");
    chk(second >= 1, "case2: still feeding during second window (span > stuck_ms)");

    chk(iv_health_snapshot(h, &s) == IV_OK, "case2: snapshot");
    chk(s.fault_code == IV_HEALTH_OK, "case2: idle workers must not raise a fault");
    chk(s.worker_count == 2, "case2: observed both workers");
    chk(s.tick_count >= 20u, "case2: ticked ~20x/s over 3.2s");

    iv_health_destroy(h);
    iv_taskpool_destroy(pool);
    /* 健康路径下 health 线程不会 close 写端，destroy 按契约也不关，
     * 所以这里必须自己关 —— 否则本用例每跑一次就漏一个 fd。 */
    close(p[1]);
    close(p[0]);
}

/* ---------------------------------------------------------------------------
 * 用例 3：不观察任何对象 → 恒健康（边界，不应崩）
 * ------------------------------------------------------------------------- */
static void case_no_observers(void)
{
    iv_health_t         *h;
    iv_health_snapshot_t s;

    h = iv_health_start(NULL, NULL, -1, 100u, 20u);
    chk(h != NULL, "case3: health start without observers");

    sleep_ms(250);
    chk(iv_health_snapshot(h, &s) == IV_OK, "case3: snapshot");
    chk(s.fault_code == IV_HEALTH_OK, "case3: nothing observed => always healthy");
    chk(s.worker_count == 0, "case3: worker_count is 0");
    chk(s.tick_count >= 3u, "case3: ticked");

    iv_health_destroy(h);
}

/* ---------------------------------------------------------------------------
 * 用例 4：无 watchdog fd（< 0）时判死也不得崩
 * ------------------------------------------------------------------------- */
static void case_dead_without_fd(void)
{
    iv_reactor_t        *r;
    iv_health_t         *h;
    iv_health_snapshot_t s;

    r = iv_reactor_create(4);
    chk(r != NULL, "case4: reactor create");

    h = iv_health_start(r, NULL, -1, 200u, 30u);
    chk(h != NULL, "case4: health start without watchdog fd");

    chk(wait_for_fault(h, 1, 2000u) == 1, "case4: fault detected without fd");
    chk(iv_health_snapshot(h, &s) == IV_OK, "case4: snapshot");
    chk(s.watchdog_fd < 0, "case4: watchdog_fd stays -1");

    iv_health_destroy(h);
    iv_reactor_destroy(r);
}

/* ---------------------------------------------------------------------------
 * 用例 5：收尾安全性（NULL 不得崩；正常收尾不泄漏，由 ASan 兜底）
 * ------------------------------------------------------------------------- */
static void case_destroy_safe(void)
{
    iv_health_t *h;

    iv_health_destroy(NULL); /* 必须安全 */

    h = iv_health_start(NULL, NULL, -1, 500u, 50u);
    chk(h != NULL, "case5: health start for destroy test");
    iv_health_destroy(h);
}

/* ---------------------------------------------------------------------------
 * 用例 6/7（S56-01）：真实 taskpool + 真实 health 线程，wd_fd=-1（现有运行期
 * 守卫对 fd<0 全部跳过看门狗动作，本组用例只考察进度判定）。
 *
 *   - 用例 6 对照组：worker 跑 1.6s 长任务且**不**调心跳 → 进度号在任务期间
 *     冻结，stuck_ms=800ms 必判 IV_HEALTH_WORKER_DEAD。
 *   - 用例 7 实验组：同参数，任务执行期间每 200ms 调一次 iv_task_heartbeat
 *     → 进度号持续推进，全程判健康。
 *   两个 worker 只开 1 个：worker 被任务占住期间不可能有空闲轮 +1，判据干净。
 * ------------------------------------------------------------------------- */

struct hb_arg {
    int         total_ms;
    int         beat_ms; /* > 0 时每 beat_ms 调一次 iv_task_heartbeat；0 = 不调 */
    _Atomic int ran;
    _Atomic int done;
};

static int fn_long_task(iv_task_ctx_t *ctx)
{
    struct hb_arg *a = (struct hb_arg *)ctx->arg;
    int step = (a->beat_ms > 0) ? a->beat_ms : 50;
    int elapsed = 0;

    a->ran = 1;
    while (elapsed < a->total_ms) {
        sleep_ms(step);
        elapsed += step;
        if (a->beat_ms > 0)
            iv_task_heartbeat(ctx);
    }
    a->done = 1;
    return IV_OK;
}

static void submit_long_task(iv_taskpool_t *pool, struct hb_arg *a, uint32_t timeout_ms)
{
    iv_task_req_t req;

    req.name        = "health-long";
    req.fn          = fn_long_task;
    req.arg         = a;
    req.timeout_ms  = timeout_ms;
    req.on_done     = NULL;
    req.on_done_arg = NULL;
    chk(iv_taskpool_submit(pool, &req, NULL) == IV_OK, "hb: submit long task");
}

static void case_worker_dead_without_heartbeat(void)
{
    iv_taskpool_t       *pool;
    iv_health_t         *h;
    iv_health_snapshot_t s;
    struct hb_arg        a;
    int                  w;

    pool = iv_taskpool_create(1, 4);
    chk(pool != NULL, "case6: taskpool create");
    if (pool == NULL)
        return;

    a.total_ms = 1600;
    a.beat_ms  = 0; /* 不调心跳：进度号在任务期间冻结 */
    a.ran = 0;
    a.done = 0;

    h = iv_health_start(NULL, pool, -1, 800u, 200u);
    chk(h != NULL, "case6: health start");
    if (h == NULL) {
        iv_taskpool_destroy(pool);
        return;
    }

    /* 立即提交并等开工：worker 被占住 1.6s，期间进度号无人推进 */
    submit_long_task(pool, &a, 0u);
    for (w = 0; w < 200 && a.ran == 0; w++)
        sleep_ms(5);
    chk(a.ran == 1, "case6: long task started");

    chk(wait_for_fault(h, 1, 5000u) == 1, "case6: fault detected during long task");
    chk(iv_health_snapshot(h, &s) == IV_OK, "case6: snapshot");
    chk((s.fault_code & IV_HEALTH_WORKER_DEAD) != 0,
        "case6: fault_code carries WORKER_DEAD");

    /* 收尾：等任务自然结束再销毁（health 必须先于 pool 销毁，见头文件硬约束） */
    for (w = 0; w < 400 && a.done == 0; w++)
        sleep_ms(10);
    chk(a.done == 1, "case6: long task finished");

    iv_health_destroy(h);
    iv_taskpool_destroy(pool);
}

static void case_worker_alive_with_heartbeat(void)
{
    iv_taskpool_t       *pool;
    iv_health_t         *h;
    iv_health_snapshot_t s;
    struct hb_arg        a;
    int                  w;
    int                  saw_fault = 0;

    pool = iv_taskpool_create(1, 4);
    chk(pool != NULL, "case7: taskpool create");
    if (pool == NULL)
        return;

    a.total_ms = 1600;
    a.beat_ms  = 200; /* 每 200ms 一次心跳，远快于 stuck_ms=800 */
    a.ran = 0;
    a.done = 0;

    h = iv_health_start(NULL, pool, -1, 800u, 200u);
    chk(h != NULL, "case7: health start");
    if (h == NULL) {
        iv_taskpool_destroy(pool);
        return;
    }

    submit_long_task(pool, &a, 0u);

    /* 任务运行全程轮询快照：心跳推进进度号，任何一次判死都是回归 */
    for (w = 0; w < 400 && a.done == 0; w++) {
        sleep_ms(10);
        if (iv_health_snapshot(h, &s) == IV_OK && s.fault_code != IV_HEALTH_OK)
            saw_fault = 1;
    }
    chk(a.done == 1, "case7: long task finished");
    chk(saw_fault == 0, "case7: heartbeat keeps the worker judged alive");

    /* 结束时快照：最后一次心跳距此刻不足一个 stuck 窗口，应判健康 */
    chk(iv_health_snapshot(h, &s) == IV_OK, "case7: final snapshot");
    chk(s.fault_code == IV_HEALTH_OK, "case7: snapshot healthy at end");
    chk(s.tick_count > 0u, "case7: ticks accumulated");
    chk(s.wd_io_fault == 0, "case7: no watchdog io fault without a watchdog fd");

    iv_health_destroy(h);
    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 用例 8（S56-01）：单次阻塞调用无法主动 heartbeat，但只要仍处在提交时声明的
 * 端到端 deadline 内，就属于合法运行而非 worker 死锁。任务总长 1.6s，健康窗口
 * 仅 800ms；若 deadline 租约没有生效，必然在任务完成前误报。
 * ------------------------------------------------------------------------- */
static void case_worker_alive_with_deadline_lease(void)
{
    iv_taskpool_t       *pool;
    iv_health_t         *h;
    iv_health_snapshot_t s;
    struct hb_arg        a;
    int                  w;
    int                  saw_fault = 0;

    pool = iv_taskpool_create(1, 4);
    chk(pool != NULL, "case8: taskpool create");
    if (pool == NULL)
        return;

    a.total_ms = 1600;
    a.beat_ms = 0; /* 模拟一次无法插入 heartbeat 的阻塞调用 */
    a.ran = 0;
    a.done = 0;

    h = iv_health_start(NULL, pool, -1, 800u, 200u);
    chk(h != NULL, "case8: health start");
    if (h == NULL) {
        iv_taskpool_destroy(pool);
        return;
    }

    submit_long_task(pool, &a, 3000u); /* deadline 覆盖 1.6s 的最坏运行时间 */
    for (w = 0; w < 400 && a.done == 0; w++) {
        sleep_ms(10);
        if (iv_health_snapshot(h, &s) == IV_OK && s.fault_code != IV_HEALTH_OK)
            saw_fault = 1;
    }

    chk(a.ran == 1 && a.done == 1, "case8: blocking task completed");
    chk(saw_fault == 0, "case8: deadline lease prevents false worker-dead alarm");
    chk(iv_health_snapshot(h, &s) == IV_OK && s.fault_code == IV_HEALTH_OK,
        "case8: final snapshot remains healthy");

    iv_health_destroy(h);
    iv_taskpool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * 用例 9（S6-01）：喂狗持续失败 → 计数、置 wd_io_fault、达到阈值 fail-stop 关 fd
 *
 * 造法：真实 pipe，**两端都立刻关掉**，把"已关闭的写端 fd 号"传给 health ——
 * fd >= 0 但 keepalive 的 write 必然 EBADF，等价"看门狗设备一直在报 I/O 错误"。
 * 注意：断言窗口内不要打开任何新 fd（本用例除 health/observer 外不做任何
 * open），避免刚关闭的 fd 号被复用，导致 health 把 0x00 写进无关对象。
 * ------------------------------------------------------------------------- */
static void case_wd_io_fault(void)
{
    int                  p[2];
    int                  write_end;
    iv_health_t         *h;
    iv_health_snapshot_t s;

    chk(pipe(p) == 0, "case9: pipe()");
    write_end = p[1];
    close(p[0]);
    close(p[1]);

    /* observers 全 NULL → 恒判健康 → 每个 tick 都会尝试喂狗；
     * interval=200ms 跑 ~1s（>= 5 tick），足够 streak 达到 IV_HEALTH_WD_FAIL_MAX */
    h = iv_health_start(NULL, NULL, write_end, 2000u, 200u);
    chk(h != NULL, "case9: health start");

    sleep_ms(1000);

    chk(iv_health_snapshot(h, &s) == IV_OK, "case9: snapshot");
    chk(s.wd_io_fault == 1, "case9: wd_io_fault latched after keepalive failures");
    chk(s.wd_fail_streak >= IV_HEALTH_WD_FAIL_MAX,
        "case9: streak reached the fail threshold");
    chk(s.watchdog_fd == -1, "case9: fail-stop closed the watchdog fd");
    chk(s.fault_code == IV_HEALTH_OK,
        "case9: fault_code semantics unchanged (progress-only)");

    iv_health_destroy(h);
}

int main(void)
{
    case_reactor_dead();
    case_healthy_worker_idle();
    case_no_observers();
    case_dead_without_fd();
    case_destroy_safe();
    case_worker_dead_without_heartbeat();
    case_worker_alive_with_heartbeat();
    case_worker_alive_with_deadline_lease();
    case_wd_io_fault();

    if (g_fail != 0) {
        fprintf(stderr, "test_health failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_health passed (dead-stop, idle-no-alarm, no-observer, no-fd, destroy, "
           "heartbeat, wd-io-failstop)\n");
    return 0;
}
