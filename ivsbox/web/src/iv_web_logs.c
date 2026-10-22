/*
 * ivsbox-web 系统日志浏览 / 下载（2026-10-22，对齐单片机参考 web 的「打印日志」）
 *
 * ============================ 板端日志落在哪 ============================
 * `iv_log` 按日期建目录、按小时切文件：`/mnt/UDISK/log/<YYYY-MM-DD>/<HH>.log`。
 * 本层只有两条路由：
 *   GET /api/logs?max=N        → 列文件（默认 20 个，最新在前）
 *   GET /api/logs/file?f=...   → 下载单个文件（走 `rsp->file_path` 流式，不进 wbuf）
 *
 * ============================ 为什么不经过 ctl ============================
 * 日志是 ivsbox-web 自己**能直接读的普通文件**，不是 ivsboxd 的内部状态。为"列个
 * 目录"走一趟本地通道，只会把 `IV_CTL_RSP_MAX`（32 KiB）套在列表上、并让日志格式
 * 变成一份协议——两边都得跟着改。与 media 的分工同理：`/media/` 管**录像快照**
 * （在 SD 上、要限速），本层管**文本日志**（在 UDISK 上、体量小）。
 *
 * ============================ 路径围栏（三道，与 iv_web_media.c 同一口径） ============================
 * 1. **相对路径按格式白名单**：只认 `<10 位日期>/<1..2 位数字>.log`——不是"过滤掉
 *    危险字符"，而是"不匹配就拒"。`..` 这类串在这里根本拼不出合法形状。
 * 2. **realpath 复核前缀**：解析符号链接/`.`/重复 `/` 后，结果必须仍在根内
 *    （前缀相等且后随 `/` 或串尾）。
 * 3. **`open(O_NOFOLLOW)` + `fstat`**：真正打开时(http 层)不跟随符号链接，且必须是
 *    普通文件。
 *
 * **鉴权一道都不能省**：日志里有内网地址、平台地址、设备串号。故与 `/api/` 主路径
 * 同口径——必须有效会话；出厂默认口令期（`must_change`）连日志也不给下载。
 *
 * ============================ 一连接一请求的静态缓冲 ============================
 * `s_log_path` 是**静态**的：本函数返回后 http 层才 `open()` 它。安全的前提是事件
 * 循环单线程、一次只处理一条请求——本函数返回前插不进第二个请求。
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "iv_web.h"

#define LOGS_MOD     "web.logs"
#define LOGS_PREFIX  "/api/logs"
#define LOGS_PFX_LEN 9u

/* 一次扫描最多收多少个候选文件（够翻两天的量；再多的旧日志本来就该清理）。 */
#define LOGS_ENTRY_MAX 64u
/* 相对名最长 `YYYY-MM-DD/HH.log` = 10 + 1 + 6 = 17，取 24 留余量。 */
#define LOGS_NAME_MAX  24u

/* 一次列表最多回多少个（`?max=` 的上限，防止把整个目录都吐给浏览器） */
#define LOGS_SHOW_DEF 20u
#define LOGS_SHOW_MAX 100u

typedef struct {
    char     f[LOGS_NAME_MAX]; /* 相对名，形如 `2026-10-22/08.log` */
    uint64_t size;
    int64_t  mtime;
} log_ent_t;

static char s_log_path[IV_WEB_LOG_ROOT_MAX + IV_WEB_PATH_MAX + 2u];

/* --------------------------------------------------------------------------
 * 小工具
 * -------------------------------------------------------------------------- */

/* 错误 JSON：外形与 iv_ctl / api.c 完全一致（前端一套解析逻辑通吃）。 */
static int logs_err(char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp,
                    int status, int code, const char *msg)
{
    int n = snprintf(out, cap, "{\"ok\":false,\"e\":%d,\"msg\":\"%s\"}", code, msg);

    rsp->status = status;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len = (n < 0) ? 0u : (((size_t)n >= cap) ? (cap - 1u) : (size_t)n);
    *out_len = rsp->len;
    return IV_OK;
}

/* 拼 `dir/name`；放不下返回 -1（不截断——截断会拼出另一个路径）。 */
static int join2(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);

    if (dl + 1u + nl + 1u > cap)
        return -1;
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, name, nl);
    out[dl + 1u + nl] = '\0';
    return 0;
}

/* `p` 是否在 `root` 之内（root 已 realpath；成段比较，`/logfoo` 不算在 `/log` 内） */
static int within_root(const char *root, const char *p)
{
    size_t rl = strlen(root);

    if (rl == 0u || strncmp(root, p, rl) != 0)
        return 0;
    return (p[rl] == '\0' || p[rl] == '/') ? 1 : 0;
}

/* 相对名白名单：**只认** `YYYY-MM-DD/HH.log`（小时 1~2 位）。
 * 注意这是"正向形状匹配"，不是"过滤危险字符"——`../x` 拼不出这个形状。 */
static int rel_ok(const char *s)
{
    size_t n = strlen(s);
    size_t hlen;
    size_t i;

    if (s == NULL)
        return 0;
    /* 10(日期) + 1('/') + hlen + 4(".log")，hlen ∈ [1,2] ⇒ 总长 16 或 17 */
    if (n != 16u && n != 17u)
        return 0;
    hlen = n - 15u;
    if (s[10] != '/')
        return 0;
    for (i = 0; i < 10u; i++) {
        if (i == 4u || i == 7u) {
            if (s[i] != '-')
                return 0;
        } else if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    for (i = 0; i < hlen; i++)
        if (s[11u + i] < '0' || s[11u + i] > '9')
            return 0;
    return (memcmp(s + 11u + hlen, ".log", 4u) == 0) ? 1 : 0;
}

/* 目录名白名单：10 位 `YYYY-MM-DD`（`iv_log` 的建目录规则） */
static int day_ok(const char *s)
{
    size_t i;

    if (s == NULL || strlen(s) != 10u)
        return 0;
    for (i = 0; i < 10u; i++) {
        if (i == 4u || i == 7u) {
            if (s[i] != '-')
                return 0;
        } else if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return 1;
}

/* 小时文件名白名单：`<1..2 位数字>.log` */
static int hour_ok(const char *s)
{
    size_t n = (s != NULL) ? strlen(s) : 0u;
    size_t hlen = (n >= 5u) ? (n - 4u) : 0u;
    size_t i;

    if (hlen < 1u || hlen > 2u)
        return 0;
    for (i = 0; i < hlen; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return (memcmp(s + hlen, ".log", 4u) == 0) ? 1 : 0;
}

/* 有序插入（按 mtime 降序）。满了只挤掉最旧的一条——**留最新**才是用户要看的。 */
static void ent_push(log_ent_t *v, unsigned *n, const log_ent_t *e)
{
    unsigned i;

    if (*n < LOGS_ENTRY_MAX) {
        i = *n;
        (*n)++;
    } else {
        if (e->mtime <= v[LOGS_ENTRY_MAX - 1u].mtime)
            return;
        i = LOGS_ENTRY_MAX - 1u;
    }
    while (i > 0u && v[i - 1u].mtime < e->mtime) {
        v[i] = v[i - 1u];
        i--;
    }
    v[i] = *e;
}

/* 扫根下所有 `YYYY-MM-DD/<HH>.log`；返回收集到的条数。目录不存在 ⇒ 0（不是错）。 */
static unsigned log_scan(const char *root, log_ent_t *v)
{
    DIR           *top;
    struct dirent *de;
    unsigned       n = 0u;

    top = opendir(root);
    if (top == NULL)
        return 0u;

    while ((de = readdir(top)) != NULL) {
        char            daypath[IV_WEB_LOG_ROOT_MAX + 32u];
        DIR            *sub;
        struct dirent  *se;
        struct stat     dst;

        if (!day_ok(de->d_name))
            continue;
        if (join2(daypath, sizeof(daypath), root, de->d_name) != 0)
            continue;
        if (stat(daypath, &dst) != 0 || !S_ISDIR(dst.st_mode))
            continue;
        sub = opendir(daypath);
        if (sub == NULL)
            continue;
        while ((se = readdir(sub)) != NULL) {
            char        fpath[IV_WEB_LOG_ROOT_MAX + 64u];
            struct stat st;
            log_ent_t   e;

            if (!hour_ok(se->d_name))
                continue;
            /* 目录与文件名都过了形状白名单，拼出来的路径必然在根内 */
            if (join2(fpath, sizeof(fpath), daypath, se->d_name) != 0)
                continue;
            if (stat(fpath, &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            memset(&e, 0, sizeof(e));
            if (join2(e.f, sizeof(e.f), de->d_name, se->d_name) != 0)
                continue;
            e.size  = (uint64_t)st.st_size;
            e.mtime = (int64_t)st.st_mtime;
            ent_push(v, &n, &e);
        }
        (void)closedir(sub);
    }
    (void)closedir(top);
    return n;
}

/* --------------------------------------------------------------------------
 * 路由：列表
 * -------------------------------------------------------------------------- */
static int logs_list(const iv_web_ctx_t *ctx, const iv_web_req_t *req,
                     char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    log_ent_t      ent[LOGS_ENTRY_MAX];
    unsigned       n;
    unsigned       shown;
    unsigned       i;
    const char    *rootsrc;
    char           root[PATH_MAX];
    char           maxs[16];
    unsigned       max = LOGS_SHOW_DEF;
    size_t         used;
    int            w;

    if (iv_web_query_get(req->query, "max", maxs, sizeof(maxs))) {
        long v = strtol(maxs, NULL, 10);

        if (v > 0 && v <= (long)LOGS_SHOW_MAX)
            max = (unsigned)v;
    }

    rootsrc = (ctx->log_root[0] != '\0') ? ctx->log_root : IV_WEB_LOG_ROOT_DEFAULT;
    if (realpath(rootsrc, root) == NULL) {
        /* 日志根不在（UDISK 没挂上）⇒ 回空列表而不是报错：页面照常渲染，
         * 只是没东西可下。**不要**在这里造一个假条目。 */
        *out_len = (size_t)snprintf(out, cap,
                                    "{\"ok\":true,\"d\":{\"root\":\"%s\",\"files\":[],"
                                    "\"total\":0,\"shown\":0}}",
                                    rootsrc);
        rsp->status = 200;
        (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
        rsp->len = *out_len;
        return IV_OK;
    }

    n = log_scan(root, ent);
    shown = (n < max) ? n : max;

    used = (size_t)snprintf(out, cap,
                            "{\"ok\":true,\"d\":{\"root\":\"%s\",\"files\":[", root);
    if (used >= cap) {
        *out_len = 0u;
        return IV_ERANGE;
    }
    for (i = 0; i < shown; i++) {
        w = snprintf(out + used, cap - used,
                     "%s{\"f\":\"%s\",\"size\":%llu,\"mtime\":%lld}", (i > 0u) ? "," : "",
                     ent[i].f, (unsigned long long)ent[i].size, (long long)ent[i].mtime);
        if (w < 0 || (size_t)w >= cap - used) {
            *out_len = 0u;
            return IV_ERANGE;
        }
        used += (size_t)w;
    }
    w = snprintf(out + used, cap - used, "],\"total\":%u,\"shown\":%u}}", n, shown);
    if (w < 0 || (size_t)w >= cap - used) {
        *out_len = 0u;
        return IV_ERANGE;
    }
    used += (size_t)w;

    *out_len = used;
    rsp->status = 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len = used;
    return IV_OK;
}

/* 单字符十六进制值；不是十六进制返回 -1 */
static int hexv(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* `%XX` 解码（就地，只认两位十六进制）。
 *
 * **为什么必须做**：`f` 的取值形如 `2026-10-22/08.log`，前端用 `encodeURIComponent`
 * 拼链接时会把 `/` 编成 `%2F`（不编的话浏览器/中间层可能把它当路径段截掉）。而
 * `iv_web_query_get()` 是**不解码**的（见其注释：本工程其它参数都是短标识符）。
 * 于是这里不还原 `%2F`，`rel_ok()` 就会把一条完全合法的请求判成 403。
 *
 * **安全性**：解码只是"先还原、再白名单"。`rel_ok()` 是正向形状匹配（只能是
 * `YYYY-MM-DD/HH.log`），所以解码出 `../` 也一样过不了 —— 不存在"解码绕过"。 */
static void pct_decode(char *s)
{
    char *w = s;
    char *r = s;

    while (*r != '\0') {
        if (r[0] == '%' && r[1] != '\0' && r[2] != '\0') {
            int hi = hexv(r[1]);
            int lo = hexv(r[2]);

            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* --------------------------------------------------------------------------
 * 路由：下载单个文件
 * -------------------------------------------------------------------------- */
static int logs_file(const iv_web_ctx_t *ctx, const iv_web_req_t *req,
                     char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    const char    *rootsrc;
    char           root[PATH_MAX];
    char           rel[LOGS_NAME_MAX];
    char           joined[IV_WEB_LOG_ROOT_MAX + LOGS_NAME_MAX + 2u];
    char           resolved[PATH_MAX];
    char           disp[LOGS_NAME_MAX];
    struct stat    st;
    size_t         i;
    int            w;

    if (!iv_web_query_get(req->query, "f", rel, sizeof(rel)))
        return logs_err(out, cap, out_len, rsp, 403, IV_EAUTH, "forbidden path");
    /* 先还原 `%2F`，再按形状白名单判 —— 顺序不能颠倒（见 pct_decode 注释） */
    pct_decode(rel);
    if (!rel_ok(rel))
        return logs_err(out, cap, out_len, rsp, 403, IV_EAUTH, "forbidden path");

    rootsrc = (ctx->log_root[0] != '\0') ? ctx->log_root : IV_WEB_LOG_ROOT_DEFAULT;
    if (realpath(rootsrc, root) == NULL)
        return logs_err(out, cap, out_len, rsp, 404, IV_ENOENT, "log root not available");
    if (join2(joined, sizeof(joined), root, rel) != 0)
        return logs_err(out, cap, out_len, rsp, 414, IV_ERANGE, "path too long");

    /* 第 2 道围栏：realpath 把符号链接/`.`/重复 `/` 全解掉，再复核仍在根内 */
    if (realpath(joined, resolved) == NULL)
        return logs_err(out, cap, out_len, rsp, 404, IV_ENOENT, "not found");
    if (!within_root(root, resolved))
        return logs_err(out, cap, out_len, rsp, 403, IV_EAUTH, "forbidden path");
    if (stat(resolved, &st) != 0)
        return logs_err(out, cap, out_len, rsp, 404, IV_ENOENT, "not found");
    if (!S_ISREG(st.st_mode))
        return logs_err(out, cap, out_len, rsp, 404, IV_ENOENT, "not a file");
    if (strlen(resolved) + 1u > sizeof(s_log_path))
        return logs_err(out, cap, out_len, rsp, 414, IV_ERANGE, "path too long");

    /* 手写 memcpy 而不是 snprintf：`resolved` 是 PATH_MAX 缓冲、`s_log_path` 只有
     * 514 字节，`%s` 会被 -Wformat-truncation 判为可能截断（-Werror 直接断编译）。
     * 长度已在上一条语句里判死，所以这里的拷贝**不会**截断。 */
    {
        size_t rl = strlen(resolved);

        memcpy(s_log_path, resolved, rl + 1u);
    }

    /* 下载名把 `/` 换成 `_`，浏览器才不会把日期当目录截掉 */
    for (i = 0; rel[i] != '\0' && i + 1u < sizeof(disp); i++)
        disp[i] = (rel[i] == '/') ? '_' : rel[i];
    disp[i] = '\0';

    rsp->has_extra = 1;
    w = snprintf(rsp->extra, sizeof(rsp->extra),
                 "Content-Disposition: attachment; filename=\"%s\"\r\n", disp);
    if (w < 0 || (size_t)w >= sizeof(rsp->extra))
        rsp->has_extra = 0;

    rsp->file_path = s_log_path;
    rsp->file_off  = 0u;
    rsp->file_len  = (uint64_t)st.st_size;
    rsp->status    = 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "text/plain; charset=utf-8");
    rsp->len  = 0u;
    *out_len  = 0u;
    IV_LOG_I(LOGS_MOD, "download %s (%llu B)", rel, (unsigned long long)st.st_size);
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 入口
 * -------------------------------------------------------------------------- */
int iv_web_logs_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                       char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    if (strncmp(req->path, LOGS_PREFIX, LOGS_PFX_LEN) != 0)
        return IV_ENOTSUP;
    /* `/api/logsX` 不是本层路径（前缀必须成段） */
    if (req->path[LOGS_PFX_LEN] != '\0' && req->path[LOGS_PFX_LEN] != '/')
        return IV_ENOTSUP;

    if (strcmp(req->method, "GET") != 0)
        return logs_err(out, cap, out_len, rsp, 405, IV_EINVAL, "method not allowed");
    if (!iv_web_sess_check(ctx, req->token))
        return logs_err(out, cap, out_len, rsp, 401, IV_EAUTH, "unauthorized");
    /* 出厂默认口令期连日志也不给 —— 日志里有内网地址与设备串号，
     * 拿出厂口令的人不该能把它们拉走（与 /api/、/media/ 同名闸门同一口径）。 */
    if (ctx->must_change != 0)
        return logs_err(out, cap, out_len, rsp, 403, IV_ESTATE, "must change password");

    if (req->path[LOGS_PFX_LEN] == '\0')
        return logs_list(ctx, req, out, cap, out_len, rsp);
    if (strcmp(req->path, LOGS_PREFIX "/file") == 0)
        return logs_file(ctx, req, out, cap, out_len, rsp);
    return logs_err(out, cap, out_len, rsp, 404, IV_ENOENT, "not found");
}
