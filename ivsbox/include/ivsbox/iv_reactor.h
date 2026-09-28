/*
 * IVSBox 事件循环 Reactor（架构 §4.2「主 Reactor」，计划 M1-S4）
 *
 * 定位：`ivsboxd` 的单线程事件核心，所有 fd 事件的唯一分发中心。
 * 架构依据：
 *   - §4.2 统一处理 UART、平台 Socket、Netlink、GPS fd、IPC、timerfd、signalfd、eventfd；
 *   - §4.2 每轮递增原子进度号，供 S6 健康线程判断主循环是否真实前进；
 *   - §6/core 层承载「事件、队列、定时器」；§14 事件循环选型为 epoll/timerfd/signalfd/eventfd。
 *
 * 层级与依赖（重要，别踩）：
 *   本模块在 `libivcore.a`，按架构 §15.5 只允许依赖 libc。链接顺序是
 *   `-livmodules -livhal -livcore`，**core 排在最后**，静态库单向扫描下 core 引用
 *   libivhal 的符号会直接链接失败（`-Wl,--no-undefined` 会当场拦下）。
 *   因此本模块的时间基准**不调用 `iv_clock_monotonic_ms()`**（它在 src/hardware/，
 *   属 libivhal），而是直接使用 libc 的 `clock_gettime(CLOCK_MONOTONIC)`：
 *   这正是架构把 clock 划给 hal、又把定时器划给 core 所产生的上下层错位，
 *   本模块作为最底层自己取时是唯一不破坏依赖方向的解法。
 *   `iv_clock` 仍是**hal 及以上**各层的统一时钟纪律，本模块不改变那条纪律。
 *
 * 线程纪律（S5 慢任务池 / S6 健康线程开工前必须读）：
 *   - 除 `iv_reactor_progress()` 只读进度号外，本模块**全部接口只在 reactor 线程内调用**，
 *     内部无锁。慢任务 worker 的结果必须经 `eventfd` 回传主循环后再处理，
 *     禁止在 worker 线程里直接调用本模块任何写接口。
 *   - 信号掩码由 `iv_reactor_create()` 设置，必须在**创建任何线程之前**调用
 *     （线程掩码创建时继承），否则子线程会漏掉屏蔽、被 SIGPIPE 打死。
 *
 * 回调纪律（架构 §4.2，本模块不提供强制手段）：
 *   单次回调只做解析、状态更新与分发；禁止 DNS、同步 SNMP/ONVIF、长文件 IO
 *   等不可控阻塞——那些必须走 S5 慢任务池。卡死的回调会让进度号停走，
 *   由 S6 健康线程停喂看门狗，这是设计意图而非缺陷。
 */
#ifndef IVSBOX_IV_REACTOR_H
#define IVSBOX_IV_REACTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 事件位（自有命名空间，调用方不需要包含 <sys/epoll.h>）
 *   - 注册用：IV_EV_READ / IV_EV_WRITE / IV_EV_ONESHOT
 *   - 回调可见：上述 read/write，外加 epoll 无条件上报的 ERR/HUP/RDHUP
 */
#define IV_EV_READ    ((uint32_t)0x0001u)
#define IV_EV_WRITE   ((uint32_t)0x0002u)
#define IV_EV_ONESHOT ((uint32_t)0x0004u) /* 触发一次后自动失活，需 mod 重新激活 */
#define IV_EV_ERR     ((uint32_t)0x0100u) /* 回调可见：fd 出错 */
#define IV_EV_HUP     ((uint32_t)0x0200u) /* 回调可见：挂断 */
#define IV_EV_RDHUP   ((uint32_t)0x0400u) /* 回调可见：对端半关闭（仅流式 fd） */

/* 注册 IV_EV_READ 时会自动附加 RDHUP 监听；ERR/HUP 由 epoll 无条件上报 */
typedef void (*iv_event_fn)(int fd, uint32_t events, void *arg);
typedef void (*iv_timer_fn)(void *arg);

typedef struct iv_event   iv_event_t;
typedef struct iv_timer   iv_timer_t;
typedef struct iv_reactor iv_reactor_t;

/* 默认/上限常量 */
#define IV_REACTOR_MAX_EVENTS_DEFAULT 64  /* epoll_wait 单轮最大回传事件数 */
#define IV_REACTOR_TIMER_MAX          256 /* 定时器上限：满则 add 返回 NULL，不扩容不排队 */
#define IV_REACTOR_IDLE_TIMEOUT_MS    1000 /* 空闲兜底：保证进度号至少每秒走一次 */

/* ---------------------------------------------------------------------------
 * Reactor 生命周期
 * ------------------------------------------------------------------------- */

/* 创建：epoll + timerfd + signalfd 一并建立并纳入分发。
 * max_events <= 0 时取 IV_REACTOR_MAX_EVENTS_DEFAULT。
 * 副作用：阻塞 SIGTERM / SIGINT / SIGPIPE（destroy 时恢复原掩码），
 *         因此必须在创建任何线程之前调用。失败返回 NULL。 */
iv_reactor_t *iv_reactor_create(int max_events);

/* 销毁：释放全部 event/timer，关闭自身创建的 epoll/timerfd/signalfd，恢复信号掩码。
 * **不关闭调用方注册的 fd**（fd 所有权始终归调用方）。
 * 只允许在 `iv_reactor_run()` 返回之后调用。 */
void iv_reactor_destroy(iv_reactor_t *r);

/* 主循环：跑到 stop 或信号触发为止。正常返回 IV_OK，epoll 失败返回 IV_EFAIL，
 * r 为 NULL 返回 IV_EINVAL。被信号打断（EINTR）不算错误，继续等待。 */
int iv_reactor_run(iv_reactor_t *r);

/* 请求退出：只清 running 标志，让 run 自然返回；可在任意回调内安全调用。 */
void iv_reactor_stop(iv_reactor_t *r);

/* 进度号：每轮循环（含超时返回的空轮）递增 1。
 * **唯一允许跨线程调用的接口**。读侧判断"是否前进"必须用差值语义：
 *     if ((uint32_t)(now - last) != 0) { ... }   // 不能写 now > last
 * 类型刻意用 uint32_t：armv7 上对齐的 32 位读写天然原子，64 位会被拆成两条
 * 指令产生撕裂值，`_Atomic uint64_t` 又会引入 -latomic。回绕周期约 49 天
 * （按每秒 1 轮计），差值语义在回绕点依然正确。 */
uint32_t iv_reactor_progress(const iv_reactor_t *r);

/* 诊断用只读计数：当前堆内定时器数量（含已取消待回收的） */
int iv_reactor_timer_count(const iv_reactor_t *r);

/* ---------------------------------------------------------------------------
 * fd 事件
 * ------------------------------------------------------------------------- */

/* 注册。成功返回句柄，失败返回 NULL（具体原因已由 iv_log 记录，不扩错误码段）。
 * 重复注册同一个 fd 会失败（epoll EEXIST）。
 * **必须用返回的句柄做后续的 mod / del**：以 fd 为键会让 reactor 额外维护一张
 * fd→event 索引表，得不偿失。 */
iv_event_t *iv_reactor_add(iv_reactor_t *r, int fd, uint32_t events,
                           iv_event_fn cb, void *arg);

/* 修改关注事件；对 oneshot 事件同时用于"重新激活"。事件不存在返回 IV_ENOENT。 */
int iv_reactor_mod(iv_reactor_t *r, iv_event_t *ev, uint32_t events);

/* 注销并释放事件句柄。回调内注销自己或注销同一批里的其他事件都是安全的：
 * 句柄的释放会延迟到本批分发结束之后，分发循环不再回调已注销的事件。 */
int iv_reactor_del(iv_reactor_t *r, iv_event_t *ev);

/* ---------------------------------------------------------------------------
 * 定时器（一次性语义）
 * ------------------------------------------------------------------------- */

/* 加入一个 timeout_ms 后到期的一次性定时器。到期回调执行完毕即自动释放。
 * 需要周期触发就在回调里重新 add（回调内重挂是支持的常态用法）。
 * 堆满（IV_REACTOR_TIMER_MAX）或参数非法返回 NULL。 */
iv_timer_t *iv_timer_add(iv_reactor_t *r, uint32_t timeout_ms,
                         iv_timer_fn cb, void *arg);

/* 取消：惰性标记，保证回调不再触发；内存由 reactor 在到期弹出或销毁时回收。
 * 在定时器自己的回调里取消自己也是安全的（本次回调跑完才释放）。
 * 重复取消返回 IV_ENOENT。 */
int iv_timer_cancel(iv_reactor_t *r, iv_timer_t *t);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_REACTOR_H */
