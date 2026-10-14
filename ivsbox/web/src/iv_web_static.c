/*
 * ivsbox-web 静态资源（M5-S5.1 前端优化点之一 / S5.3）
 *
 * 计划 S5.1 点名要改掉参考项目的做法：那边把 HTML/JS 内联进 C 数组
 * （`fsdata.c`），**改一行页面要重编整份固件**，而且页面里塞了 60+ 个单字母
 * SSI 标签。这里改成**从磁盘目录直接读文件**（`web/static/`）：
 *   - 改页面＝换文件，不动二进制；
 *   - 页面里没有任何服务端标签，数据一律由 JS 走 REST 现取。
 *
 * ============================ 安全 ============================
 * 唯一要防的是**路径穿越**（`GET /../../etc/passwd`）。做法是"字符白名单"
 * 而不是"黑名单查 `..`"：路径只允许 `[A-Za-z0-9._-/]`，`..` 自然被拦下，
 * 且不会漏掉 `%2e%2e` 之外的变体（本层不做 URL 解码，百分号本身就不在白名单里）。
 * 这是**第二道**——第一道是 static_root 本就不该包含敏感目录。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ivsbox/iv_ret.h"
#include "iv_web.h"

/* 路径字符白名单。注意空格、`%`、`\` 都不在内 —— 静态资源文件名一律用
 * 简单 ASCII，不需要给浏览器留转义口子。 */
static int path_safe(const char *p)
{
    if (p == NULL || p[0] != '/')
        return 0;
    for (; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/')
            continue;
        return 0;
    }
    return 1;
}

static const char *ctype_of(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (dot == NULL)
        return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0)
        return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".css") == 0)
        return "text/css; charset=utf-8";
    if (strcmp(dot, ".json") == 0)
        return "application/json; charset=utf-8";
    if (strcmp(dot, ".svg") == 0)
        return "image/svg+xml";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".ico") == 0)
        return "image/x-icon";
    if (strcmp(dot, ".txt") == 0)
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void set_simple(char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp,
                       int status, const char *msg)
{
    int n = snprintf(out, cap, "{\"ok\":false,\"e\":%d,\"msg\":\"%s\"}", status, msg);

    *out_len = (n < 0) ? 0u : (size_t)n;
    rsp->status = status;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len = *out_len;
}

int iv_web_static_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                         char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    char        path[IV_WEB_PATH_MAX];
    char        full[IV_WEB_PATH_MAX * 2u];
    struct stat st;
    int         fd;
    ssize_t     n;

    if (strcmp(req->method, "GET") != 0) {
        set_simple(out, cap, out_len, rsp, 405, "method not allowed");
        return IV_OK;
    }
    if (!path_safe(req->path)) {
        set_simple(out, cap, out_len, rsp, 403, "forbidden path");
        return IV_OK;
    }

    /* `/` 与任何以 `/` 结尾的路径都当目录，补 index.html */
    if (snprintf(path, sizeof(path), "%s%s", req->path,
                 (req->path[strlen(req->path) - 1u] == '/') ? "index.html" : "")
        >= (int)sizeof(path)) {
        set_simple(out, cap, out_len, rsp, 414, "path too long");
        return IV_OK;
    }

    if (snprintf(full, sizeof(full), "%.*s%.*s",
                 (int)sizeof(full) - (int)sizeof(path) - 1, ctx->static_root,
                 (int)sizeof(path) - 1, path) >= (int)sizeof(full)) {
        set_simple(out, cap, out_len, rsp, 414, "path too long");
        return IV_OK;
    }

    fd = open(full, O_RDONLY);
    if (fd < 0) {
        set_simple(out, cap, out_len, rsp, 404, "not found");
        return IV_OK;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        set_simple(out, cap, out_len, rsp, 404, "not a file");
        return IV_OK;
    }
    if ((size_t)st.st_size > (size_t)IV_WEB_STATIC_MAX || (size_t)st.st_size >= cap) {
        (void)close(fd);
        set_simple(out, cap, out_len, rsp, 413, "file too large");
        return IV_OK;
    }

    n = read(fd, out, (size_t)st.st_size);
    (void)close(fd);
    if (n < 0) {
        set_simple(out, cap, out_len, rsp, 500, "read error");
        return IV_OK;
    }

    rsp->status = 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "%s", ctype_of(path));
    *out_len = (size_t)n;
    rsp->len = (size_t)n;
    return IV_OK;
}
