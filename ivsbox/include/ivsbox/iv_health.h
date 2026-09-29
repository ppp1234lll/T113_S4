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
 *   例外是正在执行、且尚未越过 req.timeout_ms 端到端截止时间的 worker：该绝对
 *   deadline 是健康租约，允许 DNS/SNMP/ONVIF 这类单次阻塞调用在声明预算内不发
 *   主动心跳。租约到期后恢复 stuck_ms 判定；timeout_ms 必须按真实最坏耗时加合理
 *   余量填写，过大会等比例推迟卡死检出，不能用大值掩盖故障；timeout_ms=0 的长
 *   任务仍须周期调用 iv_task_heartbeat()，不能用无限租约掩盖永久阻塞。
 *
 * 线程与信号：
 *   本模块**不注册任何信号处理**。装配顺序（S10）为
 *     iv_taskpool_create() → iv_reactor_create() → iv_health_start() → iv_reactor_run()
 *   taskpool 已在建线程前阻塞 SIGTERM/SIGINT/SIGPIPE 且不恢复，健康线程自然
 *   继承该掩码；而 iv_reactor_create() 自己也要求"必须在创建任何线程之前"调用
 *   （它同时阻塞同一组信号，所以只能排在 health_start 之前）。因此这里不需要
 *   （也不应该）再动信号。
 *   **注意 reactor 必须先于本模块启动**：iv_health_start() 的首参就是 reactor 指针，
 *   且启动后不可更换；顺序写反（先 health 后 reactor）就只能传 NULL，而 NULL 等于
 *   **静默放弃"主循环卡死"这条判据**（没有事后 attach 接口）。计划 §S6 曾把顺序写成
 *   `taskpool → health → reactor`，与 §S10 自相矛盾，已于 2026-09-29 按本节口径更正。
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

/* 判据窗口与检查节奏的默认值。**这两个值加上 iv_watchdog 的默认超时必须满足
 * stuck_ms + interval_ms < 看门狗超时**，否则门控形同虚设；该不变量由
 * src/modules/iv_health.c 顶部的 #if + #error 在编译期钉死。*/
#define IV_HEALTH_STUCK_MS_DEFAULT     10000u /* 连续 10s 无进度 → 判死 */
#define IV_HEALTH_INTERVAL_MS_DEFAULT  1000u  /* 每秒检查一次 */
#define IV_HEALTH_WORKERS_MAX          8      /* == IV_TASKPOOL_WORKERS_MAX */

/* 连续喂狗失败达到该次数 → fail-stop：关闭 watchdog fd、此后不再尝试喂狗（S6-01）。
 * 喂狗失败（含被信号打断的 EINTR，同样按失败计数）只打日志不 fail-stop 的话，
 * "fd 一直在但一次都写不进去"会静默耗完看门狗窗口，快照却还显示一切正常。*/
#define IV_HEALTH_WD_FAIL_MAX 3u

/* 故障快照。由 iv_health_snapshot() 加锁拷出，读侧拿到的是副本。 */
typedef struct iv_health_snapshot {
    uint32_t reactor_progress;                   /* 最近一次检查时的进度号（未观察则为 0） */
    uint32_t worker_progress[IV_HEALTH_WORKERS_MAX];
    int      worker_count;                       /* 实际观察的 worker 数 */
    int      fault_code;                         /* 上列故障码（只反映进度判定，不含 wd） */
    uint32_t tick_count;                         /* 已完成的检查轮数 */
    int      watchdog_fd;                        /* < 0 表示未喂 / 已停喂 / 已 fail-stop */
    uint32_t wd_fail_streak;                     /* 连续喂狗失败次数（S6-01）；成功一次即清零 */
    int      wd_io_fault;                        /* 1 = 喂狗 I/O 已失败过且未恢复；从未失败为 0 */
} iv_health_snapshot_t;

typedef struct iv_health iv_health_t;

/* 启动健康线程，成功立即返回（线程已在后台跑）。
 *   reactor / pool —— 观察对象，可传 NULL 表示不观察该项；两者都 NULL 则恒判健康
 *                     （纯逻辑单测用）。
 *                     **必须在 iv_taskpool_create() / iv_reactor_create() 成功之后
 *                     再调本函数，并把真实指针传进来**：两个指针都是"启动时捕获、
 *                     之后不可更换"，给 NULL 就是**永久放弃该项判据**（没有事后
 *                     attach 接口）。放弃 reactor 判据尤其危险 —— 会静默失去
 *                     "主循环卡死 → 停喂 → 复位"这条门控，而运行期看不出异常。
 *   watchdog_fd    —— < 0 表示"不真喂狗"（只跑判据）；也可传 pipe 写端，
 *                     从而在测试里观察每一次喂狗与停喂。
 *   stuck_ms       —— 传 0 取 IV_HEALTH_STUCK_MS_DEFAULT。
 *   interval_ms    —— 传 0 取 IV_HEALTH_INTERVAL_MS_DEFAULT。
 *                     **自定义值受运行期不变量约束**：watchdog_fd >= 0（真喂狗）时，
 *                     stuck_ms + interval_ms 必须严格小于看门狗超时（对齐文件顶部
 *                     编译期 #error 的口径），违反则**启动即失败**返回 NULL ——
 *                     宁可拒绝启动，不让"判据还没判死、狗就先咬了"静默发生。
 * 失败（内存、pthread_create、或上述不变量不满足）返回 NULL。 */
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
 *
 * **调用顺序（硬约束）**：必须在销毁 reactor / taskpool **之前**调用本函数。
 *   健康线程在停止前每一轮（以及判死路径的 report_fault / fill_snapshot）都会
 *   解引用这两个指针，顺序反了就是 use-after-free。正确收尾顺序：
 *     iv_health_destroy(h) → iv_taskpool_destroy(pool) → iv_reactor_destroy(r)
 *
 * **join 期间不要动 watchdog fd**：join 最多等一个 interval（默认 1s），这期间
 *   线程可能刚好睡醒、还会再喂一次狗（循环体在 usleep 之后才复查停止标志）；若
 *   调用方在此窗口内 close 了该 fd，而 fd 号已被别的 open 复用，就等于每秒向无关
 *   对象注入 0x00 字节。要关就在本函数**返回之后**关。
 *
 * fd **不**由本函数关闭（所有权始终在调用方）。但若已判死，或喂狗连续失败达到
 *   IV_HEALTH_WD_FAIL_MAX 触发 fail-stop（S6-01），线程自己已 close 过并把内部
 *   记录置为 -1；所以"还要不要我再 close"的安全判据是快照里的
 *   `watchdog_fd < 0`（见 iv_health_snapshot_t）—— 已经是负数就别再 close，
 *   否则 double-close 可能误关一个被复用的 fd。
 *
 * 传 NULL 安全；句柄 destroy 后立即失效（内部已 free），**不得重复调用**。 */
void iv_health_destroy(iv_health_t *h);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_HEALTH_H */
