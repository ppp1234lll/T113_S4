/*
 * ivsbox-web HTTP 层端到端 —— 集成测试（功能开发计划 M5-S5.1/S5.2 验证项）
 *
 * ============================ 为什么必须是集成测试 ============================
 * 本用例**自己把 `ivsbox-web` 拉起来**，然后用裸 socket 按字节驱动它。理由：
 * 这一层真正会出错的地方是"TCP 分段"——浏览器/curl 把请求头和请求体分两次 write
 * 是常态，而 HTTP 解析器往往在"头齐了、体没齐"时被调用第二次。这类缺陷在模块级
 * 单测里天然不可复现（单测总是把一条请求一次性喂进去），只有真的走一遍 socket 才
 * 看得见。
 *
 * 2026-10-13 实测就抓到一例：`parse_req` 把请求头**就地**逐行写成 '\0'，导致第二次
 * 进入时再也找不到 `\r\n\r\n` 分隔符，请求被永久挂起（客户端只见"没有任何响应"）。
 * 本用例的 w02/w03 就是钉这个的回归守卫 —— 把 parse_req 改回就地改写，它们必须红。
 *
 * ============================ 依赖 ============================
 * 只需要 `ivsbox-web` 二进制与静态资源目录，**不需要 ivsboxd**：本用例覆盖的路径
 * （静态文件、鉴权拒绝、坏请求、目录穿越、fd 不泄漏）全部在 Web 进程内闭环，
 * 不碰本地通道。这样它在 CI 里不会因"没起主控"而假红。
 *
 * ============================ 用法 ============================
 *   test_web_http [ivsbox-web 二进制路径]
 * 省略时依次尝试环境变量 `IV_WEB_BIN`、`build/host/ivsbox-web`。
 * 静态资源根目录同样可用环境变量 `IV_WEB_ROOT` 覆盖，默认 `web/static`。
 * 二者缺一（例如在纯 ARM 构建目录下跑）时本用例**明确报 SKIP 并以 0 退出**，
 * 不会给出含义模糊的失败。
 *
 * 本用例只 kill 自己拉起的子进程，不落任何文件（AGENTS.md 规则 6）。
 */
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS      5000   /* 单次往返预算：本地回环，留足余量只为抗调度抖动 */
#define CONNECT_RETRY   5000   /* 等服务 bind+listen 完成；拉起与请求几乎同时 */
#define CONNECT_STEP    100
#define SPLIT_GAP_MS    200    /* 头/体之间的间隔：必须明显大于一个事件循环周期，
                                * 否则内核可能把两次 write 合并成一段，
                                * "分段"这个前提就不成立了 */
#define FD_LEAK_REQS    200    /* fd 泄漏检查的请求次数（ab -c 50 的本机等价替代） */
#define RSP_CAP         (128u * 1024u)

static int   g_fail;
static pid_t g_child = -1;

/* 收发缓冲放静态区：RSP_CAP 是 128 KiB 量级，不占栈。 */
static char g_rsp[RSP_CAP];
static char g_req[4096];

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

static void on_sig(int sig)
{
    (void)sig;
}

/* --------------------------------------------------------------------------
 * 起停被测进程
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
    a.sin_port        = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &alen) == 0)
        port = (int)ntohs(a.sin_port);
    (void)close(fd);
    return port;
}

static int spawn_web(const char *bin, const char *root, int port)
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* 子进程：把 stdout/stderr 丢进 /dev/null（日志里中文/噪声不该污染用例输出）。 */
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0) {
            (void)dup2(devnull, 1);
            (void)dup2(devnull, 2);
            if (devnull > 2)
                (void)close(devnull);
        }
        {
            char pbuf[16];
            char rbuf[512];

            (void)snprintf(pbuf, sizeof(pbuf), "%d", port);
            (void)snprintf(rbuf, sizeof(rbuf), "%.480s", root);
            (void)execl(bin, bin, "--port", pbuf, "--root", rbuf,
                        "--pass", "testpass", (char *)NULL);
        }
        _exit(127);
    }
    return (int)pid;
}

/* --------------------------------------------------------------------------
 * 裸 socket 往返
 * -------------------------------------------------------------------------- */
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

/*
 * 发一条请求并收全部响应。
 *   split1/split2 —— 把请求切成三段发：[0,split1) / [split1,split2) / [split2,len)，
 *   中间各隔 SPLIT_GAP_MS。用 0/len 之外的切点就能复现"头体分两次到达"。
 *   返回收到的总字节数；<0 表示失败。响应正文在 g_rsp 里。
 */
static int http_exchange(int port, const char *req, size_t len,
                         size_t split1, size_t split2, int gap_ms, int *status_out)
{
    struct timeval tv;
    int            fd = connect_retry(port);
    size_t         sent = 0;
    size_t         total = 0;
    int            status = -1;

    if (status_out != NULL)
        *status_out = -1;
    if (fd < 0)
        return -1;

    tv.tv_sec  = TIMEOUT_MS / 1000;
    tv.tv_usec = (TIMEOUT_MS % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec  = TIMEOUT_MS / 1000;
    tv.tv_usec = (TIMEOUT_MS % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 第一段 */
    while (sent < split1) {
        ssize_t n = send(fd, req + sent, split1 - sent, MSG_NOSIGNAL);

        if (n <= 0)
            goto out;
        sent += (size_t)n;
    }
    if (split1 > 0u) {
        /* 先偷看一眼：如果服务端此时就把响应发出来了（说明它没等体就处理了），
         * 也照样把响应收下来，交给上层断言去判。 */
        ssize_t n = recv(fd, g_rsp, RSP_CAP, MSG_DONTWAIT);

        if (n > 0) {
            total = (size_t)n;
            goto read_rest;
        }
    }
    sleep_ms(gap_ms);

    /* 第二段 */
    while (sent < split2) {
        ssize_t n = send(fd, req + sent, split2 - sent, MSG_NOSIGNAL);

        if (n <= 0)
            goto out;
        sent += (size_t)n;
    }
    if (split2 > split1)
        sleep_ms(gap_ms);

    /* 第三段 */
    while (sent < len) {
        ssize_t n = send(fd, req + sent, len - sent, MSG_NOSIGNAL);

        if (n <= 0)
            goto out;
        sent += (size_t)n;
    }

read_rest:
    for (;;) {
        ssize_t n;

        if (total >= RSP_CAP)
            break;
        n = recv(fd, g_rsp + total, RSP_CAP - total, 0);
        if (n == 0)
            break; /* 服务端 close：一个请求一条连接，正常收尾 */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break; /* EAGAIN = 超时，同样按"收完了"处理，由断言判长度 */
        }
        total += (size_t)n;
    }

out:
    (void)close(fd);
    if (status_out != NULL) {
        if (total > 4u && memcmp(g_rsp, "HTTP/", 5u) == 0) {
            /* "HTTP/1.1 200 OK" → 取第 3 个 token */
            const char *sp = memchr(g_rsp, ' ', 16u);

            if (sp != NULL)
                status = atoi(sp + 1);
        }
        *status_out = status;
    }
    return (int)total;
}

/* 从响应头里取 Content-Length；找不到返回 -1。 */
static long rsp_content_length(size_t total)
{
    const char *end = NULL;
    size_t      i;
    long        v = -1;

    for (i = 0; i + 3u < total; i++) {
        if (memcmp(g_rsp + i, "\r\n\r\n", 4u) == 0) {
            end = g_rsp + i + 4;
            break;
        }
    }
    if (end == NULL)
        return -1;
    for (i = 0; i + 15u < (size_t)(end - g_rsp); i++) {
        if (strncasecmp(g_rsp + i, "Content-Length:", 15u) == 0 &&
            (i == 0u || g_rsp[i - 1u] == '\n')) {
            v = strtol(g_rsp + i + 15, NULL, 10);
            break;
        }
    }
    return v;
}

static int rsp_has(const char *needle)
{
    return strstr(g_rsp, needle) != NULL;
}

/* 正文长度（总长 - 头长）。头不完整时返回 -1。 */
static long rsp_body_len(size_t total)
{
    size_t i;

    for (i = 0; i + 3u < total; i++) {
        if (memcmp(g_rsp + i, "\r\n\r\n", 4u) == 0)
            return (long)(total - (i + 4u));
    }
    return -1;
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

/* -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    const char *bin = NULL;
    const char *root;
    int         port;
    int         status = 0;
    int         total;
    long        cl;
    long        bl;

    if (argc > 1)
        bin = argv[1];
    if (bin == NULL)
        bin = getenv("IV_WEB_BIN");
    if (bin == NULL)
        bin = "build/host/ivsbox-web";

    root = getenv("IV_WEB_ROOT");
    if (root == NULL)
        root = "web/static";

    {
        struct stat st;

        if (stat(bin, &st) != 0) {
            printf("test_web_http SKIPPED: %s not found "
                   "(build with `make host`, or set IV_WEB_BIN)\n", bin);
            return 0;
        }
        if (stat(root, &st) != 0) {
            printf("test_web_http SKIPPED: static root %s not found "
                   "(set IV_WEB_ROOT)\n", root);
            return 0;
        }
    }

    port = find_free_port();
    if (port <= 0) {
        fprintf(stderr, "FAIL: no free port\n");
        return 1;
    }

    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGCHLD, on_sig);

    g_child = (pid_t)spawn_web(bin, root, port);
    if (g_child < 0) {
        fprintf(stderr, "FAIL: cannot fork ivsbox-web\n");
        return 1;
    }

    /* ---------------- w01 单次写全：静态首页 ---------------- */
    {
        int len = snprintf(g_req, sizeof(g_req),
                           "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                           "Connection: close\r\n\r\n");

        total = http_exchange(port, g_req, (size_t)len, (size_t)len, (size_t)len, 0, &status);
        chk(status == 200, "w01a: GET / -> 200");
        chk(total > 0 && rsp_has("<!DOCTYPE html>"), "w01b: index.html body");
        cl = rsp_content_length((size_t)total);
        bl = rsp_body_len((size_t)total);
        chk(cl > 0 && cl == bl, "w01c: Content-Length matches body bytes");
    }

    /* ---------------- w02 头/体分两次写：POST /api/login ---------------- */
    {
        char   body[64];
        int    blen;
        size_t head_len;
        size_t len;
        int    n;

        blen = snprintf(body, sizeof(body), "{\"pass\":\"testpass\"}");
        n = snprintf(g_req, sizeof(g_req),
                     "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n",
                     blen);
        head_len = (size_t)n;
        memcpy(g_req + head_len, body, (size_t)blen);
        len = head_len + (size_t)blen;

        /* 切在头与体之间 —— 正是 curl 的实际时序 */
        total = http_exchange(port, g_req, len, head_len, len, SPLIT_GAP_MS, &status);
        chk(status == 200, "w02a: split POST /api/login -> 200 (parse_req must be reentrant)");
        cl = rsp_content_length((size_t)total);
        bl = rsp_body_len((size_t)total);
        chk(cl > 0, "w02b: login response declares a body");
        chk(cl > 0 && cl == bl, "w02c: login body fully delivered (header+body length agree)");
        chk(total > 0 && rsp_has("\"token\""), "w02d: login returns a session token");
        chk(total > 0 && rsp_has("Set-Cookie: ivs_sid="), "w02e: login sets the session cookie");
    }

    /* ---------------- w03 逐字节喂：分段三次 ---------------- */
    {
        char   body[64];
        int    blen;
        size_t head_len;
        size_t len;
        int    n;

        blen = snprintf(body, sizeof(body), "{\"pass\":\"testpass\"}");
        n = snprintf(g_req, sizeof(g_req),
                     "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);
        head_len = (size_t)n;
        memcpy(g_req + head_len, body, (size_t)blen);
        len = head_len + (size_t)blen;

        /* 三段：头的前半 / 头的后半 / 整个体 */
        total = http_exchange(port, g_req, len, head_len / 2u, head_len, SPLIT_GAP_MS, &status);
        chk(status == 200, "w03: three-way split POST still answers 200");
        cl = rsp_content_length((size_t)total);
        bl = rsp_body_len((size_t)total);
        chk(cl > 0 && cl == bl, "w03b: body fully delivered after three-way split");
    }

    /* ---------------- w04 口令错：401 + 有正文 ---------------- */
    {
        char body[64];
        int  blen = snprintf(body, sizeof(body), "{\"pass\":\"nope\"}");
        int  n = snprintf(g_req, sizeof(g_req),
                          "POST /api/login HTTP/1.1\r\nHost: x\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);

        memcpy(g_req + n, body, (size_t)blen);
        total = http_exchange(port, g_req, (size_t)n + (size_t)blen,
                              (size_t)n + (size_t)blen, (size_t)n + (size_t)blen, 0, &status);
        chk(status == 401, "w04a: wrong password -> 401");
        chk(total > 0 && rsp_has("\"ok\":false"), "w04b: 401 carries a JSON error body");
        cl = rsp_content_length((size_t)total);
        bl = rsp_body_len((size_t)total);
        chk(cl > 0 && cl == bl, "w04c: 401 body length consistent");
    }

    /* ---------------- w05 未鉴权访问 API：401 ---------------- */
    {
        int len = snprintf(g_req, sizeof(g_req),
                           "GET /api/overview HTTP/1.1\r\nHost: x\r\n"
                           "Connection: close\r\n\r\n");

        total = http_exchange(port, g_req, (size_t)len, (size_t)len, (size_t)len, 0, &status);
        chk(status == 401, "w05a: /api without session -> 401");
        chk(total > 0 && rsp_has("unauthorized"), "w05b: 401 says unauthorized");
    }

    /* ---------------- w06 未知路径：404 ---------------- */
    {
        int len = snprintf(g_req, sizeof(g_req),
                           "GET /no/such/file.txt HTTP/1.1\r\nHost: x\r\n"
                           "Connection: close\r\n\r\n");

        total = http_exchange(port, g_req, (size_t)len, (size_t)len, (size_t)len, 0, &status);
        chk(status == 404, "w06: unknown static path -> 404");
    }

    /* ---------------- w07 目录穿越：不得吐出系统文件 ---------------- */
    {
        static const char *pats[] = {
            "/../../etc/passwd",
            "/..%2f..%2fetc/passwd",
            "/./../../etc/passwd",
        };
        size_t i;

        for (i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
            int len = snprintf(g_req, sizeof(g_req),
                               "GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                               pats[i]);

            total = http_exchange(port, g_req, (size_t)len, (size_t)len, (size_t)len, 0, &status);
            chk(status != 200, "w07a: traversal gets no 200");
            chk(!rsp_has("root:x:"), "w07b: traversal body must not leak /etc/passwd");
        }
    }

    /* ---------------- w08 超大 Content-Length：400 ---------------- */
    {
        int len = snprintf(g_req, sizeof(g_req),
                           "POST /api/login HTTP/1.1\r\nHost: x\r\n"
                           "Content-Length: 40000\r\nConnection: close\r\n\r\n");

        total = http_exchange(port, g_req, (size_t)len, (size_t)len, (size_t)len, 0, &status);
        cl = rsp_content_length((size_t)total);
        bl = rsp_body_len((size_t)total);
        chk(status == 400, "w08: Content-Length over the cap -> 400 (no hang, no OOM)");
    }

    /* ---------------- w09 静态资源 MIME ---------------- */
    {
        int len = snprintf(g_req, sizeof(g_req),
                           "GET /app.js HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        total = http_exchange(port, g_req, (size_t)len, (size_t)len, (size_t)len, 0, &status);
        chk(status == 200, "w09a: /app.js -> 200");
        chk(total > 0 && rsp_has("javascript"), "w09b: /app.js served as javascript");
        cl = rsp_content_length((size_t)total);
        bl = rsp_body_len((size_t)total);
        chk(cl > 0 && cl == bl, "w09c: static body length consistent");
    }

    /* ---------------- w10 fd 不泄漏（ab -c N 的本机等价替代） ---------------- */
    {
        int before = count_fds(g_child);
        int after;
        int i;

        chk(before > 0, "w10a: can read child fd table");
        for (i = 0; i < FD_LEAK_REQS; i++) {
            int len = snprintf(g_req, sizeof(g_req),
                               "GET /app.js HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

            total = http_exchange(port, g_req, (size_t)len, (size_t)len, (size_t)len, 0, &status);
            if (status != 200) {
                chk(0, "w10b: keep-alive-less churn request failed");
                break;
            }
        }
        sleep_ms(300); /* 给事件循环一个回收窗口 */
        after = count_fds(g_child);
        if (before > 0 && after > 0)
            chk(after == before, "w10c: no fd leak after 200 connection-per-request cycles");
    }

    /* ---------------- 收尾 ---------------- */
    (void)kill(g_child, SIGTERM);
    {
        int st = 0;
        int waited = 0;

        while (waitpid(g_child, &st, WNOHANG) == 0 && waited < 2000) {
            sleep_ms(100);
            waited += 100;
        }
        if (waited >= 2000) {
            (void)kill(g_child, SIGKILL);
            (void)waitpid(g_child, &st, 0);
        }
    }
    g_child = -1;

    if (g_fail != 0) {
        fprintf(stderr, "test_web_http FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_web_http passed (static, split-request reentrancy, auth, traversal, "
           "body cap, fd stability)\n");
    return 0;
}
