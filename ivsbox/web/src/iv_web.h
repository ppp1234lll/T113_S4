/*
 * ivsbox-web 内部接口（M5-S5.1 / S5.3；S6.4 新增媒体下载）
 *
 * ============================ 这个进程干什么 ============================
 * 对外给浏览器一个 HTTP 服务（静态页面 ＋ REST/JSON ＋ **媒体文件下载**），对内
 * **只通过本地通道**跟 `ivsboxd` 说话。它自己**不读配置文件、不碰数据库、不发平台
 * 报文** —— 这三件事都归 ivsboxd（架构 §10.4 单写者原则）。
 * 因此本进程**只链 libivcore**（用 iv_chan / iv_log / iv_ret），不链 libivmodules。
 *
 * ============================ 为什么自写 HTTP 而不是拉个库 ============================
 * 计划 S5.1 要求"选型固定一种成熟嵌入式 HTTP 库或 uhttpd（登记选型并固化版本）"。
 * 本工程的实际约束是：① 板端 sysroot 里没有任何现成嵌入式 HTTP 库可链；② 全工程
 * 一贯零第三方依赖（SNMP 客户端都是自写的，见 S4.3 定案）；③ 需要的只是
 * "GET/POST ＋ 静态文件 ＋ JSON 接口"这一小块，通用库的 CGI/SSI/模板机制我们
 * **明确不要**（计划 S5.1 的"实现必须优化"点名的就是参考项目那套 SSI/CGI）。
 * 故**选型登记为：自研最小 HTTP/1.1 服务器，只支持本工程用到的子集**：
 *   - 方法：GET / POST（其余 405）
 *   - 每连接**处理一个请求即关闭**（`Connection: close`）：不做 keep-alive /
 *     pipeline。页面请求本来就几个，浏览器会自己重连；省掉一整套连接状态机
 *     与超时语义，换来的简单性远大于那点重连开销。
 *   - 不做 chunked、不做 gzip。
 *   - **Range：仅对媒体文件下载启用**（S6.4，见下）。静态资源都很小、且整体读进
 *     内存后一把发完，走 Range 没有意义，故意不给（少一条"Range 与静态缓存"的
 *     分支就少一处能错的地方）。
 *
 * ============================ S6.4 媒体下载（新增，2026-10-14） ============================
 * 计划 S6.4「视频读取」的**方式二**：`HTTP Range 下载（供 Web/授权通道，支持
 * Range/ETag/校验值/限速/取消）`。落地在本文件所在进程里：
 *
 *   - **只读、且限定在媒体根下**：`/media/...` 的路由只读 `media_root` 下的**普通文件**；
 *     路径解析先做逐段 `..` 拒绝，再用 `realpath()` 复核前缀，最后 `open(O_NOFOLLOW)`
 *     ＋ `fstat` 确认是普通文件。**不接受写、不接受媒体根之外的任何路径。**
 *   - **流式发送**：正文**不进 `wbuf`**（`wbuf` 只有 96 KB，一个 15 分钟的录像远不止），
 *     而是记住 `send_fd/send_off/send_left`，在可写事件里一块块 `pread`→`send`。
 *     块缓冲**复用 `rbuf`**（请求已解析完，一连接一请求，不会回到解析）。
 *   - **限速**：`media_bps`（字节/秒，0 = 不限）。按 1 s 窗口算配额，配额用完就
 *     把本连接"再等 20 ms"记进 `rl_wait_until_ms`，由主循环把它折进 poll 超时 ——
 *     **不是 sleep**（单线程里 sleep 会连累其它连接），也不是不设上限地空转
 *     （POLLOUT 是电平触发，不发送就会立刻再触发 ⇒ 必须靠超时收敛）。
 *   - **取消**：客户端断开（或中途关页面）会让 `send()` 直接失败 ⇒ 关连接、关 fd，
 *     已发字节不回滚。这就是"取消"的全部语义，不需要额外的控制帧。
 *   - **与控制在传输层隔离**：文件字节**从不经过**本地通道/控制队列，也不与
 *     `ivsboxd` 的任何指令共用队列（计划原文要求）—— 下载是 Web 进程自己的 I/O。
 *
 * ⚠ **登记的分层偏离**：Web 进程此前"不碰文件系统（除静态根）"，S6.4 起它会读
 * **媒体根**下的录像文件。这是计划点名的方式二（"供 Web/授权通道"）所要求的，
 * 但确实扩大了这个进程的可读面 ⇒ 用上面的路径围栏把范围钉死在媒体根内，
 * 并在 `docs/修改记录.md` 如实登记，不冒充"零偏离"。
 *
 * ============================ 鉴权 ============================
 * **静态页面放行，`/api` 与 `/media` 下的接口一律要求会话 Cookie**（`login` 除外）。
 * 理由：静态文件里没有任何设备数据（数据全由 JS 现拉），把登录页锁上反而没法登录；
 * 而媒体下载能拿到录像内容，必须鉴权。会话 token 是进程内存里的随机串，进程重启
 * 即全部失效（嵌入式下比落盘更安全也更简单）。
 * 口令当前来自命令行（`--pass`）或环境变量，**登记欠账**：架构 §10 本应走
 * `secure/` 密钥分离（F8 未实现，与 S4.3 的 `community` 同一笔账）。
 */
#ifndef IVSBOX_WEB_IV_WEB_H
#define IVSBOX_WEB_IV_WEB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * 限额（S5.1"上限：连接数、请求体大小、上传大小、各类超时"）
 * -------------------------------------------------------------------------- */
#define IV_WEB_MAX_CONN   8u        /* 并发连接上限（超出的直接 503 并关闭） */
#define IV_WEB_HEAD_MAX   4096u     /* 请求行 ＋ 头总长上限 */
#define IV_WEB_BODY_MAX   32768u    /* 请求体上限（配置提交） */
#define IV_WEB_RSP_MAX    (96u * 1024u) /* 响应缓冲上限（静态资源整体读入） */
#define IV_WEB_PATH_MAX   256u
#define IV_WEB_TOKEN_MAX  48u
#define IV_WEB_SESS_N     8u
#define IV_WEB_IDLE_MS    30000u    /* 连接空闲超时 */
#define IV_WEB_STATIC_MAX (IV_WEB_RSP_MAX - 2048u) /* 单个静态文件上限 */

/* S6.4：媒体下载的块大小。**必须 ≤ rbuf 容量**（块缓冲复用 rbuf，见文件头）。 */
#define IV_WEB_FILE_CHUNK  32768u
/* S6.4：未配置 `--media-bps` 时的默认限速（字节/秒）。0 = 不限速（仅测试/内网）。
 * 出厂默认给一个上限：4G 私网下不让一个下载把上行占满（计划"限速可配"）。 */
#define IV_WEB_MEDIA_BPS_DEFAULT 262144u /* 256 KiB/s */
/* S6.4：媒体根默认值（与 bird_media 的 PATH_RECORD_ROOT 同源，见 iv_storage.h） */
#define IV_WEB_MEDIA_ROOT_DEFAULT "/mnt/sdcard"
#define IV_WEB_MEDIA_ROOT_MAX     256u
/* 限速窗口（ms）与"配额用完先等这么久" */
#define IV_WEB_RL_WIN_MS   1000u
#define IV_WEB_RL_WAIT_MS  20u

/* --------------------------------------------------------------------------
 * 连接
 * -------------------------------------------------------------------------- */
typedef struct {
    int      fd;
    uint64_t last_ms;
    size_t   rlen;      /* rbuf 中已收字节 */
    int      hdr_done;  /* 请求头已收齐 */
    size_t   body_need; /* 头里声明的 Content-Length（0 = 无体） */
    size_t   wlen;      /* wbuf 中待发字节 */
    size_t   wsent;     /* wbuf 中已发字节 */

    /* ---- S6.4 文件流式响应 ----
     * send_fd >= 0 表示"头已/待发，正文来自该文件"。send_left == 0 即收尾关连接。
     * 块缓冲复用 rbuf（容量 36865 ≥ IV_WEB_FILE_CHUNK），故另存 flen/fsent 记账。 */
    int      send_fd;    /* -1 = 无文件体 */
    uint64_t send_off;   /* 下一个要读的文件偏移（= 响应体起点 + 已发字节） */
    uint64_t send_left;  /* 还要发多少字节 */
    size_t   flen;       /* rbuf 里已读入的块长度 */
    size_t   fsent;      /* 该块已发出的字节 */

    /* ---- S6.4 限速 ---- */
    uint32_t rl_bps;            /* 0 = 不限 */
    uint64_t rl_win_start_ms;   /* 当前 1 s 窗口起点 */
    uint64_t rl_sent;           /* 当前窗口已发字节 */
    uint64_t rl_wait_until_ms;  /* 主循环把它折进 poll 超时，避免 POLLOUT 空转 */
    uint64_t rl_holds;          /* 本连接因配额用尽而"先不发"的次数（诊断） */

    /* 多 1 字节：解析请求头时要在 body 末尾补一个 '\0'，好让 json_tokener_parse
     * 那类"要 C 串"的调用有个明确的结尾（载荷本身不带长度语义）。 */
    char     rbuf[IV_WEB_HEAD_MAX + IV_WEB_BODY_MAX + 1u];
    char     wbuf[IV_WEB_RSP_MAX];
} iv_web_conn_t;

/* 解析后的请求 */
typedef struct {
    char    method[8];                /* "GET" / "POST" */
    char    path[IV_WEB_PATH_MAX];    /* 去掉 query 的路径 */
    char    query[IV_WEB_PATH_MAX];   /* ? 之后的原文（无则空串） */
    char    token[IV_WEB_TOKEN_MAX];  /* Cookie 里的 ivs_sid（无则空串） */
    char    range[64];                /* Range: 头原文（S6.4；无则空串） */
    char    if_range[96];             /* If-Range: 头原文（S6.4；无则空串） */
    char   *body;                     /* 指向 conn->rbuf 内的体 */
    size_t  body_len;
} iv_web_req_t;

typedef struct {
    char     token[IV_WEB_TOKEN_MAX];
    uint64_t expires_ms;
} iv_web_sess_t;

typedef struct {
    const char   *chan_path;   /* 本地通道接听点 */
    const char   *static_root; /* 静态资源根目录 */
    char          pass[64];    /* 登录口令 */

    /* S6.4 媒体下载（值语义，便于用命令行/环境变量覆盖） */
    char          media_root[IV_WEB_MEDIA_ROOT_MAX];
    uint32_t      media_bps;   /* 0 = 不限速 */

    int           chan_fd;     /* 惰性连接；-1 = 未连接 */
    iv_web_sess_t sess[IV_WEB_SESS_N];
    uint32_t      rng;         /* 会话 token 的随机源（xorshift） */
} iv_web_ctx_t;

/* 响应描述（由 api / media / static 填，http 层负责拼头与发送） */
typedef struct {
    int    status;                        /* HTTP 状态码 */
    char   ctype[48];                     /* Content-Type */
    int    set_cookie;                    /* 1 = 追加 Set-Cookie */
    /* Set-Cookie 的值：`ivs_sid=<token>; Path=/; HttpOnly`。
     * 常量部分 "ivs_sid=" (8) + "; Path=/; HttpOnly" (18) + NUL = 27 B 固定开销，
     * 故长度取 token 上限 + 32（多留余量）。原先 +16 只有 64 B，被
     * -Wformat-truncation 挡下（token 满长时需 75 B）—— 那是**真截断**，
     * Cookie 被砍掉 HttpOnly 尾部，浏览器语义就变了，不是虚警。 */
    char   cookie[IV_WEB_TOKEN_MAX + 32]; /* Set-Cookie 的值 */
    size_t len;                           /* body 长度（有 file_path 时忽略） */

    /* ---- S6.4 额外的响应头（Accept-Ranges / Content-Range / ETag / X-SHA256 …）。
     * 每行**自带 CRLF**，总长受本字段容量限制；http 层把它们原样插在固定头之后。 */
    int    has_extra;
    char   extra[288];

    /* ---- S6.4 文件体：非 NULL ⇒ 正文流式来自该文件 ---- */
    const char *file_path;
    uint64_t    file_off;
    uint64_t    file_len;
} iv_web_resp_t;

/* --------------------------------------------------------------------------
 * 公共小工具（main.c 提供）
 * -------------------------------------------------------------------------- */
uint64_t iv_web_now_ms(void);
uint32_t iv_web_rand(iv_web_ctx_t *ctx);

/* --------------------------------------------------------------------------
 * api.c —— REST 路由 + 会话 + 本地通道
 * 返回 IV_OK 表示 out 里是完整响应体；IV_ENOTSUP 表示"这个路径不归我"。
 * -------------------------------------------------------------------------- */
int iv_web_api_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                      char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp);

/* 会话：命中返回 1；同时把到期的清掉。 */
int  iv_web_sess_check(iv_web_ctx_t *ctx, const char *token);
void iv_web_sess_new(iv_web_ctx_t *ctx, char *out_token, size_t cap);

/* --------------------------------------------------------------------------
 * media.c —— S6.4 录像文件下载（Range/ETag/校验值/限速/取消）
 * 与 api.c 同口径：IV_OK = 已填好响应；IV_ENOTSUP = 这个路径不归我。
 * 命中 /media/... 但**未登录** ⇒ 401；路径非法/越界 ⇒ 403；不存在 ⇒ 404。
 * -------------------------------------------------------------------------- */
int iv_web_media_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                        char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp);

/* --------------------------------------------------------------------------
 * static.c —— 静态文件
 * -------------------------------------------------------------------------- */
int iv_web_static_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                         char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp);

/* --------------------------------------------------------------------------
 * chan.c —— 本地通道客户端
 * -------------------------------------------------------------------------- */
int  iv_web_chan_call(iv_web_ctx_t *ctx, const char *req, size_t req_len,
                      char *out, size_t cap, size_t *out_len);
void iv_web_chan_close(iv_web_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_WEB_IV_WEB_H */
