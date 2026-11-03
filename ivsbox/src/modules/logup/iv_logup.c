/*
 * 日志上报（平台 0xB2）实现 —— HTTP 三步分片上传，契约见 iv_logup.h
 *
 * 结构一句话：`iv_logup_run()` 先在 worker 线程里扫日志根、按游标算出"这次传哪
 * 一段"，再在**一条 keep-alive 连接**上跑 start → chunk×N → finish，全过才推进
 * 游标。任何一步失败都保持游标不动，下次从同一位置重传。
 *
 * 2026-11-03：**HTTP 那一半已抽到 `iv_uphttp`**（箱门图片上传要复用同一套），
 * 本文件只剩"传哪一段字节"：游标、扫目录、按整条字节流切 1 KiB 片。
 * 连接/keep-alive/"按 Content-Length 读满应答"/multipart 组包都在 iv_uphttp.c。
 *
 * 两个必须记住的口径：
 *   1) 游标是「下一个未上传字节」= `<YYYY-MM-DD> <HH> <offset>`。它是**唯一**的
 *      去重依据：别改成"按文件名判断"，那样同一小时文件被追加新行后就永远漏传。
 *   2) 分片边界必须落在**整条字节流**上（`span_flush()` 攒满才发）：按"每个日志
 *      文件各自切片"会让片数 > ceil(total/1024)，服务端回"分片序号无效"。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* SO_BINDTODEVICE / getaddrinfo 的 GNU 扩展 */
#endif

#include "ivsbox/iv_logup.h"

#include "ivsbox/iv_log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LU_MOD "logup"

#define LU_FILE_NAME "device.txt" /* 参考 `LOG_CURRENT_LOG_FILE_NAME` */
#define LU_DATE_LEN  10u

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
 * 上传主体
 * ------------------------------------------------------------------------- */

/* 第二遍的读文件回调：把这一段字节**直接推进上传流**。
 * 2026-11-03 起整次上传只有一片（`totalChunks=1`），所以不再攒 1 KiB：
 * 读多少发多少，内存只占一个 `IV_UPHTTP_SEND_CHUNK` 的栈缓冲。 */
static int span_send(span_t *s, const char *path, long off, size_t len)
{
    iv_logup_t *u = s->u;
    uint8_t     buf[IV_UPHTTP_SEND_CHUNK];
    int         fd;
    int         rc;

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
        size_t  want = (len > sizeof buf) ? sizeof buf : len;
        ssize_t n    = read(fd, buf, want);

        if (n <= 0) {
            IV_LOG_W(LU_MOD, "read %s failed at +%ld (%s)", path, off,
                     strerror(errno));
            (void)close(fd);
            return IV_EIO;
        }
        /* 头已经发出去了，Content-Length 不能改 ⇒ 这里失败只能让整次上传作废，
         * 游标不推进，下次从头重传（日志是整段快照，重传不会产生重复数据）。 */
        rc = iv_uphttp_write(&u->up, buf, (size_t)n);
        if (rc != IV_OK) {
            IV_LOG_W(LU_MOD, "write failed inside %s: rc=%d http=%d", path, rc,
                     iv_uphttp_status(&u->up));
            (void)close(fd);
            return rc;
        }
        len -= (size_t)n;
        off += (long)n;
    }
    (void)close(fd);
    return IV_OK;
}

/* start：文件名/类型/尺寸由本层给，协议细节在 `iv_uphttp_start()`。
 * 重试留在本层（同一次 start 重发是幂等的：服务端每次给一个新 uploadId，
 * 只是废掉前一具"半截会话"）。 */
static int start_upload(iv_logup_t *u, size_t total)
{
    int try;
    int rc = IV_EPROTO;

    /* start 可重试：服务端每次给一个新 uploadId，旧的半截会话只是作废 */
    for (try = 0; try < IV_LOGUP_RETRY_MAX; try++) {
        rc = iv_uphttp_start(&u->up, u->file_name, u->log_type, (uint32_t)total);
        if (rc == IV_OK)
            break;
        u->up.need_reconnect = 1;
    }
    if (rc != IV_OK)
        return rc;
    /* 前缀只发一次：重发会把 uploadId/chunkIndex 那一段摞两遍，服务端存出来
     * 的文件头就是坏的。失败就让整次上传作废，游标不推进。 */
    return iv_uphttp_begin(&u->up);
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
    iv_uphttp_cfg_t uc;

    if (u == NULL || cfg == NULL)
        return IV_EINVAL;
    if (u->inited)
        return IV_ESTATE;

    memset(u, 0, sizeof *u);

    copy_str(u->root, sizeof u->root,
             (cfg->root != NULL && cfg->root[0] != '\0') ? cfg->root
                                                         : IV_LOG_ROOT_DEFAULT);
    if (cfg->state_path != NULL && cfg->state_path[0] != '\0')
        copy_str(u->state_path, sizeof u->state_path, cfg->state_path);
    else
        cursor_default_path(u->state_path, sizeof u->state_path);
    copy_str(u->log_type, sizeof u->log_type,
             (cfg->log_type != NULL && cfg->log_type[0] != '\0') ? cfg->log_type
                                                                 : "SYSTEM");
    u->max_bytes  = (cfg->max_bytes != 0u) ? cfg->max_bytes : IV_LOGUP_MAX_BYTES_DEFAULT;

    /* HTTP 那一半交给 `iv_uphttp` 初始化（图片上传走同一份实现） */
    iv_uphttp_cfg_default(&uc);
    uc.host       = cfg->host;
    uc.port       = (cfg->port != 0u) ? cfg->port : (uint16_t)IV_LOGUP_PORT_DEFAULT;
    uc.bind_dev   = cfg->bind_dev;
    uc.device_sn  = cfg->device_sn;
    uc.connect_ms = cfg->connect_ms;
    uc.send_ms    = cfg->send_ms;
    uc.recv_ms    = cfg->recv_ms;
    {
        int urc = iv_uphttp_init(&u->up, &uc);

        if (urc != IV_OK)
            return urc;
    }
    u->inited     = 1;

    if (u->up.host[0] == '\0') {
        /* 不判失败：装配层常常先 init、等出口定了再 set_target 补地址 */
        IV_LOG_W(LU_MOD, "init: no host yet, set_target() before run()");
    }
    IV_LOG_I(LU_MOD, "init: server=%s:%u root=%s state=%s dev=%s type=%s max=%u",
             u->up.host[0] != '\0' ? u->up.host : "(unset)", (unsigned)u->up.port,
             u->root, u->state_path, u->up.dev_sn, u->log_type,
             (unsigned)u->max_bytes);
    return IV_OK;
}

int iv_logup_set_target(iv_logup_t *u, const char *host, uint16_t port,
                        const char *bind_dev)
{
    if (u == NULL || !u->inited)
        return IV_ESTATE;
    if (u->running)
        return IV_EBUSY; /* 上传中途换地址会让 keep-alive 与游标同时错位 */
    return iv_uphttp_set_target(&u->up, host, port, bind_dev);
}

void iv_logup_close(iv_logup_t *u)
{
    if (u == NULL)
        return;
    iv_uphttp_close(&u->up);
    u->inited = 0;
}

int iv_logup_run(iv_logup_t *u)
{
    char        dates[IV_LOGUP_DAYS_MAX][LU_DATE_LEN + 1u];
    span_t      sp;
    int         nd;
    int         rc = IV_OK;
    size_t      total;

    if (u == NULL || !u->inited)
        return IV_EINVAL;
    if (u->up.host[0] == '\0')
        return IV_EINVAL; /* 装配层忘了 set_target（或配置里根本没有地址） */
    if (u->running)
        return IV_EBUSY;

    u->running           = 1;
    u->st.runs++;
    u->up.status         = 0;
    u->up.need_reconnect = 0;

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
    copy_str(u->file_name, sizeof u->file_name, LU_FILE_NAME);

    /* 打的是**起点**（游标 cur_*），不是 last_* —— last 是本次要走到哪儿，
     * 结束行才打它。2026-11-03 就因为这行印了 last，start/ok 两行一模一样，
     * 被误读成"游标没推进"。 */
    IV_LOG_I(LU_MOD, "upload start: %u B (one request) -> %s:%u (from %s %02d +%ld)",
             (unsigned)total, u->up.host, (unsigned)u->up.port,
             (sp.cur_hour >= 0) ? sp.cur_date : "(no cursor)",
             (sp.cur_hour >= 0) ? sp.cur_hour : 0, sp.cur_off);

    rc = iv_uphttp_connect(&u->up);
    if (rc != IV_OK)
        goto out;
    rc = start_upload(u, total);
    if (rc != IV_OK) {
        IV_LOG_W(LU_MOD, "start failed: rc=%d http=%d", rc,
                 iv_uphttp_status(&u->up));
        goto out;
    }

    /* 第二遍：从同一游标起真读真发。游标的"起始"位置由 walk 内部按 cur_* 判定，
     * 这里把 got 清零（第一遍累计的量只用于算尺寸）。 */
    sp.got = 0u;
    rc     = span_walk(&sp, span_send);
    if (rc != IV_OK) {
        IV_LOG_W(LU_MOD, "body aborted after %u/%u B: rc=%d http=%d", (unsigned)sp.got,
                 (unsigned)total, rc, iv_uphttp_status(&u->up));
        goto out;
    }
    if (sp.got != total) {
        /* 文件在传输期间被清理会走到这里：宁可不推进游标，下次重传 */
        IV_LOG_W(LU_MOD, "byte count mismatch: sent=%u planned=%u", (unsigned)sp.got,
                 (unsigned)total);
        rc = IV_EIO;
        goto out;
    }

    rc = iv_uphttp_end(&u->up, NULL, 0); /* 日志不关心返回的 URL */
    if (rc != IV_OK) {
        IV_LOG_W(LU_MOD, "finish failed: rc=%d http=%d resp=%s", rc,
                 iv_uphttp_status(&u->up), iv_uphttp_resp(&u->up));
        goto out;
    }
    /* 只有三步全过才推进游标 —— 失败必重传是这套游标的核心承诺 */
    cursor_save(u, sp.last_date, sp.last_hour, sp.last_off);
    u->st.bytes += total;
    u->st.chunks += 1u; /* 一次上传 = 一个请求（历史字段名保留，语义已变） */
    u->st.last_bytes = (uint32_t)total;
    copy_str(u->st.last_file, sizeof u->st.last_file, u->file_name);
    IV_LOG_I(LU_MOD, "upload ok: %u B (1 request), cursor -> %s %02d +%ld",
             (unsigned)total, sp.last_date, sp.last_hour, sp.last_off);

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
    u->st.last_http = iv_uphttp_status(&u->up);
    iv_uphttp_disconnect(&u->up);
    u->running = 0;
    return rc;
}

void iv_logup_stats(const iv_logup_t *u, iv_logup_stats_t *out)
{
    if (u == NULL || out == NULL)
        return;
    *out = u->st;
}
