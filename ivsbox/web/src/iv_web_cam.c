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
 *   - 响应**整体缓冲**，上限 `IV_WEB_CAM_RSP_MAX`（8 MiB，按需扩容）；
 *     超限 502，绝不静默截断。
 *   - **WebSocket 升级（2026-10-30 新增，实时预览的唯一通路）**：请求带
 *     `Upgrade: websocket` 时改发 `Connection: Upgrade` 并透传
 *     `Sec-WebSocket-Key/Version/Protocol/Extensions`；上游回 **101** 后**立刻**跳出
 *     HTTP 解析，连接转入裸字节双向中继（相位 `IV_WEB_UP_TUNNEL`，泵在 main.c）。
 *     理由：大华/海康的 Web3.0 预览页是 `new WebSocket("ws://host/rtspoverwebsocket")`
 *     ＋ 前端自己解帧，**不是** MJPEG、也不是 HTTP 长流；旧实现遇到 101 只能等 8 s
 *     预算到点回 504，视频永远起不来（现场表现＝"WEB 端视频流太慢/出不来"）。
 *   - **仍不承诺**：MJPEG、无 Content-Length 的 HTTP 长流、需要 NPAPI/ActiveX 的
 *     插件页（现代浏览器也装不了）。
 *   - 超时：连接 2 s、响应总预算 8 s；**中继阶段不用这个预算**，改用
 *     `IV_WEB_WS_IDLE_MS`（120 s 双向都无字节才收）——直播是长连接，8 s 就 504 等于没有视频。
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
 * 主入口：三段式 begin / event / abort
 *
 * ============================ 为什么不是"一个函数做完" ============================
 * 2026-10-30 现场：经隧道打开大华 108 的设置页，界面一直转圈、刷新不出来。
 * 量出来的事实（板端对摄像机直连 vs 过反代，同一批 35 个资源）：
 *     直连 2.46 s     反代 6.40 s（＝每个请求 110～250 ms **一个接一个**地等）
 * 根因不是带宽，是**同步**：旧实现把"connect → send → recv 到收满"整段做在
 * `handle_req()` 里，单线程主循环在这几百毫秒里谁都服务不了。浏览器对一个 origin
 * 只开 6 条连接，于是页面加载变成 6 条一波、一波 6 个 RTT。
 * 现在把上游 fd 挂进同一个 poll：等待期间主循环照常受理其它连接，多个上游天然重叠。
 *
 * 相位：CONNECT（非阻塞 connect 中） → SEND（发请求头/体） → RECV（收响应）
 * 收工条件与同步版完全一致：**收满 Content-Length 立刻收**（大华发完 body 不关连接，
 * 只等 EOF 会挂死），没有该头就等 EOF。
 * 预算：总共 IV_WEB_CAM_TOTAL_MS（8 s），到点按 504 收尾 —— 绝不无限挂着。
 * ------------------------------------------------------------------------- */

/* 统一的错误应答：正文写进 out（＝ wbuf + RSP_HEAD_OFF），状态码给 rsp。 */
static int cam_err(char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp,
                   int status, const char *msg)
{
    rsp->status = status;
    (void)snprintf(rsp->ctype, sizeof rsp->ctype, "text/plain");
    *out_len = (size_t)snprintf(out, cap, "%s\n", msg);
    return IV_OK;
}

static void cam_close_up(iv_web_conn_t *c)
{
    if (c->up_fd >= 0) {
        (void)close(c->up_fd);
        c->up_fd = -1;
    }
    c->up_phase = (uint8_t)IV_WEB_UP_NONE;
}

/* 连接关闭时由 main.c 调用：上游 fd 必须一起关，否则一场页面刷新能泄漏几十个 fd。 */
void iv_web_cam_abort(iv_web_conn_t *c)
{
    if (c != NULL)
        cam_close_up(c);
}

/* 把请求头与请求体推进上游。IV_EAGAIN = 内核发缓冲满了，等下一轮 POLLOUT。 */
static int cam_send_req(iv_web_conn_t *c)
{
    while (c->up_req_sent < c->up_req_len) {
        ssize_t w = send(c->up_fd, c->up_req + c->up_req_sent,
                         c->up_req_len - c->up_req_sent, MSG_NOSIGNAL);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return IV_EAGAIN;
            return IV_EIO;
        }
        if (w == 0)
            return IV_EAGAIN;
        c->up_req_sent += (size_t)w;
    }
    while (c->up_body_sent < c->up_body_len) {
        ssize_t w = send(c->up_fd, c->rbuf + c->up_body_off + c->up_body_sent,
                         c->up_body_len - c->up_body_sent, MSG_NOSIGNAL);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return IV_EAGAIN;
            return IV_EIO;
        }
        if (w == 0)
            return IV_EAGAIN;
        c->up_body_sent += (size_t)w;
    }
    return IV_OK;
}

/* 收工：拆应答 + 改写 + 填 rsp。以下是同步版原样搬过来的那段（逻辑未动），
 * 只是状态从局部变量改为 conn 上的字段（`up_*`），以便跨 poll 轮次活着。 */
static int cam_finish(iv_web_conn_t *c, char *out, size_t cap, size_t *out_len,
                      iv_web_resp_t *rsp)
{
    const char *ipstr  = c->up_ip;
    const char *prefix = c->up_prefix;
    uint16_t    port   = c->up_port;
    unsigned    n      = (unsigned)c->up_ch;
    size_t      he     = c->up_he;
    long        clen   = c->up_clen;
    size_t      off    = c->up_off;
    size_t      head_end;
    char        head[4096];
    char        location[224];
    char        ctype[64];
    int         status = 0;

    cam_close_up(c);
    out[off] = '\0';

    /* ⑤ 上游声明的长度没收满 ⇒ **必须明确报错**，绝不能把半份正文当成功回出去。
     * 2026-10-29 现场：大华的语言表 `/custom_lang/SimpChinese.txt` 是 **391087 B**
     * 的 JSON，而缓冲当时只有 128 KiB —— 循环收满即停、状态照样回 200 ⇒ 浏览器拿到
     * 被截断的半个 JSON ⇒ `JSON.parse` 失败 ⇒ 语言表空 ⇒ 摄像机界面全是
     * `med.MainStream` 这种未翻译的 key（用户原话："108 明明是中文界面，为什么你
     * 变成了英文"）。缓冲已抬到 512 KiB，这条闸门保证"再顶穿时说话，不是装没事"。 */
    if (he != 0u && clen >= 0 && off < he + (size_t)clen) {
        IV_LOG_W(CAM_MOD,
                 "cam/%u: response truncated (%s: %ld B declared, %zu B received, cap %zu)",
                 n, c->up_path, clen, (off > he) ? (off - he) : 0u, cap);
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
            /* 摄像机资源**不吃媒体限速**：`--media-bps`（默认 256 KiB/s）是给录像
             * 下载的，拿它卡页面资源等于给每个 300 KB 的语言表/JS 白加 1.2 s。
             * 2026-10-30 现场：这正是"点设置页一直转圈"的第二个来源。 */
            rsp->no_throttle = 1;
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
             c->up_path, rsp->status, *out_len);
    return IV_OK;
}

/* 头部收齐后按声明的长度扩容。**只在大响应时发生**：板端实测大华预览页要一份
 * 957347 B 的模块，固定 512 KiB 只能 502，页面就永远转圈。
 * `out` 是 `c->wbuf + 偏移`，realloc 之后必须按同一偏移重新取指针 —— 老指针会悬空。 */
static int cam_ensure(char **out, size_t *cap, iv_web_conn_t *c, size_t need)
{
    size_t hoff = (size_t)(*out - c->wbuf);
    char  *nb;

    if (need <= *cap)
        return IV_OK;
    if (hoff + need > IV_WEB_CAM_RSP_MAX)
        return IV_ERANGE;
    nb = (char *)realloc(c->wbuf, hoff + need);
    if (nb == NULL)
        return IV_ENOMEM;
    c->wbuf = nb;
    c->wcap = hoff + need;
    *out    = nb + hoff;
    *cap    = need;
    return IV_OK;
}

/* 101 到手 ⇒ 转入裸字节中继。
 * **必须把 up_off 全部带走**：摄像机常在一次 read 里把 101 头和首帧一起吐出来，
 * 只交头就等于丢首帧（画面停在 loading）。调用方原样发给客户端，不许再拼 HTTP 头
 * （给 101 补 Content-Length/Connection 会让握手当场作废）。 */
static int cam_tunnel_start(iv_web_conn_t *c, size_t *out_len)
{
    c->up_phase       = (uint8_t)IV_WEB_UP_TUNNEL;
    c->up_deadline_ms = iv_web_now_ms() + (uint64_t)IV_WEB_WS_IDLE_MS;
    *out_len          = c->up_off;
    IV_LOG_I(CAM_MOD, "cam/%u: %s:%u %s -> 101 websocket (%zu B buffered, relay starts)",
             (unsigned)c->up_ch, c->up_ip, (unsigned)c->up_port, c->up_path, c->up_off);
    return IV_WEB_CAM_TUNNEL;
}

/* 收上游响应：一直读到 EAGAIN（返回 IV_EAGAIN 让主循环继续轮）、收满、EOF 或超预算。 */
static int cam_recv_rsp(iv_web_conn_t *c, char *out, size_t cap, size_t *out_len,
                        iv_web_resp_t *rsp)
{
    for (;;) {
        ssize_t r;
        int     rc;

        if (c->up_off >= cap - 1u)
            break; /* 缓冲满：交给 cam_finish 的截断闸门说话 */
        r = recv(c->up_fd, out + c->up_off, cap - 1u - c->up_off, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return IV_EAGAIN;
            break;
        }
        if (r == 0)
            break; /* 上游按 Connection: close 收尾（部分摄像机会） */
        c->up_off += (size_t)r;
        if (c->up_he == 0u) {
            size_t h = find_head_end(out, c->up_off);

            if (h != 0u) {
                c->up_he   = h;
                /* ① 101 Switching Protocols：**没有 Content-Length**，等下去只会等到
                 * 8 s 预算到点回 504（2026-10-30 实测就是这条把直播打死）。 */
                if (c->up_ws != 0 && h >= 12u && strncmp(out, "HTTP/1.", 7) == 0 &&
                    memcmp(out + 9, "101", 3u) == 0)
                    return cam_tunnel_start(c, out_len);
                c->up_clen = hdr_clen_bounded(out, h);
                /* 声明的正文放不下 ⇒ 先扩容再继续收，而不是等收满丢掉 */
                if (c->up_clen > 0) {
                    rc = cam_ensure(&out, &cap, c,
                                    c->up_he + (size_t)c->up_clen + 1u);
                    if (rc == IV_ERANGE) {
                        IV_LOG_W(CAM_MOD,
                                 "cam/%u: %s declares %ld B, above the %u B proxy cap",
                                 (unsigned)c->up_ch, c->up_path, c->up_clen,
                                 (unsigned)IV_WEB_CAM_RSP_MAX);
                        break; /* 交给 cam_finish 的截断闸门回 502 */
                    }
                    if (rc != IV_OK) {
                        cam_close_up(c);
                        return cam_err(out, cap, out_len, rsp, 502,
                                       "out of memory growing the proxy buffer");
                    }
                }
            }
        }
        /* ① 有 Content-Length 且已收满 ⇒ 立刻收工。板端实证 2026-10-29（大华
         * 192.168.1.108）：**请求里写了 `Connection: close`，它发完 Content-Length
         * 指定的字节后仍然不关连接** ⇒ 只等 EOF 的写法会把主循环冻死 8 s 以上。 */
        if (c->up_he != 0u && c->up_clen >= 0 &&
            c->up_off >= c->up_he + (size_t)c->up_clen)
            break;
        if (iv_web_now_ms() > c->up_deadline_ms)
            break; /* 超预算：有多少算多少（cam_finish 按截断处理） */
    }
    return cam_finish(c, out, cap, out_len, rsp);
}

/* 第一段：解析目标 + 拼上游请求 + 发起非阻塞 connect。
 * IV_EAGAIN = 已挂上，等主循环推事件；IV_OK = 当场就有应答（405/502/404…）。 */
int iv_web_cam_begin(iv_web_ctx_t *ctx, const iv_web_req_t *req, unsigned cam_ch,
                     iv_web_conn_t *c, char *out, size_t cap, size_t *out_len,
                     iv_web_resp_t *rsp)
{
    char           ipstr[24];
    char           prefix[16];
    const char    *rest = NULL;
    unsigned       n = 0u;
    uint32_t       ip_be_val;
    uint16_t       port;
    struct sockaddr_in sa;
    int            fd;
    int            rc;
    int            on = 1;

    *out_len = 0u;
    out[0] = '\0';

    if (ctx == NULL || req == NULL || c == NULL || out == NULL || out_len == NULL ||
        rsp == NULL)
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
        return cam_err(out, cap, out_len, rsp, 502, "camera not configured");
    }
    ip_be_val = ip4_parse(ipstr);
    port = (ctx->cam_upstream != 0u) ? ctx->cam_upstream : IV_WEB_CAM_UPSTREAM_DEFAULT;
    if (ip_be_val == 0u || port == 0u)
        return cam_err(out, cap, out_len, rsp, 502, "camera has an invalid address");
    /* ② 环回护栏：不许代理到本代理自己 */
    if (ip_be_val == 0x7f000001u && port == (uint16_t)ctx->cam_port) {
        IV_LOG_W(CAM_MOD, "cam/%u: target is the proxy itself, refused", n);
        return cam_err(out, cap, out_len, rsp, 502, "refused");
    }

    /* ②' 升级请求？（大华/海康的**实时预览**都走 WebSocket：
     * `/rtspoverwebsocket`、`/httpprivateoverwebsocket`。）只有客户端明确要升，
     * 我们才换握手头；普通页面资源一律照旧 `Connection: close`。 */
    c->up_ws     = (strcasestr(req->upgrade, "websocket") != NULL) ? 1u : 0u;
    c->ws_on     = 0u;
    c->ws_ul_len = 0u;
    c->ws_ul_sent = 0u;
    c->ws_dl_len = 0u;
    c->ws_dl_sent = 0u;

    /* ③ 拼上游请求（头白名单 ＋ WebSocket 握手头），存进 conn 供分轮次发送 */
    {
        int w = snprintf(c->up_req, sizeof c->up_req,
                         "%s %s%s%s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "%s"
                         "User-Agent: ivsbox-web-cam\r\n"
                         "%s%s%s%s%s%s",
                         req->method, rest,
                         (req->query[0] != '\0') ? "?" : "", req->query,
                         ipstr,
                         /* 升级请求**绝不能**带 `Connection: close`：那是让摄像机
                          * 在应答完就断，101 之后还怎么中继帧。 */
                         (c->up_ws != 0) ? "Connection: Upgrade\r\n" : "Connection: close\r\n",
                         (req->ctype[0] != '\0') ? "Content-Type: " : "",
                         req->ctype,
                         (req->ctype[0] != '\0') ? "\r\n" : "",
                         (req->cookie_raw[0] != '\0') ? "Cookie: " : "",
                         req->cookie_raw,
                         (req->cookie_raw[0] != '\0') ? "\r\n" : "");

        if (w < 0 || (size_t)w >= sizeof c->up_req - 128u)
            return cam_err(out, cap, out_len, rsp, 400, "request too long");
        if (req->auth[0] != '\0') {
            (void)snprintf(c->up_req + strlen(c->up_req),
                           sizeof c->up_req - strlen(c->up_req),
                           "Authorization: %s\r\n", req->auth);
        }
        /* 握手四件套原样透传（key 缺了摄像机回 400；protocol/extensions 缺了
         * 前端与摄像机对帧格式的协商会不一致）。版本缺省补 13。 */
        if (c->up_ws != 0) {
            (void)snprintf(c->up_req + strlen(c->up_req),
                           sizeof c->up_req - strlen(c->up_req),
                           "Upgrade: %s\r\n"
                           "Sec-WebSocket-Version: %s\r\n"
                           "Sec-WebSocket-Key: %s\r\n",
                           (req->upgrade[0] != '\0') ? req->upgrade : "websocket",
                           (req->ws_ver[0] != '\0') ? req->ws_ver : "13",
                           req->ws_key);
            if (req->ws_proto[0] != '\0') {
                (void)snprintf(c->up_req + strlen(c->up_req),
                               sizeof c->up_req - strlen(c->up_req),
                               "Sec-WebSocket-Protocol: %s\r\n", req->ws_proto);
            }
            if (req->ws_ext[0] != '\0') {
                (void)snprintf(c->up_req + strlen(c->up_req),
                               sizeof c->up_req - strlen(c->up_req),
                               "Sec-WebSocket-Extensions: %s\r\n", req->ws_ext);
            }
        }
        if (req->body_len > 0u) {
            (void)snprintf(c->up_req + strlen(c->up_req),
                           sizeof c->up_req - strlen(c->up_req),
                           "Content-Length: %zu\r\n\r\n", req->body_len);
        } else {
            (void)snprintf(c->up_req + strlen(c->up_req),
                           sizeof c->up_req - strlen(c->up_req), "\r\n");
        }
    }

    /* ④ 记下改写上下文与请求体位置，然后发起非阻塞 connect */
    (void)snprintf(c->up_ip, sizeof c->up_ip, "%s", ipstr);
    (void)snprintf(c->up_prefix, sizeof c->up_prefix, "%s", prefix);
    (void)snprintf(c->up_path, sizeof c->up_path, "%s", (rest != NULL) ? rest : "/");
    c->up_port      = port;
    c->up_ch        = (uint8_t)n;
    c->up_req_len   = strlen(c->up_req);
    c->up_req_sent  = 0u;
    c->up_body_off  = (req->body_len > 0u && req->body != NULL)
                          ? (size_t)(req->body - c->rbuf)
                          : 0u;
    c->up_body_len  = req->body_len;
    c->up_body_sent = 0u;
    c->up_off       = 0u;
    c->up_he        = 0u;
    c->up_clen      = -1;
    c->up_deadline_ms = iv_web_now_ms() + (uint64_t)IV_WEB_CAM_TOTAL_MS;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return cam_err(out, cap, out_len, rsp, 502, "camera unreachable");
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(fd);
        return cam_err(out, cap, out_len, rsp, 502, "camera unreachable");
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    /* ip_be_val 是"按大端读"的数值（0x7F000001），in_addr 要的是内存序字节 ⇒ htonl */
    sa.sin_addr.s_addr = htonl(ip_be_val);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        if (errno != EINPROGRESS) {
            IV_LOG_W(CAM_MOD, "cam/%u: connect %s:%u failed (%s)", n, ipstr,
                     (unsigned)port, strerror(errno));
            (void)close(fd);
            return cam_err(out, cap, out_len, rsp, 502, "camera unreachable");
        }
        c->up_fd    = fd;
        c->up_phase = (uint8_t)IV_WEB_UP_CONNECT;
        return IV_EAGAIN; /* 交给主循环里的 poll 等可写 */
    }
    c->up_fd    = fd;
    c->up_phase = (uint8_t)IV_WEB_UP_SEND;
    return IV_EAGAIN; /* 已连上，下一轮（或本轮末）推进发送 */
}

/* 第二段：由主循环在上游 fd 有事件（可写/可读/出错）或超时时调用。
 * IV_EAGAIN = 还需等；IV_OK = 已填好 rsp（正文在 out 里）。 */
int iv_web_cam_event(iv_web_ctx_t *ctx, iv_web_conn_t *c, char *out, size_t cap,
                     size_t *out_len, iv_web_resp_t *rsp)
{
    int rc;

    (void)ctx;
    if (c == NULL || out == NULL || out_len == NULL || rsp == NULL)
        return IV_EINVAL;
    if (c->up_fd < 0 || c->up_phase == (uint8_t)IV_WEB_UP_NONE)
        return IV_EINVAL;
    /* 中继阶段归 main.c 的 ws 泵推进；这里不参与，更不能按 8 s 预算 504。 */
    if (c->up_phase == (uint8_t)IV_WEB_UP_TUNNEL)
        return IV_EAGAIN;
    if (iv_web_now_ms() > c->up_deadline_ms) {
        IV_LOG_W(CAM_MOD, "cam/%u: upstream %s:%u timeout after %u ms", c->up_ch,
                 c->up_ip, (unsigned)c->up_port, (unsigned)IV_WEB_CAM_TOTAL_MS);
        cam_close_up(c);
        return cam_err(out, cap, out_len, rsp, 504, "camera timeout");
    }

    if (c->up_phase == (uint8_t)IV_WEB_UP_CONNECT) {
        int       err  = 0;
        socklen_t elen = sizeof err;

        if (getsockopt(c->up_fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            IV_LOG_W(CAM_MOD, "cam/%u: connect %s:%u failed (%s)", c->up_ch, c->up_ip,
                     (unsigned)c->up_port, strerror((err != 0) ? err : errno));
            cam_close_up(c);
            return cam_err(out, cap, out_len, rsp, 502, "camera unreachable");
        }
        c->up_phase = (uint8_t)IV_WEB_UP_SEND;
    }
    if (c->up_phase == (uint8_t)IV_WEB_UP_SEND) {
        rc = cam_send_req(c);
        if (rc == IV_EAGAIN)
            return IV_EAGAIN;
        if (rc != IV_OK) {
            cam_close_up(c);
            return cam_err(out, cap, out_len, rsp, 502, "upstream write failed");
        }
        c->up_phase = (uint8_t)IV_WEB_UP_RECV;
    }
    if (c->up_phase == (uint8_t)IV_WEB_UP_RECV)
        return cam_recv_rsp(c, out, cap, out_len, rsp);
    return IV_EINVAL;
}
