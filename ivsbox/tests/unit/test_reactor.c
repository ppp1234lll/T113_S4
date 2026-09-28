/*
 * iv_reactor 单测（开发计划 M1-S4）
 *
 * 覆盖：
 *   1) 入参校验、重复注册拒绝、默认 max_events；
 *   2) fd 事件：eventfd 自触发、注销后不再回调；同一批里注销另一个事件
 *      （延迟释放路径，真正的验证靠 `make asan` —— 立即 free 会在这里报 use-after-free）；
 *   3) 定时器：同刻多个到期全部触发、取消后不触发、回调里取消自己、
 *      回调里重挂自己、上限（IV_REACTOR_TIMER_MAX）拒绝、全部回收；
 *   4) 进度号：空闲轮（无任何 fd 事件）同样递增 —— S6 健康线程判活的前提；
 *   5) SIGTERM：从回调内投递，run 以 IV_OK 正常返回、后续定时器不再执行；
 *   6) EINTR：SIGALRM 打断 epoll_wait 后主循环继续（不得当成致命错误退出）。
 *
 * 说明：
 *   - 本测试不调用 iv_log_init，并显式关闭落盘出口，避免在宿主机 mkdir 默认
 *     /opt/log（沿用 test_basic 的口径）。
 *   - 第 4、6 例是真实等待（约 1.1s + 2.5s），不是 sleep 凑数：它们验证的正是
 *     "空闲时进度号仍在走"与"信号打断不算错误"，无法在零耗时的前提下验证。
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 公共小工具
 * ------------------------------------------------------------------------- */

static int efd_new(void)
{
    return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

static void efd_fire(int fd)
{
    uint64_t v = 1u;
    ssize_t  n = write(fd, &v, sizeof(v));

    (void)n; /* 失败也不影响用例语义：目的是让 fd 变可读 */
}

static void efd_drain(int fd)
{
    uint64_t v;
    while (read(fd, &v, sizeof(v)) == (ssize_t)sizeof(v))
        ;
}

/* 只带 reactor 句柄与一次触发计数的最小上下文 */
struct stopctx {
    iv_reactor_t *r;
    int           fired;
};

static void cb_stop(void *arg)
{
    struct stopctx *c = (struct stopctx *)arg;
    c->fired++;
    iv_reactor_stop(c->r);
}

static void cb_noop_event(int fd, uint32_t events, void *arg)
{
    (void)fd;
    (void)events;
    (void)arg;
}

static void cb_noop_timer(void *arg)
{
    (void)arg;
}

/* ---------------------------------------------------------------------------
 * 1) 入参校验
 * ------------------------------------------------------------------------- */

static void test_validation(void)
{
    iv_reactor_t *r;
    iv_event_t   *ev;
    int           fd = efd_new();

    chk(fd >= 0, "eventfd created");

    chk(iv_reactor_run(NULL) == IV_EINVAL, "run(NULL) rejected");
    chk(iv_reactor_progress(NULL) == 0u, "progress(NULL) == 0");
    chk(iv_reactor_timer_count(NULL) == 0, "timer_count(NULL) == 0");
    iv_reactor_stop(NULL);   /* 不崩即可 */
    iv_reactor_destroy(NULL); /* 不崩即可 */

    chk(iv_reactor_add(NULL, fd, IV_EV_READ, cb_noop_event, NULL) == NULL, "add(NULL reactor)");
    chk(iv_reactor_mod(NULL, NULL, IV_EV_READ) == IV_EINVAL, "mod(NULL reactor)");
    chk(iv_reactor_del(NULL, NULL) == IV_EINVAL, "del(NULL reactor)");
    chk(iv_timer_add(NULL, 10u, cb_noop_timer, NULL) == NULL, "timer add(NULL reactor)");
    chk(iv_timer_cancel(NULL, NULL) == IV_EINVAL, "timer cancel(NULL)");

    r = iv_reactor_create(0); /* <=0 取默认值 */
    chk(r != NULL, "create with default max_events");
    if (r == NULL) {
        (void)close(fd);
        return;
    }

    chk(iv_reactor_add(r, -1, IV_EV_READ, cb_noop_event, NULL) == NULL, "add(negative fd)");
    chk(iv_reactor_add(r, fd, 0u, cb_noop_event, NULL) == NULL, "add(no event bits)");
    chk(iv_reactor_add(r, fd, IV_EV_ONESHOT, cb_noop_event, NULL) == NULL, "add(oneshot only)");
    chk(iv_reactor_add(r, fd, IV_EV_READ, NULL, NULL) == NULL, "add(NULL cb)");

    ev = iv_reactor_add(r, fd, IV_EV_READ, cb_noop_event, NULL);
    chk(ev != NULL, "add ok");
    chk(iv_reactor_add(r, fd, IV_EV_READ, cb_noop_event, NULL) == NULL, "duplicate add rejected");

    if (ev != NULL) {
        chk(iv_reactor_mod(r, ev, 0u) == IV_EINVAL, "mod(no event bits) rejected");
        chk(iv_reactor_mod(r, ev, IV_EV_READ | IV_EV_WRITE) == IV_OK, "mod ok");
        chk(iv_reactor_del(r, ev) == IV_OK, "del ok");
        /* 句柄已随 del 释放，此后不得再引用（契约见 iv_reactor.h） */
    }
    chk(iv_reactor_del(r, NULL) == IV_EINVAL, "del(NULL event)");
    chk(iv_reactor_timer_count(r) == 0, "no timer leaked in validation case");

    iv_reactor_destroy(r);
    (void)close(fd);
}

/* ---------------------------------------------------------------------------
 * 2) 定时器上限
 * ------------------------------------------------------------------------- */

static void test_timer_limits(void)
{
    iv_reactor_t *r = iv_reactor_create(8);
    int           i;
    int           made = 0;

    chk(r != NULL, "create for timer-limit case");
    if (r == NULL)
        return;

    for (i = 0; i < IV_REACTOR_TIMER_MAX; i++) {
        if (iv_timer_add(r, 10000u, cb_noop_timer, NULL) != NULL)
            made++;
    }
    chk(made == IV_REACTOR_TIMER_MAX, "timer table accepts MAX timers");
    chk(iv_timer_add(r, 10000u, cb_noop_timer, NULL) == NULL, "timer beyond MAX rejected");

    iv_reactor_destroy(r);
}

/* ---------------------------------------------------------------------------
 * 3) fd 事件：自触发 + 注销后不再回调
 * ------------------------------------------------------------------------- */

struct evctx {
    iv_reactor_t *r;
    iv_event_t   *ev;
    int           efd;
    int           fires;
};

static void cb_ev_t2(void *arg);

static void cb_ev_read(int fd, uint32_t events, void *arg)
{
    struct evctx *c = (struct evctx *)arg;

    (void)events;
    efd_drain(fd);
    c->fires++;
}

static void cb_ev_t1(void *arg)
{
    struct evctx *c = (struct evctx *)arg;

    chk(c->fires == 1, "eventfd delivered exactly once in round 1");
    chk(iv_reactor_del(c->r, c->ev) == IV_OK, "del event from timer callback");

    efd_fire(c->efd); /* 注销之后再写：不应再被分发 */
    chk(iv_timer_add(c->r, 50u, cb_ev_t2, c) != NULL, "re-arm stop timer in callback");
}

static void cb_ev_t2(void *arg)
{
    struct evctx *c = (struct evctx *)arg;

    chk(c->fires == 1, "no callback after del");
    iv_reactor_stop(c->r);
}

static void test_fd_event(void)
{
    struct evctx c;
    int          fd = efd_new();
    int          rc;

    memset(&c, 0, sizeof(c));
    c.efd = fd;
    c.r = iv_reactor_create(8);
    chk(c.r != NULL, "create for fd-event case");
    if (c.r == NULL) {
        (void)close(fd);
        return;
    }

    c.ev = iv_reactor_add(c.r, fd, IV_EV_READ, cb_ev_read, &c);
    chk(c.ev != NULL, "register eventfd");
    efd_fire(fd);
    chk(iv_timer_add(c.r, 50u, cb_ev_t1, &c) != NULL, "arm round-1 timer");

    rc = iv_reactor_run(c.r);
    chk(rc == IV_OK, "run returns IV_OK");
    chk(c.fires == 1, "single delivery for the whole round");

    iv_reactor_destroy(c.r);
    (void)close(fd);
}

/* ---------------------------------------------------------------------------
 * 4) 同一批里注销另一个事件（延迟释放）
 * ------------------------------------------------------------------------- */

struct batchctx {
    iv_reactor_t *r;
    iv_event_t   *evb;
    int           efa;
    int           efb;
    int           fa;
    int           fb;
};

static void cb_batch_a(int fd, uint32_t events, void *arg)
{
    struct batchctx *c = (struct batchctx *)arg;

    (void)events;
    efd_drain(fd);
    c->fa++;
    if (c->evb != NULL) {
        /* 同一批里的另一个事件：此刻分发循环还没轮到它，
         * 句柄必须仍然有效（延迟释放），否则这是 use-after-free */
        chk(iv_reactor_del(c->r, c->evb) == IV_OK, "del peer event inside batch");
        c->evb = NULL;
    }
}

static void cb_batch_b(int fd, uint32_t events, void *arg)
{
    struct batchctx *c = (struct batchctx *)arg;

    (void)events;
    efd_drain(fd);
    c->fb++;
}

static void cb_batch_stop(void *arg)
{
    iv_reactor_stop(((struct batchctx *)arg)->r);
}

static void test_batch_del(void)
{
    struct batchctx c;
    int             b_first_round;
    int             rc;

    memset(&c, 0, sizeof(c));
    c.efa = efd_new();
    c.efb = efd_new();
    c.r = iv_reactor_create(8);
    chk(c.r != NULL, "create for batch-del case");
    if (c.r == NULL)
        return;

    chk(iv_reactor_add(c.r, c.efa, IV_EV_READ, cb_batch_a, &c) != NULL, "register peer A");
    c.evb = iv_reactor_add(c.r, c.efb, IV_EV_READ, cb_batch_b, &c);
    chk(c.evb != NULL, "register peer B");

    /* 两个 fd 同时可读 => 同一次 epoll_wait 返回一批两个事件 */
    efd_fire(c.efa);
    efd_fire(c.efb);
    chk(iv_timer_add(c.r, 60u, cb_batch_stop, &c) != NULL, "arm batch stop timer");
    rc = iv_reactor_run(c.r);
    chk(rc == IV_OK, "batch round returns IV_OK");
    chk(c.fa == 1, "peer A delivered once");
    /* epoll 不保证同批内的事件顺序，因此只断言"至多一次"：
     * 若 B 先被分发则 fb==1，否则 fb==0，两种都合法 */
    chk(c.fb <= 1, "peer B delivered at most once");

    b_first_round = c.fb;

    /* 第二轮：A 仍活着，B 已注销 => B 绝不能再被分发 */
    efd_fire(c.efa);
    efd_fire(c.efb);
    chk(iv_timer_add(c.r, 60u, cb_batch_stop, &c) != NULL, "arm second batch stop timer");
    rc = iv_reactor_run(c.r);
    chk(rc == IV_OK, "second batch round returns IV_OK");
    chk(c.fa == 2, "peer A still delivered in second round");
    chk(c.fb == b_first_round, "deleted peer never delivered again");

    iv_reactor_destroy(c.r);
    (void)close(c.efa);
    (void)close(c.efb);
}

/* ---------------------------------------------------------------------------
 * 5) 定时器：同刻到期 / 取消 / 自取消 / 重挂
 * ------------------------------------------------------------------------- */

struct timerctx {
    iv_reactor_t *r;
    iv_timer_t   *self;
    int           n_same;
    int           n_canceled;
    int           n_self;
    int           n_readd;
};

static void cb_t_same(void *arg)
{
    ((struct timerctx *)arg)->n_same++;
}

static void cb_t_canceled(void *arg)
{
    ((struct timerctx *)arg)->n_canceled++;
}

static void cb_t_self(void *arg)
{
    struct timerctx *c = (struct timerctx *)arg;

    c->n_self++;
    /* 取消自己：本次回调必须跑完，节点不能在回调返回前被释放 */
    chk(iv_timer_cancel(c->r, c->self) == IV_OK, "self cancel ok");
}

static void cb_t_readd(void *arg)
{
    struct timerctx *c = (struct timerctx *)arg;

    c->n_readd++;
    if (c->n_readd < 5) {
        chk(iv_timer_add(c->r, 10u, cb_t_readd, c) != NULL, "re-add in timer callback");
    } else {
        iv_reactor_stop(c->r);
    }
}

static void test_timers(void)
{
    struct timerctx c;
    iv_timer_t     *ct;
    int             rc;

    memset(&c, 0, sizeof(c));
    c.r = iv_reactor_create(8);
    chk(c.r != NULL, "create for timer case");
    if (c.r == NULL)
        return;

    /* 三个同刻到期：只处理堆顶一个是典型 bug，这里必须三个都触发 */
    chk(iv_timer_add(c.r, 20u, cb_t_same, &c) != NULL, "same-instant timer 1");
    chk(iv_timer_add(c.r, 20u, cb_t_same, &c) != NULL, "same-instant timer 2");
    chk(iv_timer_add(c.r, 20u, cb_t_same, &c) != NULL, "same-instant timer 3");

    /* 取消：不得再触发；重复取消报 IV_ENOENT（句柄因惰性回收仍然有效） */
    ct = iv_timer_add(c.r, 30u, cb_t_canceled, &c);
    chk(ct != NULL, "cancelable timer added");
    chk(iv_timer_cancel(c.r, ct) == IV_OK, "cancel ok");
    chk(iv_timer_cancel(c.r, ct) == IV_ENOENT, "double cancel rejected");

    c.self = iv_timer_add(c.r, 40u, cb_t_self, &c);
    chk(c.self != NULL, "self-canceling timer added");

    chk(iv_timer_add(c.r, 60u, cb_t_readd, &c) != NULL, "re-adding timer added");

    /* run 由 cb_t_readd 的第 5 次触发停止：此时已取消的 30ms 定时器早已到期，
     * 所以 run 返回后堆内应当一个定时器都不剩（含惰性回收路径） */
    rc = iv_reactor_run(c.r);
    chk(rc == IV_OK, "timer round returns IV_OK");
    chk(c.n_same == 3, "three timers due at the same instant all fire");
    chk(c.n_canceled == 0, "canceled timer never fires");
    chk(c.n_self == 1, "self-canceling timer fires exactly once");
    chk(c.n_readd == 5, "re-armed timer fires five times");
    chk(iv_reactor_timer_count(c.r) == 0, "all timers reclaimed after firing");

    iv_reactor_destroy(c.r);
}

/* ---------------------------------------------------------------------------
 * 6) 进度号：空闲轮同样递增
 * ------------------------------------------------------------------------- */

static void test_idle_progress(void)
{
    iv_reactor_t *r = iv_reactor_create(4);
    struct stopctx c;
    uint32_t       before;
    uint32_t       after;
    int            rc;

    chk(r != NULL, "create for progress case");
    if (r == NULL)
        return;

    memset(&c, 0, sizeof(c));
    c.r = r;
    before = iv_reactor_progress(r);
    chk(before == 0u, "progress starts at 0");

    /* 只挂一个 1100ms 的停止定时器：中间必经一次 1000ms 空闲兜底轮。
     * 若 epoll_wait 用了无限超时，这里会永远等下去、进度号不推进。 */
    chk(iv_timer_add(r, 1100u, cb_stop, &c) != NULL, "arm idle-case stop timer");
    rc = iv_reactor_run(r);
    chk(rc == IV_OK, "idle run returns IV_OK");
    chk(c.fired == 1, "stop timer fired");

    after = iv_reactor_progress(r);
    chk((uint32_t)(after - before) >= 2u, "progress advances on idle timeout rounds");

    iv_reactor_destroy(r);
}

/* ---------------------------------------------------------------------------
 * 7) SIGTERM 优雅退出
 * ------------------------------------------------------------------------- */

static void cb_terminate(void *arg)
{
    struct stopctx *c = (struct stopctx *)arg;

    c->fired++;
    chk(raise(SIGTERM) == 0, "raise SIGTERM");
}

static void cb_should_not_run(void *arg)
{
    ((struct stopctx *)arg)->fired++;
}

static void test_sigterm(void)
{
    iv_reactor_t   *r = iv_reactor_create(4);
    struct stopctx  term;
    struct stopctx  late;
    int             rc;

    chk(r != NULL, "create for SIGTERM case");
    if (r == NULL)
        return;

    memset(&term, 0, sizeof(term));
    memset(&late, 0, sizeof(late));
    term.r = r;
    late.r = r;

    chk(iv_timer_add(r, 30u, cb_terminate, &term) != NULL, "arm SIGTERM timer");
    chk(iv_timer_add(r, 300u, cb_should_not_run, &late) != NULL, "arm late timer");

    rc = iv_reactor_run(r);
    chk(rc == IV_OK, "SIGTERM ends run with IV_OK (graceful)");
    chk(term.fired == 1, "SIGTERM timer ran");
    chk(late.fired == 0, "loop exited before the late timer");

    iv_reactor_destroy(r);
}

/* ---------------------------------------------------------------------------
 * 8) EINTR：信号打断 epoll_wait 不算致命错误
 * ------------------------------------------------------------------------- */

static void on_sigalrm(int sig)
{
    (void)sig; /* 只用来打断 epoll_wait，不改变状态 */
}

static void test_eintr(void)
{
    iv_reactor_t   *r;
    struct stopctx  c;
    struct sigaction sa;
    int             rc;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigalrm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* 刻意不带 SA_RESTART：让 epoll_wait 返回 EINTR */
    chk(sigaction(SIGALRM, &sa, NULL) == 0, "install SIGALRM handler");

    r = iv_reactor_create(4);
    chk(r != NULL, "create for EINTR case");
    if (r == NULL)
        return;

    memset(&c, 0, sizeof(c));
    c.r = r;
    chk(iv_timer_add(r, 2500u, cb_stop, &c) != NULL, "arm EINTR-case stop timer");

    (void)alarm(1u); /* 1s 后打断阻塞中的 epoll_wait */
    rc = iv_reactor_run(r);
    (void)alarm(0u);

    chk(rc == IV_OK, "EINTR does not abort the loop");
    chk(c.fired == 1, "loop survived the signal and reached the stop timer");
    chk(iv_reactor_progress(r) >= 3u, "progress advanced across the interrupted round");

    iv_reactor_destroy(r);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    /* 落盘出口显式关闭：本测试不初始化日志，但 reactor 的失败路径会写日志，
     * 不关会在宿主机 mkdir 默认 /opt/log（口径同 test_basic） */
    iv_log_set_root(NULL);

    test_validation();
    test_timer_limits();
    test_fd_event();
    test_batch_del();
    test_timers();
    test_idle_progress();
    test_sigterm();
    test_eintr();

    if (g_fail) {
        fprintf(stderr, "test_reactor failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_reactor passed (fd events, timers, progress, signalfd, EINTR)\n");
    return 0;
}
