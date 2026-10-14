/*
 * IVSBox SD 卡存储策略（libivmodules，功能开发计划 M6-S6.5）
 *
 * ============================ 它是什么 ============================
 * 把 S6.5 的"存储水位 / 清理 / 异常检测"收口成**纯决策 ＋ 可注入 IO** 两半：
 *
 *   - `iv_store_decide()`：**纯函数**。吃"一轮巡检事实"（挂载与否 / 只读 /
 *     扫描出错 / 总容量 / 剩余 / 候选文件表）＋ 上一轮档位，吐出"该做什么"
 *     （档位、是否拒新任务、是否停录告警、该删哪些文件）。无 IO、无时间、
 *     无全局量 ⇒ 单测可以直接构造任意边界（84.9% / 85% / 90% / 95% / 降回 70%）。
 *   - `iv_storage_step()/run_once()`：按周期把 IO 与决策串起来（scan → decide →
 *     unlink），并持有跨轮的**迟滞档位**与统计。IO 走 `iv_storage_ops_t` 注入，
 *     生产用 `iv_storage_ops_default()`（POSIX），单测用假实现或真临时目录。
 *
 * ============================ 冻结口径（架构 §18.5 ＋ 计划 S6.5） ============================
 * 水位线（**上行为即时、下行为迟滞**）：
 *
 *     used% >= 85  → CLEAN  清理最旧可删（删到 free 回到 recover 线，或达单轮上限）
 *     used% >= 90  → REJECT 拒新任务（不再开新录像）
 *     used% >= 95  → STOP   停录 ＋ 告警
 *     used% <= 70  → 恢复 OK（**只有降回 70% 才解除**；70~95 之间维持原档 —— 迟滞，
 *                            否则在阈值上反复抖动会让录像忽开忽关）
 *
 * 三类保留优先级（**删的顺序**＝优先级倒序）：
 *
 *     普通录像（REC，最先删） → 告警录像（ALARM） → 抓图（SNAP，最后删）
 *
 * 同类内**最旧优先**（mtime 升序），并且：
 *   - **最近 `keep_youngest_s` 秒内的切片不删** —— 正在写的那个分片 mtime 就是"现在"，
 *     删它等于把在录的录像抽掉（POSIX 允许 unlink 打开中的文件，但那是白录）。
 *   - 单轮删除条数上限 `max_delete`（默认 32），避免一次性删爆；一轮不够，
 *     下一轮（默认 30 s 后）接着删 ⇒ 收敛而非停顿。
 *
 * 异常检测：根目录不可 statfs（SD 未挂）/ 只读挂载 / 目录扫描出错
 * → `fault=1` ⇒ **拒新任务 ＋ 停录 ＋ 告警**（此时录像根本落不下去，
 * 继续"录"只会刷错误日志）。故障恢复后随水位档位一起自然恢复。
 *
 * ============================ 与"写盘同步"的关系（计划原文） ============================
 * 计划 S6.5 写"按切片关闭同步而非每包 fsync"。本模块**不做写盘同步** ——
 * 录像的写/关/刷盘在媒体进程（`ivsbox/media`，**收编自 bird_media，一行未改**，
 * 用户 2026-10-14 指令"视频代码零自研"）里，由 FFmpeg 的分片关闭路径承担。
 * 本模块只做"读容量 / 删最旧"，**不改上游媒体源码**。这一点如实登记在
 * `docs/修改记录.md`，不冒充"已实现逐包 fsync 的替代方案"。
 *
 * ============================ 容量口径（§18.5 拍板） ============================
 * **SD 容量＝运行时 `statfs` 实读，不占配置键**；水位线是计划写死的工程常量
 * （`iv_storage_cfg_t` 里可覆盖，但**生产路径不接配置键** —— 免得为一个
 * "85/90/95/70" 去动默认值表与项数上限）。清理**按录像实际大小滚动删除，不按时间**。
 *
 * ⚠ **SD 挂载点仍是板端待确认事实**：默认根 `/mnt/sdcard` 与 bird_media 的
 * `defs.h`（`PATH_RECORD_ROOT=/mnt/sdcard/video`、抓拍默认 `/mnt/sdcard/snap`）
 * 同源；板子实际挂载点要用 COM28 串口核对（用户 2026-10-14 拍板"开发自查"）。
 * 环境变量 `IVSBOX_SD_ROOT` 可覆盖根目录，**仅供开发/测试**（产线不设）。
 */
#ifndef IVSBOX_IV_STORAGE_H
#define IVSBOX_IV_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 容量常量
 * ------------------------------------------------------------------------- */
#define IV_STORAGE_PATH_MAX   160u /* 全路径上限（含 NUL）；与 IV_CFG_STR_MAX 同量级 */
#define IV_STORAGE_NAME_MAX   64u  /* 文件名上限（含 NUL） */
/* 单轮候选窗口＝"最该删的 N 条"。**刻意有界**：SD 上可能有上万条录像，
 * 全量排序既费内存又费栈；我们只关心"最旧的若干条"，桶式保留即可。 */
#define IV_STORAGE_FILES_MAX  64u
/* 单轮最多删条数（≤ FILES_MAX） */
#define IV_STORAGE_DEL_MAX    32u
/* 配置里最多登记的扫描目录数 */
#define IV_STORAGE_DIR_MAX    4u

/* 水位线默认值（计划 S6.5 写死口径） */
#define IV_STORAGE_CLEAN_PCT      85u
#define IV_STORAGE_REJECT_PCT     90u
#define IV_STORAGE_STOP_PCT       95u
#define IV_STORAGE_RECOVER_PCT    70u
/* 巡检周期（计划："定时巡检"） */
#define IV_STORAGE_INTERVAL_MS    30000u
/* 最近多少秒内落盘的切片不删（防删正在写的分片；分片上限 15 min，取 2 min 足够） */
#define IV_STORAGE_KEEP_YOUNGEST_S 120u

/* 默认根与子目录（与 bird_media 的 defs.h 同源；挂载点为板端待确认事实） */
#define IV_STORAGE_ROOT_DEFAULT   "/mnt/sdcard"
#define IV_STORAGE_SNAP_SUBDIR    "snap"
#define IV_STORAGE_REC_SUBDIR     "video"
/* 告警录像的**文件名前缀**约定。⚠ 当前**没有任何生产者**产生告警录像
 * （门开沿目前做的是抓图，S4.5），本前缀是为"将来门开触发的告警录像"预留的
 * 分类钩子；在没有生产者的今天，所有 `channel*.mp4` 都归 REC 类。 */
#define IV_STORAGE_ALARM_PREFIX   "alarm_"
/* 单层目录递归深度上限（录像按 <root>/video/<YYYYMMDD>/channel*.mp4 分层） */
#define IV_STORAGE_DEPTH_MAX      3u

/* ---------------------------------------------------------------------------
 * 枚举
 * ------------------------------------------------------------------------- */
/* 保留优先级：**数值越大越先被删**（= 优先级越低） */
typedef enum {
    IV_STORE_CLS_SNAP  = 0, /* 抓图：最优先保留 */
    IV_STORE_CLS_ALARM = 1, /* 告警录像 */
    IV_STORE_CLS_REC   = 2, /* 普通录像：最先删 */
    IV_STORE_CLS_N     = 3
} iv_store_class_t;

/* 水位档位（迟滞后的当前档） */
typedef enum {
    IV_STORE_OK     = 0, /* < clean */
    IV_STORE_CLEAN  = 1, /* >= clean：清理最旧可删 */
    IV_STORE_REJECT = 2, /* >= reject：拒新任务 */
    IV_STORE_STOP   = 3  /* >= stop：停录 ＋ 告警 */
} iv_store_level_t;

/* ---------------------------------------------------------------------------
 * 配置（**值语义**：被 iv_storage_t 整拷，调用方可用栈上临时量）
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t clean_pct;    /* 0 → IV_STORAGE_CLEAN_PCT */
    uint32_t reject_pct;   /* 0 → IV_STORAGE_REJECT_PCT */
    uint32_t stop_pct;     /* 0 → IV_STORAGE_STOP_PCT */
    uint32_t recover_pct;  /* 0 → IV_STORAGE_RECOVER_PCT */
    uint32_t interval_ms;  /* 巡检周期；0 → IV_STORAGE_INTERVAL_MS */
    uint32_t keep_youngest_s; /* 0 → IV_STORAGE_KEEP_YOUNGEST_S */
    uint32_t max_delete;   /* 单轮最多删条数；0 → IV_STORAGE_DEL_MAX */
    char     root[IV_STORAGE_PATH_MAX]; /* 空 → IVSBOX_SD_ROOT → IV_STORAGE_ROOT_DEFAULT */

    /* 扫描目录表：路径（root 下的相对子目录，空串＝root 本身）＋ 该目录的默认类别。
     * n_dir=0 ⇒ 用默认两目录（snap→SNAP、video→REC）。 */
    struct {
        char    sub[IV_STORAGE_NAME_MAX];
        uint8_t cls;
    } dir[IV_STORAGE_DIR_MAX];
    int      n_dir;
} iv_storage_cfg_t;

/* ---------------------------------------------------------------------------
 * 一轮巡检的"事实"（IO 层填；单测直接构造）
 * ------------------------------------------------------------------------- */
typedef struct {
    char     path[IV_STORAGE_PATH_MAX]; /* 全路径（unlink 直接用） */
    char     name[IV_STORAGE_NAME_MAX]; /* 文件名（日志/展示） */
    uint64_t size;                      /* 字节 */
    uint64_t mtime_s;                   /* 修改时间（秒，epoch；0 = 未知 ⇒ 视为可删） */
    uint8_t  cls;                       /* iv_store_class_t */
} iv_store_file_t;

typedef struct {
    uint8_t  mounted;   /* 1 = 根目录存在且 statfs 成功 */
    uint8_t  readonly;  /* 1 = 只读挂载 */
    uint8_t  io_err;    /* 1 = 扫描过程出错 */
    uint64_t total_bytes;
    uint64_t free_bytes;
    int      n_files;   /* 候选条数（≤ IV_STORAGE_FILES_MAX） */
    iv_store_file_t files[IV_STORAGE_FILES_MAX];
} iv_store_probe_t;

/* ---------------------------------------------------------------------------
 * 决策产物
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t level;      /* 迟滞后的当前档（iv_store_level_t） */
    uint8_t fault;      /* 1 = 未挂载/只读/扫描错 */
    uint8_t allow_new;  /* 1 = 允许开新录像任务 */
    uint8_t stop_rec;   /* 1 = 需停录 */
    uint8_t alarm;      /* 1 = 需告警 */
    uint8_t recovered;  /* 1 = 本拍由高水位降回 OK（迟滞解除） */
    uint32_t used_pct;  /* 使用率（整数下取整） */
    int      n_delete;  /* 本轮建议删除条数（≤ max_delete） */
    /* ⚠ **只在本次 decide/run_once 调用期间有效**：指向 probe->files 内部元素。
     * 调用方不得保存该指针（probe 缓冲会被下一轮复用）。 */
    const iv_store_file_t *del[IV_STORAGE_DEL_MAX];
} iv_store_plan_t;

/* ---------------------------------------------------------------------------
 * 可注入 IO
 * ------------------------------------------------------------------------- */
typedef struct {
    /* 填 out（含 mounted/readonly/io_err/total/free/候选窗口）。
     * 返回非 IV_OK 时调用方按 io_err=1 走故障路径。 */
    int (*scan)(void *arg, const iv_storage_cfg_t *cfg, iv_store_probe_t *out);
    /* 删除一个文件；IV_OK 才算删成功。 */
    int (*unlink_file)(void *arg, const char *path);
    void *arg;
} iv_storage_ops_t;

/* ---------------------------------------------------------------------------
 * 统计
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t rounds;             /* 完成的巡检轮数 */
    uint64_t deletes;            /* 累计删除成功条数 */
    uint64_t del_bytes;          /* 累计删除字节数（按 stat 时的大小累加） */
    uint64_t faults;             /* 累计"故障轮"数（未挂载/只读/扫描错） */
    uint32_t last_used_pct;      /* 最近一轮使用率 */
    int      last_deleted_n;     /* 最近一轮删掉条数 */
    uint64_t last_deleted_bytes; /* 最近一轮删掉字节数 */
    uint64_t last_round_ms;      /* 最近一轮的时间戳（now_ms） */
} iv_storage_stats_t;

/* ---------------------------------------------------------------------------
 * 句柄
 * ------------------------------------------------------------------------- */
typedef struct {
    iv_storage_cfg_t  cfg;
    iv_storage_ops_t  ops;    /* 整拷（scan/unlink_file 是函数指针，arg 由调用方负责） */
    iv_store_probe_t  probe;  /* 复用的巡检缓冲（≈16 KB）：**不占栈**，避免大栈帧 */
    uint8_t           inited;
    uint8_t           level;  /* 跨轮保持的迟滞档位 */
    uint8_t           fault;  /* 最近一轮故障标志 */
    uint8_t           alarm;  /* 最近一轮告警标志 */
    uint32_t          used_pct;
    uint64_t          last_run_ms; /* 0 = 尚未跑过 */
    iv_storage_stats_t st;
} iv_storage_t;

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

/* 填默认配置：全部水位线默认、周期 30 s、max_delete 32、root 空（⇒ 环境变量 ⇒ 默认）、
 * 目录表＝ snap(SNAP) / video(REC)。 */
void iv_storage_cfg_default(iv_storage_cfg_t *cfg);

/*
 * 初始化（**整拷 cfg**）。ops 为 NULL ⇒ 用 `iv_storage_ops_default()`。
 * 返回 IV_OK / IV_EINVAL（s 为 NULL）/ IV_ESTATE（重复 init）。
 * 本函数**不碰文件系统**（第一次 IO 发生在 step/run_once）。
 */
int iv_storage_init(iv_storage_t *s, const iv_storage_cfg_t *cfg,
                    const iv_storage_ops_t *ops);

/*
 * 纯决策。now_s 用于"最近 keep_youngest_s 秒不删"（传 0 = 不做该保护，测试用）。
 * cur_level 传上一轮的档位（迟滞；首轮传 IV_STORE_OK）。
 * 返回 IV_OK / IV_EINVAL。
 */
int iv_store_decide(const iv_storage_cfg_t *cfg, const iv_store_probe_t *pr,
                    uint64_t now_s, uint8_t cur_level, iv_store_plan_t *out);

/*
 * 立刻跑一轮（忽略周期）：scan → decide → 逐个 unlink。返回 IV_OK（已跑完，
 * 含故障轮）/ IV_EINVAL / IV_ESTATE。
 */
int iv_storage_run_once(iv_storage_t *s, uint64_t now_ms);

/* 每拍调用：距上一轮不足 interval_ms 则直接返回。返回 IV_OK / IV_EINVAL / IV_ESTATE。 */
int iv_storage_step(iv_storage_t *s, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 读数（装配层据此闸门录像）
 * ------------------------------------------------------------------------- */
iv_store_level_t iv_storage_level(const iv_storage_t *s);
int              iv_storage_allow_new(const iv_storage_t *s); /* 1 = 可开新录 */
int              iv_storage_stop_rec(const iv_storage_t *s);  /* 1 = 需停录 */
int              iv_storage_alarm(const iv_storage_t *s);     /* 1 = 需告警 */
int              iv_storage_fault(const iv_storage_t *s);     /* 1 = 未挂载/只读/IO 错 */
int              iv_storage_used_pct(const iv_storage_t *s);
void             iv_storage_stats(const iv_storage_t *s, iv_storage_stats_t *out);

/* 水位档位/故障的中文名（日志用；永不 NULL） */
const char *iv_storage_level_name(int level);

/* ---------------------------------------------------------------------------
 * POSIX 缺省 IO
 * ------------------------------------------------------------------------- */
/*
 * scan = `statfs(root)` ＋ 按 cfg 的目录表做**有界两层**遍历（`video/<YYYYMMDD>/`），
 * 用"桶式保留"挑出最该删的 `IV_STORAGE_FILES_MAX` 条（类别高者优先、同类最旧优先），
 * 不进内存上限；`alarm_` 前缀的文件归 ALARM 类。
 * unlink_file = `unlink()`。
 * arg 不使用（传 NULL 即可）。
 */
void iv_storage_ops_default(iv_storage_ops_t *ops);

/* 解析实际使用的根：cfg_root 非空 → 它；否则 `IVSBOX_SD_ROOT`；否则默认。
 * out 与 cap 非法时安全返回（写空串）。 */
void iv_storage_root_resolve(const char *cfg_root, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_STORAGE_H */
