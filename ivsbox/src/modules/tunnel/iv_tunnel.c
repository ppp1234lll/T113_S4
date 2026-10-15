/*
 * 受控远程访问通道（libivmodules，功能开发计划 M8-S8.1~S8.3）
 * 头文件已写明"是什么 / 为什么这么做"，本文件只写实现要点与坑。
 *
 * 实现纪律（与本工程其它模块一致）：
 *   - 零 malloc、时间由调用方注入；
 *   - 拒绝路径一律先记原因再返回，绝不停在"看起来成功"；
 *   - 密码学只做**调用**（OpenSSL EVP），不自己做。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "ivsbox/iv_tunnel.h"

/* ---------------------------------------------------------------------------
 * 原因名与返回码
 * ------------------------------------------------------------------------- */
const char *iv_tunnel_reason_name(iv_tunnel_reason_t r)
{
    switch (r) {
    case IV_TUNNEL_OK:        return "OK";
    case IV_TUNNEL_EBADARG:   return "EBADARG";
    case IV_TUNNEL_ENOIDENT:  return "ENOIDENT";
    case IV_TUNNEL_ENOSERVER: return "ENOSERVER";
    case IV_TUNNEL_EUNMAPPED: return "EUNMAPPED";
    case IV_TUNNEL_EHOST:     return "EHOST";
    case IV_TUNNEL_EIO:       return "EIO";
    case IV_TUNNEL_ESTATE:    return "ESTATE";
    default:                  return "UNKNOWN";
    }
}

static int rc_of(iv_tunnel_reason_t r)
{
    switch (r) {
    case IV_TUNNEL_OK:        return IV_OK;
    case IV_TUNNEL_EBADARG:   return IV_EINVAL;
    case IV_TUNNEL_ENOIDENT:  return IV_EPROTO;
    case IV_TUNNEL_ENOSERVER: return IV_ENOENT;
    case IV_TUNNEL_EUNMAPPED: return IV_ENOENT;
    case IV_TUNNEL_EHOST:     return IV_EPROTO;
    case IV_TUNNEL_EIO:       return IV_EIO;
    case IV_TUNNEL_ESTATE:    return IV_ESTATE;
    default:                  return IV_EFAIL;
    }
}

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

/* 纯十进制串 → u64（位数 1..20，逐位防溢出） */
static int dec_u64(const char *s, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    size_t   i;
    if (n == 0u || n > 20u) return -1;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < '0' || c > '9') return -1;
        if (v > (UINT64_MAX - 9u) / 10u) return -1;
        v = v * 10u + (uint64_t)(c - '0');
    }
    *out = v;
    return 0;
}

/* 字节 → 小写 hex（n 字节产生 2n 个字符 + NUL） */
static void to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2u]      = H[(b[i] >> 4) & 0xfu];
        out[i * 2u + 1u] = H[b[i] & 0xfu];
    }
    out[n * 2u] = '\0';
}

/*
 * MD5 派生：hex( MD5(a + b + c)[0..8) ) → 16 个小写字符 + NUL。
 * 与 BIRD 运维箱 `utils.GenerateSTCPKey(sn, token)` 同源（c 为空串时完全等价）。
 * 用 OpenSSL EVP 而不是自写 MD5：纪律是"不自写密码学"（架构 §14）。
 */
static int md5_hex16(const char *a, const char *b, const char *c, char out[17])
{
    EVP_MD_CTX   *ctx;
    unsigned char dg[EVP_MAX_MD_SIZE];
    unsigned int  n = 0;
    int           ok = 0;

    ctx = EVP_MD_CTX_new();
    if (ctx == NULL) return -1;
    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) == 1) {
        if (a != NULL) EVP_DigestUpdate(ctx, a, strlen(a));
        if (b != NULL) EVP_DigestUpdate(ctx, b, strlen(b));
        if (c != NULL) EVP_DigestUpdate(ctx, c, strlen(c));
        if (EVP_DigestFinal_ex(ctx, dg, &n) == 1 && n >= 8u) ok = 1;
    }
    EVP_MD_CTX_free(ctx);
    if (!ok) return -1;
    to_hex(dg, 8u, out);
    return 0;
}

/* ---------------------------------------------------------------------------
 * 服务名（白名单）
 * ------------------------------------------------------------------------- */

/* 下标段：纯十进制、无前导零、< limit */
static int parse_index(const char *s, size_t n, unsigned limit, unsigned *out)
{
    uint64_t v = 0;
    if (n == 0u || n > 5u) return IV_EINVAL;
    if (n > 1u && s[0] == '0') return IV_EINVAL; /* 前导零不收：规范唯一 */
    if (dec_u64(s, n, &v) != 0) return IV_EINVAL;
    if (v >= (uint64_t)limit) return IV_ERANGE;
    *out = (unsigned)v;
    return IV_OK;
}

int iv_tunnel_svc_parse(const char *s, size_t len, iv_tunnel_svc_t *out)
{
    if (s == NULL || out == NULL) return IV_EINVAL;
    if (len == 0u) return IV_EINVAL;

    if (len == 3u && memcmp(s, "web", 3u) == 0) {
        out->kind = IV_TUNNEL_SVC_WEB;
        out->index = 0u;
        return IV_OK;
    }
    if (len > 7u && memcmp(s, "camera:", 7u) == 0) {
        unsigned idx = 0;
        int rc = parse_index(s + 7, len - 7u, IV_TUNNEL_CAM_MAX, &idx);
        if (rc != IV_OK) return rc;
        out->kind = IV_TUNNEL_SVC_CAMERA;
        out->index = idx;
        return IV_OK;
    }
    if (len > 6u && memcmp(s, "media:", 6u) == 0) {
        unsigned idx = 0;
        int rc = parse_index(s + 6, len - 6u, IV_TUNNEL_MEDIA_MAX, &idx);
        if (rc != IV_OK) return rc;
        out->kind = IV_TUNNEL_SVC_MEDIA;
        out->index = idx;
        return IV_OK;
    }
    /* 到这里的是：ip:port、域名、URL、`web:80`、`camera:`（空下标）等一切不在
     * 白名单里的形态 —— 一律拒，不做"尽力解析"。 */
    return IV_EINVAL;
}

int iv_tunnel_svc_format(const iv_tunnel_svc_t *svc, char *out, size_t cap)
{
    if (svc == NULL || out == NULL || cap == 0u) return IV_EINVAL;
    out[0] = '\0';
    switch (svc->kind) {
    case IV_TUNNEL_SVC_WEB:
        if (cap < 4u) return IV_ERANGE;
        memcpy(out, "web", 4u);
        return IV_OK;
    case IV_TUNNEL_SVC_CAMERA:
    case IV_TUNNEL_SVC_MEDIA: {
        const char *pfx = (svc->kind == IV_TUNNEL_SVC_CAMERA) ? "camera:" : "media:";
        size_t      pn = strlen(pfx);
        char        num[8];
        size_t      nn = 0, i;
        unsigned    v = svc->index;
        if (cap < pn + 2u) return IV_ERANGE;
        do {
            num[nn++] = (char)('0' + (v % 10u));
            v /= 10u;
        } while (v != 0u && nn < sizeof num);
        if (cap < pn + nn + 1u) return IV_ERANGE;
        memcpy(out, pfx, pn);
        for (i = 0; i < nn; i++) out[pn + i] = num[nn - 1u - i];
        out[pn + nn] = '\0';
        return IV_OK;
    }
    default:
        return IV_EINVAL;
    }
}

/* ---------------------------------------------------------------------------
 * secretKey 派生
 * ------------------------------------------------------------------------- */
int iv_tunnel_secret(const char *sn, const char *token, const char *svc,
                     char *out, size_t cap)
{
    if (sn == NULL || token == NULL || out == NULL) return IV_EINVAL;
    if (cap < IV_TUNNEL_SECRET_MAX) return IV_ERANGE;
    if (md5_hex16(sn, token, svc, out) != 0) return IV_EFAIL;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * frpc.toml 渲染（纯函数）
 * ------------------------------------------------------------------------- */

/* 追加格式化文本；缓冲不足返回 -1（*pos 不变） */
static int app(char *out, size_t cap, size_t *pos, const char *fmt, ...)
{
    int      n;
    va_list  ap;

    va_start(ap, fmt);
    n = vsnprintf(out + *pos, (cap > *pos) ? (cap - *pos) : 0u, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= ((cap > *pos) ? (cap - *pos) : 0u)) return -1;
    *pos += (size_t)n;
    return 0;
}

/* 环回判定：网络序首字节 0x7F（127.0.0.0/8） */
static int is_loopback(uint32_t ip_be)
{
    return ((ip_be >> 24) & 0xffu) == 0x7fu;
}

int iv_tunnel_render(const char *sn, const char *token, const char *server_addr,
                     uint16_t server_port, const iv_tunnel_resolver_t *resolver,
                     const iv_tunnel_svc_t *svcs, unsigned n,
                     char *out, size_t cap, size_t *out_len,
                     iv_tunnel_reason_t *why)
{
    size_t   pos = 0;
    unsigned i;

    if (out == NULL || cap == 0u || out_len == NULL || svcs == NULL) {
        if (why != NULL) *why = IV_TUNNEL_EBADARG;
        return IV_EINVAL;
    }
    *out_len = 0;
    out[0] = '\0';

    if (n == 0u || n > IV_TUNNEL_PROXIES_MAX) {
        if (why != NULL) *why = IV_TUNNEL_EBADARG;
        return IV_EINVAL;
    }
    if (sn == NULL || sn[0] == '\0' || token == NULL || token[0] == '\0') {
        if (why != NULL) *why = IV_TUNNEL_ENOIDENT;
        return IV_EPROTO;
    }
    if (server_addr == NULL || server_addr[0] == '\0' || server_port == 0u) {
        if (why != NULL) *why = IV_TUNNEL_ENOSERVER;
        return IV_ENOENT;
    }
    if (resolver == NULL || resolver->resolve == NULL) {
        if (why != NULL) *why = IV_TUNNEL_EUNMAPPED;
        return IV_ENOENT;
    }

    if (app(out, cap, &pos, "serverAddr = \"%s\"\n", server_addr) != 0 ||
        app(out, cap, &pos, "serverPort = %u\n", (unsigned)server_port) != 0 ||
        app(out, cap, &pos, "auth.token = \"%s\"\n", token) != 0)
        goto overflow;

    for (i = 0; i < n; i++) {
        char     sname[IV_TUNNEL_SVC_MAX];
        char     name[IV_TUNNEL_NAME_MAX];
        char     secret[IV_TUNNEL_SECRET_MAX];
        uint32_t ip_be = 0;
        uint16_t port = 0;
        int      rc;

        if (iv_tunnel_svc_format(&svcs[i], sname, sizeof sname) != IV_OK) {
            if (why != NULL) *why = IV_TUNNEL_EBADARG;
            return IV_EINVAL;
        }
        rc = resolver->resolve(&svcs[i], &ip_be, &port, resolver->arg);
        if (rc != IV_OK) {
            if (why != NULL) *why = IV_TUNNEL_EUNMAPPED;
            return IV_ENOENT;
        }
        /* 硬性闸门：frp 只能代理本机可达地址，摄像机 IP 不许进配置
         * （要访问摄像机就走设备上的受控代理，见头文件说明）。 */
        if (!is_loopback(ip_be) || port == 0u) {
            if (why != NULL) *why = IV_TUNNEL_EHOST;
            return IV_EPROTO;
        }
        if (iv_tunnel_secret(sn, token, sname, secret, sizeof secret) != IV_OK) {
            if (why != NULL) *why = IV_TUNNEL_ENOIDENT;
            return IV_EFAIL;
        }
        snprintf(name, sizeof name, "%s-%s", sn, sname);

        if (app(out, cap, &pos, "\n[[proxies]]\n") != 0 ||
            app(out, cap, &pos, "name = \"%s\"\n", name) != 0 ||
            app(out, cap, &pos, "type = \"stcp\"\n") != 0 ||
            app(out, cap, &pos, "secretKey = \"%s\"\n", secret) != 0 ||
            app(out, cap, &pos, "localIP = \"%u.%u.%u.%u\"\n",
                (unsigned)((ip_be >> 24) & 0xffu), (unsigned)((ip_be >> 16) & 0xffu),
                (unsigned)((ip_be >> 8) & 0xffu), (unsigned)(ip_be & 0xffu)) != 0 ||
            app(out, cap, &pos, "localPort = %u\n", (unsigned)port) != 0)
            goto overflow;
    }

    out[cap - 1u] = '\0';
    *out_len = pos;
    if (why != NULL) *why = IV_TUNNEL_OK;
    return IV_OK;

overflow:
    out[cap - 1u] = '\0';
    if (why != NULL) *why = IV_TUNNEL_EBADARG;
    return IV_ERANGE;
}

/* ---------------------------------------------------------------------------
 * 文件落盘（原子写 + 0600）
 * ------------------------------------------------------------------------- */

/* 逐级建目录（已存在不算错） */
static int mkdir_p(const char *path)
{
    char   tmp[IV_TUNNEL_PATH_MAX];
    size_t i, n;

    if (path == NULL || path[0] == '\0') return -1;
    n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1u);
    for (i = 1u; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    return 0;
}

/* fsync 目录，保证 rename 本身落盘（S7/S8 同款口径） */
static void fsync_dir(const char *path)
{
    char tmp[IV_TUNNEL_PATH_MAX];
    char *slash;
    int   fd;

    if (strlen(path) >= sizeof tmp) return;
    memcpy(tmp, path, strlen(path) + 1u);
    slash = strrchr(tmp, '/');
    if (slash == NULL) return;
    if (slash == tmp) tmp[1] = '\0';
    else *slash = '\0';
    fd = open(tmp, O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        (void)close(fd);
    }
}

/*
 * 原子写：tmp → write → fsync → fchmod → rename → fsync(目录)。
 * 权限显式 fchmod（open 的 mode 会被 umask 吃掉）：secretKey 在文件里，
 * 必须 0600（BIRD 同款）。
 */
static int write_file_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
    char   tmp[IV_TUNNEL_PATH_MAX];
    int    fd;
    size_t off = 0;
    int    rc = -1;

    if (path == NULL || data == NULL) return -1;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    if (mkdir_p(path) != 0) return -1;

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            goto out;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) goto out;
    if (fchmod(fd, mode) != 0) goto out;
    (void)close(fd);
    fd = -1;
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return -1;
    }
    fsync_dir(path);
    rc = 0;

out:
    if (fd >= 0) (void)close(fd);
    return rc;
}

/* ---------------------------------------------------------------------------
 * 生命周期与配置
 * ------------------------------------------------------------------------- */

void iv_tunnel_init(iv_tunnel_t *t)
{
    if (t == NULL) return;
    memset(t, 0, sizeof *t);
    t->server_port = IV_TUNNEL_PORT_DEF;
    snprintf(t->conf_path, sizeof t->conf_path, "%s", IV_TUNNEL_CONF_DEFAULT);
    snprintf(t->flag_path, sizeof t->flag_path, "%s", IV_TUNNEL_FLAG_DEFAULT);
}

int iv_tunnel_set_identity(iv_tunnel_t *t, const char *sn, const char *token)
{
    if (t == NULL || sn == NULL || token == NULL) return IV_EINVAL;
    if (sn[0] == '\0' || token[0] == '\0') return IV_EINVAL; /* fail-closed */
    if (strlen(sn) >= sizeof t->sn || strlen(token) >= sizeof t->token) return IV_ERANGE;
    snprintf(t->sn, sizeof t->sn, "%s", sn);
    snprintf(t->token, sizeof t->token, "%s", token);
    return IV_OK;
}

int iv_tunnel_set_server(iv_tunnel_t *t, const char *addr, uint16_t port)
{
    if (t == NULL || addr == NULL) return IV_EINVAL;
    if (addr[0] == '\0') return IV_EINVAL;
    if (strlen(addr) >= sizeof t->server_addr) return IV_ERANGE;
    snprintf(t->server_addr, sizeof t->server_addr, "%s", addr);
    t->server_port = (port == 0u) ? IV_TUNNEL_PORT_DEF : port;
    return IV_OK;
}

int iv_tunnel_set_resolver(iv_tunnel_t *t, const iv_tunnel_resolver_t *r)
{
    if (t == NULL) return IV_EINVAL;
    if (r == NULL || r->resolve == NULL) {
        t->resolver = NULL; /* 解除 ⇒ 一律拒绝 */
        return IV_EINVAL;
    }
    t->resolver = r;
    return IV_OK;
}

int iv_tunnel_set_paths(iv_tunnel_t *t, const char *conf_path, const char *flag_path)
{
    if (t == NULL) return IV_EINVAL;
    if (conf_path != NULL) {
        if (conf_path[0] == '\0' || strlen(conf_path) >= sizeof t->conf_path)
            return IV_EINVAL;
        snprintf(t->conf_path, sizeof t->conf_path, "%s", conf_path);
    }
    if (flag_path != NULL) {
        if (flag_path[0] == '\0' || strlen(flag_path) >= sizeof t->flag_path)
            return IV_EINVAL;
        snprintf(t->flag_path, sizeof t->flag_path, "%s", flag_path);
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 闸门
 * ------------------------------------------------------------------------- */

static void session_close(iv_tunnel_t *t, uint64_t now_s, iv_tunnel_stop_reason_t r)
{
    t->last.open_s      = t->open_s;
    t->last.close_s     = now_s;
    t->last.ttl_s       = t->ttl_s;
    t->last.n_svc       = t->n_svc;
    memcpy(t->last.svc, t->svc, sizeof t->svc);
    t->last.stop_reason = (uint8_t)r;
    t->have_last        = 1;
    if (now_s > t->open_s) t->stat.uptime_s += (now_s - t->open_s);
    t->state = IV_TUNNEL_OFF;
    t->n_svc = 0u;
}

int iv_tunnel_enable(iv_tunnel_t *t, const iv_tunnel_svc_t *svcs, unsigned n,
                     uint32_t ttl_s, uint64_t now_s, iv_tunnel_reason_t *why)
{
    char                txt[IV_TUNNEL_TXT_MAX];
    size_t              len = 0;
    iv_tunnel_reason_t  r = IV_TUNNEL_OK;
    int                 rc;
    unsigned            i;

    if (t == NULL) return IV_EINVAL;
    if (svcs == NULL || n == 0u || n > IV_TUNNEL_PROXIES_MAX) {
        r = IV_TUNNEL_EBADARG; goto deny;
    }
    if (ttl_s == 0u) ttl_s = IV_TUNNEL_TTL_DEF;
    if (ttl_s > IV_TUNNEL_TTL_MAX) { r = IV_TUNNEL_EBADARG; goto deny; }
    if (t->state == IV_TUNNEL_ON) { r = IV_TUNNEL_ESTATE; goto deny; }
    if (t->sn[0] == '\0' || t->token[0] == '\0') { r = IV_TUNNEL_ENOIDENT; goto deny; }
    if (t->server_addr[0] == '\0' || t->server_port == 0u) { r = IV_TUNNEL_ENOSERVER; goto deny; }
    if (t->conf_path[0] == '\0' || t->flag_path[0] == '\0') { r = IV_TUNNEL_EIO; goto deny; }

    rc = iv_tunnel_render(t->sn, t->token, t->server_addr, t->server_port, t->resolver,
                          svcs, n, txt, sizeof txt, &len, &r);
    if (rc != IV_OK) goto deny;

    if (write_file_atomic(t->conf_path, txt, len, 0600) != 0) {
        r = IV_TUNNEL_EIO; goto deny;
    }
    /* 标记文件＝"业务上需要这条通道"的唯一真相（monitor 只在它存在时守护 frpc）。 */
    if (write_file_atomic(t->flag_path, "1\n", 2u, 0644) != 0) {
        r = IV_TUNNEL_EIO; goto deny;
    }

    t->state    = IV_TUNNEL_ON;
    t->open_s   = now_s;
    t->expire_s = now_s + (uint64_t)ttl_s;
    t->ttl_s    = ttl_s;
    t->n_svc    = n;
    for (i = 0; i < n && i < IV_TUNNEL_PROXIES_MAX; i++) t->svc[i] = svcs[i];
    t->stat.opens++;
    if (why != NULL) *why = IV_TUNNEL_OK;
    return IV_OK;

deny:
    t->stat.denies++;
    if ((unsigned)r < IV_TUNNEL_REASON_N) t->stat.deny_by[r]++;
    if (why != NULL) *why = r;
    return rc_of(r);
}

int iv_tunnel_disable(iv_tunnel_t *t, uint64_t now_s, iv_tunnel_stop_reason_t reason)
{
    int gone = 0;

    if (t == NULL) return IV_EINVAL;
    if (t->state != IV_TUNNEL_ON) return IV_ESTATE;

    if (unlink(t->flag_path) != 0 && errno != ENOENT) gone = -1;
    session_close(t, now_s, reason);
    /* 标记删不掉也必须把状态置 OFF：状态是"本进程的授权口径"，
     * 标记只是给守护脚本看的判据；留着错状态比留着一个文件更危险。 */
    return (gone == 0) ? IV_OK : IV_EIO;
}

int iv_tunnel_tick(iv_tunnel_t *t, uint64_t now_s)
{
    if (t == NULL) return IV_EINVAL;
    if (t->state != IV_TUNNEL_ON) return 0;
    if (now_s < t->expire_s) return 0;
    t->stat.expires++;
    (void)iv_tunnel_disable(t, now_s, IV_TUNNEL_STOP_EXPIRED);
    return 1;
}

int iv_tunnel_invalidate(iv_tunnel_t *t)
{
    if (t == NULL) return IV_EINVAL;
    if (t->state != IV_TUNNEL_ON) return IV_OK;
    return (iv_tunnel_disable(t, t->open_s, IV_TUNNEL_STOP_RECONF) == IV_OK) ? IV_OK : IV_EIO;
}

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */

iv_tunnel_state_t iv_tunnel_state(const iv_tunnel_t *t)
{
    if (t == NULL) return IV_TUNNEL_OFF;
    return (iv_tunnel_state_t)t->state;
}

uint32_t iv_tunnel_remain_s(const iv_tunnel_t *t, uint64_t now_s)
{
    if (t == NULL || t->state != IV_TUNNEL_ON) return 0u;
    if (now_s >= t->expire_s) return 0u;
    return (uint32_t)(t->expire_s - now_s);
}

uint32_t iv_tunnel_caps(const iv_tunnel_t *t)
{
    if (t == NULL) return 0u;
    if (t->sn[0] == '\0' || t->token[0] == '\0') return 0u;
    if (t->server_addr[0] == '\0' || t->server_port == 0u) return 0u;
    if (t->resolver == NULL) return 0u;
    if (t->conf_path[0] == '\0' || t->flag_path[0] == '\0') return 0u;
    return IV_TUNNEL_CAP_WEB | IV_TUNNEL_CAP_CAMERA | IV_TUNNEL_CAP_MEDIA |
           IV_TUNNEL_CAP_ENABLED;
}

void iv_tunnel_stat(const iv_tunnel_t *t, iv_tunnel_stat_t *out)
{
    if (t == NULL || out == NULL) return;
    *out = t->stat;
}

int iv_tunnel_last_session(const iv_tunnel_t *t, iv_tunnel_session_t *out)
{
    if (t == NULL || out == NULL) return IV_EINVAL;
    if (!t->have_last) return IV_ENOENT;
    *out = t->last;
    return IV_OK;
}
