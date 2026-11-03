/*
 * 平台 HTTP 上传客户端（start / chunk / finish）—— 日志与图片共用
 *
 * ============================ 它是什么 ============================
 * 把参考实现（GD32F460 `General_Version/main/APP/Upload/`）那套三步上传
 * 抽成一个可复用的阻塞客户端：
 *
 *     POST /fnwlw/oss/log/upload/start   ?deviceSn=&totalSize=&totalChunks=&fileName=&logType=
 *     POST /fnwlw/oss/log/upload/chunk   multipart: uploadId / chunkIndex / file
 *     POST /fnwlw/oss/log/upload/finish  ?uploadId=
 *
 * **协议是三步，但一个上传只用一次 chunk 请求**（`totalChunks=1`、`chunkIndex=0`）。
 * 2026-11-03 真机实测（板端直连 47.104.250.225:18080，164744 B 的真抓拍图）：
 *
 *     totalChunks=1   （整块一片）      2.2 s      ← 现在用这个
 *     totalChunks=3   （64 KiB 一片）   2.2 s
 *     totalChunks=161 （1 KiB 一片）   17.6 s      ← 原来照抄单片机的写法
 *
 * 三份文件下载回来 md5 全部等于原图，说明**服务端不要求 1 KiB 边界**，片数只要
 * 与自己申报的 `totalChunks` 一致就能拼对。1 MiB 真日志（`SYSTEM` + `device.txt`）
 * 整块 12.4 s、4 MiB 图片整块 50.7 s 也都通，所以本层**统一按整块传**。
 *
 * 为什么不再做小分片：分片的好处是"单片失败只重传该片"，但代价是每个片一次完整
 * 请求/应答往返 —— 4G 上 RTT 40 ms 起，1 KiB 片就把 164 KB 的文件拖成 161 次往返。
 * 我们的上传单位是"一个文件"（图片 ≤4 MiB、日志 ≤1 MiB），失败本来就整文件重试，
 * 分片只买到开销没买到收益。
 *
 * 内存纪律：整块 ≠ 整块读进内存。`iv_uphttp_begin()` 先把 HTTP 头与 multipart 前
 * 缀发出去，`iv_uphttp_write()` 按调用者给的片段逐段发，`iv_uphttp_end()` 补尾，
 * 全程只占一个 `IV_UPHTTP_SEND_CHUNK` 大小的缓冲，**与文件大小无关**。
 *
 * ============================ 谁在用 ============================
 *   `iv_logup`（平台 0xB2 日志上报）：按游标把多份日志文件拼成一条字节流，边遍历
 *   边 `write`；
 *   `iv_imgup`（箱门抓拍图片）：`put_fd()` 一把传完一张。
 * 两个调用者的差异只在"从哪里取字节"，HTTP 这一半必须同源。
 *
 * ============================ 关于 logType / 文件名（2026-11-03 实测） ============================
 * - 服务端**不校验** logType：`IMAGE` 与 `SYSTEM` 走同一条链路，都落在
 *     https://minio.fnwlwiot.com:18444/fnwlw-01/device-log/<SN>/<名>_<时间戳>
 *   靠文件名区分；`/fnwlw/oss/image/upload`、`/fnwlw/oss/file/upload` 都是 404。
 * - **但 `.log` 扩展名会被拒**：finish 回 `code=500 文件上传OSS失败`（同一份内容
 *   换成 `.txt` / `.jpg` 立刻成功）。日志上报的文件名沿用参考实现的 `device.txt`，
 *   **不要改成 `.log`**。
 *
 * ============================ 线程纪律 ============================
 * 全部是阻塞 IO（DNS / connect / send / recv），**只能由慢任务池的 worker 调用**，
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

/* 发送缓冲：**不是协议分片**，只是 read/send 的搬运粒度。整次上传只有一片。 */
#define IV_UPHTTP_SEND_CHUNK 4096u

/* 长度上限（全部含结尾 NUL） */
#define IV_UPHTTP_HOST_MAX 128u
#define IV_UPHTTP_BIND_MAX 16u
#define IV_UPHTTP_SN_MAX   24u
#define IV_UPHTTP_NAME_MAX 64u /* 文件名：日志是 device.txt，图片是 snap_<秒>.jpg */
#define IV_UPHTTP_TYPE_MAX 16u
#define IV_UPHTTP_ID_MAX   64u /* start 应答的 uploadId */

/* 缓冲尺寸 */
#define IV_UPHTTP_HDR_MAX  512u
#define IV_UPHTTP_RESP_MAX 2048u /* 应答（头+JSON body） */
#define IV_UPHTTP_PRE_MAX  512u  /* multipart 前缀（uploadId + chunkIndex + file 头） */
#define IV_UPHTTP_TAIL_MAX 96u   /* multipart 结尾 */

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

/* 句柄：内嵌进 iv_logup_t / iv_imgup_t（零 malloc，约 8 KiB，不要放栈上） */
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

    /* 整块发送的进度：begin 记下 total，write 递减，end 要求归零 */
    uint32_t total;   /* start 申报的 totalSize */
    uint32_t left;    /* 还没 write 的字节数 */
    int      sending; /* 1 = begin 之后、end 之前 */

    char    hdr[IV_UPHTTP_HDR_MAX];
    char    resp[IV_UPHTTP_RESP_MAX];
    char    pre[IV_UPHTTP_PRE_MAX];
    char    tail[IV_UPHTTP_TAIL_MAX];
    uint8_t io[IV_UPHTTP_SEND_CHUNK];
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
 * 一次上传的四步（各自内部保证连接可用；失败时置 need_reconnect，调用者可原样重试）：
 *   start : 申报 totalSize（totalChunks 恒为 1）+ 文件名 + 类型，取 uploadId
 *           IV_OK / IV_ECONN / IV_EPROTO（非 2xx、code≠200、无 uploadId）
 *   begin : 发请求头 ＋ multipart 前缀（含 Content-Length）
 *   write : 逐段发文件内容；累计超过 start 申报的 totalSize ⇒ IV_ERANGE
 *   end   : 补 multipart 尾 → 读应答 → url 非 NULL 时把 `data` 里的链接拷出来
 * 每次调用后 `iv_uphttp_status()` 给出最近一次 HTTP 状态码。
 */
int iv_uphttp_start(iv_uphttp_t *h, const char *file_name, const char *type,
                    uint32_t total_size);
int iv_uphttp_begin(iv_uphttp_t *h);
int iv_uphttp_write(iv_uphttp_t *h, const void *data, size_t len);
int iv_uphttp_end(iv_uphttp_t *h, char *url, size_t cap);

/* 便捷：start 之后把一个 fd 从头读到尾并 finish（内部用 `io` 缓冲，
 * 与文件大小无关）。fd 位置由调用者负责（通常刚 open）。 */
int iv_uphttp_put_fd(iv_uphttp_t *h, int fd, char *url, size_t cap);

/* 最近一次 HTTP 状态码；应答原文（含头）用于失败现场取证。 */
int         iv_uphttp_status(const iv_uphttp_t *h);
const char *iv_uphttp_resp(const iv_uphttp_t *h);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_UPHTTP_H */
