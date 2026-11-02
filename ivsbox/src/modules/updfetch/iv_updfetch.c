/*
 * 平台触发升级的拉包实现（0xB3）—— 契约见 iv_updfetch.h
 *
 * 一句话：`iv_updfetch_run()` 在 worker 线程里
 *   GET info.txt → 比版本 → GET 整包（纯 HTTP 流式）→ .part → rename
 *   → 写 `<data_root>/ota/request` 交给 monitor.sh。
 *
 * 三条必须记住的口径：
 *   1) **应答必须按 Content-Length 读满**再发下一个请求，否则下一应答从上一应答的
 *      残留 body 开始解析（与 `iv_logup` 同一条教训）。头里可能已经带了一段 body，
 *      所以 `recv_some()` 先吐 `hdr` 里的残留再 `recv`。
 *   2) **不套私有分帧格式**：参考固件的 1026 B 帧（1024 数据 ＋ 2 B CRC）是它刷
 *      SPI Flash 的产物，与 HTTP 无关。2026-11-02 用户口径「升级不按单片机那套，
 *      走常规 Linux」⇒ 只保留整包 GET。完整性判据是装盘时 `iv_ota` 的 SHA-256 ＋
 *      签名（fail-closed），传输层不再重复校验 —— 多一处判据就多一处会误判的地方。
 *   3) 下载落 `.part` 再 `rename`：请求文件指向的路径必须**永远是完整的包**，
 *      否则 monitor.sh 可能在半截文件上启动安装进程。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* SO_BINDTODEVICE / getaddrinfo 的 GNU 扩展 */
#endif

#include "ivsbox/iv_updfetch.h"

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_upd.h"

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
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define UF_MOD "updfetch"

#define UF_PORT_DEFAULT 8989u
#define UF_ROOT_DEFAULT "/mnt/UDISK/ivsbox"
#define UF_OTA_SUBDIR "ota"
#define UF_HDR_END "\r\n\r\n"

/* `recv_some()` 的两个负数：区分"超时"与"断链"，日志与返回码都不一样 */
#define UF_R_TIMEOUT (-2)
#define UF_R_ERROR (-1)

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = 0u;

    if (dst == NULL || cap == 0u)
        return;
    if (src != NULL) {
        while (src[n] != '\0' && (n + 1u) < cap) {
            dst[n] = src[n];
            n++;
        }
    }
    dst[n] = '\0';
}

/* 有界追加（避免 `snprintf("%s/%s")` 在 -O2 下触发 -Wformat-truncation） */
static void append_str(char *dst, size_t cap, size_t *pos, const char *s)
{
    size_t i = 0u;

    if (dst == NULL || pos == NULL || s == NULL)
        return;
    while (s[i] != '\0' && (*pos + 1u) < cap) {
        dst[*pos] = s[i];
        (*pos)++;
        i++;
    }
    dst[*pos] = '\0';
}

/* 文件名白名单：只允许 `[A-Za-z0-9._-]`，且不许 ".."（拼路径前把关） */
static int name_ok(const char *n)
{
    size_t i;

    if (n == NULL || n[0] == '\0')
        return 0;
    if (strstr(n, "..") != NULL)
        return 0;
    for (i = 0u; n[i] != '\0'; i++) {
        char c = n[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

/* 建目录（已存在不算错；父目录由调用方先建） */
static int mkdir_p1(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return IV_OK;
    return IV_EIO;
}

static int ci_has(const char *hay, size_t hay_len, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0u || hay_len < nlen)
        return 0;
    for (i = 0u; (i + nlen) <= hay_len; i++) {
        size_t k;

        for (k = 0u; k < nlen; k++) {
            char a = hay[i + k];
            char b = needle[k];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (k == nlen)
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 连接（与 iv_logup 同口径：出口绑定 + 非阻塞 connect + SO_*TIMEO）
 * ------------------------------------------------------------------------- */

static void conn_close(iv_updfetch_t *u)
{
    if (u->fd >= 0) {
        (void)close(u->fd);
        u->fd = -1;
    }
    u->conn_host[0] = '\0';
    u->conn_port    = 0u;
}

static void set_timeo(int fd, int opt, uint32_t ms)
{
    struct timeval tv;

    tv.tv_sec  = (time_t)(ms / 1000u);
    tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
    (void)setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv);
}

static int conn_open(iv_updfetch_t *u, const char *host, uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p;
    char             pstr[8];
    int              rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    (void)snprintf(pstr, sizeof pstr, "%u", (unsigned)port);
    rc = getaddrinfo(host, pstr, &hints, &res);
    if (rc != 0 || res == NULL) {
        IV_LOG_W(UF_MOD, "resolve %s failed: %s", host, gai_strerror(rc));
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
        /* 出口绑定必须在 connect 前：双 WAN 同时在线时只靠路由会把包从不通的
         * 那条链路取（传输层与 iv_logup 踩的是同一个坑）。 */
        if (u->bind_dev[0] != '\0') {
            if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, u->bind_dev,
                           (socklen_t)(strlen(u->bind_dev) + 1u)) != 0) {
                IV_LOG_W(UF_MOD, "bind %s failed (%s), fallback to route",
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
            rc = poll(&pfd, 1, (int)u->connect_ms);
            if (rc == 0) {
                IV_LOG_W(UF_MOD, "connect %s:%u timeout", host, (unsigned)port);
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
        u->fd = fd;
        copy_str(u->conn_host, sizeof u->conn_host, host);
        u->conn_port      = port;
        u->need_reconnect = 0;
        (void)freeaddrinfo(res);
        return IV_OK;
    }

    (void)freeaddrinfo(res);
    return IV_ECONN;
}

static int ensure_conn(iv_updfetch_t *u, const char *host, uint16_t port)
{
    if (u->fd >= 0 && u->need_reconnect == 0 && u->conn_port == port &&
        strcmp(u->conn_host, host) == 0)
        return IV_OK;
    conn_close(u);
    return conn_open(u, host, port);
}

static int send_all(iv_updfetch_t *u, const void *buf, size_t len)
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
        u->need_reconnect = 1;
        return IV_ECONN;
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * HTTP 应答
 * ------------------------------------------------------------------------- */

/* 从 hdr 里找头结束位置；找不到返回 0 */
static size_t hdr_end(const iv_updfetch_t *u)
{
    size_t i;

    for (i = 0u; (i + 4u) <= u->hdr_len; i++) {
        if (u->hdr[i] == '\r' && u->hdr[i + 1u] == '\n' && u->hdr[i + 2u] == '\r' &&
            u->hdr[i + 3u] == '\n')
            return i + 4u;
    }
    return 0u;
}

/* 在头区里按名字取数值（大小写不敏感；找不到返回 -1） */
static long hdr_num(const iv_updfetch_t *u, size_t head_len, const char *name)
{
    size_t nlen = strlen(name);
    size_t i;

    for (i = 0u; (i + nlen) < head_len; i++) {
        size_t k;

        if (i > 0u && u->hdr[i - 1u] != '\n')
            continue; /* 只认行首，否则 "X-Content-Length" 也会命中 */
        for (k = 0u; k < nlen; k++) {
            char a = u->hdr[i + k];
            char b = name[k];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (k != nlen)
            continue;
        return strtol(u->hdr + i + nlen, NULL, 10);
    }
    return -1;
}

/* 收到完整头为止，解析状态码/Content-Length/是否要重连 */
static int read_head(iv_updfetch_t *u)
{
    for (;;) {
        size_t end = hdr_end(u);

        if (end != 0u) {
            int   status = 0;
            long  clen;
            int   close_hint;

            if (sscanf(u->hdr, "HTTP/%*d.%*d %d", &status) != 1) {
                u->need_reconnect = 1;
                return IV_EPROTO;
            }
            u->status   = status;
            clen        = hdr_num(u, end, "Content-Length:");
            u->body_len = clen;
            /* 只看头里有没有 Connection: close / HTTP/1.0（1.0 默认短连接） */
            close_hint = ci_has(u->hdr, end, "connection: close");
            if (close_hint || strncmp(u->hdr, "HTTP/1.0", 8) == 0)
                u->need_reconnect = 1;
            u->body_off = end;
            return IV_OK;
        }
        if (u->hdr_len + 1u >= sizeof u->hdr) {
            u->need_reconnect = 1;
            return IV_EPROTO; /* 头比缓冲还长：应答异常 */
        }
        {
            ssize_t n = recv(u->fd, u->hdr + u->hdr_len,
                             sizeof u->hdr - 1u - u->hdr_len, 0);

            if (n > 0) {
                u->hdr_len += (size_t)n;
                u->hdr[u->hdr_len] = '\0';
                continue;
            }
            if (n == 0) {
                u->need_reconnect = 1;
                return IV_ECONN;
            }
            if (errno == EINTR)
                continue;
            u->need_reconnect = 1; /* EAGAIN = SO_RCVTIMEO 到期 */
            return IV_ETIMEDOUT;
        }
    }
}

/*
 * 读一段 body：先把头里已经带回来的残留吐出来，再 recv。
 *   返回 > 0 读到的字节数；0 = 对端关闭（正常收尾）；
 *   UF_R_TIMEOUT = 读超时；UF_R_ERROR = 连接错误。
 * **不能**把"读到多少算多少"当成功 —— 调用方要用 Content-Length 对账。
 */
static long recv_some(iv_updfetch_t *u, void *dst, size_t cap)
{
    if (u->body_off < u->hdr_len) {
        size_t avail = u->hdr_len - u->body_off;

        if (avail > cap)
            avail = cap;
        memcpy(dst, u->hdr + u->body_off, avail);
        u->body_off += avail;
        return (long)avail;
    }
    for (;;) {
        ssize_t n = recv(u->fd, dst, cap, 0);

        if (n > 0)
            return (long)n;
        if (n == 0) {
            u->need_reconnect = 1;
            return 0;
        }
        if (errno == EINTR)
            continue;
        u->need_reconnect = 1;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? UF_R_TIMEOUT : UF_R_ERROR;
    }
}

/* 发一个 GET 并读到应答头 */
static int http_get(iv_updfetch_t *u, const char *host, uint16_t port, const char *path)
{
    char   req[384];
    size_t pos = 0u;
    char   port_str[8];
    int    rc;

    rc = ensure_conn(u, host, port);
    if (rc != IV_OK)
        return rc;

    (void)snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    req[0] = '\0';
    append_str(req, sizeof req, &pos, "GET ");
    append_str(req, sizeof req, &pos, path);
    append_str(req, sizeof req, &pos, " HTTP/1.1\r\nHost: ");
    append_str(req, sizeof req, &pos, host);
    /* 默认端口不带进 Host（与浏览器的规矩一致，也避免某些服务端按
     * "host:80" 与虚拟主机名不匹配） */
    if (port != 80u) {
        append_str(req, sizeof req, &pos, ":");
        append_str(req, sizeof req, &pos, port_str);
    }
    append_str(req, sizeof req, &pos, "\r\nConnection: keep-alive\r\n\r\n");

    u->hdr_len  = 0u;
    u->hdr[0]   = '\0';
    u->body_off = 0u;
    u->body_len = -1;
    u->status   = 0;

    rc = send_all(u, req, pos);
    if (rc != IV_OK)
        return rc;
    return read_head(u);
}

/* ---------------------------------------------------------------------------
 * info.txt：取、解析、URL 拆解
 * ------------------------------------------------------------------------- */

/* 从 JSON 体里取字符串字段（严格：必须紧跟冒号与引号，字段名带引号匹配） */
static int info_str(const char *body, const char *key, char *out, size_t cap)
{
    char        pat[32];
    const char *p;
    const char *q;
    size_t      n;

    (void)snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(body, pat);
    if (p == NULL)
        return IV_ENOENT;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ':')
        return IV_EPROTO;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return IV_EPROTO;
    p++;
    q = strchr(p, '"');
    if (q == NULL)
        return IV_EPROTO;
    n = (size_t)(q - p);
    if (n == 0u || n >= cap)
        return IV_ERANGE;
    memcpy(out, p, n);
    out[n] = '\0';
    return IV_OK;
}

static int fetch_info(iv_updfetch_t *u, char *url_out, size_t url_cap)
{
    char   path[IV_UPDFETCH_PATH_MAX];
    size_t pos = 0u;
    long   clen;
    long   got = 0;
    int    rc;

    path[0] = '\0';
    append_str(path, sizeof path, &pos, "/");
    append_str(path, sizeof path, &pos, u->hw);
    append_str(path, sizeof path, &pos, "/info.txt");

    rc = http_get(u, u->host, u->port, path);
    if (rc != IV_OK)
        return rc;
    if (u->status != 200) {
        IV_LOG_W(UF_MOD, "GET %s -> HTTP %d", path, u->status);
        return IV_EPROTO;
    }
    clen = u->body_len;
    if (clen <= 0 || (size_t)clen >= sizeof u->buf) {
        IV_LOG_W(UF_MOD, "info.txt body length %ld out of range", clen);
        return IV_EPROTO;
    }
    while (got < clen) {
        long n = recv_some(u, u->buf + got, (size_t)clen - (size_t)got);

        if (n == 0) {
            IV_LOG_W(UF_MOD, "info.txt short body (%ld/%ld)", got, clen);
            return IV_ECONN;
        }
        if (n < 0)
            return (n == UF_R_TIMEOUT) ? IV_ETIMEDOUT : IV_ECONN;
        got += n;
    }
    u->buf[clen] = '\0';

    rc = info_str((const char *)u->buf, "version", u->info_version,
                  sizeof u->info_version);
    if (rc != IV_OK) {
        IV_LOG_W(UF_MOD, "info.txt has no usable \"version\"");
        return IV_EPROTO;
    }
    rc = info_str((const char *)u->buf, "url", url_out, url_cap);
    if (rc != IV_OK) {
        IV_LOG_W(UF_MOD, "info.txt has no usable \"url\"");
        return IV_EPROTO;
    }
    return IV_OK;
}

/* `http://host:port/path` 或 `host/path`；https 明确不支持（参考实现同样） */
static int parse_url(iv_updfetch_t *u, const char *url)
{
    const char *p = url;
    const char *slash;
    const char *colon;
    size_t      host_len;
    size_t      pos = 0u;

    if (strncmp(url, "https://", 8) == 0) {
        IV_LOG_W(UF_MOD, "update url is https, not supported: %s", url);
        return IV_ENOTSUP;
    }
    if (strncmp(url, "http://", 7) == 0)
        p += 7;

    slash = strchr(p, '/');
    if (slash == NULL) {
        IV_LOG_W(UF_MOD, "update url has no path: %s", url);
        return IV_EPROTO;
    }
    colon = memchr(p, ':', (size_t)(slash - p));
    host_len = (size_t)((colon != NULL) ? (colon - p) : (slash - p));

    if (host_len == 0u) {
        /* 只给路径：沿用 info.txt 的服务器 */
        copy_str(u->url_host, sizeof u->url_host, u->host);
        u->url_port = u->port;
    } else {
        if (host_len >= sizeof u->url_host)
            return IV_ERANGE;
        memcpy(u->url_host, p, host_len);
        u->url_host[host_len] = '\0';
        if (colon != NULL) {
            long pv = strtol(colon + 1, NULL, 10);

            if (pv <= 0 || pv > 65535)
                return IV_EPROTO;
            u->url_port = (uint16_t)pv;
        } else {
            u->url_port = 80u;
        }
    }

    pos = 0u;
    u->url_path[0] = '\0';
    /* 路径原样保留（含可能的查询串）：签名包一般不带 query，但擅自裁剪会
     * 把带 query 的下载地址裁成 404，而现场只看到"下载失败"。 */
    append_str(u->url_path, sizeof u->url_path, &pos, slash);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 下载（纯 HTTP 流式，一条路径）
 * ------------------------------------------------------------------------- */

static int do_download(iv_updfetch_t *u, const char *part)
{
    FILE   *f;
    long    total = 0;
    long    remain;
    int     rc;

    rc = http_get(u, u->url_host, u->url_port, u->url_path);
    if (rc != IV_OK)
        return rc;
    if (u->status != 200) {
        IV_LOG_W(UF_MOD, "GET %s -> HTTP %d", u->url_path, u->status);
        return IV_EPROTO;
    }
    u->file_size = u->body_len; /* -1 = 服务器没报长度（靠 EOF 收尾） */

    f = fopen(part, "wb");
    if (f == NULL) {
        IV_LOG_W(UF_MOD, "open %s for write failed: %s", part, strerror(errno));
        return (errno == ENOSPC) ? IV_ENOSPC : IV_EIO;
    }

    /* 读满 Content-Length 就收工 —— **不能等对端关连接**：HTTP/1.1 默认 keep-alive，
     * 服务器不会主动关，等关闭只会等到 SO_RCVTIMEO 到期，把正常下载判成超时
     * （p01 第一版就是这么红的）。没有 Content-Length 时才靠 EOF 收尾。 */
    remain = (u->body_len >= 0) ? u->body_len : -1;
    for (;;) {
        size_t want = sizeof u->buf;
        long   n;

        if (remain == 0)
            break;
        if (remain > 0 && (long)want > remain)
            want = (size_t)remain;

        n = recv_some(u, u->buf, want);
        if (n == 0)
            break; /* 对端先关了：靠下面的对账判短包 */
        if (n < 0) {
            IV_LOG_W(UF_MOD, "recv body failed after %ld B (%s)", total,
                     (n == UF_R_TIMEOUT) ? "timeout" : "connection error");
            rc = (n == UF_R_TIMEOUT) ? IV_ETIMEDOUT : IV_ECONN;
            goto FAIL;
        }
        total += n;
        if (remain > 0)
            remain -= n;
        /* 边下边判上限：不等服务器报长度，"给错文件把 UDISK 写满"是最贵的失败 */
        if ((uint64_t)total > u->max_bytes) {
            IV_LOG_W(UF_MOD, "package exceeds limit %u B, aborted at %ld B",
                     (unsigned)u->max_bytes, total);
            rc = IV_ERANGE;
            goto FAIL;
        }
        if (fwrite(u->buf, 1u, (size_t)n, f) != (size_t)n) {
            rc = (errno == ENOSPC) ? IV_ENOSPC : IV_EIO;
            goto FAIL;
        }
    }

    if (total == 0) {
        IV_LOG_W(UF_MOD, "empty package body: %s", u->url_path);
        rc = IV_EPROTO;
        goto FAIL;
    }
    /* 对账：报了长度就必须一字不差，短包等于"拿半个包装上去" */
    if (u->body_len >= 0 && total != u->body_len) {
        IV_LOG_W(UF_MOD, "body %ld B != Content-Length %ld", total, u->body_len);
        rc = IV_EPROTO;
        goto FAIL;
    }
    if (u->body_len < 0)
        IV_LOG_W(UF_MOD, "no Content-Length on %s, took %ld B by EOF", u->url_path,
                 total);

    if (fflush(f) != 0) {
        rc = IV_EIO;
        goto FAIL;
    }
    /* 落盘要真的落地再 rename：否则断电后请求文件指向一个空壳 */
    if (fsync(fileno(f)) != 0) {
        int e = errno;

        (void)fclose(f);
        return (e == ENOSPC) ? IV_ENOSPC : IV_EIO;
    }
    if (fclose(f) != 0)
        return IV_EIO;
    u->st.last_bytes = (uint32_t)total;
    return IV_OK;

FAIL:
    (void)fclose(f);
    return rc;
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

void iv_updfetch_cfg_default(iv_updfetch_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->port      = (uint16_t)UF_PORT_DEFAULT;
    cfg->hw_name   = IV_UPDFETCH_HW_DEFAULT;
    cfg->data_root = UF_ROOT_DEFAULT;
    cfg->dest_name = IV_UPDFETCH_NAME_DEFAULT;
    cfg->max_bytes = IV_UPDFETCH_MAX_BYTES_DEFAULT;
    cfg->connect_ms = 5000u;
    cfg->send_ms    = 10000u;
    cfg->recv_ms    = 20000u;
}

int iv_updfetch_init(iv_updfetch_t *u, const iv_updfetch_cfg_t *cfg)
{
    if (u == NULL)
        return IV_EINVAL;
    if (u->inited != 0)
        return IV_ESTATE;
    memset(u, 0, sizeof *u);
    u->fd = -1;
    if (cfg != NULL) {
        copy_str(u->host, sizeof u->host, cfg->host);
        copy_str(u->bind_dev, sizeof u->bind_dev, cfg->bind_dev);
        copy_str(u->hw, sizeof u->hw,
                 (cfg->hw_name != NULL && cfg->hw_name[0] != '\0')
                     ? cfg->hw_name
                     : IV_UPDFETCH_HW_DEFAULT);
        /* 现场/工装临时对另一套目录命名用（`/opt/ivsbox/scripts/monitor.sh` 与
         * init 脚注里都可 export）。别名优先于编译期默认、但**不覆盖显式配置**：
         * 有了 `cfg->hw_name` 就以它为准，环境变量只是"没配时的手动挡"。 */
        {
            const char *env_hw = getenv("IVSBOX_OTA_HW");

            if (env_hw != NULL && env_hw[0] != '\0' && name_ok(env_hw)) {
                copy_str(u->hw, sizeof u->hw, env_hw);
                IV_LOG_I(UF_MOD, "hw name overridden by IVSBOX_OTA_HW: %s", u->hw);
            }
        }
        copy_str(u->ver, sizeof u->ver, cfg->ver_str);
        copy_str(u->root, sizeof u->root,
                 (cfg->data_root != NULL && cfg->data_root[0] != '\0')
                     ? cfg->data_root
                     : UF_ROOT_DEFAULT);
        copy_str(u->name, sizeof u->name,
                 (cfg->dest_name != NULL && name_ok(cfg->dest_name))
                     ? cfg->dest_name
                     : IV_UPDFETCH_NAME_DEFAULT);
        u->port       = (cfg->port != 0u) ? cfg->port : (uint16_t)UF_PORT_DEFAULT;
        u->max_bytes  = (cfg->max_bytes != 0u) ? cfg->max_bytes
                                               : IV_UPDFETCH_MAX_BYTES_DEFAULT;
        u->connect_ms = (cfg->connect_ms != 0u) ? cfg->connect_ms : 5000u;
        u->send_ms    = (cfg->send_ms != 0u) ? cfg->send_ms : 10000u;
        u->recv_ms    = (cfg->recv_ms != 0u) ? cfg->recv_ms : 20000u;
    }
    u->inited = 1;
    if (u->host[0] == '\0')
        IV_LOG_W(UF_MOD, "init without host: run() will fail until set_target()");
    if (u->ver[0] == '\0')
        IV_LOG_W(UF_MOD, "init without version string: run() will fail");
    return IV_OK;
}

int iv_updfetch_set_target(iv_updfetch_t *u, const char *host, uint16_t port,
                           const char *bind_dev, const char *ver_str)
{
    if (u == NULL || u->inited == 0)
        return IV_EINVAL;
    if (u->running != 0)
        return IV_EBUSY; /* 下载中途换地址会让连接与已收进度同时对不上 */
    if (host != NULL)
        copy_str(u->host, sizeof u->host, host);
    if (port != 0u)
        u->port = port;
    if (bind_dev != NULL)
        copy_str(u->bind_dev, sizeof u->bind_dev, bind_dev);
    if (ver_str != NULL)
        copy_str(u->ver, sizeof u->ver, ver_str);
    return IV_OK;
}

void iv_updfetch_close(iv_updfetch_t *u)
{
    if (u == NULL)
        return;
    conn_close(u);
    u->inited = 0;
    u->host[0] = '\0';
}

/* 落点：<root>/ota/download/<name>（父目录按需建） */
static int prepare_dirs(const iv_updfetch_t *u, char *out_dir, size_t cap)
{
    char   dir[IV_UPDFETCH_PATH_MAX];
    size_t pos = 0u;
    int    rc;

    dir[0] = '\0';
    append_str(dir, sizeof dir, &pos, u->root);
    rc = mkdir_p1(dir);
    if (rc != IV_OK)
        return rc;
    append_str(dir, sizeof dir, &pos, "/");
    append_str(dir, sizeof dir, &pos, UF_OTA_SUBDIR);
    rc = mkdir_p1(dir);
    if (rc != IV_OK)
        return rc;
    append_str(dir, sizeof dir, &pos, "/");
    append_str(dir, sizeof dir, &pos, IV_UPDFETCH_DOWNLOAD_SUBDIR);
    rc = mkdir_p1(dir);
    if (rc != IV_OK)
        return rc;
    copy_str(out_dir, cap, dir);
    return IV_OK;
}

static void join_path(char *out, size_t cap, const char *dir, const char *name,
                      const char *suffix)
{
    size_t pos = 0u;

    out[0] = '\0';
    append_str(out, cap, &pos, dir);
    append_str(out, cap, &pos, "/");
    append_str(out, cap, &pos, name);
    append_str(out, cap, &pos, suffix);
}

/* 用 URL 的最后一段当文件名（安全字符才用，否则保留配置里的默认名） */
static void name_from_url(iv_updfetch_t *u)
{
    const char *p = strrchr(u->url_path, '/');
    const char *q;

    if (p == NULL || p[1] == '\0')
        return;
    if (!name_ok(p + 1))
        return;
    if (strlen(p + 1) >= sizeof u->name)
        return;
    q = strchr(p + 1, '?');
    if (q != NULL)
        return; /* 带查询串的路径不当文件名 */
    copy_str(u->name, sizeof u->name, p + 1);
}

/* ---------------------------------------------------------------------------
 * 目录列表模式（`info.txt` 不存在时）
 * -------------------------------------------------------------------------
 * 发布侧只往目录里放**一个**包（2026-11-02 用户口径："必须是单个文件，压缩包也行，
 * 不要让我放多个文件，文件名必须带版本号"），于是没有 `info.txt` 可选。此时
 * `GET /<硬件名>/info.txt` 回 404，就退回去读 nginx/busybox httpd 的 autoindex，
 * 从 `href="…"` 里挑 `.pkg`，取文件名中 M.m.p 最大的那个。
 *
 * 三条纪律：
 *   - **info.txt 优先**：只要目录里有 info.txt 就走单片机那套原路；列表只是
 *     "没有 info.txt"时的退化路径，不是替代品（发布侧哪天补上 info.txt 也不用改代码）。
 *   - **只信文件名里的版本**，不信目录里的时间戳：时间会随上传方式乱、也能被
 *     `touch` 改；版本号是发布的人手写的，且与包清单里的版本必须一致 ——
 *     不一致由 `iv_ota` 在装盘时拒，**本模块不替它判**。
 *   - **只认 `.pkg`**：别把目录里的 `info.txt`、说明文档、旧 `.bin` 当包拉下来。
 */

/* 文件名里的 M.m.p：`FN_RZTY_PY-1.0.1.20261103.pkg` ⇒ `1.0.1`。
 * 第四段（日期）**故意不取**：包清单的版本口径就是三段，日期只是给人看的。 */
static int ver_from_name(const char *name, iv_ota_ver_t *out)
{
    const char *p = name;

    if (name == NULL || out == NULL)
        return IV_EINVAL;
    while (*p != '\0') {
        if (*p >= '0' && *p <= '9') {
            char        tmp[24];
            size_t      n = 0u;
            int         dots = 0;
            const char *q = p;

            while (*q != '\0' && n + 1u < sizeof tmp) {
                if (*q >= '0' && *q <= '9') {
                    tmp[n++] = *q++;
                } else if (*q == '.' && dots < 2) {
                    tmp[n++] = *q++;
                    dots++;
                } else {
                    break;
                }
            }
            tmp[n] = '\0';
            if (dots == 2 && iv_ota_ver_parse(tmp, n, out) == IV_OK)
                return IV_OK;
        }
        p++;
    }
    return IV_ENOENT;
}

/* 只收 `.pkg`（大小写不敏感），且先过 `name_ok()` 那道词法关（`..`/空格/百分号全拒） */
static int pkg_name_ok(const char *n)
{
    const size_t sl = sizeof IV_UPDFETCH_PKG_SUFFIX - 1u;
    size_t       len;
    size_t       i;

    if (!name_ok(n))
        return 0;
    len = strlen(n);
    if (len <= sl)
        return 0;
    for (i = 0u; i < sl; i++) {
        char a = n[len - sl + i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != IV_UPDFETCH_PKG_SUFFIX[i])
            return 0;
    }
    return 1;
}

/* 取下一个 `href="…"`。只认双引号这一种写法：nginx/busybox httpd 都是它，
 * 再兼容单引号/无引号等于给自己加两条永远走不到的分支。 */
static const char *next_href(const char *p, char *out, size_t cap)
{
    const char *a;
    const char *b;
    size_t      n;

    a = strstr(p, "href=\"");
    if (a == NULL)
        return NULL;
    a += 6;
    b = strchr(a, '"');
    if (b == NULL)
        return NULL;
    n = (size_t)(b - a);
    if (n == 0u || n >= cap)
        return b; /* 空的/装不下的跳过：截断的名字只会装错文件，不如不要 */
    memcpy(out, a, n);
    out[n] = '\0';
    return b;
}

/* 本机"现在的版本"（M.m.p）：编译期版本串与**已装发行版目录名**取大者。
 * 为什么不能只看编译期版本：包里的二进制可能就是设备正在跑的那个构建
 * （本次两边都是 `1.0.0.20261102`），只看它会把同一个包反复当新版本下载。
 * 为什么不用 `pending.json` 里的版本：安装失败/回滚之后它仍记着那个失败的版本，
 * 用户修好环境再点一次升级会被判"已是最新"、永远重试不了 —— 所以看 `current`
 * 指向的发行版目录名（`iv_upd_status()` 的 `current_dir`）。 */
static iv_ota_ver_t local_ver_of(const iv_updfetch_t *u)
{
    iv_upd_cfg_t    uc;
    iv_upd_status_t st;
    iv_ota_ver_t    v;
    iv_ota_ver_t    t;

    memset(&v, 0, sizeof v);
    (void)ver_from_name(u->ver, &v); /* ver_str 形如 "1.0.0.20261102" ⇒ 取前 3 段 */
    iv_upd_cfg_default(&uc);
    uc.data_root = u->root;
    iv_upd_status(&uc, &st);
    if (st.current_dir[0] != '\0' &&
        iv_ota_ver_parse(st.current_dir, strlen(st.current_dir), &t) == IV_OK &&
        iv_ota_ver_cmp(t, v) > 0) {
        v = t;
    }
    return v;
}

/* 读目录列表，挑出文件名版本最大的 `.pkg`；成功时把**只含路径**的 URL 写到 url_out */
static int fetch_listing(iv_updfetch_t *u, char *url_out, size_t url_cap,
                         char *name_out, size_t name_cap, iv_ota_ver_t *ver_out)
{
    char         path[IV_UPDFETCH_PATH_MAX];
    char         name[IV_UPDFETCH_NAME_MAX];
    char         best[IV_UPDFETCH_NAME_MAX];
    size_t       pos = 0u;
    long         got = 0;
    long         want;
    const char  *p;
    iv_ota_ver_t bv;
    int          have = 0;
    int          rc;

    /* **换一条新连接**：上一条收到的是 404，body 没读走（`fetch_info()` 直接返回
     * 了），留在 socket 里的字节会被当成下一个应答的头 —— keep-alive 复用会
     * 解析出乱码。多一次握手，只在退化路径上付。 */
    u->need_reconnect = 1;

    path[0] = '\0';
    append_str(path, sizeof path, &pos, "/");
    append_str(path, sizeof path, &pos, u->hw);
    append_str(path, sizeof path, &pos, "/");

    rc = http_get(u, u->host, u->port, path);
    if (rc != IV_OK)
        return rc;
    if (u->status != 200) {
        IV_LOG_W(UF_MOD, "GET %s -> HTTP %d", path, u->status);
        return IV_EPROTO;
    }
    if (u->body_len <= 0) {
        IV_LOG_W(UF_MOD, "GET %s: empty listing", path);
        return IV_EPROTO;
    }
    /* 列表页远小于 4 KiB（几十个条目也就几百字节）。**超过就截断解析**：
     * 截断最多漏掉排在后面的包，不会把某个名字读错。 */
    want = (u->body_len < (long)(sizeof u->buf - 1u)) ? u->body_len
                                                     : (long)(sizeof u->buf - 1u);
    while (got < want) {
        long n = recv_some(u, u->buf + got, (size_t)want - (size_t)got);

        if (n == 0) {
            IV_LOG_W(UF_MOD, "GET %s: short listing (%ld/%ld)", path, got, want);
            return IV_ECONN;
        }
        if (n < 0)
            return (n == UF_R_TIMEOUT) ? IV_ETIMEDOUT : IV_ECONN;
        got += n;
    }
    u->buf[want] = '\0';
    if (u->body_len > want) {
        IV_LOG_W(UF_MOD, "GET %s: listing truncated at %ld B, later entries ignored",
                 path, want);
    }

    memset(&bv, 0, sizeof bv);
    p = (const char *)u->buf;
    while ((p = next_href(p, name, sizeof name)) != NULL) {
        iv_ota_ver_t v;

        if (!pkg_name_ok(name))
            continue;
        if (ver_from_name(name, &v) != IV_OK) {
            IV_LOG_W(UF_MOD, "%s: no M.m.p in file name, skipped", name);
            continue;
        }
        if (!have || iv_ota_ver_cmp(v, bv) > 0) {
            bv   = v;
            have = 1;
            copy_str(best, sizeof best, name);
        }
    }
    if (!have) {
        IV_LOG_W(UF_MOD, "GET %s: no *%s in listing", path, IV_UPDFETCH_PKG_SUFFIX);
        return IV_ENOENT;
    }

    /* **只给路径**（不带 host）⇒ `parse_url()` 走它"只给路径"那条分支、沿用升级
     * 服务器那一对 host/port，免得发布侧在列表里写死 IP、日后换服务器还要重发。 */
    pos = 0u;
    url_out[0] = '\0';
    append_str(url_out, url_cap, &pos, path);
    append_str(url_out, url_cap, &pos, best);
    copy_str(name_out, name_cap, best);
    *ver_out = bv;
    IV_LOG_I(UF_MOD, "listing picked %s", best);
    return IV_OK;
}

int iv_updfetch_run(iv_updfetch_t *u)
{
    char   dir[IV_UPDFETCH_PATH_MAX];
    char   part[IV_UPDFETCH_PATH_MAX];
    char   final[IV_UPDFETCH_PATH_MAX];
    char   url[IV_UPDFETCH_URL_MAX];
    char   want_ver[IV_UPDFETCH_VER_MAX + IV_UPDFETCH_HW_MAX + 2u];
    size_t pos = 0u;
    int    rc;

    if (u == NULL || u->inited == 0)
        return IV_EINVAL;
    if (u->running != 0)
        return IV_EBUSY;
    u->running      = 1;
    u->st.runs++;
    u->st.last_bytes = 0u;
    u->st.last_http  = 0;

    if (u->host[0] == '\0' || u->ver[0] == '\0') {
        rc = IV_EINVAL;
        goto OUT;
    }
    rc = prepare_dirs(u, dir, sizeof dir);
    if (rc != IV_OK) {
        IV_LOG_W(UF_MOD, "cannot prepare %s/" IV_UPDFETCH_DOWNLOAD_SUBDIR ": %s",
                 u->root, strerror(errno));
        goto OUT;
    }

    /* 1. 先问"有没有新版本"：参考固件（也就是"单片机的逻辑"）是
     *    `GET /<硬件名>/info.txt`，见 `General_Version/main/APP/Update/src/update_http.c`。 */
    rc = fetch_info(u, url, sizeof url);
    if (rc == IV_EPROTO && u->status == 404) {
        /* 目录里只放了一个包、没有 info.txt ⇒ 404 ⇒ 退回读目录列表（见上面的注释） */
        iv_ota_ver_t rv;
        iv_ota_ver_t lv;
        char         sv[16];

        IV_LOG_I(UF_MOD, "no /%s/info.txt (HTTP 404), fall back to directory listing",
                 u->hw);
        rc = fetch_listing(u, url, sizeof url, u->name, sizeof u->name, &rv);
        if (rc != IV_OK)
            goto OUT;
        u->st.listing++;
        iv_ota_ver_format(rv, u->st.info_version, sizeof u->st.info_version);
        copy_str(u->st.url, sizeof u->st.url, url);

        lv = local_ver_of(u);
        iv_ota_ver_format(lv, sv, sizeof sv);
        if (iv_ota_ver_cmp(rv, lv) <= 0) {
            IV_LOG_I(UF_MOD, "already latest (local %s >= %s), nothing to do", sv,
                     u->st.info_version);
            u->st.same++;
            rc = IV_UPDFETCH_SAME;
            goto OUT;
        }
        IV_LOG_I(UF_MOD, "update available (listing): %s -> %s", sv, u->st.info_version);
    } else if (rc != IV_OK) {
        goto OUT;
    } else {
        copy_str(u->st.info_version, sizeof u->st.info_version, u->info_version);
        copy_str(u->st.url, sizeof u->st.url, url);

        /* 2. 版本比对（info.txt 模式）：参考实现把版本串拼成 `<硬件名>-<软件版本>`
         *    与 info.txt 比，**只比相等、不比大小**（口径与它保持一致）。 */
        want_ver[0] = '\0';
        append_str(want_ver, sizeof want_ver, &pos, u->hw);
        append_str(want_ver, sizeof want_ver, &pos, "-");
        append_str(want_ver, sizeof want_ver, &pos, u->ver);
        if (strcmp(want_ver, u->info_version) == 0) {
            IV_LOG_I(UF_MOD, "already latest (%s), nothing to do", want_ver);
            u->st.same++;
            rc = IV_UPDFETCH_SAME;
            goto OUT;
        }
        IV_LOG_I(UF_MOD, "update available: %s -> %s", want_ver, u->info_version);
    }

    /* 3. URL 拆解 → 下载（先 .part） */
    rc = parse_url(u, url);
    if (rc != IV_OK)
        goto OUT;
    name_from_url(u);

    join_path(final, sizeof final, dir, u->name, "");
    join_path(part, sizeof part, dir, u->name, ".part");
    rc = do_download(u, part);
    if (rc != IV_OK) {
        (void)unlink(part); /* 失败不留半截包：下次从零开始 */
        goto OUT;
    }
    if (rename(part, final) != 0) {
        IV_LOG_W(UF_MOD, "rename %s -> %s failed: %s", part, final, strerror(errno));
        (void)unlink(part);
        rc = IV_EIO;
        goto OUT;
    }
    copy_str(u->st.file, sizeof u->st.file, u->name);
    u->st.bytes += (uint64_t)u->st.last_bytes;
    IV_LOG_I(UF_MOD, "downloaded %s (%u B)", final, (unsigned)u->st.last_bytes);

    /* 4. 交给本地安装链路（本模块不装盘） */
    {
        iv_upd_cfg_t    uc;
        iv_upd_reason_t why = IV_UPD_OK;

        iv_upd_cfg_default(&uc);
        uc.data_root = u->root;
        rc = iv_upd_request_write(&uc, final, 1, &why);
        if (rc != IV_OK) {
            IV_LOG_W(UF_MOD, "write install request failed: rc=%d (%s)", rc,
                     iv_upd_reason_name(why));
            goto OUT;
        }
        IV_LOG_I(UF_MOD, "install request written: %s restart=1", final);
    }
    u->st.ok++;
    rc = IV_OK;

OUT:
    conn_close(u);
    u->running  = 0;
    u->st.last_rc   = rc;
    u->st.last_http = u->status;
    if (rc != IV_OK && rc != IV_UPDFETCH_SAME)
        u->st.fail++;
    return rc;
}

void iv_updfetch_stats(const iv_updfetch_t *u, iv_updfetch_stats_t *out)
{
    if (out == NULL)
        return;
    if (u == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = u->st;
}
