/*
 * iv_media 单测（libivmodules，功能开发计划 M6-S6.1）
 *
 * 对桥接打**真实回环 TCP**：用例里起一个假 bird_media（127.0.0.1 随机端口，
 * 按请求行给罐头响应），覆盖七组：
 *   c01 record start/stop/status 往返（真实 HTTP 应答）
 *   c02 media 进程不在（连不上的端口）⇒ IV_ECONN，不致命
 *   c03 抓拍自动命名入队 → 服务端落文件 ⇒ snap_ok、文件真实存在
 *   c04 服务端 200 但没落文件 ⇒ snap_fail（对端"假成功"要能识破）
 *   c05 队列深度 2：worker 忙时连入 4 个 ⇒ 丢最旧一个（snap_drop>=1），其余照做
 *   c06 保活轮询同步对端状态：对端答 0 ⇒ 会话表被清（bird_media 15 min
 *       超时停录的镜像场景，用 200ms 周期缩短验证）
 *   c07 未 init 的句柄一律 IV_ESTATE（含 fini 幂等）
 *   c08 id/参数防御
 *
 * 反向证伪（本用例内建）：
 *   f1 若桥接把 HTTP 200 直接当抓拍成功（不 stat 文件），c04 会红；
 *   f2 若会话表只加不清（不随对端状态同步），c06 会红；
 *   f3 若 record 静默吞掉 ECONN（假装成功），c02 会红。
 */
#include <arpa/inet.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_media.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void noop_sink(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

/* ============================ 假 bird_media 服务端 ============================ */

static atomic_int g_srv_rec[8];   /* tsan requires atomic */
static atomic_int g_srv_delay_ms; /* tsan requires atomic */
static atomic_int g_srv_write_file; /* tsan requires atomic */
static atomic_int g_srv_listen; /* tsan requires atomic; -1 = closed */
static pthread_t g_srv_tid;
static atomic_int g_srv_run;   /* tsan requires atomic */

static int srv_create_file(const char *path)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL)
        return -1;
    fputs("FAKEJPEG", f);
    fclose(f);
    return 0;
}

/* query 里找 name= 的值并 %XX 解码（桥接侧是编码过的） */
static void srv_decode_param(const char *query, const char *name,
                             char *out, size_t cap)
{
    const char *p = strstr(query, name);
    size_t      oi = 0;

    out[0] = '\0';
    if (p == NULL)
        return;
    p += strlen(name);
    if (*p != '=')
        return;
    p++;
    while (*p != '\0' && *p != '&' && oi + 1u < cap) {
        if (p[0] == '%' && p[1] != '\0' && p[2] != '\0') {
            char hex[3] = { p[1], p[2], 0 };
            out[oi++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';
}

static void srv_handle(int fd, const char *req)
{
    const char *line_end = strstr(req, "\r\n");
    char        line[512];
    char        uri[256] = { 0 };
    const char *sp;
    const char *sp2;
    char        body[64];
    const char *resp_body = "OK";

    if (line_end == NULL)
        return;
    (void)snprintf(line, sizeof(line), "%.*s", (int)(line_end - req), req);
    sp = strchr(line, ' ');
    if (sp == NULL)
        return;
    sp2 = strchr(sp + 1, ' ');
    if (sp2 == NULL)
        return;
    (void)snprintf(uri, sizeof(uri), "%.*s", (int)(sp2 - sp - 1), sp + 1);

    if (strncmp(uri, "/capture", 8) == 0) {
        char path[512];

        srv_decode_param(uri, "path", path, sizeof(path));
        if (atomic_load(&g_srv_delay_ms) > 0) {
            struct timespec ts;
            ts.tv_sec  = atomic_load(&g_srv_delay_ms) / 1000;
            ts.tv_nsec = (long)(g_srv_delay_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
        if (atomic_load(&g_srv_write_file) && path[0] != '\0')
            (void)srv_create_file(path);
        resp_body = "OK";
    } else if (strncmp(uri, "/record/start", 13) == 0) {
        {
            char ids[16];
            srv_decode_param(uri, "id", ids, sizeof(ids));
            {
                int id = atoi(ids);
                if (id >= 0 && id < 8)
                    atomic_store(&g_srv_rec[id], 1);
            }
        }
        resp_body = "OK";
    } else if (strncmp(uri, "/record/stop", 12) == 0) {
        {
            char ids[16];
            srv_decode_param(uri, "id", ids, sizeof(ids));
            {
                int id = atoi(ids);
                if (id >= 0 && id < 8)
                    atomic_store(&g_srv_rec[id], 0);
            }
        }
        resp_body = "OK";
    } else if (strncmp(uri, "/record/status", 14) == 0) {
        char ids[16];
        int  id = 0;

        srv_decode_param(uri, "id", ids, sizeof(ids));
        id = atoi(ids);
        if (id >= 0 && id < 8)
            resp_body = (atomic_load(&g_srv_rec[id]) != 0) ? "1" : "0";
        else
            resp_body = "0";
    } else {
        (void)snprintf(body, sizeof(body), "Not found");
        {
            char hdr[128];
            int  n = snprintf(hdr, sizeof(hdr),
                              "HTTP/1.0 404 Not Found\r\nContent-Length: %zu\r\n"
                              "Connection: close\r\n\r\n%s", strlen(body), body);
            (void)!write(fd, hdr, (size_t)n);
        }
        return;
    }

    {
        char hdr[128];
        int  n = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n"
                          "Connection: close\r\n\r\n%s",
                          strlen(resp_body), resp_body);
        (void)!write(fd, hdr, (size_t)n);
    }
}

static void *srv_main(void *arg)
{
    (void)arg;
    while (atomic_load(&g_srv_run)) {
        int cfd = accept(atomic_load(&g_srv_listen), NULL, NULL);

        if (cfd < 0)
            continue;
        {
            char    req[1024];
            size_t  got = 0;
            for (;;) {
                ssize_t r = recv(cfd, req + got, sizeof(req) - 1u - got, 0);
                if (r <= 0)
                    break;
                got += (size_t)r;
                req[got] = '\0';
                if (strstr(req, "\r\n\r\n") != NULL || got + 1u >= sizeof(req))
                    break;
            }
            if (got > 0)
                srv_handle(cfd, req);
        }
        close(cfd);
    }
    return NULL;
}

/* 起服务端；成功时把端口写 *port_out */
static int srv_start(int *port_out)
{
    struct sockaddr_in sa;
    socklen_t          slen = sizeof(sa);

    atomic_store(&g_srv_listen, socket(AF_INET, SOCK_STREAM, 0));
    if (atomic_load(&g_srv_listen) < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    if (bind(atomic_load(&g_srv_listen), (struct sockaddr *)&sa, sizeof(sa)) != 0)
        return -1;
    if (getsockname(atomic_load(&g_srv_listen), (struct sockaddr *)&sa, &slen) != 0)
        return -1;
    *port_out = (int)ntohs(sa.sin_port);
    if (listen(atomic_load(&g_srv_listen), 8) != 0)
        return -1;
    atomic_store(&g_srv_run, 1);
    return pthread_create(&g_srv_tid, NULL, srv_main, NULL) == 0 ? 0 : -1;
}

static void srv_stop(void)
{
    if (atomic_load(&g_srv_listen) >= 0) {
        atomic_store(&g_srv_run, 0);
        shutdown(atomic_load(&g_srv_listen), SHUT_RDWR);
        close(atomic_load(&g_srv_listen));
        atomic_store(&g_srv_listen, -1);
        pthread_join(g_srv_tid, NULL);
    }
}

/* 桥接句柄 + 目录 */
static iv_media_t g_m;
static char       g_dir[160];

/* 轮询等待（ms 内条件成立则 0） */
static int wait_for(int (*cond)(void *), void *arg, int ms)
{
    int waited = 0;

    while (!cond(arg)) {
        if (waited >= ms)
            return -1;
        usleep(20000);
        waited += 20;
    }
    return 0;
}

/* ============ 条件谓词（wait_for 用） ============ */
typedef struct {
    const iv_media_t *m;
    uint64_t          want_ok;
    uint64_t          want_done; /* ok+fail */
} snap_cond_t;

static int cond_snap_ok(void *arg)
{
    const snap_cond_t *c = arg;
    iv_media_stats_t   st;

    iv_media_stats(c->m, &st);
    return st.snap_ok >= c->want_ok;
}

static int cond_snap_done(void *arg)
{
    const snap_cond_t *c = arg;
    iv_media_stats_t   st;

    iv_media_stats(c->m, &st);
    return (st.snap_ok + st.snap_fail) >= c->want_done;
}

static int cond_sessions_zero(void *arg)
{
    return iv_media_sessions((const iv_media_t *)arg, NULL, NULL, 0) == 0;
}

int main(void)
{
    iv_media_cfg_t  cfg;
    char            tmpl[] = "/tmp/ivmedia_XXXXXX";
    int             port = 0;
    iv_media_stats_t st;

    iv_log_set_sink(noop_sink, NULL);
    atomic_store(&g_srv_write_file, 1); /* explicit init */

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    (void)snprintf(g_dir, sizeof(g_dir), "%s", tmpl);

    if (srv_start(&port) != 0) {
        fprintf(stderr, "fake media server failed\n");
        return 1;
    }

    /* ---------------- c01 record 往返 ---------------- */
    iv_media_cfg_default(&cfg);
    cfg.port     = port;
    cfg.snap_dir = g_dir; /* 自动命名落进临时目录（VM 上没有 /mnt/sdcard） */
    chk(iv_media_init(&g_m, &cfg) == IV_OK, "c01: init ok");
    /* 2026-10-28：start 前会真连一次 host:port 做预检 ⇒ 地址必须指向**本测试的
     * 假服务**（127.0.0.1:port）；沿用 `rtsp://10.0.0.11:554` 在 VM 上必然 500 ms
     * 超时被预检拦下，用例会红在"start accepted"这一行。 */
    {
        char rurl[96];

        (void)snprintf(rurl, sizeof(rurl), "rtsp://127.0.0.1:%d/ch0", port);
        chk(iv_media_record_start(&g_m, 0, rurl, NULL, 0) == IV_OK,
            "c01: start accepted");
    }
    {
        int rec = -1;
        chk(iv_media_record_status(&g_m, 0, &rec) == IV_OK && rec == 1,
            "c01: status reports recording");
    }
    chk(iv_media_record_stop(&g_m, 0) == IV_OK, "c01: stop accepted");
    {
        int rec = -1;
        chk(iv_media_record_status(&g_m, 0, &rec) == IV_OK && rec == 0,
            "c01: peer reports stopped after stop");
    }
    {
        int n = iv_media_sessions(&g_m, NULL, NULL, 0);
        chk(n == 0, "c01: session cleared after stop");
    }

    /* ---------------- c02 media 不在 ⇒ IV_ECONN（f3） ---------------- */
    {
        iv_media_t m2;
        int        dead_port;

        /* 占一个端口再关掉：拿到一个"确定没人听"的端口 */
        {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in sa;
            socklen_t slen = sizeof(sa);
            memset(&sa, 0, sizeof(sa));
            sa.sin_family      = AF_INET;
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            sa.sin_port        = 0;
            (void)!bind(s, (struct sockaddr *)&sa, sizeof(sa));
            (void)!listen(s, 1);
            (void)!getsockname(s, (struct sockaddr *)&sa, &slen);
            dead_port = (int)ntohs(sa.sin_port);
            close(s);
        }
        memset(&m2, 0, sizeof(m2));
        cfg.port = dead_port;
        chk(iv_media_init(&m2, &cfg) == IV_OK, "c02: init ok (dead port)");
        {
            /* 预检走**活的**假服务（否则先红在预检、测不到"媒体不可达"），
             * 真正的 ECONN 由 m2 的 cfg.port 指向死端口产生。 */
            char rurl[96];

            (void)snprintf(rurl, sizeof(rurl), "rtsp://127.0.0.1:%d/ch0", port);
            chk(iv_media_record_start(&m2, 0, rurl, NULL, 0) == IV_ECONN,
                "c02: start reports ECONN (f3)");
        }
        chk(iv_media_record_stop(&m2, 0) == IV_ECONN, "c02: stop reports ECONN");
        {
            int rec = -1;
            chk(iv_media_record_status(&m2, 0, &rec) == IV_ECONN,
                "c02: status reports ECONN");
        }
        iv_media_fini(&m2);
    }

    /* ---------------- c03 抓拍自动命名 → 落盘成功 ---------------- */
    cfg.port = port;
    {
        char out[IV_MEDIA_URL_MAX];

        chk(iv_media_capture_async(&g_m, NULL, out, sizeof(out)) == IV_OK,
            "c03: capture enqueued");
        chk(strncmp(out, g_dir, strlen(g_dir)) == 0, "c03: auto path uses snap_dir");
        {
            snap_cond_t c = { &g_m, 1, 0 };
            chk(wait_for(cond_snap_ok, &c, 3000) == 0, "c03: snap_ok observed");
        }
        {
            FILE *f = fopen(out, "rb");
            chk(f != NULL, "c03: file really on disk");
            if (f != NULL)
                fclose(f);
        }
    }

    /* ---------------- c04 200 但没落文件 ⇒ snap_fail（f1） ---------------- */
    {
        char path[IV_MEDIA_URL_MAX];

        snprintf(path, sizeof(path), "%s/never_written.jpg", g_dir);
        atomic_store(&g_srv_write_file, 0);
        chk(iv_media_capture_async(&g_m, path, NULL, 0) == IV_OK,
            "c04: capture enqueued");
        {
            snap_cond_t c = { &g_m, 0, 2 }; /* ok+fail 总数到 2 */
            chk(wait_for(cond_snap_done, &c, 3000) == 0, "c04: handled");
        }
        iv_media_stats(&g_m, &st);
        chk(st.snap_fail >= 1, "c04: counted as failure (f1)");
        atomic_store(&g_srv_write_file, 1);
    }

    /* ---------------- c05 队列深度 2：满丢最旧 ---------------- */
    {
        char p0[IV_MEDIA_URL_MAX];
        char p1[IV_MEDIA_URL_MAX];
        char p2[IV_MEDIA_URL_MAX];
        char p3[IV_MEDIA_URL_MAX];

        atomic_store(&g_srv_delay_ms, 400); /* 拖住 worker，让它出不了队列 */
        snprintf(p0, sizeof(p0), "%s/q0.jpg", g_dir);
        snprintf(p1, sizeof(p1), "%s/q1.jpg", g_dir);
        snprintf(p2, sizeof(p2), "%s/q2.jpg", g_dir);
        snprintf(p3, sizeof(p3), "%s/q3.jpg", g_dir);
        chk(iv_media_capture_async(&g_m, p0, NULL, 0) == IV_OK, "c05: q0");
        usleep(120000); /* worker 已把 q0 取走、正卡在 HTTP 里 */
        chk(iv_media_capture_async(&g_m, p1, NULL, 0) == IV_OK, "c05: q1");
        chk(iv_media_capture_async(&g_m, p2, NULL, 0) == IV_OK, "c05: q2");
        chk(iv_media_capture_async(&g_m, p3, NULL, 0) == IV_OK, "c05: q3 -> drop q1");
        usleep(50000); /* 第四个请求此刻应已触发丢弃 */
        iv_media_stats(&g_m, &st);
        chk(st.snap_drop >= 1, "c05: oldest dropped");
        atomic_store(&g_srv_delay_ms, 0);
        {
            snap_cond_t c = { &g_m, 0, 5 }; /* c03(1)+c04(1)+c05 处理 3 个（q1 被丢）＝5 */
            chk(wait_for(cond_snap_done, &c, 6000) == 0, "c05: backlog drained");
        }
        iv_media_stats(&g_m, &st);
        chk(st.snap_req == 6, "c05: request counter matches enqueues");
    }

    /* ---------------- c06 保活同步对端状态（f2） ---------------- */
    {
        iv_media_t m3;

        memset(&m3, 0, sizeof(m3));
        iv_media_cfg_default(&cfg);
        cfg.port         = port;
        cfg.ka_period_ms = 200; /* 缩短验证周期 */
        chk(iv_media_init(&m3, &cfg) == IV_OK, "c06: init ok");
        {
            char rurl[96];

            (void)snprintf(rurl, sizeof(rurl), "rtsp://127.0.0.1:%d/ch3", port);
            chk(iv_media_record_start(&m3, 3, rurl, NULL, 0) == IV_OK,
                "c06: session started");
        }
        chk(iv_media_sessions(&m3, NULL, NULL, 0) == 1, "c06: one active session");
        atomic_store(&g_srv_rec[3], 0); /* 对端 RTSP 断流自动停录后的镜像 */
        chk(wait_for(cond_sessions_zero, &m3, 3000) == 0,
            "c06: keepalive synced peer stop into session table (f2)");
        iv_media_fini(&m3);
    }

    /* ---------------- c07 未 init 的句柄一律 ESTATE ---------------- */
    {
        iv_media_t m4;

        memset(&m4, 0, sizeof(m4));
        chk(iv_media_record_start(&m4, 0, "rtsp://x", NULL, 0) == IV_ESTATE,
            "c07: not inited -> ESTATE");
        chk(iv_media_capture_async(&m4, NULL, NULL, 0) == IV_ESTATE,
            "c07: capture not inited -> ESTATE");
        iv_media_fini(&m4); /* 幂等 */
    }

    /* ---------------- c08 id/参数防御 ---------------- */
    chk(iv_media_record_start(&g_m, -1, "rtsp://x", NULL, 0) == IV_EINVAL,
        "c08: bad id lo");
    chk(iv_media_record_start(&g_m, IV_MEDIA_CH_N, "rtsp://x", NULL, 0) == IV_EINVAL,
        "c08: bad id hi");
    chk(iv_media_record_start(&g_m, 0, "", NULL, 0) == IV_EINVAL, "c08: empty url");

    iv_media_fini(&g_m);
    srv_stop();

    if (g_fail != 0) {
        fprintf(stderr, "test_media FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_media passed (record round-trip, capture ok/fail, queue drop, keepalive sync)\n");
    return 0;
}
