/*
 * 平台触发升级的拉包（0xB3）—— `iv_updfetch` 单测  2026-11-02
 *
 * ============================ 它验的是什么 ============================
 * 用**真 socket**（127.0.0.1 上起一个假升级服务器）跑完整流程，而不是 mock
 * 传输回调：本模块最容易错的四处只有真连接才暴露 ——
 *   ① HTTP 应答读没读满（头里带回来一段 body 时必须先吐出来）；
 *   ② Content-Length 与实际字节数的对账（短包不能当成功）；
 *   ③ 上限判定是"边下边判"（不能先把 UDISK 写满再说）；
 *   ④ 下载完的文件与"服务器给的数据"是否**逐字节相等**，以及请求文件的内容。
 *
 * 假服务器按参考实现的接口形状应答（**只有 GET，没有 HEAD、没有 Range** ——
 * 2026-11-02 用户口径：升级走常规 Linux，不套参考固件那套 1026 B 帧）：
 *     GET /<硬件名>/info.txt  -> {"version":"<硬件名>-<版本>","url":"http://…/pkg/x.pkg"}
 *     GET /pkg/x.pkg          -> 200 整包
 *
 * ============================ 各组断言在钉什么 ============================
 *   p01 正常下载：2500 B 逐字节相等、`last_bytes==2500`、只发 1 个 info ＋ 1 个包请求、
 *      安装请求文件 = "<包绝对路径> 1\n"、没有残留 .part。
 *   p02 版本相同：返回 `IV_UPDFETCH_SAME`，且**一个包请求都不发**。
 *   p03 短包：Content-Length 报 2500 实际只给一半 ⇒ `IV_EPROTO`，不留 .part、不留正式包。
 *   p04 超上限：包比 max_bytes 大 ⇒ `IV_ERANGE`，且**在超限那一刻就中止**（不留半截）。
 *   p05 info.txt 异常：缺 `url` ⇒ `IV_EPROTO`；url 是 https ⇒ `IV_ENOTSUP`。
 *   p06 边角：info.txt 404 ⇒ `IV_EPROTO`；端口 1 ⇒ `IV_ECONN`；空 host / close 后 ⇒ `IV_EINVAL`。
 *   p07 目录列表模式：没有 info.txt（404）⇒ 读 autoindex，按文件名里的 M.m.p 取最大
 *       （0.9.9/1.0.1/1.0.9 ⇒ 1.0.9），`notes.txt` 与 `*.tar.gz` 跳过；
 *       顺带验编译期默认硬件名 ⇒ 请求的是 `/<默认硬件名>/`。
 *   p08 列表里的版本不比本机新 ⇒ `IV_UPDFETCH_SAME`，零包请求。
 *   p09 列表里一个 `.pkg` 都没有 ⇒ `IV_ENOENT`，什么都不拉。
 *
 * ============================ 反向证伪（改完必须能看到红） ============================
 *   F1 把 `recv_some()` 里"先吐 hdr 残留"那段删掉 ⇒ p01 逐字节比对红（数据错位）；
 *   F2 把 Content-Length 对账（`total != body_len`）注释掉 ⇒ p03 红（短包被当成功）；
 *   F3 把超限判断从循环内挪到循环后 ⇒ p04 红（半截包已经落盘）；
 *   F4 去掉 `fetch_listing()` 开头那句 `need_reconnect = 1` ⇒ p07 红（404 的 body
 *      没读走就复用连接，下一份应答的头从 body 中间开始解析）；
 *   F5 挑"第一个"而不是"最大" ⇒ p07 红（会挑到 0.9.9）；
 *   F6 去掉 `.pkg` 后缀过滤 ⇒ p07/p09 红（把 notes.txt / .tar.gz 当包）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_updfetch.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 假升级服务器
 * ------------------------------------------------------------------------- */

#define SRV_BUF_MAX (64u * 1024u)

/* 服务器行为模式（只为覆盖客户端的失败分支，不是"另一个版本的服务器"） */
#define SRV_OK 0     /* 正常：整包 200 */
#define SRV_SHORT 1  /* 报 Content-Length = len，实际只发一半就关连接 */
#define SRV_NOURL 2  /* info.txt 里没有 "url" 字段 */
#define SRV_HTTPS 3  /* info.txt 里的 url 是 https:// */
#define SRV_NOINFO 4 /* info.txt 回 404，目录也 404 */
#define SRV_DIR 5    /* info.txt 回 404，但 `/<hw>/` 能列（目录列表模式） */

typedef struct {
    int         listen_fd;
    int         port;
    pthread_t   th;
    volatile int stop;

    unsigned reqs;
    unsigned info_reqs;
    unsigned pkg_reqs;
    unsigned dir_reqs;

    int      mode;
    char     version[64];  /* info.txt 里的 version */
    char     url_path[96]; /* 包里文件的路径段（/pkg/...） */
    char     dir_html[1024];      /* SRV_DIR：`/<hw>/` 返回的 autoindex */
    char     last_dir_path[96];   /* 最近一次被当成"目录"的请求路径 */
    unsigned char buf[SRV_BUF_MAX];
    size_t   len;
} srv_t;

static int srv_listen(srv_t *s)
{
    struct sockaddr_in a;
    socklen_t          al = sizeof a;
    int                on = 1;

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0)
        return -1;
    (void)setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    if (bind(s->listen_fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(s->listen_fd, 8) != 0)
        return -1;
    if (getsockname(s->listen_fd, (struct sockaddr *)&a, &al) != 0)
        return -1;
    s->port = (int)ntohs(a.sin_port);
    return 0;
}

/* 读一个请求头；返回 0 成功，-1 对端关闭 */
static int srv_read_req(int fd, char *hdr, size_t cap)
{
    size_t total = 0;

    for (;;) {
        ssize_t n;

        if (total + 1u >= cap)
            return -1;
        n = recv(fd, hdr + total, cap - 1u - total, 0);
        if (n <= 0)
            return -1;
        total += (size_t)n;
        hdr[total] = '\0';
        if (strstr(hdr, "\r\n\r\n") != NULL)
            return 0;
    }
}

/* 请求行里的路径段：`GET /FN_RZTY_PY/ HTTP/1.1` ⇒ `/FN_RZTY_PY/`。
 * 只给目录列表模式用（判断"是不是以 `/` 结尾"），不做 URL 解码。 */
static int hdr_path(const char *hdr, char *out, size_t cap)
{
    const char *a = strchr(hdr, ' ');
    const char *b;
    size_t      n;

    if (a == NULL)
        return -1;
    a++;
    b = strchr(a, ' ');
    if (b == NULL)
        return -1;
    n = (size_t)(b - a);
    if (n == 0u || n >= cap)
        return -1;
    memcpy(out, a, n);
    out[n] = '\0';
    return 0;
}

/* `clen` 与 `blen` 分开：头里要报 Content-Length（文件大小），body 可以**少发**
 * （p03 的短包场景）。第一次写时合成一个参数，给 NULL 传长度直接崩过。 */
static void srv_send(int fd, int status, const char *extra, const void *body,
                     size_t blen, size_t clen)
{
    char   head[384];
    size_t pos = 0;
    int    n;

    n = snprintf(head, sizeof head,
                 "HTTP/1.1 %d %s\r\nContent-Length: %u\r\nConnection: keep-alive\r\n%s"
                 "\r\n",
                 status, (status == 404) ? "Not Found" : "OK", (unsigned)clen,
                 (extra != NULL) ? extra : "");
    if (n <= 0)
        return;
    pos = (size_t)n;
    if (pos > sizeof head)
        return;
    /* 头与 body **一次发完**：真实服务端几乎都是这么写的，而这正好覆盖客户端
     * "一次 recv 同时拿到头尾"的分支（应答头里已经带回一段 body 时必须先吐出来，
     * 否则数据整体错位 —— 这是本模块最容易漏的一条）。 */
    if (body != NULL && blen > 0u) {
        char *msg = (char *)malloc(pos + blen);

        if (msg != NULL) {
            memcpy(msg, head, pos);
            memcpy(msg + pos, body, blen);
            (void)send(fd, msg, pos + blen, 0);
            free(msg);
            return;
        }
        (void)send(fd, head, pos, 0);
        (void)send(fd, body, blen, 0);
        return;
    }
    (void)send(fd, head, pos, 0);
}

static void *srv_thread(void *arg)
{
    srv_t *s = arg;

    while (!s->stop) {
        struct sockaddr_in ca;
        socklen_t          cl = sizeof ca;
        int                fd = accept(s->listen_fd, (struct sockaddr *)&ca, &cl);

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (;;) {
            char hdr[2048];

            if (srv_read_req(fd, hdr, sizeof hdr) != 0)
                break;
            s->reqs++;

            if (strstr(hdr, "/info.txt") != NULL) {
                char body[256];
                int  n;

                s->info_reqs++;
                if (s->mode == SRV_DIR) {
                    /* **404 的 body 必须比头缓冲大**：真 nginx 的错误页有几百字节，
                     * 一旦"头 ＋ 一部分 body"一次 recv 没读完，剩下的字节就留在 socket
                     * 里；复用连接的下一个应答会从 body 中间开始解析。body 只有 4 字节
                     * 时它们正好被同一次 recv 一起读走，这条坑测不出来（第一版就是
                     * 这样把 F4 放过去的）。 */
                    char big[1200];

                    memset(big, 'x', sizeof big);
                    srv_send(fd, 404, "", big, sizeof big, sizeof big);
                    continue;
                }
                if (s->mode == SRV_NOINFO) {
                    srv_send(fd, 404, "", "nope", 4u, 4u);
                    continue;
                }
                if (s->mode == SRV_NOURL) {
                    n = snprintf(body, sizeof body, "{\"version\":\"%s\"}", s->version);
                } else if (s->mode == SRV_HTTPS) {
                    n = snprintf(body, sizeof body,
                                 "{\"version\":\"%s\",\"url\":\"https://%s\"}",
                                 s->version, s->url_path);
                } else {
                    n = snprintf(body, sizeof body, "{\"version\":\"%s\",\"url\":\"%s\"}",
                                 s->version, s->url_path);
                }
                if (n > 0)
                    srv_send(fd, 200, "Content-Type: application/json\r\n", body,
                             (size_t)n, (size_t)n);
                continue;
            }

            /* 目录列表（SRV_DIR）：请求行里的路径以 '/' 结尾就当目录，回 autoindex。
             * 真 nginx/busybox httpd 也是这个判据（`/x` 会 301 到 `/x/`）。
             * **只有真目录请求才记 `last_dir_path`**：包的请求也带 `/<hw>/` 前缀，
             * 拿它记会把断言骗过去（第一版就是这么红的）。 */
            if (s->mode == SRV_DIR) {
                char rp[96];

                if (hdr_path(hdr, rp, sizeof rp) == 0) {
                    size_t hl = strlen(rp);

                    if (hl > 0u && rp[hl - 1u] == '/') {
                        (void)snprintf(s->last_dir_path, sizeof s->last_dir_path, "%s", rp);
                        s->dir_reqs++;
                        srv_send(fd, 200, "Content-Type: text/html\r\n", s->dir_html,
                                 strlen(s->dir_html), strlen(s->dir_html));
                        continue;
                    }
                }
            }

            /* 包本体：只有 200 整包一种形态 */
            s->pkg_reqs++;
            if (s->mode == SRV_NOINFO) {
                srv_send(fd, 404, "", "nope", 4u, 4u); /* 连目录都不给 ⇒ 取不到任何东西 */
                continue;
            }
            if (s->mode == SRV_SHORT) {
                srv_send(fd, 200, "Content-Type: application/octet-stream\r\n", s->buf,
                         s->len / 2u, s->len); /* 少发一半 */
                break;                             /* 说完就关，模拟"短包" */
            }
            srv_send(fd, 200, "Content-Type: application/octet-stream\r\n", s->buf,
                     s->len, s->len);
        }
        (void)close(fd);
    }
    return NULL;
}

static int srv_start(srv_t *s)
{
    memset(s, 0, sizeof *s);
    s->listen_fd = -1;
    (void)snprintf(s->url_path, sizeof s->url_path, "/pkg/ivsbox.pkg");
    if (srv_listen(s) != 0) {
        if (s->listen_fd >= 0)
            (void)close(s->listen_fd);
        return -1;
    }
    if (pthread_create(&s->th, NULL, srv_thread, s) != 0) {
        (void)close(s->listen_fd);
        return -1;
    }
    return 0;
}

static void srv_stop(srv_t *s)
{
    s->stop = 1;
    (void)shutdown(s->listen_fd, SHUT_RDWR);
    (void)close(s->listen_fd);
    (void)pthread_join(s->th, NULL);
}

/* ---------------------------------------------------------------------------
 * 文件工具
 * ------------------------------------------------------------------------- */

static char g_root[128];

static size_t slurp(const char *path, unsigned char *buf, size_t cap)
{
    FILE  *f = fopen(path, "rb");
    size_t n;

    if (f == NULL)
        return (size_t)-1;
    n = fread(buf, 1u, cap, f);
    (void)fclose(f);
    return n;
}

static int exists(const char *path)
{
    return (access(path, F_OK) == 0) ? 1 : 0;
}

static void rm_root(void)
{
    char cmd[256];

    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    (void)system(cmd);
}

static void cfg_of(iv_updfetch_cfg_t *c, const srv_t *s, const char *host,
                   const char *ver)
{
    iv_updfetch_cfg_default(c);
    c->host       = host;
    c->port       = (uint16_t)s->port;
    c->hw_name    = "TESTHW";
    c->ver_str    = ver;
    c->data_root  = g_root;
    c->dest_name  = "ivsbox.pkg";
    c->max_bytes  = 1024u * 1024u;
    c->connect_ms = 2000u;
    c->send_ms    = 2000u;
    c->recv_ms    = 2000u;
}

/* 句柄约 5 KiB：**不要放栈上**（与 iv_logup 同一条纪律） */
static iv_updfetch_t g_uf;

int main(void)
{
    srv_t               srv;
    iv_updfetch_cfg_t   cfg;
    iv_updfetch_stats_t st;
    unsigned char       pkg[2500];
    unsigned char       got[SRV_BUF_MAX];
    char                pkg_path[256];
    char                part_path[256];
    char                req_path[256];
    char                req[512];
    FILE               *f;
    size_t              n;
    unsigned            before_pkg;
    int                 rc;
    unsigned            i;

    (void)signal(SIGPIPE, SIG_IGN); /* 客户端提前关连接时别把测试进程打死 */

    (void)snprintf(g_root, sizeof g_root, "/tmp/ivupdfetch_%d", (int)getpid());
    rm_root();
    if (mkdir(g_root, 0755) != 0) {
        fprintf(stderr, "SKIP: cannot create %s\n", g_root);
        return 0; /* 没权限造临时目录不算失败（沙箱环境） */
    }
    if (srv_start(&srv) != 0) {
        fprintf(stderr, "SKIP: cannot listen on loopback\n");
        rm_root();
        return 0;
    }

    (void)snprintf(pkg_path, sizeof pkg_path, "%s/ota/download/ivsbox.pkg", g_root);
    (void)snprintf(part_path, sizeof part_path, "%s/ota/download/ivsbox.pkg.part", g_root);
    (void)snprintf(req_path, sizeof req_path, "%s/ota/request", g_root);

    for (i = 0u; i < sizeof pkg; i++)
        pkg[i] = (unsigned char)((i * 7u + 3u) & 0xffu);
    memcpy(srv.buf, pkg, sizeof pkg);
    srv.len = sizeof pkg;
    (void)snprintf(srv.version, sizeof srv.version, "TESTHW-9.9.9");

    /* ---- p01：正常整包下载，逐字节相等 ---- */
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p01 init");
    rc = iv_updfetch_run(&g_uf);
    if (rc != IV_OK) { /* 失败时把 rc/http 打出来：只报"不等于 IV_OK"没法定位 */
        iv_updfetch_stats(&g_uf, &st);
        fprintf(stderr, "  p01 rc=%d http=%d\n", rc, st.last_http);
    }
    chk(rc == IV_OK, "p01 run rc == IV_OK");
    n = slurp(pkg_path, got, sizeof got);
    chk(n == sizeof pkg, "p01 downloaded size == 2500");
    chk(n == sizeof pkg && memcmp(got, pkg, sizeof pkg) == 0,
        "p01 downloaded bytes identical");
    chk(exists(part_path) == 0, "p01 no .part left after rename");
    chk(srv.info_reqs == 1u && srv.pkg_reqs == 1u,
        "p01 exactly 1 info + 1 package request (no HEAD, no Range)");
    iv_updfetch_stats(&g_uf, &st);
    chk(st.last_bytes == (uint32_t)sizeof pkg, "p01 stats.last_bytes == 2500");
    chk(st.ok == 1u && st.fail == 0u, "p01 stats ok=1 fail=0");

    f = fopen(req_path, "r");
    chk(f != NULL, "p01 install request written");
    if (f != NULL) {
        req[0] = '\0';
        (void)fgets(req, sizeof req, f);
        (void)fclose(f);
        {
            char want[512];

            (void)snprintf(want, sizeof want, "%s 1\n", pkg_path);
            chk(strcmp(req, want) == 0, "p01 request file == \"<pkg> 1\\n\"");
        }
    }
    iv_updfetch_close(&g_uf);

    /* ---- p02：版本相同 ⇒ 一个包请求都不发 ---- */
    (void)snprintf(srv.version, sizeof srv.version, "TESTHW-1.0.0.20261102");
    before_pkg = srv.pkg_reqs;
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p02 init");
    rc = iv_updfetch_run(&g_uf);
    chk(rc == IV_UPDFETCH_SAME, "p02 same version -> IV_UPDFETCH_SAME");
    chk(srv.pkg_reqs == before_pkg, "p02 no package request for an up-to-date device");
    iv_updfetch_stats(&g_uf, &st);
    chk(st.same == 1u, "p02 stats.same == 1");
    iv_updfetch_close(&g_uf);

    /* ---- p03：短包（Content-Length 报 2500，实际只给一半）⇒ EPROTO ---- */
    (void)snprintf(srv.version, sizeof srv.version, "TESTHW-9.9.9");
    srv.mode = SRV_SHORT;
    (void)unlink(pkg_path);
    (void)unlink(part_path);
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p03 init");
    rc = iv_updfetch_run(&g_uf);
    /* 短包在传输层就是"连接提前关"，报 IV_ECONN；关键是**不能当成功**、
     * 也不能留下半截包。两种错误码都接受，但 IV_OK 绝对不行。 */
    chk(rc != IV_OK && rc != IV_UPDFETCH_SAME, "p03 short body is not success");
    chk(exists(part_path) == 0, "p03 no .part left after failure");
    chk(exists(pkg_path) == 0, "p03 no final package after failure");
    iv_updfetch_close(&g_uf);
    srv.mode = SRV_OK;

    /* ---- p04：超过 max_bytes ⇒ ERANGE，且超限那一刻就中止 ---- */
    srv.len = 40000u; /* 服务器给 40000 B */
    (void)unlink(part_path);
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    cfg.max_bytes = 2000u; /* 客户端只允许 2 KB */
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p04 init");
    rc = iv_updfetch_run(&g_uf);
    chk(rc == IV_ERANGE, "p04 oversize -> IV_ERANGE");
    chk(exists(part_path) == 0, "p04 no .part left after ERANGE");
    chk(exists(pkg_path) == 0, "p04 no final package after ERANGE");
    iv_updfetch_close(&g_uf);
    srv.len = sizeof pkg;

    /* ---- p05：info.txt 异常（缺 url / https） ---- */
    srv.mode = SRV_NOURL;
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p05 init (no url)");
    chk(iv_updfetch_run(&g_uf) == IV_EPROTO, "p05 info.txt without url -> IV_EPROTO");
    iv_updfetch_close(&g_uf);

    srv.mode = SRV_HTTPS;
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p05 init (https)");
    chk(iv_updfetch_run(&g_uf) == IV_ENOTSUP, "p05 https url -> IV_ENOTSUP");
    iv_updfetch_close(&g_uf);
    srv.mode = SRV_OK;

    /* ---- p07：目录列表模式（没有 info.txt，目录里有多个包） ----
     * 发布侧只放一个包 ⇒ info.txt 404 ⇒ 退回读 autoindex，按**文件名里的 M.m.p**
     * 取最大：0.9.9 / 1.0.1 / 1.0.9 里选 1.0.9；`notes.txt` 与 `2.0.0.tar.gz` 必须跳过。 */
    srv.mode = SRV_DIR;
    (void)snprintf(srv.dir_html, sizeof srv.dir_html,
                   "<html><body><pre>"
                   "<a href=\"../\">../</a>\n"
                   "<a href=\"FN_RZTY_PY-0.9.9.pkg\">FN_RZTY_PY-0.9.9.pkg</a>\n"
                   "<a href=\"notes.txt\">notes.txt</a>\n"
                   "<a href=\"FN_RZTY_PY-1.0.1.20261103.pkg\">"
                   "FN_RZTY_PY-1.0.1.20261103.pkg</a>\n"
                   "<a href=\"FN_RZTY_PY-2.0.0.tar.gz\">FN_RZTY_PY-2.0.0.tar.gz</a>\n"
                   "<a href=\"FN_RZTY_PY-1.0.9.pkg\">FN_RZTY_PY-1.0.9.pkg</a>\n"
                   "</pre></body></html>");
    srv.pkg_reqs = 0u;
    srv.dir_reqs = 0u;
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    cfg.hw_name = NULL; /* **不设硬件名** ⇒ 顺带验编译期默认值（发布侧那个目录名） */
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p07 init (listing)");
    rc = iv_updfetch_run(&g_uf);
    if (rc != IV_OK) {
        iv_updfetch_stats(&g_uf, &st);
        fprintf(stderr, "  p07 rc=%d http=%d\n", rc, st.last_http);
    }
    chk(rc == IV_OK, "p07 listing -> IV_OK");
    iv_updfetch_stats(&g_uf, &st);
    chk(st.listing == 1u, "p07 counted as a listing run");
    chk(strcmp(st.info_version, "1.0.9") == 0, "p07 picked the max version 1.0.9");
    chk(strcmp(st.file, "FN_RZTY_PY-1.0.9.pkg") == 0, "p07 file name came from listing");
    chk(srv.dir_reqs == 1u && srv.pkg_reqs == 1u, "p07 1 dir + 1 package request");
    chk(strcmp(srv.last_dir_path, "/FN_RZTY_PY/") == 0,
        "p07 default hw name lists /FN_RZTY_PY/");
    {
        char lp[256];

        (void)snprintf(lp, sizeof lp, "%s/ota/download/FN_RZTY_PY-1.0.9.pkg", g_root);
        n = slurp(lp, got, sizeof got);
        chk(n == sizeof pkg, "p07 downloaded size == 2500");
        chk(n == sizeof pkg && memcmp(got, pkg, sizeof pkg) == 0,
            "p07 downloaded bytes identical");
    }
    iv_updfetch_close(&g_uf);

    /* ---- p08：列表里的版本不比本机新 ⇒ SAME，且**一个包请求都不发** ---- */
    (void)snprintf(srv.dir_html, sizeof srv.dir_html,
                   "<a href=\"FN_RZTY_PY-1.0.0.pkg\">FN_RZTY_PY-1.0.0.pkg</a>");
    before_pkg = srv.pkg_reqs;
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    cfg.hw_name = NULL;
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p08 init (listing, up to date)");
    chk(iv_updfetch_run(&g_uf) == IV_UPDFETCH_SAME, "p08 listing same version -> SAME");
    chk(srv.pkg_reqs == before_pkg, "p08 no package request when up to date");
    iv_updfetch_close(&g_uf);

    /* ---- p09：列表里没有 `.pkg`（只有别的文件）⇒ ENOENT，别把错的东西拉下来 ---- */
    (void)snprintf(srv.dir_html, sizeof srv.dir_html,
                   "<a href=\"../\">../</a><a href=\"notes.txt\">notes.txt</a>"
                   "<a href=\"FN_RZTY_PY-2.0.0.tar.gz\">FN_RZTY_PY-2.0.0.tar.gz</a>");
    before_pkg = srv.pkg_reqs;
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    cfg.hw_name = NULL;
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p09 init (no pkg in listing)");
    chk(iv_updfetch_run(&g_uf) == IV_ENOENT, "p09 listing without .pkg -> IV_ENOENT");
    chk(srv.pkg_reqs == before_pkg, "p09 nothing downloaded");
    iv_updfetch_close(&g_uf);
    srv.mode = SRV_OK;

    /* ---- p06：边角（404 / 不可达 / 空 host / close 之后） ---- */
    srv.mode = SRV_NOINFO;
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p06 init (404)");
    chk(iv_updfetch_run(&g_uf) == IV_EPROTO, "p06 info.txt 404 -> IV_EPROTO");
    iv_updfetch_close(&g_uf);
    srv.mode = SRV_OK;

    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    cfg.port       = 1u; /* 这个端口上没有服务 */
    cfg.connect_ms = 300u;
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p06 init (bad port)");
    chk(iv_updfetch_run(&g_uf) == IV_ECONN, "p06 unreachable server -> IV_ECONN");
    iv_updfetch_close(&g_uf);

    cfg_of(&cfg, &srv, "", "1.0.0.20261102");
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p06 init with empty host allowed");
    chk(iv_updfetch_run(&g_uf) == IV_EINVAL, "p06 empty host -> IV_EINVAL");
    iv_updfetch_close(&g_uf);
    chk(iv_updfetch_run(&g_uf) == IV_EINVAL, "p06 run after close -> IV_EINVAL");

    srv_stop(&srv);
    rm_root();

    if (g_fail != 0) {
        fprintf(stderr, "updfetch: %d checks FAILED\n", g_fail);
        return 1;
    }
    printf("updfetch: all checks passed\n");
    return 0;
}
