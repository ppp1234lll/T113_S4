/*
 * 采集板 UART 可靠层（libivmodules，功能开发计划 M2-S2.3）
 *
 * ============================ 它是什么 ============================
 * 在 iv_frame（S2.2 成帧）之上的**命令级可靠收发**：
 *   - 下发命令（下行帧）→ 等同 cmd 的应答帧（上行帧）→ 应答到达即完成；
 *   - 查询/配置应答超时可有限重传；0xD1/0xD2 控制只发一次；
 *   - 无配对事务的上行帧（0xC1 箱门 / 0xC2 事件 / 意外应答）原样交上层；
 *   - 维护**链路状态**：连续失败判 DOWN、收到任意合法帧判 UP（含回调），
 *     供装配层在链路恢复时发 0xE1 全量查询重建状态镜像（对接 S2.6）。
 *
 * **它不读不写串口**：要发的字节通过 on_tx 回调交出（调用方写 fd）；
 * 收到的字节由调用方喂 iv_link_recv()。单测用构造字节流驱动，零硬件。
 *
 * ============================ 协议事实与设计取舍（架构 §18.2） ============================
 * 采集板协议**没有**请求号 / ACK 帧 / 事务号 / 重传语义 —— 应答就是
 * "同 cmd 的上行帧"。因此按计划 §S2.3 的既定预案走**"命令级确认"**路线：
 *   1. **配对规则＝cmd 同号**：在飞事务按 cmd 唯一（同 cmd 第二笔 `IV_EBUSY`）。
 *      串口命令本是慢速串行交互，这不是限制而是协议事实的忠实映射。
 *   2. **控制命令不自动重传**：超时 ≠ 对端没执行（可能只是应答丢了）。
 *      0xD1/0xD2 一律只发一次；超时后上层应先查询状态或人工确认，再决定
 *      是否重新下发。无事务号时不能保证恰好执行一次。
 *   3. **不做内容去重**：无序列号 ⇒ 无法可靠区分"重复帧"与"真实重复事件"
 *      （如短时间内两次相同的门磁事件）。重复帧一律交上层，由 S2.6 镜像的
 *      内容比较天然幂等。计划原文"重复帧被去重"属于"有请求号"分支，本协议
 *      不适用（§S2.3 已按冻结协议选另一分支）。
 *
 * ============================ 并发与生命周期 ============================
 *   - 单写者：全部接口只在装配层的一个线程内调用（与 Reactor 单线程模型
 *     一致），内部无锁；
 *   - on_done 的 data 指向内部暂存缓冲，**回调返回后失效**，需留存须拷贝；
 *   - on_done 在事务槽**摘除之后**调用：回调里再 send() / clear() 不会踩
 *     正在使用的槽（与 iv_taskpool "on_done 锁外调用"同一纪律）；
 *   - 零 malloc：iv_link_t 由调用方持有（约 5.5 KB，主要为 tx/stage 缓冲）。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   iv_link_send：
 *     IV_OK      完整帧已被发送队列接受（应答结果最终看 on_done）
 *     负码       发送队列拒绝首发，事务槽已释放，不调用 on_done
 *     IV_EINVAL  参数非法
 *     IV_ERANGE  len > IV_FRAME_DATA_MAX
 *     IV_EBUSY   同 cmd 事务在飞
 *     IV_EFULL   在飞表满（不同 cmd 共 IV_LINK_INFLIGHT_MAX 个）
 *   on_done 收到的 rc：IV_OK / IV_ETIMEDOUT（重传耗尽）/ IV_ECANCELED（clear）
 */
#ifndef IVSBOX_IV_LINK_H
#define IVSBOX_IV_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_frame.h" /* 常量与 IV_FRAME_DATA_MAX / IV_FRAME_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/* 在飞事务上限：不同 cmd 并发（0xE1 轮询 + 0xF1 配置 + 0xD1 电源 + 0xD2 重启） */
#define IV_LINK_INFLIGHT_MAX 4

/* ---------------------------------------------------------------------------
 * 配置（编译期默认 ＋ 调用方可覆盖，风格同 iv_gps_cfg_t）
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t timeout_ms;   /* 单次应答等待；到期重传（含首次发送） */
    uint8_t  max_retries;  /* E1/F1 等非控制命令重传上限；D1/D2 始终不重传 */
    uint8_t  fail_threshold; /* 连续失败多少次判链路 DOWN */
} iv_link_cfg_t;

/* 默认：2 s 应答窗口、重传 2 次（共 3 发）、连续 3 次失败判 DOWN。
 * 串口 115200、帧均几十字节，线上传输毫秒级；2 s 窗口是给单片机组装
 * JSON 应答的余量。 */
#define IV_LINK_CFG_DEFAULT { 2000u, 2u, 3u }

/* ---------------------------------------------------------------------------
 * 回调
 * ------------------------------------------------------------------------- */

/* 接受完整帧到发送队列后返回 IV_OK；失败返回负码，不得悄悄丢帧。
 * 返回后字节缓冲即可复用，回调必须复制待异步写出的内容。 */
typedef int (*iv_link_tx_cb)(const uint8_t *bytes, size_t len, void *arg);

/*
 * 事务完成（成功或最终失败；重传过程中不调用）。
 *   rc=IV_OK 时 cmd/data/len 为应答内容（data 指向内部暂存，回调返回后失效）；
 *   rc 失败时 data=NULL、len=0。
 */
typedef void (*iv_link_done_cb)(int rc, uint8_t cmd, const uint8_t *data,
                                uint16_t len, void *arg);

/* 无配对事务的上行帧（0xC1/0xC2/意外应答），原样交上层 */
typedef void (*iv_link_upstream_cb)(uint8_t cmd, const uint8_t *data,
                                    uint16_t len, void *arg);

/* 链路状态变化（up=1 恢复：装配层应发 0xE1 全量查询重建镜像；up=0 判死） */
typedef void (*iv_link_change_cb)(int up, void *arg);

/* ---------------------------------------------------------------------------
 * 解析器（零 malloc：调用方持有）
 * ------------------------------------------------------------------------- */
typedef struct iv_link iv_link_t;

struct iv_link {
    iv_framer_t          framer;   /* S2.2 成帧（同步字＝上行 0x0F0F） */
    iv_link_cfg_t        cfg;
    iv_link_tx_cb        on_tx;
    iv_link_upstream_cb  on_upstream;
    iv_link_change_cb    on_change;
    void                *arg;
    struct {
        uint8_t  active;
        uint8_t  cmd;
        uint8_t  retries;
        uint16_t len;                 /* 请求 data 长度（重传重组帧用） */
        uint32_t deadline_ms;
        iv_link_done_cb on_done;
        void    *cb_arg;
        uint8_t  data[IV_FRAME_DATA_MAX]; /* 请求 data 原件（重传重组帧用） */
    } txn[IV_LINK_INFLIGHT_MAX];
    uint8_t  link_up;      /* 初始 0：未验证过链路，等首个合法帧或成功事务 */
    uint8_t  fail_streak;  /* 连续事务失败计数（UP 状态下累计） */
    uint8_t  stage[IV_FRAME_DATA_MAX]; /* 上行帧 data 暂存（回调内有效） */
};

/*
 * 初始化。cfg==NULL 取 IV_LINK_CFG_DEFAULT；on_tx==NULL 报 IV_EINVAL
 * （没有出口的可靠层无意义），on_upstream / on_change 可为 NULL。
 */
int iv_link_init(iv_link_t *lk, const iv_link_cfg_t *cfg,
                 iv_link_tx_cb on_tx, iv_link_upstream_cb on_upstream,
                 iv_link_change_cb on_change, void *arg);

/* 复位：清在飞（**不回调**）、清链路状态（单测/重新装配用） */
void iv_link_reset(iv_link_t *lk);

/*
 * 下发命令（构建下行帧 + 登记在飞 + on_tx 交出字节）。
 * 重传与完成由 iv_link_tick()/iv_link_recv() 驱动。
 */
int iv_link_send(iv_link_t *lk, uint8_t cmd, const void *data, uint16_t len,
                 uint32_t now_ms, iv_link_done_cb on_done, void *cb_arg);

/*
 * 喂收到的串口字节。帧处理：
 *   - 同 cmd 在飞事务存在 ⇒ 事务完成（on_done(IV_OK, ...)）；
 *   - 否则 ⇒ on_upstream 交上层。
 * 任一合法帧都会把链路置 UP（DOWN→UP 时触发 on_change(1)）。
 */
void iv_link_recv(iv_link_t *lk, const uint8_t *buf, size_t len, uint32_t now_ms);

/*
 * 周期驱动（如 Reactor 1s 定时器）：扫描在飞事务——
 *   未到期：无动作；非控制命令到期且还有额度：重传并续期；
 *   控制命令到期或重传额度用尽：on_done(IV_ETIMEDOUT, ...)；
 *   重传入队失败：on_done(发送队列错误码, ...)。
 */
void iv_link_tick(iv_link_t *lk, uint32_t now_ms);

/* 清空全部在飞事务并以 rc 回调（链路判死后装配层调用，通常 IV_ECANCELED） */
void iv_link_clear(iv_link_t *lk, int rc);

/* 当前链路状态（1=UP） */
int iv_link_is_up(const iv_link_t *lk);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_LINK_H */
