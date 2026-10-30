/*
 * 受控远程访问通道（libivmodules，功能开发计划 M8-S8.1~S8.3）
 * 头文件已写明"是什么 / 为什么这么做"，本文件只写实现要点与坑。
 *
 * **2026-10-30 整块对齐 BIRD 运维箱（bird_20260623）**：一条 stcp 代理、转发本机
 * sshd（22）、密钥 md5(sn+token) 前 8 字节、配置 /etc/frp/frpc.toml、标记 /tmp/frp.enabled。
 * 原先那套"按服务名开 web/camera/media/ftp 多条代理 + resolver 映射"的做法已删除：
 * 现场要的是"GO 版怎么用，这套就怎么用"，而 GO 版把所有按需访问都交给访问端的
 * `ssh -L`（见 tools/visitor/connect.bat），设备侧不需要知道有哪些服务。
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

const char *iv_tunnel_stop_reason_name(int r)
{
    switch ((iv_tunnel_stop_reason_t)r) {
    case IV_TUNNEL_STOP_NONE:    return "none";
    case IV_TUNNEL_STOP_LOCAL:   return "local";
    case IV_TUNNEL_STOP_EXPIRED: return "expired";
    case IV_TUNNEL_STOP_RECONF:  return "reconf";
    default:                     return "unknown";
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
 * MD5 派生：hex( MD5(a + b)[0..8) ) → 16 个小写字符 + NUL。
 * 与 BIRD `internal/utils/md5.go: GenerateSTCPKey()` 逐字同源：
 *     hash := md5.Sum([]byte(sn + token)); hex.EncodeToString(hash[:8])
 * 用 OpenSSL EVP 而不是自写 MD5：纪律是"不自写密码学"（架构 §14）。
 */
static int md5_hex16(const char *a, const char *b, char out[17])
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
        if (EVP_DigestFinal_ex(ctx, dg, &n) == 1 && n >= 8u) ok = 1;
    }
    EVP_MD_CTX_free(ctx);
    if (!ok) return -1;
    to_hex(dg, 8u, out);
    return 0;
}

/* ---------------------------------------------------------------------------
 * secretKey 派生
 * ------------------------------------------------------------------------- */
int iv_tunnel_secret(const char *sn, const char *token, char *out, size_t cap)
{
    if (sn == NULL || token == NULL || out == NULL) return IV_EINVAL;
    if (cap < IV_TUNNEL_SECRET_MAX) return IV_ERANGE;
    if (md5_hex16(sn, token, out) != 0) return IV_EFAIL;
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

int iv_tunnel_render(const char *sn, const char *token, const char *server_addr,
                     uint16_t server_port, char *out, size_t cap, size_t *out_len,
                     iv_tunnel_reason_t *why)
{
    char    secret[IV_TUNNEL_SECRET_MAX];
    char    name[IV_TUNNEL_NAME_MAX];
    size_t  pos = 0;
    int     w;

    if (out == NULL || cap == 0u || out_len == NULL) {
        if (why != NULL) *why = IV_TUNNEL_EBADARG;
        return IV_EINVAL;
    }
    *out_len = 0;
    out[0] = '\0';

    if (sn == NULL || sn[0] == '\0' || token == NULL || token[0] == '\0') {
        if (why != NULL) *why = IV_TUNNEL_ENOIDENT;
        return IV_EPROTO;
    }
    if (server_addr == NULL || server_addr[0] == '\0' || server_port == 0u) {
        if (why != NULL) *why = IV_TUNNEL_ENOSERVER;
        return IV_ENOENT;
    }
    w = snprintf(name, sizeof name, "%s", sn);
    if (w < 0 || (size_t)w >= sizeof name) {
        if (why != NULL) *why = IV_TUNNEL_ENOIDENT;
        return IV_ERANGE;
    }
    if (iv_tunnel_secret(sn, token, secret, sizeof secret) != IV_OK) {
        if (why != NULL) *why = IV_TUNNEL_ENOIDENT;
        return IV_EFAIL;
    }

    /* 逐字对齐 BIRD `writeFrpConfig()` 的 fmt.Sprintf 产物（含空行位置与结尾换行）：
     * serverAddr / serverPort / auth.token ＋ 空行 ＋ 唯一的 [[proxies]] 段。 */
    if (app(out, cap, &pos, "serverAddr = \"%s\"\n", server_addr) != 0 ||
        app(out, cap, &pos, "serverPort = %u\n", (unsigned)server_port) != 0 ||
        app(out, cap, &pos, "auth.token = \"%s\"\n", token) != 0 ||
        app(out, cap, &pos, "\n[[proxies]]\n") != 0 ||
        app(out, cap, &pos, "name = \"%s\"\n", name) != 0 ||
        app(out, cap, &pos, "type = \"stcp\"\n") != 0 ||
        app(out, cap, &pos, "secretKey = \"%s\"\n", secret) != 0 ||
        app(out, cap, &pos, "localIP = \"127.0.0.1\"\n") != 0 ||
        app(out, cap, &pos, "localPort = %u\n", (unsigned)IV_TUNNEL_SSH_PORT) != 0) {
        out[cap - 1u] = '\0';
        if (why != NULL) *why = IV_TUNNEL_EBADARG;
        return IV_ERANGE;
    }

    *out_len = pos;
    if (why != NULL) *why = IV_TUNNEL_OK;
    return IV_OK;
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
    t->local_port  = (uint16_t)IV_TUNNEL_SSH_PORT;
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
    t->last.stop_reason = (uint8_t)r;
    t->have_last        = 1;
    if (now_s > t->open_s) t->stat.uptime_s += (now_s - t->open_s);
    t->state    = IV_TUNNEL_OFF;
    t->expire_s = 0u;
}

int iv_tunnel_enable(iv_tunnel_t *t, uint32_t ttl_s, uint64_t now_s,
                     iv_tunnel_reason_t *why)
{
    char                txt[IV_TUNNEL_TXT_MAX];
    size_t              len = 0;
    iv_tunnel_reason_t  r = IV_TUNNEL_OK;
    int                 rc;

    if (t == NULL) return IV_EINVAL;
    if (ttl_s > IV_TUNNEL_TTL_MAX) { r = IV_TUNNEL_EBADARG; goto deny; }
    if (t->state == IV_TUNNEL_ON) { r = IV_TUNNEL_ESTATE; goto deny; }
    if (t->sn[0] == '\0' || t->token[0] == '\0') { r = IV_TUNNEL_ENOIDENT; goto deny; }
    if (t->server_addr[0] == '\0' || t->server_port == 0u) { r = IV_TUNNEL_ENOSERVER; goto deny; }
    if (t->conf_path[0] == '\0' || t->flag_path[0] == '\0') { r = IV_TUNNEL_EIO; goto deny; }

    rc = iv_tunnel_render(t->sn, t->token, t->server_addr, t->server_port,
                          txt, sizeof txt, &len, &r);
    if (rc != IV_OK) goto deny;

    if (write_file_atomic(t->conf_path, txt, len, 0600) != 0) {
        r = IV_TUNNEL_EIO; goto deny;
    }
    /* 标记文件＝"业务上需要这条通道"的唯一真相（monitor 只在它存在时守护 frpc）。
     * BIRD 的做法是 `touch`（空文件、永不过期）；本工程支持平台显式给 TTL，此时
     * **内容写成到期时刻**，让 `frp.sh`/`monitor.sh` 那一侧能独立判过期 —— tick()
     * 跑在 ivsboxd 里，主控一崩就没人判了。ttl_s==0 时写空文件，与 BIRD 逐字等价。 */
    if (ttl_s == 0u) {
        if (write_file_atomic(t->flag_path, "", 0u, 0644) != 0) {
            r = IV_TUNNEL_EIO; goto deny;
        }
        t->expire_s = 0u;
    } else {
        char     ftxt[32];
        uint64_t expire = now_s + (uint64_t)ttl_s;

        (void)snprintf(ftxt, sizeof ftxt, "%llu\n", (unsigned long long)expire);
        if (write_file_atomic(t->flag_path, ftxt, strlen(ftxt), 0644) != 0) {
            r = IV_TUNNEL_EIO; goto deny;
        }
        t->expire_s = expire;
    }

    t->state   = IV_TUNNEL_ON;
    t->open_s  = now_s;
    t->ttl_s   = ttl_s;
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
    int was_on;

    if (t == NULL || t->flag_path[0] == '\0') return IV_EINVAL;
    was_on = (t->state == IV_TUNNEL_ON);

    /* 标记**无条件**删 —— BIRD 的 `disable_frp()` 就是 `rm -f` 不带条件，天然幂等。
     * 为什么非幂等不可：内存态与标记可能不同步（进程刚重启、`iv_tunnel_restore()` 还
     * 没跑到，或被 SIGKILL 后重来）。此时平台发 disable 若因为 "state 已经是 OFF" 就连
     * 文件都不删，回的是"成功"、通道却还开着 —— 那是比报错严重得多的失败。
     * 返回值保持不变（未开 ⇒ IV_ESTATE），让调用方仍能区分"本来就没开"。 */
    if (unlink(t->flag_path) != 0 && errno != ENOENT) gone = -1;
    if (was_on) session_close(t, now_s, reason);
    /* 标记删不掉也必须把状态置 OFF：状态是"本进程的授权口径"，
     * 标记只是给守护脚本看的判据；留着错状态比留着一个文件更危险。 */
    if (!was_on) return IV_ESTATE;
    return (gone == 0) ? IV_OK : IV_EIO;
}

int iv_tunnel_restore(iv_tunnel_t *t, uint64_t now_s)
{
    char     buf[32];
    ssize_t  n;
    int      fd;
    uint64_t expire = 0u;
    size_t   i;

    if (t == NULL || t->flag_path[0] == '\0') return IV_EINVAL;
    if (t->state == IV_TUNNEL_ON) return IV_ESTATE;

    fd = open(t->flag_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return (errno == ENOENT) ? IV_ENOENT : IV_EIO;
    n = read(fd, buf, sizeof buf - 1u);
    (void)close(fd);
    if (n < 0) return IV_EIO;
    buf[(size_t)n] = '\0';

    /* 只认十进制数字（到期时刻）。空内容 ⇒ 保持 0 ＝ 不过期（BIRD 的 `touch`）。
     * 末尾允许换行/空白：本模块写标记时就是 `"%llu\n"`，`frp.sh` 那一侧用
     * `tr -cd '0-9'` 取数，两边必须对"带换行的数字"口径一致。 */
    for (i = 0u; i < (size_t)n; i++) {
        if (buf[i] < '0' || buf[i] > '9') break;
        if (expire > (UINT64_MAX - 9u) / 10u) { expire = 0u; i = 0u; break; }
        expire = expire * 10u + (uint64_t)(buf[i] - '0');
    }
    if (i == 0u) {
        expire = 0u;
    } else {
        size_t j = i;

        while (j < (size_t)n && (buf[j] == '\n' || buf[j] == '\r' || buf[j] == ' ' ||
                                 buf[j] == '\t')) {
            j++;
        }
        if (j != (size_t)n) expire = 0u; /* 数字后面还跟着别的东西 ⇒ 不是到期时刻 */
    }

    if (expire != 0u && now_s >= expire) {
        (void)unlink(t->flag_path);
        return IV_ESTATE;
    }

    t->state      = IV_TUNNEL_ON;
    t->open_s     = now_s;
    t->expire_s   = expire;
    t->ttl_s      = (expire != 0u) ? (uint32_t)(expire - now_s) : 0u;
    t->local_port = (uint16_t)IV_TUNNEL_SSH_PORT;
    return IV_OK;
}

int iv_tunnel_tick(iv_tunnel_t *t, uint64_t now_s)
{
    if (t == NULL) return IV_EINVAL;
    if (t->state != IV_TUNNEL_ON) return 0;
    if (t->expire_s == 0u) return 0; /* 不过期（BIRD 语义） */
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
    if (t->expire_s == 0u) return 0u; /* 不过期：没有"剩余"这个概念 */
    if (now_s >= t->expire_s) return 0u;
    return (uint32_t)(t->expire_s - now_s);
}

uint32_t iv_tunnel_caps(const iv_tunnel_t *t)
{
    if (t == NULL) return 0u;
    if (t->sn[0] == '\0' || t->token[0] == '\0') return 0u;
    if (t->server_addr[0] == '\0' || t->server_port == 0u) return 0u;
    if (t->conf_path[0] == '\0' || t->flag_path[0] == '\0') return 0u;
    return IV_TUNNEL_CAP_SSH | IV_TUNNEL_CAP_ENABLED;
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
