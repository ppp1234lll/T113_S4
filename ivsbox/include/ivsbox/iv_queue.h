/*
 * 持久上报队列（libivmodules，功能开发计划 M3-S3.4）
 *
 * ============================ 它是什么 ============================
 * 断线期间"待上报数据"的**落盘有界队列**：平台连不上时把要发的帧先写盘，
 * 恢复后按序补传、确认后删除（架构 §7.3「断线数据存入有容量限制的持久队列，
 * 恢复后按序补传」；落点 §10.3 `/mnt/UDISK/ivsbox/queue/`）。
 *
 *   ── 四级优先级（架构 §7.3「门禁/控制 > 告警 > 状态 > 媒体」）──
 *     0 CTL   门禁/控制   （最重要，最后被丢）
 *     1 ALARM 告警
 *     2 STATE 状态
 *     3 MEDIA 媒体        （最不重要，最先被丢）
 *   优先级只用于**超限淘汰**与装配层的**冻结**判定；**补传顺序一律按序号**
 *   （先来先发），因为协议侧按序校验序号连续性。
 *
 *   ── 存储形态（计划 §S3.4「按序号分段文件 ＋ 内存索引」）──
 *     盘上：一条一个段文件 `<dir>/<16位十进制序号>.q`，文件头 24 字节
 *           （magic + len + seq + prio），随后是原始载荷字节。
 *     内存：`iv_queue_t.idx[]` 只存元数据（seq/len/prio），按 seq 升序。
 *     ⇒ 内存占用与 max_bytes 无关（定长索引），载荷全在盘上按需读回。
 *
 * ============================ 职责边界 ============================
 * **不碰协议、不碰传输**：本层只按字节入队/出队；"发哪条、什么时候发、发成功没"
 * 全归装配层（S3.3 状态机与平台传输层）。失败语义＝"至少一次"：只有显式
 * `iv_queue_ack()` 才删除条目；崩在补传中途，条目仍在，恢复后重传（可能重复，
 * 由平台侧幂等/序号吸收）。
 *
 * ============================ 并发与生命周期 ============================
 *   - 单写者：全部接口只在装配层（Reactor）单线程内调用，内部无锁；
 *   - 零 malloc：`iv_queue_t` 由调用方持有（约 6.5 KB，主要为定长索引＋读回缓冲）；
 *   - `iv_queue_peek()` 返回的载荷指针指向内部读回缓冲，**下次 peek/close 后失效**；
 *   - 打开即恢复：`iv_queue_open()` 扫描目录重建索引；损坏/半截段文件被丢弃并计数。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK / IV_EINVAL（参数）/ IV_ERANGE（单条越界）/ IV_ESTATE（未打开）/
 *   IV_EAGAIN（队列空，peek）/ IV_ENOENT（ack 找不到序号）/
 *   IV_EFULL（满且新条目优先级不高于库内最低级 ⇒ 丢弃新条目）/ IV_EIO（落盘失败）。
 */
#ifndef IVSBOX_IV_QUEUE_H
#define IVSBOX_IV_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量与默认值
 * ------------------------------------------------------------------------- */

/* 落盘根目录（架构 §10.3；init 脚本已预建 config/db/queue/ota/secure/releases） */
#define IV_QUEUE_DIR_DEFAULT "/mnt/UDISK/ivsbox/queue"

/* 单条载荷上限（覆盖 ## 帧最大 ~1032 字节，留余量） */
#define IV_QUEUE_ITEM_MAX 2048u
/* 内存索引条目上限（即最多同时排队多少条） */
#define IV_QUEUE_ITEMS_MAX 256u
/* 目录路径缓冲上限 */
#define IV_QUEUE_PATH_MAX 192u

/* 容量默认上限（计划 §7.3「设置字节和条目上限」；配置可覆盖） */
#define IV_QUEUE_BYTES_DEFAULT (256u * 1024u)
#define IV_QUEUE_ITEMS_DEFAULT 256u

/* 优先级：0 最高（架构 §7.3） */
typedef enum {
    IV_QUEUE_PRIO_CTL   = 0, /* 门禁/控制 */
    IV_QUEUE_PRIO_ALARM = 1, /* 告警 */
    IV_QUEUE_PRIO_STATE = 2, /* 状态 */
    IV_QUEUE_PRIO_MEDIA = 3, /* 媒体 */
    IV_QUEUE_PRIO_N     = 4  /* 级数 */
} iv_queue_prio_t;

/* ---------------------------------------------------------------------------
 * 配置与句柄
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *dir;       /* NULL → IV_QUEUE_DIR_DEFAULT */
    uint32_t    max_bytes; /* 0 → IV_QUEUE_BYTES_DEFAULT（总载荷字节上限） */
    uint16_t    max_items; /* 0 → IV_QUEUE_ITEMS_DEFAULT（条目上限） */
} iv_queue_cfg_t;

/* 索引条目（内存，仅元数据；载荷在盘上） */
typedef struct {
    uint64_t seq;
    uint32_t len;
    uint8_t  prio;
} iv_queue_idx_t;

typedef struct {
    iv_queue_idx_t idx[IV_QUEUE_ITEMS_MAX]; /* 按 seq 升序 */
    uint16_t       n;                       /* 当前条目数 */
    uint64_t       bytes;                   /* 当前载荷总字节 */
    uint64_t       next_seq;                /* 下一个分配序号（从 1 起） */
    uint64_t       dropped[IV_QUEUE_PRIO_N];/* 各级被丢弃条目计数 */
    uint64_t       corrupt;                 /* open 时丢弃的损坏段文件计数 */
    uint64_t       pushes;                  /* 累计入队成功数 */
    uint64_t       acks;                    /* 累计确认删除数 */
    uint32_t       max_bytes;
    uint16_t       max_items;
    uint8_t        opened;
    uint8_t        rbuf[IV_QUEUE_ITEM_MAX]; /* peek 读回缓冲 */
    char           dir[IV_QUEUE_PATH_MAX];
} iv_queue_t;

/* 填默认配置（dir=默认目录，上限=默认上限） */
void iv_queue_cfg_default(iv_queue_cfg_t *c);

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

/* 清零句柄（不碰盘）。使用前必须先 init（或 `= {0}`），否则句柄状态不可判定。 */
void iv_queue_init(iv_queue_t *q);

/*
 * 打开并恢复：递归创建目录、扫描已有段文件重建内存索引（按 seq 升序）、
 * 丢弃损坏/半截段文件与 `.tmp` 残留（计入 corrupt）。
 * 未 close 再次 open 返回 IV_ESTATE；目录不可用返回 IV_EIO。
 * 每次 open 重置统计（pushes/acks/dropped/corrupt 从头计）。
 */
int iv_queue_open(iv_queue_t *q, const iv_queue_cfg_t *c);

/* 关闭：清内存状态（数据仍在盘上，下次 open 恢复）。未打开返回 IV_ESTATE。 */
int iv_queue_close(iv_queue_t *q);

/* ---------------------------------------------------------------------------
 * 入队 / 出队
 * ------------------------------------------------------------------------- */

/*
 * 入队：分配下一个序号、落盘（tmp → fsync → rename → fsync(dir)）、入索引。
 * 成功时 *seq_out 回填分配到的序号（>=1），返回 IV_OK。
 * 容量满时**按优先级腾位**：反复丢弃"库内最低优先级（同级取最老）"条目，
 * 被丢条目计入 dropped[其优先级]；若库内最低级都比新条目重要（其优先级 < 新条目），
 * 则改丢**新条目**并返回 IV_EFULL（同样计入 dropped[新优先级]）。
 *   IV_EINVAL 参数错（q/data/seq_out 为 NULL、prio 越界）
 *   IV_ERANGE len == 0、len > IV_QUEUE_ITEM_MAX 或 len > max_bytes（单条放不下）
 *   IV_ESTATE 未 open  ; IV_EFULL 丢弃新条目 ; IV_EIO 落盘失败
 */
int iv_queue_push(iv_queue_t *q, uint8_t prio, const void *data, size_t len,
                  uint64_t *seq_out);

/*
 * 取**序号最小**的条目（补传顺序＝序号）。seq、data、len 三个出参任一可为 NULL。
 * *data 指向内部读回缓冲，下次 peek/close/ack 后失效。队列空返回 IV_EAGAIN。
 * 若该条目段文件已损坏/丢失（外部删除、掉电半截），自动丢弃并计数、继续取下一条。
 */
int iv_queue_peek(iv_queue_t *q, uint64_t *seq, const void **data, size_t *len);

/*
 * 补传确认：删除该序号段文件并从索引移除。未找到返回 IV_ENOENT。
 */
int iv_queue_ack(iv_queue_t *q, uint64_t seq);

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */

size_t   iv_queue_count(const iv_queue_t *q);   /* 当前条目数 */
uint64_t iv_queue_bytes(const iv_queue_t *q);   /* 当前载荷总字节 */
uint64_t iv_queue_next_seq(const iv_queue_t *q);/* 下一条要补传的序号，0=空 */
uint64_t iv_queue_dropped(const iv_queue_t *q, uint8_t prio); /* 该级累计丢弃数 */
uint64_t iv_queue_corrupt(const iv_queue_t *q); /* 恢复期丢弃的损坏段文件数 */
uint64_t iv_queue_pushes(const iv_queue_t *q);  /* 累计入队成功数 */
uint64_t iv_queue_acks(const iv_queue_t *q);    /* 累计确认删除数 */

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_QUEUE_H */
