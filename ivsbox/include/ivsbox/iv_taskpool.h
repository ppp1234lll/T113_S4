/*
 * IVSBox 慢任务池（架构 §4.2「慢任务池」，计划 M1-S5）
 *
 * 定位：`ivsboxd` 里承接**一切不可控阻塞操作**的固定线程池。架构 §4.2 规定主
 * Reactor 的单次回调不得做 DNS、同步 SNMP/ONVIF、长文件 IO，这些必须挪到本池。
 * 池子固定 2 个 worker、队列定长，满则立刻返回忙——**不允许无限排队，也不允许
 * 临时创建线程**。
 *
 * 层级与依赖（重要，别踩）：
 *   本模块在 `libivmodules.a`，**不在 `libivcore.a`**。原因是它必须使用 pthread，
 *   而 core 的纪律是「仅 libc」——S4 的 iv_reactor 正是靠 sigprocmask 替掉
 *   pthread_sigmask 才守住那条纪律的。放进 modules 后：
 *     - 架构 §15.5 的依赖方向表（core → hal → modules → app）无需改动；
 *     - 本模块可以正大光明地调 `iv_clock_monotonic_ms()`（libivhal，链接顺序
 *       `-livmodules -livhal -livcore` 里 modules 在前，引用 hal 是对的）。
 *   代价见下一节：链接期必须显式给 `-lpthread`（已加进两端 toolchain mk 的 LDLIBS）。
 *
 * 线程纪律：
 *   - `iv_taskpool_submit()` / `iv_taskpool_cancel()` / `iv_taskpool_process()`
 *     一律**只在 reactor 线程内调用**，与 iv_reactor 的调用纪律一致。
 *   - 任务函数 `fn` 在 **worker 线程** 内执行：只允许用 `ctx->arg` 做纯计算 /
 *     阻塞 IO，**禁止**调用 iv_reactor 的任何写接口、**禁止**修改全局业务状态。
 *     需要回传结果就写 `ctx->result`，真正"落到业务状态"的动作由完成回调在
 *     reactor 线程里做。
 *   - `iv_taskpool_worker_progress()` / `iv_taskpool_pending()` 允许跨线程调用。
 *   - 本模块创建线程前会阻塞 SIGTERM / SIGINT / SIGPIPE 并**不恢复**掩码，
 *     这样无论它摆在 iv_reactor_create() 之前还是之后，worker 都继承了正确掩码。
 *     S10 装配顺序（taskpool 在 Reactor 前）依赖的正是这一点。
 *
 * 原子访问纪律（TSan 相关，别改成裸读写）：
 *   两处跨线程共享量刻意走 `__atomic_*` + `memory_order_relaxed`：
 *     - worker 进度号（worker 写 / 任何人读）；
 *     - 任务的 cancel 标志（reactor 写 / worker 读）。
 *   它们之间没有锁提供 happens-before，裸读写会被 ThreadSanitizer 判成数据竞争。
 *   说"良性"没用——`make tsan` 会照样报，噪声一多就没人看真问题了。
 *   S4 的 iv_reactor_progress 用 volatile 表达"别人会改"，本模块要过 TSan，
 *   因此升级成真正的原子访问。armv5/armv7 上 4 字节 relaxed 访问是单条 ldr，
 *   不会把 libatomic 引进来。
 *
 * 一个必须知道的调用契约：worker 完成后**不**直接释放槽位，而是把槽位置为 DONE
 * 并写 eventfd；调用方必须在 reactor 里挂 eventfd 读事件并调 `iv_taskpool_process()`
 * 取走结果。**不调 process，DONE 槽位会一直占着**，槽位耗尽后 submit 一律返回
 * IV_EBUSY —— 这是有界的失败（不会泄漏、不会崩），但会静默丢任务，装配时别漏。
 */
#ifndef IVSBOX_IV_TASKPOOL_H
#define IVSBOX_IV_TASKPOOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iv_taskpool iv_taskpool_t;

/* 默认与上限 */
#define IV_TASKPOOL_WORKERS_DEFAULT 2  /* 架构 §4.2：固定两个 worker */
#define IV_TASKPOOL_QUEUE_DEFAULT   16 /* 在飞任务上限（含正在执行的） */
#define IV_TASKPOOL_WORKERS_MAX     8  /* 误传大数保护 */
#define IV_TASKPOOL_QUEUE_MAX       64 /* 误传大数保护 */
#define IV_TASKPOOL_RESULT_MAX      512 /* 单任务结果缓冲上限（架构 §4.2「结果长度上限」） */
#define IV_TASKPOOL_NAME_MAX        24  /* 任务名会被拷贝进池内定长缓冲（含结尾符） */
#define IV_TASKPOOL_IDLE_POLL_MS    1000 /* worker 空闲轮询上限：空闲也必须推进进度号 */

/* 任务上下文：worker 线程内可见的**全部**东西都由它携带 */
typedef struct iv_task_ctx {
    void         *arg;        /* 提交时 req.arg 原样传入 */
    volatile int *cancel;     /* 协作式取消标志。**不要直接解引用**，一律用下面的
                               * iv_task_canceled(ctx) —— 那是原子读，裸读会在
                               * TSan 下报竞争。池子不会强行打断已开工的任务，
                               * 长任务必须在循环里周期检查，短任务可忽略。 */
    void         *result;     /* 结果缓冲，池内提供，容量 IV_TASKPOOL_RESULT_MAX */
    size_t        result_cap; /* == IV_TASKPOOL_RESULT_MAX */
    size_t        result_len; /* 任务函数填写实际写入长度；未用则保持 0 */
} iv_task_ctx_t;

/* 任务函数：worker 线程内执行。返回 IV_OK 表示成功，其他值原样回传给完成回调。 */
typedef int (*iv_task_fn)(iv_task_ctx_t *ctx);

/* 检查取消请求（relaxed 原子读，跨线程安全）。
 * 任务函数里请用这个，不要写 `*ctx->cancel`。 */
static inline int iv_task_canceled(const iv_task_ctx_t *ctx)
{
#if defined(__GNUC__) && !defined(__cplusplus)
    return __atomic_load_n((const int *)ctx->cancel, __ATOMIC_RELAXED) != 0;
#else
    return *ctx->cancel != 0;
#endif
}

/* 完成回调：**reactor 线程内**执行（在 iv_taskpool_process 里逐个调用）。
 * result 只在本次回调期间有效，需要留存请自行拷贝。 */
typedef void (*iv_task_done_fn)(uint64_t id, int rc, const void *result,
                                size_t result_len, void *arg);

/* 提交请求。结构体在 submit 内被整体拷贝，栈上临时变量即可。 */
typedef struct iv_task_req {
    const char      *name;       /* 诊断用短名，可为 NULL。**会被拷贝进池内的定长
                                  * 缓冲**（IV_TASKPOOL_NAME_MAX），因此指向栈上
                                  * 临时字符串也安全，无需保证长寿。 */
    iv_task_fn       fn;         /* 必填，为 NULL 则 submit 失败 */
    void            *arg;        /* 传给 fn 的上下文，可为 NULL */
    uint32_t         timeout_ms; /* 从提交时刻起的截止时间；**0 = 不限**。
                                  * 语义是「开工准入」：worker 取出时若已过期就
                                  * 不执行、直接以 IV_ETIMEDOUT 完成。 */
    iv_task_done_fn  on_done;    /* 完成回调，可为 NULL */
    void            *on_done_arg;
} iv_task_req_t;

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

/* 创建线程池。参数 <= 0 时取默认值（2 worker / 队列 16），超上限则夹到上限。
 * 副作用：阻塞 SIGTERM/SIGINT/SIGPIPE 且不恢复（理由见文件头）。失败返回 NULL。 */
iv_taskpool_t *iv_taskpool_create(int nworkers, int queue_size);

/* 销毁：置停止标志 + **对正在执行的任务置取消标志**（协作式，不打断）+
 * 唤醒全部 worker + join。
 *   - 正在执行的任务收到取消请求后应尽快返回（任务函数里查 iv_task_canceled）；
 *     完全不检查标志的任务仍会跑完 —— 这是协作式的固有边界。少了这一步，
 *     关机路径会被一个 30s 的 SNMP 超时任务干等（计划 §S5 原口径）。
 *   - 队列中尚未开工的任务被丢弃，其完成回调不会执行；
 *   - DONE 槽位里尚未被 process 取走的结果同样丢弃。
 * 必须保证此后没有线程再调本池任何接口。 */
void iv_taskpool_destroy(iv_taskpool_t *pool);

/* ---------------------------------------------------------------------------
 * 提交与取消（reactor 线程）
 * ------------------------------------------------------------------------- */

/* 提交任务。成功时 *out_id 得到本次任务的 id（从 1 起递增，绝不重复）。
 * 槽位耗尽返回 IV_EBUSY（**立即返回，不排队不等待**）；参数非法返回 IV_EINVAL。 */
int iv_taskpool_submit(iv_taskpool_t *pool, const iv_task_req_t *req, uint64_t *out_id);

/* 取消任务（协作式，不打断已开工的执行）：
 *   - 尚未开工（QUEUED）：立即释放槽位，**完成回调不会执行**，返回 IV_OK；
 *   - 已开工（RUNNING）：只置 ctx 里的 cancel 标志，任务函数何时退出由它自己
 *     决定；它返回后**完成回调仍会执行**，rc 由任务函数给出（通常是 IV_ECANCELED）；
 *   - 其余情况（已完成 / 已取走 / id 不存在）：返回 IV_ENOENT。
 * 注意 in-flight 的取消请求可能撞上 worker 刚取走任务，此时按 RUNNING 分支处理，
 * 因此返回 IV_OK 不保证"回调一定不会执行"，只保证"任务不会再开始执行"。 */
int iv_taskpool_cancel(iv_taskpool_t *pool, uint64_t id);

/* ---------------------------------------------------------------------------
 * 结果收取（reactor 线程）
 * ------------------------------------------------------------------------- */

/* 供 reactor 注册用的 eventfd（非阻塞、CLOEXEC）。worker 每完成一个任务写 +1。
 * 失败返回 -1。 */
int iv_taskpool_eventfd(const iv_taskpool_t *pool);

/* 取走已完成任务并逐个调用其 on_done。
 * 本函数会先把 eventfd 读干（电平触发必须读干，否则反复上报），再处理 DONE 槽位。
 * max <= 0 表示不限；max > 0 时限流 —— **但若处理完 max 条后仍有 DONE 残留，
 * 本函数会补写一次 eventfd 自唤醒**，保证下一轮还会被调用。因此调用方不需要
 * 自己循环取到返回 0，残留槽位也不会滞留（不补这一下，残留槽位要等到下一个
 * 任务完成才有唤醒源，之后没新任务就永远占着，直到 destroy）。
 * 返回本次回调的任务数；pool 为 NULL 返回 IV_EINVAL。
 * **只允许在 reactor 线程内调用**；on_done 里再调 submit 是安全的（回调期间不持锁）。 */
int iv_taskpool_process(iv_taskpool_t *pool, int max);

/* ---------------------------------------------------------------------------
 * 只读诊断（可跨线程，内部自有同步）
 * ------------------------------------------------------------------------- */

/* worker 进度号：每完成一个任务、或每次空闲超时轮询都 +1。
 * 读侧判断"是否前进"必须用差值语义：if ((uint32_t)(now - last) != 0) {...}
 * 理由同 iv_reactor_progress（armv7 上 32 位对齐读天然原子；空闲也推进是为了
 * 让 S6 能区分"空闲"与"死锁"）。idx 越界返回 0。 */
uint32_t iv_taskpool_worker_progress(const iv_taskpool_t *pool, int idx);

int iv_taskpool_worker_count(const iv_taskpool_t *pool);

/* 当前占用槽位数（QUEUED + RUNNING + DONE），即"在飞任务"概念上的数量。
 * 内部加锁读取，因此参数不是 const。 */
int iv_taskpool_pending(iv_taskpool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_TASKPOOL_H */
