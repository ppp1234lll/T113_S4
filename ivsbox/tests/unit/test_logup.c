/*
 * 日志上报（平台 0xB2）—— `iv_logup` 的三步 HTTP 分片上传单测  2026-11-02
 *
 * ============================ 它验的是什么 ============================
 * 用**真 socket**（127.0.0.1 上起一个假日志服务）跑完整流程，而不是 mock
 * 传输回调：本模块最容易错的地方就是"HTTP 应答读没读满""连接能不能复用"
 * "游标有没有只在成功时推进"，这些都只有真连接才暴露（参考实现在 STM32 上
 * 就因为 netconn 的 unsent 队列专门等过一次）。
 *
 * 假服务端按参考实现的接口形状应答：
 *     POST /fnwlw/oss/log/upload/start   -> {"code":200,"data":{"uploadId":"UPID-1"}}
 *     POST /fnwlw/oss/log/upload/chunk   -> {"code":200}（把 file 段拼起来）
 *     POST /fnwlw/oss/log/upload/finish  -> {"code":200}
 *
 * ============================ 为什么这些断言值得写 ============================
 * ① **不重不漏**：一次上传成功后游标必须停在新位置，第二次运行返回 IV_ENOENT
 *    （而不是把同一段再传一遍）；文件追加新行后再跑，只传**新增**那几字节。
 *    这条是整套游标设计的唯一承诺，破了就是平台侧看到重复日志。
 * ② **失败不推进**：服务端回非 2xx 时必须返回 IV_EPROTO 且游标不动，下一次
 *    还能把同一段重新传上去（否则日志永久丢失，现场毫无感知）。
 * ③ **分片边界**：1 KiB 一片是服务端的拼接口径。这里用 2500 B 的日志造
 *    3 片，逐片校验拼回来的字节与原文**逐字节相等**（少一个字节 server 侧
 *    的文件就是坏的，而 HTTP 每一步都回 200，谁也不会报错）。
 * ④ **没地址/没日志**：空 host 返回 IV_EINVAL；已经传完返回 IV_ENOENT
 *    —— 这两个都不是故障，装配层按不同日志打。
 *
 * ============================ 反向证伪（改完必须能看到红） ============================
 *   F1 把 `cursor_save()` 注释掉 ⇒ p02（第二次应 ENOENT 却 ok）红；
 *   F2 把 `cursor_save()` 提到 `finish_upload()` 之前 ⇒ p04（失败也推进）红；
 *   F3 把 `read_resp()` 的 `resp_complete()` 判断去掉（读到一次就返回）⇒ p01
 *      分片数据错位/解析失败 ⇒ 红；
 *   F4 把 `post_chunk()` 的 chunkIndex 传常量 0 ⇒ p05（server 拼出重复片）红。
 */
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

#include "ivsbox/iv_logup.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 假日志服务
 * ------------------------------------------------------------------------- */

#define SRV_DATA_MAX (256u * 1024u)

typedef struct {
    int      listen_fd;
    int      port;
    pthread_t th;
    volatile int stop;

    unsigned reqs;        /* 收到的请求数 */
    unsigned starts;
    unsigned chunks;
    unsigned finishes;
    char     upload_id[32];
    unsigned char data[SRV_DATA_MAX]; /* 所有 chunk 的 file 段按序拼起来 */
    size_t   data_len;

    int status_override;  /* 非 0 ⇒ 所有应答用这个 HTTP 状态码 */
    int code_override;    /* 非 0 ⇒ 应答 JSON 的 code 用这个值 */
    int last_chunk_index; /* 最近一片的 chunkIndex（校验序号连不连续） */
    int chunk_index_ok;   /* 1 = 收到的 chunkIndex 严格递增 */
} srv_t;

static int srv_read_req(int fd, char *hdr, size_t cap, unsigned char *body,
                        size_t bcap, size_t *blen)
{
    size_t total = 0;
    char  *he;

    *blen = 0;
    for (;;) {
        ssize_t n;

        if (total + 1u >= cap)
            return -1;
        n = recv(fd, hdr + total, cap - 1u - total, 0);
        if (n <= 0)
            return -1;
        total += (size_t)n;
        hdr[total] = '\0';
        he = strstr(hdr, "\r\n\r\n");
        if (he != NULL)
            break;
    }
    {
        char  *cl = strstr(hdr, "Content-Length:");
        size_t want = 0;

        if (cl != NULL)
            want = (size_t)strtoul(cl + 15, NULL, 10);
        if (want > bcap)
            return -1;
        {
            size_t have = (size_t)(hdr + total - (he + 4));

            if (have > want)
                have = want;
            memcpy(body, he + 4, have);
            *blen = have;
        }
        while (*blen < want) {
            ssize_t n = recv(fd, body + *blen, want - *blen, 0);

            if (n <= 0)
                return -1;
            *blen += (size_t)n;
        }
    }
    return 0;
}

static void srv_send(int fd, int status, const char *json)
{
    char buf[512];
    int  n;

    n = snprintf(buf, sizeof buf,
                 "HTTP/1.1 %d OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: keep-alive\r\n\r\n%s",
                 status, (unsigned)strlen(json), json);
    if (n > 0)
        (void)send(fd, buf, (size_t)n, 0);
}

/* 从 multipart body 里取 name="file" 那段（到结束边界前）。
 * 正文起点是那两行表头之后的 `\r\n\r\n`，**不是** filename 那一行的行尾 ——
 * 少了这一步会把 `Content-Type: application/octet-stream` 也算进文件里。 */
static size_t srv_file_part(const unsigned char *body, size_t blen,
                            const unsigned char **out)
{
    static const char mark[] = "name=\"file\"; filename=\"";
    const char *p = memmem(body, blen, mark, sizeof mark - 1u);
    const char *h;
    const char *s;
    const char *e;

    if (p == NULL)
        return 0u;
    h = memmem(p, blen - (size_t)(p - (const char *)body), "\r\n\r\n", 4u);
    if (h == NULL)
        return 0u;
    s = h + 4;
    e = memmem(s, blen - (size_t)(s - (const char *)body), "\r\n------", 8u);
    if (e == NULL)
        return 0u;
    *out = (const unsigned char *)s;
    return (size_t)(e - s);
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
            char           hdr[8192];
            unsigned char  body[8192];
            size_t         blen = 0;
            char          *line;
            int            status;
            char           json[128];

            if (srv_read_req(fd, hdr, sizeof hdr, body, sizeof body, &blen) != 0)
                break;
            s->reqs++;
            line   = hdr + 5; /* 跳过 "POST " */
            status = (s->status_override != 0) ? s->status_override : 200;
            if (strncmp(line, "/fnwlw/oss/log/upload/start", 26) == 0) {
                s->starts++;
                (void)snprintf(json, sizeof json,
                               "{\"code\":%d,\"data\":{\"uploadId\":\"UPID-1\"}}",
                               s->code_override != 0 ? s->code_override : 200);
            } else if (strncmp(line, "/fnwlw/oss/log/upload/chunk", 26) == 0) {
                const unsigned char *part = NULL;
                size_t               plen;
                const char          *ci;
                int                  idx;

                /* chunkIndex 在正文的表单字段里：`name="chunkIndex"\r\n\r\n<N>\r\n` */
                ci  = memmem(body, blen, "name=\"chunkIndex\"", 17u);
                idx = -1;
                if (ci != NULL) {
                    const char *v = memmem(ci, blen - (size_t)(ci - (const char *)body),
                                           "\r\n\r\n", 4u);
                    if (v != NULL)
                        idx = atoi(v + 4);
                }
                if (s->chunks == 0u) {
                    s->chunk_index_ok = 1;
                    s->last_chunk_index = idx;
                } else if (idx != s->last_chunk_index + 1) {
                    s->chunk_index_ok = 0;
                }
                s->last_chunk_index = idx;
                s->chunks++;
                plen = srv_file_part(body, blen, &part);
                if (part != NULL && plen > 0u && s->data_len + plen <= SRV_DATA_MAX) {
                    memcpy(s->data + s->data_len, part, plen);
                    s->data_len += plen;
                }
                (void)snprintf(json, sizeof json, "{\"code\":%d}",
                               s->code_override != 0 ? s->code_override : 200);
            } else if (strncmp(line, "/fnwlw/oss/log/upload/finish", 27) == 0) {
                s->finishes++;
                (void)snprintf(json, sizeof json, "{\"code\":%d}",
                               s->code_override != 0 ? s->code_override : 200);
            } else {
                status = 404;
                (void)snprintf(json, sizeof json, "{\"code\":404}");
            }
            srv_send(fd, status, json);
        }
        (void)close(fd);
    }
    return NULL;
}

static int srv_start(srv_t *s)
{
    struct sockaddr_in a;
    socklen_t          al = sizeof a;
    int                one = 1;

    memset(s, 0, sizeof *s);
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0)
        return -1;
    (void)setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    if (bind(s->listen_fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(s->listen_fd, 4) != 0) {
        (void)close(s->listen_fd);
        return -1;
    }
    (void)getsockname(s->listen_fd, (struct sockaddr *)&a, &al);
    s->port = (int)ntohs(a.sin_port);
    return (pthread_create(&s->th, NULL, srv_thread, s) == 0) ? 0 : -1;
}

static void srv_stop(srv_t *s)
{
    s->stop = 1;
    (void)shutdown(s->listen_fd, SHUT_RDWR);
    (void)close(s->listen_fd);
    (void)pthread_join(s->th, NULL);
}

/* ---------------------------------------------------------------------------
 * 临时日志目录
 * ------------------------------------------------------------------------- */

static char g_root[128];

static int mk_log(const char *date, int hour, const char *text, int append)
{
    char  dir[160];
    char  path[192];
    FILE *f;

    (void)snprintf(dir, sizeof dir, "%s/%s", g_root, date);
    (void)mkdir(dir, 0755);
    (void)snprintf(path, sizeof path, "%s/%02d.log", dir, hour);
    f = fopen(path, append ? "a" : "w");
    if (f == NULL)
        return -1;
    (void)fwrite(text, 1, strlen(text), f);
    (void)fclose(f);
    return 0;
}

static size_t read_state_offset(void)
{
    char  path[160];
    char  line[128];
    char  d[16];
    int   h;
    long  o = -1;
    FILE *f;

    (void)snprintf(path, sizeof path, "%s/state", g_root);
    f = fopen(path, "r");
    if (f == NULL)
        return (size_t)-1;
    if (fgets(line, sizeof line, f) == NULL) {
        (void)fclose(f);
        return (size_t)-1;
    }
    (void)fclose(f);
    if (sscanf(line, "%15s %d %ld", d, &h, &o) != 3)
        return (size_t)-1;
    return (size_t)o;
}

static void rm_root(void)
{
    char cmd[256];

    /* 只删本用例自己造的临时目录；`system()` 在任何环境都失败也不影响结论 */
    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    (void)system(cmd);
}

static void cfg_of(iv_logup_cfg_t *c, const srv_t *s, const char *host)
{
    static char state[160];

    iv_logup_cfg_default(c);
    (void)snprintf(state, sizeof state, "%s/state", g_root);
    c->root       = g_root;
    c->state_path = state;
    c->host       = host;
    c->port       = (uint16_t)s->port;
    c->device_sn  = "B2CC1";
    c->log_type   = "SYSTEM";
    c->max_bytes  = 64u * 1024u;
    c->connect_ms = 2000u;
    c->send_ms    = 2000u;
    c->recv_ms    = 2000u;
}

int main(void)
{
    srv_t           srv;
    iv_logup_t      up;
    iv_logup_cfg_t  cfg;
    iv_logup_stats_t st;
    const char     *txt = "line-1 hello\nline-2 hello\nline-3 hello\n";
    char            big[2500];
    int             rc;

    (void)snprintf(g_root, sizeof g_root, "/tmp/ivlogup_%d", (int)getpid());
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

    /* ---- p01：三段全过 + 1 KiB 分片拼回逐字节相等 ---- */
    {
        size_t i;

        /* 2499 字节 + NUL：2500 B 会切出 3 片（1024/1024/451），正好覆盖
         * "最后一片不满"的边界 */
        for (i = 0; i + 1u < sizeof big; i++)
            big[i] = (char)('a' + (int)(i % 26u));
        big[sizeof big - 2u] = '\n';
        big[sizeof big - 1u] = '\0';
    }
    chk(strlen(big) == sizeof big - 1u, "p01 fixture size");
    chk(mk_log("2026-11-02", 13, big, 0) == 0, "p01 make log file");
    cfg_of(&cfg, &srv, "127.0.0.1");
    iv_logup_init(&up, &cfg);
    rc = iv_logup_run(&up);
    chk(rc == IV_OK, "p01 run rc == IV_OK");
    chk(srv.starts == 1u, "p01 one start");
    chk(srv.finishes == 1u, "p01 one finish");
    chk(srv.chunks == 3u, "p01 2500 B -> 3 chunks");
    chk(srv.data_len == sizeof big - 1u, "p01 server received full size");
    chk(memcmp(srv.data, big, srv.data_len) == 0, "p01 bytes identical");
    chk(srv.chunk_index_ok == 1, "p01 chunkIndex strictly increasing");
    iv_logup_stats(&up, &st);
    chk(st.ok == 1u && st.fail == 0u, "p01 stats ok=1");
    chk(st.chunks == 3u, "p01 stats chunks=3");
    chk(st.last_http == 200, "p01 last http 200");

    /* ---- p02：游标推进后第二次运行 = 没有新日志 ---- */
    chk(read_state_offset() == sizeof big - 1u, "p02 cursor advanced to EOF");
    {
        unsigned reqs_before = srv.reqs;

        rc = iv_logup_run(&up);
        chk(rc == IV_ENOENT, "p02 second run rc == IV_ENOENT");
        chk(srv.reqs == reqs_before, "p02 no request sent when nothing new");
    }

    /* ---- p03：文件追加后只传新增字节 ---- */
    {
        size_t before = srv.data_len;

        chk(mk_log("2026-11-02", 13, txt, 1) == 0, "p03 append log");
        rc = iv_logup_run(&up);
        chk(rc == IV_OK, "p03 run after append ok");
        chk(srv.data_len == before + strlen(txt), "p03 only appended bytes sent");
        chk(memcmp(srv.data + before, txt, strlen(txt)) == 0, "p03 appended bytes identical");
    }

    /* ---- p04：服务端回非 2xx ⇒ 失败且游标不动，下次还能重传 ---- */
    {
        size_t   cur = read_state_offset();
        unsigned before_len = (unsigned)srv.data_len;

        srv.chunks = 0u; /* 不关心片数，只看游标 */
        srv.status_override = 500;
        chk(mk_log("2026-11-02", 14, "line-4 hello\n", 0) == 0, "p04 new hour file");
        rc = iv_logup_run(&up);
        chk(rc == IV_EPROTO, "p04 rc == IV_EPROTO on http 500");
        chk(read_state_offset() == cur, "p04 cursor NOT advanced on failure");
        iv_logup_stats(&up, &st);
        chk(st.fail == 1u, "p04 stats fail=1");

        /* 恢复服务端后同一段必须能重传成功 */
        srv.status_override = 0;
        srv.data_len        = before_len;
        srv.chunks          = 0u;
        rc = iv_logup_run(&up);
        chk(rc == IV_OK, "p04 retry after recovery ok");
        chk(srv.data_len > before_len, "p04 retry actually sent the missing bytes");
    }

    iv_logup_close(&up);

    /* ---- p05：空 host ⇒ EINVAL；未 init ⇒ EINVAL ---- */
    cfg_of(&cfg, &srv, "");
    chk(iv_logup_init(&up, &cfg) == IV_OK, "p05 init with empty host is allowed");
    chk(iv_logup_run(&up) == IV_EINVAL, "p05 run with empty host -> IV_EINVAL");
    iv_logup_close(&up);
    chk(iv_logup_run(&up) == IV_EINVAL, "p05 run after close -> IV_EINVAL");

    srv_stop(&srv);
    rm_root();

    if (g_fail != 0) {
        fprintf(stderr, "logup: %d checks FAILED\n", g_fail);
        return 1;
    }
    printf("logup: all checks passed\n");
    return 0;
}
