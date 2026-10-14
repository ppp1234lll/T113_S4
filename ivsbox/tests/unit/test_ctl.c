/*
 * iv_ctl 单测（libivmodules，功能开发计划 M5-S5.1 / S5.2）
 *
 * 覆盖三件必须钉死的事：
 *   ① **协议外形**：`system.snapshot` 保持 S10 老格式（集成用例逐字断言过），
 *      其余方法统一 `{"ok":true,"d":...}` / `{"ok":false,"e":<码>,"msg":...}`；
 *   ② **未知方法静默**：返回 IV_ENOTSUP 且**一个字节都不写调用方的缓冲**——
 *      这条被 tests/integration/test_ctl_snapshot.c 依赖，改了就整机集成红；
 *   ③ **S5.4 的版本门**：`config.apply` 带旧版本号必须被 IV_EBUSY 拦下，
 *      且**失败时版本不动、值不动**（回滚），这是"并发双写不互相覆盖"的全部依据。
 *
 * 工作目录用 mkdtemp 造在 /tmp，退出前把文件与目录删干净（AGENTS.md 规则 6）；
 * 日志改走空 sink，不产生落盘副作用。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <json-c/json.h>

#include "ivsbox/iv_config.h"
#include "ivsbox/iv_ctl.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

static int  g_fail;
static char g_dir[160];

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void noop_sink(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

/* 调一次 ctl；成功时返回解析后的 JSON（调用方负责 put），否则 NULL。 */
static json_object *call(const iv_ctl_ctx_t *ctx, const char *req, int *rc_out,
                         char *raw, size_t rawcap)
{
    size_t len = 0;
    int    rc = iv_ctl_handle(ctx, req, strlen(req), raw, rawcap, &len);

    if (rc_out != NULL)
        *rc_out = rc;
    if (rc != IV_OK)
        return NULL;
    raw[len] = '\0';
    return json_tokener_parse(raw);
}

static int jobool(json_object *j, const char *k, int dflt)
{
    json_object *v = NULL;

    if (j == NULL)
        return dflt;
    if (!json_object_object_get_ex(j, k, &v) || v == NULL)
        return dflt;
    return json_object_get_boolean(v);
}

static int joerr(json_object *j)
{
    json_object *v = NULL;

    if (j == NULL)
        return 0;
    if (!json_object_object_get_ex(j, "e", &v) || v == NULL)
        return 0;
    return (int)json_object_get_int(v);
}

static json_object *jodata(json_object *j)
{
    json_object *v = NULL;

    if (j == NULL)
        return NULL;
    if (!json_object_object_get_ex(j, "d", &v))
        return NULL;
    return v;
}

int main(void)
{
    iv_ctl_ctx_t ctx;
    iv_config_t *cfg;
    char         raw[IV_CTL_RSP_MAX];
    char         tmpl[] = "/tmp/ivctl_XXXXXX";
    json_object *j;
    int          rc;

    iv_log_set_sink(noop_sink, NULL);

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    (void)snprintf(g_dir, sizeof(g_dir), "%s", tmpl);

    cfg = iv_config_open(g_dir, IV_CFG_NAME);
    if (cfg == NULL) {
        fprintf(stderr, "config open failed\n");
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg; /* agent / db 保持 NULL：本用例只测配置与协议外形 */

    /* ---------------- c01 方法表 ---------------- */
    chk(iv_ctl_method_count() > 0, "c01: method count > 0");
    chk(strcmp(iv_ctl_method_name(0), "system.snapshot") == 0,
        "c01: method[0] is system.snapshot");
    chk(iv_ctl_method_name(-1) == NULL, "c01: negative index returns NULL");
    chk(iv_ctl_method_name(iv_ctl_method_count()) == NULL, "c01: overflow index returns NULL");

    /* ---------------- c02 system.snapshot 老格式 ---------------- */
    {
        static const char req[] = "system.snapshot";
        size_t            len = 0;
        int               r;

        memset(raw, 0, sizeof(raw));
        r = iv_ctl_handle(&ctx, req, sizeof(req) - 1u, raw, sizeof(raw), &len);
        chk(r == IV_OK, "c02: snapshot returns OK");
        chk(strstr(raw, "\"method\":\"system.snapshot\"") != NULL, "c02: names the method");
        chk(strstr(raw, "\"status\":\"ok\"") != NULL, "c02: reports status ok");
        chk(strstr(raw, "\"version\":\"") != NULL, "c02: carries a version string");
    }

    /* ---------------- c03 未知方法：静默且不写缓冲 ---------------- */
    {
        static const char req[] = "no.such.method";
        size_t            len = 0;
        int               r;

        memset(raw, 0x7E, sizeof(raw));
        r = iv_ctl_handle(&ctx, req, sizeof(req) - 1u, raw, sizeof(raw), &len);
        chk(r == IV_ENOTSUP, "c03: unknown method returns IV_ENOTSUP");
        chk((unsigned char)raw[0] == 0x7E, "c03: out buffer untouched on ENOTSUP");
    }

    /* ---------------- c04 JSON 形态：sys.overview ---------------- */
    j = call(&ctx, "{\"m\":\"sys.overview\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL, "c04: overview answered");
    chk(jobool(j, "ok", 0) == 1, "c04: ok true");
    chk(jodata(j) != NULL, "c04: has data object");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c05 坏 JSON ---------------- */
    j = call(&ctx, "{not json", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL, "c05: bad json still answered");
    chk(jobool(j, "ok", 1) == 0, "c05: ok false");
    chk(joerr(j) == IV_EPROTO, "c05: e == IV_EPROTO");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c06 缺 m ---------------- */
    j = call(&ctx, "{\"p\":{}}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && joerr(j) == IV_EINVAL, "c06: missing m -> IV_EINVAL");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c07 config.get 已知键 ---------------- */
    j = call(&ctx, "{\"m\":\"config.get\",\"p\":{\"key\":\"net.server.wired.host\"}}",
             &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c07: config.get known key ok");
    {
        json_object *d = jodata(j);
        json_object *t = NULL;

        chk(d != NULL, "c07: data present");
        if (d != NULL && json_object_object_get_ex(d, "type", &t) && t != NULL)
            chk(strcmp(json_object_get_string(t), "str") == 0, "c07: type reported as str");
        else
            chk(0, "c07: type field missing");
    }
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c08 config.get 未知键 ---------------- */
    j = call(&ctx, "{\"m\":\"config.get\",\"p\":{\"key\":\"no.such.key\"}}",
             &rc, raw, sizeof(raw));
    chk(rc == IV_OK && joerr(j) == IV_ENOENT, "c08: unknown key -> IV_ENOENT");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c09 config.class ---------------- */
    j = call(&ctx, "{\"m\":\"config.class\",\"p\":{\"cls\":\"net\"}}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c09: class net ok");
    {
        json_object *d = jodata(j);
        json_object *items = NULL;
        int          n = 0;

        chk(d != NULL, "c09: data present");
        if (d != NULL && json_object_object_get_ex(d, "items", &items) && items != NULL) {
            json_object_object_foreach(items, k, v) {
                (void)v;
                chk(strncmp(k, "net.", 4) == 0, "c09: every key starts with net.");
                n++;
            }
        }
        chk(n > 0, "c09: net class is not empty");
    }
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c09b config.class 坏分类名 ---------------- */
    j = call(&ctx, "{\"m\":\"config.class\",\"p\":{\"cls\":\"nope\"}}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && joerr(j) == IV_EINVAL, "c09b: unknown class -> IV_EINVAL");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c10 config.apply 版本不符 ---------------- */
    j = call(&ctx,
             "{\"m\":\"config.apply\",\"p\":{\"ver\":999999,\"items\":"
             "{\"net.server.wired.host\":\"10.0.0.1\"}}}",
             &rc, raw, sizeof(raw));
    chk(rc == IV_OK && joerr(j) == IV_EBUSY, "c10: stale version -> IV_EBUSY");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c11 config.apply 成功 ---------------- */
    {
        char          req[512];
        uint64_t      before = iv_config_version(cfg);
        const char   *s = NULL;
        int           n;

        n = snprintf(req, sizeof(req),
                     "{\"m\":\"config.apply\",\"p\":{\"ver\":%llu,\"items\":"
                     "{\"net.server.wired.host\":\"10.9.9.9\"}}}",
                     (unsigned long long)before);
        chk(n > 0 && (size_t)n < sizeof(req), "c11: request built");
        j = call(&ctx, req, &rc, raw, sizeof(raw));
        chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c11: apply returns ok");
        chk(iv_config_version(cfg) == before + 1u, "c11: version bumped by one");
        chk(iv_config_get_str(cfg, "net.server.wired.host", &s) == IV_OK && s != NULL &&
                strcmp(s, "10.9.9.9") == 0,
            "c11: new value visible through iv_config");
        {
            json_object *d = jodata(j);

            if (d != NULL) {
                json_object *ap = NULL;

                if (json_object_object_get_ex(d, "applied", &ap) && ap != NULL)
                    chk(json_object_get_int(ap) == 1, "c11: applied == 1");
                else
                    chk(0, "c11: applied field missing");
            }
        }
        if (j != NULL)
            json_object_put(j);
    }

    /* ---------------- c12 apply 拒绝：未知键 / 类型不符，且状态不变 --------- */
    {
        char         req[512];
        uint64_t     v = iv_config_version(cfg);
        const char  *s = NULL;

        (void)snprintf(req, sizeof(req),
                       "{\"m\":\"config.apply\",\"p\":{\"ver\":%llu,\"items\":"
                       "{\"no.such.key\":1}}}", (unsigned long long)v);
        j = call(&ctx, req, &rc, raw, sizeof(raw));
        chk(rc == IV_OK && joerr(j) == IV_ENOENT, "c12: unknown key rejected");
        if (j != NULL)
            json_object_put(j);

        (void)snprintf(req, sizeof(req),
                       "{\"m\":\"config.apply\",\"p\":{\"ver\":%llu,\"items\":"
                       "{\"net.server.wired.host\":123}}}", (unsigned long long)v);
        j = call(&ctx, req, &rc, raw, sizeof(raw));
        chk(rc == IV_OK && joerr(j) == IV_EPROTO, "c12: type mismatch rejected");
        if (j != NULL)
            json_object_put(j);

        chk(iv_config_version(cfg) == v, "c12: version unchanged after rejects");
        chk(iv_config_get_str(cfg, "net.server.wired.host", &s) == IV_OK && s != NULL &&
                strcmp(s, "10.9.9.9") == 0,
            "c12: value not clobbered by rejected apply");
    }

    /* ---------------- c13 agent 缺失时降级 ---------------- */
    j = call(&ctx, "{\"m\":\"device.list\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && joerr(j) == IV_ESTATE, "c13: device.list without agent -> IV_ESTATE");
    if (j != NULL)
        json_object_put(j);

    j = call(&ctx, "{\"m\":\"snmp.result\",\"p\":{\"which\":\"cam\"}}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && joerr(j) == IV_ESTATE, "c13b: snmp.result without agent -> IV_ESTATE");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c14 defaults / all 非空 ---------------- */
    j = call(&ctx, "{\"m\":\"config.defaults\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c14: defaults ok");
    {
        json_object *d = jodata(j);
        json_object *items = NULL;

        if (d != NULL && json_object_object_get_ex(d, "items", &items) && items != NULL)
            chk(json_object_object_length(items) > 0, "c14: defaults not empty");
        else
            chk(0, "c14: defaults items missing");
    }
    if (j != NULL)
        json_object_put(j);

    j = call(&ctx, "{\"m\":\"config.all\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c14b: config.all ok");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c15 越界请求被拒 ---------------- */
    {
        size_t len = 0;
        size_t big = (size_t)IV_CTL_RSP_MAX + 1u;

        chk(iv_ctl_handle(&ctx, "x", big, raw, sizeof(raw), &len) == IV_EINVAL,
            "c15: oversized request rejected");
    }

    /* ---------------- 收尾（规则 6） ---------------- */
    iv_config_close(cfg);
    {
        char p[200];

        (void)snprintf(p, sizeof(p), "%s/ivsbox.json", g_dir);
        (void)unlink(p);
        (void)snprintf(p, sizeof(p), "%s/ivsbox.json.tmp", g_dir);
        (void)unlink(p);
        (void)rmdir(g_dir);
    }

    if (g_fail != 0) {
        fprintf(stderr, "test_ctl FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_ctl passed (protocol shape, silent unknown, version gate, apply rollback)\n");
    return 0;
}
