/*
 * ivsbox-web 摄像机受控代理端到端 —— 集成测试（功能开发计划 M8-S8.5 验证项）
 *
 * ============================ 为什么必须是集成测试 ============================
 * S8.5 的风险全在**接线**上（教训 1：接线在装配层，单测测不到）：
 *   - 专用监听口真的只绑 127.0.0.1，且 accept 出来的连接真的带着 is_cam 标记；
 *   - is_cam 连接在 handle_req 里真的进了 cam 路由（而不是掉进 static 的 404）；
 *   - cam_ip_of 经本地通道问 ivsboxd 的整条链（chan 帧 → config.get → "value"）；
 *   - 目标 IP/端口/URL/Cookie 的改写发生在真实字节流上。
 *
 * ============================ 结构（自足，不需要 ns/tmpfs） ============================
 *   [本测试进程]
 *     ├─ fork → 假 ivsboxd：unix socket（mkdtemp 目录下）上用 libivcore 的 chan 帧
 *     │        应答 config.get camera.ch.<N>.ip（按测试表给值）
 *     ├─ fork → 假摄像机：127.0.0.1:<随机口> 的最小 HTTP 服务（echo/302/Set-Cookie）
 *     ├─ fork → ivsbox-web #1：--port MP --cam-port CP --cam-upstream <摄像机口>
 *     └─ fork → ivsbox-web #2（护栏专测）：--cam-port CP2 --cam-upstream CP2
 *              （上游=监听口 ⇒ 环回护栏必然触发）
 *
 * 只 kill 自己拉起的子进程；临时目录收尾删除（AGENTS.md 规则 6）。
 * 用法：test_web_cam_e2e [ivsbox-web 二进制路径]（缺省 IV_WEB_BIN 或 build/host/ivsbox-web；
 * 二进制缺失报 SKIP 退出 0）。
 */
#define _GNU_SOURCE 1 /* strcasestr（glibc 扩展；本工程测试代码允许） */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_ret.h"

#define TIMEOUT_MS    5000
#define CONNECT_RETRY 5000
#define CONNECT_STEP  100
#define RSP_CAP       (256u * 1024u)

static int   g_fail;
static pid_t g_kid_ivsboxd = -1;
static pid_t g_kid_camera  = -1;
static pid_t g_kid_web1    = -1;
static pid_t g_kid_web2    = -1;
static char  g_rsp[RSP_CAP];
static char  g_req[4096];
static char  g_dir[256];      /* mkdtemp 结果 */
static char  g_chan[300];     /* unix socket 路径 */
static int   g_cam_port;      /* 假摄像机端口（= web#1 的 --cam-upstream） */
static int   g_web_port;      /* web#1 主口 */
static int   g_cam_lport;     /* web#1 专用口 */
static int   g_web2_port;     /* web#2 主口 */
static int   g_cam2_lport;    /* web#2 专用口（= 其上游口，触发护栏） */

/* 假 ivsboxd 的配置表（下标 = 路号 N） */
static const char *g_ip_tab[8] = {
    "127.0.0.1", /* ch0：假摄像机在线 */
    "",          /* ch1：未配置 ⇒ 502 not configured */
    "127.0.0.2", /* ch2：本机回环另一地址，无监听 ⇒ 502 unreachable */
    "127.0.0.1", /* ch3：配合 web#2 触发环回护栏 */
    "", "", "", "",
};

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/* --------------------------------------------------------------------------
 * 裸 socket 客户端（与 test_web_media_e2e 同一套）
 * -------------------------------------------------------------------------- */
static int find_free_port(void)
{
    struct sockaddr_in a;
    socklen_t          alen = sizeof(a);
    int                fd = socket(AF_INET, SOCK_STREAM, 0);
    int                port = -1;

    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &alen) == 0)
        port = (int)ntohs(a.sin_port);
    (void)close(fd);
    return port;
}

static int connect_retry(int port)
{
    int waited = 0;

    while (waited <= CONNECT_RETRY) {
        struct sockaddr_in a;
        int                fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd >= 0) {
            memset(&a, 0, sizeof(a));
            a.sin_family      = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port        = htons((uint16_t)port);
            if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
                return fd;
            (void)close(fd);
        }
        sleep_ms(CONNECT_STEP);
        waited += CONNECT_STEP;
    }
    return -1;
}

/* 发请求收全响应；返回字节数（<0 失败），响应在 g_rsp。 */
static int exchange(int port, const char *req, size_t len)
{
    int    fd = connect_retry(port);
    size_t sent = 0;
    size_t total = 0;

    if (fd < 0)
        return -1;
    {
        struct timeval tv;

        tv.tv_sec  = TIMEOUT_MS / 1000;
        tv.tv_usec = (TIMEOUT_MS % 1000) * 1000;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    while (sent < len) {
        ssize_t n = send(fd, req + sent, len - sent, MSG_NOSIGNAL);

        if (n <= 0) {
            (void)close(fd);
            return -1;
        }
        sent += (size_t)n;
    }
    for (;;) {
        ssize_t n;

        if (total >= RSP_CAP)
            break;
        n = recv(fd, g_rsp + total, RSP_CAP - total, 0);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        total += (size_t)n;
    }
    (void)close(fd);
    g_rsp[total < RSP_CAP ? total : RSP_CAP - 1u] = '\0';
    return (int)total;
}

static int rsp_status(void)
{
    const char *sp;

    if (memcmp(g_rsp, "HTTP/", 5u) != 0)
        return -1;
    sp = memchr(g_rsp, ' ', 16u);
    return (sp != NULL) ? atoi(sp + 1) : -1;
}

static long hdr_end_off(size_t total)
{
    size_t i;

    for (i = 0; i + 3u < total; i++) {
        if (memcmp(g_rsp + i, "\r\n\r\n", 4u) == 0)
            return (long)(i + 4u);
    }
    return -1;
}

static int hdr_val(size_t total, const char *name, char *out, size_t cap)
{
    long   off = hdr_end_off(total);
    size_t nlen = strlen(name);
    size_t i;

    if (off <= 0)
        return 0;
    for (i = 0; i + nlen < (size_t)off; i++) {
        if ((i == 0u || g_rsp[i - 1u] == '\n') &&
            strncasecmp(g_rsp + i, name, nlen) == 0) {
            const char *v = g_rsp + i + nlen;
            size_t      j = 0;

            while (*v == ' ' || *v == '\t')
                v++;
            while (v[j] != '\r' && v[j] != '\n' && v[j] != '\0' && j + 1u < cap) {
                out[j] = v[j];
                j++;
            }
            out[j] = '\0';
            return 1;
        }
    }
    return 0;
}

static int count_fds(pid_t pid)
{
    char           path[64];
    DIR           *d;
    struct dirent *e;
    int            n = 0;

    (void)snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
    d = opendir(path);
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.')
            n++;
    }
    (void)closedir(d);
    return n;
}

/* --------------------------------------------------------------------------
 * 假 ivsboxd：chan 帧应答 config.get camera.ch.<N>.ip
 * -------------------------------------------------------------------------- */
static int fake_ivsboxd_serve_conn(int cfd)
{
    static unsigned char rx[IV_CHAN_RECV_CAP_MIN];
    ivs_chan_hdr_t       hdr;
    size_t               plen = 0;
    int                  rc;

    for (;;) {
        int tries;

        for (tries = 0; tries < 3; tries++) {
            rc = iv_chan_recv_timeout(cfd, rx, sizeof rx, &hdr, &plen, 1000);
            if (rc != IV_EAGAIN)
                break;
        }
        if (rc != IV_OK)
            return (rc == IV_ETIMEDOUT) ? 0 : -1; /* 空闲超时＝这条连接没事了 */
        if (hdr.type != IV_CHAN_TYPE_REQ || plen == 0u)
            continue;

        {
            const char *req = (const char *)rx + IV_CHAN_HDR_SIZE;
            char        rsp[256];
            const char *val = NULL;
            unsigned    n = 9u; /* 表外值：一律未配置 */
            int         w;

            /* 只认 camera.ch.<N>.ip（N 0..7）；其余键一律 ok:false */
            if (strstr(req, "camera.ch.") != NULL &&
                sscanf(strstr(req, "camera.ch."), "camera.ch.%u.", &n) == 1 &&
                n < 8u) {
                val = g_ip_tab[n];
            }
            if (val != NULL) {
                w = snprintf(rsp, sizeof rsp,
                             "{\"ok\":true,\"d\":{\"value\":\"%s\"}}", val);
            } else {
                w = snprintf(rsp, sizeof rsp, "{\"ok\":false}");
            }
            {
                ivs_chan_hdr_t rh;

                if (iv_chan_hdr_init(&rh, IV_CHAN_TYPE_RSP, hdr.request_id, 0u,
                                     (uint32_t)w) != IV_OK)
                    return -1;
                if (iv_chan_send(cfd, &rh, rsp, (size_t)w) != IV_OK)
                    return -1;
            }
        }
    }
}

static void fake_ivsboxd_main(void)
{
    iv_chan_acl_t acl;
    int           lfd = iv_chan_listen(g_chan, 0666, 8);

    if (lfd < 0)
        _exit(3);
    iv_chan_acl_default(&acl);
    for (;;) {
        int cfd = iv_chan_accept(lfd, &acl, NULL);

        if (cfd < 0) {
            sleep_ms(20);
            continue;
        }
        (void)fake_ivsboxd_serve_conn(cfd);
        (void)close(cfd);
    }
}

/* --------------------------------------------------------------------------
 * 假摄像机：最小 HTTP 服务（echo / 302 / Set-Cookie / 页面含绝对 URL）
 * -------------------------------------------------------------------------- */
static void fake_camera_main(void)
{
    struct sockaddr_in a;
    int                lfd = socket(AF_INET, SOCK_STREAM, 0);
    static char        rbuf[8192];

    if (lfd < 0)
        _exit(3);
    {
        int on = 1;

        (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    }
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons((uint16_t)g_cam_port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(lfd, 8) != 0)
        _exit(3);

    for (;;) {
        int    cfd = accept(lfd, NULL, NULL);
        size_t got = 0;
        char  *hed;
        char   method[8] = "";
        char   path[128] = "";
        int    clen = 0;

        if (cfd < 0) {
            sleep_ms(20);
            continue;
        }
        {
            struct timeval tv;

            tv.tv_sec  = 5;
            tv.tv_usec = 0;
            (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
        /* 收齐头区 */
        for (;;) {
            ssize_t n = recv(cfd, rbuf + got, sizeof rbuf - 1u - got, 0);

            if (n <= 0)
                break;
            got += (size_t)n;
            rbuf[got] = '\0';
            if (strstr(rbuf, "\r\n\r\n") != NULL || got + 1u >= sizeof rbuf)
                break;
        }
        rbuf[got] = '\0';
        /* 收体（若有） */
        hed = strstr(rbuf, "\r\n\r\n");
        if (hed != NULL) {
            const char *cl = strcasestr(rbuf, "Content-Length:");

            if (cl != NULL)
                clen = atoi(cl + 15);
            {
                size_t have = got - (size_t)(hed + 4 - rbuf);

                while (have < (size_t)clen && got + 1u < sizeof rbuf) {
                    ssize_t n = recv(cfd, rbuf + got, sizeof rbuf - 1u - got, 0);

                    if (n <= 0)
                        break;
                    got += (size_t)n;
                    have += (size_t)n;
                    rbuf[got] = '\0';
                }
            }
        }
        (void)sscanf(rbuf, "%7s %127s", method, path);

        {
            char rsp[2048];
            const char *body = NULL;
            int         status = 200;
            const char *ctype = "text/plain";
            char        extra[256];

            extra[0] = '\0';
            if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
                static char page[512];

                ctype = "text/html";
                (void)snprintf(page, sizeof page,
                               "<html><a href=\"http://127.0.0.1:%d/login.js\">l</a>"
                               "<a href=\"/style.css\">s</a>"
                               "<script src=\"http://127.0.0.1:%d/app.js\"></script>"
                               "</html>", g_cam_port, g_cam_port);
                body = page;
            } else if (strcmp(path, "/go") == 0) {
                status = 302;
                (void)snprintf(extra, sizeof extra,
                               "Location: http://127.0.0.1:%d/idx\r\n", g_cam_port);
                body = "";
            } else if (strcmp(path, "/setck") == 0) {
                (void)snprintf(extra, sizeof extra,
                               "Set-Cookie: camsid=abc123; Path=/; HttpOnly\r\n");
                body = "ck";
            } else if (strcmp(path, "/echo") == 0) {
                static char ebuf[1024];
                const char *ct = strcasestr(rbuf, "Content-Type:");
                const char *ck = strcasestr(rbuf, "Cookie:");
                const char *au = strcasestr(rbuf, "Authorization:");
                char        cts[96] = "-";
                char        cks[128] = "-";
                char        aus[128] = "-";
                const char *bodyp = (hed != NULL) ? hed + 4 : "";
                char        bcopy[256];

                (void)snprintf(bcopy, sizeof bcopy, "%s", bodyp);
                for (char *q = bcopy; *q; q++)
                    if (*q == '\r' || *q == '\n')
                        *q = ' ';
                if (ct != NULL) (void)sscanf(ct, "Content-Type: %95s", cts);
                if (ck != NULL) (void)sscanf(ck, "Cookie: %127[^\r]", cks);
                if (au != NULL) (void)sscanf(au, "Authorization: %127[^\r]", aus);
                (void)snprintf(ebuf, sizeof ebuf,
                               "METHOD=%s PATH=%s CT=%s CK=%s AUTH=%s BODY=%s",
                               method, path, cts, cks, aus, bcopy);
                body = ebuf;
            } else {
                status = 404;
                body = "nope";
            }

            {
                int w = snprintf(rsp, sizeof rsp,
                                 "HTTP/1.1 %d X\r\nContent-Type: %s\r\n"
                                 "Content-Length: %zu\r\n%sConnection: close\r\n\r\n",
                                 status, ctype, strlen(body), extra);
                size_t blen = strlen(body);

                (void)send(cfd, rsp, (size_t)w, MSG_NOSIGNAL);
                if (blen > 0u)
                    (void)send(cfd, body, blen, MSG_NOSIGNAL);
            }
        }
        (void)close(cfd);
    }
}

/* --------------------------------------------------------------------------
 * 起停
 * -------------------------------------------------------------------------- */
static pid_t spawn_web(const char *bin, int mport, int cport, int upstream)
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0) {
            (void)dup2(devnull, 1);
            (void)dup2(devnull, 2);
            if (devnull > 2)
                (void)close(devnull);
        }
        {
            char mp[16];
            char cp[16];
            char up[16];

            (void)snprintf(mp, sizeof mp, "%d", mport);
            (void)snprintf(cp, sizeof cp, "%d", cport);
            (void)snprintf(up, sizeof up, "%d", upstream);
            (void)execl(bin, bin,
                        "--port", mp, "--cam-port", cp, "--cam-upstream", up,
                        "--chan", g_chan, "--root", g_dir, "--media-root", g_dir,
                        "--heartbeat-ms", "0", "--pass", "x", (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static void stop_kid(pid_t *kid)
{
    if (*kid > 0) {
        int st = 0;

        (void)kill(*kid, SIGKILL);
        (void)waitpid(*kid, &st, 0);
        *kid = -1;
    }
}

static void cleanup(void)
{
    stop_kid(&g_kid_web1);
    stop_kid(&g_kid_web2);
    stop_kid(&g_kid_camera);
    stop_kid(&g_kid_ivsboxd);
    (void)unlink(g_chan);
    (void)rmdir(g_dir);
}

/* -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    const char *bin = NULL;

    if (argc > 1)
        bin = argv[1];
    if (bin == NULL)
        bin = getenv("IV_WEB_BIN");
    if (bin == NULL)
        bin = "build/host/ivsbox-web";

    {
        struct stat st;

        if (stat(bin, &st) != 0) {
            printf("test_web_cam_e2e SKIPPED: %s not found "
                   "(build with `make host`, or set IV_WEB_BIN)\n", bin);
            return 0;
        }
    }

    {
        char tmpl[] = "/tmp/ivcamXXXXXX";
        char *ok = mkdtemp(tmpl);

        if (ok == NULL) {
            fprintf(stderr, "FAIL: mkdtemp: %s\n", strerror(errno));
            return 1;
        }
        (void)snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    }
    (void)snprintf(g_chan, sizeof g_chan, "%s/ctl", g_dir);

    g_cam_port  = find_free_port();
    g_web_port  = find_free_port();
    g_cam_lport = find_free_port();
    g_web2_port = find_free_port();
    g_cam2_lport = find_free_port();
    if (g_cam_port <= 0 || g_web_port <= 0 || g_cam_lport <= 0 ||
        g_web2_port <= 0 || g_cam2_lport <= 0) {
        fprintf(stderr, "FAIL: no free ports\n");
        cleanup();
        return 1;
    }

    (void)signal(SIGPIPE, SIG_IGN);

    g_kid_ivsboxd = fork();
    if (g_kid_ivsboxd == 0)
        fake_ivsboxd_main();
    if (g_kid_ivsboxd < 0) {
        fprintf(stderr, "FAIL: fork fake ivsboxd\n");
        cleanup();
        return 1;
    }
    g_kid_camera = fork();
    if (g_kid_camera == 0)
        fake_camera_main();
    if (g_kid_camera < 0) {
        fprintf(stderr, "FAIL: fork fake camera\n");
        cleanup();
        return 1;
    }

    g_kid_web1 = (pid_t)spawn_web(bin, g_web_port, g_cam_lport, g_cam_port);
    g_kid_web2 = (pid_t)spawn_web(bin, g_web2_port, g_cam2_lport, g_cam2_lport);
    if (g_kid_web1 < 0 || g_kid_web2 < 0) {
        fprintf(stderr, "FAIL: fork ivsbox-web\n");
        cleanup();
        return 1;
    }

    /* ---------------- c01 主口不服务摄像机路由 ---------------- */
    {
        int len = snprintf(g_req, sizeof g_req,
                           "GET /cam/0/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        chk(exchange(g_web_port, g_req, (size_t)len) > 0 && rsp_status() == 404,
            "c01: main port does not serve /cam/0/");
    }

    /* ---------------- c02 主口功能不受影响（登录仍 200） ---------------- */
    {
        char b[64];
        int  blen = snprintf(b, sizeof b, "{\"user\":\"admin\",\"pass\":\"x\"}");
        int  n = snprintf(g_req, sizeof g_req,
                          "POST /api/login HTTP/1.1\r\nHost: x\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);

        memcpy(g_req + n, b, (size_t)blen);
        chk(exchange(g_web_port, g_req, (size_t)n + (size_t)blen) > 0 &&
            rsp_status() == 200, "c02: main port login still works");
    }

    /* ---------------- c10 专用口路由隔离：/api/login 与 / 都是 404 ---------------- */
    {
        char b[64];
        int  blen = snprintf(b, sizeof b, "{\"user\":\"admin\",\"pass\":\"x\"}");
        int  n = snprintf(g_req, sizeof g_req,
                          "POST /api/login HTTP/1.1\r\nHost: x\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);

        memcpy(g_req + n, b, (size_t)blen);
        chk(exchange(g_cam_lport, g_req, (size_t)n + (size_t)blen) > 0 &&
            rsp_status() == 404, "c10a: dedicated port /api/login -> 404");
        chk(strstr(g_rsp, "Set-Cookie") == NULL,
            "c10b: dedicated port never issues a session cookie");
    }
    {
        int len = snprintf(g_req, sizeof g_req,
                           "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        chk(exchange(g_cam_lport, g_req, (size_t)len) > 0 && rsp_status() == 404,
            "c10c: dedicated port / -> 404");
    }

    /* ---------------- c03 页面代理 + 绝对 URL/相对路径改写 ---------------- */
    {
        int len = snprintf(g_req, sizeof g_req,
                           "GET /cam/0/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        int total = exchange(g_cam_lport, g_req, (size_t)len);

        chk(total > 0 && rsp_status() == 200, "c03a: /cam/0/ -> 200");
        chk(strstr(g_rsp, "/cam/0/login.js") != NULL,
            "c03b: absolute URL rewritten to /cam/0/login.js");
        /* 计划口径（S8.5"真机兼容性不承诺"）：改写只覆盖绝对 URL、Location、
         * Set-Cookie Path 三种形态；正文里的站内相对路径（href="/style.css"）
         * 原样透传 —— 这是登记过的已知局限，不是缺陷。 */
        chk(strstr(g_rsp, "href=\"/style.css\"") != NULL,
            "c03c: relative link passes through verbatim (documented scope)");
        chk(strstr(g_rsp, "http://127.0.0.1") == NULL,
            "c03d: no absolute upstream URL leaks to the client");
    }

    /* ---------------- c04 302 Location 重写 ---------------- */
    {
        char loc[128];
        int  len = snprintf(g_req, sizeof g_req,
                            "GET /cam/0/go HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        chk(exchange(g_cam_lport, g_req, (size_t)len) > 0 && rsp_status() == 302,
            "c04a: redirect passes through as 302");
        chk(hdr_val((size_t)exchange(g_cam_lport, g_req, (size_t)len),
                    "Location:", loc, sizeof loc) && strcmp(loc, "/cam/0/idx") == 0,
            "c04b: Location rewritten to /cam/0/idx");
    }

    /* ---------------- c05 Set-Cookie Path 重写 ---------------- */
    {
        char ck[128];
        int  len = snprintf(g_req, sizeof g_req,
                            "GET /cam/0/setck HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        chk(exchange(g_cam_lport, g_req, (size_t)len) > 0 && rsp_status() == 200,
            "c05a: cookie route -> 200");
        {
            int total2 = exchange(g_cam_lport, g_req, (size_t)len);
            int got = hdr_val((size_t)total2, "Set-Cookie:", ck, sizeof ck);

            if (!(got && strstr(ck, "camsid=abc123") != NULL &&
                  strstr(ck, "Path=/cam/0/") != NULL)) {
                fprintf(stderr, "DEBUG c05b raw response:\n%.*s\n",
                        (int)(total2 > 800 ? 800 : total2), g_rsp);
            }
            chk(got && strstr(ck, "camsid=abc123") != NULL &&
                strstr(ck, "Path=/cam/0/") != NULL,
                "c05b: Set-Cookie Path rewritten to /cam/0/");
        }
    }

    /* ---------------- c06 POST + 头透传（Cookie/Authorization/Content-Type/体） ------- */
    {
        const char *b = "hello=world";
        int  blen = (int)strlen(b);
        int  n = snprintf(g_req, sizeof g_req,
                          "POST /cam/0/echo HTTP/1.1\r\nHost: x\r\n"
                          "Content-Type: application/x-www-form-urlencoded\r\n"
                          "Cookie: camsid=xyz\r\n"
                          "Authorization: Basic dXNlcjpwYXNz\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);

        memcpy(g_req + n, b, (size_t)blen);
        chk(exchange(g_cam_lport, g_req, (size_t)n + (size_t)blen) > 0 &&
            rsp_status() == 200, "c06a: POST echo -> 200");
        chk(strstr(g_rsp, "METHOD=POST") != NULL && strstr(g_rsp, "PATH=/echo") != NULL,
            "c06b: method and path reached the camera");
        chk(strstr(g_rsp, "CT=application/x-www-form-urlencoded") != NULL,
            "c06c: Content-Type forwarded");
        chk(strstr(g_rsp, "CK=camsid=xyz") != NULL,
            "c06d: Cookie forwarded verbatim");
        chk(strstr(g_rsp, "AUTH=Basic dXNlcjpwYXNz") != NULL,
            "c06e: Authorization forwarded verbatim");
        chk(strstr(g_rsp, "BODY=hello=world") != NULL,
            "c06f: POST body forwarded");
    }

    /* ---------------- c07 未配置的路：502（不尝试连接） ---------------- */
    {
        int len = snprintf(g_req, sizeof g_req,
                           "GET /cam/1/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        chk(exchange(g_cam_lport, g_req, (size_t)len) > 0 && rsp_status() == 502,
            "c07a: unconfigured channel -> 502");
        chk(strstr(g_rsp, "not configured") != NULL,
            "c07b: body says not configured");
    }

    /* ---------------- c08 配置了但不在线：502 ---------------- */
    {
        int len = snprintf(g_req, sizeof g_req,
                           "GET /cam/2/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        chk(exchange(g_cam_lport, g_req, (size_t)len) > 0 && rsp_status() == 502,
            "c08: configured but unreachable -> 502");
    }

    /* ---------------- c09 非法路由/方法：404 / 405 ---------------- */
    {
        static const char *pats[] = {
            "/cam/8/", "/cam/01/", "/cam/x/", "/cam/", "/cam2/0/",
        };
        size_t i;

        for (i = 0; i < sizeof pats / sizeof pats[0]; i++) {
            int len = snprintf(g_req, sizeof g_req,
                               "GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                               pats[i]);

            chk(exchange(g_cam_lport, g_req, (size_t)len) > 0 && rsp_status() == 404,
                "c09a: bad route 404");
        }
    }
    {
        int len = snprintf(g_req, sizeof g_req,
                           "PUT /cam/0/ HTTP/1.1\r\nHost: x\r\n"
                           "Content-Length: 0\r\nConnection: close\r\n\r\n");

        chk(exchange(g_cam_lport, g_req, (size_t)len) > 0 && rsp_status() == 405,
            "c09b: PUT -> 405");
    }

    /* ---------------- c12 环回护栏：上游=监听口 ⇒ 拒绝 ---------------- */
    {
        int len = snprintf(g_req, sizeof g_req,
                           "GET /cam/3/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        chk(exchange(g_cam2_lport, g_req, (size_t)len) > 0 && rsp_status() == 502,
            "c12a: self-target refused -> 502");
        chk(strstr(g_rsp, "refused") != NULL,
            "c12b: body says refused");
    }

    /* ---------------- c13 fd 稳定性 ---------------- */
    {
        int before = count_fds(g_kid_web1);
        int after;
        int i;
        int ok = 1;

        for (i = 0; i < 10 && ok; i++) {
            int len = snprintf(g_req, sizeof g_req,
                               "GET /cam/0/ HTTP/1.1\r\nHost: x\r\n"
                               "Connection: close\r\n\r\n");

            if (exchange(g_cam_lport, g_req, (size_t)len) <= 0 || rsp_status() != 200)
                ok = 0;
        }
        chk(ok, "c13a: 10 proxied requests all answered 200");
        sleep_ms(300);
        after = count_fds(g_kid_web1);
        if (before > 0 && after > 0)
            chk(after == before, "c13b: no fd leak across cam proxy requests");
    }

    cleanup();
    if (g_fail != 0) {
        fprintf(stderr, "test_web_cam_e2e FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_web_cam_e2e passed (route isolation, proxy+rewrites, header/body "
           "passthrough, 502 paths, loopback guard, fd stability)\n");
    return 0;
}
