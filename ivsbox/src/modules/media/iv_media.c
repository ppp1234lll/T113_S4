/*
 * IVSBox 媒体桥接实现（M6-S6.1，契约见 iv_media.h 文件头）
 *
 * 结构一句话：调用方（Reactor 单线程）只做"入队/同步 HTTP/改会话表"；
 * 唯一的慢动作（/capture 的 V4L2 出帧）由 worker 线程承担；worker 顺带按
 * 周期给活跃录像会话打保活（bird_media 的 /record/status 查询即续命）。
 */
#include "ivsbox/iv_media.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_log.h"

#define MD_MOD "media"

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

/* 监听墙钟秒（抓拍自动命名用；命名只求可读唯一，不参与任何判定） */
static int64_t wall_sec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec;
}

/* percent-encode：RFC3986 unreserved 原样，其余 %XX。out 需 >= 3*len+1。 */
static int url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t oi = 0;
    size_t i;

    for (i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                         c == '.' || c == '~';
        if (unreserved) {
            if (oi + 2u > cap)
                return IV_ERANGE;
            out[oi++] = (char)c;
        } else {
            if (oi + 4u > cap)
                return IV_ERANGE;
            out[oi++] = '%';
            out[oi++] = hex[c >> 4];
            out[oi++] = hex[c & 0x0F];
        }
    }
    if (oi + 1u > cap)
        return IV_ERANGE;
    out[oi] = '\0';
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 极小回环 HTTP 客户端（GET，只认状态行与 body）
 * ------------------------------------------------------------------------- */

/* 连接 host:port，最坏 2 s（非阻塞 connect + poll）。成功返回 fd（阻塞态）。 */
static int http_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    struct in_addr     addr;
    int                fd;
    int                rc;
    struct timeval     tv;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (host == NULL || inet_pton(AF_INET, host, &addr) != 1)
        return IV_EINVAL;
    sa.sin_addr = addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return IV_EIO;

    /* 非阻塞发起连接，poll 等结果——127.0.0.1 上对端死了立即 RST（ECONNREFUSED），
     * 这里只防"活着但半死"的极端情形。 */
    (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0 && errno == EINPROGRESS) {
        struct pollfd p;

        p.fd      = fd;
        p.events  = POLLOUT;
        p.revents = 0;
        rc = poll(&p, 1, IV_MEDIA_HTTP_TIMEOUT_MS);
        if (rc <= 0) {
            close(fd);
            return IV_ECONN;
        }
        {
            int       err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 ||
                err != 0) {
                close(fd);
                return IV_ECONN;
            }
        }
    } else if (rc != 0) {
        close(fd);
        return IV_ECONN; /* ECONNREFUSED 等：media 进程不在 */
    }

    (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    tv.tv_sec  = IV_MEDIA_HTTP_TIMEOUT_MS / 1000u;
    tv.tv_usec = (long)(IV_MEDIA_HTTP_TIMEOUT_MS % 1000u) * 1000L;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

/*
 * GET path，收响应。出参 body（NUL 结尾）与 http 状态码。
 * 返回 IV_OK / 负码。resp 需可容纳整个响应（本机接口响应都很小，1 KB 够）。
 */
static int http_get(const char *host, int port, const char *path,
                    char *resp, size_t cap, int *status)
{
    char   req[512];
    int    fd;
    size_t got = 0;
    size_t n;

    *status = 0;
    (void)snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n"
                                     "Connection: close\r\n\r\n",
                   path, (host != NULL) ? host : "127.0.0.1");

    fd = http_connect(host, port);
    if (fd < 0)
        return fd;

    n = strlen(req);
    if (send(fd, req, n, 0) != (ssize_t)n) {
        close(fd);
        return IV_EIO;
    }

    /* 收到连接关闭为止（HTTP/1.0 + Connection: close，bird_media mongoose 如此）。
     * 响应上限 1 KB：四接口的响应最长是 "OK"/"1"/"0"/错误文本。 */
    while (got + 1u < cap) {
        ssize_t r = recv(fd, resp + got, cap - 1u - got, 0);
        if (r == 0)
            break;
        if (r < 0) {
            close(fd);
            return IV_EIO;
        }
        got += (size_t)r;
    }
    close(fd);
    resp[got] = '\0';

    if (got < 12u || strncmp(resp, "HTTP/1.", 7) != 0)
        return IV_EPROTO;
    *status = atoi(resp + 9);

    /* 把 body 挪到 resp 头部（复用缓冲，调用方只关心 body） */
    {
        char *body = strstr(resp, "\r\n\r\n");
        size_t blen;
        if (body == NULL)
            return IV_EPROTO;
        body += 4;
        blen = got - (size_t)(body - resp);
        memmove(resp, body, blen + 1u);
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * worker：抓拍队列 ＋ 会话保活
 * ------------------------------------------------------------------------- */

static uint64_t mono_ms(void)
{
    return iv_clock_monotonic_ms();
}

/* 对一路会话补保活：status 查询即续命；对端说没在录就清会话表。 */
static void ka_one(iv_media_t *m, int id, uint64_t now)
{
    char    resp[256];
    int     status = 0;
    char    path[64];
    int     rec = 0;

    (void)now;
    (void)snprintf(path, sizeof(path), "/record/status?id=%d", id);
    if (http_get(m->cfg.host, m->cfg.port, path, resp, sizeof(resp),
                 &status) != IV_OK || status != 200)
        return; /* media 进程不在/卡：保活失败，下轮再试（对端 15 min 才超时） */

    rec = (resp[0] == '1');
    pthread_mutex_lock(&m->lock);
    if (!rec && m->rec_active[id]) {
        IV_LOG_I(MD_MOD, "record[%d]: peer reports stopped (rtsp ended?), session cleared", id);
        m->rec_active[id] = 0;
    }
    pthread_mutex_unlock(&m->lock);
}

/* ---------------------------------------------------------------------------
 * 退出标志的跨线程访问
 *
 * `stop` 是**唯一的跨线程标志**：调用方在 `iv_media_fini()` 里写，worker 在读。
 * 而 worker 那句 `while (!m->stop)` **不在锁内**（锁内那句 `!m->stop && q_count==0`
 * 才是有序的）⇒ 裸读写就是数据竞争，`make tsan` 会如实报一条
 * （2026-10-22 门禁上真的红过一次：`Read of size 1 … worker_main iv_media.c:230`
 * 对 `Previous write … iv_media_fini iv_media.c:380`）。
 *
 * 三处一律走 relaxed 原子，**不许混着来**：只把读改成原子、写还是裸写，TSan 照样判
 * 竞争（原子与非原子访问同一地址同样是竞争）。做法与 iv_taskpool 的进度号/cancel
 * 标志完全一致；armv7 上 1/4 字节 relaxed 访问是单条 ldr/str，不会引入 libatomic。
 * ------------------------------------------------------------------------- */
static int media_stopped(iv_media_t *m)
{
    return (int)__atomic_load_n(&m->stop, __ATOMIC_RELAXED);
}

static void media_request_stop(iv_media_t *m)
{
    __atomic_store_n(&m->stop, 1u, __ATOMIC_RELAXED);
}

static void *worker_main(void *arg)
{
    iv_media_t *m = arg;
    uint32_t    ka = (m->cfg.ka_period_ms != 0u) ? m->cfg.ka_period_ms : 60000u;

    m->last_ka_ms = mono_ms();
    while (media_stopped(m) == 0) {
        char     path[IV_MEDIA_URL_MAX];
        int      have = 0;
        uint64_t now;

        /* 取一个抓拍请求（有就做；没有就等 ka 周期或唤醒） */
        pthread_mutex_lock(&m->lock);
        if (m->q_count > 0) {
            memcpy(path, m->q_path[m->q_head], sizeof(path));
            m->q_head = (m->q_head + 1) % IV_MEDIA_QUEUE_DEPTH;
            m->q_count--;
            have = 1;
        }
        pthread_mutex_unlock(&m->lock);

        if (have) {
            char enc[IV_MEDIA_URL_MAX * 3];
            char req[IV_MEDIA_URL_MAX * 3 + 32];
            char resp[256];
            int  status = 0;

            if (url_encode(path, enc, sizeof(enc)) == IV_OK)
                (void)snprintf(req, sizeof(req), "/capture?path=%s", enc);
            else
                req[0] = '\0';
            if (req[0] != '\0' &&
                http_get(m->cfg.host, m->cfg.port, req, resp, sizeof(resp),
                         &status) == IV_OK && status == 200) {
                struct stat st;
                pthread_mutex_lock(&m->lock);
                if (stat(path, &st) == 0) {
                    m->st.snap_ok++;
                    IV_LOG_I(MD_MOD, "capture ok: %s (%ld bytes)", path,
                             (long)st.st_size);
                } else {
                    m->st.snap_fail++;
                    IV_LOG_W(MD_MOD, "capture: peer 200 but %s missing", path);
                }
                pthread_mutex_unlock(&m->lock);
            } else {
                pthread_mutex_lock(&m->lock);
                m->st.snap_fail++;
                pthread_mutex_unlock(&m->lock);
                IV_LOG_W(MD_MOD, "capture failed: %s (http %d)", path, status);
            }
            continue; /* 干完一个立刻看队列里还有没有 */
        }

        /* 队列空：到点给活跃会话打保活 */
        now = mono_ms();
        if (now - m->last_ka_ms >= ka) {
            int id;
            m->last_ka_ms = now;
            for (id = 0; id < IV_MEDIA_CH_N; id++) {
                uint8_t active;
                pthread_mutex_lock(&m->lock);
                active = m->rec_active[id];
                pthread_mutex_unlock(&m->lock);
                if (active)
                    ka_one(m, id, now);
            }
        }

        /* 睡到有人叫或 ka 到点（取短；CLOCK_REALTIME 绝对时刻） */
        {
            uint64_t deadline = m->last_ka_ms + ka;
            uint64_t left_ms  = (deadline > now) ? (deadline - now) : 1u;
            struct timespec ts;

            if (left_ms > ka)
                left_ms = ka;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(left_ms / 1000u);
            ts.tv_nsec += (long)(left_ms % 1000u) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_mutex_lock(&m->lock);
            if (media_stopped(m) == 0 && m->q_count == 0)
                pthread_cond_timedwait(&m->cond, &m->lock, &ts);
            pthread_mutex_unlock(&m->lock);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

void iv_media_cfg_default(iv_media_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->host         = NULL; /* init 里落默认 127.0.0.1 */
    cfg->port         = 0;    /* init 里落默认 18000 */
    cfg->snap_dir     = NULL;
    cfg->ka_period_ms = 60000u;
}

static const char *host_of(const iv_media_t *m)
{
    return (m->cfg.host != NULL) ? m->cfg.host : "127.0.0.1";
}

int iv_media_init(iv_media_t *m, const iv_media_cfg_t *cfg)
{
    if (m == NULL || cfg == NULL)
        return IV_EINVAL;
    if (m->inited)
        return IV_ESTATE;

    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    /* 默认值就地落死：worker/record 各处不再做三元判空（漏一处就是连 0 端口） */
    if (m->cfg.host == NULL)
        m->cfg.host = "127.0.0.1";
    if (m->cfg.port == 0)
        m->cfg.port = IV_MEDIA_PORT_DEFAULT;
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->cond, NULL);

    {
        /* 工作线程**总是要起**：抓拍异步化依赖它。ka_period_ms=0 仅表示"不做
         * 会话保活轮询"（纯手动 record 场景）。 */
        if (pthread_create(&m->worker, NULL, worker_main, m) != 0) {
            IV_LOG_E(MD_MOD, "media worker thread create failed");
            pthread_cond_destroy(&m->cond);
            pthread_mutex_destroy(&m->lock);
            return IV_EIO;
        }
        m->worker_up = 1;
    }

    m->inited = 1;
    IV_LOG_I(MD_MOD, "media bridge up: %s:%d snap_dir=%s ka=%us",
             host_of(m),
             (m->cfg.port != 0) ? m->cfg.port : IV_MEDIA_PORT_DEFAULT,
             (m->cfg.snap_dir != NULL) ? m->cfg.snap_dir : IV_MEDIA_SNAP_DIR_DEFAULT,
             (unsigned)((m->cfg.ka_period_ms != 0u) ? m->cfg.ka_period_ms : 0u));
    return IV_OK;
}

void iv_media_fini(iv_media_t *m)
{
    if (m == NULL || !m->inited)
        return;

    if (m->worker_up) {
        pthread_mutex_lock(&m->lock);
        media_request_stop(m);
        pthread_cond_broadcast(&m->cond);
        pthread_mutex_unlock(&m->lock);
        pthread_join(m->worker, NULL);
        m->worker_up = 0;
    }
    pthread_cond_destroy(&m->cond);
    pthread_mutex_destroy(&m->lock);
    m->inited = 0;
}

/* ---------------------------------------------------------------------------
 * 抓拍
 * ------------------------------------------------------------------------- */

int iv_media_capture_async(iv_media_t *m, const char *path,
                           char *out_path, size_t cap)
{
    char    auto_path[IV_MEDIA_URL_MAX];
    int64_t sec;

    if (m == NULL || !m->inited)
        return IV_ESTATE;

    if (path == NULL) {
        const char *dir = (m->cfg.snap_dir != NULL) ? m->cfg.snap_dir
                                                    : IV_MEDIA_SNAP_DIR_DEFAULT;
        sec = wall_sec();
        (void)snprintf(auto_path, sizeof(auto_path), "%s/snap_%lld.jpg", dir,
                       (long long)sec);
        path = auto_path;
    }
    if (out_path != NULL)
        (void)snprintf(out_path, cap, "%s", path);

    if (!m->worker_up)
        return IV_ESTATE; /* 无线程就没有异步（口径见 iv_media_init） */

    pthread_mutex_lock(&m->lock);
    if (m->q_count >= IV_MEDIA_QUEUE_DEPTH) {
        /* 满则丢最旧：新门事件比旧的更值得关注（口径写进头文件） */
        m->q_head = (m->q_head + 1) % IV_MEDIA_QUEUE_DEPTH;
        m->q_count--;
        m->drop_cnt++;
        m->st.snap_drop++;
        IV_LOG_W(MD_MOD, "capture queue full, oldest dropped (total %llu)",
                 (unsigned long long)m->st.snap_drop);
    }
    (void)snprintf(m->q_path[(m->q_head + m->q_count) % IV_MEDIA_QUEUE_DEPTH],
                   IV_MEDIA_URL_MAX, "%s", path);
    m->q_count++;
    m->st.snap_req++;
    pthread_cond_signal(&m->cond);
    pthread_mutex_unlock(&m->lock);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 录像
 * ------------------------------------------------------------------------- */

int iv_media_record_start(iv_media_t *m, int id, const char *url)
{
    char req[IV_MEDIA_URL_MAX * 3 + 32];
    char eurl[IV_MEDIA_URL_MAX * 3];
    char resp[256];
    int  status = 0;
    int  rc;

    if (m == NULL || !m->inited)
        return IV_ESTATE;
    if (id < 0 || id >= IV_MEDIA_CH_N || url == NULL || url[0] == '\0')
        return IV_EINVAL;

    rc = url_encode(url, eurl, sizeof(eurl));
    if (rc != IV_OK)
        return rc;
    (void)snprintf(req, sizeof(req), "/record/start?id=%d&url=%s", id, eurl);
    if (http_get(host_of(m), m->cfg.port, req, resp, sizeof(resp), &status) != IV_OK || status != 200)
        return IV_ECONN;

    pthread_mutex_lock(&m->lock);
    m->rec_active[id] = 1;
    (void)snprintf(m->rec_url[id], sizeof(m->rec_url[id]), "%s", url);
    m->st.rec_starts++;
    pthread_mutex_unlock(&m->lock);
    IV_LOG_I(MD_MOD, "record[%d] start accepted: %s", id, url);
    return IV_OK;
}

int iv_media_record_stop(iv_media_t *m, int id)
{
    char req[64];
    char resp[256];
    int  status = 0;

    if (m == NULL || !m->inited)
        return IV_ESTATE;
    if (id < 0 || id >= IV_MEDIA_CH_N)
        return IV_EINVAL;

    (void)snprintf(req, sizeof(req), "/record/stop?id=%d", id);
    if (http_get(host_of(m), m->cfg.port, req, resp, sizeof(resp), &status) != IV_OK || status != 200)
        return IV_ECONN;

    pthread_mutex_lock(&m->lock);
    m->rec_active[id] = 0;
    m->st.rec_stops++;
    pthread_mutex_unlock(&m->lock);
    IV_LOG_I(MD_MOD, "record[%d] stop accepted", id);
    return IV_OK;
}

int iv_media_record_status(iv_media_t *m, int id, int *recording)
{
    char req[64];
    char resp[256];
    int  status = 0;

    if (m == NULL || !m->inited || recording == NULL)
        return IV_ESTATE;
    if (id < 0 || id >= IV_MEDIA_CH_N)
        return IV_EINVAL;

    (void)snprintf(req, sizeof(req), "/record/status?id=%d", id);
    if (http_get(host_of(m), m->cfg.port, req, resp, sizeof(resp), &status) != IV_OK || status != 200)
        return IV_ECONN;

    *recording = (resp[0] == '1');
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 读数
 * ------------------------------------------------------------------------- */

void iv_media_stats(const iv_media_t *m, iv_media_stats_t *out)
{
    if (m == NULL || out == NULL)
        return;
    pthread_mutex_lock(&((iv_media_t *)m)->lock);
    *out = m->st;
    pthread_mutex_unlock(&((iv_media_t *)m)->lock);
}

int iv_media_sessions(const iv_media_t *m, int *id_out,
                      char (*url_out)[IV_MEDIA_URL_MAX], int chcap)
{
    int n   = 0;
    int id;

    if (m == NULL)
        return 0;
    pthread_mutex_lock(&((iv_media_t *)m)->lock);
    for (id = 0; id < IV_MEDIA_CH_N; id++) {
        if (!m->rec_active[id])
            continue;
        if (id_out != NULL && n < chcap)
            id_out[n] = id;
        if (url_out != NULL && n < chcap)
            memcpy(url_out[n], m->rec_url[id], IV_MEDIA_URL_MAX);
        n++;
    }
    pthread_mutex_unlock(&((iv_media_t *)m)->lock);
    return n;
}
