/*
 * IVSBox 健康线程 + 看门狗门控（modules 层，功能开发计划 M1-S6）
 *
 * 定位：独立的看门狗门控线程，只回答一个问题 —— "主循环与慢任务池是不是都还
 * 活着"，并把答案变成"喂狗 / 停喂"：
 *   - 全部健康  → 每 interval_ms 调一次 iv_watchdog_keepalive()
 *   - 任一超预算 → stderr 打一次故障快照 → 关闭 watchdog fd（＝停喂）→ 整机复位
 * 它**绝不**替主循环跑业务、**绝不**重启别的线程（架构 §4.2 / 计划 §S6 明令）。
 *
 * 落点（别改错）：本模块在 `src/modules/`，进 libivmodules。
 *   - 依赖 pthread（与同层 iv_taskpool 一致，libivmodules 已带 -lpthread）；
 *   - 引用 iv_reactor（libivcore）、iv_taskpool（同层）、iv_clock_monotonic_ms()
 *     （libivhal）—— 链接顺序里 modules 在 hal/core 之前，方向合法。
 *   - **不放 `src/app/`**：app 层的 .c 不参与任何静态库，放那里
 *     `tests/unit/test_health.c` 链接不到符号（Makefile 只链三个 .a）。
 *     计划 §S6 原文写的 `src/app/health.c` 已按此更正为 `src/modules/iv_health.c`。
 *
 * 判据（配套 S4/S5 已埋的进度号）：
 *   每个被观察对象维护一对状态 ——「上次见到的进度号」+「该进度号最近一次变化的
 *   单调时刻」。进度号变了就刷新时刻；进度号没变且 (now - 变化时刻) >= stuck_ms
 *   才判死。**两个量缺一不可**：把进度号本身当时间戳用是错的（量纲不同）。
 *   首次观察只播种、不判死，否则启动瞬间必报故障。
 *   为什么"进度号不动"确实等价于"卡死"：Reactor 的 epoll_wait 与 worker 的
 *   cond_timedwait 都用有限超时，空闲期也会推进进度号 —— 这是 S4/S5 刻意埋的。
 *
 * 线程与信号：
 *   本模块**不注册任何信号处理**。装配顺序（S10）为
 *   iv_taskpool_create() → iv_health_start() → iv_reactor_create()，
 *   taskpool 已在建线程前阻塞 SIGTERM/SIGINT/SIGPIPE 且不恢复，健康线程自然
 *   继承该掩码。因此这里不需要（也不应该）再动信号。
 *
 * 日志：
 *   故障快照与喂狗失败走 **stderr**，不依赖 iv_log —— 避免"日志系统卡住"与
 *   "主循环卡住"互为因果时，把最后一份证据也一起丢掉。
 */
#ifndef IVSBOX_IV_HEALTH_H
#define IVSBOX_IV_HEALTH_H

#include <stdint.h>

#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_taskpool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 故障码（按位或）。0 表示健康。 */
#define IV_HEALTH_OK           0
#define IV_HEALTH_REACTOR_DEAD 1 /* 主循环进度号在 stuck_ms 内未前进 */
#define IV_HEALTH_WORKER_DEAD  2 /* 至少一个 worker 进度号在 stuck_ms 内未前进 */
#define IV_HEALTH_ALL_DEAD     3 /* 两者都卡（1|2） */

/* 判据窗口与检查节奏的默认值 */
#define IV_HEALTH_STUCK_MS_DEFAULT     10000u /* 连续 10s 无进度 → 判死 */
#define IV_HEALTH_INTERVAL_MS_DEFAULT  1000u  /* 每秒检查一次 */
#define IV_HEALTH_WORKERS_MAX          8      /* == IV_TASKPOOL_WORKERS_MAX */

/* 故障快照。由 iv_health_snapshot() 加锁拷出，读侧拿到的是副本。 */
typedef struct iv_health_snapshot {
    uint32_t reactor_progress;                   /* 最近一次检查时的进度号（未观察则为 0） */
    uint32_t worker_progress[IV_HEALTH_WORKERS_MAX];
    int      worker_count;                       /* 实际观察的 worker 数 */
    int      fault_code;                         /* 上列故障码 */
    uint32_t tick_count;                         /* 已完成的检查轮数 */
    int      watchdog_fd;                        /* < 0 表示未喂 / 已停喂 */
} iv_health_snapshot_t;

typedef struct iv_health iv_health_t;

/* 启动健康线程，成功立即返回（线程已在后台跑）。
 *   reactor / pool —— 观察对象，可传 NULL 表示不观察该项；两者都 NULL 则恒判健康
 *                     （纯逻辑单测用）。
 *   watchdog_fd    —— < 0 表示"不真喂狗"（只跑判据）；也可传 pipe 写端，
 *                     从而在测试里观察每一次喂狗与停喂。
 *   stuck_ms       —— 传 0 取 IV_HEALTH_STUCK_MS_DEFAULT。
 *   interval_ms    —— 传 0 取 IV_HEALTH_INTERVAL_MS_DEFAULT。
 * 失败（内存或 pthread_create）返回 NULL。 */
iv_health_t *iv_health_start(const iv_reactor_t *reactor,
                             const iv_taskpool_t *pool,
                             int watchdog_fd,
                             uint32_t stuck_ms,
                             uint32_t interval_ms);

/* 等价于 iv_health_start(reactor, pool, watchdog_fd, 0, 0) */
iv_health_t *iv_health_start_default(const iv_reactor_t *reactor,
                                     const iv_taskpool_t *pool,
                                     int watchdog_fd);

/* 加锁拷出最新快照。h / out 为 NULL 返回 IV_EINVAL。
 * h 声明为非 const：取快照要拿内部锁（对外仍是只读语义）。 */
int iv_health_snapshot(iv_health_t *h, iv_health_snapshot_t *out);

/* 收尾：置停止标志 + join（最多等一个检查周期）。
 * watchdog fd **不**由本函数关闭 —— fd 归属调用方；若已判死，线程自己已 close
 * 过并把内部记录置 -1。
 * 传 NULL 安全；句柄 destroy 后立即失效（内部已 free），**不得重复调用**。 */
void iv_health_destroy(iv_health_t *h);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_HEALTH_H */
