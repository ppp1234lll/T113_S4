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
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "iv_web.h"

#define WEB_MOD         "web"
#define WEB_PORT_DEF    80
#define WEB_ROOT_DEF    "/opt/ivsbox/web/static"
#define WEB_PASS_DEF    "admin"
/* 响应头预留区：所有响应头都很短（固定 4 行 + 可选 Set-Cookie），512 绰绰有余。 */
#define RSP_HEAD_OFF    512u

static iv_web_ctx_t  g_ctx;
static iv_web_conn_t g_conn[IV_WEB_MAX_CONN];
static int           g_listen_fd = -1;
static volatile sig_atomic_t g_stop = 0;

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
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
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
        } else if (strncasecmp(line, "Cookie:", 7) == 0) {
            const char *sid = strstr(line + 7, "ivs_sid=");

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
    char head[RSP_HEAD_OFF];
    int  n;

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "Cache-Control: no-store\r\n"
                 "%s%s%s"
                 "\r\n",
                 rsp->status, status_text(rsp->status), rsp->ctype, body_len,
                 rsp->set_cookie ? "Set-Cookie: " : "",
                 rsp->set_cookie ? rsp->cookie : "",
                 rsp->set_cookie ? "\r\n" : "");
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

    rc = iv_web_api_handle(&g_ctx, req, body, body_cap, &body_len, &rsp);
    if (rc == IV_ENOTSUP)
        rc = iv_web_static_handle(&g_ctx, req, body, body_cap, &body_len, &rsp);
    if (rc != IV_OK) {
        body_len = (size_t)snprintf(body, body_cap,
                                    "{\"ok\":false,\"e\":%d,\"msg\":\"internal\"}", rc);
        rsp.status = 500;
        (void)snprintf(rsp.ctype, sizeof(rsp.ctype), "application/json");
        rsp.set_cookie = 0;
    }

    build_response(c, &rsp, body_len);
    c->last_ms = iv_web_now_ms();
}

/* 读事件：收字节 → 收到一条完整请求 → 处理 → 转写。 */
static void on_readable(int slot)
{
    iv_web_conn_t *c = &g_conn[slot];
    ssize_t        n;
    iv_web_req_t   req;
    int            prc;

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

/* 写事件：把 wbuf 里剩下的发完即关。 */
static void on_writable(int slot)
{
    iv_web_conn_t *c = &g_conn[slot];
    ssize_t        n;

    if (c->wsent >= c->wlen) {
        conn_close(slot);
        return;
    }
    n = send(c->fd, c->wbuf + c->wsent, c->wlen - c->wsent, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        conn_close(slot);
        return;
    }
    c->wsent += (size_t)n;
    c->last_ms = iv_web_now_ms();
    if (c->wsent >= c->wlen)
        conn_close(slot);
}

/* --------------------------------------------------------------------------
 * 监听
 * -------------------------------------------------------------------------- */
static int make_listener(int port)
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
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
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

static void do_accept(void)
{
    for (;;) {
        int slot = conn_slot_free();
        int cfd;

        cfd = accept(g_listen_fd, NULL, NULL);
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
        g_conn[slot].last_ms = iv_web_now_ms();
    }
}

/* --------------------------------------------------------------------------
 * 入口
 * -------------------------------------------------------------------------- */
static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--port N] [--root DIR] [--chan PATH] [--pass PW] [--version]\n"
            "  defaults: port %d, root %s, chan %s\n",
            argv0, WEB_PORT_DEF, WEB_ROOT_DEF, IV_CHAN_PATH_DEFAULT);
}

int main(int argc, char *argv[])
{
    int port = WEB_PORT_DEF;
    int i;
    int n;

    g_ctx.chan_path   = IV_CHAN_PATH_DEFAULT;
    g_ctx.static_root = WEB_ROOT_DEF;
    g_ctx.chan_fd     = -1;
    (void)snprintf(g_ctx.pass, sizeof(g_ctx.pass), "%s", WEB_PASS_DEF);
    g_ctx.rng = (uint32_t)((uint64_t)time(NULL) ^ (uint64_t)getpid()) | 1u;

    for (i = 0; i < (int)IV_WEB_MAX_CONN; i++)
        g_conn[i].fd = -1;

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
        } else if (strcmp(argv[i], "--pass") == 0 && i + 1 < argc) {
            (void)snprintf(g_ctx.pass, sizeof(g_ctx.pass), "%.*s", 63, argv[++i]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (port <= 0 || port > 65535) {
        usage(argv[0]);
        return 1;
    }
    {
        const char *env = getenv("IVSBOX_WEB_PASS");

        if (env != NULL && env[0] != '\0')
            (void)snprintf(g_ctx.pass, sizeof(g_ctx.pass), "%.*s", 63, env);
    }

    /* 日志失败不致命：host 上没有 /mnt/UDISK/log 是常态，Web 照样要能跑。 */
    (void)iv_log_init("ivsbox-web");
    iv_log_set_stderr(1);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    g_listen_fd = make_listener(port);
    if (g_listen_fd < 0) {
        IV_LOG_E(WEB_MOD, "listen on port %d failed: %s", port, strerror(errno));
        fprintf(stderr, "ivsbox-web: listen on port %d failed: %s\n",
                port, strerror(errno));
        return 1;
    }
    IV_LOG_I(WEB_MOD, "ivsbox-web listening on :%d, root=%s, chan=%s",
             port, g_ctx.static_root, g_ctx.chan_path);

    while (!g_stop) {
        struct pollfd pf[IV_WEB_MAX_CONN + 1];
        int           map[IV_WEB_MAX_CONN + 1];
        int           nfds = 0;
        uint64_t      now;

        pf[0].fd      = g_listen_fd;
        pf[0].events  = POLLIN;
        pf[0].revents = 0;
        map[0] = -1;
        nfds = 1;

        for (i = 0; i < (int)IV_WEB_MAX_CONN; i++) {
            if (g_conn[i].fd < 0)
                continue;
            pf[nfds].fd      = g_conn[i].fd;
            pf[nfds].events  = (g_conn[i].wlen > 0u) ? POLLOUT : POLLIN;
            pf[nfds].revents = 0;
            map[nfds] = i;
            nfds++;
        }

        n = poll(pf, (nfds_t)nfds, 1000);
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
                if (pf[i].revents & POLLIN)
                    do_accept();
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
    iv_web_chan_close(&g_ctx);
    IV_LOG_I(WEB_MOD, "ivsbox-web stopped");
    return 0;
}
