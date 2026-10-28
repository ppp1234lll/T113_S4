/*
 * IVSBox 媒体桥接（libivmodules，功能开发计划 M6-S6.1/S6.2/S6.3）
 *
 * ============================ 它是什么 ============================
 * 本机媒体进程 `ivsbox-media`（**收编自姊妹工程 bird_media，一行未改**，
 * 来源与布局见 `media/README.ivsbox.md`）在本机 `127.0.0.1:18000` 暴露四个
 * HTTP 接口：`/capture`（V4L2 MJPEG 抓图）、`/record/start|stop|status`
 * （FFmpeg RTSP→MP4 remux 直封装录像）。本模块是 ivsboxd 侧的**桥接**：
 * 把四个 HTTP 接口包成 C 函数，供本地通道路由（iv_ctl）、门开抓拍钩子
 * （iv_agent）与断网录像状态机（iv_recctl）调用。
 *
 * **为什么走回环 HTTP 而不是把 bird_media 链进来**：
 *   - 视频代码零自研（用户 2026-10-14 指令）：收编进程原样跑，ffmpeg 的
 *     3 MB 静态库隔离在独立进程里，不进 ivsboxd；
 *   - 崩溃隔离：media 进程崩溃只影响录像/抓图，控制面（ivsboxd）不受牵连，
 *     由 monitor-media.sh 循环拉起（S6.1"崩溃只重启自身"）。
 *
 * ============================ 语义与并发的三个关键决定 ============================
 *   1) **抓拍一律异步**：bird_media 的 `/capture` 在服务端是同步实现
 *      （V4L2 DQBUF 阻塞到出帧），一次抓拍可达数百毫秒甚至更久。桥接层把它
 *      变成**入队即回**：内部一个工作线程＋深度 2 的有界队列，满则丢最旧并
 *      计数（snap_drop）——门开回调与 ctl 路由都在 Reactor 单线程内，绝不允许
 *      阻塞（架构 §4.2）。成败以"HTTP 200 后 stat 落盘文件存在"判定并计数，
 *      供 Web 总览与日志观测。
 *   2) **record 三接口同步 HTTP**：bird_media 的 start/stop 只是置标志、status
 *      读标志，都是即回的（毫秒级）；media 进程不在时 connect 立即 ECONNREFUSED。
 *      socket 读写都带 2 s 超时，最坏阻塞 2 s 属可接受（与"卡 2 s"相比，
 *      把这三个也异步化会让 ctl 应答语义变复杂，不值）。
 *   3) **会话表兼心跳**：bird_media 对每路录像有 15 min"无人查询即自动停录"
 *      的保活语义（`/record/status` 查询即续命）。桥接层自持会话表，工作线程
 *      每 `ka_period_ms` 对活跃会话补一次 status 查询，**顺带同步对端的真实
 *      状态**（对端 RTSP 断流自动停录后，status 返回 0，桥接把会话表清掉）。
 *
 * ============================ 依赖与生命周期 ============================
 *   - 纯 libc（socket/pthread），不引 Reactor/不碰 iv_chan；worker 线程与
 *     调用方以互斥锁＋条件变量同步，队列操作只持锁刹那。
 *   - 零 malloc：`iv_media_t` 由调用方持有（约 3 KB，主要是 8 路会话 URL 表）。
 *   - `ka_period_ms=0` 表示**不做会话保活轮询**（纯手动 record 场景）；
 *     工作线程总是创建（抓拍异步化与线程绑定）。
 *   - 本模块是**可选外设**：media 进程不在时全部接口返回 IV_ECONN/IV_EIO，
 *     调用方（ctl/agent）按"业务失败"上抛，不致命。
 */
#ifndef IVSBOX_IV_MEDIA_H
#define IVSBOX_IV_MEDIA_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量（与 bird_media 的 defs.h 对齐；那边的 MEDIA_MAX_CHANNELS=8）
 * ------------------------------------------------------------------------- */
#define IV_MEDIA_CH_N        8   /* 录像通道槽位数（＝bird_media 上限） */
#define IV_MEDIA_URL_MAX     256 /* RTSP URL 与抓拍路径长度上限（含 NUL） */
#define IV_MEDIA_QUEUE_DEPTH 2   /* 抓拍有界队列深度；满丢最旧 */
#define IV_MEDIA_PORT_DEFAULT 18000
#define IV_MEDIA_SNAP_DIR_DEFAULT "/mnt/sdcard/snap" /* 板端 SD 挂载点确认前先按
                                          bird_media 的 /mnt/sdcard 根系走（登记） */

/* 单次回环 HTTP 的收发超时（ms）。media 进程死了是立即拒连，这里只防"活着但卡死"。 */
#define IV_MEDIA_HTTP_TIMEOUT_MS 2000u
/* 录像启动前 TCP 预检的连接超时（ms）与原因文本上限。
 * 500 ms 是权衡：太短会把"网线在、摄像机慢"误判成不通；太长会让 Reactor
 * 单线程在多路全不通时明显卡顿（每路累加）。 */
#define IV_MEDIA_PROBE_MS        500u
#define IV_MEDIA_WHY_MAX         96

/* ---------------------------------------------------------------------------
 * 配置与句柄
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *host;      /* NULL → 127.0.0.1（回环，媒体接口不对网内暴露） */
    int         port;      /* 0 → IV_MEDIA_PORT_DEFAULT */
    const char *snap_dir;  /* 自动命名的抓拍落盘目录；NULL → 默认 */
    uint32_t    ka_period_ms; /* 会话保活轮询周期；0 → 60 s；**0 且队列要用的
                                 场景见文件头**：非 0 才起工作线程（抓拍异步）。 */
} iv_media_cfg_t;

typedef struct {
    uint64_t snap_req;  /* 累计入队抓拍请求数 */
    uint64_t snap_ok;   /* 累计确认落盘成功（HTTP 200 ＋ 文件存在） */
    uint64_t snap_fail; /* 累计确认失败（HTTP 非 200 ／ 文件不存在） */
    uint64_t snap_drop; /* 累计因队满丢弃 */
    uint64_t rec_starts;  /* 累计 record.start 成功受理 */
    uint64_t rec_stops;   /* 累计 record.stop 成功受理 */
} iv_media_stats_t;

typedef struct {
    iv_media_cfg_t cfg;          /* 整拷（指针字段指向调用方静态串或默认串） */
    uint8_t        inited;       /* 1 = init 成功（工作线程已起，若启用） */
    uint8_t        stop;         /* 1 = 请求 worker 退出 */
    uint8_t        rec_active[IV_MEDIA_CH_N]; /* 会话表：本侧认为在录 */
    char           rec_url[IV_MEDIA_CH_N][IV_MEDIA_URL_MAX];
    uint64_t       last_ka_ms;   /* 上次保活轮询（worker 时钟） */

    /* 抓拍有界队列（环形，满丢最旧） */
    char           q_path[IV_MEDIA_QUEUE_DEPTH][IV_MEDIA_URL_MAX];
    int            q_head;       /* 下一弹出位置 */
    int            q_count;
    uint64_t       drop_cnt;     /* 与 stats.snap_drop 同步（worker 侧消费） */

    /* 统计（worker 写 ok/fail，调用方写 req/drop；各字段原子性靠"只被一方写"
     * ＋ 64 位对齐读，工程内 iv_agent 的计数同口径） */
    iv_media_stats_t st;

    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_t       worker;
    uint8_t         worker_up;
} iv_media_t;

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

/* 填默认配置（host/port/snap_dir 全默认，ka_period_ms=60000）。 */
void iv_media_cfg_default(iv_media_cfg_t *cfg);

/*
 * 初始化：起工作线程（ka_period_ms 非 0 时）。重复 init 返回 IV_ESTATE。
 * 返回 IV_OK / IV_EINVAL / IV_ESTATE；线程创建失败返回 IV_EIO（inited 不置位，
 * 调用方按"媒体桥接不可用"降级，不影响整机）。
 */
int iv_media_init(iv_media_t *m, const iv_media_cfg_t *cfg);

/* 关闭：停线程（join）。未 init 幂等返回。 */
void iv_media_fini(iv_media_t *m);

/* ---------------------------------------------------------------------------
 * 抓拍（异步）
 * ------------------------------------------------------------------------- */

/*
 * 入队一次抓拍。path 为 NULL 时按 `snap_dir/snap_<墙钟秒>.jpg` 自动命名，
 * 命名结果写进 out_path（可为 NULL 表示不关心）。**只入队、不等结果**：
 * 成败看 iv_media_stats() 的 snap_ok/snap_fail 与日志。
 * 返回 IV_OK（已入队）/ IV_EINVAL / IV_ESTATE（未 init）/ IV_EBUSY
 * （队列满且无法挤掉更旧请求——不会发生，满时丢最旧后必有空位，留作防御）。
 */
int iv_media_capture_async(iv_media_t *m, const char *path,
                           char *out_path, size_t cap);

/* ---------------------------------------------------------------------------
 * 录像（同步；会话表兼保活）
 * ------------------------------------------------------------------------- */

/*
 * 开始录第 id 路（0..7）到 url。
 *
 * **先预检、后发 start**：配置类错误（不是 rtsp:// / 没有主机 / 主机不是 IPv4）
 * 与网络类错误（连接超时 / 拒连 / 无路由）在这一步就分开，原因写进 why
 * （why 可为 NULL）。这样"录像没起来"在日志里是一句人话，不是一个错误码。
 *
 * **受理 ≠ 在录**：bird_media 的 /record/start 只置标志，ffmpeg 在它的 worker
 * 里异步拉流。账密错、编码/路径不匹配时这里照样回 IV_OK。真实状态必须由
 * `iv_media_record_status()` 复查（`iv_recctl` 的启动确认就是这么做的）。
 *
 * 预检会建一次 TCP 连接，**最多阻塞 IV_MEDIA_PROBE_MS**；调用方（Reactor
 * 单线程）必须接受这个量级，且必须自己做失败退避，否则每拍重试会累加卡顿。
 *
 * 返回：IV_OK（已受理）/ IV_EINVAL（入参或**配置**错）/ IV_ESTATE（未 init）/
 *       IV_ECONN（media 进程不可达，或**摄像机网络**不可达）。
 */
int iv_media_record_start(iv_media_t *m, int id, const char *url,
                          char *why, size_t whycap);

/* 停第 id 路。 */
int iv_media_record_stop(iv_media_t *m, int id);

/* 查第 id 路状态：*recording 1/0。查询即对端续命（bird_media 保活语义）。 */
int iv_media_record_status(iv_media_t *m, int id, int *recording);

/* ---------------------------------------------------------------------------
 * 读数
 * ------------------------------------------------------------------------- */
void iv_media_stats(const iv_media_t *m, iv_media_stats_t *out);

/* 会话表快照（诊断/总览）：n_out = 活跃路数，id 与 url 逐路给出。
 * id_out/url_out 为 NULL 或 chcap 太小则只填能装下的；返回活跃路数。 */
int iv_media_sessions(const iv_media_t *m, int *id_out, char (*url_out)[IV_MEDIA_URL_MAX],
                      int chcap);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_MEDIA_H */
