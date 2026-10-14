/*
 * IVSBox 本地通道方法路由 —— 实现（M5-S5.1 / S5.2 服务端）
 *
 * 接口契约、方法表、协议格式见 include/ivsbox/iv_ctl.h 的文件头，这里只补
 * 实现层面的取舍：
 *
 * ============================ 为什么用 json-c 而不是手写拼串 ============================
 * 响应里塞的是**用户可控的字符串**（摄像机型号、OID、设备串号都可能来自现场），
 * 手写 `snprintf("\"%s\"")` 一旦遇到引号/反斜杠/控制字符就会拼出坏 JSON，
 * 而坏 JSON 在前端表现为"整个页面加载失败"，排查成本极高。json-c 的转义是
 * 经过验证的，用它生成比手写正确得多。
 * 依赖纪律上这是合规的：架构 §15.5 只禁止 **libivcore / libivhal** 引 json-c，
 * 本文件在 libivmodules，与 src/modules/config/ 同级。
 *
 * ============================ 请求解析：不用 json_tokener_parse ============================
 * 通道递上来的载荷**不是 NUL 结尾的 C 串**（`iv_chan_recv` 只保证"头 + 长度"），
 * 直接喂 `json_tokener_parse()` 会读越界。故一律用 `json_tokener_parse_ex()`
 * 带显式长度解析，并检查 `json_tokener_get_error()`。
 *
 * ============================ config.apply 的两条硬口径 ============================
 *   1. **只接受默认值表里已有的键**。文件里的未知扩展键（向前兼容保留的）不许
 *      经 Web 写入 —— 它们没有类型与范围约束，写进去等于绕过校验。
 *   2. **版本号不符即整条拒绝**（`ver != iv_config_version()`）。这就是 S5.4 的
 *      "带版本号防覆盖更新（ETag 语义）"：两个页面同时打开、先后提交，后提交的
 *      那个带的是旧版本号，直接 `IV_EBUSY`，不会把先提交的改动冲掉。
 *   提交失败时**回滚内存**（用提交前抓的原值恢复），保证"失败保留旧配置"，
 *   而不是留下一半新一半旧的中间态。
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "ivsbox/iv_camdet.h"
#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_ctl.h"
#include "ivsbox/iv_media.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_status.h"
#include "ivsbox/iv_upd.h"
#include "ivsbox/iv_version.h"

/* ============================ 方法表 ============================ */

static const char *const k_methods[] = {
    "system.snapshot",
    "sys.overview",
    "config.get",
    "config.class",
    "config.all",
    "config.defaults",
    "config.apply",
    "device.list",
    "snmp.result",
    "snmp.run",
    "camera.search",
    "system.time",
    "system.reboot",
    "snapshot.take",
    "record.start",
    "record.stop",
    "record.status",
    "ota.status",
    "ota.request"
};

int iv_ctl_method_count(void)
{
    return (int)(sizeof(k_methods) / sizeof(k_methods[0]));
}

const char *iv_ctl_method_name(int i)
{
    if (i < 0 || i >= iv_ctl_method_count())
        return NULL;
    return k_methods[i];
}

/* ============================ 响应构造 ============================ */

/* 把 json 对象序列化进 out（顺带写一个 '\0'）。root 的所有权被本函数消费。 */
static int rsp_finish(json_object *root, char *out, size_t cap, size_t *out_len)
{
    const char *s;
    size_t      n;

    if (root == NULL)
        return IV_ENOMEM;
    s = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    n = (s != NULL) ? strlen(s) : 0u;
    if (s == NULL || n + 1u > cap) {
        json_object_put(root);
        return IV_ERANGE;
    }
    memcpy(out, s, n + 1u);
    *out_len = n;
    json_object_put(root);
    return IV_OK;
}

/* 成功响应：{"ok":true,"d":<data>}。data 的所有权被本函数消费。 */
static int rsp_data(json_object *data, char *out, size_t cap, size_t *out_len)
{
    json_object *root = json_object_new_object();

    if (root == NULL) {
        if (data != NULL)
            json_object_put(data);
        return IV_ENOMEM;
    }
    json_object_object_add(root, "ok", json_object_new_boolean(1));
    if (data != NULL)
        json_object_object_add(root, "d", data);
    return rsp_finish(root, out, cap, out_len);
}

/* 失败响应：{"ok":false,"e":<code>,"msg":"..."}。 */
static int rsp_err(int code, const char *msg, char *out, size_t cap, size_t *out_len)
{
    json_object *root = json_object_new_object();

    if (root == NULL)
        return IV_ENOMEM;
    json_object_object_add(root, "ok", json_object_new_boolean(0));
    json_object_object_add(root, "e", json_object_new_int(code));
    json_object_object_add(root, "msg", json_object_new_string(msg != NULL ? msg : ""));
    return rsp_finish(root, out, cap, out_len);
}

/* ============================ 参数工具 ============================ */

/* 取请求的 "p" 对象；不是对象或不存在都返回 NULL。 */
static json_object *param_obj(json_object *req)
{
    json_object *p = NULL;

    if (req == NULL)
        return NULL;
    if (!json_object_object_get_ex(req, "p", &p) || p == NULL)
        return NULL;
    if (!json_object_is_type(p, json_type_object))
        return NULL;
    return p;
}

/* 取字符串参数；缺失/类型不符返回 NULL。 */
static const char *param_str(json_object *p, const char *k)
{
    json_object *v = NULL;

    if (p == NULL || k == NULL)
        return NULL;
    if (!json_object_object_get_ex(p, k, &v) || v == NULL)
        return NULL;
    if (!json_object_is_type(v, json_type_string))
        return NULL;
    return json_object_get_string(v);
}

/* 取整数参数。命中 IV_OK；缺失 IV_ENOENT；类型不符 IV_EPROTO。 */
static int param_i64(json_object *p, const char *k, int64_t *out)
{
    json_object *v = NULL;

    if (p == NULL || k == NULL || out == NULL)
        return IV_EINVAL;
    if (!json_object_object_get_ex(p, k, &v) || v == NULL)
        return IV_ENOENT;
    if (!json_object_is_type(v, json_type_int))
        return IV_EPROTO;
    *out = (int64_t)json_object_get_int64(v);
    return IV_OK;
}

/* ============================ 类型名 / 值序列化 ============================ */

static const char *cfg_type_name(iv_cfg_type_t t)
{
    switch (t) {
    case IV_CFG_T_STR:  return "str";
    case IV_CFG_T_INT:  return "int";
    case IV_CFG_T_BOOL: return "bool";
    case IV_CFG_T_DBL:  return "dbl";
    default:            return "?";
    }
}

/* 一级分类名（`config.class` 的参数值）↔ 枚举。查不到返回 -1。 */
static int cls_from_name(const char *s)
{
    static const struct { const char *n; int c; } tab[] = {
        { "sys", IV_CFG_CLS_SYS },       { "net", IV_CFG_CLS_NET },
        { "probe", IV_CFG_CLS_PROBE },   { "camera", IV_CFG_CLS_CAMERA },
        { "switch", IV_CFG_CLS_SWITCH }, { "elec", IV_CFG_CLS_ELEC },
        { "netthr", IV_CFG_CLS_NETTHR }, { "sensor", IV_CFG_CLS_SENSOR }
    };
    size_t i;

    if (s == NULL)
        return -1;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (strcmp(s, tab[i].n) == 0)
            return tab[i].c;
    }
    return -1;
}

static const char *cls_name_of(int c)
{
    switch (c) {
    case IV_CFG_CLS_SYS:    return "sys";
    case IV_CFG_CLS_NET:    return "net";
    case IV_CFG_CLS_PROBE:  return "probe";
    case IV_CFG_CLS_CAMERA: return "camera";
    case IV_CFG_CLS_SWITCH: return "switch";
    case IV_CFG_CLS_ELEC:   return "elec";
    case IV_CFG_CLS_NETTHR: return "netthr";
    case IV_CFG_CLS_SENSOR: return "sensor";
    default:                return "?";
    }
}

/* 按类型取一个键的值，转成 json。键不存在/类型不符返回 NULL。 */
static json_object *cfg_value_json(const iv_config_t *cfg, const char *key, iv_cfg_type_t t)
{
    if (cfg == NULL || key == NULL)
        return NULL;
    switch (t) {
    case IV_CFG_T_STR: {
        const char *s = NULL;

        if (iv_config_get_str(cfg, key, &s) == IV_OK && s != NULL)
            return json_object_new_string(s);
        break;
    }
    case IV_CFG_T_INT: {
        int64_t v = 0;

        if (iv_config_get_int(cfg, key, &v) == IV_OK)
            return json_object_new_int64(v);
        break;
    }
    case IV_CFG_T_BOOL: {
        int b = 0;

        if (iv_config_get_bool(cfg, key, &b) == IV_OK)
            return json_object_new_boolean(b);
        break;
    }
    case IV_CFG_T_DBL: {
        double d = 0.0;

        if (iv_config_get_dbl(cfg, key, &d) == IV_OK)
            return json_object_new_double(d);
        break;
    }
    default:
        break;
    }
    return NULL;
}

/* ============================ system.snapshot ============================ */

/* **保持 S10 的老格式**（集成用例逐字断言了 method/status/version 三处）。 */
static int m_snapshot(char *out, size_t cap, size_t *out_len)
{
    int n = snprintf(out, cap,
                     "{\"method\":\"system.snapshot\",\"version\":\"%s\",\"status\":\"ok\"}",
                     iv_version_string());

    if (n < 0 || (size_t)n >= cap)
        return IV_ERANGE;
    *out_len = (size_t)n;
    return IV_OK;
}

/* ============================ sys.overview ============================ */

/* 值缺失就写 JSON null（页面显示"—"）。**不给 0** —— 0 在这些量上是合法读数，
 * 用它冒充"尚未采集"会误导现场。 */
static void add_or_null(json_object *o, const char *k, int valid, json_object *v)
{
    if (valid) {
        json_object_object_add(o, k, v);
    } else {
        if (v != NULL)
            json_object_put(v);
        /* 0.13 表示 JSON null 的惯用法：add(..., NULL) */
        json_object_object_add(o, k, NULL);
    }
}

static void add_status_view(json_object *d, const iv_report_t *rp)
{
    json_object       *v = json_object_new_object();
    const iv_status_t *st;

    if (v == NULL)
        return;
    if (rp == NULL) {
        json_object_object_add(d, "status", v);
        return;
    }
    st = &rp->st;
    json_object_object_add(d, "upstream", json_object_new_boolean(rp->opened ? 1 : 0));
    add_or_null(v, "V", st->valid.V, json_object_new_double((double)st->data.V));
    add_or_null(v, "A", st->valid.A, json_object_new_double((double)st->data.A));
    add_or_null(v, "T", st->valid.T, json_object_new_double((double)st->data.T));
    add_or_null(v, "H", st->valid.H, json_object_new_double((double)st->data.H));
    add_or_null(v, "DS", st->valid.DS, json_object_new_int(st->data.DS));
    add_or_null(v, "RELAY", st->valid.RELAY, json_object_new_int(st->valid.RELAY ? st->data.RELAY[0] : 0));
    json_object_object_add(d, "status", v);

    {
        json_object *pwr = json_object_new_object();

        if (pwr != NULL) {
            json_object_object_add(pwr, "APOWER", json_object_new_string(st->data.APOWER));
            json_object_object_add(pwr, "AKW", json_object_new_string(st->data.AKW));
            json_object_object_add(d, "power", pwr);
        }
    }
}

/* ---------------------------------------------------------------------------
 * S6.5 存储视图（架构 §9.1：总览页列"SD 卡"，媒体页列"存储水位"）
 *
 * **它是 SD 异常唯一的对外出口**：在本次改动之前，`iv_storage` 的故障/告警
 * 只落本地日志（`iv_storage_alarm()` 除了单测没有任何生产调用者），上层
 * 完全看不到 ⇒ §16.2 场景 6 要求的"产生明确告警"无从体现。这里把档位、
 * 故障、水位与统计一并交出去，Web 据此在总览页亮灯。
 *
 * **恒返回一个对象**（哪怕没启用）：`enabled=0` ＝ 本机未启用存储策略。
 * 未启用时**不再给** level/used_pct 等零值字段 —— 那些零值是"没巡检"，
 * 不是"巡检过且正常"，混在一起会让人把"没启用"读成"磁盘一切正常"。
 *
 * 平台侧 `ERR` 故障码上报**未接**：SD 故障码在 `iv_err.h` 里没有槽位
 * （类型段 `0x4~0xF` 归属未冻结，须与单片机侧协商）⇒ 如实登记为卡点。
 * ------------------------------------------------------------------------- */
static json_object *build_storage(const iv_ctl_ctx_t *ctx)
{
    json_object       *s  = json_object_new_object();
    iv_storage_t      *st = (ctx->agent != NULL) ? iv_agent_storage(ctx->agent) : NULL;
    iv_storage_stats_t ss;

    if (s == NULL)
        return NULL;

    json_object_object_add(s, "enabled", json_object_new_boolean(iv_storage_enabled(st)));
    if (!iv_storage_enabled(st))
        return s;

    memset(&ss, 0, sizeof ss);
    iv_storage_stats(st, &ss);

    json_object_object_add(s, "level", json_object_new_int((int)iv_storage_level(st)));
    json_object_object_add(s, "level_name",
                           json_object_new_string(iv_storage_level_name(iv_storage_level(st))));
    json_object_object_add(s, "used_pct", json_object_new_int(iv_storage_used_pct(st)));
    json_object_object_add(s, "fault", json_object_new_int(iv_storage_fault(st)));
    json_object_object_add(s, "alarm", json_object_new_int(iv_storage_alarm(st)));
    json_object_object_add(s, "allow_new", json_object_new_int(iv_storage_allow_new(st)));
    json_object_object_add(s, "stop_rec", json_object_new_int(iv_storage_stop_rec(st)));
    json_object_object_add(s, "rounds", json_object_new_int64((int64_t)ss.rounds));
    json_object_object_add(s, "deletes", json_object_new_int64((int64_t)ss.deletes));
    json_object_object_add(s, "del_bytes", json_object_new_int64((int64_t)ss.del_bytes));
    json_object_object_add(s, "faults", json_object_new_int64((int64_t)ss.faults));
    return s;
}

static json_object *build_overview(const iv_ctl_ctx_t *ctx)
{
    json_object *d   = json_object_new_object();
    json_object *net = json_object_new_object();
    json_object *cam = json_object_new_object();

    if (d == NULL || net == NULL || cam == NULL) {
        if (d != NULL)
            json_object_put(d);
        if (net != NULL)
            json_object_put(net);
        if (cam != NULL)
            json_object_put(cam);
        return NULL;
    }

    json_object_object_add(d, "version", json_object_new_string(iv_version_string()));
    json_object_object_add(d, "uptime_ms",
                           json_object_new_int64((int64_t)iv_clock_monotonic_ms()));

    if (ctx->agent != NULL) {
        iv_netmgr_state_t st = iv_agent_netmgr_state(ctx->agent);
        iv_wan_t          aw = iv_agent_active_wan(ctx->agent);
        json_object      *mn = json_object_new_array();
        const iv_camdet_t *cd = iv_agent_camdet(ctx->agent);
        json_object      *cns = json_object_new_array();
        int               w;
        int               c;

        json_object_object_add(net, "state",
                               json_object_new_string(iv_netmgr_state_name(st)));
        json_object_object_add(net, "state_code", json_object_new_int((int)st));
        json_object_object_add(net, "active_wan",
                               json_object_new_string(aw == IV_WAN_WIRED ? "wired"
                                                                          : "wireless"));
        /* MN：1 正常 / 2 断开（迟滞判定后）；0 = 尚未判定 */
        for (w = 0; w < IV_REPORT_MN_N; w++)
            json_object_array_add(mn, json_object_new_int((int)iv_agent_mn(ctx->agent, w)));
        json_object_object_add(net, "mn", mn);

        /* 摄像机：CNS 六路（0 无IP/1 正常/2 故障/4 延时/5 丢包）＋ 当前检测方式 */
        if (cd != NULL) {
            for (c = 0; c < IV_CAMDET_CH_N; c++)
                json_object_array_add(cns,
                                      json_object_new_int((int)iv_camdet_status(cd, c)));
            json_object_object_add(cam, "mode", json_object_new_int(iv_camdet_mode(cd)));
        }
        json_object_object_add(cam, "cns", cns);
        json_object_object_add(d, "cameras", cam);

        json_object_object_add(d, "devices",
                               json_object_new_int(iv_agent_device_count(ctx->agent)));
        json_object_object_add(d, "door_events",
                               json_object_new_int64((int64_t)iv_agent_door_events(ctx->agent)));

        {
            json_object *snmp = json_object_new_object();

            if (snmp != NULL) {
                json_object_object_add(snmp, "cam",
                    json_object_new_int(iv_agent_snmp_state(ctx->agent, IV_AGENT_SNMP_CAM)));
                json_object_object_add(snmp, "sw",
                    json_object_new_int(iv_agent_snmp_state(ctx->agent, IV_AGENT_SNMP_SW)));
                json_object_object_add(d, "snmp", snmp);
            }
        }
        add_status_view(d, iv_agent_report(ctx->agent));
    } else {
        json_object_object_add(net, "state", json_object_new_string("n/a"));
        json_object_object_add(cam, "cns", json_object_new_array());
        json_object_object_add(d, "cameras", cam);
        json_object_object_add(d, "devices", json_object_new_int(0));
        json_object_object_add(d, "door_events", json_object_new_int64(0));
    }
    json_object_object_add(d, "net", net);

    if (ctx->cfg != NULL)
        json_object_object_add(d, "config_version",
                               json_object_new_int64((int64_t)iv_config_version(ctx->cfg)));

    /* S6.5：SD 卡 / 存储水位（架构 §9.1 总览页）。恒给一个对象，见 build_storage()。 */
    {
        json_object *sv = build_storage(ctx);

        if (sv != NULL)
            json_object_object_add(d, "storage", sv);
    }
    return d;
}

static int m_overview(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    if (ctx->cfg == NULL && ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "no data source", out, cap, out_len);
    return rsp_data(build_overview(ctx), out, cap, out_len);
}

/* ============================ config.get / class / all / defaults ============================ */

static int m_config_get(const iv_ctl_ctx_t *ctx, json_object *p,
                        char *out, size_t cap, size_t *out_len)
{
    const char     *key = param_str(p, "key");
    iv_cfg_type_t   t;
    json_object    *d;
    json_object    *v;
    iv_cfg_origin_t org;

    if (ctx->cfg == NULL)
        return rsp_err(IV_ESTATE, "config unavailable", out, cap, out_len);
    if (key == NULL)
        return rsp_err(IV_EINVAL, "missing key", out, cap, out_len);
    if (iv_config_get_type(ctx->cfg, key, &t) != IV_OK)
        return rsp_err(IV_ENOENT, "no such key", out, cap, out_len);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "key", json_object_new_string(key));
    json_object_object_add(d, "type", json_object_new_string(cfg_type_name(t)));
    v = cfg_value_json(ctx->cfg, key, t);
    json_object_object_add(d, "value", v);
    if (iv_config_origin(ctx->cfg, key, &org) == IV_OK)
        json_object_object_add(d, "origin", json_object_new_string(
            org == IV_CFG_FROM_FILE ? "file" : "default"));
    return rsp_data(d, out, cap, out_len);
}

/* cls < 0 表示"全部"。 */
static int m_config_list(const iv_ctl_ctx_t *ctx, int cls,
                         char *out, size_t cap, size_t *out_len)
{
    json_object  *d;
    json_object  *items;
    const char   *k;
    iv_cfg_type_t t;
    unsigned      i;

    if (ctx->cfg == NULL)
        return rsp_err(IV_ESTATE, "config unavailable", out, cap, out_len);

    d = json_object_new_object();
    items = json_object_new_object();
    if (d == NULL || items == NULL) {
        if (d != NULL)
            json_object_put(d);
        if (items != NULL)
            json_object_put(items);
        return IV_ENOMEM;
    }

    for (i = 0; iv_config_item_at(ctx->cfg, i, &k, &t) == IV_OK; i++) {
        if (cls >= 0 && iv_config_class_of(k) != cls)
            continue;
        json_object_object_add(items, k, cfg_value_json(ctx->cfg, k, t));
    }
    json_object_object_add(d, "cls",
                           json_object_new_string(cls >= 0 ? cls_name_of(cls) : "all"));
    json_object_object_add(d, "version",
                           json_object_new_int64((int64_t)iv_config_version(ctx->cfg)));
    json_object_object_add(d, "items", items);
    return rsp_data(d, out, cap, out_len);
}

static int m_config_defaults(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    json_object  *d;
    json_object  *items;
    const char   *k;
    iv_cfg_type_t t;
    unsigned      i;

    if (ctx->cfg == NULL)
        return rsp_err(IV_ESTATE, "config unavailable", out, cap, out_len);

    d = json_object_new_object();
    items = json_object_new_object();
    if (d == NULL || items == NULL) {
        if (d != NULL)
            json_object_put(d);
        if (items != NULL)
            json_object_put(items);
        return IV_ENOMEM;
    }

    /* 内存里的键已由 open() 补齐过默认，故枚举内存键即可覆盖全部已知键；
     * 默认值单独从默认值表取（内存里的当前值可能是文件给的）。 */
    for (i = 0; iv_config_item_at(ctx->cfg, i, &k, &t) == IV_OK; i++) {
        iv_cfg_type_t dt = IV_CFG_T_STR;
        char          txt[IV_CFG_STR_MAX];
        json_object  *e;

        if (iv_config_default_get(IV_CFG_NAME, k, &dt, txt, sizeof(txt)) != IV_OK)
            continue;
        e = json_object_new_object();
        if (e == NULL)
            continue;
        json_object_object_add(e, "t", json_object_new_string(cfg_type_name(dt)));
        switch (dt) {
        case IV_CFG_T_INT:
            json_object_object_add(e, "v", json_object_new_int64(strtoll(txt, NULL, 10)));
            break;
        case IV_CFG_T_DBL:
            json_object_object_add(e, "v", json_object_new_double(strtod(txt, NULL)));
            break;
        case IV_CFG_T_BOOL:
            json_object_object_add(e, "v",
                json_object_new_boolean((strcmp(txt, "true") == 0 || strcmp(txt, "1") == 0) ? 1 : 0));
            break;
        default:
            json_object_object_add(e, "v", json_object_new_string(txt));
            break;
        }
        json_object_object_add(items, k, e);
    }
    json_object_object_add(d, "items", items);
    return rsp_data(d, out, cap, out_len);
}

/* ============================ config.apply ============================ */

/* 备份项：原值按目标类型存，供失败回滚。
 * **静态数组而非 malloc**：本模块单线程（见头文件"依赖与并发"），静态区够用，
 * 且省掉一条 malloc 失败路径。 */
typedef struct {
    char          key[IV_CFG_KEY_MAX];
    iv_cfg_type_t type;
    char          s[IV_CFG_STR_MAX];
    int64_t       i;
    int           b;
    double        dv;
} apply_bak_t;

static apply_bak_t s_bak[IV_CTL_APPLY_ITEMS_MAX];

/* 校验一个 JSON 值是否匹配目标配置类型。 */
static int json_matches_type(json_object *v, iv_cfg_type_t want)
{
    switch (want) {
    case IV_CFG_T_STR:
        return json_object_is_type(v, json_type_string);
    case IV_CFG_T_INT:
        return json_object_is_type(v, json_type_int);
    case IV_CFG_T_BOOL:
        return json_object_is_type(v, json_type_boolean);
    case IV_CFG_T_DBL:
        return json_object_is_type(v, json_type_double) ||
               json_object_is_type(v, json_type_int);
    default:
        return 0;
    }
}

/* 入参刻意非 const：本函数会经 iv_config_set_* 写回内存配置（回滚亦同）。
 * 先前写成 `const iv_config_t *` 被 -Werror=discarded-qualifiers 挡下 —— 那次
 * 编译失败正好证明"改配置"这条路径没有被 const 悄悄绕开。 */
static void apply_set(iv_config_t *cfg, const char *key, iv_cfg_type_t t,
                      json_object *v)
{
    switch (t) {
    case IV_CFG_T_STR:
        (void)iv_config_set_str(cfg, key, json_object_get_string(v));
        break;
    case IV_CFG_T_INT:
        (void)iv_config_set_int(cfg, key, (int64_t)json_object_get_int64(v));
        break;
    case IV_CFG_T_BOOL:
        (void)iv_config_set_bool(cfg, key, json_object_get_boolean(v));
        break;
    case IV_CFG_T_DBL:
        (void)iv_config_set_dbl(cfg, key, json_object_get_double(v));
        break;
    default:
        break;
    }
}

static void apply_restore(iv_config_t *cfg, const apply_bak_t *b)
{
    switch (b->type) {
    case IV_CFG_T_STR:
        (void)iv_config_set_str(cfg, b->key, b->s);
        break;
    case IV_CFG_T_INT:
        (void)iv_config_set_int(cfg, b->key, b->i);
        break;
    case IV_CFG_T_BOOL:
        (void)iv_config_set_bool(cfg, b->key, b->b);
        break;
    case IV_CFG_T_DBL:
        (void)iv_config_set_dbl(cfg, b->key, b->dv);
        break;
    default:
        break;
    }
}

static int m_config_apply(const iv_ctl_ctx_t *ctx, json_object *p,
                          char *out, size_t cap, size_t *out_len)
{
    int64_t      ver = 0;
    json_object *items = NULL;
    int          rc;
    int          n = 0;
    char         detail[IV_CFG_DETAIL_MAX];
    uint64_t     newver;

    if (ctx->cfg == NULL)
        return rsp_err(IV_ESTATE, "config unavailable", out, cap, out_len);
    if (param_i64(p, "ver", &ver) != IV_OK)
        return rsp_err(IV_EINVAL, "missing ver", out, cap, out_len);
    if (p == NULL || !json_object_object_get_ex(p, "items", &items) || items == NULL ||
        !json_object_is_type(items, json_type_object))
        return rsp_err(IV_EINVAL, "missing items", out, cap, out_len);
    if ((uint64_t)json_object_object_length(items) > (uint64_t)IV_CTL_APPLY_ITEMS_MAX)
        return rsp_err(IV_ERANGE, "too many items", out, cap, out_len);

    /* ① 版本门（S5.4 并发双写拦截） */
    if ((uint64_t)ver != iv_config_version(ctx->cfg))
        return rsp_err(IV_EBUSY, "version conflict", out, cap, out_len);

    /* ② 预检 + 备份原值：全部通过才动手，"一个键不合格就整条不落" */
    {
        json_object_object_foreach(items, k, v) {
            iv_cfg_type_t want = IV_CFG_T_STR;
            char          dtxt[IV_CFG_STR_MAX];

            if (n >= IV_CTL_APPLY_ITEMS_MAX)
                return rsp_err(IV_ERANGE, "too many items", out, cap, out_len);
            /* 只允许默认值表里已有的键（见文件头"两条硬口径"第 1 条） */
            if (iv_config_default_get(IV_CFG_NAME, k, &want, dtxt, sizeof(dtxt)) != IV_OK)
                return rsp_err(IV_ENOENT, "unknown key for web write", out, cap, out_len);
            if (!json_matches_type(v, want))
                return rsp_err(IV_EPROTO, "item type mismatch", out, cap, out_len);

            (void)snprintf(s_bak[n].key, sizeof(s_bak[n].key), "%.*s",
                           (int)sizeof(s_bak[n].key) - 1, k);
            s_bak[n].type = want;
            switch (want) {
            case IV_CFG_T_STR: {
                const char *os = NULL;

                s_bak[n].s[0] = '\0';
                if (iv_config_get_str(ctx->cfg, k, &os) == IV_OK && os != NULL)
                    (void)snprintf(s_bak[n].s, sizeof(s_bak[n].s), "%.*s",
                                   (int)sizeof(s_bak[n].s) - 1, os);
                break;
            }
            case IV_CFG_T_INT:
                s_bak[n].i = 0;
                (void)iv_config_get_int(ctx->cfg, k, &s_bak[n].i);
                break;
            case IV_CFG_T_BOOL:
                s_bak[n].b = 0;
                (void)iv_config_get_bool(ctx->cfg, k, &s_bak[n].b);
                break;
            case IV_CFG_T_DBL:
                s_bak[n].dv = 0.0;
                (void)iv_config_get_dbl(ctx->cfg, k, &s_bak[n].dv);
                break;
            default:
                break;
            }
            n++;
        }
    }

    /* ③ 写入内存 */
    json_object_object_foreach(items, k, v) {
        iv_cfg_type_t want = IV_CFG_T_STR;
        char          dtxt[IV_CFG_STR_MAX];

        (void)iv_config_default_get(IV_CFG_NAME, k, &want, dtxt, sizeof(dtxt));
        apply_set(ctx->cfg, k, want, v);
    }

    /* ④ 落盘；失败回滚内存，保证"失败保留旧配置" */
    detail[0] = '\0';
    rc = iv_config_save(ctx->cfg, detail, sizeof(detail));
    if (rc != IV_OK) {
        int j;

        for (j = 0; j < n; j++)
            apply_restore(ctx->cfg, &s_bak[j]);
        return rsp_err(rc, detail[0] != '\0' ? detail : "save failed", out, cap, out_len);
    }

    newver = iv_config_version(ctx->cfg);
    {
        json_object *d = json_object_new_object();

        if (d == NULL)
            return IV_ENOMEM;
        json_object_object_add(d, "version", json_object_new_int64((int64_t)newver));
        json_object_object_add(d, "applied", json_object_new_int(n));
        return rsp_data(d, out, cap, out_len);
    }
}

/* ============================ device.list / snmp / camera.search ============================ */

static void mac_to_str(const uint8_t mac[6], char out[18])
{
    (void)snprintf(out, 18u, "%02x:%02x:%02x:%02x:%02x:%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int m_device_list(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    json_object *d;
    json_object *arr;
    int          n;
    int          i;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);

    d = json_object_new_object();
    arr = json_object_new_array();
    if (d == NULL || arr == NULL) {
        if (d != NULL)
            json_object_put(d);
        if (arr != NULL)
            json_object_put(arr);
        return IV_ENOMEM;
    }

    n = iv_agent_device_count(ctx->agent);
    for (i = 0; i < n; i++) {
        iv_dev_desc_t desc;
        json_object  *e;
        char          ip[INET_ADDRSTRLEN] = "";
        char          mac[18];

        if (iv_agent_device_get(ctx->agent, i, &desc) != IV_OK)
            continue;
        e = json_object_new_object();
        if (e == NULL)
            continue;
        if (desc.ip_be != 0u) {
            struct in_addr in;

            in.s_addr = desc.ip_be;
            (void)inet_ntop(AF_INET, &in, ip, sizeof(ip));
        }
        mac_to_str(desc.mac, mac);
        json_object_object_add(e, "brand", json_object_new_string(iv_dev_brand_name(desc.brand)));
        json_object_object_add(e, "ip", json_object_new_string(ip));
        json_object_object_add(e, "mac", json_object_new_string(mac));
        json_object_object_add(e, "model", json_object_new_string(desc.model));
        json_object_object_add(e, "serial", json_object_new_string(desc.serial));
        json_object_object_add(e, "port", json_object_new_int((int)desc.port));
        json_object_object_add(e, "found_by", json_object_new_string(iv_dev_phase_name(desc.found_by)));
        json_object_array_add(arr, e);
    }
    json_object_object_add(d, "count", json_object_new_int((int)json_object_array_length(arr)));
    json_object_object_add(d, "items", arr);
    return rsp_data(d, out, cap, out_len);
}

/* which："cam" → 0，"sw" → 1；其余返回 -1。 */
static int snmp_which(const char *s)
{
    if (s == NULL)
        return -1;
    if (strcmp(s, "cam") == 0)
        return IV_AGENT_SNMP_CAM;
    if (strcmp(s, "sw") == 0)
        return IV_AGENT_SNMP_SW;
    return -1;
}

static int m_snmp_result(const iv_ctl_ctx_t *ctx, json_object *p,
                         char *out, size_t cap, size_t *out_len)
{
    int          which = snmp_which(param_str(p, "which"));
    json_object *d;
    json_object *slots;
    int          i;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);
    if (which < 0)
        return rsp_err(IV_EINVAL, "which must be cam or sw", out, cap, out_len);

    d = json_object_new_object();
    slots = json_object_new_array();
    if (d == NULL || slots == NULL) {
        if (d != NULL)
            json_object_put(d);
        if (slots != NULL)
            json_object_put(slots);
        return IV_ENOMEM;
    }

    for (i = 0; i < (int)IV_AGENT_SNMP_SLOT_MAX; i++) {
        const char         *name = NULL;
        const char         *oid = NULL;
        int                 nrows = 0;
        iv_agent_snmp_row_t rows[IV_AGENT_SNMP_ROWS_MAX];
        json_object        *e;

        if (iv_agent_snmp_get(ctx->agent, which, i, &name, &oid, &nrows,
                              rows, (int)IV_AGENT_SNMP_ROWS_MAX) != IV_OK)
            break;
        e = json_object_new_object();
        if (e == NULL)
            continue;
        json_object_object_add(e, "name", json_object_new_string(name != NULL ? name : ""));
        json_object_object_add(e, "oid", json_object_new_string(oid != NULL ? oid : ""));
        json_object_object_add(e, "n_rows", json_object_new_int(nrows));
        {
            json_object *rr = json_object_new_array();
            int          j;

            for (j = 0; j < nrows && j < (int)IV_AGENT_SNMP_ROWS_MAX; j++) {
                json_object *row = json_object_new_object();

                json_object_object_add(row, "type",
                    json_object_new_string(iv_snmp_type_name(rows[j].type)));
                json_object_object_add(row, "num", json_object_new_int64(rows[j].num));
                json_object_object_add(row, "str", json_object_new_string(rows[j].str));
                json_object_array_add(rr, row);
            }
            json_object_object_add(e, "rows", rr);
        }
        json_object_array_add(slots, e);
    }
    json_object_object_add(d, "which",
                           json_object_new_string(which == IV_AGENT_SNMP_CAM ? "cam" : "sw"));
    json_object_object_add(d, "state", json_object_new_int(iv_agent_snmp_state(ctx->agent, which)));
    json_object_object_add(d, "slots", slots);
    return rsp_data(d, out, cap, out_len);
}

static int m_snmp_run(const iv_ctl_ctx_t *ctx, json_object *p,
                      char *out, size_t cap, size_t *out_len)
{
    int          which = snmp_which(param_str(p, "which"));
    int          rc;
    json_object *d;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);
    if (which < 0)
        return rsp_err(IV_EINVAL, "which must be cam or sw", out, cap, out_len);
    rc = iv_agent_snmp_start(ctx->agent, which, iv_clock_monotonic_ms());
    if (rc != IV_OK)
        return rsp_err(rc, "snmp start rejected", out, cap, out_len);
    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "accepted", json_object_new_boolean(1));
    return rsp_data(d, out, cap, out_len);
}

static int m_camera_search(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    int          rc;
    json_object *d;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);
    rc = iv_agent_device_search(ctx->agent, iv_clock_monotonic_ms());
    if (rc != IV_OK)
        return rsp_err(rc, "search rejected", out, cap, out_len);
    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "started", json_object_new_boolean(1));
    return rsp_data(d, out, cap, out_len);
}

/* ============================ system.time / system.reboot ============================ */

static int m_system_time(const iv_ctl_ctx_t *ctx, json_object *p,
                         char *out, size_t cap, size_t *out_len)
{
    int64_t ts = 0;
    int     rc;

    if (param_i64(p, "ts", &ts) != IV_OK)
        return rsp_err(IV_EINVAL, "missing ts", out, cap, out_len);
    if (ctx->do_settime == NULL)
        return rsp_err(IV_ENOTSUP, "settime not wired", out, cap, out_len);
    rc = ctx->do_settime(ts, ctx->settime_arg);
    if (rc != IV_OK)
        return rsp_err(rc, "settime failed", out, cap, out_len);
    return rsp_data(json_object_new_object(), out, cap, out_len);
}

static int m_system_reboot(const iv_ctl_ctx_t *ctx, json_object *p,
                           char *out, size_t cap, size_t *out_len)
{
    int          confirm = 0;
    json_object *v = NULL;
    json_object *d;

    /* 高风险操作二次确认（S5.2）：不带 {"confirm":true} 一律拒绝 */
    if (p != NULL && json_object_object_get_ex(p, "confirm", &v) && v != NULL)
        confirm = json_object_get_boolean(v);
    if (!confirm)
        return rsp_err(IV_EPROTO, "confirm required", out, cap, out_len);
    if (ctx->do_reboot == NULL)
        return rsp_err(IV_ENOTSUP, "reboot not wired", out, cap, out_len);
    (void)ctx->do_reboot(ctx->reboot_arg);
    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "accepted", json_object_new_boolean(1));
    return rsp_data(d, out, cap, out_len);
}

/* ============================ 媒体（M6-S6.1） ============================ */

/* 四个媒体方法共同的门口：未挂 ops 一律 IV_ENOTSUP（同 do_reboot 口径）。 */
static int m_media_capture(const iv_ctl_ctx_t *ctx, json_object *p,
                           char *out, size_t cap, size_t *out_len)
{
    const char  *path = NULL;
    char         out_path[IV_MEDIA_URL_MAX];
    json_object *d;
    int          rc;

    if (ctx->media == NULL || ctx->media->capture == NULL)
        return rsp_err(IV_ENOTSUP, "media not wired", out, cap, out_len);
    if (p != NULL)
        path = param_str(p, "path");
    rc = ctx->media->capture(path, out_path, sizeof(out_path), ctx->media_arg);
    if (rc == IV_EINVAL)
        return rsp_err(IV_EINVAL, "bad path", out, cap, out_len);
    if (rc == IV_EBUSY)
        return rsp_err(IV_EBUSY, "capture queue full", out, cap, out_len);
    if (rc != IV_OK)
        return rsp_err(rc, "capture not accepted", out, cap, out_len);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "queued", json_object_new_boolean(1));
    json_object_object_add(d, "path", json_object_new_string(out_path));
    return rsp_data(d, out, cap, out_len);
}

static int m_media_record_start(const iv_ctl_ctx_t *ctx, json_object *p,
                                char *out, size_t cap, size_t *out_len)
{
    int64_t     id = -1;
    const char *url;
    int         rc;

    if (ctx->media == NULL || ctx->media->record_start == NULL)
        return rsp_err(IV_ENOTSUP, "media not wired", out, cap, out_len);
    if (param_i64(p, "id", &id) != IV_OK || id < 0 || id >= IV_MEDIA_CH_N)
        return rsp_err(IV_EINVAL, "bad id", out, cap, out_len);
    url = param_str(p, "url");
    if (url == NULL || url[0] == '\0')
        return rsp_err(IV_EINVAL, "missing url", out, cap, out_len);
    rc = ctx->media->record_start((int)id, url, ctx->media_arg);
    if (rc != IV_OK)
        return rsp_err(rc, "record start failed", out, cap, out_len);
    {
        json_object *d = json_object_new_object();
        if (d == NULL)
            return IV_ENOMEM;
        json_object_object_add(d, "id", json_object_new_int((int)id));
        json_object_object_add(d, "started", json_object_new_boolean(1));
        return rsp_data(d, out, cap, out_len);
    }
}

static int m_media_record_stop(const iv_ctl_ctx_t *ctx, json_object *p,
                               char *out, size_t cap, size_t *out_len)
{
    int64_t id = -1;
    int     rc;

    if (ctx->media == NULL || ctx->media->record_stop == NULL)
        return rsp_err(IV_ENOTSUP, "media not wired", out, cap, out_len);
    if (param_i64(p, "id", &id) != IV_OK || id < 0 || id >= IV_MEDIA_CH_N)
        return rsp_err(IV_EINVAL, "bad id", out, cap, out_len);
    rc = ctx->media->record_stop((int)id, ctx->media_arg);
    if (rc != IV_OK)
        return rsp_err(rc, "record stop failed", out, cap, out_len);
    {
        json_object *d = json_object_new_object();
        if (d == NULL)
            return IV_ENOMEM;
        json_object_object_add(d, "id", json_object_new_int((int)id));
        json_object_object_add(d, "stopped", json_object_new_boolean(1));
        return rsp_data(d, out, cap, out_len);
    }
}

static int m_media_record_status(const iv_ctl_ctx_t *ctx, json_object *p,
                                 char *out, size_t cap, size_t *out_len)
{
    int64_t id = -1;
    int     rec = 0;
    int     rc;
    json_object *d;

    if (ctx->media == NULL || ctx->media->record_status == NULL)
        return rsp_err(IV_ENOTSUP, "media not wired", out, cap, out_len);
    if (param_i64(p, "id", &id) != IV_OK || id < 0 || id >= IV_MEDIA_CH_N)
        return rsp_err(IV_EINVAL, "bad id", out, cap, out_len);
    rc = ctx->media->record_status((int)id, &rec, ctx->media_arg);
    if (rc != IV_OK)
        return rsp_err(rc, "record status failed", out, cap, out_len);
    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "id", json_object_new_int((int)id));
    json_object_object_add(d, "recording", json_object_new_boolean(rec));
    return rsp_data(d, out, cap, out_len);
}

/* ============================ ota.status / ota.request（M7-S7.2） ============================ */

/* OTA 落点：本模块不持有配置，只在调用瞬间借用（与 cfg/agent 同一口径）。 */
static void ota_cfg_of(const iv_ctl_ctx_t *ctx, iv_upd_cfg_t *c)
{
    iv_upd_cfg_default(c);
    if (ctx->ota_app_root != NULL)
        c->app_root = ctx->ota_app_root;
    if (ctx->ota_data_root != NULL)
        c->data_root = ctx->ota_data_root;
}

/* 升级状态。**只读**：读 pending.json / events.log / current 链接，一个字节都不写。 */
static int m_ota_status(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    iv_upd_cfg_t    c;
    iv_upd_status_t st;
    json_object    *d;
    char            ver[IV_UPD_VER_MAX];
    char            prev[IV_UPD_VER_MAX];

    ota_cfg_of(ctx, &c);
    iv_upd_status(&c, &st);
    iv_ota_ver_format(st.version, ver, sizeof ver);
    iv_ota_ver_format(st.previous, prev, sizeof prev);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "pending", json_object_new_boolean(st.pending != 0 ? 1 : 0));
    json_object_object_add(d, "active", json_object_new_boolean(st.active != 0 ? 1 : 0));
    json_object_object_add(d, "state",
                           json_object_new_string(st.state[0] != '\0' ? st.state : "-"));
    json_object_object_add(d, "version", json_object_new_string(ver));
    json_object_object_add(d, "previous", json_object_new_string(prev));
    json_object_object_add(d, "bootcount", json_object_new_int((int)st.bootcount));
    json_object_object_add(d, "max_boot", json_object_new_int((int)st.max_boot));
    json_object_object_add(d, "rollbacks", json_object_new_int((int)st.rollbacks));
    json_object_object_add(d, "last_event",
                           json_object_new_string(st.last_event[0] != '\0' ? st.last_event : "-"));
    json_object_object_add(d, "last_reason",
                           json_object_new_string(st.last_reason[0] != '\0' ? st.last_reason : "-"));
    json_object_object_add(d, "current",
                           json_object_new_string(st.current_dir[0] != '\0' ? st.current_dir : "-"));
    return rsp_data(d, out, cap, out_len);
}

/* 落一个"请装这个包"的请求。**本方法不做安装**：包必须已经落在 <data_root>/ota/ 下，
 * 真正改盘的安装由 monitor.sh 拉起 ivsbox-updater 完成（理由见 iv_upd.h 里
 * iv_upd_request_write 的注释：危险动作要有自己的进程生命周期）。 */
static int m_ota_request(const iv_ctl_ctx_t *ctx, json_object *p, char *out, size_t cap,
                         size_t *out_len)
{
    iv_upd_cfg_t    c;
    iv_upd_reason_t why = IV_UPD_OK;
    const char     *path = param_str(p, "path");
    json_object    *v = NULL;
    json_object    *d;
    int             restart = 0;
    int             rc;

    if (path == NULL)
        return rsp_err(IV_EINVAL, "missing path", out, cap, out_len);
    if (p != NULL && json_object_object_get_ex(p, "restart", &v) && v != NULL)
        restart = json_object_get_boolean(v) ? 1 : 0;

    ota_cfg_of(ctx, &c);
    rc = iv_upd_request_write(&c, path, restart, &why);
    if (rc != IV_OK)
        return rsp_err(rc, iv_upd_reason_name(why), out, cap, out_len);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "accepted", json_object_new_boolean(1));
    json_object_object_add(d, "restart", json_object_new_boolean(restart != 0 ? 1 : 0));
    json_object_object_add(d, "note", json_object_new_string(
                                          "install 由 monitor.sh 拉起 ivsbox-updater 执行；进度查 ota.status"));
    return rsp_data(d, out, cap, out_len);
}

/* ============================ 入口 ============================ */

int iv_ctl_handle(const iv_ctl_ctx_t *ctx, const char *in, size_t in_len,
                  char *out, size_t out_cap, size_t *out_len)
{
    char         method[IV_CTL_METHOD_MAX];
    json_object *req = NULL;
    json_object *params = NULL;
    int          rc;

    if (ctx == NULL || in == NULL || out == NULL || out_len == NULL)
        return IV_EINVAL;
    if (in_len == 0u || in_len > IV_CTL_RSP_MAX)
        return IV_EINVAL;

    if (in[0] == '{') {
        /* JSON 形态：用带长度的解析器（载荷非 NUL 结尾，见文件头） */
        json_tokener *t = json_tokener_new();
        const char   *m;

        if (t == NULL)
            return IV_ENOMEM;
        req = json_tokener_parse_ex(t, in, (int)in_len);
        if (req == NULL || json_tokener_get_error(t) != json_tokener_success) {
            if (req != NULL)
                json_object_put(req);
            json_tokener_free(t);
            return rsp_err(IV_EPROTO, "bad json", out, out_cap, out_len);
        }
        json_tokener_free(t);
        if (!json_object_is_type(req, json_type_object)) {
            json_object_put(req);
            return rsp_err(IV_EPROTO, "payload must be an object", out, out_cap, out_len);
        }
        m = param_str(req, "m");
        if (m == NULL) {
            json_object_put(req);
            return rsp_err(IV_EINVAL, "missing m", out, out_cap, out_len);
        }
        (void)snprintf(method, sizeof(method), "%.*s", (int)sizeof(method) - 1, m);
        params = param_obj(req);
    } else {
        /* 裸方法名（S10 老形态） */
        if (in_len >= sizeof(method))
            return IV_ENOTSUP;
        memcpy(method, in, in_len);
        method[in_len] = '\0';
    }

    if (strcmp(method, "system.snapshot") == 0)
        rc = m_snapshot(out, out_cap, out_len);
    else if (strcmp(method, "sys.overview") == 0)
        rc = m_overview(ctx, out, out_cap, out_len);
    else if (strcmp(method, "config.get") == 0)
        rc = m_config_get(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "config.class") == 0) {
        int cls = cls_from_name(param_str(params, "cls"));

        if (cls < 0)
            rc = rsp_err(IV_EINVAL, "unknown class", out, out_cap, out_len);
        else
            rc = m_config_list(ctx, cls, out, out_cap, out_len);
    } else if (strcmp(method, "config.all") == 0)
        rc = m_config_list(ctx, -1, out, out_cap, out_len);
    else if (strcmp(method, "config.defaults") == 0)
        rc = m_config_defaults(ctx, out, out_cap, out_len);
    else if (strcmp(method, "config.apply") == 0)
        rc = m_config_apply(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "device.list") == 0)
        rc = m_device_list(ctx, out, out_cap, out_len);
    else if (strcmp(method, "snmp.result") == 0)
        rc = m_snmp_result(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "snmp.run") == 0)
        rc = m_snmp_run(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "camera.search") == 0)
        rc = m_camera_search(ctx, out, out_cap, out_len);
    else if (strcmp(method, "system.time") == 0)
        rc = m_system_time(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "system.reboot") == 0)
        rc = m_system_reboot(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "snapshot.take") == 0)
        rc = m_media_capture(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "record.start") == 0)
        rc = m_media_record_start(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "record.stop") == 0)
        rc = m_media_record_stop(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "record.status") == 0)
        rc = m_media_record_status(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "ota.status") == 0)
        rc = m_ota_status(ctx, out, out_cap, out_len);
    else if (strcmp(method, "ota.request") == 0)
        rc = m_ota_request(ctx, params, out, out_cap, out_len);
    else
        rc = IV_ENOTSUP; /* **不写 out**，由调用方静默丢弃 */

    if (req != NULL)
        json_object_put(req);
    return rc;
}
