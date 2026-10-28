/*
 * ivsbox-web 内部接口（M5-S5.1 / S5.3；S6.4 新增媒体下载；M10 新增登录账号与改密）
 *
 * ============================ 这个进程干什么 ============================
 * 对外给浏览器一个 HTTP 服务（静态页面 ＋ REST/JSON ＋ **媒体文件下载**），对内
 * **只通过本地通道**跟 `ivsboxd` 说话。它自己**不读配置文件、不碰数据库、不发平台
 * 报文** —— 这三件事都归 ivsboxd（架构 §10.4 单写者原则）。
 * 因此本进程**只链 libivcore**（用 iv_chan / iv_log / iv_ret / iv_sha256），不链 libivmodules。
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
 *   - **Range：仅对媒体文件下载启用**（S6.4，见下）。静态资源**不支持** Range：
 *     小文件整体读入后一把发完，大文件（2026-10-28 起）走文件体分流一次性发完；
 *     浏览器取页面资源本来也不发 Range，故意不给（少一条"Range 与静态缓存"的
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
 *
 * ---- M10：账号 ＋ 口令（2026-10-16 用户定） ----
 * 默认 **账号 `admin` / 口令 `88888888`**（`IV_WEB_USER_DEF` / `IV_WEB_PASS_DEF`）。
 * **出厂默认口令下一律强制改密**：`ctx->must_change == 1` 时，除
 * `POST /api/passwd`（改密）与 `POST /api/logout`（登出）之外，`/api/` 与 `/media/` 开头的
 * 路径一律 **403 + `msg:"must change password"`**。前端拿到 403 或登录响应里的
 * `must_change:true` 就切到"修改密码"视图。**这条闸门放在服务端**：只靠前端提示
 * 等于没防线——知道出厂口令的人直接 curl 就能继续用。
 * 新口令复杂度由 `iv_web_pass_rule()` 判（8~12 位且含大写/小写/数字），
 * 提示原文见 `IV_WEB_PASS_HINT_TEXT`（与参考实现逐字节一致，见 iv_web_pass.c 文件头）。
 * 改密成功后写 `<secure>/web.passwd`（`0600`，**只存哈希**，见 iv_web_pass.c）。
 * ⚠ 与 `--pass` / `IVSBOX_WEB_PASS` 的关系：那两个键现在是**初始口令**的来源（工厂/
 * 验证用），盘上有密码文件时以文件为准。**登记欠账**：口令类数据走 `secure/` 的
 * 密钥分离（F8）仍未实现，本文件所在目录不是真机密存储，与 S4.3 的 `community`
 * 是同一笔账 —— 区别只是这里连明文都不落盘。
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
#define IV_WEB_RSP_MAX    (128u * 1024u) /* 响应缓冲上限（静态资源整体读入） */
#define IV_WEB_PATH_MAX   256u
#define IV_WEB_TOKEN_MAX  48u
#define IV_WEB_SESS_N     8u
#define IV_WEB_IDLE_MS    30000u    /* 连接空闲超时 */
#define IV_WEB_STATIC_MAX (IV_WEB_RSP_MAX - 2048u) /* 静态资源"整体读入"的阈值 */
/* ⚠ 从 2026-10-28 起这**不再是硬顶**：超过它的静态文件不再回 413，而是走 S6.4 的
 *   文件体分流（`rsp->file_path` —— 头进 wbuf、正文 `pread` 分块发），因此**不需要
 *   再为"页面变大"抬 `IV_WEB_RSP_MAX`**。本宏现在只是"小文件快路径"的分界线：
 *   ≤ 它就一次 `read` 进 wbuf 一发完事（省一次 pread 与分块记账），> 它就流式发。
 *   历史：2026-10-22 恢复出厂那张卡把 app.js 顶到 99207 B > 当时的 96256 B，只能把
 *   96 KiB 抬到 128 KiB；2026-10-28 app.js 长到 133673 B 又顶穿 129024 B ⇒ 板端
 *   `/app.js` 回 413、Web UI 整页白屏。这次按当时写下的办法改成分流，没再抬上限
 *   （抬上限的代价是 `g_conn[]` 每连接多占 32 KiB，8 连接 ≈ +256 KiB BSS）。 */

/* S6.4：媒体下载的块大小。**必须 ≤ rbuf 容量**（块缓冲复用 rbuf，见文件头）。 */
#define IV_WEB_FILE_CHUNK  32768u
/* S6.4：未配置 `--media-bps` 时的默认限速（字节/秒）。0 = 不限速（仅测试/内网）。
 * 出厂默认给一个上限：4G 私网下不让一个下载把上行占满（计划"限速可配"）。 */
#define IV_WEB_MEDIA_BPS_DEFAULT 262144u /* 256 KiB/s */
/* S6.4：媒体根默认值（与 bird_media 的 PATH_RECORD_ROOT 同源，见 iv_storage.h） */
#define IV_WEB_MEDIA_ROOT_DEFAULT "/mnt/sdcard"
#define IV_WEB_MEDIA_ROOT_MAX     256u
/* 系统日志根（对齐参考 web 的「打印日志」）：iv_log 按 `/<YYYY-MM-DD>/<HH>.log` 落盘，
 * 默认值与 S65ivsboxd 的心跳/日志目录同源（`/mnt/UDISK/log`）。 */
#define IV_WEB_LOG_ROOT_DEFAULT "/mnt/UDISK/log"
#define IV_WEB_LOG_ROOT_MAX     256u

/* ---- 升级包分片上传（对齐参考 web 的「升级有线/无线」的**反向**）----
 * 参考实现是"设备自己去服务器下载"；本机没有下载器，包只能由浏览器（或平台/U 盘）
 * 送到设备上。落点必须与 ivsboxd 侧 `iv_upd_request_write()` 的围栏一致 —— 它只
 * 接受**位于 `<data_root>/ota/` 之下**的已存在普通文件（默认 data_root 就是
 * `/mnt/UDISK/ivsbox`），所以这里默认值取它的 ota 子目录，两边对不上时 ctl 会拒绝，
 * 而不会静默装错包。 */
#define IV_WEB_OTA_ROOT_DEFAULT "/mnt/UDISK/ivsbox/ota"
#define IV_WEB_OTA_ROOT_MAX     256u
/* 单片上限。请求体总上限是 `IV_WEB_BODY_MAX`（32 KiB），这里取 24 KiB 给请求头与
 * 未来加参数留余量。**前端必须用同一个数**（`app.js` 的 `OTA_SLICE`）。 */
#define IV_WEB_OTA_SLICE        (24u * 1024u)
/* 包名上限（含 `.part` 后缀前的本体）；整包体积上限（远大于实际版本包，防误传大文件
 * 把 UDISK 填满）。 */
#define IV_WEB_OTA_NAME_MAX     64u
#define IV_WEB_OTA_PKG_MAX      (64u * 1024u * 1024u)
/* 起传前要求的空闲余量：留出解压暂存（`<data_root>/ota/staging/`）的空间。 */
#define IV_WEB_OTA_FREE_MIN     (32u * 1024u * 1024u)
/* 限速窗口（ms）与"配额用完先等这么久" */
#define IV_WEB_RL_WIN_MS   1000u
#define IV_WEB_RL_WAIT_MS  20u

/* --------------------------------------------------------------------------
 * M10：登录账号 / 口令 / 改密
 * -------------------------------------------------------------------------- */
#define IV_WEB_USER_MAX      24u
#define IV_WEB_PASS_MAX      64u
#define IV_WEB_PASS_HEX_MAX  65u  /* sha256 十六进制 64 位 ＋ NUL */
#define IV_WEB_PASS_PATH_MAX 256u
#define IV_WEB_PASS_LEN_MIN  8u   /* 复杂度下限（与参考实现一致） */
#define IV_WEB_PASS_LEN_MAX  12u  /* 复杂度上限 */

#define IV_WEB_USER_DEF "admin"     /* 出厂账号 */
#define IV_WEB_PASS_DEF "88888888"  /* 出厂口令（首次登录必须改掉） */
/* 口令落盘：与 `tunnel.token` 同目录（架构 §10 的 `secure/`；S65ivsboxd 预建该目录）。 */
#define IV_WEB_PASS_FILE_DEF "/mnt/UDISK/ivsbox/secure/web.passwd"

/* 复杂度不合格时回给浏览器的提示：**与参考实现逐字节一致**
 * 出处：雅下项目 `web_server/makefsdata-4g/fs/index.shtml` 第 976 行 ——
 *   alert("请输入正确的密码格式：\n密码长度:8-12\n密码由数字+大写英文字母+小写英文字母组成")
 * 纯文本版给前端/文档看，回 JSON 时用转义版（`\n` 是两字符转义）。
 * 两处必须是同一句话——各写一遍迟早只改一处。
 * **本串只进 HTTP 响应体，不经任何控制台/日志**，故这里直接保留中文原文
 * （不用 `\x..` 转义写法：那样没人能目视校验它与参考实现是否一致）。 */
#define IV_WEB_PASS_HINT_TEXT \
    "请输入正确的密码格式：\n密码长度:8-12\n密码由数字+大写英文字母+小写英文字母组成"
/* 同一句话的 JSON 版（换行写成 \n 两字符转义，供 api_err() 直接拼进 "msg"）。 */
#define IV_WEB_PASS_HINT_JSON \
    "请输入正确的密码格式：\\n密码长度:8-12\\n密码由数字+大写英文字母+小写英文字母组成"

/* --------------------------------------------------------------------------
 * 连接
 * -------------------------------------------------------------------------- */
typedef struct {
    int      fd;
    uint8_t  is_cam;    /* S8.5：1 = 来自摄像机代理专用口 ⇒ 只许走 cam 路由 */
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
    /* S8.5 摄像机代理要原样转发的请求头（只取这三个，其余不透传 —— 代理不是管道，
     * 转发面越窄越好：Host 由代理按目标重写，超响应面的头一律不带过去）。 */
    char    ctype[64];                /* Content-Type（无则空串） */
    char    cookie_raw[256];          /* Cookie 头原文（无则空串；token 只取 ivs_sid） */
    char    auth[160];                /* Authorization（无则空串；浏览器对摄像机的
                                         Basic 认证经此透传，**不是**配置里的凭据） */
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

    /* ---- M10 登录凭据 ----
     * `user` / `pass_hash` 是**登录比对的事实来源**；`pass` 只是启动期的种子
     * （出厂默认 / `--pass` / env），`iv_web_pass_init()` 哈希完立即擦除明文。 */
    char          user[IV_WEB_USER_MAX];
    char          pass[IV_WEB_PASS_MAX];
    char          pass_hash[IV_WEB_PASS_HEX_MAX];
    char          pass_file[IV_WEB_PASS_PATH_MAX]; /* 空串 = 不落盘（验证用） */
    int           must_change;  /* 1 = 仍在出厂默认口令 ⇒ 除改密/登出外一律 403 */

    /* S6.4 媒体下载（值语义，便于用命令行/环境变量覆盖） */
    char          media_root[IV_WEB_MEDIA_ROOT_MAX];
    uint32_t      media_bps;   /* 0 = 不限速 */

    /* 系统日志浏览/下载（值语义，同上）。空串 = 用 IV_WEB_LOG_ROOT_DEFAULT。 */
    char          log_root[IV_WEB_LOG_ROOT_MAX];

    /* 升级包上传落点（值语义，同上）。空串 = 用 IV_WEB_OTA_ROOT_DEFAULT。
     * **必须与 ivsboxd 的 `<data_root>/ota/` 同一个目录**，否则 `ota.request` 会被
     * iv_upd 的围栏拒掉（那是刻意的：谁都不许让安装器吃任意路径）。 */
    char          ota_root[IV_WEB_OTA_ROOT_MAX];

    /* S8.5 摄像机受控代理：cam_port 为**本机代理监听口**（只绑 127.0.0.1；0 = 未开），
     * 用作"目标是代理自己"的环回护栏；cam_upstream 是**摄像机侧的 Web 端口**（唯一
     * 允许被代理的端口，默认 80）。两者都来自命令行/环境变量 —— Web 不读配置。 */
    int           cam_port;
    uint16_t      cam_upstream;

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
    /* 文件体免限速：**静态资源填 1**（页面资源体量小、等它没意义），媒体/日志下载
     * 留 0 ⇒ 受 `--media-bps` 约束（4G 下不让一个下载把上行占满）。
     * 0 是 `memset(&rsp,0,…)` 的默认值，故既有路径行为不变。 */
    int         no_throttle;
} iv_web_resp_t;

/* --------------------------------------------------------------------------
 * 公共小工具（main.c 提供）
 * -------------------------------------------------------------------------- */
uint64_t iv_web_now_ms(void);
uint32_t iv_web_rand(iv_web_ctx_t *ctx);

/* 从 `a=1&b=2` 里取 `key` 的值（不做 percent-decode：本工程的参数都是短标识符）。
 * 实现在 api.c；logs.c 复用同一份 —— 两处各写一遍迟早只有一处支持新语法。
 * ⚠ 返回 **1 = 取到 / 0 = 没取到**，不是 IV_OK/IV_ENOENT（`IV_OK` 恰好是 0，
 *   写成 `== IV_OK` 会变成"永远取不到"；2026-10-28 在目录列表的 `?n=` 上踩过）。 */
int iv_web_query_get(const char *q, const char *key, char *out, size_t cap);

/* --------------------------------------------------------------------------
 * pass.c —— 口令：哈希 / 复杂度 / 落盘（M10）
 *   - `iv_web_pass_hash()` 是登录比对与落盘**共用**的摘要函数（含域名前缀，见 .c 头）；
 *   - `iv_web_pass_rule()` 是复杂度的**唯一权威**（服务端判，前端只做即时提示）；
 *   - `iv_web_pass_init()` 在 main() 里调一次：读盘 → 定 `must_change` → 擦除明文种子；
 *   - `iv_web_pass_save()` 原子落盘；**返回非 OK 时调用方不得提交到内存**
 *     （否则"改了密码、重启又变回去"）。
 * -------------------------------------------------------------------------- */
void iv_web_pass_hash(const char *user, const char *pass, char out[IV_WEB_PASS_HEX_MAX]);
int  iv_web_pass_rule(const char *pass);
int  iv_web_pass_init(iv_web_ctx_t *ctx);
int  iv_web_pass_load(iv_web_ctx_t *ctx);
int  iv_web_pass_save(const iv_web_ctx_t *ctx, const char *user, const char *hex);

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
 * 命中 /media/... 但**未登录** ⇒ 401；**待改密（must_change）** ⇒ 403；
 * 路径非法/越界 ⇒ 403；不存在 ⇒ 404。
 *
 * 2026-10-28 起**路径是目录时返回列表 JSON**（`{ok,d:{dir,n,items:[{n,s,t}]}}`，
 * 按 mtime 降序、最多 48 条）：Web 的「抓拍图片」要用 `GET /media/snap/` 列出刚抓
 * 的几张。目录与文件走同一套路径围栏与鉴权闸门，没有扩大可读面（文件名另有
 * 字符白名单，见 iv_web_media.c 的说明）。
 * -------------------------------------------------------------------------- */
int iv_web_media_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                        char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp);

/* --------------------------------------------------------------------------
 * logs.c —— 系统日志浏览/下载（对齐参考 web 的「打印日志」）
 *   GET /api/logs?max=N       → {"ok":true,"d":{"root":..,"files":[{f,size,mtime}],..}}
 *   GET /api/logs/file?f=..   → 文件体（走 rsp->file_path 流式，不经 wbuf）
 * 与 api/media 同口径：IV_OK = 已填好响应；IV_ENOTSUP = 这个路径不归我。
 * 未登录 ⇒ 401；出厂默认口令期 ⇒ 403；路径形状不合法/越界 ⇒ 403；不存在 ⇒ 404。
 * -------------------------------------------------------------------------- */
int iv_web_logs_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                       char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp);

/* --------------------------------------------------------------------------
 * api.c —— 给同工程其它层用的 ctl 调用口
 * 把 `{m,p}` 包一层发给 ivsboxd，并把响应原样搬进 out（与 api 层自己用的一致）。
 * 暴露出来是为了让 logs/ota 这类同进程模块**不必各写一份**通道封装 ——
 * 两处各写一遍，迟早只有一处跟上新的头/错误语义。
 * -------------------------------------------------------------------------- */
int iv_web_api_call(iv_web_ctx_t *ctx, const char *method, const char *p_json,
                    char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp);

/* --------------------------------------------------------------------------
 * ota.c —— 升级包分片上传 / 校验 / 请求安装（2026-10-22）
 *   POST /api/ota/chunk?name=..&off=N   体＝本片字节 → 追加到 `<name>.part`
 *   POST /api/ota/finish   {"name","size","sha256"} → 校验并 rename 成正式包
 *   POST /api/ota/install  {"name","restart"} → 本地通道 `ota.request`（**不安装**，
 *                                              安装由 monitor.sh 拉起 updater）
 *   POST /api/ota/abort    {"name"}            → 丢弃未完成的分片文件
 *   GET  /api/ota/chunk?name=..                → 未完成上传的续传点
 * 返回口径与 api/media/logs 相同：IV_OK = 已填响应；IV_ENOTSUP = 路径不归我。
 * 鉴权不在这里做：本层只被 `iv_web_api_handle()` 在会话闸门**之后**转交过来。
 * -------------------------------------------------------------------------- */
int iv_web_ota_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                      char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp);

/* --------------------------------------------------------------------------
 * cam.c —— S8.5 摄像机受控代理（架构 §9.3）
 * 只挂在**环回专用监听口**（frp 的 stcp visitor 落点）上，与主口完全隔离：
 * 这里**没有**登录页、没有 /api、没有静态资源 —— 非法路径一律 404 且不透露本机在跑
 * 什么。路由只有 `/cam/<N>/...`，目标 IP 只来自 ivsboxd 的配置键
 * `camera.ch.<N>.ip`（经本地通道查询），端口只有 `cam_upstream` 一个 ⇒
 * "白名单外地址不可达"在这里是物理事实，不是约定。
 * 返回口径与 api/media 相同：IV_OK = 已填响应；IV_ENOTSUP = 路径不归我（404）。
 * -------------------------------------------------------------------------- */
int iv_web_cam_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
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
