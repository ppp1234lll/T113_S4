/*
 * ivsbox-web 升级包分片上传 / 校验 / 请求安装（2026-10-22）
 *
 * ============================ 为什么要有这一层 ============================
 * 参考实现的「升级有线/无线」是**设备主动去服务器下载**；本机没有下载器，包只能由
 * 浏览器（或平台下发、或随 U 盘落地）送到设备上。此前运维页只能**显示**升级状态，
 * 因为 `IV_WEB_BODY_MAX` 是 32 KiB 而版本包有几 MB —— 一次 POST 根本传不进来。
 *
 * 本层把"传包"拆成可恢复的分片，**不碰安装**：安装仍是 `monitor.sh` 拉起
 * `ivsbox-updater` 的活（单写者，见 `iv_upd.h`）。本层只做两件事：
 *   1. 把字节安全地落到 `<ota_root>/<name>.part`，校验长度与 SHA-256，再原子改名；
 *   2. 把"请装这个包"交给本地通道的 `ota.request`（它只写一个请求文件）。
 *
 * ============================ 为什么分片而不是给 http 层加"体落文件"状态机 ============================
 * 后者要动请求解析那个"一连接一请求、一次读满 rbuf"的核心循环，出错面覆盖**所有**
 * 接口；而分片只是多一个循环的 REST 调用，且天然带来续传能力（现场 4G 上传断掉
 * 不用从头再来）。代价是前端要多写十几行，值与不值很清楚。
 *
 * ============================ 路径围栏 ============================
 * 名字**只收正向形状**：`[A-Za-z0-9][A-Za-z0-9._-]*`，长度 ≤ `IV_WEB_OTA_NAME_MAX`。
 * 不是"过滤掉危险字符"（`..` 全由合法字符组成），而是"不匹配就拒"，因此拼出来的
 * 路径必然在 `ota_root` 之下。再加 `open(O_NOFOLLOW)`：即便有人事先在同一目录放了
 * 一个同名符号链接，也不会顺着它写到别处去。
 * 包名不做 percent-decode —— 合法字符集里没有需要转义的字符，前端也不需要编码；
 * 真有人发 `%2F`，它会在白名单那一步被拒（比"先解码再判"少一条出错路径）。
 *
 * ============================ 与 ivsboxd 的口径必须一致 ============================
 * `iv_upd_request_write()` 只接受**位于 `<data_root>/ota/` 之下**的已存在普通文件，
 * 所以本层的 `ota_root` 默认值就是 `<data_root>/ota`。两边配不上时 ctl 会返回
 * `EINVAL`，本层把它如实报给前端并**在日志里点明原因** —— 绝不静默装错包。
 *
 * ============================ 一连接一请求的静态缓冲 ============================
 * `s_pkg_path` 是静态的：本函数返回后 http 层才用它（安装请求走的是同一拍内的 ctl
 * 调用，用完即弃）。安全前提同 logs.c：事件循环单线程，一次只处理一条请求。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <json-c/json.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_sha256.h"
#include "iv_web.h"

#define OTA_MOD     "web.ota"
#define OTA_PREFIX  "/api/ota/"
#define OTA_PFX_LEN 9u

/* 拼出来的包绝对路径（`<root>/<name>` 或 `<root>/<name>.part`）。静态区见文件头。 */
static char s_pkg_path[IV_WEB_OTA_ROOT_MAX + IV_WEB_OTA_NAME_MAX + 8u];

/* --------------------------------------------------------------------------
 * 小工具
 * -------------------------------------------------------------------------- */
static int ota_err(char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp,
                   int status, int code, const char *msg)
{
    int n = snprintf(out, cap, "{\"ok\":false,\"e\":%d,\"msg\":\"%s\"}", code, msg);

    rsp->status = status;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len = (n < 0) ? 0u : (((size_t)n >= cap) ? (cap - 1u) : (size_t)n);
    *out_len = rsp->len;
    return IV_OK;
}

static int ota_ok(char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp,
                  const char *json_d)
{
    int n = snprintf(out, cap, "{\"ok\":true,\"d\":%s}", json_d);

    rsp->status = 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len = (n < 0) ? 0u : (((size_t)n >= cap) ? (cap - 1u) : (size_t)n);
    *out_len = rsp->len;
    return IV_OK;
}

/* 调用方已经用 snprintf 写进 `out` 的情况（409 带尾巴这类），只补齐 rsp 记账。
 * **别自己另写一个临时缓冲**——2026-10-22 就是这么踩的：JSON 拼进了局部 `d`，
 * 却没拷回 `out`，于是 409 的响应体是一串 NUL，前端只会报"解析失败"。 */
static int ota_emit(char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp, int status)
{
    size_t n = strlen(out);

    if (n >= cap)
        n = cap - 1u;
    rsp->status = status;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len = n;
    *out_len = n;
    return IV_OK;
}

/* 包名正向白名单。**不接受空串、不接受首字符是 `.`、不接受任何非白名单字符**。 */
static int name_ok(const char *n)
{
    size_t i;

    if (n == NULL)
        return 0;
    if (n[0] == '\0' || strlen(n) > (size_t)IV_WEB_OTA_NAME_MAX)
        return 0;
    for (i = 0u; n[i] != '\0'; i++) {
        char c = n[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            if (i == 0u && c == '.')
                return 0; /* 首字符 `."" ⇒ 会拼出隐藏名或 `..` 类串 */
            continue;
        }
        return 0;
    }
    return 1;
}

/* 命令行没给就回落默认值。 */
static const char *ota_root_of(const iv_web_ctx_t *ctx)
{
    if (ctx->ota_root[0] != '\0')
        return ctx->ota_root;
    return IV_WEB_OTA_ROOT_DEFAULT;
}

/* 拼 `<root>/<name>[.part]`；放不下返回 -1（**不截断**——截断会拼出另一个路径）。 */
static int pkg_path(const iv_web_ctx_t *ctx, const char *name, int part)
{
    const char *root = ota_root_of(ctx);
    int         n = snprintf(s_pkg_path, sizeof(s_pkg_path), "%s/%s%s", root, name,
                             part ? ".part" : "");

    if (n < 0 || (size_t)n >= sizeof(s_pkg_path))
        return -1;
    return 0;
}

/* 严格解析十进制无符号数：整串都必须是数字（`strtoull` 默认会放过 "12abc"）。 */
static int parse_u64(const char *s, uint64_t *out)
{
    unsigned long long v;
    char              *end = NULL;

    if (s == NULL || s[0] == '\0' || s[0] == '-' || s[0] == '+')
        return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

/* 把 hex 摘要统一成小写后比较（前端可能给大写）。 */
static int hex_same(const char *a, const char *b)
{
    size_t i;

    if (a == NULL || b == NULL || strlen(a) != strlen(b) || strlen(a) != 64u)
        return 0;
    for (i = 0u; i < 64u; i++) {
        char x = a[i];
        char y = b[i];

        if (x >= 'A' && x <= 'F')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'F')
            y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return 1;
}

/* 请求体当 JSON 对象取；用完必须 `json_object_put()`。 */
static json_object *ota_body_json(const iv_web_req_t *req)
{
    json_tokener *t;
    json_object  *o;

    if (req->body == NULL || req->body_len == 0u)
        return NULL;
    t = json_tokener_new();
    if (t == NULL)
        return NULL;
    o = json_tokener_parse_ex(t, req->body, (int)req->body_len);
    if (o == NULL || json_tokener_get_error(t) != json_tokener_success) {
        if (o != NULL)
            json_object_put(o);
        json_tokener_free(t);
        return NULL;
    }
    json_tokener_free(t);
    if (!json_object_is_type(o, json_type_object)) {
        json_object_put(o);
        return NULL;
    }
    return o;
}

static const char *jstr(json_object *o, const char *k)
{
    json_object *v = NULL;

    if (o == NULL || !json_object_object_get_ex(o, k, &v) || v == NULL)
        return NULL;
    return json_object_get_string(v);
}

/* --------------------------------------------------------------------------
 * POST /api/ota/chunk?name=..&off=N
 * --------------------------------------------------------------------------
 * `off` 必须**显式给**（不默认 0）：默认 0 意味着"手滑少写一个参数就把已有分片清掉"，
 * 而这种错误在浏览器里只表现为"传完的包变小了"，极难查。
 * `off > 0` 且现存长度不等于 `off` ⇒ 409 并把**实际**偏移回给前端，让它决定续传还是
 * 重传 —— 用它当续传握手，比前端自己猜可靠。
 */
static int ota_chunk(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                     char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    char            name[IV_WEB_OTA_NAME_MAX + 1u];
    char            offs[32];
    uint64_t        off = 0u;
    uint64_t        total;
    struct stat     st;
    struct statvfs  vfs;
    size_t          done = 0u;
    char            d[256];
    int             fd;
    uint64_t        freeb;

    if (strcmp(req->method, "POST") != 0 && strcmp(req->method, "GET") != 0)
        return ota_err(out, cap, out_len, rsp, 405, IV_EINVAL, "GET/POST only");
    if (!iv_web_query_get(req->query, "name", name, sizeof(name)))
        return ota_err(out, cap, out_len, rsp, 400, IV_EINVAL, "missing name");
    if (!name_ok(name)) {
        IV_LOG_W(OTA_MOD, "reject package name: %.80s", name);
        return ota_err(out, cap, out_len, rsp, 403, IV_EAUTH, "bad package name");
    }
    if (pkg_path(ctx, name, 1) != 0)
        return ota_err(out, cap, out_len, rsp, 400, IV_ERANGE, "ota root too long");

    /* GET = 查续传点（页面刷新后前端靠它接上，而不是从头再传一遍） */
    if (strcmp(req->method, "GET") == 0) {
        if (stat(s_pkg_path, &st) == 0 && S_ISREG(st.st_mode)) {
            (void)snprintf(d, sizeof(d), "{\"name\":\"%s\",\"off\":%llu}", name,
                           (unsigned long long)st.st_size);
            return ota_ok(out, cap, out_len, rsp, d);
        }
        (void)snprintf(d, sizeof(d), "{\"name\":\"%s\",\"off\":0}", name);
        return ota_ok(out, cap, out_len, rsp, d);
    }

    if (req->body_len == 0u)
        return ota_err(out, cap, out_len, rsp, 400, IV_EINVAL, "empty slice");
    if (req->body_len > (size_t)IV_WEB_OTA_SLICE) {
        IV_LOG_W(OTA_MOD, "slice too large: %u > %u", (unsigned)req->body_len,
                 (unsigned)IV_WEB_OTA_SLICE);
        return ota_err(out, cap, out_len, rsp, 413, IV_ERANGE, "slice too large");
    }
    if (!iv_web_query_get(req->query, "off", offs, sizeof(offs)) || parse_u64(offs, &off) != 0)
        return ota_err(out, cap, out_len, rsp, 400, IV_EINVAL, "missing/invalid off");
    if (off > (uint64_t)IV_WEB_OTA_PKG_MAX ||
        off + (uint64_t)req->body_len > (uint64_t)IV_WEB_OTA_PKG_MAX)
        return ota_err(out, cap, out_len, rsp, 413, IV_ERANGE, "package too large");

    fd = open(s_pkg_path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
                                ((off == 0u) ? O_TRUNC : 0),
              0600);
    if (fd < 0) {
        IV_LOG_W(OTA_MOD, "open %s failed: %s", s_pkg_path, strerror(errno));
        return ota_err(out, cap, out_len, rsp, (errno == ELOOP) ? 403 : 500,
                       (errno == ELOOP) ? IV_EAUTH : IV_EIO,
                       (errno == ELOOP) ? "refusing to follow symlink" : "cannot open staging file");
    }

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        return ota_err(out, cap, out_len, rsp, 500, IV_EIO, "bad staging file");
    }
    /* 续传握手：偏移对不上就报实际值，**绝不在对不上的位置上乱写**（那会把包写成
     * 一份"看起来传完了、SHA 却不对"的东西，比直接失败难查得多）。 */
    if ((uint64_t)st.st_size != off) {
        total = (uint64_t)st.st_size;
        (void)close(fd);
        (void)snprintf(out, cap,
                       "{\"ok\":false,\"e\":%d,\"msg\":\"offset mismatch\",\"d\":{\"name\":\"%s\",\"off\":%llu}}",
                       IV_ESTATE, name, (unsigned long long)total);
        return ota_emit(out, cap, out_len, rsp, 409);
    }

    /* **必须显式定位**：`open()` 之后文件偏移是 0，直接 write 会把前面已经传好的字节
     * 覆盖掉，而 `fstat` 看到的长度变化会骗过"传完了"的判断（2026-10-22 由
     * `tests/web/ota_e2e.py` 抓到：第二片起每片都写回开头，文件永远停在第一片大
     * 小，而响应还报着"已收到 off+N"）。 */
    if (off > 0u && lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) {
        (void)close(fd);
        return ota_err(out, cap, out_len, rsp, 500, IV_EIO, "cannot seek staging file");
    }

    if (off == 0u) {
        /* 起传前看空闲空间：UDISK 被填满会连带拖垮录像与数据库，这里先挡一道。 */
        if (statvfs(ota_root_of(ctx), &vfs) != 0) {
            (void)close(fd);
            return ota_err(out, cap, out_len, rsp, 500, IV_EIO, "cannot statvfs ota root");
        }
        freeb = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
        if (freeb < (uint64_t)IV_WEB_OTA_FREE_MIN) {
            (void)close(fd);
            IV_LOG_W(OTA_MOD, "not enough space: free=%llu", (unsigned long long)freeb);
            return ota_err(out, cap, out_len, rsp, 507, IV_ENOSPC, "not enough space");
        }
    }

    while (done < req->body_len) {
        ssize_t w = write(fd, req->body + done, req->body_len - done);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            IV_LOG_W(OTA_MOD, "write %s failed: %s", s_pkg_path, strerror(errno));
            return ota_err(out, cap, out_len, rsp, 500, IV_EIO, "write failed");
        }
        done += (size_t)w;
    }
    if (close(fd) != 0)
        return ota_err(out, cap, out_len, rsp, 500, IV_EIO, "close failed");

    (void)snprintf(d, sizeof(d),
                   "{\"name\":\"%s\",\"off\":%llu,\"next\":%llu}", name,
                   (unsigned long long)(off + (uint64_t)done),
                   (unsigned long long)(off + (uint64_t)done));
    return ota_ok(out, cap, out_len, rsp, d);
}

/* --------------------------------------------------------------------------
 * POST /api/ota/finish  {"name","size","sha256"}
 * --------------------------------------------------------------------------
 * 长度与摘要**都**要核：只核长度挡不住坏包，只核摘要在传输被截断时白算一遍。
 * 校验通过后 `fsync` 再 `rename`（同目录内原子），这样改名那一刻包已经是完整内容 ——
 * 掉电最坏是留下一份 `.part`，而不是一份"名字像成品、内容没写完"的包。
 */
static int ota_finish(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                      char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    json_object  *o = ota_body_json(req);
    const char   *name;
    const char   *sha;
    const char   *sz;
    uint64_t      want = 0u;
    uint64_t      got;
    uint8_t       dig[IV_SHA256_DIGEST_LEN];
    char          hex[IV_SHA256_DIGEST_LEN * 2u + 1u];
    char          part[sizeof(s_pkg_path)];
    /* 要装下 name(64) + path(328) + size(20) + sha256(64) ＋ 外壳，640 留足；
     * 取 320 时被 -Werror=format-truncation 挡下——那是**真截断**（响应被砍成半截
     * JSON，前端只会报"解析失败"，查起来很远）。 */
    char          d[640];
    struct stat   st;
    size_t        i;
    int           fd;
    int           rc = IV_OK;

    if (o == NULL)
        return ota_err(out, cap, out_len, rsp, 400, IV_EINVAL, "body must be a JSON object");
    name = jstr(o, "name");
    sha = jstr(o, "sha256");
    sz = jstr(o, "size");
    if (name == NULL || sz == NULL) {
        json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 400, IV_EINVAL, "need name/size");
    }
    if (!name_ok(name)) {
        json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 403, IV_EAUTH, "bad package name");
    }
    if (parse_u64(sz, &want) != 0 || want == 0u || want > (uint64_t)IV_WEB_OTA_PKG_MAX) {
        json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 400, IV_EINVAL, "bad size");
    }
    if (pkg_path(ctx, name, 1) != 0) {
        json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 400, IV_ERANGE, "ota root too long");
    }
    (void)snprintf(part, sizeof(part), "%s", s_pkg_path);

    if (stat(part, &st) != 0 || !S_ISREG(st.st_mode)) {
        json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 404, IV_ENOENT, "no uploaded package");
    }
    got = (uint64_t)st.st_size;
    if (got != want) {
        json_object_put(o);
        (void)snprintf(out, cap,
                       "{\"ok\":false,\"e\":%d,\"msg\":\"size mismatch\",\"d\":{\"got\":%llu,\"want\":%llu}}",
                       IV_ESTATE, (unsigned long long)got, (unsigned long long)want);
        return ota_emit(out, cap, out_len, rsp, 409);
    }

    if (iv_sha256_file(part, dig) != IV_OK) {
        json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 500, IV_EIO, "cannot hash upload");
    }
    for (i = 0u; i < IV_SHA256_DIGEST_LEN; i++)
        (void)snprintf(hex + i * 2u, 3u, "%02x", dig[i]);
    /* `sha256` **可选**：浏览器在纯 http（非 localhost）下拿不到 `crypto.subtle`，
     * 前端算不出摘要。缺省时本层只把算出来的摘要回给前端显示 —— 完整性另有兜底：
     * 真正的闸门是 `iv_upd_install()` 的 Ed25519 验签（坏包根本装不上）。
     * 前端能算时（https/自测）仍然照传，多一道关口。 */
    if (sha != NULL && sha[0] != '\0' && !hex_same(hex, sha)) {
        IV_LOG_W(OTA_MOD, "sha256 mismatch for %s: got %s", name, hex);
        json_object_put(o);
        (void)snprintf(out, cap,
                       "{\"ok\":false,\"e\":%d,\"msg\":\"sha256 mismatch\",\"d\":{\"sha256\":\"%s\"}}",
                       IV_ECORRUPT, hex);
        return ota_emit(out, cap, out_len, rsp, 409);
    }

    /* 先在分片文件上 fsync（只读 fd 也能 fsync），再改名。 */
    fd = open(part, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fsync(fd) != 0) {
        rc = IV_EIO;
    }
    if (fd >= 0)
        (void)close(fd);

    if (rc == IV_OK && pkg_path(ctx, name, 0) != 0)
        rc = IV_ERANGE;
    if (rc == IV_OK && rename(part, s_pkg_path) != 0) {
        IV_LOG_W(OTA_MOD, "rename %s -> %s failed: %s", part, s_pkg_path, strerror(errno));
        rc = IV_EIO;
    }
    json_object_put(o);
    if (rc != IV_OK)
        return ota_err(out, cap, out_len, rsp, 500, rc, "cannot commit package");

    IV_LOG_I(OTA_MOD, "package ready: %s (%llu B, sha256=%s)", s_pkg_path,
             (unsigned long long)want, hex);
    (void)snprintf(d, sizeof(d),
                   "{\"name\":\"%s\",\"path\":\"%s\",\"size\":%llu,\"sha256\":\"%s\"}",
                   name, s_pkg_path, (unsigned long long)want, hex);
    return ota_ok(out, cap, out_len, rsp, d);
}

/* --------------------------------------------------------------------------
 * POST /api/ota/install  {"name","restart"}
 * --------------------------------------------------------------------------
 * 只写"安装请求"，**不安装**：`ota.request` 的语义就是落一个请求文件，monitor.sh 见到
 * 它才拉起 `ivsbox-updater`。这里不直接调 updater 的理由见 `iv_upd.h`：改盘动作要有
 * 自己的进程生命周期（不继承看门狗 fd、不在 reactor 里阻塞）。
 */
static int ota_install(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                       char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    json_object *o = ota_body_json(req);
    json_object *v = NULL;
    const char  *name;
    char         p[IV_WEB_OTA_ROOT_MAX + IV_WEB_OTA_NAME_MAX + 48u];
    char         pj[IV_WEB_OTA_ROOT_MAX + IV_WEB_OTA_NAME_MAX + 64u];
    struct stat  st;
    int          restart = 0;

    if (o == NULL)
        return ota_err(out, cap, out_len, rsp, 400, IV_EINVAL, "body must be a JSON object");
    name = jstr(o, "name");
    if (name == NULL || !name_ok(name)) {
        json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 400, IV_EINVAL, "bad package name");
    }
    if (json_object_object_get_ex(o, "restart", &v) && v != NULL)
        restart = json_object_get_boolean(v) ? 1 : 0;

    if (pkg_path(ctx, name, 0) != 0) {
        json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 400, IV_ERANGE, "ota root too long");
    }
    if (stat(s_pkg_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 404, IV_ENOENT, "package not uploaded yet");
    }
    (void)snprintf(p, sizeof(p), "%s", s_pkg_path);
    json_object_put(o);

    if (snprintf(pj, sizeof(pj), "{\"path\":\"%s\",\"restart\":%d}", p, restart) >=
        (int)sizeof(pj))
        return ota_err(out, cap, out_len, rsp, 400, IV_ERANGE, "path too long");

    /* 把响应原样透传：`ota.request` 已经是 `{"ok":..}` 外形，重包一层只会让同一个
     * 错误码在两处各定一套含义。失败时额外落一句日志点明最常见的原因。 */
    IV_LOG_I(OTA_MOD, "install request: %s (restart=%d)", p, restart);
    return iv_web_api_call(ctx, "ota.request", pj, out, cap, out_len, rsp);
}

/* POST /api/ota/abort {"name"} —— 丢弃未完成的分片（页面上"取消"用它）。 */
static int ota_abort(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                     char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    json_object *o = ota_body_json(req);
    const char  *name = jstr(o, "name");
    int          had = 0;

    if (name == NULL || !name_ok(name)) {
        if (o != NULL)
            json_object_put(o);
        return ota_err(out, cap, out_len, rsp, 400, IV_EINVAL, "bad package name");
    }
    if (pkg_path(ctx, name, 1) == 0 && unlink(s_pkg_path) == 0)
        had = 1;
    json_object_put(o);
    if (had)
        IV_LOG_I(OTA_MOD, "upload aborted: %s", name);
    return ota_ok(out, cap, out_len, rsp,
                  had ? "{\"aborted\":true}" : "{\"aborted\":false}");
}

/* --------------------------------------------------------------------------
 * 分派
 * -------------------------------------------------------------------------- */
int iv_web_ota_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                      char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    if (req == NULL || req->path == NULL)
        return IV_EINVAL;
    if (strncmp(req->path, OTA_PREFIX, OTA_PFX_LEN) != 0)
        return IV_ENOTSUP;

    if (strcmp(req->path + OTA_PFX_LEN, "chunk") == 0)
        return ota_chunk(ctx, req, out, cap, out_len, rsp);
    if (strcmp(req->path + OTA_PFX_LEN, "finish") == 0)
        return ota_finish(ctx, req, out, cap, out_len, rsp);
    if (strcmp(req->path + OTA_PFX_LEN, "install") == 0)
        return ota_install(ctx, req, out, cap, out_len, rsp);
    if (strcmp(req->path + OTA_PFX_LEN, "abort") == 0)
        return ota_abort(ctx, req, out, cap, out_len, rsp);
    return IV_ENOTSUP; /* 未知子路径：交给 api 的兜底 404 */
}
