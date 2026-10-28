/*
 * ivsbox-web 媒体下载端到端 —— 集成测试（功能开发计划 M6-S6.4 验证项）
 *
 * ============================ 为什么必须是集成测试 ============================
 * S6.4 的判据是"Range 请求拿到的分片必须与全量 sha256 一致"，而真正容易错的
 * 地方全在 http 层与文件层的**交界**上：Content-Length 取的是文件段长度还是
 * body 长度、206 的 Content-Range 边界、If-Range 失配该退全量、`send_left`
 * 归零的收尾、限速把 POLLOUT 变成空转……这些在模块单测里都不可复现 —— 单测
 * 不会经过 socket，也不会让内核把一块 32 KiB 的数据拆成多次可写事件。
 *
 * 本用例**自己拉起 `ivsbox-web`**（`--media-root` 指向临时目录、`--media-bps`
 * 给一个明显小的值），用裸 socket 按字节驱动它，然后：
 *   - 把响应体与**本地生成的那份确定内容**逐字节比对（不是比长度）；
 *   - 把 `X-SHA256` 与本地 `iv_sha256_file()` 的结果比对；
 *   - 用墙钟验证限速真的生效（去限速的变异会立刻把这个断言变红）。
 *
 * ============================ 依赖 ============================
 * 需要 `ivsbox-web` 二进制。**不需要 ivsboxd**：媒体下载、鉴权、路径围栏全部在
 * Web 进程内闭环（登录只比对 `--pass`，不走本地通道），所以在 CI 里不会因
 * "没起主控"而假红。
 *
 * ============================ 用法 ============================
 *   test_web_media_e2e [ivsbox-web 二进制路径]
 * 省略时依次尝试环境变量 `IV_WEB_BIN`、`build/host/ivsbox-web`。二进制缺失时
 * 报 SKIP 并以 0 退出。
 *
 * 本用例只在自己的临时目录里造一份测试文件，收尾时全部删除；只 kill 自己拉起的
 * 子进程（AGENTS.md 规则 6）。
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "ivsbox/iv_sha256.h"

#define TIMEOUT_MS    5000
#define CONNECT_RETRY 5000
#define CONNECT_STEP  100
#define RSP_CAP       (256u * 1024u)
#define MEDIA_BPS     65536          /* 64 KiB/s：让"限速生效"能用墙钟判出来 */
/* 载荷长度刻意不是 2 的幂，也不对齐块边界 —— 收尾那块"零头"最容易算错。 */
#define PAYLOAD_LEN   (65536u + 123u)
#define MEDIA_REL     "/media/video/20261014/channel0_120000.mp4"

static int   g_fail;
static pid_t g_child = -1;
static char  g_rsp[RSP_CAP];
static char  g_req[4096];
static char  g_media[256];   /* 临时媒体根 */
static char  g_file[512];    /* 测试文件全路径 */
/* 目录列表（m14）用的快照夹具：两张 jpg ＋ 一个"白名单外"的文件名 */
static char  g_snap_dir[300];
static char  g_snap_a[400];
static char  g_snap_b[400];
static char  g_snap_bad[400];
static unsigned char g_payload[PAYLOAD_LEN];

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
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
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &alen) == 0)
        port = (int)ntohs(a.sin_port);
    (void)close(fd);
    return port;
}

static int spawn_web(const char *bin, int port)
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
            char pbuf[16];
            char bbuf[16];

            (void)snprintf(pbuf, sizeof(pbuf), "%d", port);
            (void)snprintf(bbuf, sizeof(bbuf), "%d", MEDIA_BPS);
            (void)execl(bin, bin, "--port", pbuf,
                        "--root", g_media, "--media-root", g_media,
                        "--media-bps", bbuf, "--pass", "testpass", (char *)NULL);
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

static void set_timeouts(int fd)
{
    struct timeval tv;

    tv.tv_sec  = TIMEOUT_MS / 1000;
    tv.tv_usec = (TIMEOUT_MS % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* 发一条完整请求并收全部响应；返回收到字节数（<0 失败）。响应在 g_rsp。 */
static int exchange(int port, const char *req, size_t len, uint64_t *elapsed_out)
{
    int      fd = connect_retry(port);
    size_t   sent = 0;
    size_t   total = 0;
    uint64_t t0;

    if (elapsed_out != NULL)
        *elapsed_out = 0u;
    if (fd < 0)
        return -1;
    set_timeouts(fd);

    while (sent < len) {
        ssize_t n = send(fd, req + sent, len - sent, MSG_NOSIGNAL);

        if (n <= 0)
            goto out;
        sent += (size_t)n;
    }

    t0 = now_ms();
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
    if (elapsed_out != NULL)
        *elapsed_out = now_ms() - t0;

out:
    (void)close(fd);
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

/* 头区结束偏移（body 起点）；找不到返回 -1。 */
static long hdr_end_off(size_t total)
{
    size_t i;

    for (i = 0; i + 3u < total; i++) {
        if (memcmp(g_rsp + i, "\r\n\r\n", 4u) == 0)
            return (long)(i + 4u);
    }
    return -1;
}

static long rsp_body_len(size_t total)
{
    long off = hdr_end_off(total);

    return (off < 0) ? -1 : (long)((size_t)total - (size_t)off);
}

static const char *rsp_body(size_t total)
{
    long off = hdr_end_off(total);

    return (off < 0) ? NULL : (g_rsp + off);
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
 * 造测试文件（确定内容的伪随机流）
 * -------------------------------------------------------------------------- */
static int make_media_file(void)
{
    char  d1[300];
    char  d2[400];
    int   fd;
    size_t off = 0;
    size_t i;
    uint32_t x = 0x12345678u;

    (void)snprintf(d1, sizeof(d1), "%s/video", g_media);
    (void)snprintf(d2, sizeof(d2), "%s/video/20261014", g_media);
    (void)snprintf(g_file, sizeof(g_file), "%s/video/20261014/channel0_120000.mp4",
                   g_media);

    /* g_media 已由 mkdtemp() 建好，这里只补两层子目录。 */
    if (mkdir(d1, 0700) != 0)
        return -1;
    if (mkdir(d2, 0700) != 0)
        return -1;

    for (i = 0; i < PAYLOAD_LEN; i++) {
        x = x * 1103515245u + 12345u;
        g_payload[i] = (unsigned char)(x >> 16);
    }

    fd = open(g_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    while (off < PAYLOAD_LEN) {
        ssize_t n = write(fd, g_payload + off, PAYLOAD_LEN - off);

        if (n <= 0) {
            (void)close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    (void)close(fd);
    return 0;
}

/* 造 `<media>/snap/` 夹具（m14 用）：两张 jpg，mtime 差 1 秒、大小 100/200 B，
 * 外加一个**名字带空格**的文件 —— 列表只认 `[A-Za-z0-9._-]`（拼 JSON 前不做转义，
 * 用白名单代替），这种名字必须被跳过。 */
static int make_snap_fixture(void)
{
    char *paths[2];
    long  times[2];
    int   sizes[2];
    int   i;

    (void)snprintf(g_snap_dir, sizeof(g_snap_dir), "%s/snap", g_media);
    (void)snprintf(g_snap_a, sizeof(g_snap_a), "%s/snap_1700000001.jpg", g_snap_dir);
    (void)snprintf(g_snap_b, sizeof(g_snap_b), "%s/snap_1700000002.jpg", g_snap_dir);
    (void)snprintf(g_snap_bad, sizeof(g_snap_bad), "%s/we ird.jpg", g_snap_dir);

    if (mkdir(g_snap_dir, 0700) != 0)
        return -1;

    paths[0] = g_snap_a;
    paths[1] = g_snap_b;
    times[0] = 1700000001L;
    times[1] = 1700000002L;
    sizes[0] = 100;
    sizes[1] = 200;

    for (i = 0; i < 2; i++) {
        char    blob[200];
        int     fd;
        ssize_t n;

        memset(blob, 'J', sizeof(blob));
        fd = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
            return -1;
        n = write(fd, blob, (size_t)sizes[i]);
        (void)close(fd);
        if (n != (ssize_t)sizes[i])
            return -1;
        {
            struct utimbuf ut;

            ut.actime  = (time_t)times[i];
            ut.modtime = (time_t)times[i];
            if (utime(paths[i], &ut) != 0)
                return -1;
        }
    }

    {
        int fd = open(g_snap_bad, O_WRONLY | O_CREAT | O_TRUNC, 0600);

        if (fd < 0)
            return -1;
        if (write(fd, "X", 1) != 1) {
            (void)close(fd);
            return -1;
        }
        (void)close(fd);
    }
    return 0;
}

static void clean_media(void)
{
    char d1[300];
    char d2[400];

    (void)snprintf(d1, sizeof(d1), "%s/video", g_media);
    (void)snprintf(d2, sizeof(d2), "%s/video/20261014", g_media);
    if (g_file[0] != '\0')
        (void)unlink(g_file);
    (void)rmdir(d2);
    (void)rmdir(d1);
    if (g_snap_a[0] != '\0')
        (void)unlink(g_snap_a);
    if (g_snap_b[0] != '\0')
        (void)unlink(g_snap_b);
    if (g_snap_bad[0] != '\0')
        (void)unlink(g_snap_bad);
    if (g_snap_dir[0] != '\0')
        (void)rmdir(g_snap_dir);
    (void)rmdir(g_media);
}

/* -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    const char *bin = NULL;
    int         port;
    int         total;
    char        cookie[64];

    if (argc > 1)
        bin = argv[1];
    if (bin == NULL)
        bin = getenv("IV_WEB_BIN");
    if (bin == NULL)
        bin = "build/host/ivsbox-web";

    {
        struct stat st;

        if (stat(bin, &st) != 0) {
            printf("test_web_media_e2e SKIPPED: %s not found "
                   "(build with `make host`, or set IV_WEB_BIN)\n", bin);
            return 0;
        }
    }

    (void)snprintf(g_media, sizeof(g_media), "/tmp/ivwebmediaXXXXXX");
    {
        char *ok = mkdtemp(g_media);

        if (ok == NULL) {
            fprintf(stderr, "FAIL: mkdtemp: %s\n", strerror(errno));
            return 1;
        }
    }
    if (make_media_file() != 0) {
        fprintf(stderr, "FAIL: cannot create media fixture\n");
        clean_media();
        return 1;
    }
    if (make_snap_fixture() != 0) {
        fprintf(stderr, "FAIL: cannot create snapshot listing fixture\n");
        clean_media();
        return 1;
    }

    port = find_free_port();
    if (port <= 0) {
        fprintf(stderr, "FAIL: no free port\n");
        clean_media();
        return 1;
    }

    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGCHLD, on_sig);

    g_child = (pid_t)spawn_web(bin, port);
    if (g_child < 0) {
        fprintf(stderr, "FAIL: cannot fork ivsbox-web\n");
        clean_media();
        return 1;
    }

    /* ---------------- m01 未登录访问媒体：401 ---------------- */
    {
        int len = snprintf(g_req, sizeof(g_req),
                           "GET " MEDIA_REL " HTTP/1.1\r\nHost: x\r\n"
                           "Connection: close\r\n\r\n");

        total = exchange(port, g_req, (size_t)len, NULL);
        chk(total > 0 && rsp_status() == 401, "m01a: /media without session -> 401");
        chk(total > 0 && hdr_val((size_t)total, "Content-Length:", cookie, sizeof(cookie)),
            "m01b: 401 declares a body");
    }

    /* ---------------- m02 登录取会话 ---------------- */
    cookie[0] = '\0';
    {
        char body[64];
        int  blen = snprintf(body, sizeof(body), "{\"user\":\"admin\",\"pass\":\"testpass\"}");
        int  n = snprintf(g_req, sizeof(g_req),
                          "POST /api/login HTTP/1.1\r\nHost: x\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);

        memcpy(g_req + n, body, (size_t)blen);
        total = exchange(port, g_req, (size_t)n + (size_t)blen, NULL);
        chk(total > 0 && rsp_status() == 200, "m02a: login -> 200");
        {
            char ck[96];

            chk(hdr_val((size_t)total, "Set-Cookie:", ck, sizeof(ck)),
                "m02b: login sets a session cookie");
            if (strncmp(ck, "ivs_sid=", 8u) == 0) {
                size_t i = 8;
                size_t j = 0;

                while (ck[i] != '\0' && ck[i] != ';' && j + 1u < sizeof(cookie)) {
                    cookie[j++] = ck[i++];
                }
                cookie[j] = '\0';
            }
            chk(cookie[0] != '\0', "m02c: session token parsed");
        }
    }

    /* ---------------- m03 全量下载 + ?sha=1：200 + ETag + X-SHA256 ---------------- */
    {
        uint8_t dig[IV_SHA256_DIGEST_LEN];
        char    hex[IV_SHA256_HEX_LEN + 1u];
        char    got[80];
        char    clen[32];
        char    etag[64];
        int     len;

        chk(iv_sha256_file(g_file, dig) == 0, "m03a: local sha256 computed");
        iv_sha256_hex(dig, hex, sizeof(hex));

        len = snprintf(g_req, sizeof(g_req),
                       "GET " MEDIA_REL "?sha=1 HTTP/1.1\r\nHost: x\r\n"
                       "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);
        total = exchange(port, g_req, (size_t)len, NULL);
        chk(total > 0 && rsp_status() == 200, "m03b: full GET -> 200");
        chk(hdr_val((size_t)total, "Accept-Ranges:", got, sizeof(got)) &&
            strcmp(got, "bytes") == 0, "m03c: Accept-Ranges: bytes");
        chk(hdr_val((size_t)total, "ETag:", etag, sizeof(etag)),
            "m03d: ETag present");
        chk(hdr_val((size_t)total, "Content-Length:", clen, sizeof(clen)) &&
            strtol(clen, NULL, 10) == (long)PAYLOAD_LEN,
            "m03e: Content-Length equals file size");
        chk(hdr_val((size_t)total, "X-SHA256:", got, sizeof(got)) &&
            strcmp(got, hex) == 0, "m03f: X-SHA256 matches local digest");
        chk(rsp_body_len((size_t)total) == (long)PAYLOAD_LEN,
            "m03g: whole body delivered");
        chk(rsp_body((size_t)total) != NULL &&
            memcmp(rsp_body((size_t)total), g_payload, PAYLOAD_LEN) == 0,
            "m03h: body bytes identical to the fixture");
    }

    /* ---------------- m04 中段 Range：206 + Content-Range 边界 ---------------- */
    {
        char cr[80];
        int  len;

        len = snprintf(g_req, sizeof(g_req),
                       "GET " MEDIA_REL " HTTP/1.1\r\nHost: x\r\n"
                       "Range: bytes=1000-1999\r\n"
                       "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);
        total = exchange(port, g_req, (size_t)len, NULL);
        chk(total > 0 && rsp_status() == 206, "m04a: ranged GET -> 206");
        chk(hdr_val((size_t)total, "Content-Range:", cr, sizeof(cr)), "m04b: has Content-Range");
        {
            char want[80];

            (void)snprintf(want, sizeof(want), "bytes 1000-1999/%u", (unsigned)PAYLOAD_LEN);
            chk(strcmp(cr, want) == 0, "m04c: Content-Range start-end/size exactly right");
        }
        chk(rsp_body_len((size_t)total) == 1000, "m04d: 1000 bytes of body");
        chk(rsp_body((size_t)total) != NULL &&
            memcmp(rsp_body((size_t)total), g_payload + 1000, 1000u) == 0,
            "m04e: ranged slice matches the same offset in the fixture");
    }

    /* ---------------- m05 后缀 Range：-100 ⇒ 末尾 100 字节 ---------------- */
    {
        int len = snprintf(g_req, sizeof(g_req),
                           "GET " MEDIA_REL " HTTP/1.1\r\nHost: x\r\n"
                           "Range: bytes=-100\r\n"
                           "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);

        total = exchange(port, g_req, (size_t)len, NULL);
        chk(total > 0 && rsp_status() == 206, "m05a: suffix range -> 206");
        chk(rsp_body_len((size_t)total) == 100, "m05b: suffix range is 100 bytes");
        chk(rsp_body((size_t)total) != NULL &&
            memcmp(rsp_body((size_t)total), g_payload + (PAYLOAD_LEN - 100u), 100u) == 0,
            "m05c: suffix slice is the file tail");
    }

    /* ---------------- m06 起址越界：416 + Content-Range 星号/size ---------------- */
    {
        char cr[64];
        char req[256];
        int  len = snprintf(req, sizeof(req),
                            "GET " MEDIA_REL " HTTP/1.1\r\nHost: x\r\n"
                            "Range: bytes=%u-\r\n"
                            "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n",
                            (unsigned)(PAYLOAD_LEN + 10u), cookie);

        total = exchange(port, req, (size_t)len, NULL);
        chk(total > 0 && rsp_status() == 416, "m06a: start beyond EOF -> 416");
        chk(hdr_val((size_t)total, "Content-Range:", cr, sizeof(cr)), "m06b: 416 has Content-Range");
        {
            char want[64];

            (void)snprintf(want, sizeof(want), "bytes */%u", (unsigned)PAYLOAD_LEN);
            chk(strcmp(cr, want) == 0, "m06c: 416 Content-Range is bytes */size");
        }
    }

    /* ---------------- m07 If-Range 失配：退全量 200 ---------------- */
    {
        int len = snprintf(g_req, sizeof(g_req),
                           "GET " MEDIA_REL " HTTP/1.1\r\nHost: x\r\n"
                           "Range: bytes=1000-1999\r\n"
                           "If-Range: \"stale-etag\"\r\n"
                           "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);

        total = exchange(port, g_req, (size_t)len, NULL);
        chk(total > 0 && rsp_status() == 200, "m07a: stale If-Range falls back to 200");
        chk(rsp_body_len((size_t)total) == (long)PAYLOAD_LEN,
            "m07b: fallback delivers the whole file");
    }

    /* ---------------- m08 路径围栏：穿越一律 403 ---------------- */
    {
        static const char *pats[] = {
            "/media/../etc/passwd",
            "/media/video/../../etc/passwd",
            "/media/./../etc/passwd",
        };
        size_t i;

        for (i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
            int len = snprintf(g_req, sizeof(g_req),
                               "GET %s HTTP/1.1\r\nHost: x\r\n"
                               "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n",
                               pats[i], cookie);

            total = exchange(port, g_req, (size_t)len, NULL);
            chk(total > 0 && rsp_status() == 403, "m08a: traversal under /media -> 403");
            chk(total > 0 && strstr(g_rsp, "root:x:") == NULL,
                "m08b: traversal must not leak /etc/passwd");
        }
    }

    /* ---------------- m09 不存在：404（不是 500） ---------------- */
    {
        int len = snprintf(g_req, sizeof(g_req),
                           "GET /media/nope/20261014/channel9_000000.mp4 HTTP/1.1\r\n"
                           "Host: x\r\nCookie: ivs_sid=%s\r\nConnection: close\r\n\r\n",
                           cookie);

        total = exchange(port, g_req, (size_t)len, NULL);
        chk(total > 0 && rsp_status() == 404, "m09: missing media file -> 404");
    }

    /* ---------------- m13 第二道围栏：root 内的符号链接不得外泄 ----------------
     * 文件名 `evil.mp4` 完全合法（第一道逐段检查拦不住它），危险在链接目标。
     * 拦住它的是 `realpath()` 前缀复核（第二道）＋ `open(O_NOFOLLOW)`（第三道）。 */
    {
        char link[300];

        (void)snprintf(link, sizeof(link), "%s/evil.mp4", g_media);
        if (access("/etc/passwd", R_OK) == 0 && symlink("/etc/passwd", link) == 0) {
            int len = snprintf(g_req, sizeof(g_req),
                               "GET /media/evil.mp4 HTTP/1.1\r\nHost: x\r\n"
                               "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);

            total = exchange(port, g_req, (size_t)len, NULL);
            chk(total > 0 && rsp_status() == 403,
                "m13a: a symlink pointing outside the media root -> 403");
            chk(total > 0 && strstr(g_rsp, "root:x:") == NULL,
                "m13b: symlink must not leak /etc/passwd");
            (void)unlink(link);
        } else {
            printf("note: m13 skipped (cannot create the symlink fixture)\n");
        }
    }

    /* ---------------- m10 限速：全量下载必须被 bps 拖住 ---------------- */
    {
        uint64_t ms = 0u;
        int      len = snprintf(g_req, sizeof(g_req),
                                "GET " MEDIA_REL " HTTP/1.1\r\nHost: x\r\n"
                                "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);

        total = exchange(port, g_req, (size_t)len, &ms);
        chk(total > 0 && rsp_status() == 200, "m10a: throttled full GET -> 200");
        chk(rsp_body_len((size_t)total) == (long)PAYLOAD_LEN,
            "m10b: throttled download still delivers everything");
        /* 64 KiB @ 64 KiB/s ⇒ 至少 1 s；去掉限速的话是本机回环的毫秒级。
         * 下界只用来证伪"限速没接上"，不用于测量精度。 */
        chk(ms >= 500u, "m10c: transfer is actually throttled by media-bps");
    }

    /* ---------------- m11 取消：读一半就走，进程必须还活着 ---------------- */
    {
        int fd = connect_retry(port);

        chk(fd >= 0, "m11a: connect for the cancel case");
        if (fd >= 0) {
            int len = snprintf(g_req, sizeof(g_req),
                               "GET " MEDIA_REL " HTTP/1.1\r\nHost: x\r\n"
                               "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);

            set_timeouts(fd);
            (void)send(fd, g_req, (size_t)len, MSG_NOSIGNAL);
            (void)recv(fd, g_rsp, 1024, 0);   /* 只收一点点 */
            (void)close(fd);                  /* 中途取消 */
            sleep_ms(300);                    /* 让服务端在写失败里收尾 */
        }
        {
            int len = snprintf(g_req, sizeof(g_req),
                               "GET " MEDIA_REL " HTTP/1.1\r\nHost: x\r\n"
                               "Range: bytes=0-15\r\n"
                               "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);

            total = exchange(port, g_req, (size_t)len, NULL);
            chk(total > 0 && rsp_status() == 206,
                "m11b: server survives a mid-transfer cancel");
        }
    }

    /* ---------------- m12 fd 稳定性：反复下载不泄漏 ---------------- */
    {
        int before = count_fds(g_child);
        int after;
        int i;
        int ok = 1;

        for (i = 0; i < 20 && ok; i++) {
            int len = snprintf(g_req, sizeof(g_req),
                               "GET " MEDIA_REL " HTTP/1.1\r\nHost: x\r\n"
                               "Range: bytes=0-4095\r\n"
                               "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);

            total = exchange(port, g_req, (size_t)len, NULL);
            if (total <= 0 || rsp_status() != 206)
                ok = 0;
        }
        chk(ok, "m12a: 20 ranged downloads all answered 206");
        sleep_ms(300);
        after = count_fds(g_child);
        if (before > 0 && after > 0)
            chk(after == before, "m12b: no fd leak across media downloads");
    }

    /* ---------------- m14 目录列表（2026-10-28：Web 抓拍图片列表用） ----------------
     * 前端「抓拍」卡要列出 `/media/snap/` 里的图，所以 `/media/<目录>/` 必须回
     * JSON 而不是 403。这里钉住四件事：结构（ok/dir/n/items）、顺序（新的在前，
     * 前端拿第一条当大图）、白名单（名字带空格的跳过，绝不把需要转义的字节拼进
     * JSON）、以及"列目录不影响取文件"（同一个路径换成文件还得是 image/jpeg）。 */
    {
        const char *b;
        const char *p_old;
        const char *p_new;
        char        ct[64];
        int         len;

        len = snprintf(g_req, sizeof(g_req),
                       "GET /media/snap/ HTTP/1.1\r\nHost: x\r\n"
                       "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);
        total = exchange(port, g_req, (size_t)len, NULL);
        chk(total > 0 && rsp_status() == 200, "m14a: directory listing -> 200");
        chk(hdr_val((size_t)total, "Content-Type:", ct, sizeof(ct)) &&
            strstr(ct, "application/json") != NULL,
            "m14b: listing is JSON (not octet-stream)");
        b = rsp_body((size_t)total);
        chk(b != NULL && strstr(b, "\"ok\":true") != NULL, "m14c: listing body is ok:true");
        chk(b != NULL && strstr(b, "\"dir\":\"/snap/\"") != NULL,
            "m14d: echoes the requested directory");
        chk(b != NULL && strstr(b, "\"n\":2") != NULL,
            "m14e: n counts the two jpgs only (space name skipped)");
        chk(b != NULL && strstr(b, "snap_1700000001.jpg") != NULL, "m14f: older jpg listed");
        chk(b != NULL && strstr(b, "snap_1700000002.jpg") != NULL, "m14g: newer jpg listed");
        chk(b != NULL && strstr(b, "we ird.jpg") == NULL,
            "m14h: names outside [A-Za-z0-9._-] are skipped");
        chk(b != NULL && strstr(b, "\"s\":200") != NULL, "m14i: size is included");
        chk(b != NULL && strstr(b, "\"t\":1700000002") != NULL, "m14j: mtime is included");
        p_old = (b != NULL) ? strstr(b, "snap_1700000001.jpg") : NULL;
        p_new = (b != NULL) ? strstr(b, "snap_1700000002.jpg") : NULL;
        chk(p_old != NULL && p_new != NULL && p_new < p_old,
            "m14k: newest first (the UI uses items[0] as the big picture)");

        /* ?n=1 只回一条（前端可以按需少取） */
        len = snprintf(g_req, sizeof(g_req),
                       "GET /media/snap/?n=1 HTTP/1.1\r\nHost: x\r\n"
                       "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);
        total = exchange(port, g_req, (size_t)len, NULL);
        b = rsp_body((size_t)total);
        chk(total > 0 && rsp_status() == 200 && b != NULL && strstr(b, "\"n\":1") != NULL,
            "m14l: ?n=1 returns exactly one item");

        /* 同一路径换成文件：还是原来的图片下载（列目录没有抢走这条路） */
        len = snprintf(g_req, sizeof(g_req),
                       "GET /media/snap/snap_1700000002.jpg HTTP/1.1\r\nHost: x\r\n"
                       "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n", cookie);
        total = exchange(port, g_req, (size_t)len, NULL);
        chk(total > 0 && rsp_status() == 200 && rsp_body_len((size_t)total) == 200,
            "m14m: the jpg under the listed dir still downloads (200 bytes)");
        chk(hdr_val((size_t)total, "Content-Type:", ct, sizeof(ct)) &&
            strcmp(ct, "image/jpeg") == 0, "m14n: jpg keeps image/jpeg");
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
    clean_media();

    if (g_fail != 0) {
        fprintf(stderr, "test_web_media_e2e FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_web_media_e2e passed (auth, range 206/416, if-range, path fence, "
           "sha256, throttle, cancel, fd stability, dir listing)\n");
    return 0;
}
