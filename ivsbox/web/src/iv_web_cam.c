/*
 * ivsbox-web 摄像机受控代理（web 层，功能开发计划 M8-S8.5；架构 §9.3）
 *
 * ============================ 它是什么 ============================
 * frp 的 stcp visitor 落在本机的 `sys.tunnel.cam.port`（默认 8081）上 —— 那个口是
 * **本文件所在进程**用**只绑 127.0.0.1** 的专用监听提供的。运维修从通道进来访问
 * 第 N 路摄像机的 Web 页面时，请求长这样：
 *
 *     GET /cam/<N>/... HTTP/1.1
 *
 * 本文件把它翻译成对 `camera.ch.<N>.ip` 的一个**受限 HTTP 反向代理**请求。
 *
 * ============================ 安全围栏（§9.3/§9.4 的落点） ============================
 *   ① **目标只可能来自配置键**：`/cam/<N>` 的 N 是唯一自由度（0..7），IP 一律经本地
 *      通道向 `ivsboxd` 查 `camera.ch.<N>.ip`。URL 里**没有**任何能指定目标的位置
 *      —— "禁止任意 ip:port" 在这里是物理事实，不是代码自觉。
 *   ② **端口只有一个**：上游端口＝`cam_upstream`（默认 80，命令行/环境变量可改）。
 *      摄像机的 RTSP/SDK/私有端口**不在代理面内**（现场端口集合待确认，已登记）。
 *   ③ **监听只绑 127.0.0.1**：局域网里任何人都碰不到这个口 —— 入口只有两条，
 *      本机进程与持钥匙的 frp visitor。专用口上也没有登录页/API/静态资源，
 *      非法路径回一个不透露任何信息的 404。
 *   ④ **环回护栏**：目标是"本代理自己"（127.0.0.1:cam_port）⇒ 拒绝，防自环。
 *   ⑤ **凭据零下发**：配置里存的摄像机口令（SM4 密文）本进程**读都不读**（Web 不链
 *      libivmodules，也没有 SM4）——浏览器里看到的一切都是摄像机自己的登录流，
 *      经本代理原样往返。配置凭据的"自动注入登录"不做（各家登录方式不一，
 *      且那会把配置口令写进上游请求头）。
 *
 * ============================ 转发语义（刻意很窄） ============================
 *   - 方法只放行 GET/POST；其余 405。
 *   - 请求头只透传 Content-Type / Cookie / Authorization（Host 由本代理按目标重写；
 *     每请求补 `Connection: close`，上游一应答完就断，不维护上游连接池）。
 *   - 响应**整体缓冲**，上限 `IV_WEB_CAM_RSP_MAX`（≈90 KB，受 wbuf 硬约束）；
 *     超限 502。**登记不承诺**：MJPEG/HTTP 长流、WebSocket 升级、插件页 ——
 *     摄像机的实时视频走 RTSP/私有流，不在本代理的职责内（§9.3 口径）。
 *   - **Location / Set-Cookie / 绝对 URL 重写**：把 `http://<ip>[:<port>]/x` 与
 *     `/x` 改写成 `/cam/<N>/x`，Cookie 的 `Path=/x` 改成 `Path=/cam/<N>/x` ——
 *     尽力而为的文本替换（只处理这一种形态；"真机兼容性不承诺"已按计划登记）。
 *   - 超时：连接 2 s、响应总预算 8 s（阻塞式 —— 与 `iv_web_chan_call` 同一模型，
 *     单线程事件循环里的同步调用；上限就是阻塞上限）。
 *   - Transfer-Encoding: chunked 一律 502（不做 de-chunk；带 Content-Length 的
 *     应答才转发）。
 *
 * ============================ 为什么手写 "value":"..." 的解析 ============================
 * 分层纪律（Makefile 里写死）：Web **只链 libivcore**，不链 libivmodules —— json-c
 * 不在 Web 的依赖面里。`config.get` 的应答形状是本工程自己定的
 * （`{"ok":true,"d":{"value":"..."}}`），手写一个只认这一形状的扫描器即可，
 * 与 iv_prov 手写容错扫描器是同一条纪律。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "iv_web.h"
#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

#define CAM_MOD "web-cam"

/* 上游端口默认值：摄像机 Web 的出厂端口（现场多端口集合待确认，见计划 S8.5）。 */
#define IV_WEB_CAM_UPSTREAM_DEFAULT 80u
/* 连接与总预算（毫秒）。 */
#define IV_WEB_CAM_CONNECT_MS 2000u
#define IV_WEB_CAM_TOTAL_MS   8000u

/* 摄像机路数上限：配置里 camera.ch 是 8 槽（探测只用前 6 路 —— 口径见 iv_config） */
#define CAM_CH_N 8

/* ---------------------------------------------------------------------------
 * 从 chan 应答里抠 "value":"..."（只认 config.get 的形状；无转义处理 —— IP 不会有）
 * ------------------------------------------------------------------------- */
static int scan_value_str(const char *json, char *out, size_t cap)
{
    const char *p;
    const char *q;
    size_t      n;

    out[0] = '\0';
    p = strstr(json, "\"value\"");
    if (p == NULL)
        return IV_ENOENT;
    p = strchr(p + 7, ':');
    if (p == NULL)
        return IV_EPROTO;
    p = strchr(p + 1, '"');
    if (p == NULL)
        return IV_EPROTO;
    p++;
    q = strchr(p, '"');
    if (q == NULL)
        return IV_EPROTO;
    n = (size_t)(q - p);
    if (n == 0u)
        return IV_ENOENT; /* 空串 ＝ 未配置 */
    if (n >= cap)
        n = cap - 1u;
    memcpy(out, p, n);
    out[n] = '\0';
    return IV_OK;
}

/* 取第 N 路摄像机的 IP（经本地通道问 ivsboxd；Web 自己不读配置）。 */
static int cam_ip_of(iv_web_ctx_t *ctx, unsigned n, char *ip, size_t cap)
{
    char req[96];
    char rsp[1024];
    size_t rlen = 0;
    int    rc;

    (void)snprintf(req, sizeof req,
                   "{\"m\":\"config.get\",\"p\":{\"key\":\"camera.ch.%u.ip\"}}", n);
    rc = iv_web_chan_call(ctx, req, strlen(req), rsp, sizeof rsp, &rlen);
    if (rc != IV_OK)
        return rc;
    rsp[rlen] = '\0';
    if (strstr(rsp, "\"ok\":true") == NULL)
        return IV_ENOENT; /* 键不存在（不该发生：默认值表里有） */
    return scan_value_str(rsp, ip, cap);
}

/* 严格 IPv4 → uint32（按"数值按大端读"的全工程约定，如 0x7F000001）。失败 0。 */
static uint32_t ip4_parse(const char *s)
{
    struct in_addr in;

    if (s == NULL || s[0] == '\0')
        return 0u;
    if (inet_pton(AF_INET, s, &in) != 1)
        return 0u;
    return ntohl(in.s_addr);
}

/* 连接上游（**非阻塞 connect + poll**，IV_WEB_CAM_CONNECT_MS 预算 —— 阻塞 connect
 * 的内核兜底可达两分钟，会把整个 Web 事件循环冻死那么久）。成功返回阻塞态 fd。 */
static int upstream_connect(uint32_t ip_be_val, uint16_t port)
{
    struct sockaddr_in sa;
    struct pollfd      pfd;
    int                fd;
    int                on  = 1;
    int                err = 0;
    socklen_t          elen = sizeof err;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    /* ip_be_val 是"按大端读"的数值（0x7F000001），in_addr 要的是内存序字节 ⇒ htonl */
    sa.sin_addr.s_addr = htonl(ip_be_val);

    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        if (errno != EINPROGRESS) {
            (void)close(fd);
            return -1;
        }
        pfd.fd      = fd;
        pfd.events  = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, (int)IV_WEB_CAM_CONNECT_MS) != 1) {
            (void)close(fd); /* 超时或出错（摄像机不在线是常态，不是崩溃） */
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            (void)close(fd);
            return -1;
        }
    }
    /* 恢复阻塞态：后面的 send/recv 用同一段代码的简单语义（总预算另有 deadline）。 */
    {
        int fl = fcntl(fd, F_GETFL, 0);

        if (fl >= 0)
            (void)fcntl(fd, F_SETFL, fl & ~(int)O_NONBLOCK);
    }
    return fd;
}

/* 请求路径 → 路号（0..CAM_CH_N-1）与代理内的剩余路径。合法返回 0。 */
static int cam_route_parse(const char *path, unsigned *n, const char **rest)
{
    const char *p = path;
    char       *e = NULL;
    long        v;

    if (p == NULL || strncmp(p, "/cam/", 5) != 0)
        return IV_ENOTSUP;
    p += 5;
    if (p[0] < '0' || p[0] > '9')
        return IV_ENOTSUP;
    v = strtol(p, &e, 10);
    if (e == p || v < 0 || v >= (long)CAM_CH_N)
        return IV_ENOTSUP;
    if (p[0] == '0' && e - p != 1)
        return IV_ENOTSUP; /* 前导零（/cam/01）不认，与全工程服务名口径一致 */
    if (*e != '\0' && *e != '/')
        return IV_ENOTSUP;
    *n    = (unsigned)v;
    *rest = (*e == '/') ? e : "/"; /* /cam/0 ⇒ 上游拿 "/" */
    return IV_OK;
}

/* 把 buf 里所有 `http://<ip>[:<port>]/…` 换成 `<prefix>/…`（prefix＝"/cam/N"）。
 * ⚠ 替换串必须保留路径分隔符：needle 带尾 "/"、替换成 prefix+"/" —— 早先
 * "needle 带尾 / 替换成 prefix（不带 /）"会把 `/login.js` 变成 `0login.js`
 * （集成用例 c03 实证）。裸主机形态（无路径）替换成 prefix 本身。 */
static void rewrite_abs_urls(char *buf, size_t cap, const char *ipstr, uint16_t port,
                             const char *prefix)
{
    char        needle_sl[64];
    char        needle[48];
    char        repl_sl[24];
    size_t      used = strlen(buf);
    size_t      pn  = strlen(prefix);
    size_t      pns = pn + 1u; /* prefix + "/" */
    /* 空前缀（端口寻址形态）下裸主机 `http://<ip>` 要换成 "/"，不能换成空串 ——
     * 换成空串会把 `http://<ip>` 连同后面的路径粘成一坨（早先 c03 的同类坑）。 */
    const char *repl = (pn != 0u) ? prefix : "/";
    size_t      rn   = strlen(repl);

    (void)snprintf(needle, sizeof needle, "http://%s", ipstr);
    (void)snprintf(needle_sl, sizeof needle_sl, "http://%s:%u/", ipstr, (unsigned)port);
    (void)snprintf(repl_sl, sizeof repl_sl, "%s/", prefix);

    /* 带端口的先换（更长、更specific），再换不带端口的 */
    for (;;) {
        char *hit = strstr(buf, needle_sl);
        size_t hn = strlen(needle_sl);

        if (hit == NULL)
            break;
        if (used - hn + pns + 1u > cap)
            return; /* 放不下就保持原文（尽力而为） */
        memmove(hit + pns, hit + hn, used - (size_t)(hit - buf) - hn + 1u);
        memcpy(hit, repl_sl, pns);
        used -= hn - pns;
    }
    for (;;) {
        char *hit = strstr(buf, needle);
        size_t hn = strlen(needle);

        if (hit == NULL)
            break;
        if (used - hn + rn + 1u > cap)
            return;
        memmove(hit + rn, hit + hn, used - (size_t)(hit - buf) - hn + 1u);
        memcpy(hit, repl, rn);
        used -= hn - rn;
    }
}

/* Location / Set-Cookie 的路径改写：绝对地址剥成站内相对，再加 /cam/N 前缀。 */
static void rewrite_location(char *val, size_t cap, const char *ipstr, uint16_t port,
                             const char *prefix)
{
    char abs[96];

    (void)snprintf(abs, sizeof abs, "http://%s:%u", ipstr, (unsigned)port);
    {
        size_t an = strlen(abs);

        if (strncmp(val, abs, an) == 0) {
            /* 需要把剩余部分连同后移空间一起搬 —— val 就是 extra 里的行内缓冲 */
            char tmp[192];

            (void)snprintf(tmp, sizeof tmp, "%s%s", prefix, val + an);
            (void)snprintf(val, cap, "%s", tmp);
            return;
        }
    }
    (void)snprintf(abs, sizeof abs, "http://%s", ipstr);
    {
        size_t an = strlen(abs);

        if (strncmp(val, abs, an) == 0) {
            char tmp[192];

            (void)snprintf(tmp, sizeof tmp, "%s%s", prefix, val + an);
            (void)snprintf(val, cap, "%s", tmp);
            return;
        }
    }
    if (val[0] == '/') {
        char tmp[192];

        (void)snprintf(tmp, sizeof tmp, "%s%s", prefix, val);
        (void)snprintf(val, cap, "%s", tmp);
    }
}

static void rewrite_cookie_path(char *val, size_t cap, const char *prefix)
{
    char *p = strstr(val, "Path=/");

    if (p == NULL || strstr(val, "Path=/cam/") != NULL)
        return;
    {
        /* "…Path=/x" ⇒ "…Path=" + prefix("/cam/N") + "/x"。
         * p 指向 'P'：头部要带上 "Path="（p-val+5），尾巴从 p+5（路径值）起 ——
         * 早先插在 "Path=" 前面，得到 "/cam/0Path=/"（集成用例 c05 实证）。 */
        char tmp[192];

        (void)snprintf(tmp, sizeof tmp, "%.*s%s%s",
                       (int)(p - val) + 5, val, prefix, p + 5);
        (void)snprintf(val, cap, "%s", tmp);
    }
}

/* 从响应头区提取单值头（行首名匹配，大小写不敏感；值去首尾空白）。 */
static void hdr_get(const char *head, const char *name, char *out, size_t cap)
{
    const char *p = head;
    size_t      nl = strlen(name);

    out[0] = '\0';
    while (p != NULL && *p != '\0') {
        const char *e = strstr(p, "\r\n");

        if (e == NULL)
            break;
        if ((size_t)(e - p) > nl && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            size_t      n = (size_t)(e - v);

            while (n > 0u && (*v == ' ' || *v == '\t')) { v++; n--; }
            while (n > 0u && (v[n - 1u] == ' ' || v[n - 1u] == '\t')) n--;
            if (n >= cap)
                n = cap - 1u;
            memcpy(out, v, n);
            out[n] = '\0';
            return;
        }
        p = e + 2;
    }
}

/* 从响应头区提取全部 Set-Cookie 行（写进 extra，最多 extra 容量允许的条数）。
 * 返回写进 extra 的字节数（0 = 没有 Cookie 行）——调用方据此置 has_extra：
 * build_response 只在 has_extra 时发 extra，早先漏置导致纯 Set-Cookie 响应
 * 的 Cookie 被整段丢弃（集成用例 c05 实证）。 */
static size_t cookies_to_extra(const char *head, const char *prefix, char *extra, size_t cap)
{
    const char *p = head;
    size_t      used = 0u;

    extra[0] = '\0';
    while (p != NULL && *p != '\0') {
        const char *e = strstr(p, "\r\n");
        char        line[224];

        if (e == NULL)
            break;
        if ((size_t)(e - p) > 11 && strncasecmp(p, "Set-Cookie:", 11) == 0) {
            const char *v = p + 11;
            size_t      n = (size_t)(e - v);
            int         w;

            while (n > 0u && (*v == ' ' || *v == '\t')) { v++; n--; }
            if (n >= sizeof line)
                n = sizeof line - 1u;
            memcpy(line, v, n);
            line[n] = '\0';
            rewrite_cookie_path(line, sizeof line, prefix);
            w = snprintf(extra + used, cap - used, "Set-Cookie: %s\r\n", line);
            if (w < 0 || (size_t)w >= cap - used)
                break; /* extra 放不下：丢弃余下的 Cookie（尽力而为） */
            used += (size_t)w;
        }
        p = e + 2;
    }
    return used;
}

/* 响应是否带 chunked（是 ⇒ 502，不做 de-chunk）。头区由 hdr_get 逐值取。 */
static int resp_is_chunked(const char *head)
{
    char v[32];

    hdr_get(head, "Transfer-Encoding", v, sizeof v);
    return strncasecmp(v, "chunked", 7) == 0;
}

/* ---------------------------------------------------------------------------
 * 收包期间用到的**长度受限**小工具（此时 out 还没 NUL 终止，不能用 strstr）
 * ------------------------------------------------------------------------- */

/* 头区结束偏移（"\r\n\r\n" 之后那一个字节）；没收到完整头区返回 0。 */
static size_t find_head_end(const char *buf, size_t len)
{
    size_t i;

    for (i = 0u; i + 3u < len; i++)
        if (buf[i] == '\r' && buf[i + 1u] == '\n' && buf[i + 2u] == '\r' &&
            buf[i + 3u] == '\n')
            return i + 4u;
    return 0u;
}

/* 在 [0, he) 里找下标 from 之后最近的行尾；没有则返回 he。 */
static size_t line_end_at(const char *buf, size_t from, size_t he)
{
    size_t i;

    for (i = from; i + 1u < he; i++)
        if (buf[i] == '\r' && buf[i + 1u] == '\n')
            return i;
    return he;
}

/* 头区里的 Content-Length；没有/非法返回 -1。**必须带长度**：调用时 body 可能
 * 是二进制且缓冲区未终止。 */
static long hdr_clen_bounded(const char *buf, size_t he)
{
    size_t p = 0u;

    while (p < he) {
        size_t e = line_end_at(buf, p, he);

        if (e - p >= 15u && strncasecmp(buf + p, "Content-Length:", 15u) == 0) {
            long v = strtol(buf + p + 15u, NULL, 10);

            return (v >= 0) ? v : -1;
        }
        p = e + 2u;
    }
    return -1;
}

/* 响应体是否为文本（决定要不要做 URL 改写）。二进制体**绝不能**按字符串处理：
 * `strlen()` 会在第一个 0 字节处截断，摄像机页面里的图片/字体就是这么丢的。 */
static int ctype_is_text(const char *ct)
{
    if (ct == NULL || ct[0] == '\0')
        return 0;
    if (strncasecmp(ct, "text/", 5u) == 0)
        return 1;
    if (strcasestr(ct, "javascript") != NULL || strcasestr(ct, "json") != NULL ||
        strcasestr(ct, "xml") != NULL || strcasestr(ct, "x-www-form-urlencoded") != NULL)
        return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 主入口
 * ------------------------------------------------------------------------- */
int iv_web_cam_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req, unsigned cam_ch,
                      char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    char          ipstr[24];
    char          prefix[16];
    char          upreq[2048];
    const char   *rest = NULL;
    unsigned      n = 0u;
    uint32_t      ip_be_val;
    uint16_t      port;
    int           fd;
    int           rc;
    size_t        off = 0u;
    size_t        head_end;
    char          head[4096];
    char          location[224];
    char          ctype[64];
    int           status = 0;
    uint64_t      deadline;

    *out_len = 0u;
    out[0] = '\0';

    if (ctx == NULL || req == NULL || out == NULL || out_len == NULL || rsp == NULL)
        return IV_EINVAL;
    rc = cam_route_parse(req->path, &n, &rest);
    if (rc == IV_OK) {
        /* ② 路径寻址（兼容旧配置）：/cam/<N>/... —— 前缀就是改写基准 */
        (void)snprintf(prefix, sizeof prefix, "/cam/%u", n);
    } else {
        /* ① 端口寻址（首选）：本连接落在 cam_port+cam_ch 上 ⇒ 根路径即摄像机根。
         * 前缀留空：改写时 `http://<ip>/x` 变 `/x`，摄像机当成自己的根，前端那些
         * 根绝对路径（/RPC2、/doc/index.html…）才不会被打成 404。 */
        if (cam_ch >= (unsigned)CAM_CH_N)
            return IV_ENOTSUP;
        n      = cam_ch;
        rest   = req->path;
        prefix[0] = '\0';
    }

    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "POST") != 0) {
        rsp->status = 405;
        (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
        *out_len = (size_t)snprintf(out, cap, "method not allowed\n");
        return IV_OK;
    }

    /* ① 目标＝配置里的第 N 路（Web 不读配置，经本地通道问 ivsboxd） */
    rc = cam_ip_of(ctx, n, ipstr, sizeof ipstr);
    if (rc != IV_OK) {
        IV_LOG_W(CAM_MOD, "cam/%u: no configured ip (rc=%d)", n, rc);
        rsp->status = 502;
        (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
        *out_len = (size_t)snprintf(out, cap, "camera %u not configured\n", n);
        return IV_OK;
    }
    ip_be_val = ip4_parse(ipstr);
    port = (ctx->cam_upstream != 0u) ? ctx->cam_upstream : IV_WEB_CAM_UPSTREAM_DEFAULT;
    if (ip_be_val == 0u || port == 0u) {
        rsp->status = 502;
        (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
        *out_len = (size_t)snprintf(out, cap, "camera %u has an invalid address\n", n);
        return IV_OK;
    }
    /* ② 环回护栏：不许代理到本代理自己 */
    if (ip_be_val == 0x7f000001u && port == (uint16_t)ctx->cam_port) {
        IV_LOG_W(CAM_MOD, "cam/%u: target is the proxy itself, refused", n);
        rsp->status = 502;
        (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
        *out_len = (size_t)snprintf(out, cap, "refused\n");
        return IV_OK;
    }

    /* ③ 拼上游请求（头白名单 + Connection: close） */
    {
        int w = snprintf(upreq, sizeof upreq,
                         "%s %s%s%s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Connection: close\r\n"
                         "User-Agent: ivsbox-web-cam\r\n"
                         "%s%s%s%s%s%s",
                         req->method, rest,
                         (req->query[0] != '\0') ? "?" : "", req->query,
                         ipstr,
                         (req->ctype[0] != '\0') ? "Content-Type: " : "",
                         req->ctype,
                         (req->ctype[0] != '\0') ? "\r\n" : "",
                         (req->cookie_raw[0] != '\0') ? "Cookie: " : "",
                         req->cookie_raw,
                         (req->cookie_raw[0] != '\0') ? "\r\n" : "");
        if (w < 0 || (size_t)w >= sizeof upreq - 64u) {
            rsp->status = 400;
            (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
            *out_len = (size_t)snprintf(out, cap, "request too long\n");
            return IV_OK;
        }
        if (req->auth[0] != '\0') {
            (void)snprintf(upreq + strlen(upreq), sizeof upreq - strlen(upreq),
                           "Authorization: %s\r\n", req->auth);
        }
        if (req->body_len > 0u) {
            (void)snprintf(upreq + strlen(upreq), sizeof upreq - strlen(upreq),
                           "Content-Length: %zu\r\n\r\n", req->body_len);
        } else {
            (void)snprintf(upreq + strlen(upreq), sizeof upreq - strlen(upreq), "\r\n");
        }
    }
    /* 头在 upreq；体（若 POST）不走这段缓冲，连接后单独 send —— 免得 32 KB 的
     * 提交体把 2 KB 的头缓冲撑爆。 */

    /* ④ 连接 + 发送 + 收响应（连接预算 2 s；收发总预算 8 s） */
    fd = upstream_connect(ip_be_val, port);
    if (fd < 0) {
        IV_LOG_W(CAM_MOD, "cam/%u: connect %s:%u failed (%s)", n, ipstr,
                 (unsigned)port, strerror(errno));
        rsp->status = 502;
        (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
        *out_len = (size_t)snprintf(out, cap, "camera unreachable\n");
        return IV_OK;
    }

    /* 收发都要有超时：**阻塞 recv 自身没有时间上限时，下面那个 8 s 总预算根本不生效**
     * —— 摄像机发完 body 不关连接（2026-10-29 板端实证，见收包循环注释），recv 会
     * 一直挂着，把单线程 Web 主循环冻住。1 s 一跳，让总预算能真正判到。 */
    {
        struct timeval tv;

        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }

    {
        size_t sent = 0u;
        size_t total = strlen(upreq);

        while (sent < total) {
            ssize_t w = send(fd, upreq + sent, total - sent, MSG_NOSIGNAL);

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                (void)close(fd);
                rsp->status = 502;
                (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
                *out_len = (size_t)snprintf(out, cap, "upstream write failed\n");
                return IV_OK;
            }
            sent += (size_t)w;
        }
        if (req->body_len > 0u) {
            sent = 0u;
            while (sent < req->body_len) {
                ssize_t w = send(fd, req->body + sent, req->body_len - sent,
                                 MSG_NOSIGNAL);

                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                sent += (size_t)w;
            }
        }
    }

    deadline = (uint64_t)time(NULL) * 1000u + IV_WEB_CAM_TOTAL_MS;
    /* 这两个出块后还要用（判"上游声明的长度有没有收满"），所以声明在块外。
     * `he == 0` ＝ 头区还没收齐（此时整段不能当字符串用）；`clen < 0` ＝ 没该头。 */
    size_t he   = 0u;
    long   clen = -1;
    {
        while (off < cap - 1u) {
            ssize_t r = recv(fd, out + off, cap - 1u - off, 0);

            if (r < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* SO_RCVTIMEO 1 s 一跳：判总预算，别在这儿死等（见下） */
                    if ((uint64_t)time(NULL) * 1000u > deadline)
                        break;
                    continue;
                }
                break;
            }
            if (r == 0)
                break; /* 上游按 Connection: close 收尾（部分摄像机会） */
            off += (size_t)r;
            if (he == 0u) {
                size_t h = find_head_end(out, off);

                if (h != 0u) {
                    he   = h;
                    clen = hdr_clen_bounded(out, h);
                }
            }
            /* ① 有 Content-Length 且已收满 ⇒ 立刻收工。
             * 板端实证 2026-10-29（大华 192.168.1.108）：**请求里写了
             * `Connection: close`，它发完 Content-Length 指定的字节后仍然不关连接**
             * ⇒ 只等 EOF 的写法会把单线程 Web 主循环冻死 8 s 以上，心跳过期后
             * monitor-web.sh 杀掉重启（浏览器看到 `Connection reset by peer`，
             * 现场表现就是"远程看摄像机打不开，还把整个 Web 拖挂 10 秒"）。 */
            if (he != 0u && clen >= 0 && off >= he + (size_t)clen)
                break;
            if ((uint64_t)time(NULL) * 1000u > deadline)
                break; /* 超预算：有多少算多少（下面按截断应答处理） */
        }
    }
    (void)close(fd);
    out[off] = '\0';

    /* ⑤ 上游声明的长度没收满 ⇒ **必须明确报错**，绝不能把半份正文当成功回出去。
     * 2026-10-29 现场：大华的语言表 `/custom_lang/SimpChinese.txt` 是 **391087 B**
     * 的 JSON，而缓冲当时只有 128 KiB —— 循环收满即停、状态照样回 200 ⇒ 浏览器拿到
     * 被截断的半个 JSON ⇒ `JSON.parse` 失败 ⇒ 语言表空 ⇒ 摄像机界面全是
     * `med.MainStream` 这种未翻译的 key（用户原话："108 明明是中文界面，为什么你
     * 变成了英文"）。缓冲已抬到 512 KiB，这条闸门保证"再顶穿时说话，不是装没事"。 */
    if (he != 0u && clen >= 0 && off < he + (size_t)clen) {
        IV_LOG_W(CAM_MOD,
                 "cam/%u: response truncated (%ld B declared, %zu B received, cap %zu)",
                 n, clen, (off > he) ? (off - he) : 0u, cap);
        rsp->status = 502;
        (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
        *out_len = (size_t)snprintf(out, cap,
                                    "camera response too large for the proxy buffer\n");
        return IV_OK;
    }

    /* ⑤ 拆应答：状态行 + 头 + 体 */
    {
        char *body = strstr(out, "\r\n\r\n");

        if (body == NULL || off < 12u || strncmp(out, "HTTP/1.", 7) != 0) {
            rsp->status = 502;
            (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
            *out_len = (size_t)snprintf(out, cap, "bad upstream response\n");
            return IV_OK;
        }
        head_end = (size_t)(body - out);
        if (head_end + 2u >= sizeof head) {
            head_end = sizeof head - 3u;
        }
        memcpy(head, out, head_end);
        /* 末尾**补一个 CRLF**：`hdr_get()`/`cookies_to_extra()`/`resp_is_chunked()`
         * 都按行匹配（先找齐本行的 "\r\n" 再比对名字），而 head_end 指向的正是
         * "\r\n\r\n" 的第一个字节 —— 所以头区的**最后一行**在这个副本里没有行尾，
         * 不补就会被整行漏掉。板端实证 2026-10-29：大华把 `Content-Type` 放在最后，
         * 于是类型恒为空 ⇒ 回 `application/octet-stream` ⇒ 浏览器把摄像机页面当
         * 文件下载（"远程查看摄像机网页"直接变成"下载一个 html"）。 */
        head[head_end]      = '\r';
        head[head_end + 1u] = '\n';
        head[head_end + 2u] = '\0';
        status = atoi(out + 8);

        if (status < 100 || status > 599 || resp_is_chunked(head)) {
            rsp->status = 502;
            (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
            *out_len = (size_t)snprintf(out, cap, "unsupported upstream response\n");
            return IV_OK;
        }

        /* ⑥ 重写与回填（文本体就地改写；二进制体原样按长度搬运） */
        {
            char  *b = body + 4;
            size_t blen = off - head_end - 4u;
            int    is_text;

            hdr_get(head, "Content-Type", ctype, sizeof ctype);
            is_text = ctype_is_text(ctype);
            hdr_get(head, "Location", location, sizeof location);
            if (location[0] != '\0')
                rewrite_location(location, sizeof location, ipstr, port, prefix);
            if (is_text != 0)
                rewrite_abs_urls(b, cap - (size_t)(b - out) - 1u, ipstr, port, prefix);
            /* 长度只对文本体重算（改写只会变短）；二进制体（图片/字体/打包 JS）
             * **一律保持实测字节数**：拿 strlen() 量二进制会在第一个 0 字节处截断，
             * 摄像机页面的图标、样式表就是这么丢的。 */
            if (is_text != 0)
                blen = strlen(b);

            rsp->status = status;
            (void)snprintf(rsp->ctype, sizeof rsp->ctype, "%s",
                           (ctype[0] != '\0') ? ctype : "application/octet-stream");
            rsp->has_extra = 0;
            rsp->extra[0] = '\0';
            if (location[0] != '\0') {
                int w = snprintf(rsp->extra, sizeof rsp->extra,
                                 "Location: %s\r\n", location);
                if (w > 0)
                    rsp->has_extra = 1;
            }
            {
                size_t ck = cookies_to_extra(head, prefix,
                                             rsp->extra + strlen(rsp->extra),
                                             (rsp->has_extra != 0)
                                                 ? sizeof rsp->extra - strlen(rsp->extra)
                                                 : sizeof rsp->extra);

                if (ck > 0u)
                    rsp->has_extra = 1;
            }

            /* 体已在 out 内就地重写（b == out+head_end+4）：把它搬到缓冲前部 */
            if (b != out)
                memmove(out, b, blen);
            *out_len = blen;
            out[blen] = '\0';
        }
    }

    IV_LOG_I(CAM_MOD, "cam/%u -> %s:%u %s = %d (%zu B)", n, ipstr, (unsigned)port,
             rest, rsp->status, *out_len);
    return IV_OK;
}
