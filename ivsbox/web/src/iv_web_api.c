/*
 * ivsbox-web REST API（M5-S5.2 / S5.4 前端侧）
 *
 * ============================ 这一层只做三件事 ============================
 *   1. **鉴权**：除 `POST /api/login` 外，所有 `/api` 下的接口都要有效会话 Cookie。
 *   2. **翻译**：把浏览器能懂的 REST 路径，翻成本地通道上的 `iv_ctl` 方法调用
 *      （例如 `GET /api/config?cls=net` → `{"m":"config.class","p":{"cls":"net"}}`）。
 *   3. **回话**：把 ivsboxd 给的 `{"ok":...}` 响应**原样**交给浏览器 —— 不重新
 *      包装 JSON，免得同一个错误码在两处各定一套含义。
 *
 * 写操作（配置提交 / 重启 / 搜索摄像机）**不在这里做二次校验**：类型、范围、
 * 版本号都在 `iv_ctl` 侧一次判死（那才是唯一权威）。这里只做"参数在不在"的
 * 粗筛，挡掉明显不成形的请求，少走一趟通道。
 *
 * ============================ 会话 ============================
 * token 是 128 位随机数的十六进制串，**只存进程内存**；进程一重启全部失效。
 * 到期时间固定 8 小时，不滑动续期（嵌入式下"一直有效"比"够久"更危险）。
 * 表满时**挤掉最早到期的那个**，而不是拒绝登录 —— 拒绝会让"忘了登出"的用户
 * 再也进不来，而挤掉最多让另一个人重新登录一次。
 *
 * ============================ M10：账号 ＋ 出厂默认口令闸门 ============================
 * 登录体由 `{"pass":...}` 变为 `{"user":...,"pass":...}`；比对的是
 * `iv_web_pass_hash()` 的摘要（`ctx->pass_hash`），**不是明文**（内存里也没有明文，
 * 见 iv_web_pass.c 文件头）。账号错与口令错回**同一句** 401 —— 不告诉试探者
 * "账号是对的、只差口令"。
 * `ctx->must_change`（仍在出厂默认口令）为真时，除 `POST /api/passwd` 与
 * `POST /api/logout` 一律 403 `must change password`。**闸门放服务端**：只做前端提示
 * 等于没有防线 —— 知道出厂口令的人直接 curl 就能绕过去继续用设备。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <json-c/json.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "iv_web.h"

/* 本文件自己打日志时用的模块名（web/src/main.c 的 WEB_MOD 是那个文件的局部宏） */
#define API_MOD "web-api"

#define IV_API_SESS_TTL_MS (8u * 3600u * 1000u)

/* ctl 请求拼接缓冲：body 上限 + 包一层的余量。静态区（单线程事件循环）。 */
static char s_ctl_req[IV_WEB_BODY_MAX + 256u];

/* --------------------------------------------------------------------------
 * 小工具
 * -------------------------------------------------------------------------- */

/* 从 `a=1&b=2` 里取 `key` 的值（不做 URL 解码：本工程用到的参数都是
 * 短标识符，没有需要转义的字符；真需要时在这里补 percent-decode）。 */
int iv_web_query_get(const char *q, const char *key, char *out, size_t cap)
{
    size_t      klen = strlen(key);
    const char *p = q;

    if (q == NULL || out == NULL || cap == 0u)
        return 0;
    out[0] = '\0';
    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        size_t      seg = (amp != NULL) ? (size_t)(amp - p) : strlen(p);

        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1u;
            size_t      vlen = seg - klen - 1u;

            if (vlen >= cap)
                vlen = cap - 1u;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (amp == NULL)
            break;
        p = amp + 1;
    }
    return 0;
}

/* 生成错误 JSON（与 iv_ctl 的成功/失败外形一致，前端一套解析逻辑通吃）。 */
static size_t api_err(char *out, size_t cap, int code, const char *msg)
{
    int n = snprintf(out, cap, "{\"ok\":false,\"e\":%d,\"msg\":\"%s\"}", code, msg);

    if (n < 0)
        return 0;
    return ((size_t)n >= cap) ? cap - 1u : (size_t)n;
}

/* 把 Web 的 POST body 解析成 JSON 对象；失败返回 NULL。 */
static json_object *body_json(const iv_web_req_t *req)
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

/* 把 JSON 体里的 `"e":<int>` 映射成 HTTP 状态码（R-06）。
 *
 * 病根：原先**一律 200**，靠体内 `ok:false` 表达失败。浏览器里没区别（前端
 * 只看 `j.ok`），但任何不带业务逻辑的观察者对错误完全无感 —— curl、反代访问
 * 日志、探针都显示 200，现场排障时"接口通了没有"和"业务成功没有"分不开。
 *
 * 只改**状态码**，响应体一字不动（前端 `api()` 对非 2xx 仍按 JSON 解析，
 * 唯一例外是既有的 403＝必须改密那条路）。
 * 找不到 `ok:false` ⇒ 200；错误码认不出 ⇒ 500（宁可让人看见"有错"）。 */
static int status_of(const char *body)
{
    const char *e = strstr(body, "\"ok\":false");
    const char *p;
    int         code = 0;
    int         sign = 1;

    if (e == NULL)
        return 200;
    p = strstr(e, "\"e\":");
    if (p == NULL)
        return 400;
    p += 4;
    while (*p == ' ')
        p++;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    while (*p >= '0' && *p <= '9')
        code = code * 10 + (*p++ - '0');
    code *= sign;

    switch (code) {
    case IV_EINVAL:
    case IV_EPROTO:
    case IV_ERANGE:
        return 400;
    case IV_EAUTH:
        return 403;
    case IV_ENOENT:
        return 404;
    case IV_EBUSY:
    case IV_EEXIST:
    case IV_ESTATE:
    case IV_ECANCELED:
        return 409;
    case IV_ENOSPC:
        return 507;
    case IV_ENOTSUP:
        return 501;
    default:
        return 500;
    }
}

/* 统一把通道响应搬进 out，并设好 Content-Type。 */
static int to_rsp(char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    *out_len = strlen(out);
    rsp->status = status_of(out);
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len = *out_len;
    (void)cap;
    return IV_OK;
}

/* 调一次通道。resp_json/req_json 均为 NUL 结尾串。 */
static int chan_json(iv_web_ctx_t *ctx, const char *req_json, size_t req_len,
                     char *out, size_t cap)
{
    size_t out_len = 0;
    int    rc = iv_web_chan_call(ctx, req_json, req_len, out, cap, &out_len);

    if (rc != IV_OK) {
        (void)api_err(out, cap, rc,
                      rc == IV_ENOENT ? "ivsboxd not running" : "channel error");
        return IV_OK; /* 通道错误也回 200 + ok:false，前端统一处理 */
    }
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 会话
 * -------------------------------------------------------------------------- */
int iv_web_sess_check(iv_web_ctx_t *ctx, const char *token)
{
    uint64_t now = iv_web_now_ms();
    int      i;

    if (token == NULL || token[0] == '\0')
        return 0;
    for (i = 0; i < (int)IV_WEB_SESS_N; i++) {
        if (ctx->sess[i].token[0] == '\0')
            continue;
        if (ctx->sess[i].expires_ms <= now) {
            ctx->sess[i].token[0] = '\0'; /* 过期即清 */
            continue;
        }
        if (strcmp(ctx->sess[i].token, token) == 0)
            return 1;
    }
    return 0;
}

void iv_web_sess_new(iv_web_ctx_t *ctx, char *out_token, size_t cap)
{
    int      slot = 0;
    int      i;
    uint64_t best = ~(uint64_t)0;

    (void)snprintf(out_token, cap, "%08x%08x%08x%08x",
                   iv_web_rand(ctx), iv_web_rand(ctx),
                   iv_web_rand(ctx), iv_web_rand(ctx));

    for (i = 0; i < (int)IV_WEB_SESS_N; i++) {
        if (ctx->sess[i].token[0] == '\0') {
            slot = i;
            break;
        }
        if (ctx->sess[i].expires_ms < best) {
            best = ctx->sess[i].expires_ms;
            slot = i;
        }
    }
    (void)snprintf(ctx->sess[slot].token, sizeof(ctx->sess[slot].token), "%s", out_token);
    ctx->sess[slot].expires_ms = iv_web_now_ms() + IV_API_SESS_TTL_MS;
}

static void sess_drop(iv_web_ctx_t *ctx, const char *token)
{
    int i;

    if (token == NULL)
        return;
    for (i = 0; i < (int)IV_WEB_SESS_N; i++) {
        if (ctx->sess[i].token[0] != '\0' && strcmp(ctx->sess[i].token, token) == 0)
            ctx->sess[i].token[0] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * 各接口
 * -------------------------------------------------------------------------- */

/* 定长比较：不因"第几个字节不同"提前收工 —— 口令/摘要比对不给时间侧信道。
 * 长度不等直接算失败，但**照样走完全程**（提前 return 会把长度也漏出去）。 */
static int same_secret(const char *a, const char *b)
{
    size_t        i;
    size_t        la = (a != NULL) ? strlen(a) : 0u;
    size_t        lb = (b != NULL) ? strlen(b) : 0u;
    unsigned char diff = (la == lb) ? 0u : 1u;

    for (i = 0; i < la; i++)
        diff |= (unsigned char)((unsigned char)a[i] ^ (unsigned char)b[(i < lb) ? i : 0u]);
    return diff == 0u;
}

static int api_login(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                     char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    json_object *o = body_json(req);
    const char  *pass = NULL;
    const char  *user = NULL;
    char         hex[IV_WEB_PASS_HEX_MAX];
    char         tok[IV_WEB_TOKEN_MAX];
    int          ok;

    if (o != NULL) {
        json_object *v = NULL;

        if (json_object_object_get_ex(o, "user", &v) && v != NULL)
            user = json_object_get_string(v);
        v = NULL;
        if (json_object_object_get_ex(o, "pass", &v) && v != NULL)
            pass = json_object_get_string(v);
    }
    ok = (user != NULL) && (pass != NULL) && same_secret(user, ctx->user);
    if (ok) {
        iv_web_pass_hash(ctx->user, pass, hex);
        ok = same_secret(hex, ctx->pass_hash);
    }
    if (o != NULL)
        json_object_put(o);
    if (!ok) {
        /* 账号错 / 口令错 / 少字段 —— 同一句，且不区分（见文件头）。 */
        *out_len = api_err(out, cap, IV_EAUTH, "bad password");
        rsp->status = 401;
        (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
        rsp->len = *out_len;
        return IV_OK;
    }

    iv_web_sess_new(ctx, tok, sizeof(tok));
    rsp->set_cookie = 1;
    (void)snprintf(rsp->cookie, sizeof(rsp->cookie), "ivs_sid=%s; Path=/; HttpOnly", tok);
    /* must_change 一并告诉前端：它据此直接切到"修改密码"视图，不用再探一次。 */
    *out_len = (size_t)snprintf(out, cap,
                                "{\"ok\":true,\"d\":{\"token\":\"%s\",\"must_change\":%s}}",
                                tok, (ctx->must_change != 0) ? "true" : "false");
    (void)to_rsp(out, cap, out_len, rsp);
    return IV_OK;
}

static int api_logout(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                      char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    sess_drop(ctx, req->token);
    *out_len = (size_t)snprintf(out, cap, "{\"ok\":true}");
    (void)to_rsp(out, cap, out_len, rsp);
    return IV_OK;
}

/* 拼一个不带参数的 ctl 请求并转发。 */
static int api_simple_call(iv_web_ctx_t *ctx, const char *method,
                           char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    int n = snprintf(s_ctl_req, sizeof(s_ctl_req), "{\"m\":\"%s\"}", method);

    if (n < 0 || (size_t)n >= sizeof(s_ctl_req)) {
        *out_len = api_err(out, cap, IV_ERANGE, "request too long");
        return to_rsp(out, cap, out_len, rsp);
    }
    (void)chan_json(ctx, s_ctl_req, (size_t)n, out, cap);
    return to_rsp(out, cap, out_len, rsp);
}

/* 带 p 对象参数：p_json 必须是合法的 JSON 对象文本。 */
static int api_p_call(iv_web_ctx_t *ctx, const char *method, const char *p_json,
                      char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    int n = snprintf(s_ctl_req, sizeof(s_ctl_req), "{\"m\":\"%s\",\"p\":%s}",
                     method, p_json != NULL ? p_json : "{}");

    if (n < 0 || (size_t)n >= sizeof(s_ctl_req)) {
        *out_len = api_err(out, cap, IV_ERANGE, "request too long");
        return to_rsp(out, cap, out_len, rsp);
    }
    (void)chan_json(ctx, s_ctl_req, (size_t)n, out, cap);
    return to_rsp(out, cap, out_len, rsp);
}

/* 同进程其它层用的 ctl 调用口（见 iv_web.h）。`ota.c` 用它发 `ota.request`：
 * 自己再写一份 `{"m":..,"p":..}` 封装，等于让通道请求的外形有两处定义。 */
int iv_web_api_call(iv_web_ctx_t *ctx, const char *method, const char *p_json,
                    char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    return api_p_call(ctx, method, p_json, out, cap, out_len, rsp);
}

/* --------------------------------------------------------------------------
 * 抓拍（2026-10-28，Web「抓拍」按钮）
 *
 * `POST /api/snapshot` → ctl `snapshot.take`，自动命名落 `<snap_dir>/snap_<秒>.jpg`。
 *
 * **入队 ≠ 已落盘**：ctl 侧是异步的（V4L2 出帧可达数百毫秒），所以响应里的
 * `queued:true` 只代表"已受理"。前端必须拿 `url` 去取图，取不到就是失败
 * （`<img onerror>` 重试几次后报错），不能把 queued 当成"拍好了"弹成功 ——
 * 这正是 2026-10-28 板端 `peer 200 but … missing` 那条日志在界面上的对应物。
 *
 * `url` 由本层**现算**：绝对路径换算成 `/media/...`。只有落在媒体根之下才算得
 * 出来（否则 `url` 缺省，前端退化成只显示文件名）。不让前端自己拼，是因为
 * `media_root` 只有本进程知道（`--media-root`）。
 * -------------------------------------------------------------------------- */
static int api_media_url(const iv_web_ctx_t *ctx, const char *abs, char *out, size_t cap)
{
    const char *root = (ctx->media_root[0] != '\0') ? ctx->media_root
                                                    : IV_WEB_MEDIA_ROOT_DEFAULT;
    size_t      rl   = strlen(root);
    int         n;

    if (abs == NULL || abs[0] != '/')
        return 0;
    if (rl == 0u || strncmp(abs, root, rl) != 0 || abs[rl] != '/')
        return 0;
    n = snprintf(out, cap, "/media%s", abs + rl);
    return (n > 0 && (size_t)n < cap) ? 1 : 0;
}

/* 路径只允许这些字符（本机命名全是 ASCII 安全字符）。不合格就整条拒 ——
 * 不许把任意字节拼进 JSON 字符串（拼串比转义容易出错，索性不给机会）。 */
static int api_path_json_safe(const char *p)
{
    size_t i;

    for (i = 0; p[i] != '\0'; i++) {
        unsigned char c = (unsigned char)p[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/')
            continue;
        return 0;
    }
    return (i > 0u) ? 1 : 0;
}

static int api_snapshot(iv_web_ctx_t *ctx, char *out, size_t cap, size_t *out_len,
                        iv_web_resp_t *rsp)
{
    static const char *k_req = "{\"m\":\"snapshot.take\",\"p\":{}}";
    json_object *resp;
    json_object *d;
    json_object *v;
    const char  *path = NULL;
    const char  *name;
    char         url[IV_WEB_PATH_MAX + 32u];
    int          has_url;
    int          n;

    (void)chan_json(ctx, k_req, strlen(k_req), out, cap);

    resp = json_tokener_parse(out);
    if (resp == NULL || !json_object_is_type(resp, json_type_object) ||
        !json_object_object_get_ex(resp, "ok", &v) || v == NULL ||
        !json_object_get_boolean(v)) {
        if (resp != NULL)
            json_object_put(resp);
        return to_rsp(out, cap, out_len, rsp); /* 失败原样透传（-95 未装配等） */
    }
    d = json_object_object_get(resp, "d");
    if (d != NULL && json_object_object_get_ex(d, "path", &v) && v != NULL)
        path = json_object_get_string(v);
    if (path == NULL || !api_path_json_safe(path)) {
        json_object_put(resp);
        *out_len = api_err(out, cap, IV_EPROTO, "bad path from daemon");
        return to_rsp(out, cap, out_len, rsp);
    }

    name = strrchr(path, '/');
    name = (name != NULL) ? (name + 1) : path;
    has_url = api_media_url(ctx, path, url, sizeof(url));
    n = snprintf(out, cap,
                 "{\"ok\":true,\"d\":{\"queued\":true,\"path\":\"%s\",\"name\":\"%s\"%s%s%s}}",
                 path, name, (has_url != 0) ? ",\"url\":\"" : "",
                 (has_url != 0) ? url : "", (has_url != 0) ? "\"" : "");
    json_object_put(resp);
    if (n < 0 || (size_t)n >= cap) {
        *out_len = api_err(out, cap, IV_ERANGE, "path too long");
        return to_rsp(out, cap, out_len, rsp);
    }
    *out_len     = (size_t)n;
    rsp->status  = 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len     = *out_len;
    return IV_OK;
}

static int api_config_get(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                          char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    char cls[32];
    char p[64];

    if (iv_web_query_get(req->query, "all", cls, sizeof(cls))) {
        return api_simple_call(ctx, "config.all", out, cap, out_len, rsp);
    }
    if (iv_web_query_get(req->query, "defaults", cls, sizeof(cls))) {
        return api_simple_call(ctx, "config.defaults", out, cap, out_len, rsp);
    }
    if (!iv_web_query_get(req->query, "cls", cls, sizeof(cls)) || cls[0] == '\0') {
        *out_len = api_err(out, cap, IV_EINVAL, "missing cls");
        return to_rsp(out, cap, out_len, rsp);
    }
    if (snprintf(p, sizeof(p), "{\"cls\":\"%s\"}", cls) >= (int)sizeof(p)) {
        *out_len = api_err(out, cap, IV_EINVAL, "cls too long");
        return to_rsp(out, cap, out_len, rsp);
    }
    return api_p_call(ctx, "config.class", p, out, cap, out_len, rsp);
}

static int api_config_apply(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                            char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    if (req->body == NULL || req->body_len == 0u) {
        *out_len = api_err(out, cap, IV_EINVAL, "missing body");
        return to_rsp(out, cap, out_len, rsp);
    }
    /* 通道容量闸门（64 KiB）——Web 侧先挡一道，免得拼完才发现超了。 */
    if (req->body_len > IV_WEB_BODY_MAX) {
        *out_len = api_err(out, cap, IV_ERANGE, "body too large");
        return to_rsp(out, cap, out_len, rsp);
    }
    return api_p_call(ctx, "config.apply", req->body, out, cap, out_len, rsp);
}

static int api_snmp_get(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                        char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    char which[16];
    char p[48];

    if (!iv_web_query_get(req->query, "which", which, sizeof(which)))
        (void)snprintf(which, sizeof(which), "cam");
    if (snprintf(p, sizeof(p), "{\"which\":\"%s\"}", which) >= (int)sizeof(p)) {
        *out_len = api_err(out, cap, IV_EINVAL, "bad which");
        return to_rsp(out, cap, out_len, rsp);
    }
    return api_p_call(ctx, "snmp.result", p, out, cap, out_len, rsp);
}

/* --------------------------------------------------------------------------
 * 改密（M10）
 * -------------------------------------------------------------------------- */
static int api_passwd(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                      char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    json_object *o = body_json(req);
    const char  *nw = NULL;
    const char  *old = NULL;
    char         hex[IV_WEB_PASS_HEX_MAX];

    if (o != NULL) {
        json_object *v = NULL;

        if (json_object_object_get_ex(o, "new", &v) && v != NULL)
            nw = json_object_get_string(v);
        v = NULL;
        if (json_object_object_get_ex(o, "old", &v) && v != NULL)
            old = json_object_get_string(v);
    }

    /* 1. 复杂度：服务端判（前端那份只是即时提示，**不是**闸门）。 */
    if (iv_web_pass_rule(nw) != IV_OK) {
        if (o != NULL)
            json_object_put(o);
        *out_len = api_err(out, cap, IV_EINVAL, IV_WEB_PASS_HINT_JSON);
        rsp->status = 400;
        rsp->len = *out_len;
        return IV_OK;
    }

    /* 2. 非强制改密必须校验旧口令：会话可能是别人替你留下的，而改密是提权动作。
     *    强制改密（must_change=1）路径**不校验**：用户刚用出厂口令登进来，
     *    再让他抄一遍出厂口令没有意义（参考实现也是只给一个"新密码"字段）。 */
    if (ctx->must_change == 0) {
        if (old == NULL) {
            if (o != NULL)
                json_object_put(o);
            *out_len = api_err(out, cap, IV_EINVAL, "old password required");
            rsp->status = 400;
            rsp->len = *out_len;
            return IV_OK;
        }
        iv_web_pass_hash(ctx->user, old, hex);
        if (!same_secret(hex, ctx->pass_hash)) {
            if (o != NULL)
                json_object_put(o);
            *out_len = api_err(out, cap, IV_EAUTH, "bad password");
            rsp->status = 401;
            rsp->len = *out_len;
            return IV_OK;
        }
    }

    /* 3. **先落盘、再提交内存**：落盘失败就整体失败。反过来（先改内存再落盘）会
     *    产出"改成功了、重启又变回出厂口令"——现场说不清也查不出。
     *    未配置落盘（验证场景）按成功处理，只在日志里留一句。 */
    iv_web_pass_hash(ctx->user, nw, hex);
    {
        int rc = iv_web_pass_save(ctx, ctx->user, hex);

        if (rc != IV_OK && rc != IV_ENOTSUP) {
            if (o != NULL)
                json_object_put(o);
            *out_len = api_err(out, cap, rc, "cannot save password");
            rsp->status = 500;
            rsp->len = *out_len;
            return IV_OK;
        }
    }
    if (o != NULL)
        json_object_put(o);
    (void)snprintf(ctx->pass_hash, sizeof(ctx->pass_hash), "%s", hex);
    ctx->must_change = 0;
    *out_len = (size_t)snprintf(out, cap, "{\"ok\":true}");
    rsp->status = 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len = *out_len;
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 恢复出厂 / 擦除存储（2026-10-22 单片机 web 对齐第 ⑥ 项）
 *
 * 分工（写在两处容易看不全，所以在这儿讲清楚）：
 *   - **配置回默认 + 擦数据** 走本地通道 `sys.factory_reset`（`ivsboxd` 是唯一
 *     能写配置的进程）。本层只负责转手，不自己动配置。
 *   - **口令** 由本层自己重置 —— `<secure>/web.passwd` 的单写者就是 ivsbox-web，
 *     `ivsboxd` 连这个路径都不该认识。恢复出厂后口令回到出厂默认并**强制改密**，
 *     同时**踢掉所有会话**：把"回到出厂口令"的窗口压到最短。
 *
 * 请求体：`{"confirm":true,"erase":["media","log","db"],"reset_password":true}`
 *   - `confirm` 必填（与 ctl 的二次确认门一致，前后端各判一次 —— 前端那道只是
 *     "别让手滑点到"，后端这道才是闸门）；
 *   - `erase` 可省略 ＝ 只恢复配置、**不删任何数据**（用户 2026-10-22 口径）；
 *   - `reset_password` 缺省 true；显式给 false 时只恢复配置与数据、不动口令
 *     （留一个口子给"只想清配置"的现场，且这条路径不会踢会话）。
 * -------------------------------------------------------------------------- */
static void sess_drop_all(iv_web_ctx_t *ctx)
{
    /* 全会话作废：恢复出厂之后不该还有任何一扇门开着 */
    memset(ctx->sess, 0, sizeof(ctx->sess));
}

static int api_factory_reset(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                             char *out, size_t cap, size_t *out_len,
                             iv_web_resp_t *rsp)
{
    json_object *o       = body_json(req);
    json_object *v       = NULL;
    json_object *d       = NULL;
    const char  *erase_s = NULL;
    char         pbuf[320];
    char         hex[IV_WEB_PASS_HEX_MAX];
    int          confirm       = 0;
    int          reset_passwd  = 1;
    int          pwd_done      = 0;
    int          n;
    int          rc;

    if (o != NULL) {
        if (json_object_object_get_ex(o, "confirm", &v) && v != NULL)
            confirm = json_object_get_boolean(v);
        v = NULL;
        if (json_object_object_get_ex(o, "reset_password", &v) && v != NULL)
            reset_passwd = json_object_get_boolean(v);
        v = NULL;
        /* erase 允许字符串或数组：原样序列化再拼进 ctl 请求，免得这里再写一份
         * "哪些名字合法"的判断（那份判断属于 ctl/iv_factory）。 */
        if (json_object_object_get_ex(o, "erase", &v) && v != NULL &&
            (json_object_is_type(v, json_type_array) || json_object_is_type(v, json_type_string)))
            erase_s = json_object_to_json_string(v);
    }
    if (!confirm) {
        if (o != NULL)
            json_object_put(o);
        *out_len = api_err(out, cap, IV_EPROTO, "confirm required");
        rsp->status = 400;
        rsp->len = *out_len;
        return IV_OK;
    }

    if (erase_s != NULL) {
        n = snprintf(pbuf, sizeof(pbuf), "{\"confirm\":true,\"erase\":%s}", erase_s);
    } else {
        n = snprintf(pbuf, sizeof(pbuf), "{\"confirm\":true}");
    }
    if (n < 0 || (size_t)n >= sizeof(pbuf)) {
        if (o != NULL)
            json_object_put(o);
        *out_len = api_err(out, cap, IV_ERANGE, "erase too long");
        rsp->status = 400;
        rsp->len = *out_len;
        return IV_OK;
    }
    if (o != NULL)
        json_object_put(o);

    rc = iv_web_api_call(ctx, "sys.factory_reset", pbuf, out, cap, out_len, rsp);
    if (rc != IV_OK)
        return to_rsp(out, cap, out_len, rsp);
    /* ctl 侧成功才是真的成功 */
    {
        json_object *resp = json_tokener_parse(out);

        if (resp == NULL || !json_object_is_type(resp, json_type_object) ||
            !json_object_object_get_ex(resp, "ok", &v) || v == NULL ||
            !json_object_get_boolean(v)) {
            if (resp != NULL)
                json_object_put(resp);
            return to_rsp(out, cap, out_len, rsp);
        }
        d = json_object_object_get(resp, "d");
        /* d 属于 resp，后面要拼回 out，所以先留下它的文本 */
        if (d != NULL && reset_passwd) {
            /* 1) 落盘：写回**出厂默认**口令的哈希（只存哈希，见 iv_web_pass.c） */
            iv_web_pass_hash(ctx->user, IV_WEB_PASS_DEF, hex);
            rc = iv_web_pass_save(ctx, ctx->user, hex);
            if (rc == IV_OK || rc == IV_ENOTSUP) {
                (void)snprintf(ctx->pass_hash, sizeof(ctx->pass_hash), "%s", hex);
                ctx->must_change = 1; /* 出厂口令期：强制改密（与 M10 同一道闸） */
                pwd_done = 1;
            } else {
                IV_LOG_E(API_MOD, "factory reset: cannot save factory password (rc=%d)", rc);
            }
        }
        /* 2) 拼响应：ctl 的字段照抄，另加本层做的两件事 */
        {
            char         tail[192];
            int          tl;

            tl = snprintf(tail, sizeof(tail),
                          "\"password_reset\":%s,\"must_change\":%s}",
                          (pwd_done != 0) ? "true" : "false",
                          (ctx->must_change != 0) ? "true" : "false");
            if (tl < 0 || (size_t)tl >= sizeof(tail)) {
                json_object_put(resp);
                *out_len = api_err(out, cap, IV_ERANGE, "response too long");
                rsp->status = 500;
                rsp->len = *out_len;
                return IV_OK;
            }
            (void)snprintf(out, cap, "{\"ok\":true,\"d\":%s,%s",
                           (d != NULL) ? json_object_to_json_string(d) : "{}", tail);
            *out_len = strlen(out);
        }
        json_object_put(resp);
    }

    if (pwd_done != 0)
        sess_drop_all(ctx); /* 口令回出厂 ⇒ 现存会话一律作废 */
    rsp->status = 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->len = *out_len;
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 入口
 * -------------------------------------------------------------------------- */
int iv_web_api_handle(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                      char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    rsp->status = 200;
    (void)snprintf(rsp->ctype, sizeof(rsp->ctype), "application/json");
    rsp->set_cookie = 0;

    if (strncmp(req->path, "/api/", 5u) != 0)
        return IV_ENOTSUP; /* 不归我管：交给静态文件 */

    /* 登录接口本身当然不能要求会话 */
    if (strcmp(req->path, "/api/login") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            *out_len = api_err(out, cap, IV_EINVAL, "POST required");
            rsp->status = 405;
            rsp->len = *out_len;
            return IV_OK;
        }
        return api_login(ctx, req, out, cap, out_len, rsp);
    }

    /* 其余一律要会话 */
    if (!iv_web_sess_check(ctx, req->token)) {
        *out_len = api_err(out, cap, IV_EAUTH, "unauthorized");
        rsp->status = 401;
        rsp->len = *out_len;
        return IV_OK;
    }

    /* 出厂默认口令期：只放行"改密"与"登出"（M10）。前端拿这个 403 切到改密视图。 */
    if (ctx->must_change != 0 &&
        strcmp(req->path, "/api/passwd") != 0 &&
        strcmp(req->path, "/api/logout") != 0) {
        *out_len = api_err(out, cap, IV_ESTATE, "must change password");
        rsp->status = 403;
        rsp->len = *out_len;
        return IV_OK;
    }

    if (strcmp(req->path, "/api/passwd") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            *out_len = api_err(out, cap, IV_EINVAL, "POST required");
            rsp->status = 405;
            rsp->len = *out_len;
            return IV_OK;
        }
        return api_passwd(ctx, req, out, cap, out_len, rsp);
    }
    if (strcmp(req->path, "/api/logout") == 0)
        return api_logout(ctx, req, out, cap, out_len, rsp);
    if (strcmp(req->path, "/api/factory_reset") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            *out_len = api_err(out, cap, IV_EINVAL, "POST required");
            rsp->status = 405;
            rsp->len = *out_len;
            return IV_OK;
        }
        return api_factory_reset(ctx, req, out, cap, out_len, rsp);
    }
    if (strcmp(req->path, "/api/overview") == 0)
        return api_simple_call(ctx, "sys.overview", out, cap, out_len, rsp);
    if (strcmp(req->path, "/api/devices") == 0)
        return api_simple_call(ctx, "device.list", out, cap, out_len, rsp);
    if (strcmp(req->path, "/api/config") == 0)
        return api_config_get(ctx, req, out, cap, out_len, rsp);
    if (strcmp(req->path, "/api/config/apply") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            *out_len = api_err(out, cap, IV_EINVAL, "POST required");
            rsp->status = 405;
            rsp->len = *out_len;
            return IV_OK;
        }
        return api_config_apply(ctx, req, out, cap, out_len, rsp);
    }
    if (strcmp(req->path, "/api/snmp") == 0)
        return api_snmp_get(ctx, req, out, cap, out_len, rsp);
    if (strcmp(req->path, "/api/snmp/run") == 0)
        return api_p_call(ctx, "snmp.run",
                          (req->body != NULL) ? req->body : "{}",
                          out, cap, out_len, rsp);
    /* S2.5 GPS：只读。定位/统计都由 ivsboxd 侧解析，本层仅透传。 */
    if (strcmp(req->path, "/api/gps") == 0)
        return api_simple_call(ctx, "gps.status", out, cap, out_len, rsp);
    /* 4G 模组状态：只读。AT 查询在 ivsboxd 侧跑（与拨号脚本共用 AT 口，
     * 只发查询命令），本层只转发，不碰串口。 */
    if (strcmp(req->path, "/api/cell") == 0)
        return api_simple_call(ctx, "cellular.status", out, cap, out_len, rsp);
    /* S2.6 采集板（控制板）：只读，镜像与通信统计全在 ivsboxd 侧，本层仅透传。 */
    if (strcmp(req->path, "/api/board") == 0)
        return api_simple_call(ctx, "board.status", out, cap, out_len, rsp);
    /* S2.6 继电器人工控制：**写**路径（下发 0xD1）。只收 POST —— 控制动作不能
     * 靠一个 GET 被浏览器预取/缓存命中而误触发。 */
    if (strcmp(req->path, "/api/board/relay") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            *out_len = api_err(out, cap, IV_EINVAL, "POST required");
            rsp->status = 405;
            rsp->len = *out_len;
            return IV_OK;
        }
        return api_p_call(ctx, "relay.set",
                          (req->body != NULL) ? req->body : "{}",
                          out, cap, out_len, rsp);
    }
    /* 设备身份配置：**写**路径（落工厂身份分区）。同样只收 POST ——
     * 身份写坏会让设备"变成另一台"，绝不能靠 GET 预取误触发。 */
    if (strcmp(req->path, "/api/device/identity") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            *out_len = api_err(out, cap, IV_EINVAL, "POST required");
            rsp->status = 405;
            rsp->len = *out_len;
            return IV_OK;
        }
        return api_p_call(ctx, "device.identity.set",
                          (req->body != NULL) ? req->body : "{}",
                          out, cap, out_len, rsp);
    }
    if (strcmp(req->path, "/api/camera/search") == 0)
        return api_simple_call(ctx, "camera.search", out, cap, out_len, rsp);
    /* 立即重测一路（摄像机页「测」按钮）。参数 `{ch:1..6}` 原样交 ctl，
     * 越界/未配 IP 的错误话术由 `m_camera_test()` 给（一处口径，不在这里重写）。 */
    if (strcmp(req->path, "/api/camera/test") == 0)
        return api_p_call(ctx, "camera.test",
                          (req->body != NULL) ? req->body : "{}",
                          out, cap, out_len, rsp);
    /* 媒体录像页（2026-10-28）。GET = 一次读全 8 路状态（`record.page`），
     * POST = 手动启停（`record.manual`）。**启停只收 POST**：按钮动作不能让浏览器
     * 的预取/刷新重放一次 —— 与 relay.set / device.identity.set 同一条口径。
     * 注意路径在 `/api/` 前缀下，与录像下载的 `/media/...`（iv_web_media.c）不是
     * 一回事，两处互不影响。 */
    if (strcmp(req->path, "/api/media/record") == 0) {
        if (strcmp(req->method, "POST") == 0)
            return api_p_call(ctx, "record.manual",
                              (req->body != NULL) ? req->body : "{}",
                              out, cap, out_len, rsp);
        return api_simple_call(ctx, "record.page", out, cap, out_len, rsp);
    }
    /* 抓拍（USB 摄像头，2026-10-28）：**只收 POST** —— 抓拍会往 SD 卡写文件，
     * 不能被浏览器的预取/刷新重放一次（与 relay.set 同一条口径）。
     * 取图不在这里：图片走 `/media/snap/<文件名>`（iv_web_media.c 的 Range 下载），
     * 列表走 `GET /media/snap/`（同一文件的目录分支）。 */
    if (strcmp(req->path, "/api/snapshot") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            *out_len = api_err(out, cap, IV_EINVAL, "POST required");
            rsp->status = 405;
            rsp->len = *out_len;
            return IV_OK;
        }
        return api_snapshot(ctx, out, cap, out_len, rsp);
    }
    if (strcmp(req->path, "/api/system/time") == 0)
        return api_p_call(ctx, "system.time",
                          (req->body != NULL) ? req->body : "{}",
                          out, cap, out_len, rsp);
    if (strcmp(req->path, "/api/system/reboot") == 0)
        return api_p_call(ctx, "system.reboot",
                          (req->body != NULL) ? req->body : "{}",
                          out, cap, out_len, rsp);
    /* 升级状态：**只读**（`ota.status` 不写盘）。真正的安装由 monitor.sh 拉起
     * ivsbox-updater 执行，Web 侧只把状态显示出来——"谁改版本"永远只有一个写者。 */
    if (strcmp(req->path, "/api/ota") == 0)
        return api_simple_call(ctx, "ota.status", out, cap, out_len, rsp);

    /* 日志浏览/下载（`iv_web_logs.c`）。**必须在兜底 404 之前**转出去：
     * `/api/logs` 属于 `/api/` 前缀，本函数会先命中它；不在这里转交的话，
     * 永远走不到 logs.c，表现是"路由写好了却恒 404"（2026-10-22 踩过一次）。 */
    if (strncmp(req->path, "/api/logs", 9u) == 0 &&
        (req->path[9] == '\0' || req->path[9] == '/'))
        return iv_web_logs_handle(ctx, req, out, cap, out_len, rsp);

    /* 升级包上传/校验/请求安装（`iv_web_ota.c`）。同样必须在兜底 404 之前转出去。
     * 只接 `/api/ota/<子路径>`：`/api/ota` 本身是状态查询，已在上面处理完了。 */
    if (strncmp(req->path, "/api/ota/", 9u) == 0)
        return iv_web_ota_handle(ctx, req, out, cap, out_len, rsp);

    *out_len = api_err(out, cap, IV_ENOENT, "no such api");
    rsp->status = 404;
    rsp->len = *out_len;
    return IV_OK;
}
