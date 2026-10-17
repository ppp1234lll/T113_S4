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

#include "ivsbox/iv_ret.h"
#include "iv_web.h"

#define IV_API_SESS_TTL_MS (8u * 3600u * 1000u)

/* ctl 请求拼接缓冲：body 上限 + 包一层的余量。静态区（单线程事件循环）。 */
static char s_ctl_req[IV_WEB_BODY_MAX + 256u];

/* --------------------------------------------------------------------------
 * 小工具
 * -------------------------------------------------------------------------- */

/* 从 `a=1&b=2` 里取 `key` 的值（不做 URL 解码：本工程用到的参数都是
 * 短标识符，没有需要转义的字符；真需要时在这里补 percent-decode）。 */
static int query_get(const char *q, const char *key, char *out, size_t cap)
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

/* 统一把通道响应搬进 out，并设好 Content-Type。 */
static int to_rsp(char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    *out_len = strlen(out);
    rsp->status = 200;
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

static int api_config_get(iv_web_ctx_t *ctx, const iv_web_req_t *req,
                          char *out, size_t cap, size_t *out_len, iv_web_resp_t *rsp)
{
    char cls[32];
    char p[64];

    if (query_get(req->query, "all", cls, sizeof(cls))) {
        return api_simple_call(ctx, "config.all", out, cap, out_len, rsp);
    }
    if (query_get(req->query, "defaults", cls, sizeof(cls))) {
        return api_simple_call(ctx, "config.defaults", out, cap, out_len, rsp);
    }
    if (!query_get(req->query, "cls", cls, sizeof(cls)) || cls[0] == '\0') {
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

    if (!query_get(req->query, "which", which, sizeof(which)))
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
    if (strcmp(req->path, "/api/camera/search") == 0)
        return api_simple_call(ctx, "camera.search", out, cap, out_len, rsp);
    if (strcmp(req->path, "/api/system/time") == 0)
        return api_p_call(ctx, "system.time",
                          (req->body != NULL) ? req->body : "{}",
                          out, cap, out_len, rsp);
    if (strcmp(req->path, "/api/system/reboot") == 0)
        return api_p_call(ctx, "system.reboot",
                          (req->body != NULL) ? req->body : "{}",
                          out, cap, out_len, rsp);

    *out_len = api_err(out, cap, IV_ENOENT, "no such api");
    rsp->status = 404;
    rsp->len = *out_len;
    return IV_OK;
}
