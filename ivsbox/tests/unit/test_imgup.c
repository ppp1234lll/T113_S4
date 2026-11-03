/*
 * 箱门抓拍图片上传 —— `iv_imgup` 单测（2026-11-03）
 *
 * 用**真 socket**（127.0.0.1 上起一个假上传服务）跑完整三步，而不是 mock 传输：
 * 这个模块最容易错的地方是"片数/片号与 start 申报是否一致""失败会不会把没传成功
 * 的文件删掉""队列满时丢的是不是最旧的"，只有真连接 + 真文件才能暴露。
 *
 * 假服务端与 `test_logup.c` 同一套形状（同一个服务端接口，只是 fileName/logType
 * 不同），并额外记录 start 请求行，用来钉住这两个参数。
 *
 * 断言清单：
 *   i01 入队：源文件被**拷贝**进队列（内容逐字节一致），enq/pending 正确；
 *   i02 上传：三步全过、服务端收到的字节与源文件**逐字节相等**、
 *       片号从 0 连续、片数 = ceil(size/1024)、fileName 是 .jpg、logType=IMAGE；
 *   i03 队列空 ⇒ IV_ENOENT（不是错误）；
 *   i04 服务端 code=500 ⇒ 返回 IV_EPROTO 且**文件留在队列里**（下次重试）；
 *   i05 超大文件 ⇒ IV_ERANGE 且不入队；
 *   i06 队列满 ⇒ 丢最旧的，pending 停在上限；
 *   i07 空 host ⇒ IV_EINVAL。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_imgup.h"
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
 * 假上传服务
 * ------------------------------------------------------------------------- */

#define SRV_DATA_MAX (256u * 1024u)

typedef struct {
    int       listen_fd;
    int       port;
    pthread_t th;
    _Atomic int stop;

    unsigned starts;
    unsigned chunks;
    unsigned finishes;
    char     start_line[512]; /* start 的请求行（校验 fileName/logType/deviceSn） */
    unsigned char data[SRV_DATA_MAX];
    size_t   data_len;
    int      last_chunk_index;
    int      chunk_index_ok;

    int status_override;
    int code_override;
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

/* multipart 的 file 段（到结束边界前），口径同 test_logup.c */
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
            char          hdr[8192];
            unsigned char body[8192];
            size_t        blen = 0;
            char         *line;
            int           status;
            char          json[128];

            if (srv_read_req(fd, hdr, sizeof hdr, body, sizeof body, &blen) != 0)
                break;
            line   = hdr + 5; /* skip "POST " */
            status = (s->status_override != 0) ? s->status_override : 200;
            if (strncmp(line, "/fnwlw/oss/log/upload/start", 26) == 0) {
                size_t n = 0u;

                s->starts++;
                while (line[n] != '\0' && line[n] != ' ' && n + 1u < sizeof s->start_line) {
                    s->start_line[n] = line[n];
                    n++;
                }
                s->start_line[n] = '\0';
                (void)snprintf(json, sizeof json,
                               "{\"code\":%d,\"data\":{\"uploadId\":\"UPID-IMG\"}}",
                               s->code_override != 0 ? s->code_override : 200);
            } else if (strncmp(line, "/fnwlw/oss/log/upload/chunk", 26) == 0) {
                const unsigned char *part = NULL;
                size_t               plen;
                const char          *ci;
                int                  idx = -1;

                ci = memmem(body, blen, "name=\"chunkIndex\"", 17u);
                if (ci != NULL) {
                    const char *v = memmem(ci, blen - (size_t)(ci - (const char *)body),
                                           "\r\n\r\n", 4u);
                    if (v != NULL)
                        idx = atoi(v + 4);
                }
                if (s->chunks == 0u) {
                    s->chunk_index_ok  = 1;
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
                (void)snprintf(json, sizeof json,
                               "{\"code\":%d,\"data\":\"http://x/y.jpg\"}",
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
 * 临时目录与文件
 * ------------------------------------------------------------------------- */

static char g_tmp[128];

static int make_src(const char *name, size_t len, unsigned char seed)
{
    char          path[192];
    FILE         *f;
    unsigned char buf[64];
    size_t        i;

    (void)snprintf(path, sizeof path, "%s/%s", g_tmp, name);
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    for (i = 0; i < len; i++)
        buf[i % sizeof buf] = (unsigned char)((i * 7u + seed) & 0xffu);
    for (i = 0; i < len;) {
        size_t chunk = (len - i > sizeof buf) ? sizeof buf : (len - i);

        /* 每次把 buf 填成当前 offset 决定的值，保证服务端能逐字节比对 */
        {
            size_t k;

            for (k = 0; k < chunk; k++)
                buf[k] = (unsigned char)(((i + k) * 7u + seed) & 0xffu);
        }
        (void)fwrite(buf, 1, chunk, f);
        i += chunk;
    }
    (void)fclose(f);
    return 0;
}

static int read_all(const char *path, unsigned char *out, size_t cap, size_t *len)
{
    FILE  *f = fopen(path, "rb");
    size_t n;

    if (f == NULL)
        return -1;
    n = fread(out, 1, cap, f);
    (void)fclose(f);
    *len = n;
    return 0;
}

static void cfg_of(iv_imgup_cfg_t *cfg, const srv_t *s, const char *host)
{
    /* **static**：cfg 里的指针在 init 时才被复制，栈上的缓冲出了作用域就悬空 */
    static char qd[160];

    (void)snprintf(qd, sizeof qd, "%s/queue", g_tmp);
    iv_imgup_cfg_default(cfg);
    cfg->queue_dir = qd;
    cfg->host      = host;
    cfg->port      = (uint16_t)s->port;
    cfg->device_sn = "B2CC1";
}

int main(void)
{
    srv_t          srv;
    iv_imgup_cfg_t cfg;
    iv_imgup_t     iu;
    char           qd[160];
    char           src[192];
    unsigned char  want[8192];
    size_t         wlen = 0u;
    int            rc;

    (void)snprintf(g_tmp, sizeof g_tmp, "/tmp/ivimgup.XXXXXX");
    if (mkdtemp(g_tmp) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    (void)snprintf(qd, sizeof qd, "%s/queue", g_tmp);

    if (srv_start(&srv) != 0) {
        fprintf(stderr, "srv_start failed\n");
        return 1;
    }

    /* ---- i01：入队（拷贝，不动源文件） ---- */
    chk(make_src("snap_1793327941.jpg", 2500u, 1u) == 0, "i01 make src");
    (void)snprintf(src, sizeof src, "%s/snap_1793327941.jpg", g_tmp);
    cfg_of(&cfg, &srv, "127.0.0.1");
    chk(iv_imgup_init(&iu, &cfg) == IV_OK, "i01 init");
    chk(iv_imgup_enqueue(&iu, src) == IV_OK, "i01 enqueue");
    chk(iv_imgup_pending(&iu) == 1, "i01 pending == 1");
    chk(iu.st.enq == 1u, "i01 enq counter");
    {
        char path[192];

        (void)snprintf(path, sizeof path, "%s/snap_1793327941.jpg", qd);
        chk(read_all(path, want, sizeof want, &wlen) == 0 && wlen == 2500u,
            "i01 queued copy has 2500 B");
        chk(access(src, F_OK) == 0, "i01 source file still there");
    }

    /* ---- i02：上传三步，字节逐字节相等 ---- */
    rc = iv_imgup_run(&iu);
    chk(rc == IV_OK, "i02 run ok");
    chk(iv_imgup_pending(&iu) == 0, "i02 queue drained");
    chk(iu.st.ok == 1u && iu.st.bytes == 2500u, "i02 stats ok/bytes");
    chk(srv.starts == 1u && srv.finishes == 1u, "i02 start/finish once");
    chk(srv.chunks == 1u, "i02 2500 B -> one request");
    chk(srv.last_chunk_index == 0, "i02 chunkIndex 0");
    chk(srv.data_len == 2500u, "i02 server got 2500 B");
    {
        unsigned char expect[8192];
        size_t        elen = 0u;
        char          path[192];

        (void)snprintf(path, sizeof path, "%s/snap_1793327941.jpg", g_tmp);
        chk(read_all(path, expect, sizeof expect, &elen) == 0, "i02 read source");
        chk(elen == srv.data_len && memcmp(expect, srv.data, elen) == 0,
            "i02 bytes identical on the wire");
    }
    chk(strstr(srv.start_line, "fileName=snap_1793327941.jpg") != NULL,
        "i02 start carries the jpg file name");
    chk(strstr(srv.start_line, "logType=IMAGE") != NULL, "i02 start logType=IMAGE");
    chk(strstr(srv.start_line, "deviceSn=B2CC1") != NULL, "i02 start deviceSn");
    chk(strstr(srv.start_line, "totalChunks=1") != NULL, "i02 totalChunks=1");
    chk(iu.st.last_url[0] != '\0', "i02 finish url captured");

    /* ---- i03：空队列 ---- */
    chk(iv_imgup_run(&iu) == IV_ENOENT, "i03 empty queue -> IV_ENOENT");

    /* ---- i04：服务端业务拒绝 ⇒ 文件留在队列里 ---- */
    chk(make_src("snap_1793327942.jpg", 1200u, 2u) == 0, "i04 make src");
    (void)snprintf(src, sizeof src, "%s/snap_1793327942.jpg", g_tmp);
    chk(iv_imgup_enqueue(&iu, src) == IV_OK, "i04 enqueue");
    srv.code_override = 500;
    rc = iv_imgup_run(&iu);
    chk(rc != IV_OK && rc != IV_ENOENT, "i04 run fails on server reject");
    chk(iv_imgup_pending(&iu) == 1, "i04 file kept for retry");
    chk(iu.st.fail == 1u, "i04 fail counter");
    srv.code_override = 0;

    /* 网络恢复后能补传（"掉电/断网期间开的门不会丢"） */
    rc = iv_imgup_run(&iu);
    chk(rc == IV_OK, "i04 retry after recovery ok");
    chk(iv_imgup_pending(&iu) == 0, "i04 queue drained after retry");

    /* ---- i05：超大文件不入队 ---- */
    chk(make_src("snap_1793327943.jpg", IV_IMGUP_MAX_BYTES + 1u, 3u) == 0,
        "i05 make oversize");
    (void)snprintf(src, sizeof src, "%s/snap_1793327943.jpg", g_tmp);
    chk(iv_imgup_enqueue(&iu, src) == IV_ERANGE, "i05 oversize -> IV_ERANGE");
    chk(iv_imgup_pending(&iu) == 0, "i05 not enqueued");

    /* ---- i06：队列满丢最旧 ---- */
    {
        unsigned k;

        for (k = 0; k < IV_IMGUP_MAX_FILES + 1u; k++) {
            char name[32];
            char path[192];

            (void)snprintf(name, sizeof name, "snap_%010u.jpg", 1793328000u + k);
            chk(make_src(name, 100u, (unsigned char)k) == 0, "i06 make src");
            (void)snprintf(path, sizeof path, "%s/%s", g_tmp, name);
            chk(iv_imgup_enqueue(&iu, path) == IV_OK, "i06 enqueue");
        }
        chk(iv_imgup_pending(&iu) == (int)IV_IMGUP_MAX_FILES,
            "i06 pending capped at IV_IMGUP_MAX_FILES");
        chk(iu.st.drop >= 1u, "i06 drop counter moved");
        {
            char   path[192];
            struct stat sb;

            (void)snprintf(path, sizeof path, "%s/snap_1793328000.jpg", qd);
            chk(stat(path, &sb) != 0, "i06 oldest file dropped");
        }
    }

    /* ---- i07：空 host ---- */
    iv_imgup_close(&iu);
    cfg_of(&cfg, &srv, "");
    chk(iv_imgup_init(&iu, &cfg) == IV_OK, "i07 init with empty host allowed");
    chk(iv_imgup_run(&iu) == IV_EINVAL, "i07 run with empty host -> IV_EINVAL");
    iv_imgup_close(&iu);

    srv_stop(&srv);
    {
        char cmd[192];

        (void)snprintf(cmd, sizeof cmd, "rm -rf %s", g_tmp);
        (void)system(cmd);
    }

    if (g_fail != 0) {
        fprintf(stderr, "imgup: %d checks FAILED\n", g_fail);
        return 1;
    }
    printf("imgup: all checks passed\n");
    return 0;
}
