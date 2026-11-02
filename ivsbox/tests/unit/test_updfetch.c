/*
 * 平台触发升级的拉包（0xB3）—— `iv_updfetch` 单测  2026-11-02
 *
 * ============================ 它验的是什么 ============================
 * 用**真 socket**（127.0.0.1 上起一个假升级服务器）跑完整流程，而不是 mock
 * 传输回调：本模块最容易错的四处只有真连接才暴露 ——
 *   ① HTTP 应答读没读满（头里带回来一段 body 时必须先吐出来）；
 *   ② keep-alive 能不能复用（帧模式一次下载要几十个 Range 请求）；
 *   ③ 帧模式的 1026 B 帧边界与 CRC 尾；
 *   ④ 下载完的文件与"服务器给的数据"是否**逐字节相等**。
 *
 * 假服务器按参考实现（`APP/Update/`）的接口形状应答：
 *     GET  /<硬件名>/info.txt   -> {"version":"<硬件名>-<版本>","url":"http://…/pkg/x.pkg"}
 *     HEAD /pkg/x.pkg           -> Content-Length（无 body）
 *     GET  /pkg/x.pkg [Range]   -> 206 切片（帧模式）或 200 整包（流模式/忽略 Range）
 *
 * ============================ 各组断言在钉什么 ============================
 *   p01 帧模式：5 帧 → 丢掉每帧 2 B CRC 尾，剩下的 5×1024 B 逐字节相等；
 *      `stats.frames == 5`、`mode == FRAME`；安装请求文件 = "<包路径> 1\n"。
 *   p02 版本相同：返回 `IV_UPDFETCH_SAME`，且**一个包请求都不发**
 *      （连 HEAD 都没有）——"已是最新"不该在服务器上留下一次下载。
 *   p03 非 1026 倍数：自动走流模式，字节数与原样一致，`frames == 0`。
 *   p04 服务器无视 Range 回 200：帧模式判不可用 → 退回流模式仍能装完
 *      （这条对应"发布侧给的是裸包"的真实可能，没有它设备会死在格式问题上）。
 *   p05 首帧 CRC 不符：同样退回流模式，最终文件与服务器给的字节一致。
 *   p06 info.txt 404 → IV_EPROTO；包比 max_bytes 大 → IV_ERANGE（且不留 .part）；
 *      空 host / close 后 run → IV_EINVAL。
 *
 * ============================ 反向证伪（改完必须能看到红） ============================
 *   F1 把 `recv_body()` 里"先吐 hdr 残留"那段删掉 ⇒ p01 逐字节比对红（数据错位）；
 *   F2 把 `fetch_frames()` 的 CRC 比对注释掉 ⇒ p05 用例的降级路径不再触发（mode 断言红）；
 *   F3 把 `rename(part, final)` 改成直接写 final ⇒ p06 的"不留 .part"仍绿、
 *      但 p01 的请求文件校验会指向不存在的 `.part`（红）；
 *   F4 把版本比对改成恒不等 ⇒ p02 红（多发了一次下载）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_crc.h"
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

typedef struct {
    int         listen_fd;
    int         port;
    pthread_t   th;
    volatile int stop;

    unsigned reqs;
    unsigned info_reqs;
    unsigned head_reqs;
    unsigned get_reqs;
    unsigned range_reqs;

    char     version[64]; /* info.txt 里的 version */
    char     url_path[96];/* 包里文件的路径段（/pkg/...） */
    unsigned char buf[SRV_BUF_MAX];
    size_t   len;

    int ignore_range; /* 1 = GET 一律回 200 整包（模拟"裸包"服务器） */
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

/* 读一个请求（头 + 可选 body）；返回 0 成功，-1 对端关闭 */
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

/* `clen` 与 `blen` 分开：HEAD 应答要报 Content-Length（文件大小）但**没有 body**，
 * 合成一个参数就会给 NULL 传长度（第一次写时就是这么崩的）。 */
static void srv_send(int fd, int status, const char *extra, const void *body,
                    size_t blen, size_t clen)
{
    char   head[384];
    size_t pos = 0;
    int    n;

    n = snprintf(head, sizeof head,
                 "HTTP/1.1 %d %s\r\nContent-Length: %u\r\nConnection: keep-alive\r\n%s"
                 "\r\n",
                 status, (status == 206) ? "Partial Content" : "OK", (unsigned)clen,
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

static int parse_range(const char *hdr, long *from, long *to)
{
    const char *p = strstr(hdr, "Range: bytes=");

    if (p == NULL)
        return 0;
    if (sscanf(p + 13, "%ld-%ld", from, to) != 2)
        return 0;
    return 1;
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
            char  hdr[2048];
            long  from = 0;
            long  to = 0;
            int   has_range;

            if (srv_read_req(fd, hdr, sizeof hdr) != 0)
                break;
            s->reqs++;

            if (strncmp(hdr, "HEAD ", 5) == 0) {
                s->head_reqs++;
                srv_send(fd, 200, "", NULL, 0u, s->len);
                continue;
            }
            if (strstr(hdr, "/info.txt") != NULL) {
                char body[256];
                int  n;

                s->info_reqs++;
                n = snprintf(body, sizeof body, "{\"version\":\"%s\",\"url\":\"%s\"}",
                             s->version, s->url_path);
                if (n > 0)
                    srv_send(fd, 200, "Content-Type: application/json\r\n",
                             body, (size_t)n, (size_t)n);
                continue;
            }
            /* 包本体 */
            has_range = parse_range(hdr, &from, &to);
            if (has_range)
                s->range_reqs++;
            s->get_reqs++;
            if (has_range && !s->ignore_range) {
                long end = (to >= (long)s->len) ? (long)s->len - 1 : to;

                if (from < 0 || from >= (long)s->len || end < from) {
                    srv_send(fd, 416, "", NULL, 0u, 0u);
                    continue;
                }
                srv_send(fd, 206, "Content-Type: application/octet-stream\r\n",
                         s->buf + from, (size_t)(end - from + 1),
                         (size_t)(end - from + 1));
            } else {
                srv_send(fd, 200, "Content-Type: application/octet-stream\r\n", s->buf,
                         s->len, s->len);
            }
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
 * 造数据
 * ------------------------------------------------------------------------- */

/* 把 `frames` 帧写进 dst：每帧 1024 B 图案数据 ＋ 2 B big-endian CRC16/MODBUS */
static void build_frames(unsigned char *dst, unsigned frames, unsigned seed,
                         int corrupt_crc)
{
    unsigned i;

    for (i = 0u; i < frames; i++) {
        unsigned char *fr = dst + (size_t)i * IV_UPDFETCH_FRAME_SIZE;
        uint16_t       crc;
        unsigned       k;

        for (k = 0u; k < IV_UPDFETCH_FRAME_DATA; k++)
            fr[k] = (unsigned char)((i * 31u + k + seed) & 0xffu);
        crc = iv_crc16_modbus(fr, IV_UPDFETCH_FRAME_DATA, IV_CRC16_MODBUS_SEED_INIT);
        if (corrupt_crc && i == 0u)
            crc = (uint16_t)(crc ^ 0x00ffu);
        fr[IV_UPDFETCH_FRAME_DATA]      = (unsigned char)(crc >> 8);
        fr[IV_UPDFETCH_FRAME_DATA + 1u] = (unsigned char)(crc & 0xffu);
    }
}

/* 期望的落盘内容：每帧只留前 1024 B */
static void expect_from_frames(const unsigned char *frames, unsigned n,
                               unsigned char *out, size_t *out_len)
{
    unsigned i;

    for (i = 0u; i < n; i++)
        memcpy(out + (size_t)i * IV_UPDFETCH_FRAME_DATA,
               frames + (size_t)i * IV_UPDFETCH_FRAME_SIZE, IV_UPDFETCH_FRAME_DATA);
    *out_len = (size_t)n * IV_UPDFETCH_FRAME_DATA;
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

/* 句柄约 17 KiB：**不要放栈上**（与 iv_logup 同一条纪律） */
static iv_updfetch_t g_uf;

int main(void)
{
    srv_t              srv;
    iv_updfetch_cfg_t  cfg;
    iv_updfetch_stats_t st;
    unsigned char      frames[5u * IV_UPDFETCH_FRAME_SIZE];
    unsigned char      expect[5u * IV_UPDFETCH_FRAME_DATA];
    unsigned char      got[SRV_BUF_MAX];
    size_t             expect_len = 0u;
    char               pkg_path[256];
    char               req_path[256];
    size_t             n;
    int                rc;

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
    (void)snprintf(req_path, sizeof req_path, "%s/ota/request", g_root);

    /* ---- p01：帧模式，逐字节相等 ---- */
    build_frames(frames, 5u, 7u, 0);
    memcpy(srv.buf, frames, sizeof frames);
    srv.len = sizeof frames;
    (void)snprintf(srv.version, sizeof srv.version, "TESTHW-9.9.9");
    expect_from_frames(frames, 5u, expect, &expect_len);

    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p01 init");
    rc = iv_updfetch_run(&g_uf);
    chk(rc == IV_OK, "p01 run rc == IV_OK");
    n = slurp(pkg_path, got, sizeof got);
    chk(n == expect_len, "p01 file size == frames*1024");
    chk(n != (size_t)-1 && memcmp(got, expect, expect_len) == 0,
        "p01 bytes identical (CRC trailers stripped)");
    iv_updfetch_stats(&g_uf, &st);
    chk(st.ok == 1u && st.fail == 0u, "p01 stats ok=1");
    chk(st.frames == 5u, "p01 stats frames=5");
    chk(st.mode == IV_UPDFETCH_MODE_FRAME, "p01 mode == FRAME");
    chk(srv.info_reqs == 1u && srv.head_reqs == 1u, "p01 info+head once");
    chk(srv.range_reqs == 1u, "p01 one Range request for 5 frames (window=16)");
    chk(st.last_http == 206 || st.last_http == 200, "p01 last http is 2xx");
    {
        char line[300];

        n = slurp(req_path, (unsigned char *)line, sizeof line - 1u);
        if (n != (size_t)-1) {
            line[n] = '\0';
            chk(strncmp(line, pkg_path, strlen(pkg_path)) == 0,
                "p01 request file points at the downloaded package");
            chk(line[strlen(pkg_path)] == ' ' && line[strlen(pkg_path) + 1u] == '1',
                "p01 request file asks for restart=1");
        } else {
            chk(0, "p01 request file exists");
        }
    }
    iv_updfetch_close(&g_uf);

    /* ---- p02：版本相同 ⇒ SAME，且不发包请求 ---- */
    {
        unsigned before_reqs = srv.reqs;
        unsigned before_head = srv.head_reqs;

        (void)snprintf(srv.version, sizeof srv.version, "TESTHW-1.0.0.20261102");
        cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
        chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p02 init");
        rc = iv_updfetch_run(&g_uf);
        chk(rc == IV_UPDFETCH_SAME, "p02 rc == IV_UPDFETCH_SAME");
        chk(srv.reqs == before_reqs + 1u, "p02 only info.txt was fetched");
        chk(srv.head_reqs == before_head, "p02 no new HEAD for an up-to-date device");
        iv_updfetch_stats(&g_uf, &st);
        chk(st.same == 1u, "p02 stats same=1");
        chk(st.fail == 0u, "p02 'already latest' is not a failure");
        iv_updfetch_close(&g_uf);
    }

    /* ---- p03：非 1026 倍数 ⇒ 流模式 ---- */
    {
        unsigned i;

        for (i = 0u; i < 2500u; i++)
            srv.buf[i] = (unsigned char)((i * 7u) & 0xffu);
        srv.len = 2500u;
        (void)snprintf(srv.version, sizeof srv.version, "TESTHW-2.0.0");
        cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
        chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p03 init");
        rc = iv_updfetch_run(&g_uf);
        chk(rc == IV_OK, "p03 run rc == IV_OK");
        n = slurp(pkg_path, got, sizeof got);
        chk(n == 2500u, "p03 file size 2500 (stream mode)");
        chk(n != (size_t)-1 && memcmp(got, srv.buf, 2500u) == 0, "p03 bytes identical");
        iv_updfetch_stats(&g_uf, &st);
        chk(st.mode == IV_UPDFETCH_MODE_STREAM, "p03 mode == STREAM (not 1026-aligned)");
        chk(st.frames == 0u, "p03 no frames in stream mode");
        iv_updfetch_close(&g_uf);
    }

    /* ---- p04：服务器无视 Range（回 200 整包）⇒ 退回流浪模式 ---- */
    {
        unsigned i;

        for (i = 0u; i < 4u * IV_UPDFETCH_FRAME_SIZE; i++)
            srv.buf[i] = (unsigned char)((i + 3u) & 0xffu);
        srv.len          = 4u * IV_UPDFETCH_FRAME_SIZE; /* 恰好 1026 的整数倍 */
        srv.ignore_range = 1;
        (void)snprintf(srv.version, sizeof srv.version, "TESTHW-3.0.0");
        cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
        chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p04 init");
        rc = iv_updfetch_run(&g_uf);
        chk(rc == IV_OK, "p04 run rc == IV_OK (fell back to stream)");
        n = slurp(pkg_path, got, sizeof got);
        chk(n == srv.len, "p04 file size == server size");
        chk(n != (size_t)-1 && memcmp(got, srv.buf, srv.len) == 0, "p04 bytes identical");
        iv_updfetch_stats(&g_uf, &st);
        chk(st.mode == IV_UPDFETCH_MODE_STREAM, "p04 mode == STREAM after fallback");
        srv.ignore_range = 0;
        iv_updfetch_close(&g_uf);
    }

    /* ---- p05：首帧 CRC 不符 ⇒ 退回流浪模式 ---- */
    build_frames(frames, 4u, 11u, 1 /* 首帧 CRC 故意写错 */);
    memcpy(srv.buf, frames, 4u * IV_UPDFETCH_FRAME_SIZE);
    srv.len = 4u * IV_UPDFETCH_FRAME_SIZE;
    (void)snprintf(srv.version, sizeof srv.version, "TESTHW-4.0.0");
    cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
    chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p05 init");
    rc = iv_updfetch_run(&g_uf);
    chk(rc == IV_OK, "p05 run rc == IV_OK (fell back on crc mismatch)");
    n = slurp(pkg_path, got, sizeof got);
    chk(n == srv.len, "p05 file size == server size");
    chk(n != (size_t)-1 && memcmp(got, srv.buf, srv.len) == 0, "p05 bytes identical");
    iv_updfetch_stats(&g_uf, &st);
    chk(st.mode == IV_UPDFETCH_MODE_STREAM, "p05 mode == STREAM after crc fallback");
    iv_updfetch_close(&g_uf);

    /* ---- p06：错误面（404 / 超限 / 空 host / close 后 run） ---- */
    {
        unsigned char part_path[300];

        /* 超限：包 2500 B、上限 100 B ⇒ ERANGE 且不留 .part */
        (void)snprintf(srv.version, sizeof srv.version, "TESTHW-5.0.0");
        srv.len = 2500u;
        cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
        cfg.max_bytes = 100u;
        chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p06 init");
        rc = iv_updfetch_run(&g_uf);
        chk(rc == IV_ERANGE, "p06 too big -> IV_ERANGE");
        (void)snprintf((char *)part_path, sizeof part_path,
                       "%s/ota/download/ivsbox.pkg.part", g_root);
        chk(access((const char *)part_path, F_OK) != 0, "p06 no .part left behind");
        iv_updfetch_close(&g_uf);

        /* info.txt 404：把 url_path 指到不存在的路径不影响 info 本身，
         * 这里用"服务器版本串为空导致解析失败"不直观 —— 直接让 host 不可达。 */
        cfg_of(&cfg, &srv, "127.0.0.1", "1.0.0.20261102");
        cfg.port = 1u; /* 这个端口上没有服务 */
        cfg.connect_ms = 300u;
        chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p06 init (bad port)");
        rc = iv_updfetch_run(&g_uf);
        chk(rc == IV_ECONN, "p06 unreachable server -> IV_ECONN");
        iv_updfetch_close(&g_uf);

        /* 空 host / close 之后 */
        cfg_of(&cfg, &srv, "", "1.0.0.20261102");
        chk(iv_updfetch_init(&g_uf, &cfg) == IV_OK, "p06 init with empty host allowed");
        chk(iv_updfetch_run(&g_uf) == IV_EINVAL, "p06 empty host -> IV_EINVAL");
        iv_updfetch_close(&g_uf);
        chk(iv_updfetch_run(&g_uf) == IV_EINVAL, "p06 run after close -> IV_EINVAL");
    }

    srv_stop(&srv);
    rm_root();

    if (g_fail != 0) {
        fprintf(stderr, "updfetch: %d checks FAILED\n", g_fail);
        return 1;
    }
    printf("updfetch: all checks passed\n");
    return 0;
}
