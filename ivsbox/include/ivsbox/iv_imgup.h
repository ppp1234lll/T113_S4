/*
 * 箱门抓拍图片上传 —— 复用日志上报那套 HTTP 三步上传（`iv_uphttp`），一张一个请求
 *
 * ============================ 它是什么 ============================
 * 门开 → `on_door_open()` → `iv_media_capture_async()` 落盘
 * `/mnt/sdcard/snap/snap_<秒>.jpg`；随后把这张图**入队**，由慢任务池的 worker
 * 用 `iv_uphttp` 传给平台文件服务，成功后删掉队列副本（原图仍留在 CD 卡上，
 * 由存储策略按保留期清）。
 *
 * 队列是**目录**（`$IVSBOX_HOME/imgup/`），不是内存链表：
 *   - 掉电/重启不丢：没传成功的那张原样躺在队列里，下次开机继续；
 *   - 天然去重：一个文件一个任务，成功即 unlink，不存在"游标指错"；
 *   - 队列副本是**拷贝**（/mnt/sdcard → /mnt/UDISK 跨文件系统，`link()` 会
 *     EXDEV），代价是一张约 160 KiB 的写放大，换来"CD 卡策略可以随便删原图"。
 *
 * ============================ 与日志上报的差异（刻意的） ============================
 *   1) **没有游标**：日志是"一条不断增长的流"，所以要记"传到哪了"；图片是
 *      "一堆离散文件"，用目录队列表达"传完没传完"更准。
 *   2) **一次 run 传一张**：一张图 ≈ 160 KiB ≈ 160 片，4G 上几秒；一次只传
 *      一张，任务时长可控、失败影响面小。队列里还有就下一拍再来。
 *   3) **队列有上限**（`IV_IMGUP_MAX_FILES`）：门被反复开关时不能把 UDISK 塞满
 *      —— 超了丢**最旧**的（与 `iv_media` 抓拍队列满丢最旧同口径），并计数。
 *   4) **fileName/logType**：`snap_<秒>.jpg` + `IMAGE`。服务端不校验这两项
 *      （2026-11-03 实测），落到 `device-log/<SN>/<名>_<时间戳>.jpg`。
 *
 * ============================ 线程纪律 ============================
 *   `iv_imgup_run()` 阻塞（DNS/connect/read/recv），只能由慢任务池 worker 调。
 *   `iv_imgup_enqueue()` / `iv_imgup_pending()` 只碰文件系统，可在 Reactor 线程
 *   调（一次 readdir 而已）。统计字段约定同 `iv_logup`：worker 写、回调后读。
 */
#ifndef IVSBOX_IV_IMGUP_H
#define IVSBOX_IV_IMGUP_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_uphttp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IV_IMGUP_DIR_MAX   96u
#define IV_IMGUP_NAME_MAX  64u
#define IV_IMGUP_URL_MAX   160u
#define IV_IMGUP_TYPE      "IMAGE" /* 上传时的 logType（服务端不校验） */

#define IV_IMGUP_MAX_FILES  32u                    /* 队列上限，超了丢最旧 */
#define IV_IMGUP_MAX_BYTES  (4u * 1024u * 1024u)   /* 单张上限；超过不传 */
#define IV_IMGUP_RETRY_MAX  3                      /* 单片重试次数 */

typedef struct {
    const char *queue_dir; /* 待传目录；NULL → $IVSBOX_HOME/imgup，再退
                            * /mnt/UDISK/ivsbox/imgup */
    const char *host;      /* **必填**：上传服务地址（与平台同一个 IP） */
    uint16_t    port;      /* 0 → IV_UPHTTP_PORT_DEFAULT（18080） */
    const char *bind_dev;  /* 出口网卡（usb0/eth0）；NULL = 由内核按路由选 */
    const char *device_sn; /* deviceSn（十六进制串）；NULL → "0" */
    uint32_t    connect_ms;
    uint32_t    send_ms;
    uint32_t    recv_ms;
} iv_imgup_cfg_t;

typedef struct {
    uint64_t enq;      /* 累计入队张数（含后来被丢的） */
    uint64_t ok;       /* 累计上传成功张数 */
    uint64_t fail;     /* 累计失败次数（含重试轮次） */
    uint64_t drop;     /* 累计丢弃（队列满 / 超大 / 读不了） */
    uint64_t bytes;    /* 累计成功上传的字节数 */
    int      last_rc;  /* 最近一次 run() 的返回码 */
    int      last_http;/* 最近一次 HTTP 状态码 */
    char     last_name[IV_IMGUP_NAME_MAX];
    char     last_url[IV_IMGUP_URL_MAX]; /* finish 回的链接（失败时为空） */
} iv_imgup_stats_t;

typedef struct {
    char         queue_dir[IV_IMGUP_DIR_MAX];
    int          inited;
    int          running; /* 1 = run() 在飞（同进程内防重入，worker 独占） */
    iv_uphttp_t  up;      /* 与日志上报同一份 HTTP 实现 */
    iv_imgup_stats_t st;
} iv_imgup_t;

/* 填默认配置（queue_dir/port/超时全默认；host 为空 ⇒ run 返回 IV_EINVAL） */
void iv_imgup_cfg_default(iv_imgup_cfg_t *cfg);

/* 初始化（复制配置、建队列目录）。重复 init 返回 IV_ESTATE。不建线程、不连网。 */
int iv_imgup_init(iv_imgup_t *u, const iv_imgup_cfg_t *cfg);

/* 换目标（地址 / 端口 / 出口网卡），已 init 且没在传时随时可调。 */
int iv_imgup_set_target(iv_imgup_t *u, const char *host, uint16_t port,
                        const char *bind_dev);

void iv_imgup_close(iv_imgup_t *u);

/*
 * 把 `src_path` 拷进队列（跨文件系统用拷贝，不用 link）。
 *
 * 返回：
 *   IV_OK      已入队（文件名取 basename）
 *   IV_ENOENT  源文件不存在（门开抓拍失败时上游就不会调，正常不该出现）
 *   IV_ERANGE  文件超 `IV_IMGUP_MAX_BYTES`，**不传**（记 drop 并返回）
 *   IV_ENOSPC  / IV_EIO 目录或写失败
 * 队列超过 `IV_IMGUP_MAX_FILES` 时先丢最旧的，再入队。
 */
int iv_imgup_enqueue(iv_imgup_t *u, const char *src_path);

/* 队列里待传的文件数（<0 = 目录读不了）。只读文件系统，可在 Reactor 线程调。 */
int iv_imgup_pending(const iv_imgup_t *u);

/*
 * 同步上传队列里**最旧的一张**。只能由 worker 线程调。
 * 返回：
 *   IV_OK      这一张传完了（已从队列删除）
 *   IV_ENOENT  队列空（不是错误）
 *   IV_EINVAL  未 init / host 空
 *   IV_ECONN / IV_EPROTO / IV_EIO  失败（文件留在队列里，下次重试）
 */
int iv_imgup_run(iv_imgup_t *u);

void iv_imgup_stats(const iv_imgup_t *u, iv_imgup_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_IMGUP_H */
