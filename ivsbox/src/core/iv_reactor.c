/*
 * IVSBox 事件循环 Reactor 实现（计划 M1-S4）
 *
 * 设计要点与取舍（细节理由见 iv_reactor.h 顶部注释）：
 *
 * 1) 时间基准：直接 clock_gettime(CLOCK_MONOTONIC)，不调 iv_clock。
 *    原因是链接顺序（core 在最后）不允许反向依赖 libivhal，头文件已详述。
 *
 * 2) 定时器 = 最小堆（绝对到期时刻） + 单个 timerfd 作"唤醒闹钟"。
 *    堆负责"谁先到"，timerfd 只负责"把内核从 epoll_wait 里叫醒"，
 *    到期后一次性弹空所有 deadline <= now 的节点（同刻到期可能有多个，
 *    只处理堆顶一个是常见 bug）。timerfd 必须读干，否则电平触发会持续上报。
 *
 * 3) 取消定时器采用**惰性标记**：只置 canceled，不立即释放。
 *    因为节点还在堆里，提前释放会留下悬空指针；随后由"到期弹出"或
 *    "批次末尾压缩"两条路径回收，两条路径都保证没有定时器正在回调中。
 *    代价是"取消成功返回后句柄立即失效"——节点随时可能被上述两条路径
 *    回收，此后再使用该句柄（含重复 cancel）是 UB，契约见 iv_reactor.h。
 *
 * 4) fd 事件的注销采用**延迟释放**（zombie 链）：一次 epoll_wait 会返回一批
 *    事件，若某个回调里 del 掉了同一批中的另一个事件，立即 free 会让分发
 *    循环读到已释放内存。故分发期间 del 只摘链并入 zombie，整批分发结束
 *    后再统一 free；分发循环据 active 标记跳过已注销项。
 *
 * 5) 电平触发（LT）而非边沿触发：本层是通用分发中心，十几个 fd 的规模下
 *    ET 的性能优势为零，却要求每个回调都严格遵守"读到 EAGAIN"的隐式契约。
 *    ERR/HUP/RDHUP 由 epoll 无条件上报，必须翻译后交给回调，否则 M2 的
 *    平台 socket 断线事件会被静默丢弃。
 *
 * 6) epoll_wait 超时必须是有限值（空闲兜底 1s）：进度号在超时空轮同样递增，
 *    S6 健康线程才能区分"空闲"与"主循环卡死"。用 -1 会让空闲被误判为卡死。
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

#define REACTOR_MOD "reactor"

/* ---------------------------------------------------------------------------
 * 数据结构
 * ------------------------------------------------------------------------- */

struct iv_event {
    int          fd;
    uint32_t     events;  /* 调用方注册的事件位（IV_EV_*） */
    iv_event_fn  cb;
    void        *arg;
    int          active;  /* 1 = 句柄有效且已注册；0 = 已注销，等待回收 */
    iv_reactor_t *owner;  /* 句柄归属：mod/del 校验 owner == r，防跨实例误用 */
    iv_event_t  *prev;    /* reactor 活性事件双向链表 */
    iv_event_t  *next;
    iv_event_t  *znext;   /* 待回收单向链表 */
};

struct iv_timer {
    uint64_t     deadline_ms; /* 绝对到期时刻（CLOCK_MONOTONIC 毫秒） */
    iv_timer_fn  cb;
    void        *arg;
    int          canceled;    /* 惰性取消标记 */
    int          dispatched;  /* 1 = 正在回调中，禁止被回收 */
    iv_reactor_t *owner;      /* 句柄归属：cancel 校验 owner == r，防跨实例误用 */
};

struct iv_reactor {
    int          epfd;
    int          tfd;      /* timerfd：唯一唤醒闹钟 */
    int          sfd;      /* signalfd：SIGTERM/SIGINT/SIGPIPE */
    struct epoll_event *epev;
    int          max_events;
    int          running;
    uint32_t     progress;      /* 进度号：跨线程读取，仅经 __atomic_* 访问（见头文件） */
    int          dispatching;   /* >0：处于事件分发批次内 */
    iv_event_t  *ev_head;       /* 活性事件链表 */
    iv_event_t  *zombies;       /* 待回收事件链表 */
    iv_timer_t  *heap[IV_REACTOR_TIMER_MAX]; /* 最小堆：按 deadline 排序 */
    int          heap_n;
    int          canceled_n;    /* 堆内已取消、待回收的定时器数 */
    sigset_t     oldmask;
    int          mask_saved;
};

/* ---------------------------------------------------------------------------
 * 基础工具
 * ------------------------------------------------------------------------- */

/* 单调毫秒。刻意不复用 iv_clock（libivhal）——见文件头第 1 条说明。
 * clock_gettime 不可失败（合法参数下），失败按 0 处理不会引入错误逻辑分支，
 * 只会让"相对差值"整体偏移，且这种失败在 Linux 上不存在。 */
static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* 自有事件位 -> epoll 位。READ 自动附加 RDHUP：半关闭对上层是必要信息，
 * 且只在流式 fd 上有意义，对 eventfd/timerfd 无副作用。 */
static uint32_t to_epoll(uint32_t events)
{
    uint32_t e = 0u;

    if ((events & IV_EV_READ) != 0u)
        e |= EPOLLIN | EPOLLRDHUP;
    if ((events & IV_EV_WRITE) != 0u)
        e |= EPOLLOUT;
    if ((events & IV_EV_ONESHOT) != 0u)
        e |= EPOLLONESHOT;
    return e;
}

/* epoll 位 -> 自有事件位（回调可见）。ERR/HUP/RDHUP 无条件透传。 */
static uint32_t to_iv(uint32_t epev)
{
    uint32_t e = 0u;

    if ((epev & EPOLLIN) != 0u)
        e |= IV_EV_READ;
    if ((epev & EPOLLOUT) != 0u)
        e |= IV_EV_WRITE;
    if ((epev & EPOLLERR) != 0u)
        e |= IV_EV_ERR;
    if ((epev & EPOLLHUP) != 0u)
        e |= IV_EV_HUP;
    if ((epev & EPOLLRDHUP) != 0u)
        e |= IV_EV_RDHUP;
    return e;
}

/* ---------------------------------------------------------------------------
 * 活性事件链表 / 待回收链表
 * ------------------------------------------------------------------------- */

static void ev_list_push(iv_reactor_t *r, iv_event_t *ev)
{
    ev->prev = NULL;
    ev->next = r->ev_head;
    if (r->ev_head != NULL)
        r->ev_head->prev = ev;
    r->ev_head = ev;
}

static void ev_list_unlink(iv_reactor_t *r, iv_event_t *ev)
{
    if (ev->prev != NULL)
        ev->prev->next = ev->next;
    else
        r->ev_head = ev->next;
    if (ev->next != NULL)
        ev->next->prev = ev->prev;
    ev->prev = NULL;
    ev->next = NULL;
}

static void flush_zombies(iv_reactor_t *r)
{
    iv_event_t *ev = r->zombies;

    r->zombies = NULL;
    while (ev != NULL) {
        iv_event_t *n = ev->znext;
        free(ev);
        ev = n;
    }
}

/* ---------------------------------------------------------------------------
 * 定时器最小堆
 * ------------------------------------------------------------------------- */

static void heap_swim(iv_reactor_t *r, int i)
{
    while (i > 0) {
        int p = (i - 1) / 2;
        iv_timer_t *t;

        if (r->heap[p]->deadline_ms <= r->heap[i]->deadline_ms)
            break;
        t = r->heap[p];
        r->heap[p] = r->heap[i];
        r->heap[i] = t;
        i = p;
    }
}

static void heap_sink(iv_reactor_t *r, int i)
{
    for (;;) {
        int l = 2 * i + 1;
        int m = i;
        iv_timer_t *t;

        if (l < r->heap_n && r->heap[l]->deadline_ms < r->heap[m]->deadline_ms)
            m = l;
        if (l + 1 < r->heap_n && r->heap[l + 1]->deadline_ms < r->heap[m]->deadline_ms)
            m = l + 1;
        if (m == i)
            break;
        t = r->heap[m];
        r->heap[m] = r->heap[i];
        r->heap[i] = t;
        i = m;
    }
}

static void heap_pop(iv_reactor_t *r)
{
    r->heap_n--;
    r->heap[0] = r->heap[r->heap_n];
    r->heap[r->heap_n] = NULL;
    if (r->heap_n > 0)
        heap_sink(r, 0);
}

/* 原地堆化（Floyd），仅在压缩后使用 */
static void heap_build(iv_reactor_t *r)
{
    int i;

    for (i = r->heap_n / 2 - 1; i >= 0; i--)
        heap_sink(r, i);
}

/* 回收堆内所有"已取消且不在回调中"的定时器。
 * 只允许在没有任何定时器正在回调时调用（当前唯一调用点：批处理末尾）。 */
static void compact_canceled(iv_reactor_t *r)
{
    int i;
    int w = 0;

    if (r->canceled_n == 0)
        return;
    for (i = 0; i < r->heap_n; i++) {
        iv_timer_t *t = r->heap[i];

        if (t->canceled != 0) {
            free(t);
            continue;
        }
        r->heap[w] = t;
        w++;
    }
    r->heap_n = w;
    r->canceled_n = 0;
    heap_build(r);
}

/* 按堆顶装定 timerfd；堆空则 disarm（it_value 全 0）。
 * 注意 it_value == 0 是"解除定时"的语义，因此"已到期"必须给一个非零最小值。 */
static int arm_timer(iv_reactor_t *r)
{
    struct itimerspec its;
    uint64_t now;
    uint64_t delta;

    memset(&its, 0, sizeof(its));
    if (r->heap_n > 0) {
        now = now_ms();
        delta = (r->heap[0]->deadline_ms > now) ? (r->heap[0]->deadline_ms - now) : 0u;
        its.it_value.tv_sec = (time_t)(delta / 1000u);
        its.it_value.tv_nsec = (long)((delta % 1000u) * 1000000u);
        if (delta == 0u)
            its.it_value.tv_nsec = 1L;
    }
    if (timerfd_settime(r->tfd, 0, &its, NULL) != 0) {
        IV_LOG_E(REACTOR_MOD, "timerfd_settime failed, errno=%d", errno);
        return IV_EFAIL;
    }
    return IV_OK;
}

/* 下次 epoll_wait 的等待毫秒：min(空闲兜底, 最近定时器剩余) */
static int next_timeout_ms(const iv_reactor_t *r)
{
    uint64_t now;
    uint64_t delta;

    if (r->heap_n == 0)
        return IV_REACTOR_IDLE_TIMEOUT_MS;
    now = now_ms();
    if (r->heap[0]->deadline_ms <= now)
        return 0;
    delta = r->heap[0]->deadline_ms - now;
    return (delta < (uint64_t)IV_REACTOR_IDLE_TIMEOUT_MS) ? (int)delta : IV_REACTOR_IDLE_TIMEOUT_MS;
}

/* 弹空所有已到期定时器并逐个回调。一次性语义：回调返回后即释放节点。 */
static void reap_timers(iv_reactor_t *r)
{
    uint64_t now = now_ms();

    for (;;) {
        iv_timer_t *t;

        if (r->heap_n == 0)
            break;
        t = r->heap[0];
        if (t->deadline_ms > now)
            break;
        heap_pop(r);
        if (t->canceled != 0) {
            r->canceled_n--;
            free(t);
            continue;
        }
        t->dispatched = 1;
        if (t->cb != NULL)
            t->cb(t->arg);
        t->dispatched = 0;
        /* 回调里取消了自己的情形也在此收尾：节点此刻已不在堆内，可直接释放 */
        if (t->canceled != 0)
            r->canceled_n--;
        free(t);
    }
    compact_canceled(r);
}

/* ---------------------------------------------------------------------------
 * 内部 fd 事件的回调
 * ------------------------------------------------------------------------- */

static void on_timerfd(int fd, uint32_t events, void *arg)
{
    iv_reactor_t *r = (iv_reactor_t *)arg;
    uint64_t expiries;

    (void)events;
    /* 必须读到 EAGAIN：电平触发下残留计数会让 epoll_wait 立刻再次上报 */
    for (;;) {
        ssize_t n = read(fd, &expiries, sizeof(expiries));

        if (n == (ssize_t)sizeof(expiries))
            continue;
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    reap_timers(r);
}

static void on_signalfd(int fd, uint32_t events, void *arg)
{
    iv_reactor_t *r = (iv_reactor_t *)arg;
    struct signalfd_siginfo si;

    (void)events;
    for (;;) {
        ssize_t n = read(fd, &si, sizeof(si));

        if (n == (ssize_t)sizeof(si)) {
            if (si.ssi_signo == (uint32_t)SIGTERM || si.ssi_signo == (uint32_t)SIGINT) {
                IV_LOG_I(REACTOR_MOD, "signal %u received, stopping", (unsigned)si.ssi_signo);
                r->running = 0;
            } else {
                /* SIGPIPE：写已关闭的 socket 时由内核投递，此处只记录，
                 * 真正的处理是调用方拿到 EPIPE 后走自己的断连逻辑 */
                IV_LOG_D(REACTOR_MOD, "signal %u ignored", (unsigned)si.ssi_signo);
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Reactor 生命周期
 * ------------------------------------------------------------------------- */

iv_reactor_t *iv_reactor_create(int max_events)
{
    iv_reactor_t *r;
    sigset_t mask;

    if (max_events <= 0)
        max_events = IV_REACTOR_MAX_EVENTS_DEFAULT;

    r = (iv_reactor_t *)calloc(1, sizeof(*r));
    if (r == NULL) {
        IV_LOG_E(REACTOR_MOD, "create: out of memory");
        return NULL;
    }
    r->epfd = -1;
    r->tfd = -1;
    r->sfd = -1;
    r->max_events = max_events;

    r->epev = (struct epoll_event *)calloc((size_t)max_events, sizeof(*r->epev));
    if (r->epev == NULL) {
        IV_LOG_E(REACTOR_MOD, "create: out of memory for event array");
        goto fail;
    }

    r->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epfd < 0) {
        IV_LOG_E(REACTOR_MOD, "epoll_create1 failed, errno=%d", errno);
        goto fail;
    }
    r->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (r->tfd < 0) {
        IV_LOG_E(REACTOR_MOD, "timerfd_create failed, errno=%d", errno);
        goto fail;
    }

    /*
     * 信号：必须先屏蔽再建 signalfd（signalfd 只对"已屏蔽"的信号生效）。
     * 这里用 sigprocmask 而非 pthread_sigmask：本模块在创建 reactor 时尚无任何
     * 线程，sigprocmask 语义正确；而 pthread_sigmask 在 glibc 2.25（板端）位于
     * libpthread，会把 -lpthread 引进 libivcore，破坏"仅 libc"约定。
     * 必须在创建线程之前调用——线程掩码在 pthread_create 时继承。
     */
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &mask, &r->oldmask) != 0) {
        IV_LOG_E(REACTOR_MOD, "sigprocmask failed, errno=%d", errno);
        goto fail;
    }
    r->mask_saved = 1;

    r->sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (r->sfd < 0) {
        IV_LOG_E(REACTOR_MOD, "signalfd failed, errno=%d", errno);
        goto fail;
    }

    /* timerfd / signalfd 走与业务 fd 完全相同的分发路径，无特殊通道 */
    if (iv_reactor_add(r, r->tfd, IV_EV_READ, on_timerfd, r) == NULL)
        goto fail;
    if (iv_reactor_add(r, r->sfd, IV_EV_READ, on_signalfd, r) == NULL)
        goto fail;

    return r;

fail:
    iv_reactor_destroy(r);
    return NULL;
}

void iv_reactor_destroy(iv_reactor_t *r)
{
    iv_event_t *ev;
    iv_event_t *next;
    int i;

    if (r == NULL)
        return;

    flush_zombies(r);

    for (ev = r->ev_head; ev != NULL; ev = next) {
        next = ev->next;
        free(ev);
    }
    r->ev_head = NULL;

    for (i = 0; i < r->heap_n; i++) {
        free(r->heap[i]);
        r->heap[i] = NULL;
    }
    r->heap_n = 0;
    r->canceled_n = 0;

    /* 只关自己创建的 fd：调用方注册的 fd 所有权始终归调用方 */
    if (r->tfd >= 0)
        (void)close(r->tfd);
    if (r->sfd >= 0)
        (void)close(r->sfd);
    if (r->epfd >= 0)
        (void)close(r->epfd);

    if (r->mask_saved != 0)
        (void)sigprocmask(SIG_SETMASK, &r->oldmask, NULL);

    free(r->epev);
    free(r);
}

/* ---------------------------------------------------------------------------
 * fd 事件
 * ------------------------------------------------------------------------- */

iv_event_t *iv_reactor_add(iv_reactor_t *r, int fd, uint32_t events,
                           iv_event_fn cb, void *arg)
{
    iv_event_t *ev;
    struct epoll_event ee;

    if (r == NULL || fd < 0 || cb == NULL) {
        IV_LOG_E(REACTOR_MOD, "add: invalid argument");
        return NULL;
    }
    if ((events & (IV_EV_READ | IV_EV_WRITE)) == 0u) {
        /* 只给 ONESHOT 之类的位没有意义：fd 注册了也永远不会被上报 */
        IV_LOG_E(REACTOR_MOD, "add: no readable/writable bit, fd=%d", fd);
        return NULL;
    }

    ev = (iv_event_t *)calloc(1, sizeof(*ev));
    if (ev == NULL) {
        IV_LOG_E(REACTOR_MOD, "add: out of memory");
        return NULL;
    }
    ev->fd = fd;
    ev->events = events;
    ev->cb = cb;
    ev->arg = arg;
    ev->owner = r; /* 归属登记：mod/del 据此拒绝别的 reactor 拿句柄越权操作 */

    memset(&ee, 0, sizeof(ee));
    ee.events = to_epoll(events);
    ee.data.ptr = ev;
    if (epoll_ctl(r->epfd, EPOLL_CTL_ADD, fd, &ee) != 0) {
        /* 重复注册同一 fd 时这里就是 EEXIST */
        IV_LOG_E(REACTOR_MOD, "epoll_ctl(ADD) failed, fd=%d errno=%d", fd, errno);
        free(ev);
        return NULL;
    }

    ev->active = 1;
    ev_list_push(r, ev);
    return ev;
}

int iv_reactor_mod(iv_reactor_t *r, iv_event_t *ev, uint32_t events)
{
    struct epoll_event ee;

    if (r == NULL || ev == NULL)
        return IV_EINVAL;
    if (ev->active == 0 || ev->owner != r)
        return IV_EINVAL; /* 句柄已注销或不属于本 reactor */
    if ((events & (IV_EV_READ | IV_EV_WRITE)) == 0u)
        return IV_EINVAL;

    memset(&ee, 0, sizeof(ee));
    ee.events = to_epoll(events);
    ee.data.ptr = ev;
    if (epoll_ctl(r->epfd, EPOLL_CTL_MOD, ev->fd, &ee) != 0) {
        IV_LOG_E(REACTOR_MOD, "epoll_ctl(MOD) failed, fd=%d errno=%d", ev->fd, errno);
        return IV_EFAIL;
    }
    ev->events = events; /* oneshot 位同在此处更新；EPOLL_CTL_MOD 会重新装定 oneshot */
    return IV_OK;
}

int iv_reactor_del(iv_reactor_t *r, iv_event_t *ev)
{
    if (r == NULL || ev == NULL)
        return IV_EINVAL;
    if (ev->active == 0 || ev->owner != r)
        return IV_EINVAL; /* 句柄已注销或不属于本 reactor */

    if (epoll_ctl(r->epfd, EPOLL_CTL_DEL, ev->fd, NULL) != 0) {
        /* fd 已被调用方自己关闭时会拿到 EBADF/ENOENT：不阻断注销与回收 */
        IV_LOG_D(REACTOR_MOD, "epoll_ctl(DEL) fd=%d errno=%d", ev->fd, errno);
    }
    ev->active = 0;
    ev_list_unlink(r, ev);

    /*
     * 分发批次内只摘链不释放：同一批里其他事件仍可能引用它，
     * 立即 free 会让分发循环读到已释放内存。整批结束后由 flush_zombies 统一释放。
     */
    if (r->dispatching > 0) {
        ev->znext = r->zombies;
        r->zombies = ev;
    } else {
        free(ev);
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 定时器
 * ------------------------------------------------------------------------- */

iv_timer_t *iv_timer_add(iv_reactor_t *r, uint32_t timeout_ms,
                         iv_timer_fn cb, void *arg)
{
    iv_timer_t *t;

    if (r == NULL || cb == NULL)
        return NULL;
    if (r->heap_n >= IV_REACTOR_TIMER_MAX) {
        /* 有界不扩容：与 taskpool "满则报忙"一致，避免运行期内存无界增长 */
        IV_LOG_W(REACTOR_MOD, "timer table full (%d), add rejected", IV_REACTOR_TIMER_MAX);
        return NULL;
    }

    t = (iv_timer_t *)calloc(1, sizeof(*t));
    if (t == NULL) {
        IV_LOG_E(REACTOR_MOD, "timer add: out of memory");
        return NULL;
    }
    t->deadline_ms = now_ms() + (uint64_t)timeout_ms;
    t->cb = cb;
    t->arg = arg;
    t->owner = r; /* 归属登记：cancel 据此拒绝别的 reactor 拿句柄越权操作 */

    r->heap[r->heap_n] = t;
    r->heap_n++;
    heap_swim(r, r->heap_n - 1);
    return t;
}

int iv_timer_cancel(iv_reactor_t *r, iv_timer_t *t)
{
    if (r == NULL || t == NULL)
        return IV_EINVAL;
    if (t->owner != r)
        return IV_EINVAL; /* 句柄不属于本 reactor（先于 canceled 检查） */
    if (t->canceled != 0)
        return IV_ENOENT; /* 窗口内的重复取消：尽力而为的防御，不构成承诺 */

    /*
     * 只标记不释放：节点还在堆里，提前 free 会留下悬空指针。
     * 回收由 reap_timers()（到期弹出）或 compact_canceled()（批末压缩）完成，
     * 两条路径都保证没有任何定时器正在回调中，因此对"回调里取消自己"同样安全。
     */
    t->canceled = 1;
    r->canceled_n++;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 主循环
 * ------------------------------------------------------------------------- */

static void dispatch_batch(iv_reactor_t *r, int n)
{
    int i;

    r->dispatching++;
    for (i = 0; i < n; i++) {
        iv_event_t *ev = (iv_event_t *)r->epev[i].data.ptr;

        if (ev == NULL || ev->active == 0)
            continue; /* 已被前面的回调注销：句柄是 zombie，内容仍可安全读取 */
        ev->cb(ev->fd, to_iv((uint32_t)r->epev[i].events), ev->arg);
    }
    r->dispatching--;
    flush_zombies(r);
}

int iv_reactor_run(iv_reactor_t *r)
{
    if (r == NULL)
        return IV_EINVAL;

    r->running = 1;
    while (r->running) {
        int timeout = next_timeout_ms(r);
        int n;

        if (arm_timer(r) != IV_OK)
            return IV_EFAIL; /* 内部唤醒源失效，不能静默退化成 1s 轮询 */
        n = epoll_wait(r->epfd, r->epev, r->max_events, timeout);

        /*
         * 每轮（含超时返回的空轮）递增进度号，S6 健康线程据此判断主循环是否
         * 真实前进。刻意放在错误分支之前：被信号打断（EINTR）同样说明主循环在跑。
         * 跨线程读取侧走 __atomic_load_n（见 iv_reactor_progress），写侧同样
         * 用 __atomic 内建：裸读写会被 TSan 判成数据竞争。
         */
        (void)__atomic_add_fetch(&r->progress, 1u, __ATOMIC_RELAXED);

        if (n < 0) {
            if (errno == EINTR)
                continue; /* 非本 reactor 关心的信号（如 SIGUSR1）打断，继续等 */
            IV_LOG_E(REACTOR_MOD, "epoll_wait failed, errno=%d", errno);
            return IV_EFAIL;
        }
        if (n > 0)
            dispatch_batch(r, n);
    }
    return IV_OK;
}

void iv_reactor_stop(iv_reactor_t *r)
{
    if (r != NULL)
        r->running = 0;
}

uint32_t iv_reactor_progress(const iv_reactor_t *r)
{
    if (r == NULL)
        return 0u;
    return (uint32_t)__atomic_load_n(&r->progress, __ATOMIC_RELAXED);
}

int iv_reactor_timer_count(const iv_reactor_t *r)
{
    return (r != NULL) ? r->heap_n : 0;
}
