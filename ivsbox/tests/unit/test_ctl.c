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
 *   ④ **D-08 / D-10 的两个语义**（2026-10-22）：`system.snapshot` 除老三字段外
 *      必须带**真快照**（开机时长 ＋ 配置诊断）；`record.status` 的三种"没在录"
 *      必须分开 —— 没接线＝ENOTSUP、桥查不到＝`ok:true`＋`known:false`、
 *      id 越界＝EINVAL。
 *
 * 工作目录用 mkdtemp 造在 /tmp，退出前把文件与目录删干净（AGENTS.md 规则 6）；
 * 日志改走空 sink，不产生落盘副作用。
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <json-c/json.h>

#include "ivsbox/iv_config.h"
#include "ivsbox/iv_ctl.h"
#include "ivsbox/iv_db.h"
#include "ivsbox/iv_store.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_media.h" /* IV_MEDIA_CH_N（c16 媒体方法的 id 上限） */
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_tunnel.h" /* c2x 通道方法（M8-S8.3） */

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

static iv_tunnel_t g_tun;
/* 隧道落点（临时目录）：配置文件与标记文件都放这里，绝不碰 /etc/frp 与 /tmp。 */
static char g_tdir[256];
static char g_tconf[512];
static char g_tflag[512];

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

/* 取整数字段。**不能用 jobool()**：`json_object_get_boolean()` 对任何非零整数
 * 都返回 1，`cfg_changed > 0` / `version > v0` 这类断言会因此全部失去区分力
 * （c25k 首版就是这么假绿的）。 */
static int jint(json_object *j, const char *k, int dflt)
{
    json_object *v = NULL;

    if (j == NULL)
        return dflt;
    if (!json_object_object_get_ex(j, k, &v) || v == NULL)
        return dflt;
    return (int)json_object_get_int(v);
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

/* ---- c16 媒体假 ops（M6-S6.1）：只计数与回显，不做任何 I/O ---- */
static int g_n_cap, g_n_start, g_n_stop, g_n_status;

static int media_fake_capture(const char *path, char *out_path, size_t cap,
                              void *arg)
{
    (void)arg;
    g_n_cap++;
    (void)snprintf(out_path, cap, "%s", (path != NULL) ? path : "snap_auto.jpg");
    return IV_OK;
}

static int media_fake_start(int id, const char *url, void *arg)
{
    (void)arg;
    (void)url;
    g_n_start++;
    return (id >= 0 && id < IV_MEDIA_CH_N) ? IV_OK : IV_EINVAL;
}

static int media_fake_stop(int id, void *arg)
{
    (void)arg;
    (void)id;
    g_n_stop++;
    return IV_OK;
}

/* c27 要模拟"桥连不上"（D-10）：真实 iv_media_record_status() 在 media 进程
 * 不可用时回 IV_ECONN，假 ops 必须能复现同一个码，否则测不到那条映射。
 * **默认 IV_OK**，只有 c27 段临时改它，改完必须还原，免得影响后续块。 */
static int g_status_rc = IV_OK;

static int media_fake_status(int id, int *recording, void *arg)
{
    (void)arg;
    (void)id;
    g_n_status++;
    if (g_status_rc != IV_OK)
        return g_status_rc;
    *recording = 1;
    return IV_OK;
}

/* ---- c28 手动录像（2026-10-28，record.page / record.manual） ----------------
 * 三个假钩子合起来能造出页面要显示的四种状态：配了/没配、开着/关着、
 * 在录/没在录、失败带原因。第 0 路**永久**带一条失败原因 —— 页面把"原因"
 * 抄错（比如抄成上一路的）就会在 c28 里露馅。 */
static int         g_n_manual;
static int         g_manual_on;
static int         g_manual_rc = IV_OK;      /* 设成 IV_ESTATE 可测"失败原因透传" */
static int         g_rec_state = 1;          /* 1 在录 / 0 未在录 / -1 不可用 */
static int         g_rec_forced;             /* 强制态标记 */
static int         g_rec_held_off;           /* 人工停止后压制自动开录 */
static const char *g_manual_why = "摄像机端口未开放：192.168.1.64:554 拒绝连接";

static int media_fake_reason(int id, char *buf, size_t cap, void *arg)
{
    (void)arg;
    if (buf != NULL && cap > 0u)
        buf[0] = '\0';
    if (id != 0)
        return 0;
    if (buf != NULL)
        (void)snprintf(buf, cap, "配置错误：拉流地址必须以 rtsp:// 开头");
    return 1;
}

static int media_fake_rstate(int *forced, int *held_off, void *arg)
{
    (void)arg;
    if (forced != NULL)
        *forced = g_rec_forced;
    if (held_off != NULL)
        *held_off = g_rec_held_off;
    return g_rec_state;
}

static int media_fake_rmanual(int on, char *why, size_t cap, void *arg)
{
    (void)arg;
    g_n_manual++;
    g_manual_on = on;
    if (why != NULL && cap > 0u)
        why[0] = '\0';
    if (g_manual_rc != IV_OK) {
        if (on && why != NULL && cap > 0u)
            (void)snprintf(why, cap, "%s", g_manual_why);
        return g_manual_rc;
    }
    return IV_OK;
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

    /* S9 延伸：真 db + store，验 config.apply 的审计落盘（装配接线断言）。 */
    static char         dbtmpl[] = "/tmp/ivdb_XXXXXX";
    static char         g_ddir[64];
    static iv_db_t     *tdb = NULL;
    static iv_store_t  *tstore = NULL;
    int64_t             an = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg; /* agent 保持 NULL：本用例只测配置与协议外形 */
    if (mkdtemp(dbtmpl) == NULL) {
        fprintf(stderr, "mkdtemp(db) failed\n");
        return 1;
    }
    (void)snprintf(g_ddir, sizeof(g_ddir), "%s", dbtmpl);
    {
        char dbp[128];

        (void)snprintf(dbp, sizeof(dbp), "%s/control.db", g_ddir);
        if (iv_db_open(dbp, &tdb) != IV_OK ||
            iv_store_open(tdb, &tstore) != IV_OK) {
            fprintf(stderr, "store open failed\n");
            return 1;
        }
    }
    ctx.store = tstore;

    /* ---------------- c17 接缝未接：tunnel.* 必须报"没接"，不是"不许调" ---------------- */
    j = call(&ctx, "{\"m\":\"tunnel.status\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_ENOTSUP, "c17: tunnel.status with no tunnel attached returns IV_ENOTSUP");

    /* ---------------- c2x 通道方法（M8-S8.3）：接上真句柄 ---------------- */
    {
        static char tmpl[] = "/tmp/ivtun_XXXXXX";

        if (mkdtemp(tmpl) == NULL) {
            fprintf(stderr, "mkdtemp(tunnel) failed\n");
            return 1;
        }
        (void)snprintf(g_tdir, sizeof(g_tdir), "%s", tmpl);
        (void)snprintf(g_tconf, sizeof(g_tconf), "%s/frpc.toml", g_tdir);
        (void)snprintf(g_tflag, sizeof(g_tflag), "%s/tunnel.enabled", g_tdir);
    }
    iv_tunnel_init(&g_tun);
    chk(iv_tunnel_set_identity(&g_tun, "TESTSN01", "test-token") == IV_OK, "c2x: identity");
    chk(iv_tunnel_set_server(&g_tun, "10.9.9.9", 0) == IV_OK, "c2x: server (port defaults)");
    chk(iv_tunnel_set_paths(&g_tun, g_tconf, g_tflag) == IV_OK, "c2x: paths");
    ctx.tunnel = &g_tun;

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
    /* S6.5：总览里**恒有** storage 段；未挂 agent 时它**恰好**是 `{"enabled":false}`。
     * 三个一起守住：① 出口没被删掉；② 未启用**不**被当成"体检通过"（`storage` 是
     * build_overview 最后追加的成员，故它以 `}}` 收尾 —— 零值字段一旦被塞进来，
     * 这个收尾就不再成立）；③ 键顺序未被无意改动。 */
    chk(strstr(raw, "\"storage\":{\"enabled\":false}}") != NULL,
        "c04: overview carries storage={enabled:false} and nothing else when no agent is attached");
    /* c04b 设备身份段：2026-10-26 用户要求 Web 上显示「软件版本 / 硬件型号 /
     * 设备类型」。钉住三样真的都出了口，且**设备类型只有一个来源**：
     * `IV_PROTO_DEVTYPE_DEFAULT`，与上报帧头 `DEVTYPE=`、查询应答 CRC 是同一个宏。
     * `devtype` 用小写 4 位（与上报帧 `%04x` 一致），前端再转大写显示 ——
     * 同一个码在两个界面上不能一处大写一处小写。 */
    chk(strstr(raw, "\"devtype\":\"0a00\"") != NULL,
        "c04b: device.devtype is 0a00 (IV_PROTO_DEVTYPE_DEFAULT)");
    chk(strstr(raw, "\"devtype_name\"") == NULL,
        "c04b: no devtype_name in JSON (Chinese labels live in app.js, not in C)");
    chk(strstr(raw, "\"sysver\":\"") != NULL, "c04b: device.sysver present");
    chk(strstr(raw, "\"hw_model\":\"FN_RZTY_PY\"") != NULL,
        "c04b: device.hw_model comes from sys.device.model default");
    /* 新旧两个「型号」必须并存、且互不顶替：`model` 是设备树串（宿主机上没有 ⇒
     * null，这一条由 b08 在集成侧钉），`hw_model` 是配置键。把 hw_model 写成
     * model 的别名会在这里变红。 */
    chk(strstr(raw, "\"model\":null") != NULL,
        "c04b: device.model stays the device-tree string (null on host)");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c31 默认网关只在出口有载波时采信（2026-10-19 修正） ----------------
     * 缺陷原型：断链后内核**不删**默认路由（只打 `RTNH_F_LINKDOWN`，而这一位
     * 并不出现在 `/proc/net/route` 的 Flags 列 —— 那列由 `fib_flag_trans()` 现算），
     * 旧实现照单全收 ⇒ 网络页把一条已经断掉的链路报成"默认网关"，与 4G 页
     * "SIM 卡未插入"自相矛盾。判定抽成纯函数就是为了能在这里钉死，不必等真机
     * 掉进那个窗口才有机会复现。 */
    {
        iv_ctl_route_t cand[2];
        json_object   *d, *net, *link;

        (void)memset(cand, 0, sizeof(cand));
        (void)snprintf(cand[0].ifname, sizeof(cand[0].ifname), "usb0");
        (void)snprintf(cand[0].gw, sizeof(cand[0].gw), "10.158.0.1");
        (void)snprintf(cand[1].ifname, sizeof(cand[1].ifname), "eth0");
        (void)snprintf(cand[1].gw, sizeof(cand[1].gw), "192.168.2.1");
        cand[0].iface_ok = 0; /* 无载波：内核那条 default 还在，报文却出不去 */
        cand[1].iface_ok = 0;
        chk(iv_ctl_route_pick(cand, 2) == -1, "c31: all dead => no gateway (-1)");
        cand[1].iface_ok = 1;
        chk(iv_ctl_route_pick(cand, 2) == 1,
            "c31: skips the dead route and picks the ready one");
        cand[0].iface_ok = 1;
        chk(iv_ctl_route_pick(cand, 2) == 0, "c31: keeps /proc order when both are ready");
        chk(iv_ctl_route_pick(NULL, 2) == -1, "c31: NULL candidates => -1");
        chk(iv_ctl_route_pick(cand, 0) == -1, "c31: empty list => -1");
        chk(iv_ctl_route_pick(cand, -3) == -1, "c31: negative count => -1");

        /* JSON 外形：ifaces 每项都要带 `carrier`，link 要带 `gateway_ignored`
         * —— 前端靠这两样把"关了 / 断了 / 连着"分开说。 */
        j    = call(&ctx, "{\"m\":\"sys.overview\"}", &rc, raw, sizeof(raw));
        d    = jodata(j);
        net  = NULL;
        link = NULL;
        if (d != NULL)
            (void)json_object_object_get_ex(d, "net", &net);
        if (net != NULL)
            (void)json_object_object_get_ex(net, "link", &link);
        chk(link != NULL, "c31: net.link present");
        if (link != NULL) {
            json_object *v   = NULL;
            json_object *ifc = NULL;

            chk(json_object_object_get_ex(link, "gateway_ignored", &v) && v != NULL,
                "c31: net.link carries gateway_ignored");
            chk(json_object_object_get_ex(link, "ifaces", &ifc) && ifc != NULL &&
                    json_object_is_type(ifc, json_type_array) &&
                    json_object_array_length(ifc) == 2u,
                "c31: net.link.ifaces is a 2-element array (eth0, usb0)");
            if (ifc != NULL && json_object_is_type(ifc, json_type_array)) {
                size_t k;
                int    have = 1;

                for (k = 0; k < json_object_array_length(ifc); k++) {
                    json_object *e  = json_object_array_get_idx(ifc, k);
                    json_object *cv = NULL;

                    if (e == NULL ||
                        !json_object_object_get_ex(e, "carrier", &cv) || cv == NULL ||
                        !json_object_is_type(cv, json_type_boolean))
                        have = 0;
                }
                chk(have, "c31: every iface carries a boolean `carrier` flag");
            }
        }
        if (j != NULL)
            json_object_put(j);
    }

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

        /* S9 延伸：成功的写操作必须留痕（审计里只有键数，绝无键值）。 */
        chk(iv_store_rows(ctx.store, "audit", &an) == IV_OK && an == 1,
            "c11a: config.apply wrote one audit row");
        chk(iv_db_scalar_int(tdb,
                "SELECT COUNT(*) FROM audit WHERE method='config.apply'"
                " AND detail='items=1' AND result=0;", &an) == IV_OK && an == 1,
            "c11b: audit row has method/detail/result as designed");
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

    /* -------- c12c 超长字符串 / 非法 IP：必须**报错**，不许 ok:true 却没写 --------
     * R-03 病根：预检只查"键存在 + 类型对"，长度交给写入阶段，而写入阶段把
     * setter 的返回值 `(void)` 丢弃 ⇒ 超长值被 IV_ERANGE 静默拒掉，接口照样回
     * `ok:true applied:1`，网页显示保存成功、回读还是旧值。 */
    {
        char     req[640];
        uint64_t v = iv_config_version(cfg);
        const char *s = NULL;
        size_t   i;
        size_t   pos;

        pos = (size_t)snprintf(req, sizeof(req),
                               "{\"m\":\"config.apply\",\"p\":{\"ver\":%llu,\"items\":"
                               "{\"net.server.wired.host\":\"",
                               (unsigned long long)v);
        for (i = 0; i < 200u && pos + 3u < sizeof(req); i++)
            req[pos++] = 'a';
        (void)snprintf(req + pos, sizeof(req) - pos, "\"}}}");

        j = call(&ctx, req, &rc, raw, sizeof(raw));
        chk(rc == IV_OK && joerr(j) == IV_ERANGE, "c12c: over-long string rejected");
        if (j != NULL)
            json_object_put(j);
        chk(iv_config_get_str(cfg, "net.server.wired.host", &s) == IV_OK && s != NULL &&
                strcmp(s, "10.9.9.9") == 0,
            "c12d: over-long value did not clobber the current one");
        chk(iv_config_version(cfg) == v, "c12e: version unchanged by the rejected write");

        /* R-05 经 ctl 这条路：非法 IP 必须在写入侧被拒、且不落盘 */
        (void)snprintf(req, sizeof(req),
                       "{\"m\":\"config.apply\",\"p\":{\"ver\":%llu,\"items\":"
                       "{\"net.ip\":\"999.1.1.1\"}}}", (unsigned long long)v);
        j = call(&ctx, req, &rc, raw, sizeof(raw));
        chk(rc == IV_OK && joerr(j) == IV_EINVAL, "c12f: bogus ip rejected");
        if (j != NULL)
            json_object_put(j);
        chk(iv_config_get_str(cfg, "net.ip", &s) == IV_OK && s != NULL && s[0] == '\0',
            "c12g: bogus ip left the key at its default");
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

    /* ==================== c16 媒体方法（M6-S6.1，假 ops 注入） ====================
     * f1 若 media==NULL 未按 IV_ENOTSUP 应答（写出了别的东西），c16a 会红；
     * f2 若 ops 假实现没被调到（路由漏接），c16b 的计数器不会动；
     * f3 若 record.status 的布尔应答写反，c16c 会红。 */
    {
        static const iv_ctl_media_ops_t ops = {
            media_fake_capture, media_fake_start, media_fake_stop,
            media_fake_status,  media_fake_reason,
            media_fake_rstate,  media_fake_rmanual,
            NULL
        };

        /* c16a 未挂 ops：四个方法一律 IV_ENOTSUP（"能调但没接"可辨） */
        j = call(&ctx, "{\"m\":\"snapshot.take\"}", &rc, raw, sizeof(raw));
        chk(rc == IV_OK && j != NULL && joerr(j) == IV_ENOTSUP,
            "c16a: snapshot.take without ops -> ENOTSUP (f1)");
        if (j != NULL)
            json_object_put(j);
        j = call(&ctx, "{\"m\":\"record.start\",\"p\":{\"id\":0,\"url\":\"rtsp://x\"}}",
                 &rc, raw, sizeof(raw));
        chk(rc == IV_OK && j != NULL && joerr(j) == IV_ENOTSUP,
            "c16a: record.start without ops -> ENOTSUP");
        if (j != NULL)
            json_object_put(j);

        /* c16b 挂上假 ops：capture 入队应答带自动命名路径 */
        ctx.media     = &ops;
        ctx.media_arg = NULL;
        j = call(&ctx, "{\"m\":\"snapshot.take\"}", &rc, raw, sizeof(raw));
        chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c16b: capture accepted");
        {
            json_object *d = jodata(j);
            json_object *v = NULL;

            if (d != NULL && json_object_object_get_ex(d, "path", &v))
                chk(strcmp(json_object_get_string(v), "snap_auto.jpg") == 0,
                    "c16b: auto path echoed");
            else
                chk(0, "c16b: path missing");
        }
        if (j != NULL)
            json_object_put(j);
        chk(g_n_cap == 1, "c16b: fake capture called once (f2)");
        /* 显式 path 透传 */
        j = call(&ctx, "{\"m\":\"snapshot.take\",\"p\":{\"path\":\"/tmp/x.jpg\"}}",
                 &rc, raw, sizeof(raw));
        chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c16b: explicit path accepted");
        if (j != NULL)
            json_object_put(j);
        chk(g_n_cap == 2, "c16b: fake capture called twice");

        /* c16c record.start：合法 id 受理；非法 id 拒 */
        j = call(&ctx, "{\"m\":\"record.start\",\"p\":{\"id\":1,\"url\":\"rtsp://10.0.0.5/ch1\"}}",
                 &rc, raw, sizeof(raw));
        chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c16c: start accepted");
        if (j != NULL)
            json_object_put(j);
        chk(g_n_start == 1, "c16c: fake start called");
        j = call(&ctx, "{\"m\":\"record.start\",\"p\":{\"id\":99,\"url\":\"rtsp://x\"}}",
                 &rc, raw, sizeof(raw));
        chk(rc == IV_OK && j != NULL && joerr(j) == IV_EINVAL,
            "c16c: bad id rejected before reaching ops");
        if (j != NULL)
            json_object_put(j);
        chk(g_n_start == 1, "c16c: ops NOT called for bad id");
        j = call(&ctx, "{\"m\":\"record.start\",\"p\":{\"id\":0}}",
                 &rc, raw, sizeof(raw));
        chk(rc == IV_OK && j != NULL && joerr(j) == IV_EINVAL,
            "c16c: missing url rejected");
        if (j != NULL)
            json_object_put(j);

        /* c16d record.stop / record.status */
        j = call(&ctx, "{\"m\":\"record.stop\",\"p\":{\"id\":1}}", &rc, raw, sizeof(raw));
        chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c16d: stop accepted");
        if (j != NULL)
            json_object_put(j);
        chk(g_n_stop == 1, "c16d: fake stop called");
        j = call(&ctx, "{\"m\":\"record.status\",\"p\":{\"id\":2}}", &rc, raw, sizeof(raw));
        chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c16d: status answered");
        {
            json_object *d = jodata(j);
            json_object *v = NULL;

            if (d != NULL && json_object_object_get_ex(d, "recording", &v))
                chk(json_object_get_boolean(v) == 1, "c16d: recording flag true (f3)");
            else
                chk(0, "c16d: recording field missing");
        }
        if (j != NULL)
            json_object_put(j);
        chk(g_n_status == 1, "c16d: fake status called");
    }

    /* ==================== c28 手动录像（2026-10-28：record.page / record.manual） ====================
     * 这一页存在的理由只有一条：**起不来要能看出为什么**（用户 2026-10-28 原话）。
     * 因此钉四件事：
     *   · c28a/c28b `record.page` 一次给全 8 路，且**绝不回 RTSP 地址**（含账密）；
     *   · c28c    `record.manual` 的 on 真的传到了 ops；
     *   · c28d    失败时后端那句 why **原样**当 msg 回 —— 被改写成
     *             "record start failed" 就失去意义，现场得照着这句话修；
     *   · c28e    缺 on 参数 ⇒ EINVAL 且不调 ops。
     *
     * 全程只用假 ops 与假配置，不碰媒体进程；用完不残留任何配置改动。 */
    {
        json_object *j;
        int          n0;

        /* c28a 外形：recording/forced/ch 齐全，ch 恰好 8 条 */
        j = call(&ctx, "{\"m\":\"record.page\"}", &rc, raw, sizeof(raw));
        chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c28a: record.page answered");
        chk(strstr(raw, "rtsp://") == NULL, "c28a: no rtsp url leaks into the page (f7)");
        {
            json_object *d   = jodata(j);
            json_object *v   = NULL;
            json_object *arr = NULL;

            chk(d != NULL && json_object_object_get_ex(d, "ch", &arr) &&
                    json_object_is_type(arr, json_type_array),
                "c28a: ch array present");
            if (arr != NULL)
                chk(json_object_array_length(arr) == (size_t)IV_RECCTL_CH_N,
                    "c28a: one entry per recctl channel");
            if (d != NULL && json_object_object_get_ex(d, "recording", &v))
                chk(json_object_get_boolean(v) == 1, "c28a: recording mirrors ops");
            else
                chk(0, "c28a: recording field missing");
            v = NULL;
            if (d != NULL && json_object_object_get_ex(d, "forced", &v))
                chk(json_object_get_boolean(v) == 0, "c28a: forced false by default");
            else
                chk(0, "c28a: forced field missing");
            /* held_off 必须出现：它是"人工停过、正在压制自动开录"的唯一证据，
             * 少了它页面只能干巴巴说"未录像"（2026-10-28 板端踩过）。 */
            v = NULL;
            if (d != NULL && json_object_object_get_ex(d, "held_off", &v))
                chk(json_object_get_boolean(v) == 0, "c28a: held_off false by default");
            else
                chk(0, "c28a: held_off field missing");
            /* 第 0 路：假 reason 报它失败 ⇒ on 必须是 false，且 why 得是**那一路**的原因 */
            if (arr != NULL && json_object_array_length(arr) > 0) {
                json_object *e0 = json_object_array_get_idx(arr, 0);
                json_object *w  = NULL;
                json_object *on = NULL;

                if (e0 != NULL && json_object_object_get_ex(e0, "on", &on))
                    chk(json_object_get_boolean(on) == 0,
                        "c28b: a failed channel is never reported as recording");
                else
                    chk(0, "c28b: on field missing");
                if (e0 != NULL && json_object_object_get_ex(e0, "why", &w))
                    chk(strcmp(json_object_get_string(w),
                               "配置错误：拉流地址必须以 rtsp:// 开头") == 0,
                        "c28b: per-channel reason belongs to that channel (f8)");
                else
                    chk(0, "c28b: failed channel carries no reason");
            }
            /* c28b2 最后一个通道没配地址、开关也是关的 ⇒ 整机在录也**不许**报它在录。
             * 2026-10-28 板端实测踩到过：把"整机在录"当成每路都在录，8 路全显示
             * "录制中"，页面直接骗人。 */
            if (arr != NULL && json_object_array_length(arr) > 1) {
                json_object *e7 = json_object_array_get_idx(
                    arr, json_object_array_length(arr) - 1);
                json_object *on = NULL;

                if (e7 != NULL && json_object_object_get_ex(e7, "on", &on))
                    chk(json_object_get_boolean(on) == 0,
                        "c28b2: an unconfigured channel is never reported as recording");
                else
                    chk(0, "c28b2: on field missing on an unconfigured channel");
            }
        }
        if (j != NULL)
            json_object_put(j);

        /* c28c on 真的传到底 */
        j = call(&ctx, "{\"m\":\"record.manual\",\"p\":{\"on\":true}}", &rc, raw,
                 sizeof(raw));
        chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c28c: manual start accepted");
        chk(g_n_manual == 1 && g_manual_on == 1, "c28c: on=1 reached the ops (f9)");
        if (j != NULL)
            json_object_put(j);
        j = call(&ctx, "{\"m\":\"record.manual\",\"p\":{\"on\":false}}", &rc, raw,
                 sizeof(raw));
        chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c28c: manual stop accepted");
        chk(g_n_manual == 2 && g_manual_on == 0, "c28c: on=0 reached the ops");
        if (j != NULL)
            json_object_put(j);

        /* c28d 失败原因原样当 msg（f10） */
        g_manual_rc = IV_ESTATE;
        j = call(&ctx, "{\"m\":\"record.manual\",\"p\":{\"on\":true}}", &rc, raw,
                 sizeof(raw));
        chk(rc == IV_OK && j != NULL && joerr(j) == IV_ESTATE,
            "c28d: failure surfaces as an error code");
        {
            json_object *m = NULL;

            chk(j != NULL && json_object_object_get_ex(j, "msg", &m) && m != NULL &&
                    strcmp(json_object_get_string(m), g_manual_why) == 0,
                "c28d: the ops' why is passed through verbatim (f10)");
        }
        if (j != NULL)
            json_object_put(j);
        g_manual_rc = IV_OK;

        /* c28e 缺 on ⇒ EINVAL 且不调 ops */
        n0 = g_n_manual;
        j = call(&ctx, "{\"m\":\"record.manual\",\"p\":{}}", &rc, raw, sizeof(raw));
        chk(rc == IV_OK && j != NULL && joerr(j) == IV_EINVAL, "c28e: missing on rejected");
        chk(g_n_manual == n0, "c28e: ops NOT called when on is missing");
        if (j != NULL)
            json_object_put(j);
    }

    /* ==================== c26 D-08：system.snapshot 必须带真快照内容 ====================
     * 原实现只回 `{method,version,status}`（S10 兼容垫片），方法名与行为不符。
     * 修法是**保留老三个字段**（c02 与集成用例 test_ctl_snapshot 逐字断言过，
     * 前端也在读 status），在后面追加只读快照 —— 追加不破坏任何既有断言。
     *
     * f1 若又把快照内容删回空壳，c26b/c26c 会红；
     * f2 若 cfg 段谎报版本号（不取 iv_config_version），c26d 会红；
     * f3 若把 uptime 写成常量 0，c26a 的"类型 + 非负"抓不到 —— 故用两条：类型是
     *    整数、且第二次调用不早于第一次（单调不减），常量 0 才无处可藏。 */
    {
        static const char req[] = "system.snapshot";
        int64_t           up1 = -1;
        int64_t           up2 = -1;
        int               k;

        for (k = 0; k < 2; k++) {
            size_t       len = 0;
            int          r;
            json_object *j;
            json_object *v = NULL;

            memset(raw, 0, sizeof(raw));
            r = iv_ctl_handle(&ctx, req, sizeof(req) - 1u, raw, sizeof(raw), &len);
            chk(r == IV_OK, "c26a: snapshot returns OK");
            /* 老格式原样保留（这三条是 S10 契约，不是本次新加的） */
            chk(strstr(raw, "\"method\":\"system.snapshot\"") != NULL, "c26a: old method field kept");
            chk(strstr(raw, "\"status\":\"ok\"") != NULL, "c26a: old status field kept");
            chk(strstr(raw, "\"version\":\"") != NULL, "c26a: old version field kept");

            j = json_tokener_parse(raw);
            chk(j != NULL, "c26a: snapshot is valid JSON");
            if (j != NULL) {
                chk(json_object_object_get_ex(j, "uptime_ms", &v) &&
                        json_object_get_type(v) == json_type_int,
                    "c26a: uptime_ms present and integer");
                if (v != NULL && json_object_get_type(v) == json_type_int)
                    (k == 0) ? (up1 = json_object_get_int64(v))
                             : (up2 = json_object_get_int64(v));
                v = NULL;
                chk(json_object_object_get_ex(j, "cfg", &v) &&
                        json_object_get_type(v) == json_type_object,
                    "c26b: cfg section present and object");
                if (v != NULL && json_object_get_type(v) == json_type_object) {
                    json_object *w = NULL;

                    if (json_object_object_get_ex(v, "items", &w))
                        chk(json_object_get_type(w) == json_type_int &&
                                json_object_get_int64(w) > 0,
                            "c26b: cfg.items is a positive integer");
                    else
                        chk(0, "c26b: cfg.items missing");
                    w = NULL;
                    if (json_object_object_get_ex(v, "version", &w))
                        chk(json_object_get_type(w) == json_type_int &&
                                (uint64_t)json_object_get_int64(w) == iv_config_version(cfg),
                            "c26d: cfg.version equals the live config version (f2)");
                    else
                        chk(0, "c26d: cfg.version missing");
                    w = NULL;
                    if (json_object_object_get_ex(v, "dirty", &w))
                        chk(json_object_get_type(w) == json_type_boolean,
                            "c26b: cfg.dirty is boolean");
                    else
                        chk(0, "c26b: cfg.dirty missing");
                    w = NULL;
                    if (json_object_object_get_ex(v, "loaded", &w))
                        chk(json_object_get_type(w) == json_type_boolean,
                            "c26b: cfg.loaded is boolean");
                    else
                        chk(0, "c26b: cfg.loaded missing");
                }
                json_object_put(j);
            }
        }
        chk(up1 >= 0 && up2 >= up1, "c26a: uptime_ms non-negative and monotonic (f3)");

        /* c26e 没有配置句柄时（schedule 侧只借到半个 ctx）不许崩，也不许回 ESTATE
         * —— system.snapshot 是唯一允许 cfg==NULL 的方法（见 iv_ctl.h 的 ctx 注释）。 */
        {
            iv_ctl_ctx_t bare;
            size_t       len = 0;
            int          r;

            memset(&bare, 0, sizeof(bare));
            memset(raw, 0, sizeof(raw));
            r = iv_ctl_handle(&bare, req, sizeof(req) - 1u, raw, sizeof(raw), &len);
            chk(r == IV_OK, "c26e: snapshot works without cfg");
            chk(strstr(raw, "\"status\":\"ok\"") != NULL, "c26e: still status ok");
            chk(strstr(raw, "\"items\"") == NULL, "c26e: no fabricated cfg numbers");
        }
    }

    /* ==================== c27 D-10：record.status 的"查不到"≠"参数错" ====================
     * 真实 iv_media_record_status() 在 media 进程不可用时回 IV_ECONN(-22)，
     * 原实现把它当业务失败整体回 `{"ok":false,"e":-22,"msg":"record status failed"}`，
     * 而 -22 在错误码表里是"连不上"，页面上被当参数错处理 —— 语义完全错位。
     * 正确口径：
     *   · 未接 ops（ctx->media==NULL）        ⇒ IV_ENOTSUP（"能调但没接"，不变）
     *   · 接上了但桥查不到（IV_ECONN/ESTATE）⇒ **ok:true + recording:false + known:false**
     *   · id 越界                            ⇒ IV_EINVAL 且**根本不调 ops**
     *
     * f4 若把"查不到"又变回 ok:false，c27b 会红；
     * f5 若把 known 写成 true（谎称"确实没在录"），c27c 会红；
     * f6 若 id 校验挪到 ops 调用之后，c27e 的计数器会动。 */
    {
        int n0;

        /* c27a 没接 ops：仍是 ENOTSUP（与 c16a 同一口径，这里显式钉 record.status） */
        {
            iv_ctl_ctx_t      bare;
            static const char req[] = "{\"m\":\"record.status\",\"p\":{\"id\":0}}";
            size_t            len = 0;
            json_object      *j;

            memset(&bare, 0, sizeof(bare));
            memset(raw, 0, sizeof(raw));
            chk(iv_ctl_handle(&bare, req, sizeof(req) - 1u, raw, sizeof(raw), &len) == IV_OK,
                "c27a: answered");
            j = json_tokener_parse(raw);
            chk(j != NULL && joerr(j) == IV_ENOTSUP,
                "c27a: no ops -> ENOTSUP (distinguishable from ECONN)");
            if (j != NULL)
                json_object_put(j);
        }

        /* c27b/c27c 桥连不上 */
        g_status_rc = IV_ECONN;
        {
            json_object *j;
            json_object *d;
            json_object *v = NULL;

            memset(raw, 0, sizeof(raw));
            j = call(&ctx, "{\"m\":\"record.status\",\"p\":{\"id\":0}}", &rc, raw, sizeof(raw));
            chk(rc == IV_OK && jobool(j, "ok", 0) == 1,
                "c27b: media unreachable still answers ok:true (f4)");
            chk(j == NULL || joerr(j) == 0, "c27b: no error code for a mere unreachable bridge");
            d = jodata(j);
            chk(d != NULL, "c27b: data present");
            if (d != NULL && json_object_object_get_ex(d, "recording", &v))
                chk(json_object_get_boolean(v) == 0,
                    "c27b: recording false (conservative: don't claim it's recording)");
            else
                chk(0, "c27b: recording field missing");
            v = NULL;
            if (d != NULL && json_object_object_get_ex(d, "known", &v))
                chk(json_object_get_boolean(v) == 0, "c27c: known false (f5)");
            else
                chk(0, "c27c: known field missing");
            v = NULL;
            if (d != NULL && json_object_object_get_ex(d, "reason", &v))
                chk(json_object_get_type(v) == json_type_string &&
                        json_object_get_string_len(v) > 0,
                    "c27c: reason explains why unknown");
            else
                chk(0, "c27c: reason field missing");
            v = NULL;
            if (d != NULL && json_object_object_get_ex(d, "id", &v))
                chk(json_object_get_int(v) == 0, "c27c: id echoed");
            else
                chk(0, "c27c: id missing");
            if (j != NULL)
                json_object_put(j);
        }

        /* c27d 桥正常时 known:true，避免"永远 known:false"也能过 c27c */
        g_status_rc = IV_OK;
        {
            json_object *j;
            json_object *d;
            json_object *v = NULL;

            memset(raw, 0, sizeof(raw));
            j = call(&ctx, "{\"m\":\"record.status\",\"p\":{\"id\":2}}", &rc, raw, sizeof(raw));
            chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c27d: normal path ok");
            d = jodata(j);
            if (d != NULL && json_object_object_get_ex(d, "known", &v))
                chk(json_object_get_boolean(v) == 1, "c27d: known true when bridge answers");
            else
                chk(0, "c27d: known field missing on the normal path");
            v = NULL;
            if (d != NULL && json_object_object_get_ex(d, "recording", &v))
                chk(json_object_get_boolean(v) == 1, "c27d: recording from the bridge");
            else
                chk(0, "c27d: recording missing on the normal path");
            if (j != NULL)
                json_object_put(j);
        }

        /* c27e id 越界 ⇒ EINVAL 且 ops 一次都不该被调（f6） */
        n0 = g_n_status;
        {
            json_object *j;

            memset(raw, 0, sizeof(raw));
            j = call(&ctx, "{\"m\":\"record.status\",\"p\":{\"id\":99}}", &rc, raw, sizeof(raw));
            chk(rc == IV_OK && j != NULL && joerr(j) == IV_EINVAL,
                "c27e: out-of-range id stays EINVAL");
            if (j != NULL)
                json_object_put(j);
        }
        chk(g_n_status == n0, "c27e: ops NOT called for an out-of-range id (f6)");
        g_status_rc = IV_OK;
    }

    /* ---------------- c18 状态：未开启时的外形（无钥匙、无令牌） ---------------- */
    j = call(&ctx, "{\"m\":\"tunnel.status\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL, "c18: status answered");
    chk(jobool(j, "ok", 0) == 1, "c18: ok true");
    {
        json_object *d = jodata(j);
        json_object *v = NULL;

        chk(d != NULL, "c18: has data");
        if (d != NULL && json_object_object_get_ex(d, "on", &v))
            chk(json_object_get_boolean(v) == 0, "c18: off by default");
        else
            chk(0, "c18: on field missing");
        if (d != NULL && json_object_object_get_ex(d, "enabled", &v))
            chk(json_object_get_boolean(v) == 1,
                "c18: capability enabled (identity + server present)");
        else
            chk(0, "c18: enabled field missing");
    }
    chk(strstr(raw, "secret") == NULL && strstr(raw, "test-token") == NULL,
        "c18: neither secretKey nor token ever appears in a response");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c19 有效期：越界当场拒（BIRD 没有 TTL，这是本工程的护栏） ---- */
    j = call(&ctx, "{\"m\":\"tunnel.enable\",\"p\":{\"ttl\":86401}}", &rc, raw,
             sizeof(raw));
    chk(rc == IV_OK && j != NULL && joerr(j) == IV_ERANGE, "c19a: ttl over max rejected");
    if (j != NULL)
        json_object_put(j);
    j = call(&ctx, "{\"m\":\"tunnel.enable\",\"p\":{\"ttl\":-5}}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL && joerr(j) == IV_ERANGE, "c19b: negative ttl rejected");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c20 不带 ttl ⇒ 不过期（＝BIRD 的 debug.mode 开关） ---------- */
    j = call(&ctx, "{\"m\":\"tunnel.enable\",\"p\":{}}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c20a: enable without ttl accepted");
    {
        json_object *d = (j != NULL) ? jodata(j) : NULL;
        json_object *v = NULL;
        json_object *sv = NULL;

        if (d != NULL && json_object_object_get_ex(d, "on", &v))
            chk(json_object_get_boolean(v) == 1, "c20a: reports on");
        else
            chk(0, "c20a: on field missing");
        if (d != NULL && json_object_object_get_ex(d, "ttl_s", &v))
            chk(json_object_get_int(v) == 0, "c20a: ttl_s 0 means never expires");
        else
            chk(0, "c20a: ttl_s field missing");
        /* 通道只有一条代理：回包里的服务集合固定 ["ssh"] */
        if (d != NULL && json_object_object_get_ex(d, "svcs", &sv) && sv != NULL)
            chk(json_object_array_length(sv) == 1, "c20a: exactly one service (ssh)");
        else
            chk(0, "c20a: svcs array missing");
    }
    if (j != NULL)
        json_object_put(j);
    chk(access(g_tflag, F_OK) == 0, "c20b: flag file created");
    chk(access(g_tconf, F_OK) == 0, "c20b: frpc.toml created");
    {
        /* 配置内容必须与 BIRD 逐字同构：一条 stcp、转发 22、钥匙 16 位 */
        char   txt[1024];
        long   n;
        FILE  *f = fopen(g_tconf, "rb");

        chk(f != NULL, "c20b: conf readable");
        if (f != NULL) {
            n = (long)fread(txt, 1u, sizeof txt - 1u, f);
            if (n < 0) n = 0;
            txt[n] = '\0';
            (void)fclose(f);
            chk(strstr(txt, "type = \"stcp\"") != NULL, "c20b: stcp proxy");
            chk(strstr(txt, "localPort = 22") != NULL, "c20b: forwards sshd 22");
            chk(strstr(txt, "name = \"TESTSN01\"") != NULL, "c20b: proxy name == SN");
        }
    }
    j = call(&ctx, "{\"m\":\"tunnel.status\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL && strstr(raw, "\"on\":true") != NULL, "c20c: status on");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c21 重复开启 ⇒ ESTATE ---------------- */
    j = call(&ctx, "{\"m\":\"tunnel.enable\",\"p\":{}}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL && joerr(j) == IV_ESTATE, "c21: double enable is ESTATE");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c22 关闭清标记；再开可带 ttl ---------------- */
    j = call(&ctx, "{\"m\":\"tunnel.disable\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c22a: disable while on");
    if (j != NULL)
        json_object_put(j);
    chk(access(g_tflag, F_OK) != 0, "c22a: flag removed (monitor stops frpc)");
    j = call(&ctx, "{\"m\":\"tunnel.enable\",\"p\":{\"ttl\":60}}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c22b: enable with ttl accepted");
    if (j != NULL) {
        json_object *d = jodata(j);
        json_object *v = NULL;

        if (d != NULL && json_object_object_get_ex(d, "remain_s", &v))
            chk(json_object_get_int(v) > 0, "c22b: remain_s > 0 with ttl");
        else
            chk(0, "c22b: remain_s missing");
        json_object_put(j);
    }
    j = call(&ctx, "{\"m\":\"tunnel.disable\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c22c: closed again");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c23 关闭后留档 ---------------- */
    j = call(&ctx, "{\"m\":\"tunnel.status\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL && strstr(raw, "\"stop\":\"local\"") != NULL,
        "c23: last session records stop reason local");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c24 老客户端仍带 svcs：忽略但不得报错 ---------------- */
    j = call(&ctx, "{\"m\":\"tunnel.enable\",\"p\":{\"svcs\":[\"web\",\"camera:0\"]}}",
             &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c24: legacy svcs ignored, not rejected");
    if (j != NULL) {
        json_object *d = jodata(j);
        json_object *sv = NULL;

        if (d != NULL && json_object_object_get_ex(d, "svcs", &sv) && sv != NULL)
            chk(json_object_array_length(sv) == 1, "c24: still one service (ssh)");
        else
            chk(0, "c24: svcs array missing");
        json_object_put(j);
    }
    j = call(&ctx, "{\"m\":\"tunnel.disable\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && jobool(j, "ok", 0) == 1, "c24: closed again");
    if (j != NULL)
        json_object_put(j);

    /* ---------------- c25 恢复出厂（sys.factory_reset，2026-10-22） ----------------
     * 放在最后：它会把整份配置打回默认，前面那些"改过配置"的用例不能排在它后面。 */
    {
        int found = 0, i;

        for (i = 0; i < iv_ctl_method_count(); i++) {
            if (strcmp(iv_ctl_method_name(i), "sys.factory_reset") == 0)
                found = 1;
        }
        chk(found == 1, "c25c: sys.factory_reset is registered");
    }
    /* 二次确认门：与 system.reboot 同一道 */
    j = call(&ctx, "{\"m\":\"sys.factory_reset\"}", &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL && joerr(j) == IV_EPROTO,
        "c25d: confirm required");
    if (j != NULL)
        json_object_put(j);
    j = call(&ctx, "{\"m\":\"sys.factory_reset\",\"p\":{\"confirm\":true,\"erase\":[\"nope\"]}}",
             &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL && joerr(j) == IV_EINVAL,
        "c25e: unknown erase name rejected");
    if (j != NULL)
        json_object_put(j);
    /* 没接根 ⇒ 擦除位一律"能调但没接" */
    j = call(&ctx, "{\"m\":\"sys.factory_reset\",\"p\":{\"confirm\":true,\"erase\":\"media\"}}",
             &rc, raw, sizeof(raw));
    chk(rc == IV_OK && j != NULL && joerr(j) == IV_ENOTSUP,
        "c25f: erase media without factory_media_root -> ENOTSUP");
    if (j != NULL)
        json_object_put(j);

    /* 布景：先把配置改脏，再确认"恢复出厂真的动了东西、且落了盘" */
    chk(iv_config_set_str(cfg, "sys.device.name", "SITE-X") == IV_OK, "c25g: dirty a key");
    chk(iv_config_save(cfg, NULL, 0u) == IV_OK, "c25h: save dirty state");
    {
        uint64_t v0 = iv_config_version(cfg);

        j = call(&ctx, "{\"m\":\"sys.factory_reset\",\"p\":{\"confirm\":true}}", &rc, raw,
                 sizeof(raw));
        chk(rc == IV_OK && j != NULL && jobool(j, "ok", 0) == 1, "c25i: reset ok");
        if (j != NULL) {
            json_object *d = jodata(j);

            chk(d != NULL && jobool(d, "password_reset_required", 0) == 1,
                "c25j: web is told to reset the password");
            chk(d != NULL && jint(d, "cfg_changed", 0) > 0, "c25k: changed keys reported");
            chk(d != NULL && (uint64_t)jint(d, "version", 0) > v0,
                "c25l: config version advanced (saved)");
            json_object_put(j);
        } else {
            chk(0, "c25i: no response object");
        }
        {
            const char *s = NULL;
            int64_t     audit1 = 0;

            chk(iv_config_get_str(cfg, "sys.device.name", &s) == IV_OK && s[0] == '\0',
                "c25m: device name back to default");
            chk(iv_store_rows(tstore, "audit", &audit1) == IV_OK && audit1 > 0,
                "c25n: the reset left an audit row");
        }
    }
    /* 再来一次：已经出厂态 ⇒ 不报错、也不重复改 */
    j = call(&ctx, "{\"m\":\"sys.factory_reset\",\"p\":{\"confirm\":true}}", &rc, raw,
             sizeof(raw));
    chk(rc == IV_OK && j != NULL && jobool(j, "ok", 0) == 1, "c25o: second reset ok");
    if (j != NULL) {
        json_object *d = jodata(j);

        chk(d != NULL && jint(d, "cfg_changed", 9) == 0, "c25p: nothing left to change");
        json_object_put(j);
    }

    /* ---------------- 收尾（规则 6） ---------------- */
    iv_config_close(cfg);
    {
        char p[sizeof(g_tdir) + 32];

        (void)snprintf(p, sizeof(p), "%s/ivsbox.json", g_dir);
        (void)unlink(p);
        (void)snprintf(p, sizeof(p), "%s/ivsbox.json.tmp", g_dir);
        (void)unlink(p);
        (void)rmdir(g_dir);
        /* c2x 通道落点（write_file_atomic 会先写 .tmp 再 rename，一并清） */
        (void)snprintf(p, sizeof(p), "%s/frpc.toml", g_tdir);
        (void)unlink(p);
        (void)snprintf(p, sizeof(p), "%s/frpc.toml.tmp", g_tdir);
        (void)unlink(p);
        (void)snprintf(p, sizeof(p), "%s/tunnel.enabled", g_tdir);
        (void)unlink(p);
        (void)snprintf(p, sizeof(p), "%s/tunnel.enabled.tmp", g_tdir);
        (void)unlink(p);
        (void)rmdir(g_tdir);
        iv_store_close(tstore);
        iv_db_close(tdb);
        {
            char p2[128];

            (void)snprintf(p2, sizeof(p2), "%s/control.db", g_ddir);
            (void)unlink(p2);
            (void)rmdir(g_ddir);
        }
    }

    if (g_fail != 0) {
        fprintf(stderr, "test_ctl FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_ctl passed (protocol shape, silent unknown, version gate, apply rollback, "
           "net.link carrier/gateway, snapshot content, record status known/unknown)\n");
    return 0;
}
