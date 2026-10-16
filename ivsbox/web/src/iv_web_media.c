/*
 * ivsbox-web 媒体文件下载（M6-S6.4 方式二：HTTP Range 下载）
 *
 * 设计总纲、分层偏离登记、限速与取消语义都在 `iv_web.h` 的文件头，本文件只写
 * **实现要点**。一句话：把 `/media/...` 映射到 `media_root` 下的普通文件，做
 * 路径围栏 + Range/ETag/校验值，正文交给 http 层流式发送。
 *
 * ============================ 路径围栏（三道） ============================
 * 1. **逐段校验**：把 `/media` 前缀剥掉后剩下的相对路径逐段检查，字符只允许
 *    `[A-Za-z0-9._-@+]`，且**单段不得是 `.` 或 `..`**。
 *    ⚠ 这里与 `iv_web_static.c` 的 `path_safe()` 有个关键差别：那边只做"字符
 *    白名单"，而 `..` 恰好**完全由白名单字符组成**（`.` 与 `/`），所以单靠
 *    字符白名单拦不住 `../..`；本层因此**额外逐段拒绝 `.` / `..`**。
 * 2. **realpath 复核前缀**：`realpath()` 把符号链接、`.` 段、重复 `/` 全部消掉，
 *    结果必须落在 `realpath(media_root)` 之内（前缀相等且后随 `/` 或串尾）。
 * 3. **`open(O_NOFOLLOW)` + `fstat`**：在 http 层真正打开时不做符号链接跟随，
 *    并确认是普通文件（不是目录/设备/FIFO）。
 *
 * ============================ Range / ETag / 校验值 ============================
 *   - 只认 `bytes=` 单区间（`a-b` / `a-` / `-n`）；多区间与非法语法**忽略 Range
 *     走 200 全量**（规范允许忽略，且比"实现多区间 multipart/byteranges"省一整套
 *     边界生成逻辑，本工程用不到）。
 *   - 起址越界（`a >= size`）⇒ **416** 并带 `Content-Range: bytes *​/size`
 *     （注意：这个头里的 `*` 是内容，不是注释）。
 *   - `ETag: "<size>-<mtime>"` 恒给；`If-Range` 与它**逐字节相等**才认 Range，
 *     否则按全量 200 回（规范的"断点续传前提"语义）。
 *   - 校验值 `X-SHA256` **只在请求带 `?sha=1` 时才算**：全量摘要要把文件再整读
 *     一遍，对一个 15 分钟的录像就是几百 MB 的额外 IO，不能默认背上；而 206 分片
 *     也无法校验全量摘要，故分片响应永远不给这个头。
 *
 * ============================ 一连接一请求的静态缓冲 ============================
 * `s_media_path` 是**静态**的：本函数返回后 http 层才 `open()` 它。这是安全的，
 * 因为事件循环是单线程、一次只处理一条请求，本函数返回前不会插进第二个请求。
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_sha256.h"
#include "iv_web.h"

#define MEDIA_MOD     "web.media"
#define MEDIA_PREFIX  "/media"
#define MEDIA_PFX_LEN 6u
#define MEDIA_SEG_MAX 128u   /* 单段路径长度上限（录像/抓图文件名远短于此） */

/* Range 解析三态 */
#define RANGE_NONE  0   /* 无 Range / 语法不认识 ⇒ 按全量 200 */
#define RANGE_OK    1   /* 已填 off/len */
#define RANGE_UNSAT (-1) /* 起址越界 ⇒ 416 */

static char s_media_path[IV_WEB_MEDIA_ROOT_MAX + IV_WEB_PATH_MAX + 2u];

/* --------------------------------------------------------------------------
 * 小工具
 * -------------------------------------------------------------------------- */
static void media_err(char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp,
                      int status, const char *msg)
{
    int n = snprintf(out, cap, "{\"ok\":false,\"e\":%d,\"msg\":\"%s\"}", status, msg);

    if (n < 0)
        *out_len = 0u;
    else if ((size_t)n >= cap)
        *out_len = cap - 1u;
    else
        *out_len = (size_t)n;
    rsp->status = status;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len  = *out_len;
    rsp->file_path = NULL;
}

/* 追加到 extra（有界；放不下就丢弃该行，绝不越界）。返回新的已用长度。 */
static size_t extra_add(char *buf, size_t cap, size_t used, const char *s)
{
    size_t n = strlen(s);

    if (used + n + 1u > cap)
        return used;
    memcpy(buf + used, s, n);
    buf[used + n] = '\0';
    return used + n;
}

/* 相对路径逐段校验（见文件头第 1 道围栏）。 */
static int rel_path_ok(const char *rel)
{
    const char *p = rel;

    if (p == NULL || p[0] != '/')
        return 0;
    while (*p != '\0') {
        const char *seg;
        size_t      len;

        if (*p == '/') {
            p++;
            continue;
        }
        seg = p;
        while (*p != '\0' && *p != '/') {
            unsigned char c = (unsigned char)*p;

            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
                c == '@' || c == '+')
                p++;
            else
                return 0;
        }
        len = (size_t)(p - seg);
        if (len == 0u || len > MEDIA_SEG_MAX)
            return 0;
        if ((len == 1u && seg[0] == '.') ||
            (len == 2u && seg[0] == '.' && seg[1] == '.'))
            return 0;   /* `.` / `..` 段：穿越的唯一入口，直接拒 */
    }
    return 1;
}

static int within_root(const char *root, const char *real)
{
    size_t rl = strlen(root);

    if (rl == 0u || strncmp(root, real, rl) != 0)
        return 0;
    return (real[rl] == '\0' || real[rl] == '/');
}

static void make_etag(const struct stat *st, char *out, size_t cap)
{
    (void)snprintf(out, cap, "\"%llu-%llu\"",
                   (unsigned long long)st->st_size,
                   (unsigned long long)st->st_mtime);
}

static const char *media_ctype(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (dot == NULL)
        return "application/octet-stream";
    if (strcmp(dot, ".mp4") == 0)
        return "video/mp4";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".txt") == 0)
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/* 解析 `Range: bytes=a-b` / `a-` / `-n`（单区间）。见文件头说明。 */
static int parse_range(const char *hdr, uint64_t size, uint64_t *off, uint64_t *len)
{
    char               tmp[32];
    const char        *p;
    const char        *q;
    unsigned long long a;
    unsigned long long b;
    size_t             n;

    if (hdr == NULL || hdr[0] == '\0')
        return RANGE_NONE;
    if (strncmp(hdr, "bytes=", 6) != 0)
        return RANGE_NONE;
    p = hdr + 6;
    if (strchr(p, ',') != NULL)
        return RANGE_NONE;          /* 多区间：不支持，忽略 */
    if (size == 0ull)
        return RANGE_UNSAT;         /* 空文件：任何区间都不满足 */

    if (*p == '-') {                /* 后缀区间 -N */
        q = p + 1;
        n = 0;
        while (*q >= '0' && *q <= '9' && n < sizeof(tmp) - 1u)
            tmp[n++] = *q++;
        tmp[n] = '\0';
        if (n == 0u || *q != '\0')
            return RANGE_NONE;
        b = strtoull(tmp, NULL, 10);
        if (b == 0ull)
            return RANGE_UNSAT;     /* 规范：后缀 0 不满足 */
        if (b > size)
            b = size;
        *off = (uint64_t)(size - b);
        *len = (uint64_t)b;
        return RANGE_OK;
    }

    /* 起址 a */
    q = p;
    n = 0;
    while (*q >= '0' && *q <= '9' && n < sizeof(tmp) - 1u)
        tmp[n++] = *q++;
    tmp[n] = '\0';
    if (n == 0u || *q != '-')
        return RANGE_NONE;
    a = strtoull(tmp, NULL, 10);
    q++;
    if (*q == '\0') {               /* a- ：到文件尾 */
        if (a >= (unsigned long long)size)
            return RANGE_UNSAT;
        *off = (uint64_t)a;
        *len = (uint64_t)((unsigned long long)size - a);
        return RANGE_OK;
    }

    /* a-b */
    n = 0;
    {
        const char *r = q;

        while (*r >= '0' && *r <= '9' && n < sizeof(tmp) - 1u)
            tmp[n++] = *r++;
        tmp[n] = '\0';
        if (n == 0u || *r != '\0')
            return RANGE_NONE;
    }
    b = strtoull(tmp, NULL, 10);
    if (a >= (unsigned long long)size)
        return RANGE_UNSAT;
    if (b >= (unsigned long long)size)
        b = (unsigned long long)size - 1ull;
    if (b < a)
        return RANGE_NONE;          /* 反向区间：忽略 */
    *off = (uint64_t)a;
    *len = (uint64_t)(b - a + 1ull);
    return RANGE_OK;
}

/* 请求串里有没有 `sha` 这个键（值为何都算）。 */
static int query_has(const char *q, const char *key)
{
    size_t      klen = strlen(key);
    const char *p = q;

    if (q == NULL)
        return 0;
    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        size_t      seg = (amp != NULL) ? (size_t)(amp - p) : strlen(p);

        if (seg >= klen && strncmp(p, key, klen) == 0 &&
            (seg == klen || p[klen] == '='))
            return 1;
        if (amp == NULL)
            break;
        p = amp + 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * 入口
 * -------------------------------------------------------------------------- */
int iv_web_media_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                        char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    char        root[PATH_MAX];
    char        real[PATH_MAX];
    char        etag[48];
    const char *rel;
    struct stat st;
    uint64_t    size;
    uint64_t    off = 0;
    uint64_t    rlen = 0;
    int         rr;
    int         partial = 0;
    int         want_sha;
    size_t      used = 0;
    const char *root_src;

    rsp->status = 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->set_cookie = 0;
    rsp->file_path  = NULL;

    if (strncmp(req->path, MEDIA_PREFIX, MEDIA_PFX_LEN) != 0)
        return IV_ENOTSUP;
    /* `/mediafoo` 不是本层路径（前缀必须成段） */
    if (req->path[MEDIA_PFX_LEN] != '\0' && req->path[MEDIA_PFX_LEN] != '/')
        return IV_ENOTSUP;

    if (strcmp(req->method, "GET") != 0) {
        media_err(out, cap, out_len, rsp, 405, "method not allowed");
        return IV_OK;
    }
    /* 媒体能拿到录像内容，**必须鉴权**（静态页面才是例外） */
    if (!iv_web_sess_check(ctx, req->token)) {
        media_err(out, cap, out_len, rsp, 401, "unauthorized");
        return IV_OK;
    }
    /* M10：出厂默认口令期连录像也不给下载 —— 否则"必须改密"只是不给看界面，
     * 拿出厂口令的人照样能把录像拉走（与 api.c 的同名闸门同一口径）。 */
    if (ctx->must_change != 0) {
        media_err(out, cap, out_len, rsp, 403, "must change password");
        return IV_OK;
    }

    rel = req->path + MEDIA_PFX_LEN;
    if (rel[0] == '\0')
        rel = "/";
    if (!rel_path_ok(rel)) {
        media_err(out, cap, out_len, rsp, 403, "forbidden path");
        return IV_OK;
    }

    root_src = (ctx->media_root[0] != '\0') ? ctx->media_root
                                            : IV_WEB_MEDIA_ROOT_DEFAULT;
    if (realpath(root_src, root) == NULL) {
        /* 媒体根不在（没插 SD/没挂载）⇒ 404：服务本身没问题，是资源没有 */
        media_err(out, cap, out_len, rsp, 404, "media root not available");
        return IV_OK;
    }

    {
        size_t rl = strlen(root);
        size_t ll = strlen(rel);

        if (rl + ll + 1u > sizeof(real)) {
            media_err(out, cap, out_len, rsp, 414, "path too long");
            return IV_OK;
        }
        memcpy(real, root, rl);
        memcpy(real + rl, rel, ll);
        real[rl + ll] = '\0';
    }

    /* 第 2 道围栏：realpath 把符号链接/`.` 段/重复斜杠全解掉，再复核仍在根内 */
    {
        char resolved[PATH_MAX];

        if (realpath(real, resolved) == NULL) {
            media_err(out, cap, out_len, rsp, 404, "not found");
            return IV_OK;
        }
        if (!within_root(root, resolved)) {
            media_err(out, cap, out_len, rsp, 403, "forbidden path");
            return IV_OK;
        }
        {
            size_t l = strlen(resolved);

            if (l + 1u > sizeof(real)) {
                media_err(out, cap, out_len, rsp, 414, "path too long");
                return IV_OK;
            }
            memcpy(real, resolved, l + 1u);
        }
    }

    if (stat(real, &st) != 0) {
        media_err(out, cap, out_len, rsp, 404, "not found");
        return IV_OK;
    }
    if (!S_ISREG(st.st_mode)) {
        media_err(out, cap, out_len, rsp, 404, "not a file");
        return IV_OK;
    }

    size = (uint64_t)st.st_size;
    make_etag(&st, etag, sizeof(etag));
    want_sha = query_has(req->query, "sha");

    /* ---- 基础额外头（任何成功响应都给） ---- */
    rsp->has_extra = 1;
    rsp->extra[0]  = '\0';
    used = extra_add(rsp->extra, sizeof(rsp->extra), used, "Accept-Ranges: bytes\r\n");
    used = extra_add(rsp->extra, sizeof(rsp->extra), used, "ETag: ");
    used = extra_add(rsp->extra, sizeof(rsp->extra), used, etag);
    used = extra_add(rsp->extra, sizeof(rsp->extra), used, "\r\n");

    /* ---- Range ---- */
    rr = parse_range(req->range, size, &off, &rlen);
    if (rr == RANGE_UNSAT) {
        char tmp[64];

        (void)snprintf(tmp, sizeof(tmp), "Content-Range: bytes */%llu\r\n",
                       (unsigned long long)size);
        used = extra_add(rsp->extra, sizeof(rsp->extra), used, tmp);
        media_err(out, cap, out_len, rsp, 416, "range not satisfiable");
        rsp->has_extra = 1;   /* media_err 只动 status/ctype/len，这里再确认一次 */
        IV_LOG_I(MEDIA_MOD, "416 %s (size=%llu)", req->path, (unsigned long long)size);
        return IV_OK;
    }
    if (rr == RANGE_OK && req->if_range[0] != '\0' && strcmp(req->if_range, etag) != 0) {
        /* If-Range 对不上（文件已被新的录像片段替换）⇒ 必须回全量，不能续传 */
        rr = RANGE_NONE;
    }
    if (rr == RANGE_OK) {
        char tmp[80];

        partial = 1;
        (void)snprintf(tmp, sizeof(tmp), "Content-Range: bytes %llu-%llu/%llu\r\n",
                       (unsigned long long)off,
                       (unsigned long long)(off + rlen - 1ull),
                       (unsigned long long)size);
        used = extra_add(rsp->extra, sizeof(rsp->extra), used, tmp);
    } else {
        off  = 0u;
        rlen = size;
    }

    /* 校验值：只有全量请求 + 显式要才算（分片无法核全量摘要）。 */
    if (want_sha && !partial) {
        uint8_t dig[IV_SHA256_DIGEST_LEN];
        char    hex[IV_SHA256_HEX_LEN + 1u];

        if (iv_sha256_file(real, dig) == IV_OK) {
            iv_sha256_hex(dig, hex, sizeof(hex));
            used = extra_add(rsp->extra, sizeof(rsp->extra), used, "X-SHA256: ");
            used = extra_add(rsp->extra, sizeof(rsp->extra), used, hex);
            used = extra_add(rsp->extra, sizeof(rsp->extra), used, "\r\n");
        }
    }
    (void)used;

    /* 路径固定下来交给 http 层 open（单线程，本函数返回前不会被插队） */
    {
        size_t l = strlen(real);

        memcpy(s_media_path, real, l + 1u);
    }

    rsp->status    = partial ? 206 : 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "%s", media_ctype(real));
    rsp->file_path = s_media_path;
    rsp->file_off  = off;
    rsp->file_len  = rlen;
    *out_len = 0u;
    rsp->len = 0u;

    IV_LOG_I(MEDIA_MOD, "%d %s off=%llu len=%llu", partial ? 206 : 200, req->path,
             (unsigned long long)off, (unsigned long long)rlen);
    return IV_OK;
}
