/*
 * ivsbox-web 进程入口 —— 最小 HTTP/1.1 服务器（M5-S5.1）
 *
 * 选型理由、支持的方法子集、鉴权口径见 web/src/iv_web.h 的文件头。
 * 本文件只负责：建监听 socket → poll 事件循环 → 收全一个请求 → 交给
 * api/static 生成响应 → 发回去 → 关连接。
 *
 * ============================ 连接模型 ============================
 * **一连接一请求**（`Connection: close`）。不做 keep-alive / pipeline。
 * 页面加载会并发若干个请求（HTML + JS + CSS + 若干 API），故必须**多连接**：
 * 8 个槽，`accept` 满了直接回 503 并关闭，不做排队 —— 排队会把"服务器忙"
 * 变成"浏览器一直转圈"，错误暴露得晚且更难查。
 *
 * 事件循环用 `poll`：连接数 ≤ 8，fd 集合极小，`epoll` 换不来可观测的收益，
 * 却要多维护一套"fd → 状态"的注册表。
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "iv_web.h"

#define WEB_MOD         "web"
#define WEB_PORT_DEF    80
#define WEB_ROOT_DEF    "/opt/ivsbox/web/static"
/* ---- M10：登录账号与口令 ----
 * `WEB_PASS_DEF` 原来是 `"admin"`（那是**口令**，与账号同名，纯属历史巧合）。
 * 2026-10-16 用户定：**账号 `admin` / 口令 `88888888`，首次登录必须改密**。
 * 这里的默认值只是**初始值**：盘上有 `<secure>/web.passwd` 时以文件为准
 * （见 iv_web_pass.c 的 `iv_web_pass_init()`）。 */
#define WEB_USER_DEF    IV_WEB_USER_DEF
#define WEB_PASS_DEF    IV_WEB_PASS_DEF
/* 响应头预留区：所有响应头都很短（固定 4 行 + 可选 Set-Cookie），512 绰绰有余。 */
#define RSP_HEAD_OFF    512u

/* ---- 主循环心跳（M7-S7.4，架构 §16.2 场景 7）----
 * 主循环每 WEB_HB_MS_DEF 把**单调毫秒数**写进该文件；monitor-web.sh 用「内容有没有变」
 * 判主循环是否卡死（`kill -STOP`/死锁的进程在 /proc 里照样存在，只看 pid 接不住）。
 * 落 /tmp（tmpfs）：复位即清，不会把上一轮的旧值当成本轮的心跳。
 * 用 `--heartbeat-ms 0`（或 IVSBOX_WEB_HEARTBEAT_MS=0）可显式关闭，仅用于验证。 */
#define WEB_HB_PATH_DEF "/tmp/ivsbox-web.heartbeat"
#define WEB_HB_MS_DEF   5000u

/* ---- S8.5 摄像机代理专用监听 ----
 * cam_port：**只绑 127.0.0.1** 的第二监听口（默认 8081，与 sys.tunnel.cam.port 同源
 * —— frp stcp visitor 的落点就是它）。0 = 不开（关闭代理入口）。
 * cam_upstream：摄像机侧 Web 端口（唯一允许被代理的上游端口，默认 80）。
 * 与 iv_web_cam.c 的 IV_WEB_CAM_UPSTREAM_DEFAULT 同值：一个面向监听、一个面向上游。 */
#define WEB_CAM_PORT_DEF      8081
#define WEB_CAM_UPSTREAM_DEF  80u
/* 摄像机口是**每路一个**：cam_port + N（N = 0..WEB_CAM_CH_MAX-1）。为什么不用
 * 单口加 `/cam/N` 前缀：大华/海康前端大量使用根绝对路径（实证 `jsCore/app.js` 里
 * `DevInit.getStatus("/RPC2", ...)`），带前缀时这些请求必 404 ⇒ 网页登录直接报错。
 * 让每路独占一个口、根路径就是摄像机根，前端不用改一行。
 * 数值须与 iv_web_cam.c 的 CAM_CH_N 一致（都是 8 路）。 */
#define WEB_CAM_CH_MAX        8

static iv_web_ctx_t  g_ctx;
static iv_web_conn_t g_conn[IV_WEB_MAX_CONN];
static int           g_listen_fd = -1;
static int           g_cam_fd[WEB_CAM_CH_MAX]; /* S8.5：每路一个口（-1 = 未开） */
static volatile sig_atomic_t g_stop = 0;

static const char *g_hb_path   = WEB_HB_PATH_DEF;
static uint32_t    g_hb_ms     = WEB_HB_MS_DEF;
static uint64_t    g_hb_next   = 0u; /* 下一次允许写心的时刻（0＝立刻写第一拍） */
static int         g_hb_warned = 0;  /* 写失败只告警一次，不刷屏 */

/* --------------------------------------------------------------------------
 * 公共小工具
 * -------------------------------------------------------------------------- */
uint64_t iv_web_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* --------------------------------------------------------------------------
 * 主循环心跳（M7-S7.4）
 * -------------------------------------------------------------------------- */
/* 频率刻意不高：心跳只须在守护侧超时（默认 30 s，见 monitor-web.sh）内到达即可；
 * 5 s 一条对 tmpfs 是噪声级开销，又比超时小一个量级 ⇒ 偶发一次写失败不会被误判卡死。
 *
 * **必须由主循环调用**：换成独立线程来刷，则 poll/业务卡死时线程照常前进、心跳一直新鲜
 * —— 那等于没有卡死检测。这是「判活」与「判卡死」的分水岭，别为了「解耦」把它挪走。 */
static void heartbeat_tick(void)
{
    uint64_t now;
    char     buf[32];
    int      fd;
    int      len;

    if (g_hb_path == NULL || g_hb_path[0] == '\0' || g_hb_ms == 0u)
        return;                       /* 显式关闭（验证/无 tmpfs 场景） */
    now = iv_web_now_ms();
    if (g_hb_next != 0u && now < g_hb_next)
        return;
    g_hb_next = now + (uint64_t)g_hb_ms;

    fd = open(g_hb_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        /* 写不进去（目录不存在/只读）不是致命错误：告警一次，主业务照跑。
         * 代价是守护侧会把本进程判成「卡死」并重拉一次 —— 这个方向是安全的：
         * 误重启一个健康进程，好过「该重启时永远不重启」。 */
        if (g_hb_warned == 0) {
            g_hb_warned = 1;
            IV_LOG_W(WEB_MOD, "heartbeat write failed: %s (%s)", g_hb_path,
                     strerror(errno));
        }
        return;
    }
    len = snprintf(buf, sizeof buf, "%llu\n", (unsigned long long)now);
    if (len > 0)
        (void)write(fd, buf, (size_t)len);
    (void)close(fd);
}

/* xorshift32：只用来生成会话 token。**不用于安全关键用途**（token 的强度还
 * 来自"进程内存 + 8 小时过期"这两条边界）；这里要的只是"两次登录不撞车"。 */
uint32_t iv_web_rand(iv_web_ctx_t *ctx)
{
    uint32_t x = ctx->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    ctx->rng = (x != 0u) ? x : 0x1234567u;
    return ctx->rng;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static const char *status_text(int s)
{
    switch (s) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

/* --------------------------------------------------------------------------
 * 连接槽
 * -------------------------------------------------------------------------- */
static void conn_close(int slot)
{
    if (slot < 0 || slot >= (int)IV_WEB_MAX_CONN)
        return;
    /* S6.4：文件体可能还开着 fd，先关它再关连接。 */
    if (g_conn[slot].send_fd >= 0) {
        (void)close(g_conn[slot].send_fd);
        g_conn[slot].send_fd = -1;
    }
    if (g_conn[slot].fd >= 0)
        (void)close(g_conn[slot].fd);
    g_conn[slot].fd = -1;
}

static int conn_slot_free(void)
{
    int i;

    for (i = 0; i < (int)IV_WEB_MAX_CONN; i++) {
        if (g_conn[i].fd < 0)
            return i;
    }
    return -1;
}

/* 取一个头的值（跳前导空白），超长截断，恒 NUL 结尾。 */
static void hdr_value(const char *v, char *dst, size_t cap)
{
    size_t i = 0;

    while (*v == ' ' || *v == '\t')
        v++;
    while (v[i] != '\0' && i + 1u < cap) {
        dst[i] = v[i];
        i++;
    }
    dst[i] = '\0';
}

/* --------------------------------------------------------------------------
 * 请求解析
 * 返回 IV_OK（已收全一条请求）/ IV_EAGAIN（还没收齐）/ IV_EPROTO（不成形）
 *
 * ⚠ **解析必须是无副作用的：绝不能就地改写 rbuf**（2026-10-13 端到端实测踩过）。
 * 一条 POST 的"头"与"体"常常分两次 recv 到达（curl 写头、再写体），第一次进这里
 * 只能返回 IV_EAGAIN。此前的实现把每行行尾的 '\r' 就地写成 '\0'，于是第二次进来
 * 时 `strstr(rbuf,"\r\n\r\n")` 再也找不到分隔符（已被改成 "\0\n\r\n"）⇒ 又返回
 * IV_EAGAIN，**连接就这么一直挂着直到 30 s 空闲超时**，客户端只看到一个没有响应的
 * 请求（curl 报 size_download=0）。单测天然测不到：它要求"同一条请求跨两次 recv"。
 * 现改为把请求行/每个头行**拷进局部缓冲**再解析，rbuf 只在拿到完整请求后于
 * body 末尾补一个 '\0'（那个位置在 hdr_end 之后，不影响重入时的分隔符搜索）。
 * -------------------------------------------------------------------------- */
static int parse_req(iv_web_conn_t *c, iv_web_req_t *req)
{
    /* 行缓冲：只用于解析，不动 rbuf。512 足够放下本层关心的两种头
     * （Content-Length 的数值、Cookie 里的 32 位 token，都在行首不远处）；
     * 更长的头行按前 511 字节判定，关键字在行首 ⇒ 截断不影响识别。 */
    char   line[512];
    char  *hdr_end;
    char  *line_end;
    char  *p;
    char   target[IV_WEB_PATH_MAX + 64];
    size_t body_off;
    size_t n;

    memset(req, 0, sizeof(*req));
    c->rbuf[c->rlen] = '\0';

    hdr_end = strstr(c->rbuf, "\r\n\r\n");
    if (hdr_end == NULL) {
        if (c->rlen >= IV_WEB_HEAD_MAX)
            return IV_EPROTO; /* 头超过上限还没结束：当坏请求 */
        return IV_EAGAIN;
    }

    line_end = strstr(c->rbuf, "\r\n");
    if (line_end == NULL || line_end > hdr_end)
        return IV_EPROTO;

    n = (size_t)(line_end - c->rbuf);
    if (n >= sizeof(line))
        return IV_EPROTO;
    memcpy(line, c->rbuf, n);
    line[n] = '\0';
    if (sscanf(line, "%7s %319s", req->method, target) != 2)
        return IV_EPROTO;

    {
        char *q = strchr(target, '?');

        if (q != NULL) {
            *q = '\0';
            (void)snprintf(req->query, sizeof(req->query), "%.*s",
                           (int)sizeof(req->query) - 1, q + 1);
        }
        (void)snprintf(req->path, sizeof(req->path), "%.*s",
                       (int)sizeof(req->path) - 1, target);
    }

    c->body_need = 0;
    for (p = line_end + 2; p < hdr_end; ) {
        char *e = strstr(p, "\r\n");

        if (e == NULL || e > hdr_end)
            break;
        n = (size_t)(e - p);
        if (n >= sizeof(line))
            n = sizeof(line) - 1u;
        memcpy(line, p, n);
        line[n] = '\0';

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            long v = strtol(line + 15, NULL, 10);

            if (v < 0 || v > (long)IV_WEB_BODY_MAX)
                return IV_EPROTO;
            c->body_need = (size_t)v;
        } else if (strncasecmp(line, "Range:", 6) == 0) {
            hdr_value(line + 6, req->range, sizeof(req->range));
        } else if (strncasecmp(line, "If-Range:", 9) == 0) {
            hdr_value(line + 9, req->if_range, sizeof(req->if_range));
        } else if (strncasecmp(line, "Content-Type:", 13) == 0) {
            hdr_value(line + 13, req->ctype, sizeof(req->ctype));
        } else if (strncasecmp(line, "Cookie:", 7) == 0) {
            const char *sid = strstr(line + 7, "ivs_sid=");

            /* S8.5：Cookie 头**原文**也要留给摄像机代理（浏览器↔摄像机自己的会话） */
            hdr_value(line + 7, req->cookie_raw, sizeof(req->cookie_raw));
            if (sid != NULL) {
                size_t i = 0;

                sid += 8;
                while (sid[i] != '\0' && sid[i] != ';' && sid[i] != ' ' &&
                       i < IV_WEB_TOKEN_MAX - 1u) {
                    req->token[i] = sid[i];
                    i++;
                }
                req->token[i] = '\0';
            }
        } else if (strncasecmp(line, "Authorization:", 14) == 0) {
            hdr_value(line + 14, req->auth, sizeof(req->auth));
        }
        p = e + 2;
    }

    body_off = (size_t)(hdr_end + 4 - c->rbuf);
    if (c->rlen < body_off + c->body_need)
        return IV_EAGAIN; /* 头全了、体还没齐 */

    if (c->body_need > 0u) {
        req->body = c->rbuf + body_off;
        /* 唯一一处对 rbuf 的写入：补在 body 末尾（hdr_end 之后）。
         * 重入安全——本函数返回 IV_OK 后调用方不会再解析同一条请求。 */
        c->rbuf[body_off + c->body_need] = '\0';
        req->body_len = c->body_need;
    }
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 生成响应：把响应头拼进 wbuf 前部，body 已由 api/static 写在 wbuf+RSP_HEAD_OFF
 * -------------------------------------------------------------------------- */
static void build_response(iv_web_conn_t *c, const iv_web_resp_t *rsp, size_t body_len)
{
    char        head[RSP_HEAD_OFF];
    int         n;
    int         ffd = -1;
    struct stat fst;
    size_t      clen = body_len;

    /* S6.4：文件体 —— 先打开，成功才把 Content-Length 定为文件段长度。
     * O_NOFOLLOW 是第三道围栏（media 层已 realpath 解过符号链接）。 */
    if (rsp->file_path != NULL) {
        ffd = open(rsp->file_path, O_RDONLY | O_NOFOLLOW);
        if (ffd >= 0 && (fstat(ffd, &fst) != 0 || !S_ISREG(fst.st_mode))) {
            (void)close(ffd);
            ffd = -1;
        }
        if (ffd < 0) {
            /* 竞态：解析后文件被清理/替换。回不带体的 404，别再往下走。 */
            c->wlen = (size_t)snprintf(c->wbuf, sizeof(c->wbuf),
                                       "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                       "Connection: close\r\n\r\n");
            c->wsent = 0u;
            return;
        }
        clen = (size_t)rsp->file_len;
    }

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "Cache-Control: no-store\r\n"
                 "%s%s%s%s"
                 "\r\n",
                 rsp->status, status_text(rsp->status), rsp->ctype, clen,
                 rsp->set_cookie ? "Set-Cookie: " : "",
                 rsp->set_cookie ? rsp->cookie : "",
                 rsp->set_cookie ? "\r\n" : "",
                 rsp->has_extra ? rsp->extra : "");
    /* ⚠ 头终止空行的配平（集成用例 test_web_http w01/w02 实证）：
     * 终止符是最后的 "\r\n"，它必须**有且只有一个**。早先写成 `"%s%s" "\r\n"`，
     * 带 Set-Cookie 的响应头从未被终止（curl 表现为 200 + Cookie 齐全但 body 恒 0 字节）；
     * 若改成给 `%s%s` 行补 CRLF，无 Cookie 时又会多出一行（Content-Length 多算 2 字节）。
     * 所以行尾 CRLF 挂在第三个 `%s` 上：只有真写了 Cookie 行才补它的行尾。 */
    if (n < 0 || (size_t)n >= sizeof(head)) {
        /* 头都放不下：只回一个空的 500，绝不发出半截头 */
        n = snprintf(head, sizeof(head),
                     "HTTP/1.1 500 Internal Server Error\r\n"
                     "Content-Length: 0\r\nConnection: close\r\n\r\n");
        if (n < 0 || (size_t)n >= sizeof(head)) {
            if (ffd >= 0)
                (void)close(ffd);
            c->wlen = 0u;
            return;
        }
        memcpy(c->wbuf, head, (size_t)n);
        c->wlen = (size_t)n;
        c->wsent = 0u;
        return;
    }

    /* body 原本在 wbuf + RSP_HEAD_OFF；头比预留区短时把它往前挪，中间不留空洞。 */
    if (body_len > 0u && (size_t)n != RSP_HEAD_OFF)
        memmove(c->wbuf + n, c->wbuf + RSP_HEAD_OFF, body_len);
    memcpy(c->wbuf, head, (size_t)n);
    c->wlen = (size_t)n + body_len;
    c->wsent = 0u;

    /* S6.4：把文件体挂到连接上，正文由 on_writable 一块块 pread->send。
     * clen 与 send_left 同源（rsp->file_len），不一致会立刻被浏览器察觉。 */
    c->send_fd   = ffd;
    c->send_off  = rsp->file_off;
    c->send_left = (ffd >= 0) ? rsp->file_len : 0u;
    c->flen      = 0u;
    c->fsent     = 0u;
    /* 限速只给"录像/日志下载"这类大流量；静态资源走 `no_throttle` 免限速
     * （否则 13 万字节的 app.js 在默认 256 KiB/s 下要多等 0.5 s，
     *  而它本来就不值得限）。0 是 memset 默认值 ⇒ 媒体路径行为不变。 */
    c->rl_bps           = (rsp->no_throttle != 0) ? 0u : g_ctx.media_bps;
    c->rl_win_start_ms  = iv_web_now_ms();
    c->rl_sent          = 0u;
    c->rl_wait_until_ms = 0u;
    c->rl_holds         = 0u;
}

/* --------------------------------------------------------------------------
 * 处理一条已收全的请求
 * -------------------------------------------------------------------------- */
static void handle_req(iv_web_conn_t *c, const iv_web_req_t *req)
{
    iv_web_resp_t rsp;
    char         *body = c->wbuf + RSP_HEAD_OFF;
    size_t        body_cap = sizeof(c->wbuf) - RSP_HEAD_OFF;
    size_t        body_len = 0;
    int           rc;

    memset(&rsp, 0, sizeof(rsp));
    rsp.status = 200;
    (void)snprintf(rsp.ctype, sizeof(rsp.ctype), "application/json");

    if (c->is_cam) {
        /* S8.5：摄像机代理专用口。**只**认 /cam/<N>/...，其余一律一个不透露信息的
         * 404 —— 这里没有登录页、没有 /api、没有静态资源，任何东西都不落出层外。 */
        rc = iv_web_cam_handle(&g_ctx, req, (unsigned)c->cam_ch, body, body_cap,
                               &body_len, &rsp);
        if (rc == IV_ENOTSUP) {
            rsp.status    = 404;
            rsp.set_cookie = 0;
            (void)snprintf(rsp.ctype, sizeof(rsp.ctype), "text/plain");
            body_len = (size_t)snprintf(body, body_cap, "not found\n");
            rc = IV_OK;
        }
        if (rc != IV_OK) {
            body_len = (size_t)snprintf(body, body_cap,
                                        "{\"ok\":false,\"e\":%d,\"msg\":\"internal\"}", rc);
            rsp.status = 500;
            (void)snprintf(rsp.ctype, sizeof(rsp.ctype), "application/json");
        }
        build_response(c, &rsp, body_len);
        return;
    }

    rc = iv_web_api_handle(&g_ctx, req, body, body_cap, &body_len, &rsp);
    if (rc == IV_ENOTSUP)
        rc = iv_web_media_handle(&g_ctx, req, body, body_cap, &body_len, &rsp);
    if (rc == IV_ENOTSUP)
        rc = iv_web_static_handle(&g_ctx, req, body, body_cap, &body_len, &rsp);
    if (rc != IV_OK) {
        body_len = (size_t)snprintf(body, body_cap,
                                    "{\"ok\":false,\"e\":%d,\"msg\":\"internal\"}", rc);
        rsp.status = 500;
        (void)snprintf(rsp.ctype, sizeof(rsp.ctype), "application/json");
        rsp.set_cookie = 0;
    }

    /* 文件体不进 wbuf ⇒ 传给 build_response 的 body 长度取 0。 */
    build_response(c, &rsp, (rsp.file_path != NULL) ? 0u : body_len);
    c->last_ms = iv_web_now_ms();
}

/* 读事件：收字节 → 收到一条完整请求 → 处理 → 转写。 */
static void on_readable(int slot)
{
    iv_web_conn_t *c = &g_conn[slot];
    ssize_t        n;
    iv_web_req_t   req;
    int            prc;

    /* S6.4：文件体阶段 rbuf 正被当作发送块缓冲用，绝不能被 recv 覆盖。
     * 一连接一请求，此时再来数据（pipeline/探活）直接丢弃。 */
    if (c->send_fd >= 0)
        return;

    if (c->rlen >= sizeof(c->rbuf) - 1u) {
        /* 缓冲满还没成形：回 413 并准备关。**必须写进 wbuf** —— on_writable 只发
         * wbuf；早先写进 rbuf、发出去的却是 wbuf 里的旧内容/全零（集成用例 w08 实证）。 */
        c->wlen = (size_t)snprintf(c->wbuf, sizeof(c->wbuf),
                                   "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\n"
                                   "Connection: close\r\n\r\n");
        c->wsent = 0u;
        c->rlen = 0u;
        return;
    }

    n = recv(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - 1u - c->rlen, 0);
    if (n == 0) {
        conn_close(slot);
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        conn_close(slot);
        return;
    }
    c->rlen += (size_t)n;
    c->last_ms = iv_web_now_ms();

    prc = parse_req(c, &req);
    if (prc == IV_EAGAIN)
        return;
    if (prc != IV_OK) {
        /* 同 413 快路径：响应必须落在 wbuf（on_writable 只发 wbuf）。 */
        c->wlen = (size_t)snprintf(c->wbuf, sizeof(c->wbuf),
                                   "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                                   "Connection: close\r\n\r\n");
        c->wsent = 0u;
        c->rlen = 0u;
        return;
    }
    handle_req(c, &req);
}

/* S6.4 限速：返回"本轮最多能发多少字节"。0 表示本窗口配额已尽，
 * 已把"再等 IV_WEB_RL_WAIT_MS"记进 rl_wait_until_ms（主循环据此唤醒）。 */
static size_t rl_allow(iv_web_conn_t *c, uint64_t now, size_t want)
{
    uint64_t left;

    if (c->rl_bps == 0u)
        return want;                       /* 0 = 不限速 */
    if (now < c->rl_win_start_ms || now - c->rl_win_start_ms >= IV_WEB_RL_WIN_MS) {
        c->rl_win_start_ms = now;          /* 开新窗口，配额清零 */
        c->rl_sent = 0u;
    }
    if (c->rl_sent >= (uint64_t)c->rl_bps) {
        c->rl_wait_until_ms = now + IV_WEB_RL_WAIT_MS;
        c->rl_holds++;
        return 0u;
    }
    left = (uint64_t)c->rl_bps - c->rl_sent;
    return (left > (uint64_t)want) ? want : (size_t)left;
}

/* 写事件：先发响应头（wbuf），再流式发文件体（若有）。发完即关。
 * 客户端中途断开会让 send() 直接失败 ⇒ 关连接关 fd，**已发字节不回滚** ——
 * 这就是 S6.4 要求的"取消"的全部语义，不需要额外控制帧。 */
static void on_writable(int slot)
{
    iv_web_conn_t *c = &g_conn[slot];
    uint64_t       now = iv_web_now_ms();
    ssize_t        n;

    /* 1) 响应头（Content-Length/Content-Range/ETag…）。头很小，不参与限速。 */
    if (c->wsent < c->wlen) {
        n = send(c->fd, c->wbuf + c->wsent, c->wlen - c->wsent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            conn_close(slot);
            return;
        }
        c->wsent += (size_t)n;
        c->last_ms = now;
        if (c->wsent < c->wlen)
            return;
    }

    if (c->send_fd < 0) {
        conn_close(slot);                  /* 普通响应：头发完即结束 */
        return;
    }
    if (c->send_left == 0u) {
        conn_close(slot);
        return;
    }

    /* 2) 文件体：块缓冲复用 rbuf（请求已解析完，本阶段不会再读它）。 */
    if (c->flen == 0u || c->fsent >= c->flen) {
        size_t  want = (c->send_left > (uint64_t)IV_WEB_FILE_CHUNK)
                           ? (size_t)IV_WEB_FILE_CHUNK
                           : (size_t)c->send_left;
        ssize_t r = pread(c->send_fd, c->rbuf, want, (off_t)c->send_off);

        if (r <= 0) {
            /* 读失败或文件被截断：继续发会给出内容不完整的假成功，直接收连接。 */
            IV_LOG_W(WEB_MOD, "media read failed at off=%llu: %s",
                     (unsigned long long)c->send_off, strerror(errno));
            conn_close(slot);
            return;
        }
        c->flen  = (size_t)r;
        c->fsent = 0u;
    }

    {
        size_t allow = rl_allow(c, now, c->flen - c->fsent);

        if (allow == 0u)
            return;                        /* 配额尽：等 poll 超时唤醒 */
        n = send(c->fd, c->rbuf + c->fsent, allow, MSG_NOSIGNAL);
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        conn_close(slot);
        return;
    }
    c->fsent    += (size_t)n;
    c->send_off += (uint64_t)n;
    c->send_left = (c->send_left > (uint64_t)n) ? (c->send_left - (uint64_t)n) : 0u;
    c->rl_sent  += (uint64_t)n;
    c->last_ms   = now;

    if (c->send_left == 0u && c->fsent >= c->flen)
        conn_close(slot);
}

/* --------------------------------------------------------------------------
 * 监听
 * -------------------------------------------------------------------------- */
/* 监听指定地址与端口。ip_be 是"数值按大端读"的全工程约定值（0x7F000001 = 127.0.0.1）；
 * 0 = INADDR_ANY（主口）。sockaddr 要的是网络字节序内存布局，故这里 htonl 是**正确**的
 * —— 别与 iv_tunnel 渲染那处"勿 htonl"混淆：那边是取字节判断，这边是填 sockaddr。 */
static int make_listener_at(uint32_t ip_be, int port)
{
    struct sockaddr_in addr;
    int                fd;
    int                on = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = (ip_be == 0u) ? htonl(INADDR_ANY) : htonl(ip_be);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        (void)close(fd);
        return -1;
    }
    if (listen(fd, (int)IV_WEB_MAX_CONN) != 0) {
        (void)close(fd);
        return -1;
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static void do_accept(int lfd, int is_cam, unsigned cam_ch)
{
    for (;;) {
        int slot = conn_slot_free();
        int cfd;

        cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            return;
        }

        if (slot < 0) {
            /* 槽满：回 503 立刻关。**不排队**（见文件头说明）。 */
            static const char body[] =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n";

            (void)send(cfd, body, sizeof(body) - 1u, MSG_NOSIGNAL);
            (void)close(cfd);
            continue;
        }

        if (fcntl(cfd, F_SETFL, O_NONBLOCK) != 0) {
            (void)close(cfd);
            continue;
        }
        memset(&g_conn[slot], 0, sizeof(g_conn[slot]));
        g_conn[slot].fd = cfd;
        g_conn[slot].is_cam = (uint8_t)is_cam; /* 路由隔离的依据（S8.5） */
        g_conn[slot].cam_ch = (uint8_t)cam_ch; /* 该口就是第 cam_ch 路 */
        g_conn[slot].send_fd = -1;   /* memset 会把 send_fd 清成 0（＝stdin！） */
        g_conn[slot].last_ms = iv_web_now_ms();
    }
}

/* --------------------------------------------------------------------------
 * 入口
 * -------------------------------------------------------------------------- */
static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--port N] [--root DIR] [--chan PATH]\n"
            "          [--user NAME] [--pass PW] [--pass-file PATH]\n"
            "          [--media-root DIR] [--media-bps N]\n"
            "          [--log-root DIR]\n"
            "          [--ota-root DIR]\n"
            "          [--cam-port N] [--cam-upstream N]\n"
            "          [--heartbeat PATH] [--heartbeat-ms N] [--version]\n"
            "  defaults: port %d, root %s, chan %s\n"
            "            user %s, pass = factory default (must be changed on first login)\n"
            "            pass-file %s (empty = do not persist)\n"
            "            media-root %s, media-bps %u (0 = unlimited)\n"
            "            log-root %s\n"
            "            ota-root %s\n"
            "            cam-port %d (127.0.0.1 only, 0 = off), cam-upstream %u\n",
            argv0, WEB_PORT_DEF, WEB_ROOT_DEF, IV_CHAN_PATH_DEFAULT,
            WEB_USER_DEF, IV_WEB_PASS_FILE_DEF,
            IV_WEB_MEDIA_ROOT_DEFAULT, IV_WEB_MEDIA_BPS_DEFAULT,
            IV_WEB_LOG_ROOT_DEFAULT,
            IV_WEB_OTA_ROOT_DEFAULT,
            WEB_CAM_PORT_DEF, (unsigned)WEB_CAM_UPSTREAM_DEF);
}

int main(int argc, char *argv[])
{
    int port = WEB_PORT_DEF;
    int cam_port = WEB_CAM_PORT_DEF;
    int i;
    int n;

    /* 静态数组默认 0 == stdin，必须显式置 -1（否则没开的口会被当成可 poll 的 fd）。 */
    for (i = 0; i < WEB_CAM_CH_MAX; i++)
        g_cam_fd[i] = -1;
    g_ctx.chan_path   = IV_CHAN_PATH_DEFAULT;
    g_ctx.static_root = WEB_ROOT_DEF;
    g_ctx.chan_fd     = -1;
    g_ctx.cam_port    = cam_port;          /* 环回护栏用（iv_web_cam.c） */
    g_ctx.cam_upstream = WEB_CAM_UPSTREAM_DEF;
    (void)snprintf(g_ctx.user, sizeof(g_ctx.user), "%s", WEB_USER_DEF);
    (void)snprintf(g_ctx.pass, sizeof(g_ctx.pass), "%s", WEB_PASS_DEF);
    (void)snprintf(g_ctx.pass_file, sizeof(g_ctx.pass_file), "%s", IV_WEB_PASS_FILE_DEF);
    (void)snprintf(g_ctx.media_root, sizeof(g_ctx.media_root), "%s",
                   IV_WEB_MEDIA_ROOT_DEFAULT);
    g_ctx.media_bps = IV_WEB_MEDIA_BPS_DEFAULT;
    (void)snprintf(g_ctx.log_root, sizeof(g_ctx.log_root), "%s", IV_WEB_LOG_ROOT_DEFAULT);
    (void)snprintf(g_ctx.ota_root, sizeof(g_ctx.ota_root), "%s", IV_WEB_OTA_ROOT_DEFAULT);
    g_ctx.rng = (uint32_t)((uint64_t)time(NULL) ^ (uint64_t)getpid()) | 1u;

    for (i = 0; i < (int)IV_WEB_MAX_CONN; i++) {
        g_conn[i].fd = -1;
        g_conn[i].send_fd = -1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("ivsbox-web\n");
            return 0;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            g_ctx.static_root = argv[++i];
        } else if (strcmp(argv[i], "--chan") == 0 && i + 1 < argc) {
            g_ctx.chan_path = argv[++i];
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            (void)snprintf(g_ctx.user, sizeof(g_ctx.user), "%.*s",
                           (int)sizeof(g_ctx.user) - 1, argv[++i]);
        } else if (strcmp(argv[i], "--pass") == 0 && i + 1 < argc) {
            /* M10 起 `--pass` 是**初始口令**（改过密码后以 `<secure>/web.passwd` 为准）。 */
            (void)snprintf(g_ctx.pass, sizeof(g_ctx.pass), "%.*s",
                           (int)sizeof(g_ctx.pass) - 1, argv[++i]);
        } else if (strcmp(argv[i], "--pass-file") == 0 && i + 1 < argc) {
            /* 空串 = 不落盘（验证用；改密只在内存生效）。 */
            (void)snprintf(g_ctx.pass_file, sizeof(g_ctx.pass_file), "%.*s",
                           (int)sizeof(g_ctx.pass_file) - 1, argv[++i]);
        } else if (strcmp(argv[i], "--media-root") == 0 && i + 1 < argc) {
            (void)snprintf(g_ctx.media_root, sizeof(g_ctx.media_root), "%.*s",
                           (int)sizeof(g_ctx.media_root) - 1, argv[++i]);
        } else if (strcmp(argv[i], "--media-bps") == 0 && i + 1 < argc) {
            g_ctx.media_bps = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--log-root") == 0 && i + 1 < argc) {
            (void)snprintf(g_ctx.log_root, sizeof(g_ctx.log_root), "%.*s",
                           (int)sizeof(g_ctx.log_root) - 1, argv[++i]);
        } else if (strcmp(argv[i], "--ota-root") == 0 && i + 1 < argc) {
            (void)snprintf(g_ctx.ota_root, sizeof(g_ctx.ota_root), "%.*s",
                           (int)sizeof(g_ctx.ota_root) - 1, argv[++i]);
        } else if (strcmp(argv[i], "--cam-port") == 0 && i + 1 < argc) {
            cam_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cam-upstream") == 0 && i + 1 < argc) {
            g_ctx.cam_upstream = (uint16_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--heartbeat") == 0 && i + 1 < argc) {
            g_hb_path = argv[++i];
        } else if (strcmp(argv[i], "--heartbeat-ms") == 0 && i + 1 < argc) {
            g_hb_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (port <= 0 || port > 65535) {
        usage(argv[0]);
        return 1;
    }
    if (cam_port < 0 || cam_port > 65535) {
        usage(argv[0]);
        return 1;
    }
    g_ctx.cam_port = cam_port; /* CLI 终值（环回护栏与监听同源，不许分叉） */
    {
        const char *env = getenv("IVSBOX_WEB_USER");

        if (env != NULL && env[0] != '\0')
            (void)snprintf(g_ctx.user, sizeof(g_ctx.user), "%.*s",
                           (int)sizeof(g_ctx.user) - 1, env);
        env = getenv("IVSBOX_WEB_PASS");
        if (env != NULL && env[0] != '\0')
            (void)snprintf(g_ctx.pass, sizeof(g_ctx.pass), "%.*s",
                           (int)sizeof(g_ctx.pass) - 1, env);
        env = getenv("IVSBOX_WEB_PASS_FILE");
        if (env != NULL)
            (void)snprintf(g_ctx.pass_file, sizeof(g_ctx.pass_file), "%.*s",
                           (int)sizeof(g_ctx.pass_file) - 1, env);
        env = getenv("IVSBOX_WEB_MEDIA_ROOT");
        if (env != NULL && env[0] != '\0')
            (void)snprintf(g_ctx.media_root, sizeof(g_ctx.media_root), "%.*s",
                           (int)sizeof(g_ctx.media_root) - 1, env);
        env = getenv("IVSBOX_WEB_MEDIA_BPS");
        if (env != NULL && env[0] != '\0')
            g_ctx.media_bps = (uint32_t)strtoul(env, NULL, 10);
        env = getenv("IVSBOX_WEB_LOG_ROOT");
        if (env != NULL && env[0] != '\0')
            (void)snprintf(g_ctx.log_root, sizeof(g_ctx.log_root), "%.*s",
                           (int)sizeof(g_ctx.log_root) - 1, env);
        env = getenv("IVSBOX_WEB_OTA_ROOT");
        if (env != NULL && env[0] != '\0')
            (void)snprintf(g_ctx.ota_root, sizeof(g_ctx.ota_root), "%.*s",
                           (int)sizeof(g_ctx.ota_root) - 1, env);
        /* 心跳路径/周期（S7.4）：monitor-web.sh 用同名变量，两边必须指同一个文件。 */
        env = getenv("IVSBOX_WEB_HEARTBEAT");
        if (env != NULL && env[0] != '\0')
            g_hb_path = env;
        env = getenv("IVSBOX_WEB_HEARTBEAT_MS");
        if (env != NULL && env[0] != '\0')
            g_hb_ms = (uint32_t)strtoul(env, NULL, 10);
        /* S8.5：摄像机代理监听/上游端口。cam-port 0 = 关闭代理入口。
         * 环境变量在 CLI 之后生效口径与其它键一致：没设就用 CLI 值。 */
        env = getenv("IVSBOX_WEB_CAM_PORT");
        if (env != NULL && env[0] != '\0') {
            cam_port = atoi(env);
            if (cam_port < 0 || cam_port > 65535) {
                usage("ivsbox-web");
                return 1;
            }
            g_ctx.cam_port = cam_port;
        }
        env = getenv("IVSBOX_WEB_CAM_UPSTREAM");
        if (env != NULL && env[0] != '\0')
            g_ctx.cam_upstream = (uint16_t)strtoul(env, NULL, 10);
    }

    /* 日志失败不致命：host 上没有 /mnt/UDISK/log 是常态，Web 照样要能跑。 */
    (void)iv_log_init("ivsbox-web");
    iv_log_set_stderr(1);

    /* M10：口令哈希化 + 读盘（决定 must_change）。放在 listen 之前 ——
     * 监听一旦建立就可能有登录请求，那时 pass_hash 必须是终值；放日志初始化之后
     * 只是为了让"密码文件读不动"的告警真的写得出去。 */
    (void)iv_web_pass_init(&g_ctx);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    g_listen_fd = make_listener_at(0u, port);
    if (g_listen_fd < 0) {
        IV_LOG_E(WEB_MOD, "listen on port %d failed: %s", port, strerror(errno));
        fprintf(stderr, "ivsbox-web: listen on port %d failed: %s\n",
                port, strerror(errno));
        return 1;
    }
    /* S8.5：摄像机代理口 —— **每路一个，只绑 127.0.0.1**，局域网直接不可达；
     * 入口只有本机进程与持钥匙的 frp stcp visitor。N 路口 = cam_port + N，
     * 该口的根路径就是第 N 路摄像机的根（见 iv_web.h 的两种寻址形态）。0 = 关闭。
     * 第 0 路绑不上属于致命错（配置写明了要开）；其余路绑不上只告警跳过 ——
     * 现场可能只关心某几路，不该因为端口冲突整体起不来。 */
    if (g_ctx.cam_port > 0) {
        unsigned k;

        for (k = 0u; k < (unsigned)WEB_CAM_CH_MAX; k++) {
            int lp = g_ctx.cam_port + (int)k;

            if (lp > 65535)
                break;
            g_cam_fd[k] = make_listener_at(0x7f000001u, (uint16_t)lp);
            if (g_cam_fd[k] < 0) {
                IV_LOG_W(WEB_MOD, "cam listen on 127.0.0.1:%d (ch%u) failed: %s", lp, k,
                         strerror(errno));
                if (k == 0u) {
                    fprintf(stderr, "ivsbox-web: cam listen on 127.0.0.1:%d failed: %s\n",
                            lp, strerror(errno));
                    return 1;
                }
            }
        }
    }
    IV_LOG_I(WEB_MOD, "ivsbox-web listening on :%d, root=%s, chan=%s",
             port, g_ctx.static_root, g_ctx.chan_path);
    IV_LOG_I(WEB_MOD, "media root=%s bps=%u", g_ctx.media_root, g_ctx.media_bps);
    IV_LOG_I(WEB_MOD, "log root=%s", g_ctx.log_root);
    IV_LOG_I(WEB_MOD, "ota root=%s (package upload target)", g_ctx.ota_root);
    IV_LOG_I(WEB_MOD, "cam proxy %s (upstream port %u, ports %d..%d)",
             (g_cam_fd[0] >= 0) ? "enabled" : "off", (unsigned)g_ctx.cam_upstream,
             g_ctx.cam_port, g_ctx.cam_port + WEB_CAM_CH_MAX - 1);
    /* 只报"要不要改密"与落点，**绝不报口令/哈希**（日志会进 /mnt/UDISK/log）。 */
    IV_LOG_I(WEB_MOD, "auth user=%s must_change=%d pass_file=%s",
             g_ctx.user, g_ctx.must_change,
             (g_ctx.pass_file[0] != '\0') ? g_ctx.pass_file : "(none)");

    while (!g_stop) {
        struct pollfd pf[IV_WEB_MAX_CONN + 2 + WEB_CAM_CH_MAX];
        int           map[IV_WEB_MAX_CONN + 2 + WEB_CAM_CH_MAX];
        int           nfds = 0;
        int           timeout = 1000;
        uint64_t      now;

        /* 主循环心跳（S7.4）：放在本圈最前面 —— 无论本圈是立刻返回还是等满
         * timeout（上限 1000 ms），心跳都随循环前进；循环一停，心跳即停。 */
        heartbeat_tick();

        pf[0].fd      = g_listen_fd;
        pf[0].events  = POLLIN;
        pf[0].revents = 0;
        map[0] = -1;
        nfds = 1;

        /* S8.5：摄像机代理口（每路一个）。map = -(2+N) 区分来源 ⇒ accept 时打
         * is_cam 与 cam_ch 标记。 */
        {
            unsigned k;

            for (k = 0u; k < (unsigned)WEB_CAM_CH_MAX; k++) {
                if (g_cam_fd[k] < 0)
                    continue;
                pf[nfds].fd      = g_cam_fd[k];
                pf[nfds].events  = POLLIN;
                pf[nfds].revents = 0;
                map[nfds] = -(int)(2u + k);
                nfds++;
            }
        }

        now = iv_web_now_ms();
        for (i = 0; i < (int)IV_WEB_MAX_CONN; i++) {
            int pending;
            int waiting;

            if (g_conn[i].fd < 0)
                continue;
            /* S6.4：还欠客户端字节 = 响应头没发完，或文件体还没收尾。 */
            pending = (g_conn[i].wsent < g_conn[i].wlen) || (g_conn[i].send_fd >= 0);
            /* 限速配额用尽：本轮**不注册 POLLOUT**（POLLOUT 电平触发会立刻再报，
             * 空转烧 CPU），改为把等待截止折进 poll 超时，由超时唤醒。 */
            waiting = pending && (g_conn[i].rl_wait_until_ms > now);
            pf[nfds].fd      = g_conn[i].fd;
            pf[nfds].events  = pending ? (waiting ? 0 : POLLOUT) : POLLIN;
            pf[nfds].revents = 0;
            map[nfds] = i;
            if (waiting) {
                uint64_t d  = g_conn[i].rl_wait_until_ms - now;
                int      di = (d > 1000u) ? 1000 : (int)d;

                if (di < 1)
                    di = 1;
                if (di < timeout)
                    timeout = di;
            }
            nfds++;
        }

        n = poll(pf, (nfds_t)nfds, timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            IV_LOG_E(WEB_MOD, "poll failed: %s", strerror(errno));
            break;
        }

        for (i = 0; i < nfds; i++) {
            if (pf[i].revents == 0)
                continue;
            if (map[i] < 0) {
                if (pf[i].revents & POLLIN) {
                    if (map[i] <= -2) {
                        unsigned k = (unsigned)(-(map[i] + 2));

                        if (k < (unsigned)WEB_CAM_CH_MAX && g_cam_fd[k] >= 0)
                            do_accept(g_cam_fd[k], 1, k);
                    } else {
                        do_accept(g_listen_fd, 0, 0u);
                    }
                }
                continue;
            }
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                conn_close(map[i]);
                continue;
            }
            if (pf[i].revents & POLLIN) {
                on_readable(map[i]);
                /* on_readable 可能已经把 fd 关掉（槽变空闲）：下面按槽是否活着
                 * 再判一次，避免对已关闭的 fd 做写操作。 */
                if (g_conn[map[i]].fd < 0)
                    continue;
            }
            if (pf[i].revents & POLLOUT)
                on_writable(map[i]);
        }

        /* 空闲超时回收 */
        now = iv_web_now_ms();
        for (i = 0; i < (int)IV_WEB_MAX_CONN; i++) {
            if (g_conn[i].fd >= 0 && now - g_conn[i].last_ms > IV_WEB_IDLE_MS)
                conn_close(i);
        }
    }

    for (i = 0; i < (int)IV_WEB_MAX_CONN; i++)
        conn_close(i);
    if (g_listen_fd >= 0)
        (void)close(g_listen_fd);
    {
        unsigned k;

        for (k = 0u; k < (unsigned)WEB_CAM_CH_MAX; k++) {
            if (g_cam_fd[k] >= 0)
                (void)close(g_cam_fd[k]);
        }
    }
    iv_web_chan_close(&g_ctx);
    IV_LOG_I(WEB_MOD, "ivsbox-web stopped");
    return 0;
}
