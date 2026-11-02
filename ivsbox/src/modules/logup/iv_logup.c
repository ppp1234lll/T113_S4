/*
 * 日志上报（平台 0xB2）实现 —— HTTP 三步分片上传，契约见 iv_logup.h
 *
 * 结构一句话：`iv_logup_run()` 先在 worker 线程里扫日志根、按游标算出"这次传哪
 * 一段"，再在**一条 keep-alive 连接**上跑 start → chunk×N → finish，全过才推进
 * 游标。任何一步失败都保持游标不动，下次从同一位置重传。
 *
 * 三个必须记住的口径：
 *   1) 游标是「下一个未上传字节」= `<YYYY-MM-DD> <HH> <offset>`。它是**唯一**的
 *      去重依据：别改成"按文件名判断"，那样同一小时文件被追加新行后就永远漏传。
 *   2) 服务端应答必须**按 Content-Length 读满**再发下一个请求，否则下一个请求的
 *      应答会从上一个应答的残留 body 开始解析（错位后 code 永远读不到 200）。
 *      chunked 应答读到 `\r\n0\r\n\r\n` 为止（参考实现同样处理）。
 *   3) 1 KiB 一片是**参考实现的线上口径**（服务端按 chunkIndex 拼），不要为了
 *      "快"改成 64 KiB —— 服务端的 slice 校验会按 1 KiB 边界对不上。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* SO_BINDTODEVICE / getaddrinfo 的 GNU 扩展 */
#endif

#include "ivsbox/iv_logup.h"

#include "ivsbox/iv_log.h"

#include <arpa/inet.h>
#include <dirent.h>
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
#include <time.h>
#include <unistd.h>

#define LU_MOD "logup"

/* 参考实现的请求头/表单缓冲口径（`upload_http.c`） */
#define LU_BOUNDARY      "----FNLOG_UPLOAD_BOUNDARY"
#define LU_FORM_MAX      512u
#define LU_API_CODE_OK   200 /* 服务端接口 JSON 的 `code` 成功值 */
#define LU_FILE_NAME     "device.txt" /* 参考 `LOG_CURRENT_LOG_FILE_NAME` */
#define LU_DATE_LEN      10u

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

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

/* "YYYY-MM-DD"：严格 10 位，且 '-' 只在 4/7 位、其余全是数字。
 * 严格校验是**防目录穿越**的第一道闸：日志根下只认这种名字。 */
static int date_name_ok(const char *n)
{
    unsigned i;

    if (n == NULL || strlen(n) != LU_DATE_LEN)
        return 0;
    for (i = 0; i < LU_DATE_LEN; i++) {
        if (i == 4u || i == 7u) {
            if (n[i] != '-')
                return 0;
        } else if (n[i] < '0' || n[i] > '9') {
            return 0;
        }
    }
    return 1;
}

/* 游标比较：先日期（字典序即时间序）再小时 */
static int cur_lt(const char *d1, int h1, const char *d2, int h2)
{
    int c = strcmp(d1, d2);

    if (c != 0)
        return c < 0;
    return h1 < h2;
}

/* 逐段拼接（不用 snprintf：`-O2` 下 GCC 会对"目标大小未知的 %s 拼接"报
 * -Wformat-truncation，而这里本来就有意截断 —— 手工拼更清楚也更省） */
static void append_str(char *dst, size_t cap, size_t *pos, const char *s)
{
    size_t n;

    if (*pos + 1u >= cap)
        return;
    n = strlen(s);
    if (n > cap - 1u - *pos)
        n = cap - 1u - *pos;
    memcpy(dst + *pos, s, n);
    *pos += n;
    dst[*pos] = '\0';
}

static void path_join(char *out, size_t cap, const char *a, const char *b,
                      const char *c)
{
    size_t pos = 0u;

    if (out == NULL || cap == 0u)
        return;
    out[0] = '\0';
    append_str(out, cap, &pos, a);
    append_str(out, cap, &pos, "/");
    append_str(out, cap, &pos, b);
    if (c != NULL) {
        append_str(out, cap, &pos, "/");
        append_str(out, cap, &pos, c);
    }
}

/* ---------------------------------------------------------------------------
 * 游标（`<date> <hour> <offset>` 单行文本）
 * ------------------------------------------------------------------------- */

static void cursor_default_path(char *out, size_t cap)
{
    const char *home = getenv("IVSBOX_HOME");

    if (home != NULL && home[0] != '\0')
        (void)snprintf(out, cap, "%s/logup.state", home);
    else
        (void)snprintf(out, cap, "/mnt/UDISK/ivsbox/logup.state");
}

static void cursor_load(iv_logup_t *u, char *date, int *hour, long *off)
{
    FILE *f;
    char  line[128];

    date[0] = '\0';
    *hour   = -1;
    *off    = 0L;

    f = fopen(u->state_path, "r");
    if (f == NULL)
        return; /* 没有游标文件 = 第一次上传，从头开始 */
    if (fgets(line, sizeof line, f) != NULL) {
        char d[16] = {0};
        int  h = -1;
        long o = 0L;

        if (sscanf(line, "%15s %d %ld", d, &h, &o) == 3 && date_name_ok(d) &&
            h >= 0 && h <= 23 && o >= 0L) {
            copy_str(date, LU_DATE_LEN + 1u, d);
            *hour = h;
            *off  = o;
        }
    }
    (void)fclose(f);
}

static void cursor_save(iv_logup_t *u, const char *date, int hour, long off)
{
    char  tmp[IV_LOGUP_STATE_MAX + 8u];
    FILE *f;

    (void)snprintf(tmp, sizeof tmp, "%s.tmp", u->state_path);
    f = fopen(tmp, "w");
    if (f == NULL) {
        IV_LOG_W(LU_MOD, "cursor save open failed: %s (%s)", tmp, strerror(errno));
        return;
    }
    (void)fprintf(f, "%s %02d %ld\n", date, hour, off);
    (void)fflush(f);
    (void)fclose(f);
    if (rename(tmp, u->state_path) != 0) {
        IV_LOG_W(LU_MOD, "cursor save rename failed: %s (%s)", u->state_path,
                 strerror(errno));
        (void)unlink(tmp);
    }
}

/* ---------------------------------------------------------------------------
 * 扫日志根：只收严格 "YYYY-MM-DD" 的目录，按字典序（＝时间序）排序
 * ------------------------------------------------------------------------- */

static int cmp_date(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int scan_dates(iv_logup_t *u, char (*dates)[LU_DATE_LEN + 1u], int cap)
{
    DIR           *d;
    struct dirent *e;
    struct stat    sb;
    char           path[IV_LOGUP_ROOT_MAX + 32u];
    int            n = 0;

    d = opendir(u->root);
    if (d == NULL) {
        IV_LOG_W(LU_MOD, "log root unreadable: %s (%s)", u->root, strerror(errno));
        return IV_EIO;
    }
    while ((e = readdir(d)) != NULL) {
        if (!date_name_ok(e->d_name))
            continue;
        path_join(path, sizeof path, u->root, e->d_name, NULL);
        if (stat(path, &sb) != 0 || !S_ISDIR(sb.st_mode))
            continue;
        if (n >= cap)
            break; /* 超出上限：本轮先传最早的这些，剩下的下次 */
        copy_str(dates[n], LU_DATE_LEN + 1u, e->d_name);
        n++;
    }
    (void)closedir(d);
    if (n > 1)
        qsort(dates, (size_t)n, sizeof dates[0], cmp_date);
    return n;
}

/* ---------------------------------------------------------------------------
 * 按游标走一遍日志文件（两遍复用：第一遍算尺寸，第二遍真传）
 *
 * 游标语义：只有 `== 游标文件` 的那个从 `offset` 开始，更晚的从 0 开始，更早的
 * 整份跳过。游标指向的文件已被保留期清掉（3 天）⇒ 游标不匹配任何现存文件 ⇒
 * 从最旧文件开头开始（"漏掉的不再补"是刻意选择：那些字节已经不在了）。
 * ------------------------------------------------------------------------- */

typedef struct {
    iv_logup_t *u;
    char      (*dates)[LU_DATE_LEN + 1u];
    int         nd;
    char        cur_date[LU_DATE_LEN + 1u];
    int         cur_hour;
    long        cur_off;
    size_t      want;       /* 本次预算上限 */
    size_t      got;        /* 已算入/已发送的字节数 */
    char        last_date[LU_DATE_LEN + 1u];
    int         last_hour;
    long        last_off;
    unsigned    sent_chunks;/* 第二遍用：下一个 chunkIndex */
} span_t;

/* 每段回调：path 从 off 起有 len 字节待处理；返回 IV_OK 继续 */
typedef int (*span_cb_t)(span_t *s, const char *path, long off, size_t len);

static int span_walk(span_t *s, span_cb_t cb)
{
    char   dir[IV_LOGUP_ROOT_MAX + 32u];
    char   path[IV_LOGUP_ROOT_MAX + 64u];
    char   name[8];
    int    di;
    int    hh;
    int    rc = IV_OK;

    for (di = 0; di < s->nd; di++) {
        path_join(dir, sizeof dir, s->u->root, s->dates[di], NULL);
        for (hh = 0; hh < 24; hh++) {
            struct stat sb;
            long        off;
            size_t      len;

            if (s->got >= s->want)
                return IV_OK;
            (void)snprintf(name, sizeof name, "%02d.log", hh);
            path_join(path, sizeof path, dir, name, NULL);
            if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode))
                continue;
            if (s->cur_hour >= 0) {
                if (cur_lt(s->dates[di], hh, s->cur_date, s->cur_hour))
                    continue; /* 已经被上次传过 */
                off = (strcmp(s->dates[di], s->cur_date) == 0 && hh == s->cur_hour)
                          ? s->cur_off
                          : 0L;
            } else {
                off = 0L;
            }
            if (off >= (long)sb.st_size)
                continue; /* 这一小时已传完（或文件被截断重建） */
            len = (size_t)((long)sb.st_size - off);
            if (len > s->want - s->got)
                len = s->want - s->got;
            if (len == 0u)
                continue;

            if (cb != NULL) {
                rc = cb(s, path, off, len);
                if (rc != IV_OK)
                    return rc;
            }
            s->got += len;
            copy_str(s->last_date, sizeof s->last_date, s->dates[di]);
            s->last_hour = hh;
            s->last_off  = off + (long)len;
        }
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * HTTP 客户端（阻塞、带超时、keep-alive）
 * ------------------------------------------------------------------------- */

static void conn_close(iv_logup_t *u)
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

static int conn_open(iv_logup_t *u)
{
    struct addrinfo hints;
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
        IV_LOG_W(LU_MOD, "resolve %s failed: %s", u->host, gai_strerror(rc));
        return IV_ECONN;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        int  fd;
        int  err = 0;
        int  fl;
        socklen_t elen = sizeof err;

        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        /* 出口绑定必须在 connect 前：双 WAN 同时在线时，只靠路由会把日志从
         * 不通的那条链路发出去（传输层踩过同一个坑，见 iv_transport）。 */
        if (u->bind_dev[0] != '\0') {
            if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, u->bind_dev,
                           (socklen_t)(strlen(u->bind_dev) + 1u)) != 0) {
                IV_LOG_W(LU_MOD, "bind %s failed (%s), fallback to route",
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
                IV_LOG_W(LU_MOD, "connect %s:%u timeout", u->host, (unsigned)u->port);
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
        u->fd           = fd;
        u->need_reconnect = 0;
        (void)freeaddrinfo(res);
        return IV_OK;
    }

    (void)freeaddrinfo(res);
    return IV_ECONN;
}

static int ensure_conn(iv_logup_t *u)
{
    if (u->fd >= 0 && u->need_reconnect == 0)
        return IV_OK;
    conn_close(u);
    return conn_open(u);
}

static int send_all(iv_logup_t *u, const void *buf, size_t len)
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

/* 应答是否收完（参考 `upload_http_response_complete()` 的同口径） */
static int resp_complete(const iv_logup_t *u)
{
    const char *he;
    const char *p;

    he = strstr(u->resp, "\r\n\r\n");
    if (he == NULL)
        return 0;
    p = strstr(u->resp, "Transfer-Encoding:");
    if (p != NULL && p < he) {
        return (strstr(he, "\r\n0\r\n\r\n") != NULL) ? 1 : 0;
    }
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

static int read_resp(iv_logup_t *u)
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
        char *mut = (char *)he; /* 只在本次判断里临时截断，立刻还原 */
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
static int body_int(const iv_logup_t *u, const char *key, int *out)
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

static int body_str(const iv_logup_t *u, const char *key, char *out, size_t cap)
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

static int api_ok(const iv_logup_t *u)
{
    int code = 0;

    return (body_int(u, "code", &code) == IV_OK && code == LU_API_CODE_OK) ? IV_OK
                                                                          : IV_EPROTO;
}

/* 无 body 的 POST（start / finish） */
static int post_query(iv_logup_t *u, const char *path, int keep_alive)
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

/* 一片日志：multipart/form-data，字段与参考实现逐字对齐 */
static int post_chunk(iv_logup_t *u, unsigned index, const void *data, size_t len)
{
    size_t pos;
    int    n;

    n = snprintf(u->body, sizeof u->body,
                 "--%s\r\nContent-Disposition: form-data; name=\"uploadId\"\r\n\r\n%s\r\n"
                 "--%s\r\nContent-Disposition: form-data; name=\"chunkIndex\"\r\n\r\n%u\r\n"
                 "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n",
                 LU_BOUNDARY, u->upload_id, LU_BOUNDARY, index, LU_BOUNDARY,
                 u->file_name);
    if (n < 0 || (size_t)n + len + 64u >= sizeof u->body)
        return IV_ERANGE;
    pos = (size_t)n;
    memcpy(u->body + pos, data, len);
    pos += len;
    n = snprintf(u->body + pos, sizeof u->body - pos, "\r\n--%s--\r\n", LU_BOUNDARY);
    if (n < 0 || (size_t)n >= sizeof u->body - pos)
        return IV_ERANGE;
    pos += (size_t)n;

    if (ensure_conn(u) != IV_OK)
        return IV_ECONN;
    n = snprintf(u->hdr, sizeof u->hdr,
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Content-Type: multipart/form-data; boundary=%s\r\n"
                 "Content-Length: %lu\r\n"
                 "Connection: keep-alive\r\n\r\n",
                 IV_LOGUP_PATH_CHUNK, u->host, (unsigned)u->port, LU_BOUNDARY,
                 (unsigned long)pos);
    if (n < 0 || (size_t)n >= sizeof u->hdr)
        return IV_ERANGE;
    if (send_all(u, u->hdr, (size_t)n) != IV_OK ||
        send_all(u, u->body, pos) != IV_OK) {
        u->need_reconnect = 1;
        return IV_ECONN;
    }
    if (read_resp(u) != IV_OK)
        return u->need_reconnect ? IV_ECONN : IV_ETIMEDOUT;
    if (u->status < 200 || u->status >= 300)
        return IV_EPROTO;
    return api_ok(u);
}

/* ---------------------------------------------------------------------------
 * 上传主体
 * ------------------------------------------------------------------------- */

/* 第二遍的读文件回调：一个文件段可能跨多片，按 1 KiB 切开逐片发 */
static int span_send(span_t *s, const char *path, long off, size_t len)
{
    iv_logup_t *u = s->u;
    int         fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        IV_LOG_W(LU_MOD, "open %s failed (%s)", path, strerror(errno));
        return IV_EIO;
    }
    if (off > 0 && lseek(fd, off, SEEK_SET) < 0) {
        (void)close(fd);
        return IV_EIO;
    }
    while (len > 0u) {
        size_t  want = (len > IV_LOGUP_CHUNK_SIZE) ? IV_LOGUP_CHUNK_SIZE : len;
        ssize_t n    = read(fd, u->chunk, want);
        int     try;
        int     rc   = IV_EIO;

        if (n <= 0) {
            (void)close(fd);
            return IV_EIO;
        }
        for (try = 0; try < IV_LOGUP_RETRY_MAX; try++) {
            rc = post_chunk(u, s->sent_chunks, u->chunk, (size_t)n);
            if (rc == IV_OK)
                break;
            u->need_reconnect = 1; /* 下一轮从同一下标重发（幂等） */
        }
        if (rc != IV_OK) {
            IV_LOG_W(LU_MOD, "chunk #%u failed: rc=%d http=%d", s->sent_chunks, rc,
                     u->status);
            (void)close(fd);
            return (rc == IV_EPROTO) ? IV_EPROTO : IV_ECONN;
        }
        s->sent_chunks++;
        len -= (size_t)n;
    }
    (void)close(fd);
    return IV_OK;
}

static int start_upload(iv_logup_t *u, size_t total, unsigned total_chunks)
{
    char path[384];
    int  n;
    int  try;
    int  rc = IV_EPROTO;

    n = snprintf(path, sizeof path,
                 "%s?deviceSn=%s&totalSize=%lu&totalChunks=%u&fileName=%s&logType=%s",
                 IV_LOGUP_PATH_START, u->dev_sn, (unsigned long)total, total_chunks,
                 u->file_name, u->log_type);
    if (n < 0 || (size_t)n >= sizeof path)
        return IV_ERANGE;

    for (try = 0; try < IV_LOGUP_RETRY_MAX; try++) {
        rc = post_query(u, path, 1);
        if (rc == IV_OK && api_ok(u) == IV_OK &&
            body_str(u, "uploadId", u->upload_id, sizeof u->upload_id) == IV_OK &&
            u->upload_id[0] != '\0') {
            return IV_OK;
        }
        if (rc == IV_OK)
            rc = IV_EPROTO; /* 2xx 但 JSON 里没有 uploadId */
        u->need_reconnect = 1;
    }
    return rc;
}

static int finish_upload(iv_logup_t *u)
{
    char path[256];
    int  n;
    int  try;
    int  rc = IV_EPROTO;

    n = snprintf(path, sizeof path, "%s?uploadId=%s", IV_LOGUP_PATH_FINISH,
                 u->upload_id);
    if (n < 0 || (size_t)n >= sizeof path)
        return IV_ERANGE;
    for (try = 0; try < IV_LOGUP_RETRY_MAX; try++) {
        rc = post_query(u, path, 0);
        if (rc == IV_OK)
            return api_ok(u);
        u->need_reconnect = 1;
    }
    return rc;
}

/* ---------------------------------------------------------------------------
 * 公开接口
 * ------------------------------------------------------------------------- */

void iv_logup_cfg_default(iv_logup_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->root       = NULL;
    cfg->state_path = NULL;
    cfg->host       = NULL;
    cfg->port       = IV_LOGUP_PORT_DEFAULT;
    cfg->bind_dev   = NULL;
    cfg->device_sn  = NULL;
    cfg->log_type   = NULL;
    cfg->max_bytes  = IV_LOGUP_MAX_BYTES_DEFAULT;
    cfg->connect_ms = 5000u;
    cfg->send_ms    = 10000u;
    cfg->recv_ms    = 10000u;
}

int iv_logup_init(iv_logup_t *u, const iv_logup_cfg_t *cfg)
{
    if (u == NULL || cfg == NULL)
        return IV_EINVAL;
    if (u->inited)
        return IV_ESTATE;

    memset(u, 0, sizeof *u);
    u->fd = -1;

    copy_str(u->root, sizeof u->root,
             (cfg->root != NULL && cfg->root[0] != '\0') ? cfg->root
                                                         : IV_LOG_ROOT_DEFAULT);
    if (cfg->state_path != NULL && cfg->state_path[0] != '\0')
        copy_str(u->state_path, sizeof u->state_path, cfg->state_path);
    else
        cursor_default_path(u->state_path, sizeof u->state_path);
    copy_str(u->host, sizeof u->host, cfg->host);
    copy_str(u->bind_dev, sizeof u->bind_dev, cfg->bind_dev);
    copy_str(u->dev_sn, sizeof u->dev_sn,
             (cfg->device_sn != NULL && cfg->device_sn[0] != '\0') ? cfg->device_sn : "0");
    copy_str(u->log_type, sizeof u->log_type,
             (cfg->log_type != NULL && cfg->log_type[0] != '\0') ? cfg->log_type
                                                                 : "SYSTEM");
    u->port       = (cfg->port != 0u) ? cfg->port : (uint16_t)IV_LOGUP_PORT_DEFAULT;
    u->max_bytes  = (cfg->max_bytes != 0u) ? cfg->max_bytes : IV_LOGUP_MAX_BYTES_DEFAULT;
    u->connect_ms = (cfg->connect_ms != 0u) ? cfg->connect_ms : 5000u;
    u->send_ms    = (cfg->send_ms != 0u) ? cfg->send_ms : 10000u;
    u->recv_ms    = (cfg->recv_ms != 0u) ? cfg->recv_ms : 10000u;
    u->inited     = 1;

    if (u->host[0] == '\0') {
        /* 不判失败：装配层常常先 init、等出口定了再 set_target 补地址 */
        IV_LOG_W(LU_MOD, "init: no host yet, set_target() before run()");
    }
    IV_LOG_I(LU_MOD, "init: server=%s:%u root=%s state=%s dev=%s type=%s max=%u",
             u->host[0] != '\0' ? u->host : "(unset)", (unsigned)u->port, u->root,
             u->state_path, u->dev_sn, u->log_type, (unsigned)u->max_bytes);
    return IV_OK;
}

int iv_logup_set_target(iv_logup_t *u, const char *host, uint16_t port,
                        const char *bind_dev)
{
    if (u == NULL || !u->inited)
        return IV_ESTATE;
    if (u->running)
        return IV_EBUSY;
    if (host == NULL || host[0] == '\0')
        return IV_EINVAL;
    if (strcmp(u->host, host) != 0) {
        copy_str(u->host, sizeof u->host, host);
        u->need_reconnect = 1; /* 旧连接指向旧地址，必须换 */
    }
    u->port = (port != 0u) ? port : (uint16_t)IV_LOGUP_PORT_DEFAULT;
    copy_str(u->bind_dev, sizeof u->bind_dev, bind_dev);
    return IV_OK;
}

void iv_logup_close(iv_logup_t *u)
{
    if (u == NULL)
        return;
    conn_close(u);
    u->inited = 0;
}

int iv_logup_run(iv_logup_t *u)
{
    char        dates[IV_LOGUP_DAYS_MAX][LU_DATE_LEN + 1u];
    span_t      sp;
    int         nd;
    int         rc = IV_OK;
    size_t      total;
    unsigned    chunks;

    if (u == NULL || !u->inited)
        return IV_EINVAL;
    if (u->host[0] == '\0')
        return IV_EINVAL; /* 装配层忘了 set_target（或配置里根本没有地址） */
    if (u->running)
        return IV_EBUSY;

    u->running        = 1;
    u->st.runs++;
    u->status         = 0;
    u->need_reconnect = 0;

    memset(&sp, 0, sizeof sp);
    sp.u = u;
    nd   = scan_dates(u, dates, (int)IV_LOGUP_DAYS_MAX);
    if (nd < 0) {
        rc = nd;
        goto out;
    }
    if (nd == 0) {
        IV_LOG_I(LU_MOD, "no log dir under %s", u->root);
        rc = IV_ENOENT;
        goto out;
    }

    sp.dates = dates;
    sp.nd    = nd;
    sp.want  = u->max_bytes;
    cursor_load(u, sp.cur_date, &sp.cur_hour, &sp.cur_off);

    rc = span_walk(&sp, NULL); /* 第一遍：算本次要传多少 */
    if (rc != IV_OK)
        goto out;
    total = sp.got;
    if (total == 0u) {
        IV_LOG_I(LU_MOD, "no new log since cursor %s %02d +%ld", sp.cur_date,
                 sp.cur_hour, sp.cur_off);
        rc = IV_ENOENT;
        goto out;
    }
    chunks = (unsigned)((total + IV_LOGUP_CHUNK_SIZE - 1u) / IV_LOGUP_CHUNK_SIZE);

    copy_str(u->file_name, sizeof u->file_name, LU_FILE_NAME);
    u->upload_id[0] = '\0';

    IV_LOG_I(LU_MOD, "upload start: %u B in %u chunks -> %s:%u (from %s %02d +%ld)",
             (unsigned)total, chunks, u->host, (unsigned)u->port, sp.last_date,
             sp.last_hour, sp.last_off);

    rc = conn_open(u);
    if (rc != IV_OK)
        goto out;
    rc = start_upload(u, total, chunks);
    if (rc != IV_OK) {
        IV_LOG_W(LU_MOD, "start failed: rc=%d http=%d", rc, u->status);
        goto out;
    }

    /* 第二遍：从同一游标起真读真发。游标的"起始"位置由 walk 内部按 cur_* 判定，
     * 这里把 got 清零（第一遍累计的量只用于算尺寸）。 */
    sp.got         = 0u;
    sp.sent_chunks = 0u;
    rc             = span_walk(&sp, span_send);
    if (rc != IV_OK) {
        IV_LOG_W(LU_MOD, "chunks aborted at #%u: rc=%d http=%d", sp.sent_chunks, rc,
                 u->status);
        goto out;
    }
    if (sp.sent_chunks != chunks) {
        /* 文件在传输期间被清理会走到这里：宁可不推进游标，下次重传 */
        IV_LOG_W(LU_MOD, "chunk count mismatch: sent=%u planned=%u", sp.sent_chunks,
                 chunks);
        rc = IV_EIO;
        goto out;
    }

    rc = finish_upload(u);
    if (rc != IV_OK) {
        IV_LOG_W(LU_MOD, "finish failed: rc=%d http=%d", rc, u->status);
        goto out;
    }

    /* 只有三步全过才推进游标 —— 失败必重传是这套游标的核心承诺 */
    cursor_save(u, sp.last_date, sp.last_hour, sp.last_off);
    u->st.bytes += total;
    u->st.chunks += chunks;
    u->st.last_bytes = (uint32_t)total;
    copy_str(u->st.last_file, sizeof u->st.last_file, u->file_name);
    IV_LOG_I(LU_MOD, "upload ok: %u B, %u chunks, cursor -> %s %02d +%ld",
             (unsigned)total, chunks, sp.last_date, sp.last_hour, sp.last_off);

out:
    if (rc == IV_OK) {
        u->st.ok++;
    } else if (rc != IV_ENOENT) {
        /* "没有新日志"不计失败：它是正常状态（游标已在最新），装配层也只打一行
         * INFO。把它算进 fail 会让现场看到"日志上传一直在失败"，去查一个不存在的
         * 网络问题。 */
        u->st.fail++;
        u->st.last_bytes = 0u;
    }
    u->st.last_rc   = rc;
    u->st.last_http = u->status;
    conn_close(u);
    u->running = 0;
    return rc;
}

void iv_logup_stats(const iv_logup_t *u, iv_logup_stats_t *out)
{
    if (u == NULL || out == NULL)
        return;
    *out = u->st;
}
