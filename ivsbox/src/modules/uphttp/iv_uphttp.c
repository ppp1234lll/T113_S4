/*
 * 平台 HTTP 上传客户端实现 —— start / chunk(整块一片) / finish，契约见 iv_uphttp.h
 *
 * 结构一句话：一条 keep-alive 连接上依次发 start、**一次** chunk（body 由
 * begin/write/end 三段流式发出）、finish；每一步都把应答**按 Content-Length
 * 读满**再往下走。重试留给调用者，本层只负责"一次上传做对"。
 *
 * 三个必须记住的口径：
 *   1) 应答必须读满：只读到 `\r\n\r\n` 就用，下一请求的应答会从上一请求的残留
 *      body 开始解析，`code` 永远读不到 200。
 *   2) **1 KiB 一片是单片机的约束，不是服务端的**（2026-11-03 真机实测：同一张
 *      164744 B 的图，1 片 2.2 s、64 KiB×3 2.2 s、1 KiB×161 17.6 s，三份文件
 *      md5 全等于原图）。现在固定 `totalChunks=1`，一个文件一次请求。
 *   3) 分片号与申报的 totalChunks 必须自洽（服务端按 `chunkIndex` 摞字节，多一片
 *      回"分片序号无效"）；整块传时就是 0/1，不会踩到。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* SO_BINDTODEVICE / getaddrinfo 的 GNU 扩展 */
#endif

#include "ivsbox/iv_uphttp.h"

#include "ivsbox/iv_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define UU_MOD "uphttp"

/* 参考实现的请求头/表单缓冲口径（`upload_http.c`） */
#define UU_BOUNDARY    "----FNLOG_UPLOAD_BOUNDARY"
#define UU_API_CODE_OK 200 /* 服务端接口 JSON 的 `code` 成功值 */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (dst == NULL || cap == 0u)
        return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap)
        n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---------------------------------------------------------------------------
 * 连接（阻塞、带超时）
 * ------------------------------------------------------------------------- */

static void conn_close(iv_uphttp_t *u)
{
    if (u->fd >= 0) {
        (void)close(u->fd);
        u->fd = -1;
    }
}

static void set_timeo(int fd, int opt, uint32_t ms)
{
    struct timeval tv;

    tv.tv_sec  = (time_t)(ms / 1000u);
    tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
    (void)setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv);
}

static int conn_open(iv_uphttp_t *u)
{
    struct addrinfo  hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p;
    char             pstr[8];
    int              rc;

    if (u->host[0] == '\0')
        return IV_EINVAL;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    (void)snprintf(pstr, sizeof pstr, "%u", (unsigned)u->port);
    rc = getaddrinfo(u->host, pstr, &hints, &res);
    if (rc != 0 || res == NULL) {
        IV_LOG_W(UU_MOD, "resolve %s failed: %s", u->host, gai_strerror(rc));
        return IV_ECONN;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        int       fd;
        int       err = 0;
        int       fl;
        socklen_t elen = sizeof err;

        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        /* 出口绑定必须在 connect 前：双 WAN 同时在线时，只靠路由会把上传从
         * 不通的那条链路发出去（传输层踩过同一个坑，见 iv_transport）。 */
        if (u->bind_dev[0] != '\0') {
            if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, u->bind_dev,
                           (socklen_t)(strlen(u->bind_dev) + 1u)) != 0) {
                IV_LOG_W(UU_MOD, "bind %s failed (%s), fallback to route",
                         u->bind_dev, strerror(errno));
            }
        }
        fl = fcntl(fd, F_GETFL, 0);
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd;

            pfd.fd     = fd;
            pfd.events = POLLOUT;
            rc         = poll(&pfd, 1, (int)u->connect_ms);
            if (rc == 0) {
                IV_LOG_W(UU_MOD, "connect %s:%u timeout", u->host, (unsigned)u->port);
                (void)close(fd);
                continue;
            }
            if (rc < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 ||
                err != 0) {
                (void)close(fd);
                continue;
            }
        } else if (rc != 0) {
            (void)close(fd);
            continue;
        }
        (void)fcntl(fd, F_SETFL, fl); /* 回到阻塞；超时交给 SO_*TIMEO */
        set_timeo(fd, SO_SNDTIMEO, u->send_ms);
        set_timeo(fd, SO_RCVTIMEO, u->recv_ms);
        u->fd             = fd;
        u->need_reconnect = 0;
        (void)freeaddrinfo(res);
        return IV_OK;
    }

    (void)freeaddrinfo(res);
    return IV_ECONN;
}

static int ensure_conn(iv_uphttp_t *u)
{
    if (u->fd >= 0 && u->need_reconnect == 0)
        return IV_OK;
    conn_close(u);
    return conn_open(u);
}

static int send_all(iv_uphttp_t *u, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0u) {
        ssize_t n = send(u->fd, p, len, 0);

        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return IV_ECONN;
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 应答读取
 * ------------------------------------------------------------------------- */

/* 应答是否收完（参考 `upload_http_response_complete()` 的同口径） */
static int resp_complete(const iv_uphttp_t *u)
{
    const char *he;
    const char *p;

    he = strstr(u->resp, "\r\n\r\n");
    if (he == NULL)
        return 0;
    p = strstr(u->resp, "Transfer-Encoding:");
    if (p != NULL && p < he)
        return (strstr(he, "\r\n0\r\n\r\n") != NULL) ? 1 : 0;
    p = strstr(u->resp, "Content-Length:");
    if (p == NULL || p > he)
        return 1; /* 无长度信息：视为已收完（参考同口径） */
    {
        long want = strtol(p + 15, NULL, 10);
        long got  = (long)u->resp_len - (long)(he + 4 - u->resp);

        if (want < 0)
            return 1;
        return got >= want;
    }
}

static int read_resp(iv_uphttp_t *u)
{
    const char *sp;
    const char *he;

    u->resp_len  = 0u;
    u->resp[0]   = '\0';
    u->status    = 0;
    u->resp_body = u->resp;

    for (;;) {
        ssize_t n;

        if (resp_complete(u))
            break;
        if (u->resp_len + 1u >= sizeof u->resp) {
            /* 缓冲满还没收完：应答异常（或服务端回了超大 JSON）——
             * 判错并重连，别把半截 JSON 当完整去解析 */
            u->need_reconnect = 1;
            return IV_EPROTO;
        }
        n = recv(u->fd, u->resp + u->resp_len, sizeof u->resp - 1u - u->resp_len, 0);
        if (n > 0) {
            u->resp_len += (size_t)n;
            u->resp[u->resp_len] = '\0';
            continue;
        }
        if (n == 0) {
            u->need_reconnect = 1; /* 对端关连接：已收到的头仍可用 */
            break;
        }
        if (errno == EINTR)
            continue;
        /* EAGAIN/EWOULDBLOCK = SO_RCVTIMEO 到期 */
        u->need_reconnect = 1;
        if (!resp_complete(u))
            return IV_ETIMEDOUT;
        break;
    }

    sp = strstr(u->resp, "HTTP/");
    if (sp == NULL || sscanf(sp, "HTTP/%*d.%*d %d", &u->status) != 1) {
        u->need_reconnect = 1;
        return IV_EPROTO;
    }
    he = strstr(u->resp, "\r\n\r\n");
    u->resp_body = (he != NULL) ? (he + 4) : (u->resp + u->resp_len);
    /* `Connection: close` 只看**头**：body 里出现 "close" 是业务字段，
     * 拿整个应答去 strstr 会在 JSON 含该词时误判成"要重连"。 */
    if (he != NULL) {
        char *mut  = (char *)he; /* 只在本次判断里临时截断，立刻还原 */
        char  save = *mut;

        *mut = '\0';
        if (strstr(u->resp, "Connection: close") != NULL ||
            strstr(u->resp, "Connection:close") != NULL) {
            u->need_reconnect = 1;
        }
        *mut = save;
    }
    return IV_OK;
}

/* 从应答 JSON 里取整型字段（直接在原始应答里搜，不引 json-c） */
static int body_int(const iv_uphttp_t *u, const char *key, int *out)
{
    char        pat[32];
    const char *p;

    if (u->resp_body == NULL)
        return IV_ENOENT;
    (void)snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(u->resp_body, pat);
    if (p == NULL)
        return IV_ENOENT;
    p = strchr(p, ':');
    if (p == NULL)
        return IV_ENOENT;
    return (sscanf(p + 1, "%d", out) == 1) ? IV_OK : IV_ENOENT;
}

static int body_str(const iv_uphttp_t *u, const char *key, char *out, size_t cap)
{
    char        pat[32];
    const char *p;
    const char *e;
    size_t      n;

    if (u->resp_body == NULL || out == NULL || cap == 0u)
        return IV_EINVAL;
    (void)snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(u->resp_body, pat);
    if (p == NULL)
        return IV_ENOENT;
    p = strchr(p, ':');
    if (p == NULL)
        return IV_ENOENT;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"')
        p++;
    e = p;
    while (*e != '\0' && *e != '"' && *e != ',' && *e != '}')
        e++;
    n = (size_t)(e - p);
    if (n == 0u || n >= cap)
        return IV_ERANGE;
    memcpy(out, p, n);
    out[n] = '\0';
    return IV_OK;
}

static int api_ok(const iv_uphttp_t *u)
{
    int code = 0;

    return (body_int(u, "code", &code) == IV_OK && code == UU_API_CODE_OK) ? IV_OK
                                                                          : IV_EPROTO;
}

/* 无 body 的 POST（start / finish） */
static int post_query(iv_uphttp_t *u, const char *path, int keep_alive)
{
    int n;

    if (ensure_conn(u) != IV_OK)
        return IV_ECONN;
    n = snprintf(u->hdr, sizeof u->hdr,
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: %s\r\n\r\n",
                 path, u->host, (unsigned)u->port, keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= sizeof u->hdr)
        return IV_ERANGE;
    if (send_all(u, u->hdr, (size_t)n) != IV_OK) {
        u->need_reconnect = 1;
        return IV_ECONN;
    }
    if (read_resp(u) != IV_OK)
        return u->need_reconnect ? IV_ECONN : IV_ETIMEDOUT;
    return (u->status >= 200 && u->status < 300) ? IV_OK : IV_EPROTO;
}

/* 一片数据（现在是**唯一**一片）：请求头 + multipart 前缀先发出去，剩下的
 * 内容由 `iv_uphttp_write()` 逐段发，`iv_uphttp_end()` 补尾并收 finish 应答。
 * 三段的 Content-Length 必须一次算准：`前缀 + total + 尾`。 */
int iv_uphttp_begin(iv_uphttp_t *h)
{
    size_t plen;
    size_t tlen;
    int    n;

    if (h == NULL || !h->inited)
        return IV_EINVAL;
    if (h->upload_id[0] == '\0')
        return IV_ESTATE; /* 还没 start */
    if (h->sending)
        return IV_ESTATE; /* 上一次还没 end */
    if (h->total == 0u)
        return IV_EINVAL;

    n = snprintf(h->pre, sizeof h->pre,
                 "--%s\r\nContent-Disposition: form-data; name=\"uploadId\"\r\n\r\n%s\r\n"
                 "--%s\r\nContent-Disposition: form-data; name=\"chunkIndex\"\r\n\r\n0\r\n"
                 "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n",
                 UU_BOUNDARY, h->upload_id, UU_BOUNDARY, UU_BOUNDARY, h->file_name);
    if (n < 0 || (size_t)n >= sizeof h->pre)
        return IV_ERANGE;
    plen = (size_t)n;

    n = snprintf(h->tail, sizeof h->tail, "\r\n--%s--\r\n", UU_BOUNDARY);
    if (n < 0 || (size_t)n >= sizeof h->tail)
        return IV_ERANGE;
    tlen = (size_t)n;

    if (ensure_conn(h) != IV_OK)
        return IV_ECONN;
    n = snprintf(h->hdr, sizeof h->hdr,
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Content-Type: multipart/form-data; boundary=%s\r\n"
                 "Content-Length: %lu\r\n"
                 "Connection: keep-alive\r\n\r\n",
                 IV_UPHTTP_PATH_CHUNK, h->host, (unsigned)h->port, UU_BOUNDARY,
                 (unsigned long)(plen + (size_t)h->total + tlen));
    if (n < 0 || (size_t)n >= sizeof h->hdr)
        return IV_ERANGE;
    if (send_all(h, h->hdr, (size_t)n) != IV_OK ||
        send_all(h, h->pre, plen) != IV_OK) {
        h->need_reconnect = 1;
        return IV_ECONN;
    }
    h->left    = h->total;
    h->sending = 1;
    return IV_OK;
}

int iv_uphttp_write(iv_uphttp_t *h, const void *data, size_t len)
{
    if (h == NULL || !h->inited || data == NULL)
        return IV_EINVAL;
    if (!h->sending)
        return IV_ESTATE;
    if (len > (size_t)h->left)
        return IV_ERANGE;
    if (send_all(h, data, len) != IV_OK) {
        h->need_reconnect = 1;
        h->sending        = 0;
        return IV_ECONN;
    }
    h->left -= (uint32_t)len;
    return IV_OK;
}

int iv_uphttp_end(iv_uphttp_t *h, char *url, size_t cap)
{
    char path[256];
    size_t tlen;
    int  n;
    int  rc;

    if (h == NULL || !h->inited)
        return IV_EINVAL;
    if (!h->sending)
        return IV_ESTATE;
    if (h->left != 0u)
        return IV_ESTATE; /* 少发了：宁可失败，也不让服务端存半个文件 */
    tlen = strlen(h->tail);
    if (send_all(h, h->tail, tlen) != IV_OK) {
        h->need_reconnect = 1;
        h->sending        = 0;
        return IV_ECONN;
    }
    h->sending = 0;

    /* chunk 请求的应答（"分片上传成功"） */
    if (read_resp(h) != IV_OK)
        return h->need_reconnect ? IV_ECONN : IV_ETIMEDOUT;
    if (h->status < 200 || h->status >= 300 || api_ok(h) != IV_OK)
        return IV_EPROTO;

    /* finish：触发服务端拼片并转存 OSS，链接只在这一次应答里 */
    n = snprintf(path, sizeof path, "%s?uploadId=%s", IV_UPHTTP_PATH_FINISH,
                 h->upload_id);
    if (n < 0 || (size_t)n >= sizeof path)
        return IV_ERANGE;
    rc = post_query(h, path, 0);
    if (rc != IV_OK)
        return rc;
    if (api_ok(h) != IV_OK)
        return IV_EPROTO;
    if (url != NULL && cap > 0u)
        (void)body_str(h, "data", url, cap); /* 取不到不算失败 */
    return IV_OK;
}

int iv_uphttp_put_fd(iv_uphttp_t *h, int fd, char *url, size_t cap)
{
    size_t  want;
    ssize_t n;
    int     rc;

    if (h == NULL || !h->inited || fd < 0)
        return IV_EINVAL;
    rc = iv_uphttp_begin(h);
    if (rc != IV_OK)
        return rc;
    while (h->left > 0u) {
        want = (h->left < sizeof h->io) ? (size_t)h->left : sizeof h->io;
        n    = read(fd, h->io, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            iv_uphttp_disconnect(h);
            h->sending = 0;
            return IV_EIO;
        }
        if (n == 0) {
            /* 文件比申报的短：已经发出去的头改不了，只能断链作废这次上传 */
            iv_uphttp_disconnect(h);
            h->sending = 0;
            return IV_ERANGE;
        }
        rc = iv_uphttp_write(h, h->io, (size_t)n);
        if (rc != IV_OK)
            return rc;
    }
    return iv_uphttp_end(h, url, cap);
}

/* ---------------------------------------------------------------------------
 * 公开接口
 * ------------------------------------------------------------------------- */

void iv_uphttp_cfg_default(iv_uphttp_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->host       = NULL;
    cfg->port       = (uint16_t)IV_UPHTTP_PORT_DEFAULT;
    cfg->bind_dev   = NULL;
    cfg->device_sn  = NULL;
    cfg->connect_ms = 5000u;
    cfg->send_ms    = 10000u;
    cfg->recv_ms    = 10000u;
}

int iv_uphttp_init(iv_uphttp_t *h, const iv_uphttp_cfg_t *cfg)
{
    if (h == NULL || cfg == NULL)
        return IV_EINVAL;
    if (h->inited)
        return IV_ESTATE;

    memset(h, 0, sizeof *h);
    h->fd = -1;

    copy_str(h->host, sizeof h->host, cfg->host);
    copy_str(h->bind_dev, sizeof h->bind_dev, cfg->bind_dev);
    copy_str(h->dev_sn, sizeof h->dev_sn,
             (cfg->device_sn != NULL && cfg->device_sn[0] != '\0') ? cfg->device_sn : "0");
    h->port       = (cfg->port != 0u) ? cfg->port : (uint16_t)IV_UPHTTP_PORT_DEFAULT;
    h->connect_ms = (cfg->connect_ms != 0u) ? cfg->connect_ms : 5000u;
    h->send_ms    = (cfg->send_ms != 0u) ? cfg->send_ms : 10000u;
    h->recv_ms    = (cfg->recv_ms != 0u) ? cfg->recv_ms : 10000u;
    h->inited     = 1;
    return IV_OK;
}

int iv_uphttp_set_target(iv_uphttp_t *h, const char *host, uint16_t port,
                         const char *bind_dev)
{
    if (h == NULL || !h->inited)
        return IV_ESTATE;
    if (host == NULL || host[0] == '\0')
        return IV_EINVAL;
    if (strcmp(h->host, host) != 0) {
        copy_str(h->host, sizeof h->host, host);
        h->need_reconnect = 1; /* 旧连接指向旧地址，必须换 */
    }
    h->port = (port != 0u) ? port : (uint16_t)IV_UPHTTP_PORT_DEFAULT;
    copy_str(h->bind_dev, sizeof h->bind_dev, bind_dev);
    return IV_OK;
}

void iv_uphttp_disconnect(iv_uphttp_t *h)
{
    if (h == NULL)
        return;
    conn_close(h);
}

void iv_uphttp_close(iv_uphttp_t *h)
{
    if (h == NULL)
        return;
    conn_close(h);
    h->inited = 0;
}

int iv_uphttp_connect(iv_uphttp_t *h)
{
    if (h == NULL || !h->inited)
        return IV_EINVAL;
    conn_close(h);
    return conn_open(h);
}

int iv_uphttp_start(iv_uphttp_t *h, const char *file_name, const char *type,
                    uint32_t total_size)
{
    char path[384];
    int  n;

    if (h == NULL || !h->inited || file_name == NULL || file_name[0] == '\0')
        return IV_EINVAL;
    if (h->host[0] == '\0')
        return IV_EINVAL;
    if (total_size == 0u)
        return IV_EINVAL;

    copy_str(h->file_name, sizeof h->file_name, file_name);
    h->upload_id[0] = '\0';
    h->sending      = 0;
    h->total        = total_size;
    h->left         = 0u;

    /* totalChunks 恒为 1：一个文件一次 chunk 请求（见文件头实测数据） */
    n = snprintf(path, sizeof path,
                 "%s?deviceSn=%s&totalSize=%lu&totalChunks=1&fileName=%s&logType=%s",
                 IV_UPHTTP_PATH_START, h->dev_sn, (unsigned long)total_size,
                 h->file_name,
                 (type != NULL && type[0] != '\0') ? type : "SYSTEM");
    if (n < 0 || (size_t)n >= sizeof path)
        return IV_ERANGE;

    n = post_query(h, path, 1);
    if (n != IV_OK)
        return n;
    if (api_ok(h) != IV_OK)
        return IV_EPROTO;
    if (body_str(h, "uploadId", h->upload_id, sizeof h->upload_id) != IV_OK ||
        h->upload_id[0] == '\0')
        return IV_EPROTO;
    return IV_OK;
}

int iv_uphttp_status(const iv_uphttp_t *h)
{
    return (h == NULL) ? 0 : h->status;
}

const char *iv_uphttp_resp(const iv_uphttp_t *h)
{
    return (h == NULL) ? "" : h->resp;
}
