/*
 * 平台 HTTP 上传客户端（start / chunk × N / finish）—— 日志与图片共用
 *
 * ============================ 它是什么 ============================
 * 把参考实现（GD32F460 `General_Version/main/APP/Upload/`）那套**三步分片
 * 上传**抽成一个可复用的阻塞客户端：
 *
 *     POST /fnwlw/oss/log/upload/start   ?deviceSn=&totalSize=&totalChunks=&fileName=&logType=
 *     POST /fnwlw/oss/log/upload/chunk   multipart: uploadId / chunkIndex / file
 *     POST /fnwlw/oss/log/upload/finish  ?uploadId=[&md5=]
 *
 * 调用者只提供「一整块字节」的分片，协议细节留在本层：
 *   - 一条 keep-alive 连接跑完一次上传（`Connection: close` 或断链自动重连）；
 *   - 应答必须**按 Content-Length 读满**再发下一个请求（否则下一次应答会从
 *     上一次的残留 body 开始解析，`code` 永远读不到 200）；
 *   - 单请求不重试（重试的是调用者：同 chunkIndex 重发幂等，语义更清楚）。
 *
 * ============================ 谁在用 ============================
 *   `iv_logup`（平台 0xB2 日志上报）：按游标切日志流，片号连续;
 *   `iv_imgup`（箱门抓拍图片）：整文件切 1 KiB 片，一次传一张。
 * 两个调用者的差异只在"从哪里取字节"，HTTP 这一半必须同源 —— 2026-11-02
 * 真机踩过的坑（分片必须按**整条字节流**切、1 KiB 边界不能改）都写在这里。
 *
 * ============================ 关于 logType / 文件名（2026-11-03 实测） ============================
 * 服务端**不校验** logType 与扩展名：用 `logType=IMAGE`、`fileName=snap_x.jpg`
 * 走同一条 `/fnwlw/oss/log/upload` 链路（start/chunk/finish 三个子路径）能传通，
 * finish 回的文件落在
 *     https://minio.fnwlwiot.com:18444/fnwlw-01/device-log/<SN>/<名>_<时间戳>.jpg
 * 也就是"图片和日志同一个桶、同一个前缀"，靠文件名区分。所以本层只认
 * `file_name` 与 `type` 两个参数，不再分接口。
 *
 * ============================ 线程纪律 ============================
 * 全部是阻塞 IO（DNS / connect / recv），**只能由慢任务池的 worker 调用**，
 * 不许在 Reactor 线程里跑（架构 §4.2）。本层不建线程、不加锁。
 */
#ifndef IVSBOX_IV_UPHTTP_H
#define IVSBOX_IV_UPHTTP_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 接口路径与端口（参考 `APP/Upload/inc/upload_http.h` / `upload.c`） */
#define IV_UPHTTP_PATH_START  "/fnwlw/oss/log/upload/start"
#define IV_UPHTTP_PATH_CHUNK  "/fnwlw/oss/log/upload/chunk"
#define IV_UPHTTP_PATH_FINISH "/fnwlw/oss/log/upload/finish"

#define IV_UPHTTP_PORT_DEFAULT 18080u
#define IV_UPHTTP_CHUNK_SIZE   1024u /* 服务端按 1 KiB 边界拼，**不许改** */

/* 长度上限（全部含结尾 NUL） */
#define IV_UPHTTP_HOST_MAX 128u
#define IV_UPHTTP_BIND_MAX 16u
#define IV_UPHTTP_SN_MAX   24u
#define IV_UPHTTP_NAME_MAX 64u /* 文件名：日志是 device.txt，图片是 snap_<秒>.jpg */
#define IV_UPHTTP_TYPE_MAX 16u
#define IV_UPHTTP_ID_MAX   64u /* start 应答的 uploadId */

/* 缓冲尺寸 */
#define IV_UPHTTP_HDR_MAX  512u
#define IV_UPHTTP_RESP_MAX 2048u                              /* 应答（头+JSON body） */
#define IV_UPHTTP_BODY_MAX (512u + IV_UPHTTP_CHUNK_SIZE + 64u) /* multipart 组包 */

typedef struct {
    const char *host;      /* **必填**：上传服务地址（IP 或域名；与平台同一个） */
    uint16_t    port;      /* 0 → IV_UPHTTP_PORT_DEFAULT（18080） */
    const char *bind_dev;  /* 出口网卡（usb0/eth0）；NULL/"" = 由内核按路由选。
                            * 双 WAN 必须给：否则报文会从不通的那条链路出去。 */
    const char *device_sn; /* deviceSn（十六进制串，参考 `%lX`）；NULL → "0" */
    uint32_t    connect_ms;/* 0 → 5000 */
    uint32_t    send_ms;   /* 0 → 10000 */
    uint32_t    recv_ms;   /* 0 → 10000 */
} iv_uphttp_cfg_t;

/* 句柄：内嵌进 iv_logup_t / iv_imgup_t（零 malloc，约 3 KiB，不要放栈上） */
typedef struct {
    /* 配置（字符串落在下方定长缓冲里，init 后自洽） */
    char     host[IV_UPHTTP_HOST_MAX];
    uint16_t port;
    char     bind_dev[IV_UPHTTP_BIND_MAX];
    char     dev_sn[IV_UPHTTP_SN_MAX];
    uint32_t connect_ms;
    uint32_t send_ms;
    uint32_t recv_ms;
    int      inited;

    /* 一次上传的会话状态 */
    int  fd;             /* -1 = 未连 */
    int  need_reconnect; /* 1 = 下一次请求前重连（服务端 close / 断链） */
    int  status;         /* 最近一次 HTTP 状态码（0 = 没连上/没应答） */
    char upload_id[IV_UPHTTP_ID_MAX + 1u];
    char file_name[IV_UPHTTP_NAME_MAX];
    size_t   resp_len;
    const char *resp_body; /* 指向 resp 内 body 起点（无 body 时指向末尾） */

    char    hdr[IV_UPHTTP_HDR_MAX];
    char    resp[IV_UPHTTP_RESP_MAX];
    char    body[IV_UPHTTP_BODY_MAX];
    uint8_t chunk[IV_UPHTTP_CHUNK_SIZE];
} iv_uphttp_t;

/* 填默认配置（port/超时全默认；host 为空 ⇒ 后续 start 返回 IV_EINVAL） */
void iv_uphttp_cfg_default(iv_uphttp_cfg_t *cfg);

/* 初始化（复制配置、fd = -1）。重复 init 返回 IV_ESTATE。**不建线程、不连网**。 */
int iv_uphttp_init(iv_uphttp_t *h, const iv_uphttp_cfg_t *cfg);

/* 换目标（地址 / 端口 / 出口网卡），已 init 且没在传时随时可调。
 * 地址变了 ⇒ 强制下一次请求重连（旧连接指向旧地址）。 */
int iv_uphttp_set_target(iv_uphttp_t *h, const char *host, uint16_t port,
                         const char *bind_dev);

/* 断开当前连接（不清配置）。下一次请求自动重连。 */
void iv_uphttp_disconnect(iv_uphttp_t *h);

/* 关句柄：断连 + 清 inited。 */
void iv_uphttp_close(iv_uphttp_t *h);

/* 手工建连（可选）。返回 IV_OK / IV_ECONN / IV_EINVAL（未 init / host 空）。 */
int iv_uphttp_connect(iv_uphttp_t *h);

/*
 * 三步接口（各自内部保证连接可用；失败时置 need_reconnect，调用者可原样重试）：
 *   start  : IV_OK / IV_ECONN / IV_EPROTO（非 2xx、code≠200、无 uploadId）
 *   chunk  : 同上；index 由调用者从 0 连续给，len ≤ IV_UPHTTP_CHUNK_SIZE
 *   finish : 同上；url 非 NULL 时把应答 `data` 里的链接拷出来（拿不到就留空）
 * 每次调用后 `iv_uphttp_status()` 给出最近一次 HTTP 状态码。
 */
int iv_uphttp_start(iv_uphttp_t *h, const char *file_name, const char *type,
                    uint32_t total_size, unsigned total_chunks);
int iv_uphttp_chunk(iv_uphttp_t *h, unsigned index, const void *data, size_t len);
int iv_uphttp_finish(iv_uphttp_t *h, char *url, size_t cap);

/* 最近一次 HTTP 状态码；应答原文（含头）用于失败现场取证。 */
int         iv_uphttp_status(const iv_uphttp_t *h);
const char *iv_uphttp_resp(const iv_uphttp_t *h);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_UPHTTP_H */
