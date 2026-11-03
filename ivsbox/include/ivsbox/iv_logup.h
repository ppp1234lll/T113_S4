/*
 * 日志上报（平台 0xB2）—— HTTP 三步上传（一个请求发完）
 *
 * ============================ 它是什么 ============================
 * 平台下 `0xB2`「日志上报」后，把本机日志按参考实现（GD32F460 主控
 * `General_Version/main/APP/Upload/`）的**同一套 HTTP 接口**传到日志服务：
 *
 *     POST /fnwlw/oss/log/upload/start   ?deviceSn=&totalSize=&totalChunks=&fileName=&logType=
 *     POST /fnwlw/oss/log/upload/chunk   multipart: uploadId / chunkIndex / file
 *     POST /fnwlw/oss/log/upload/finish  ?uploadId=[&md5=]
 *
 * 地址与参考实现一致：**与平台服务器同一个 IP**（47.104.250.225），端口
 * 18080（参考 `APP/Upload/src/upload.c` 的 `sg_uploadparam_t`）。本层不写死
 * 服务器：`host` 由装配层按 `net.server.log.host`（非空优先）或活动出口平台
 * 地址给进来，`port` 按 `net.server.log.port`（非 0 优先）或 18080。
 *
 * ============================ 与参考实现的差异（都是刻意的） ============================
 *   1) **日志源不是 littlefs 批次，是本机的日期目录**：
 *      参考设备是 `/log/current/device.txt` 封存成 pending 再传；本工程日志由
 *      `iv_log` 按 `<root>/YYYY-MM-DD/HH.log` 落盘（`/mnt/UDISK/log`）。本层
 *      因此**自己扫目录、按时间顺序读**，不依赖 `iv_log` 的内部批次状态。
 *   2) **用游标做"不重不漏"**（参考靠批次状态，本工程没有批次）：
 *      游标文件记「下一个未上传字节」＝ `<date> <hour> <offset>`。上传成功才
 *      推进游标；失败保持不动，下一次 0xB2 从同一位置重传。游标指向的文件
 *      已被保留期清掉（3 天）⇒ 自动从现存最旧文件开头开始。
 *   3) **单次有上限**（`IV_LOGUP_MAX_BYTES_DEFAULT` = 1 MiB）：一个请求发完，
 *      不再切 1 KiB 片。参考没上限是因为 littlefs 批次本身有 5000 条上限；
 *      本机保留 3 天、每小时 1 MiB 上限，一次全传最多 72 MiB，仍必须截断。
 *      剩下的留给后续 0xB2（或下一次平台催报）。
 *   4) **finish 不带 md5**：参考的 `finish_md5_enable` 出厂就是 DISABLE，服务端
 *      接受不带 md5；本工程借此免一个 MD5 实现（`iv_sha256` 是另一套）。
 *   5) **连接 keep-alive**：一次上传（start + chunk + finish）复用同一条 TCP。
 *      **分片数是 1**（`totalChunks=1`）：2026-11-03 真机实测同一张 164744 B 的图，
 *      1 片 2.2 s、1 KiB×161 片 17.6 s，三份文件 md5 全部等于原图 ⇒ 服务端不要求
 *      1 KiB 边界。1 MiB 日志整块 12.4 s（原来 1024 片要上百秒）。详见
 *      `iv_uphttp.h` 文件头。服务端回 `Connection: close` 或断链时自动重连，
 *      并从**本次上传开头**重来（游标不推进，日志是整段快照，重传不产生重复）。
 *
 * ============================ 线程与调用纪律 ============================
 *   `iv_logup_run()` 是**同步阻塞**的（DNS、connect、读写 socket、读文件）。
 *   架构 §4.2 禁止在 Reactor 线程做这类事 ⇒ 只能由慢任务池（`iv_taskpool`）
 *   的 worker 调用。装配层的用法：
 *       - `0xB2` 到达 ⇒ 判 `busy` ⇒ 回 ACK ⇒ `iv_taskpool_submit(run)`
 *       - 任务完成回调里清 `busy`、打结果日志
 *   本层**不建线程、不碰 Reactor**，句柄内无锁：`run()` 与统计读取分别由
 *   worker 与 reactor 线程做，统计字段按"只被 run() 写、回调后只读"约定共享
 *   （与 `iv_media` 的计数同口径）。
 *
 *   本模块是 libivmodules 成员，只依赖 libc（socket/getaddrinfo/poll），
 *   不引 pthread、不引 iv_chan —— 与 iv_media 同层同纪律。
 */
#ifndef IVSBOX_IV_LOGUP_H
#define IVSBOX_IV_LOGUP_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_uphttp.h" /* 三步 HTTP 与图片上传同源（2026-11-03 抽出） */

#ifdef __cplusplus
extern "C" {
#endif

/* 参考实现的接口路径与端口 —— **定义在 iv_uphttp.h**，这里保留旧名做别名：
 * 日志与图片走同一条 `/fnwlw/oss/log/upload` 接口（三个子路径），只是
 * fileName/logType 不同
 * （2026-11-03 用 .jpg 实测同一个桶同一个前缀，见 iv_uphttp.h 文件头）。 */
#define IV_LOGUP_PATH_START  IV_UPHTTP_PATH_START
#define IV_LOGUP_PATH_CHUNK  IV_UPHTTP_PATH_CHUNK
#define IV_LOGUP_PATH_FINISH IV_UPHTTP_PATH_FINISH

#define IV_LOGUP_PORT_DEFAULT      IV_UPHTTP_PORT_DEFAULT
#define IV_LOGUP_SEND_CHUNK        IV_UPHTTP_SEND_CHUNK
#define IV_LOGUP_MAX_BYTES_DEFAULT (1024u * 1024u) /* 单次上传上限 1 MiB，一个请求发完 */
#define IV_LOGUP_RETRY_MAX         3              /* 单请求重试次数（参考同名常量） */

/* 长度上限（全部含结尾 NUL） */
#define IV_LOGUP_ROOT_MAX     96u
#define IV_LOGUP_STATE_MAX    128u
#define IV_LOGUP_HOST_MAX     128u
#define IV_LOGUP_BIND_MAX     16u
#define IV_LOGUP_SN_MAX       24u
#define IV_LOGUP_TYPE_MAX     16u
#define IV_LOGUP_NAME_MAX     32u  /* 上传文件名（参考固定 "device.txt"） */
#define IV_LOGUP_ID_MAX       64u  /* start 应答的 uploadId */

/* 缓冲尺寸 */
#define IV_LOGUP_DAYS_MAX 16u                                      /* 单次扫到的日期目录数上限 */
#define IV_LOGUP_FILES_MAX (IV_LOGUP_DAYS_MAX * 24u)               /* 跨 3 天最多 72 个 */

/* 请求参数。字符串字段在 init 时**复制进句柄**，调用方可立即释放。 */
typedef struct {
    const char *root;       /* 日志根；NULL → IV_LOG_ROOT_DEFAULT（/mnt/UDISK/log） */
    const char *state_path; /* 游标文件；NULL → $IVSBOX_HOME/logup.state，
                             * 再退 /mnt/UDISK/ivsbox/logup.state */
    const char *host;       /* **必填**：日志服务地址（IP 或域名；与平台同 IP） */
    uint16_t    port;       /* 0 → IV_LOGUP_PORT_DEFAULT（18080） */
    const char *bind_dev;   /* 出口网卡（如 usb0/eth0）；NULL/"" = 由内核按路由选。
                             * 双 WAN 时必须给：4G 与有线同时在线时，只靠路由会把
                             * 日志从不通的那条链路发出去（传输层踩过同一个坑）。 */
    const char *device_sn;  /* deviceSn（设备号的十六进制串，参考 `%lX`）；NULL → "0" */
    const char *log_type;   /* logType；NULL → "SYSTEM" */
    uint32_t    max_bytes;  /* 单次上限；0 → IV_LOGUP_MAX_BYTES_DEFAULT */
    uint32_t    connect_ms; /* 0 → 5000 */
    uint32_t    send_ms;    /* 0 → 10000 */
    uint32_t    recv_ms;    /* 0 → 10000 */
} iv_logup_cfg_t;

typedef struct {
    uint64_t runs;      /* 累计执行次数（含失败） */
    uint64_t ok;        /* 三步全过的次数 */
    uint64_t fail;      /* 失败次数（连接/应答/无日志） */
    uint64_t chunks;    /* 累计成功上传的请求数（一次上传 = 1，字段名沿用旧口径） */
    uint64_t bytes;     /* 累计成功上传的字节数（游标实际推进量） */
    int      last_rc;   /* 最近一次 `iv_logup_run()` 返回码 */
    int      last_http; /* 最近一次 HTTP 状态码（0 = 没连上/没应答） */
    uint32_t last_bytes;/* 最近一次上传字节数 */
    char     last_file[IV_LOGUP_NAME_MAX]; /* 最近一次上传文件名 */
} iv_logup_stats_t;

/* 句柄：内嵌进 `iv_agent_t`（与 iv_media/iv_storage 同风格，零 malloc） */
typedef struct {
    /* 配置（字符串落在下方定长缓冲里，init 后自洽）。
     * 地址 / 出口网卡 / deviceSn / 超时**只在 `up` 里存一份**（与图片上传同源），
     * 本层不另抄 —— 抄两份迟早漂移。 */
    char     root[IV_LOGUP_ROOT_MAX];
    char     state_path[IV_LOGUP_STATE_MAX];
    char     log_type[IV_LOGUP_TYPE_MAX];
    uint32_t max_bytes;
    int      inited;

    /* 一次上传的会话状态（只在 run() 内用） */
    int      running;         /* 1 = run() 在飞（防 set_target 中途改地址） */
    char     file_name[IV_LOGUP_NAME_MAX];

    /* HTTP 这一半在 `iv_uphttp`（与图片上传共用同一份实现）：连接、keep-alive、
     * "按 Content-Length 读满应答"、multipart 组包全在那边，本层只管"传哪一段字节"。
     * 句柄体因此约 8 KiB；不要放栈上。 */
    iv_uphttp_t up;

    iv_logup_stats_t st;
} iv_logup_t;

/* 填默认配置（root/state/port/max_bytes/超时全默认；host 为空 ⇒ 后续 run 返回 IV_ESTATE） */
void iv_logup_cfg_default(iv_logup_cfg_t *cfg);

/*
 * 初始化（复制配置、清统计、fd = -1）。重复 init 返回 IV_ESTATE。
 * **不建线程、不连网**：连接在 `iv_logup_run()` 里现建。
 * host 为空**不算失败**（返回 IV_OK 并打 WARN）：装配层可能先 init、等出口
 * 定了或配置到了再 `iv_logup_set_target()` 补地址。真到 run 时还没地址才
 * 返回 IV_EINVAL。
 */
int iv_logup_init(iv_logup_t *u, const iv_logup_cfg_t *cfg);

/*
 * 换目标（地址 / 端口 / 出口网卡），已 init 且未在传时随时可调。
 *
 * 为什么需要它：日志服务地址可能来自配置（`net.server.log.host`），也可能要
 * 回落到**当前活动出口的平台地址**；而出口（usb0/eth0）会随 netmgr 切换。
 * 每次 `run()` 前由装配层按"这一刻"的事实刷新一遍，避免一次切换后日志继续
 * 从已经不通的那条链路发。
 * 正在传（`run()` 未返回）时调本接口**不生效**（返回 IV_EBUSY）——上传中途
 * 换地址会让 keep-alive 连接与游标语义同时对不上。
 */
int iv_logup_set_target(iv_logup_t *u, const char *host, uint16_t port,
                        const char *bind_dev);

/* 关句柄（关 socket、清零配置）。不是必须调用，但重启前调用可立即释放 fd。 */
void iv_logup_close(iv_logup_t *u);

/*
 * 同步执行一次上传：扫日志 → 读游标 → start → chunk×N → finish → 推进游标。
 * **只能由 worker 线程调用**（阻塞 IO，架构 §4.2）。
 *
 * 返回：
 *   IV_OK       上传成功（游标已推进）
 *   IV_ENOENT   **没有新日志**（游标已在最新位置）—— 不是错误，装配层按"无可传"
 *               记一行日志即可，不要当故障
 *   IV_EINVAL   参数非法（未 init / host 空 / 日志根不可读）
 *   IV_EIO      DNS / connect / send / recv 失败
 *   IV_EPROTO   HTTP 非 2xx，或应答 JSON 的 `code` 不是 200
 *
 * 成败细节看 `iv_logup_stats()` 的 last_http / last_rc。
 */
int iv_logup_run(iv_logup_t *u);

/* 取统计快照（逐字段整拷；可在任意线程读） */
void iv_logup_stats(const iv_logup_t *u, iv_logup_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_LOGUP_H */
