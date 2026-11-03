/*
 * 箱门抓拍图片上传实现 —— 目录队列 + `iv_uphttp`（一个文件一次请求），契约见 iv_imgup.h
 *
 * 一句话数据流：
 *   门开 → 抓拍落盘 → `iv_imgup_enqueue()` 拷进队列 → worker 里
 *   `iv_imgup_run()` 传最旧的一张（start → 整包 chunk → finish）→ 成功后删队列副本。
 *
 * 三个容易写错的地方：
 *   1) **队列副本必须是拷贝**：`/mnt/sdcard`（CD 卡）→ `/mnt/UDISK` 是两个文件
 *      系统，`link()` 会 EXDEV；而"共享一个文件 + 队列记路径"会被存储策略的
 *      保留期删原图搞成悬空引用。
 *   2) **一个文件一片**（`totalChunks=1`）：服务端不要求 1 KiB 边界，真机实测
 *      整块比 1 KiB 分片快 8 倍（164 KB：2.2 s vs 17.6 s，三份 md5 全一致，
 *      见 `iv_uphttp.h`）。重试必须**整文件重来**并换新 uploadId。
 *   3) **失败不删文件**：队列里的文件就是"没传成功"的唯一证据；删了既丢图又
 *      看不出发生过什么。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "ivsbox/iv_imgup.h"

#include "ivsbox/iv_log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IU_MOD       "imgup"
#define IU_COPY_BUF  4096u
#define IU_LIST_MAX  64u /* 单次扫描最多收这么多（队列上限远小于它） */
/* 拼路径的缓冲：目录 96 ＋ dirent 名字最长 255 ＋ "/" 与结尾 */
#define IU_PATH_MAX  (IV_IMGUP_DIR_MAX + 264u)

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

/* 递归 mkdir（只处理已存在的父目录，够用即可） */
static int mkdir_p(const char *path)
{
    char   tmp[IV_IMGUP_DIR_MAX];
    size_t i;
    size_t n;

    if (path == NULL || path[0] == '\0')
        return IV_EINVAL;
    n = strlen(path);
    if (n >= sizeof tmp)
        return IV_ERANGE;
    memcpy(tmp, path, n + 1u);
    for (i = 1u; i < n; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            IV_LOG_W(IU_MOD, "mkdir %s failed (%s)", tmp, strerror(errno));
            return IV_EIO;
        }
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        IV_LOG_W(IU_MOD, "mkdir %s failed (%s)", tmp, strerror(errno));
        return IV_EIO;
    }
    return IV_OK;
}

static void queue_default_dir(char *out, size_t cap)
{
    const char *home = getenv("IVSBOX_HOME");

    if (home != NULL && home[0] != '\0')
        (void)snprintf(out, cap, "%s/imgup", home);
    else
        (void)snprintf(out, cap, "/mnt/UDISK/ivsbox/imgup");
}

/* 队列里只认图片后缀：`snap_1793327941.jpg` / `.jpeg`（大小写不敏感） */
static int name_is_image(const char *n)
{
    size_t len;
    const char *ext;

    if (n == NULL || n[0] == '.')
        return 0;
    len = strlen(n);
    if (len < 5u)
        return 0;
    ext = n + len - 4u;
    if (strcasecmp(ext, ".jpg") == 0)
        return 1;
    if (len >= 6u && strcasecmp(n + len - 5u, ".jpeg") == 0)
        return 1;
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* 扫队列目录：只收图片文件，按文件名升序（= 时间序，`snap_<10 位秒数>`）。
 * 返回文件数；<0 = 目录读不了。 */
static int queue_scan(const iv_imgup_t *u, char (*names)[IV_IMGUP_NAME_MAX], int cap)
{
    DIR           *d;
    struct dirent *e;
    struct stat    sb;
    char           path[IU_PATH_MAX];
    int            n = 0;

    d = opendir(u->queue_dir);
    if (d == NULL)
        return (errno == ENOENT) ? 0 : -1; /* 目录还没建 = 空队列 */
    while ((e = readdir(d)) != NULL) {
        if (!name_is_image(e->d_name))
            continue;
        (void)snprintf(path, sizeof path, "%s/%s", u->queue_dir, e->d_name);
        if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode))
            continue;
        if (n >= cap)
            break;
        copy_str(names[n], IV_IMGUP_NAME_MAX, e->d_name);
        n++;
    }
    (void)closedir(d);
    if (n > 1)
        qsort(names, (size_t)n, sizeof names[0], cmp_name);
    return n;
}

/* 丢最旧的一张（队列满时）。返回 IV_OK = 真删了一张 */
static int queue_drop_oldest(iv_imgup_t *u)
{
    char names[IU_LIST_MAX][IV_IMGUP_NAME_MAX];
    char path[IU_PATH_MAX];
    int  n;

    n = queue_scan(u, names, (int)IU_LIST_MAX);
    if (n <= 0)
        return IV_ENOENT;
    (void)snprintf(path, sizeof path, "%s/%s", u->queue_dir, names[0]);
    if (unlink(path) != 0)
        return IV_EIO;
    u->st.drop++;
    IV_LOG_W(IU_MOD, "queue full, oldest dropped: %s (total drop=%llu)", names[0],
             (unsigned long long)u->st.drop);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 公开接口
 * ------------------------------------------------------------------------- */

void iv_imgup_cfg_default(iv_imgup_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->queue_dir  = NULL;
    cfg->host       = NULL;
    cfg->port       = (uint16_t)IV_UPHTTP_PORT_DEFAULT;
    cfg->bind_dev   = NULL;
    cfg->device_sn  = NULL;
    cfg->connect_ms = 5000u;
    cfg->send_ms    = 10000u;
    cfg->recv_ms    = 10000u;
}

int iv_imgup_init(iv_imgup_t *u, const iv_imgup_cfg_t *cfg)
{
    iv_uphttp_cfg_t uc;

    if (u == NULL || cfg == NULL)
        return IV_EINVAL;
    if (u->inited)
        return IV_ESTATE;

    memset(u, 0, sizeof *u);
    if (cfg->queue_dir != NULL && cfg->queue_dir[0] != '\0')
        copy_str(u->queue_dir, sizeof u->queue_dir, cfg->queue_dir);
    else
        queue_default_dir(u->queue_dir, sizeof u->queue_dir);

    iv_uphttp_cfg_default(&uc);
    uc.host       = cfg->host;
    uc.port       = cfg->port;
    uc.bind_dev   = cfg->bind_dev;
    uc.device_sn  = cfg->device_sn;
    uc.connect_ms = cfg->connect_ms;
    uc.send_ms    = cfg->send_ms;
    uc.recv_ms    = cfg->recv_ms;
    {
        int rc = iv_uphttp_init(&u->up, &uc);

        if (rc != IV_OK)
            return rc;
    }
    u->inited = 1;

    /* 目录建不出来不算致命：enqueue 时会再试，失败就记 drop */
    (void)mkdir_p(u->queue_dir);
    if (u->up.host[0] == '\0')
        IV_LOG_W(IU_MOD, "init: no host yet, set_target() before run()");
    IV_LOG_I(IU_MOD, "init: server=%s:%u queue=%s dev=%s",
             u->up.host[0] != '\0' ? u->up.host : "(unset)", (unsigned)u->up.port,
             u->queue_dir, u->up.dev_sn);
    return IV_OK;
}

int iv_imgup_set_target(iv_imgup_t *u, const char *host, uint16_t port,
                        const char *bind_dev)
{
    if (u == NULL || !u->inited)
        return IV_ESTATE;
    if (u->running)
        return IV_EBUSY;
    return iv_uphttp_set_target(&u->up, host, port, bind_dev);
}

void iv_imgup_close(iv_imgup_t *u)
{
    if (u == NULL)
        return;
    iv_uphttp_close(&u->up);
    u->inited = 0;
}

int iv_imgup_pending(const iv_imgup_t *u)
{
    char names[IU_LIST_MAX][IV_IMGUP_NAME_MAX];

    if (u == NULL || !u->inited)
        return 0;
    return queue_scan(u, names, (int)IU_LIST_MAX);
}

/* 把一个普通文件整份拷到 dst（**不用 link**：跨文件系统 + 存储策略会删原图） */
static int copy_file(const char *src, const char *dst)
{
    int     ifd;
    int     ofd;
    char    buf[IU_COPY_BUF];
    int     rc = IV_OK;

    ifd = open(src, O_RDONLY);
    if (ifd < 0)
        return IV_ENOENT;
    ofd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) {
        (void)close(ifd);
        return IV_EIO;
    }
    for (;;) {
        ssize_t n = read(ifd, buf, sizeof buf);
        ssize_t off;

        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = IV_EIO;
            break;
        }
        off = 0;
        while (off < n) {
            ssize_t w = write(ofd, buf + off, (size_t)(n - off));

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = IV_EIO;
                break;
            }
            off += w;
        }
        if (rc != IV_OK)
            break;
    }
    (void)close(ifd);
    if (rc == IV_OK && fsync(ofd) != 0)
        rc = IV_EIO; /* 掉电后队列里出现半张图比"没入队"更糟：读完就落盘 */
    if (close(ofd) != 0 && rc == IV_OK)
        rc = IV_EIO;
    if (rc != IV_OK)
        (void)unlink(dst);
    return rc;
}

int iv_imgup_enqueue(iv_imgup_t *u, const char *src_path)
{
    const char *base;
    char        dst[IU_PATH_MAX];
    struct stat sb;
    int         rc;

    if (u == NULL || !u->inited || src_path == NULL || src_path[0] == '\0')
        return IV_EINVAL;
    if (stat(src_path, &sb) != 0 || !S_ISREG(sb.st_mode))
        return IV_ENOENT;
    if (sb.st_size <= 0 || (uint64_t)sb.st_size > (uint64_t)IV_IMGUP_MAX_BYTES) {
        u->st.drop++;
        IV_LOG_W(IU_MOD, "skip %s: size %ld out of range (drop=%llu)", src_path,
                 (long)sb.st_size, (unsigned long long)u->st.drop);
        return IV_ERANGE;
    }

    base = strrchr(src_path, '/');
    base = (base != NULL) ? (base + 1) : src_path;
    if (!name_is_image(base))
        return IV_EINVAL;

    if (mkdir_p(u->queue_dir) != IV_OK)
        return IV_EIO;

    /* 队列满 → 先丢最旧的（门被反复开时不能把 UDISK 塞满） */
    {
        char names[IU_LIST_MAX][IV_IMGUP_NAME_MAX];
        int  cnt = queue_scan(u, names, (int)IU_LIST_MAX);

        while (cnt >= (int)IV_IMGUP_MAX_FILES) {
            if (queue_drop_oldest(u) != IV_OK)
                break;
            cnt = queue_scan(u, names, (int)IU_LIST_MAX);
        }
    }

    (void)snprintf(dst, sizeof dst, "%s/%s", u->queue_dir, base);
    rc = copy_file(src_path, dst);
    if (rc != IV_OK) {
        u->st.drop++;
        IV_LOG_W(IU_MOD, "enqueue %s failed: rc=%d", src_path, rc);
        return rc;
    }
    u->st.enq++;
    IV_LOG_I(IU_MOD, "enqueued %s (pending=%d)", base, iv_imgup_pending(u));
    return IV_OK;
}

int iv_imgup_run(iv_imgup_t *u)
{
    char     names[IU_LIST_MAX][IV_IMGUP_NAME_MAX];
    char     path[IU_PATH_MAX];
    struct stat sb;
    char     url[IV_IMGUP_URL_MAX];
    int      n;
    int      fd;
    int      rc;
    uint32_t total;
    int      try;

    if (u == NULL || !u->inited)
        return IV_EINVAL;
    if (u->up.host[0] == '\0')
        return IV_EINVAL;
    if (u->running)
        return IV_EBUSY;

    u->running      = 1;
    u->st.last_rc   = IV_ENOENT;
    u->st.last_http = 0;
    url[0]          = '\0';

    n = queue_scan(u, names, (int)IU_LIST_MAX);
    if (n <= 0) {
        rc = (n < 0) ? IV_EIO : IV_ENOENT; /* 空队列不是错误 */
        goto out;
    }

    (void)snprintf(path, sizeof path, "%s/%s", u->queue_dir, names[0]);
    if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size <= 0 ||
        (uint64_t)sb.st_size > (uint64_t)IV_IMGUP_MAX_BYTES) {
        u->st.drop++;
        (void)unlink(path); /* 坏件不能堵住队列头 */
        rc = IV_EIO;
        goto out;
    }
    total = (uint32_t)sb.st_size;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        rc = IV_EIO;
        goto out;
    }

    u->up.status         = 0;
    u->up.need_reconnect = 0;
    rc = iv_uphttp_connect(&u->up);
    if (rc != IV_OK) {
        (void)close(fd);
        goto out;
    }

    /* 一个文件一次请求（`totalChunks=1`）：start → 整包 chunk → finish。
     * 失败重试是**整文件重来** —— 复用同一个 uploadId 会把同一片再摞一遍，
     * 服务端存出来就是双倍长度的坏文件。 */
    rc = IV_EPROTO;
    for (try = 0; try < IV_IMGUP_RETRY_MAX; try++) {
        if (try > 0) {
            (void)lseek(fd, 0, SEEK_SET); /* 上一轮已经把 fd 读出去一截 */
            u->up.need_reconnect = 1;
        }
        rc = iv_uphttp_start(&u->up, names[0], IV_IMGUP_TYPE, total);
        if (rc != IV_OK) {
            u->up.need_reconnect = 1;
            continue;
        }
        rc = iv_uphttp_put_fd(&u->up, fd, url, sizeof url);
        if (rc == IV_OK)
            break;
        u->up.need_reconnect = 1;
    }
    (void)close(fd);
    if (rc != IV_OK) {
        IV_LOG_W(IU_MOD, "upload failed for %s (%u B): rc=%d http=%d", names[0],
                 (unsigned)total, rc, iv_uphttp_status(&u->up));
        goto out;
    }

    /* 三步全过才删队列副本 —— 这是"没传成功的证据"唯一去处 */
    (void)unlink(path);
    u->st.ok++;
    u->st.bytes += total;
    copy_str(u->st.last_name, sizeof u->st.last_name, names[0]);
    copy_str(u->st.last_url, sizeof u->st.last_url, url);
    IV_LOG_I(IU_MOD, "upload ok: %s (%u B) -> %s", names[0], (unsigned)total,
             url[0] != '\0' ? url : "(no url)");

out:
    u->st.last_rc   = rc;
    u->st.last_http = iv_uphttp_status(&u->up);
    if (rc != IV_OK && rc != IV_ENOENT)
        u->st.fail++;
    iv_uphttp_disconnect(&u->up);
    u->running = 0;
    return rc;
}

void iv_imgup_stats(const iv_imgup_t *u, iv_imgup_stats_t *out)
{
    if (u == NULL || out == NULL)
        return;
    *out = u->st;
}
