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
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#include "ivsbox/iv_camdet.h"
#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_ctl.h"
#include "ivsbox/iv_factory.h"
#include "ivsbox/iv_media.h"
#include "ivsbox/iv_provision.h" /* 设备 SN／身份 MAC：只读工厂身份分区 */
#include "ivsbox/iv_recovery.h" /* 0xD1 载荷的唯一权威构造：iv_recovery_build_payload() */
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
    "gps.status",
    "board.status",
    "netthr.status",
    "relay.set",
    "device.identity.set",
    "camera.search",
    "camera.test",
    "system.time",
    "system.reboot",
    "sys.factory_reset",
    "snapshot.take",
    "record.start",
    "record.stop",
    "record.status",
    "ota.status",
    "ota.request",
    "tunnel.status",
    "tunnel.enable",
    "tunnel.disable"
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

/* 开机时长（毫秒）。**只读内核事实**，与 sys.overview 读 `/proc/net/route`
 * 同一口径：不过滤、不换算、不编造。取不到返回 -1，响应里给 null ——
 * "没读到"和"刚开机 0 ms"必须能分开（与 add_or_null 的 null 口径一致）。
 * `/proc/uptime` 第一列是秒、带两位小数，乘 1000 取整即毫秒。 */
static long long proc_uptime_ms(void)
{
    FILE  *f;
    double sec = -1.0;

    f = fopen("/proc/uptime", "r");
    if (f == NULL)
        return -1;
    if (fscanf(f, "%lf", &sec) != 1)
        sec = -1.0;
    (void)fclose(f);
    if (!(sec >= 0.0))
        return -1;
    return (long long)(sec * 1000.0);
}

/*
 * **保持 S10 的老三字段**（`test_ctl` c02 与集成用例 `test_ctl_snapshot` 都逐字
 * 断言了 method/version/status，前端也在读 status），其后追加只读快照内容。
 *
 * ============================ D-08：为什么补内容而不是改名 ============================
 * 方法名叫 snapshot，原实现却只回三个固定字段 —— 任何人拿到都会以为"快照在
 * `d` 里"，实际什么都没有。改名（如 `system.info`）会让既有客户端、集成用例
 * 与前端一起破；而**追加字段对 `strstr` 型断言与前端都是透明的**，所以选追加。
 *
 * ============================ 放什么、不放什么 ============================
 * 只放"读一次就有、零成本、不改变任何状态"的东西：**开机时长** ＋ **配置诊断
 * 快照**（就是 `iv_config_stat()` 那份只读诊断）。刻意**不放**媒体 / 代理 / 隧道
 * —— 那三者各自有专门方法（`record.status` / `tunnel.status` / `snmp.result`），
 * 在这里再抄一份必然两处不一致，而"同一个量两个接口两个答案"是现场最难查的
 * 一类问题。
 *
 * `cfg` 段在 `ctx->cfg == NULL` 时**整个省略**，不写 null：`system.snapshot` 是
 * 唯一允许无配置句柄的方法（见 iv_ctl.h 的 ctx 注释），省略比 null 少一种前端
 * 分支，也避免伪造出 "items":0 这种会被误读成"一个键都没有"的假数。
 */
static int m_snapshot(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    json_object *d;
    const char  *js;
    size_t       n;
    long long    up;

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;

    json_object_object_add(d, "method", json_object_new_string("system.snapshot"));
    json_object_object_add(d, "version", json_object_new_string(iv_version_string()));
    json_object_object_add(d, "status", json_object_new_string("ok"));

    up = proc_uptime_ms();
    if (up >= 0)
        json_object_object_add(d, "uptime_ms", json_object_new_int64(up));
    else
        json_object_object_add(d, "uptime_ms", NULL); /* JSON null：没读到 */

    if (ctx != NULL && ctx->cfg != NULL) {
        iv_cfg_stat_t st;
        json_object  *c = json_object_new_object();

        if (c != NULL && iv_config_stat(ctx->cfg, &st) == IV_OK) {
            json_object_object_add(c, "version",
                                   json_object_new_int64((int64_t)st.version));
            json_object_object_add(c, "items", json_object_new_int((int)st.items));
            json_object_object_add(c, "from_file",
                                   json_object_new_int((int)st.from_file));
            json_object_object_add(c, "from_default",
                                   json_object_new_int((int)st.from_default));
            json_object_object_add(c, "invalid", json_object_new_int((int)st.invalid));
            json_object_object_add(c, "loaded",
                                   json_object_new_boolean(st.loaded ? 1 : 0));
            json_object_object_add(c, "crc_mismatch",
                                   json_object_new_boolean(st.crc_mismatch ? 1 : 0));
            json_object_object_add(c, "dirty", json_object_new_boolean(st.dirty ? 1 : 0));
            json_object_object_add(d, "cfg", c);
        } else if (c != NULL) {
            json_object_put(c); /* 取不到诊断就不给这一段，不回半截对象 */
        }
    }

    js = json_object_to_json_string_ext(d, JSON_C_TO_STRING_PLAIN);
    n  = (js != NULL) ? strlen(js) : 0u;
    if (js == NULL || n == 0u || n + 1u > cap) {
        json_object_put(d);
        return IV_ERANGE;
    }
    /* **先拷贝再 put**：`js` 指向 json_object 内部的缓冲，put 之后即悬空。 */
    memcpy(out, js, n);
    out[n] = '\0';
    json_object_put(d);
    *out_len = n;
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
 * 本机网口"当前生效"状态（M5-S5.5 网络页）
 *
 * ============================ 为什么非要有这一块 ============================
 * `net.ip` / `net.mask` / `net.gateway` 三个键的出厂默认是**空串**，语义是
 * "不在此处指定、交由系统网络配置决定"（本产品实际由天嵌
 * `EmbedSkyProgress.ini` 的 `ip_mode=staticIp` 生效）。于是网络页上这三个
 * 输入框天然是空的 —— 而现场打开这一页第一件要看的事恰恰是"现在到底是什么
 * 地址"。**把空串当成"值丢了"去查代码是死路**：缺的不是赋值，而是一份
 * "实际生效值"的只读展示。这里补的就是它。
 *
 * ============================ 口径 ============================
 * **只读**：不发 netlink 请求、不改地址/路由、不参与任何判定（判定归
 * netmgr）。取的纯粹是内核当前事实，与 `ip -4 addr` / `ip route` 同源。
 * 取不到的字段给**空串**（前端显示"未获取"），绝不编造一个看起来合理的值。
 *
 * ============================ 默认网关的一个坑 ============================
 * `/proc/net/route` 把 `s_addr` 的**内存小端值**按 `%08X` 打印，故
 * `strtoul(hex)` 的结果**直接赋给 `s_addr`** 就是网络序字节
 * （`0102A8C0` → 192.168.2.1）。**不要套 htonl/ntohl** —— 套了就变成
 * `0.1.2.192` 这类反向地址，且在多字节地址上很难一眼看出。
 * ------------------------------------------------------------------------- */
/* 内核私有标志位：`IFF_LOWER_UP` = "有物理承载"（网线插着 / 4G 拨通）。
 * glibc 的 `<net/if.h>` 不定义它（与 iv_netlink.c 的处理一致），按内核值补一个。
 *
 * **为什么用它而不是 `IFF_RUNNING`**：`ip link` 输里的 `NO-CARRIER` 正是这一位
 * 缺失，语义与"载波"严丝合缝；`IFF_RUNNING` 是内核按 RFC2863 合成的操作态，
 * 还掺了 `netif_running` 与 operstate 判断，对 USB 网卡有过渡态。
 * 这与 `iv_netlink.h` 文件头写的事件路径判据（`IFF_UP && IFF_LOWER_UP`）一致。
 *
 * `getifaddrs()` 的 `ifa_flags` 是 `unsigned int`，装得下 0x10000 这一位；
 * 而 ioctl 的 `ifr_flags` 是 short 装不下 —— 这就是 `iv_netlink` 查询路径不得不
 * 退用 `IFF_RUNNING` 的原因，本处没有那个限制。 */
#define IVCL_KF_LOWER_UP ((unsigned int)0x00010000u)

/* 单个网口的三态。三个都 0 表示这个网口不存在。 */
typedef struct {
    int present; /* 内核里有这个网口 */
    int up;      /* 管理态已启用（IFF_UP） */
    int carrier; /* 有物理承载（IFF_LOWER_UP） */
} ivcl_iface_t;

/*
 * 取一个网口的三态。
 *
 * **默认网关的可用性与网络页的显示都用它**——判定只留在这一处，免得"同一个
 * 网口两处两个答案"（这类不一致在现场最难查）。同网口会返回多条记录
 * （AF_INET / AF_INET6 / AF_PACKET），flags 相同，取第一条即可。
 */
static void iface_flags_of(const char *name, ivcl_iface_t *out)
{
    struct ifaddrs *head = NULL;
    struct ifaddrs *ifa;

    out->present = 0;
    out->up      = 0;
    out->carrier = 0;
    if (getifaddrs(&head) != 0)
        return;
    for (ifa = head; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == NULL || strcmp(ifa->ifa_name, name) != 0)
            continue;
        out->present = 1;
        out->up      = (ifa->ifa_flags & IFF_UP) != 0;
        out->carrier = (ifa->ifa_flags & IVCL_KF_LOWER_UP) != 0;
        break;
    }
    freeifaddrs(head);
}

/* 出口网卡为什么不能用 —— 页面要照实说，不能笼统写"不可用"。 */
static const char *iface_state_text(const ivcl_iface_t *s)
{
    if (!s->present)
        return "未检测到";
    if (!s->up)
        return "已关闭";
    return "无载波";
}

int iv_ctl_route_pick(const iv_ctl_route_t *r, int n)
{
    int i;

    if (r == NULL || n <= 0)
        return -1;
    for (i = 0; i < n; i++) {
        if (r[i].iface_ok)
            return i;
    }
    return -1;
}

static void add_iface_link(json_object *arr, const char *want)
{
    struct ifaddrs *head = NULL;
    struct ifaddrs *ifa;
    json_object    *o;
    ivcl_iface_t    st;
    char            ip[INET_ADDRSTRLEN]   = "";
    char            mask[INET_ADDRSTRLEN] = "";
    char            mac[20]               = "";

    /* 三个状态位统一走 iface_flags_of()（默认网关那边用的是同一个判定）。 */
    iface_flags_of(want, &st);

    if (getifaddrs(&head) != 0)
        return;
    for (ifa = head; ifa != NULL; ifa = ifa->ifa_next) {
        const struct sockaddr *sa;

        if (ifa->ifa_name == NULL || strcmp(ifa->ifa_name, want) != 0)
            continue;
        sa = ifa->ifa_addr;
        if (sa == NULL)
            continue;
        if (sa->sa_family == AF_INET && ip[0] == '\0') {
            const struct sockaddr_in *sin =
                (const struct sockaddr_in *)(const void *)sa;

            if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) != NULL &&
                ifa->ifa_netmask != NULL) {
                const struct sockaddr_in *nm =
                    (const struct sockaddr_in *)(const void *)ifa->ifa_netmask;

                (void)inet_ntop(AF_INET, &nm->sin_addr, mask, sizeof(mask));
            }
        } else if (sa->sa_family == AF_PACKET && mac[0] == '\0') {
            const struct sockaddr_ll *sll =
                (const struct sockaddr_ll *)(const void *)sa;

            if (sll->sll_halen == 6u)
                (void)snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                               sll->sll_addr[0], sll->sll_addr[1], sll->sll_addr[2],
                               sll->sll_addr[3], sll->sll_addr[4], sll->sll_addr[5]);
        }
    }
    freeifaddrs(head);

    o = json_object_new_object();
    if (o == NULL)
        return;
    json_object_object_add(o, "name", json_object_new_string(want));
    json_object_object_add(o, "present", json_object_new_boolean(st.present));
    /* `up` 只管管理态（等价于 `ip link` 里的 `<...,UP>`）；
     * `carrier` 单独给"有没有承载"。两者分开是因为**断链时 `up` 仍为真**
     * （网口开着、只是没线/没信号），合成一个字段就没法区分"关掉了"与"断了"。 */
    json_object_object_add(o, "up", json_object_new_boolean(st.up));
    json_object_object_add(o, "carrier", json_object_new_boolean(st.carrier));
    json_object_object_add(o, "ip", json_object_new_string(ip));
    json_object_object_add(o, "mask", json_object_new_string(mask));
    json_object_object_add(o, "mac", json_object_new_string(mac));
    json_object_array_add(arr, o);
}

/*
 * 取**可用**默认路由的出口与网关。取不到给空串。
 *
 * ============================ 与旧实现的差别 ============================
 * 旧实现见到 `Destination==0 && Flags&RTF_UP` 就采信。但断链后内核**不删**默认
 * 路由，只给它打 `RTNH_F_LINKDOWN`（`ip route` 显示成
 * `default via 10.158.0.1 dev usb0 linkdown`），于是断掉的链路被报成"默认网关"，
 * 与 4G 页"SIM 卡未插入"自相矛盾。现在多一道闸门：**出口网卡必须存在、已启用、
 * 有载波**；不满足的条目不采信，并把"内核里那条为什么不算"写进 skipout，
 * 让页面能照实说明，而不是悄悄换一个值。
 *
 * **不要去 `/proc/net/route` 的 Flags 列找 linkdown 位**：那一列由内核
 * `fib_flag_trans()` 现算，值域只有 `RTF_UP|RTF_GATEWAY|RTF_HOST|RTF_REJECT`
 * （linux-5.4 `net/ipv4/fib_trie.c`），linkdown 根本不在里面。
 */
static void default_route_of(char *ifout, size_t ifcap, char *gwout, size_t gwcap,
                             char *skipout, size_t skipcap)
{
    iv_ctl_route_t cand[IV_CTL_ROUTE_MAX];
    const char    *why[IV_CTL_ROUTE_MAX] = { NULL };
    int            ncand = 0;
    int            pick;
    int            i;
    FILE          *f;

    if (ifout != NULL && ifcap > 0u)
        ifout[0] = '\0';
    if (gwout != NULL && gwcap > 0u)
        gwout[0] = '\0';
    if (skipout != NULL && skipcap > 0u)
        skipout[0] = '\0';

    f = fopen("/proc/net/route", "r");
    if (f == NULL)
        return;
    {
        char line[256];

        if (fgets(line, sizeof(line), f) == NULL) {
            (void)fclose(f); /* 连表头都没有：空表 */
            return;
        }
        while (fgets(line, sizeof(line), f) != NULL && ncand < IV_CTL_ROUTE_MAX) {
            char          ifname[32];
            unsigned long dst = 0ul;
            unsigned long g   = 0ul;
            unsigned long fl  = 0ul;
            struct in_addr a;
            ivcl_iface_t  st;

            if (sscanf(line, "%31s %lx %lx %lx", ifname, &dst, &g, &fl) != 4)
                continue;
            if (dst != 0ul || (fl & 0x1ul) == 0ul) /* 非默认路由 / 未启用 */
                continue;
            (void)memset(&cand[ncand], 0, sizeof(cand[ncand]));
            (void)snprintf(cand[ncand].ifname, sizeof(cand[ncand].ifname), "%s",
                           ifname);
            /* 见上面"默认网关的一个坑"：strtoul 的结果直接就是网络序字节 */
            a.s_addr = (uint32_t)g;
            if (inet_ntop(AF_INET, &a, cand[ncand].gw,
                          (socklen_t)sizeof(cand[ncand].gw)) == NULL)
                cand[ncand].gw[0] = '\0';

            iface_flags_of(ifname, &st);
            cand[ncand].iface_ok = (st.present && st.up && st.carrier) ? 1 : 0;
            why[ncand]           = iface_state_text(&st);
            ncand++;
        }
    }
    (void)fclose(f);

    pick = iv_ctl_route_pick(cand, ncand);
    if (pick >= 0) {
        if (ifout != NULL && ifcap > 0u)
            (void)snprintf(ifout, ifcap, "%s", cand[pick].ifname);
        if (gwout != NULL && gwcap > 0u)
            (void)snprintf(gwout, gwcap, "%s", cand[pick].gw);
    }

    /* 不可用的那些照样列出来：现场最想知道的是"我明明看见有默认路由，
     * 为什么设备说没有"。多条用"；"连，超长就地截断。 */
    {
        size_t used = 0u;

        for (i = 0; i < ncand; i++) {
            int n;

            if (cand[i].iface_ok)
                continue;
            if (skipout == NULL || skipcap == 0u || used + 1u >= skipcap)
                break;
            n = snprintf(skipout + used, skipcap - used, "%s%s（%s %s）",
                         (used > 0u) ? "；" : "",
                         (cand[i].gw[0] != '\0') ? cand[i].gw : "0.0.0.0",
                         cand[i].ifname,
                         (why[i] != NULL) ? why[i] : "不可用");
            if (n > 0) {
                size_t adv = used + (size_t)n;

                used = (adv < skipcap) ? adv : (skipcap - 1u);
            }
        }
    }
}

/* 恒返回一个对象（取不到也返回，只是各字段为空/0），前端据此统一渲染。 */
static json_object *build_link_view(void)
{
    json_object *l   = json_object_new_object();
    json_object *arr = json_object_new_array();
    char         gwif[32];
    char         gw[INET_ADDRSTRLEN];
    char         skipped[256];

    if (l == NULL || arr == NULL) {
        if (l != NULL)
            json_object_put(l);
        if (arr != NULL)
            json_object_put(arr);
        return NULL;
    }

    default_route_of(gwif, sizeof(gwif), gw, sizeof(gw), skipped, sizeof(skipped));
    json_object_object_add(l, "gateway", json_object_new_string(gw));
    json_object_object_add(l, "gateway_if", json_object_new_string(gwif));
    /* 内核里有、但出口没载波而被忽略的默认路由（空串＝没有）。
     * 单列出来是因为"设备说没有网关"与"有网关但链路断了"是两回事，
     * 现场看到的现象一样，处置办法完全不同。 */
    json_object_object_add(l, "gateway_ignored", json_object_new_string(skipped));

    /* 只报两条 WAN（架构 §18.3：无线＝usb0、有线＝eth0）。
     * `wlan0` 是给摄像机用的 AP，不是 WAN，故不列 —— 列进来会被读成"第三条外网"。*/
    add_iface_link(arr, "eth0");
    add_iface_link(arr, "usb0");
    json_object_object_add(l, "ifaces", arr);
    return l;
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

/* ---------------------------------------------------------------------------
 * 设备基础信息（总览页）
 *
 * ============================ 总览只答一个问题 ============================
 * "**这台设备是谁**"。版本号、硬件型号、设备 SN、MAC、主机名这类"不随时间变"的
 * 身份信息归它；网络状态归网络页、摄像机六路归摄像机页、采集板镜像（含箱门／
 * 总功率）归控制板页 —— 各归各位，不再往总览页堆。
 *
 * ============================ 「型号」有两个，别混 ============================
 *   `hw_model` ＝ 配置键 `sys.device.model`，平台 0xE3 应答里的 `mod`，
 *                随箱体变体走、Web 可改 —— **平台认的硬件型号**；
 *   `model`    ＝ 设备树板级 compatible 串，由板卡决定、现场改不了。
 * 2026-10-26 之前总览页只显示后者却标成"硬件型号"，与平台报上去的 `mod`
 * 对不上号；现在两个都给，标签在前端分开写（「硬件型号」/「板卡型号」）。
 *
 * ============================ 三个来源，各有各的坑 ============================
 * 1. **板级型号／SoC**：`/proc/device-tree/model` 是**带尾随 NUL 的定长串**（实测还
 *    带尾随空格），`fread` 拿回来的长度不能用 `strlen` 算 —— 值域里真有 NUL。
 *    `compatible` 更是 NUL 分隔的多段（`embedsky,tqt113-core\0arm,sun8iw20p1`），
 *    取第一段即板级 compatible；C 字符串天然在第一个 NUL 处结束，不必额外截断。
 * 2. **设备 SN／MAC**：读工厂身份分区。**分区存在 ≠ 已写入** —— 未写入时 magic 与
 *    累加校验都不过，按"不存在"上报。此时给 JSON null（前端显示"—"），**不给空串**：
 *    空串的语义是"读到了，但值是空的"，与"没有身份"是两件事。
 *    身份只在产线写入、运行期不变（写入后需重启生效），故**只读一次就缓存**：
 *    总览 5 s 刷一次，每次都去读 4 KiB 块设备没有意义。
 * 3. **内核／主机名**：`uname()` 与 `gethostname()`，纯本机事实，不涉配置。
 * ------------------------------------------------------------------------- */

/* 读一个小文本文件并裁掉尾随的 NUL / 空白。读不到就留空串（调用方据此给 null）。 */
static void read_dt_string(const char *path, char *out, size_t cap)
{
    FILE  *f;
    size_t n;

    if (out == NULL || cap < 2u)
        return;
    out[0] = '\0';
    f = fopen(path, "rb");
    if (f == NULL)
        return;
    n = fread(out, 1u, cap - 1u, f);
    (void)fclose(f);
    out[n] = '\0';
    while (n > 0u) {
        char c = out[n - 1u];

        if (c == '\0' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
            out[--n] = '\0';
        else
            break;
    }
}

/* 工厂身份分区未写入时的设备 SN 默认值（2026-10-17 用户定）。
 * **只影响"显示/上报用的编号"，不影响隧道 secretKey 的取值** —— 隧道侧拿不到
 * 身份就是拿不到，不会因为这里有个默认值就把通道打开。 */
#define IV_CTL_SN_DEFAULT "1"

typedef struct {
    int  loaded;                  /* 只尝试一次：产线写入需重启，运行期重试无意义 */
    int  from_factory;            /* 1＝读过工厂分区；0＝用的是默认值 */
    char sn[IV_PROV_TID_MAX];
    char mac[IV_PROV_MAC_MAX];    /* 空串＝没有身份 MAC */
} dev_identity_t;

/* 身份缓存放**文件级**（不是函数内 static）：`device.identity.set` 写成功后要把
 * 它整块失效，否则页面刷新还显示旧值，看起来像没写成。 */
static dev_identity_t g_dev_id;

static const dev_identity_t *device_identity(void)
{
    iv_prov_id_t raw;

    if (g_dev_id.loaded)
        return &g_dev_id;
    g_dev_id.loaded = 1;
    if (iv_prov_private_read(NULL, &raw) == IV_OK) {
        static const uint8_t zero_mac[6] = {0, 0, 0, 0, 0, 0};

        iv_prov_tid_from_u32(raw.devid, g_dev_id.sn);
        /* 全零 MAC 视为"没有身份 MAC"：只写 ID 不写 MAC 的记录会留下 6 个 0，
         * 直接格式化会显示成 `00:00:00:00:00:00`，看着像真 MAC，比留空更误导。 */
        if (memcmp(raw.mac, zero_mac, sizeof zero_mac) != 0)
            iv_prov_mac_format(raw.mac, g_dev_id.mac);
        g_dev_id.from_factory = 1;
    } else {
        /* 本板就是这状态：`/dev/by-name/private` 在，但 magic/校验不过。
         * SN 取默认值让页面有个可读编号，但**必须把来源一起给出去**
         * （`sn_source`），否则售后会把默认值当成产线写入的真 SN。 */
        (void)snprintf(g_dev_id.sn, sizeof g_dev_id.sn, "%s", IV_CTL_SN_DEFAULT);
    }
    return &g_dev_id;
}

static void device_identity_invalidate(void)
{
    memset(&g_dev_id, 0, sizeof g_dev_id);
}

/* 一个字段：有值给字符串，没值给 JSON null（**不给空串**）。 */
static void add_str_or_null(json_object *o, const char *k, const char *v)
{
    if (v != NULL && v[0] != '\0')
        json_object_object_add(o, k, json_object_new_string(v));
    else
        json_object_object_add(o, k, NULL);
}

/* 设备类型的码值（4 位小写十六进制，与上报帧头 `DEVTYPE=%04x` 同一种写法）。
 *
 * ============================ 码值只有一个来源 ============================
 * `IV_PROTO_DEVTYPE_DEFAULT`（iv_proto.h）同时是**上报帧头 `DEVTYPE=`**、
 * **查询应答 CRC 的输入**、以及本处的显示值 —— 不是三处各写一份。
 * 2026-10-26 用户拍板「出厂定死 0x0A00，暂时不需要配置」，所以这里**没有配置键**：
 * 要改只能改那个宏并重编。
 *
 * ============================ 只给码值，不给中文名 ============================
 * 本文件（及整个后端）的 JSON 值一律是**标识符或原样数值**，中文标签全在前端
 * （`web/static/app.js` 的 `CNS_NAME` / `MN_NAME` / `SNMP_STATE` 都是这个套路）。
 * 设备类型的可读名同理放在 `app.js` 的 `DEVTYPE_NAME` 里 —— 表只有一张，
 * 加类型只改那一处；前端漏了也有码值可看，不会显示成错误的名字。 */
static void add_devtype(json_object *dev)
{
    char hex[8];

    (void)snprintf(hex, sizeof hex, "%04x", (unsigned)IV_PROTO_DEVTYPE_DEFAULT);
    json_object_object_add(dev, "devtype", json_object_new_string(hex));
}

static void add_device_info(json_object *d, const iv_config_t *cfg)
{
    json_object          *dev = json_object_new_object();
    const dev_identity_t *id  = device_identity();
    struct utsname        u;
    char                  model[64]  = "";
    char                  soc[64]    = "";
    char                  ethmac[20] = "";
    char                  host[64]   = "";
    const char           *name       = NULL;
    const char           *hwm        = NULL;

    if (dev == NULL)
        return;

    json_object_object_add(dev, "version", json_object_new_string(iv_version_string()));

    /* ============================ 设备类型 / 硬件型号 / 平台口径版本 ============================
     * 2026-10-26 用户要求 Web 上「显示设备的软件、硬件版本号、设备类型等参数」。
     * 三个"版本/型号"必须各自说清来源，否则"版本号"这一个词会有三个互相打架的答案：
     *
     *   `devtype`
     *       出厂定死的设备类型码值，见 add_devtype()。**没有配置键**。
     *   `hw_model`
     *       配置键 `sys.device.model`，就是平台 0xE3「查询设备版本号」应答里的 `mod`
     *       —— 这才是平台认的**硬件型号**。它随箱体变体走，Web 配置页可改。
     *       与下面的 `model` **不是一回事**：`model` 是设备树的板级 compatible 串
     *       （如 `embedsky,tqt113-core`），由板卡决定、现场改不了。
     *   `sysver`
     *       平台口径的软件版本 `1.0.0.<构建日期>`（见 iv_version.h 的分工）：
     *       工程串 `version` 带 `+g<短哈希>` 供定位提交，平台串只带日期。
     *
     * 缺值的处置与本函数其他字段同口径：空串 ⇒ JSON null（前端显示"—"）。 */
    add_devtype(dev);
    json_object_object_add(dev, "sysver",
                           json_object_new_string(iv_version_sysver()));

    /* 设备名称来自配置 `sys.device.name`（不是身份分区）：它是**称呼**、不是唯一标识，
     * 现场随时会改，写进工厂分区既没必要也不该动。空串＝没起名 ⇒ 给 null，
     * 与"读到了但是空"的其他字段同口径（前端 null 显示"—"）。 */
    if (cfg != NULL && iv_config_get_str(cfg, "sys.device.name", &name) == IV_OK)
        add_str_or_null(dev, "name", name);
    else
        add_str_or_null(dev, "name", NULL);

    if (cfg != NULL && iv_config_get_str(cfg, "sys.device.model", &hwm) == IV_OK)
        add_str_or_null(dev, "hw_model", hwm);
    else
        add_str_or_null(dev, "hw_model", NULL);

    read_dt_string("/proc/device-tree/model", model, sizeof model);
    read_dt_string("/proc/device-tree/compatible", soc, sizeof soc);
    add_str_or_null(dev, "model", model);
    add_str_or_null(dev, "soc", soc);

    /* SN 与 MAC 一起来自身份记录；SN 缺失时取默认值 "1"（见 IV_CTL_SN_DEFAULT），
     * 但来源如实给出去：`sn_source` = factory / default。
     * 身份 MAC 缺了就只能给 null —— 它没有默认值，也不能拿 eth0 的 MAC 顶替
     * （一个是产线分配、一个是网卡驱动决定，两件事）。 */
    add_str_or_null(dev, "sn", id->sn);
    json_object_object_add(dev, "sn_source",
                           json_object_new_string(id->from_factory ? "factory" : "default"));
    add_str_or_null(dev, "id_mac", id->mac);

    read_dt_string("/sys/class/net/eth0/address", ethmac, sizeof ethmac);
    add_str_or_null(dev, "eth0_mac", ethmac);

    if (gethostname(host, sizeof host) == 0) {
        host[sizeof host - 1u] = '\0';
        add_str_or_null(dev, "hostname", host);
    } else {
        add_str_or_null(dev, "hostname", "");
    }

    if (uname(&u) == 0) {
        add_str_or_null(dev, "kernel", u.release);
        add_str_or_null(dev, "machine", u.machine);
    } else {
        add_str_or_null(dev, "kernel", "");
        add_str_or_null(dev, "machine", "");
    }

    /* 系统墙钟：现场核对"设备时间对不对"最直接的一眼（对时通道见 system.time）。 */
    json_object_object_add(dev, "wall_sec", json_object_new_int64((int64_t)time(NULL)));
    json_object_object_add(d, "device", dev);
}

/* 平台传输层状态名。注意与 `iv_netmgr_state_name()` 的区别：那边答"该走哪条 WAN"，
 * 这边答"报文管道通不通"，两件事不能互相顶替。 */
static const char *transport_state_name(iv_transport_state_t s)
{
    switch (s) {
    case IV_TRANSPORT_UP:
        return "up";
    case IV_TRANSPORT_CONNECTING:
        return "connecting";
    case IV_TRANSPORT_DOWN:
        return "down";
    case IV_TRANSPORT_CLOSED:
        return "closed";
    default:
        return "unknown";
    }
}

/* 当前通信网口（网络页「平台通信」块的数据源）。
 *
 * 这里必须把**判定**和**事实**分开给：
 *   - `ifname/kind/ifindex`＝netmgr 判定"该走哪条 WAN"（带迟滞）；
 *   - `bound/transport`＝传输层实际绑定的网卡与连接状态（真正决定报文从哪儿出）。
 * 2026-10-19 之前就出现过"界面显示选了 4G、报文仍从 eth0 出去"——配置值没传到
 * 传输层，只看活动出口完全看不出来。所以在后端就把一致性算成 `bound_ok`，
 * 不让前端各自判（同一条记录两个接口两个答案的坑，本项目已经踩过）。
 */
static json_object *build_wan_view(const iv_ctl_ctx_t *ctx)
{
    json_object *w = json_object_new_object();
    iv_report_t *rp;
    const char  *nm;
    const char  *bound = "";

    if (w == NULL)
        return NULL;

    nm = iv_agent_active_wan_name(ctx->agent);
    rp = iv_agent_report(ctx->agent);

    json_object_object_add(w, "ifname", json_object_new_string(nm));
    json_object_object_add(
        w, "kind",
        json_object_new_string(iv_agent_active_wan(ctx->agent) == IV_WAN_WIRED ? "wired"
                                                                              : "wireless"));
    json_object_object_add(w, "ifindex",
                           json_object_new_int(iv_agent_active_ifindex(ctx->agent)));
    json_object_object_add(
        w, "switches", json_object_new_int64((int64_t)iv_agent_netmgr_switches(ctx->agent)));
    json_object_object_add(
        w, "txn_fail", json_object_new_int64((int64_t)iv_agent_netmgr_txn_fail(ctx->agent)));

    if (rp != NULL) {
        bound = iv_transport_bind_if(&rp->tr);
        json_object_object_add(w, "bound", json_object_new_string(bound));
        json_object_object_add(
            w, "transport",
            json_object_new_string(transport_state_name(iv_transport_state(&rp->tr))));
        json_object_object_add(w, "server", json_object_new_string(rp->srv_host));
        json_object_object_add(w, "port", json_object_new_int((int)rp->srv_port));
        json_object_object_add(w, "queue",
                               json_object_new_int((int)iv_report_queue_count(rp)));
        json_object_object_add(
            w, "bind_fail", json_object_new_int64((int64_t)iv_transport_bind_fail(&rp->tr)));
        json_object_object_add(w, "bind_errno",
                               json_object_new_int(iv_transport_bind_errno(&rp->tr)));
    } else {
        /* 没有上报链：字段照样给（空值/0），前端不必为"接口缺字段"再写一套分支 */
        json_object_object_add(w, "bound", json_object_new_string(""));
        json_object_object_add(w, "transport", json_object_new_string("closed"));
        json_object_object_add(w, "server", json_object_new_string(""));
        json_object_object_add(w, "port", json_object_new_int(0));
        json_object_object_add(w, "queue", json_object_new_int(0));
        json_object_object_add(w, "bind_fail", json_object_new_int64(0));
        json_object_object_add(w, "bind_errno", json_object_new_int(0));
    }
    /* 空名字＝该 WAN 没配名字（不是"绑对了"），这种情况一律算不一致 */
    json_object_object_add(
        w, "bound_ok",
        json_object_new_boolean(nm[0] != '\0' && strcmp(nm, bound) == 0));
    return w;
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
    /* 总览页要的那一块（身份/型号/内核/主机名）。其余块留给各自的页面。 */
    add_device_info(d, ctx->cfg);

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
    /* M5-S5.5：网络页要的"当前生效"（IP/掩码/网关/MAC）。**只读**，与 netmgr
     * 的判定无关；配置里的 `net.ip` 为空时前端就用这块补位，见 build_link_view()。 */
    {
        json_object *lv = build_link_view();

        if (lv != NULL)
            json_object_object_add(net, "link", lv);
    }
    /* 当前通信网口：agent 为空时字段全给空值/0（语义＝"未装配"），
     * 前端据 `net.state == "n/a"` 区分"未装配"与"没配名字"，不必判字段缺失。 */
    {
        json_object *wv = build_wan_view(ctx);

        if (wv != NULL)
            json_object_object_add(net, "wan", wv);
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
        char          dsc[IV_CFG_DESC_MAX];
        json_object  *e;

        if (iv_config_default_get(IV_CFG_NAME, k, &dt, txt, sizeof(txt)) != IV_OK)
            continue;
        e = json_object_new_object();
        if (e == NULL)
            continue;
        json_object_object_add(e, "t", json_object_new_string(cfg_type_name(dt)));
        /* 中文说明：前端拿它当字段标签（否则界面上全是 `net.server.wired.host`
         * 这类英文键，现场人员看不懂）。取不到就不给 `d`，前端回落成键名。 */
        dsc[0] = '\0';
        if (iv_config_default_desc(IV_CFG_NAME, k, dsc, sizeof(dsc)) == IV_OK && dsc[0] != '\0')
            json_object_object_add(e, "d", json_object_new_string(dsc));
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
 * 编译失败正好证明"改配置"这条路径没有被 const 悄悄绕开。
 *
 * 返回 setter 的错误码（R-03）：原先这里一律 `(void)` 丢弃返回值，于是
 * 超长字符串被 `iv_config_set_str()` 以 IV_ERANGE 拒绝、调用方却照样回
 * `ok:true applied:N`，网页显示"保存成功"而回读是空的。 */
static int apply_set(iv_config_t *cfg, const char *key, iv_cfg_type_t t,
                     json_object *v)
{
    switch (t) {
    case IV_CFG_T_STR:
        return iv_config_set_str(cfg, key, json_object_get_string(v));
    case IV_CFG_T_INT:
        return iv_config_set_int(cfg, key, (int64_t)json_object_get_int64(v));
    case IV_CFG_T_BOOL:
        return iv_config_set_bool(cfg, key, json_object_get_boolean(v));
    case IV_CFG_T_DBL:
        return iv_config_set_dbl(cfg, key, json_object_get_double(v));
    default:
        return IV_EINVAL;
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
            /* 字符串长度预检（R-03）：`iv_config_set_str()` 对 >= IV_CFG_STR_MAX
             * 的值返回 IV_ERANGE 并**保持原值**，但预检原先只查"键存在+类型对"，
             * 于是超长值一路走到写入阶段被静默拒掉，接口还回 `ok:true applied:N`。
             * 长度上限与 setter 严格同源（同一常量），避免两处各写一个数。 */
            if (want == IV_CFG_T_STR &&
                strlen(json_object_get_string(v)) >= IV_CFG_STR_MAX)
                return rsp_err(IV_ERANGE, "string too long", out, cap, out_len);

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

    /* ③ 写入内存。任一条被 setter 拒绝（区间/枚举越界等）就**整条回滚**并报错：
     * 只报"成功"却不写、"写了一半"两种情形都不允许 —— 前者的现场表现是
     * "保存成功但值没变"，后者更要命（半套配置已生效，页面上却是一份失败）。 */
    {
        int applied = 0;

        json_object_object_foreach(items, k, v) {
            iv_cfg_type_t want = IV_CFG_T_STR;
            char          dtxt[IV_CFG_STR_MAX];
            int           wrc;

            (void)iv_config_default_get(IV_CFG_NAME, k, &want, dtxt, sizeof(dtxt));
            wrc = apply_set(ctx->cfg, k, want, v);
            if (wrc != IV_OK) {
                int j;

                /* 手头这一条已经被拒（原值未动），只需回滚前面写成功的 */
                for (j = 0; j < applied; j++)
                    apply_restore(ctx->cfg, &s_bak[j]);
                return rsp_err(wrc, "item rejected by config layer", out, cap,
                               out_len);
            }
            applied++;
        }
        n = applied;
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
    /* S9 延伸：审计落 control.db（detail 只含计数，**不含键值** —— items 里
     * 可能出现 camera.ch.N.pass 这类键，值绝不进审计）。 */
    if (ctx->store != NULL) {
        char adetail[48];

        (void)snprintf(adetail, sizeof adetail, "items=%d", n);
        (void)iv_store_audit_add(ctx->store, "config.apply", adetail, 0,
                                 (int64_t)time(NULL));
    }
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

/* ---- camera.test：立即重测一路（2026-10-22 补，Web 摄像机页「测」按钮）----
 *
 * 为什么是"排上"而不是"返回结果"：`iv_camdet` 是非阻塞状态机（RTSP 一次
 * OPTIONS 1 s 超时、端口扫描 7 个端口各 1 s、PING 要集满 4 个样本），在这里
 * 同步等结果就会把 Reactor 卡住 —— 与"ctl 路由绝不阻塞"（架构 §4.2）直接冲突。
 * 所以本接口只做"立刻重开该路的探测"，`expect_ms` 给出结论大约多久后出现，
 * 前端据此决定多少毫秒后自动刷新 CNS。
 *
 * **`ch` 是 1 基（1..6）**：与 `relay.set` 的 1..3、页面上"摄像机1"同序。
 * 内部下标是 `ch-1`（`camera.ch.N.*` 的 N 是 0 基），差一在这一处一次翻完，
 * 前端只管按显示序号传。 */
static int m_camera_test(const iv_ctl_ctx_t *ctx, json_object *p,
                         char *out, size_t cap, size_t *out_len)
{
    const iv_camdet_t *cd;
    json_object       *d;
    int64_t            ch = 0;
    int                mode;
    int                rc;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);
    if (param_i64(p, "ch", &ch) != IV_OK)
        return rsp_err(IV_EINVAL, "missing ch", out, cap, out_len);
    if (ch < 1 || ch > IV_CAMDET_CH_N)
        return rsp_err(IV_EINVAL, "ch must be 1..6", out, cap, out_len);

    rc = iv_agent_camdet_probe(ctx->agent, (int)ch - 1, iv_clock_monotonic_ms());
    /* ENOENT 单独给话术：现场点「测」最常见的原因就是这一路还没配 IP，
     * 报"probe rejected"等于让人去翻日志。 */
    if (rc == IV_ENOENT)
        return rsp_err(rc, "this channel has no IP configured", out, cap, out_len);
    if (rc != IV_OK)
        return rsp_err(rc, "probe rejected", out, cap, out_len);

    cd   = iv_agent_camdet(ctx->agent);
    mode = (cd != NULL) ? iv_camdet_mode(cd) : 0;

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "queued", json_object_new_boolean(1));
    json_object_object_add(d, "ch", json_object_new_int((int)ch));
    json_object_object_add(d, "mode", json_object_new_int(mode));
    json_object_object_add(d, "mode_name",
                           json_object_new_string(iv_camdet_mode_name(mode)));
    /* 最坏时长 + 一拍余量：PING 4 样本 × 5 s；端口扫描 7 端口 × 1 s；
     * RTSP 单次 OPTIONS 1 s。前端按这个值定时回读 CNS。 */
    json_object_object_add(d, "expect_ms",
                           json_object_new_int((mode == IV_CAMDET_MODE_PING) ? 20000
                                                : (mode == IV_CAMDET_MODE_PORT) ? 8000
                                                                               : 1500));
    return rsp_data(d, out, cap, out_len);
}

/* ============================ board.status（S2.6 采集板镜像出口） ============================ */

/* 镜像字段的"未知"处理：`iv_status` 用 **valid 位图**区分"未填"与"真零"
 * （电压电流不会真零，但温度/湿度允许 0，靠值本身判不出来）。这里如实转成
 * JSON **null** —— 采集板没接时页面上必须是一排"—"，不能被读成"输入 0V"。 */
static void add_f32_v(json_object *o, const char *k, int valid, float v)
{
    char txt[24];

    if (valid == 0) {
        add_or_null(o, k, 0, NULL); /* 未填 ⇒ JSON null（不是 0.00） */
        return;
    }
    /* **自己给文本**：镜像里存的是 `float`，若直接 `json_object_new_double()`，
     * json-c 0.13 按 `%.17g` 打印成 `234.16999816894531` —— 值没错，但在界面和抓包里
     * 是一串噪声，看见就得怀疑"是不是解析错了"。采集板的测量精度本就到不了第 3 位
     * 小数（电压 0.01 V、电流 0.01 A），故固定两位小数并把这串文本一起交给 json-c，
     * 保证"数值仍是 number、文本干净"两件事同时成立。 */
    (void)snprintf(txt, sizeof txt, "%.2f", (double)v);
    add_or_null(o, k, 1, json_object_new_double_s((double)v, txt));
}

static void add_i32_v(json_object *o, const char *k, int valid, int32_t v)
{
    add_or_null(o, k, valid != 0, json_object_new_int(v));
}

/* 采集板（控制板）通信参数出口。
 *
 * 三块内容，对应现场三句不同的话：
 *   `link`  —— "线接上了吗、它到底在不在说话"（串口状态 ＋ 成帧/CRC 统计 ＋ 数据年龄）；
 *   `data`  —— "它在说什么"（0xE1 应答镜像的运行参数）；
 *   `limit` / `relay` —— 阈值与三路继电器。
 *
 * **为什么要有 age_ms**：`frames == 0` 有两种完全不同的故障 ——
 * 串口一个字节都没收到（线没接/没上电/波特率错），和收到了字节但没有一帧过校验
 * （协议版本不符、CRC 范围不一致）。只看帧计数两种情况长得一模一样。 */
static int m_board_status(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    const iv_report_t      *rp;
    const iv_status_t      *st;
    const iv_frame_stats_t *fs;
    const char             *spath;
    json_object            *d;
    json_object            *o;
    uint64_t                now;
    int                     i;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);
    rp = iv_agent_report(ctx->agent);
    if (rp == NULL)
        return rsp_err(IV_ESTATE, "board link unavailable", out, cap, out_len);

    st = &rp->st;
    fs = iv_framer_stats(&rp->lk.framer);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;

    /* ---- 链路与统计 ---- */
    o = json_object_new_object();
    if (o != NULL) {
        spath = (rp->cfg.serial_path != NULL) ? rp->cfg.serial_path : "";
        json_object_object_add(o, "path", json_object_new_string(spath));
        json_object_object_add(o, "open", json_object_new_boolean(rp->serial_fd >= 0));
        json_object_object_add(o, "up", json_object_new_int(rp->lk.link_up ? 1 : 0));
        json_object_object_add(o, "rx_bytes",
                               json_object_new_int64((int64_t)rp->serial_rx));
        json_object_object_add(o, "tx_bytes",
                               json_object_new_int64((int64_t)rp->serial_tx));
        json_object_object_add(o, "serial_err",
                               json_object_new_int64((int64_t)rp->serial_err));
        json_object_object_add(o, "tx_drop",
                               json_object_new_int64((int64_t)rp->tx_drop));
        /* 成帧统计：frames 是"交付的完整帧"（CRC ＋ 帧尾都过） */
        json_object_object_add(o, "frames", json_object_new_int((int)fs->frames));
        json_object_object_add(o, "crc_err", json_object_new_int((int)fs->crc_err));
        json_object_object_add(o, "tail_err", json_object_new_int((int)fs->tail_err));
        json_object_object_add(o, "too_long", json_object_new_int((int)fs->too_long));
        json_object_object_add(o, "noise", json_object_new_int((int)fs->noise));
        json_object_object_add(o, "resets", json_object_new_int((int)fs->resets));
        /* 事务侧：轮询下发次数与应答情况 */
        json_object_object_add(o, "polls", json_object_new_int64((int64_t)rp->polls));
        json_object_object_add(o, "reports",
                               json_object_new_int64((int64_t)rp->reports));
        json_object_object_add(o, "heartbeats",
                               json_object_new_int64((int64_t)rp->heartbeats));
        json_object_object_add(o, "query_rsp",
                               json_object_new_int64((int64_t)rp->query_resp));
        json_object_object_add(o, "queue", json_object_new_int((int)iv_report_queue_count(rp)));
        json_object_object_add(o, "queue_full",
                               json_object_new_int64((int64_t)iv_report_queue_full(rp)));
        /* 数据年龄：只剩"多久没听到它说话了"这一个判据 */
        now = iv_clock_monotonic_ms();
        if (rp->last_rx_ms == 0u || now < rp->last_rx_ms)
            json_object_object_add(o, "age_ms", NULL); /* 从未收到任何字节 */
        else
            json_object_object_add(o, "age_ms",
                                   json_object_new_int64((int64_t)(now - rp->last_rx_ms)));
        json_object_object_add(d, "link", o);
    }

    /* ---- 运行参数（0xE1 应答键名照搬，便于与《单片机通信》表逐项对照） ---- */
    o = json_object_new_object();
    if (o != NULL) {
        add_f32_v(o, "V", st->valid.V, st->data.V);
        add_f32_v(o, "A", st->valid.A, st->data.A);
        add_f32_v(o, "H", st->valid.H, st->data.H);
        add_f32_v(o, "T", st->valid.T, st->data.T);
        add_i32_v(o, "DS", st->valid.DS, st->data.DS);   /* 1=关 / 2=开 */
        add_i32_v(o, "P", st->valid.P, st->data.P);
        add_i32_v(o, "SPD", st->valid.SPD, st->data.SPD); /* 防雷 1/2 */
        add_i32_v(o, "PA", st->valid.PA, st->data.PA);
        add_i32_v(o, "PV", st->valid.PV, st->data.PV);
        /* APOWER/AKW 源端就是**字符串化的小数**（不是 JSON number），原样转发，
         * 本层不做数值转换 —— 转了就会引入"0.00 变 0"与精度口径分歧。 */
        add_or_null(o, "APOWER", st->data.APOWER[0] != '\0',
                    json_object_new_string(st->data.APOWER));
        add_or_null(o, "AKW", st->data.AKW[0] != '\0',
                    json_object_new_string(st->data.AKW));
        json_object_object_add(d, "data", o);
    }

    /* ---- 阈值（0xE1 ＋ 0xF1 共享同一批字段；`tl` 允许负） ---- */
    o = json_object_new_object();
    if (o != NULL) {
        add_i32_v(o, "hv", st->valid.hv, st->data.hv);
        add_i32_v(o, "lv", st->valid.lv, st->data.lv);
        add_i32_v(o, "ov", st->valid.ov, st->data.ov);
        add_i32_v(o, "tu", st->valid.tu, st->data.tu);
        add_i32_v(o, "tl", st->valid.tl, st->data.tl);
        add_i32_v(o, "hu", st->valid.hu, st->data.hu);
        add_i32_v(o, "hl", st->valid.hl, st->data.hl);
        add_i32_v(o, "sl", st->valid.sl, st->data.sl);
        add_i32_v(o, "ld", st->valid.ld, st->data.ld);
        json_object_object_add(d, "limit", o);
    }

    /* ---- 三路继电器：状态 1=开 / 2=关 ＋ 各自的电压/电流/功率/用电量 ---- */
    o = json_object_new_array();
    if (o != NULL) {
        for (i = 0; i < 3; i++) {
            json_object *r = json_object_new_object();

            if (r == NULL)
                continue;
            /* 状态与三路测量各有独立 valid 位：某一路只报了状态没报测量时，
             * 该路的 v/a/power/elec 是 null，而不是 0.0。 */
            add_i32_v(r, "state", st->valid.RELAY, st->data.RELAY[i]);
            add_f32_v(r, "v", st->valid.CHV, st->data.CHV[i]);
            add_f32_v(r, "a", st->valid.CHA, st->data.CHA[i]);
            add_f32_v(r, "power", st->valid.POWER, st->data.POWER[i]);
            add_f32_v(r, "elec", st->valid.ELEC, st->data.ELEC[i]);
            json_object_array_add(o, r);
        }
        json_object_object_add(d, "relay", o);
    }

    /* 门开事件累计（S4.5，来自 `0xC1`；与镜像里的 `DS` 是两回事：
     * `DS` 是"现在开着没"，这个是"累计开过几次"）。 */
    json_object_object_add(d, "door_events",
                           json_object_new_int64(
                               (int64_t)iv_agent_door_events(ctx->agent)));

    return rsp_data(d, out, cap, out_len);
}

/* ============================ netthr.status（2026-10-22） ============================
 *
 * `netthr.*` 四个键的**运行期真值**。存在的理由与 `board.status` 一类相同：
 * 配置页能改能存，但"改完到底有没有生效"从界面上看不出 —— 本组键长期是
 * 「网页改了等于没改」（只读一次的 + 零消费方的死键），现场只能靠重启赌一把。
 * 这里直接读模块内部生效值，改完打这个接口就知道有没有落地。
 *
 * 为什么读模块不读 config：两者长期不一致正是本轮修复的对象（`nm_fail_n`
 * 曾经由 `probe.max_fail` 喂，与 `netthr.loss_count` 毫无关系）。
 *
 * `credit` 是每通道**剩余**自愈动作次数 —— 现场排查"为什么这台不再断电重启了"
 * 时，`reboot_max=2` 但 `credit` 全 0 就说明次数已耗尽，不是没配。
 * ⚠ 它和 `reboot_max` 不是一回事：改配置会把 credit 重置回满额。 */
static int m_netthr_status(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    iv_agent_netthr_view_t v;
    json_object           *d;
    json_object           *a;
    int                    i;
    int                    rc;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);
    rc = iv_agent_netthr_view(ctx->agent, &v);
    if (rc != IV_OK)
        return rsp_err(rc, "netthr view unavailable", out, cap, out_len);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    /* `loss_count` 是**切换门限**（`netthr.loss_count`）；`probe_fail_n` 是
     * 探测判 DOWN 的门限（`probe.max_fail`）。两个键、两个消费者，别混。 */
    json_object_object_add(d, "loss_count", json_object_new_int(v.nm_fail_n));
    json_object_object_add(d, "probe_fail_n", json_object_new_int(v.probe_fail_n));
    /* 有线网络延时门限（`probe.timeout_ms` 在引擎里的真值）。平台 `0xFE` /
     * 网页改完，这里跟着变才算"真的生效了"；只变配置不变这里＝只落盘。 */
    json_object_object_add(d, "probe_delay_ms", json_object_new_int(v.probe_delay_ms));
    /* 实测平均时延（每路 WAN）：与 `probe_delay_ms` 一起看才解释得了"为什么显示延时"。 */
    {
        json_object *rtt = json_object_new_array();
        int          w;

        if (rtt != NULL) {
            for (w = 0; w < (int)IV_WAN_N; w++)
                json_object_array_add(rtt, json_object_new_int(v.probe_rtt_ms[w]));
            json_object_object_add(d, "probe_rtt_ms", rtt);
        }
    }
    json_object_object_add(d, "reboot_max", json_object_new_int(v.reboot_max));
    json_object_object_add(d, "reboot_interval_s",
                           json_object_new_int((int)(v.cooldown_ms / 1000u)));

    a = json_object_new_array();
    if (a != NULL) {
        for (i = 0; i < (int)IV_RECOVERY_PORT_N; i++)
            json_object_array_add(a, json_object_new_int((int)v.credit[i]));
        json_object_object_add(d, "credit", a);
    }
    return rsp_data(d, out, cap, out_len);
}

/* ============================ relay.set（S2.6 继电器人工控制） ============================ */

/* 人点一下按钮就把某路继电器吸合/断开。
 *
 * **协议口径（架构 §18.2，一处也不能自创）**：
 *   - 动作帧 `0xD1`，data ＝ JSON `{"port":<0~7>,"status":<0 关/1 开/2 重启>}`，
 *     由 `iv_recovery_build_payload()` **逐字**构造（与 S4.4 自愈下发同一份实现，
 *     不另写一遍 —— 两处生成同一帧时，格式漂移会变成"人工能用、自愈不能用"）；
 *   - **继电器编号 ＝ port ＋ 1**（本工程既定：继电器 1 ⇒ port 0（主网）、
 *     继电器 2~8 ⇒ port 1~7（摄像机））。界面上说的是"第 N 路"，线上必须换算成
 *     `N-1`，换算只在这一个地方做；
 *   - `RELAY` 镜像的口径是 1=开 / 2=关（与 `DS` 相反），而 `0xD1` 的 `status`
 *     是 0=关 / 1=开。**不要把镜像值直接当 status 发出去**。
 *
 * **只发一次、不重传**（`0xD1` 是控制命令，见 `iv_link.h`）；`IV_EBUSY`（同 cmd
 * 有在飞）原样上抛，让调用方稍后重试，绝不在这里排队或丢弃。
 *
 * 发成功后**主动催一次 0xE1**：`0xD1` 的应答不进状态镜像，不催的话界面要等下一个
 * 轮询周期（默认 30 s）才看得到结果 —— 一个人工去合闸的动作，回读必须快。 */
static int m_relay_set(const iv_ctl_ctx_t *ctx, json_object *p,
                       char *out, size_t cap, size_t *out_len)
{
    json_object *v = NULL;
    json_object *d;
    iv_report_t *rp;
    char         payload[24];
    int64_t      ch = 0;
    int          on;
    int          port;
    int          plen;
    int          rc;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);
    if (param_i64(p, "ch", &ch) != IV_OK)
        return rsp_err(IV_EINVAL, "missing ch", out, cap, out_len);
    /* 镜像里只有 3 路带测量的继电器（RELAY1/2/3），故对外只开放 1~3。 */
    if (ch < 1 || ch > 3)
        return rsp_err(IV_EINVAL, "ch must be 1..3", out, cap, out_len);
    if (!json_object_object_get_ex(p, "on", &v) || v == NULL)
        return rsp_err(IV_EINVAL, "missing on", out, cap, out_len);
    on = json_object_get_boolean(v) ? 1 : 0;

    port = (int)ch - 1; /* 继电器 N ⇒ port N-1（见上面的口径） */
    plen = iv_recovery_build_payload(port,
                                     on ? (int)IV_RECOVERY_PWR_ON
                                        : (int)IV_RECOVERY_PWR_OFF,
                                     payload, sizeof payload);
    if (plen < 0)
        return rsp_err(plen, "payload build failed", out, cap, out_len);

    rp = iv_agent_report(ctx->agent);
    if (rp == NULL || iv_report_is_open(rp) != 1)
        return rsp_err(IV_ESTATE, "board link not open", out, cap, out_len);

    /* 时刻取 `rp->now_ms`（report 模块的时钟域），**不要**另取
     * iv_clock_monotonic_ms()：iv_report 的口径是"时间一律由调用方以 now_ms
     * 传入"，而清在飞事务的是 iv_report_step→iv_link_tick(rp->now_ms)。
     * 两个时钟域混用会让 deadline 落在另一个域里 ⇒ 0xD1 永不超时，
     * 后续 relay.set 全被 IV_EBUSY 挡住（集成用例给假时钟时必现）。 */
    rc = iv_report_send_cmd(rp, IV_FRAME_CMD_POWER, payload, (uint16_t)plen,
                            rp->now_ms);
    if (rc != IV_OK)
        return rsp_err(rc, "board link busy or down, retry later", out, cap, out_len);

    /* 催一次全量查询：界面"下发→回读"之间不再空等一个轮询周期。
     * 并打"刷新后立刻补报"标记：`0xD1` 的应答不进镜像，只有 0xE1 应答才刷新，
     * 而平台要的是"继电器一变就上报"（默认周期 60 s 太慢，用户 2026-10-27 报过）。
     * 顺序不能反：先刷镜像、后上报，否则补报出去的是旧状态。
     * 平台 `0xC4` 走的是 `main.c:on_plat_relay()`，同一对调用 —— 两处别只改一处。 */
    iv_report_request_poll(rp);
    iv_report_request_report_after_poll(rp);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "ch", json_object_new_int((int)ch));
    json_object_object_add(d, "port", json_object_new_int(port));
    json_object_object_add(d, "on", json_object_new_boolean(on));
    /* 把真正发出去的 data 原样回显：现场对不上时，第一件事就是看这串字节。 */
    json_object_object_add(d, "payload", json_object_new_string(payload));

    if (ctx->store != NULL) {
        char ad[40];

        (void)snprintf(ad, sizeof ad, "ch=%d port=%d on=%d", (int)ch, port, on);
        (void)iv_store_audit_add(ctx->store, "relay.set", ad, 0, (int64_t)time(NULL));
    }
    return rsp_data(d, out, cap, out_len);
}

/* ============================ device.identity.set ============================ */

/*
 * Web 配置「设备 ID」与「设备 MAC」（用户 2026-10-17 要求）。
 *
 * ============================ 为什么写身份分区，不新加配置键 ============================
 * 设备身份的权威只有一处：工厂身份分区（S5.5）。再加 `sys.devid` / `sys.mac` 两个
 * 配置键就成了**双源** —— 网卡实际用哪个、隧道算 secretKey 用哪个、界面显示哪个，
 * 三处一旦不一致，现场根本排不出来（本项目已有"勿双源"的既定教训）。
 * 所以 Web 配置**直接落到分区里**，读路径（`sys.overview` 的 device 段、隧道取 SN）
 * 保持唯一权威不变。
 *
 * ============================ 五条硬约束 ============================
 * 1. **任一字段可省，省＝不改**：只传 `devid` 就保留原 MAC，反之亦然；两个都省 ⇒
 *    报文错。半个字段写进去比不写更糟。
 * 2. **格式不合整条拒绝**：设备 ID 接受 1~6 位 HEX（≡ 数值，协议里 tid 是 6 位零
 *    填充，现场习惯写 "1"），但 `000000` 是协议里的**广播占位**，写进去等于"没有
 *    ID"，直接拒；MAC 必须 6 字节可解析。
 * 3. **空分区只给 MAC 也拒**：没有记录时必须有 `devid`，否则会写进一条 devid=0 的
 *    "存在但无效"记录 —— 比空分区更难查。
 * 4. **写失败如实报错**（分区缺失/只读/短写都返回负码），不做"存起来了回头生效"。
 * 5. **写完清本模块的身份缓存**，让页面立刻显示新值；但**隧道不跟着变** ——
 *    `main.c` 的 `tunnel_read_sn()` 只在启动时读一次，返回里带 `restart_required`，
 *    免得有人以为改完 SN 通道就换了钥匙。
 *
 * 另：**这里改的是"身份 MAC"，不是网卡 MAC**。网卡 MAC 由天嵌
 * `EmbedSkyProgress.ini` 的 `mac_mode=fixed` + `mac=` 决定，两者互不影响 ——
 * 界面上必须说清楚，否则会有人以为改完这里 `ip link` 就变了。
 */

/* 1~6 位 HEX → 数值。协议里 tid 是 6 位零填充，但现场习惯把 1 号机写成 "1"，
 * 两者数值相同，故宽容输入、统一按数值存（回显时再补零）。 */
static int parse_devid(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    size_t   n = 0;

    if (s == NULL || out == NULL)
        return IV_EINVAL;
    while (s[n] != '\0') {
        int d;

        if (n >= 6u)
            return IV_EINVAL;
        if (s[n] >= '0' && s[n] <= '9')
            d = s[n] - '0';
        else if (s[n] >= 'a' && s[n] <= 'f')
            d = s[n] - 'a' + 10;
        else if (s[n] >= 'A' && s[n] <= 'F')
            d = s[n] - 'A' + 10;
        else
            return IV_EINVAL;
        v = (v << 4) | (uint32_t)d;
        n++;
    }
    if (n == 0u)
        return IV_EINVAL;
    *out = v;
    return IV_OK;
}

static int m_device_identity_set(const iv_ctl_ctx_t *ctx, json_object *p,
                                 char *out, size_t cap, size_t *out_len)
{
    const char  *devid_s = param_str(p, "devid");
    const char  *mac_s   = param_str(p, "mac");
    iv_prov_id_t cur;
    iv_prov_id_t next;
    json_object *d;
    char         tid[IV_PROV_TID_MAX];
    char         mac[IV_PROV_MAC_MAX];
    uint32_t     devid = 0;
    int          have_id;
    int          have_mac;
    int          have_rec;
    int          rc;

    have_id  = (devid_s != NULL && devid_s[0] != '\0');
    have_mac = (mac_s != NULL && mac_s[0] != '\0');
    if (!have_id && !have_mac)
        return rsp_err(IV_EINVAL, "devid or mac required", out, cap, out_len);

    if (have_id) {
        if (parse_devid(devid_s, &devid) != IV_OK)
            return rsp_err(IV_EINVAL, "devid must be 1..6 hex digits", out, cap, out_len);
        if (devid == 0u)
            return rsp_err(IV_EINVAL, "devid 000000 is the broadcast placeholder",
                           out, cap, out_len);
    }

    memset(&cur, 0, sizeof cur);
    have_rec = (iv_prov_private_read(NULL, &cur) == IV_OK);
    if (!have_rec && !have_id)
        return rsp_err(IV_EINVAL, "devid required: identity partition is blank",
                       out, cap, out_len);

    memset(&next, 0, sizeof next);
    if (have_rec)
        next = cur; /* 保留 suite/server 与本次没改的字段 */
    else
        next.suite_id = 5u; /* 与产线工装（tools/provision）一致的默认套件 */
    if (have_id)
        next.devid = devid;
    if (have_mac && iv_prov_mac_parse(mac_s, next.mac) != IV_OK)
        return rsp_err(IV_EINVAL, "bad mac", out, cap, out_len);

    rc = iv_prov_private_write(NULL, &next);
    if (rc != IV_OK)
        return rsp_err(rc, "identity partition write failed", out, cap, out_len);

    /* 写成功才失效缓存：失败时留着旧缓存，界面显示的还是真实生效值。 */
    device_identity_invalidate();

    iv_prov_tid_from_u32(next.devid, tid);
    iv_prov_mac_format(next.mac, mac);
    /* 全零 MAC = 没有身份 MAC（见 device_identity 的同一判断）。回显要与
     * `sys.overview` 一致：那边给 null，这里就不能回 `00:00:00:00:00:00`，
     * 否则前端会把一串零填回输入框，看着像真有 MAC。 */
    {
        static const uint8_t zero_mac[6] = {0, 0, 0, 0, 0, 0};

        if (memcmp(next.mac, zero_mac, sizeof zero_mac) == 0)
            mac[0] = '\0';
    }

    if (ctx->store != NULL) {
        char ad[48];

        (void)snprintf(ad, sizeof ad, "devid=%s mac=%s", tid, mac);
        (void)iv_store_audit_add(ctx->store, "device.identity.set", ad, 0,
                                 (int64_t)time(NULL));
    }

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "devid", json_object_new_string(tid));
    add_str_or_null(d, "mac", mac); /* 没有身份 MAC 时给 null，与 sys.overview 一致 */
    /* 隧道侧只在启动时读一次身份（main.c: tunnel_read_sn），所以这里必须明说。 */
    json_object_object_add(d, "restart_required", json_object_new_boolean(1));
    return rsp_data(d, out, cap, out_len);
}

/* ============================ gps.status（S2.5） ============================ */

/* double 字段：NA（NAN）写成 JSON **null**。
 * **不能拿 0 顶替** —— 纬度 0 / 经度 0 是几内亚湾里的一个真实位置，"未知"与
 * "在赤道"在界面上必须分得开（这也是 `iv_gps.h` 用 NAN 做哨兵的原因）。
 * 另：json-c 序列化 NAN 会产出非法 JSON（`NaN`），所以只在有效时才建对象。 */
static void add_dbl_or_null(json_object *o, const char *k, double v)
{
    add_or_null(o, k, !isnan(v), json_object_new_double(v));
}

static int m_gps_status(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    const iv_gps_t *g;
    iv_gps_fix_t    f;
    json_object    *d;
    json_object    *link;
    json_object    *fx;
    json_object    *tm;
    json_object    *st;
    const char     *path = NULL;
    int             fd   = -1;
    uint64_t        last;
    uint64_t        now;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);
    g = iv_agent_gps(ctx->agent);
    if (g == NULL || iv_gps_fix(g, &f) != IV_OK)
        return rsp_err(IV_ESTATE, "gps unavailable", out, cap, out_len);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;

    /* 链路：串口开没开、收了多少字节、多久之前解析出过语句。
     * 现场第一眼要看的正是这三个数：`rx_bytes == 0` 是线/波特率问题，
     * 有字节但 `valid == false` 是天线/遮蔽问题，两者处置完全不同。 */
    link = json_object_new_object();
    if (link != NULL) {
        int lrc = iv_agent_gps_link(ctx->agent, &path, &fd);

        json_object_object_add(link, "path",
                               json_object_new_string((path != NULL) ? path : ""));
        json_object_object_add(link, "open", json_object_new_boolean(fd >= 0));
        json_object_object_add(link, "rc", json_object_new_int(lrc));
        json_object_object_add(link, "rx_bytes",
                               json_object_new_int64(
                                   (int64_t)iv_agent_gps_rx_bytes(ctx->agent)));
        last = iv_agent_gps_last_parse_ms(ctx->agent);
        now  = iv_clock_monotonic_ms();
        if (last == 0u || now < last)
            json_object_object_add(link, "age_ms", NULL); /* 从未解析出语句 */
        else
            json_object_object_add(link, "age_ms",
                                   json_object_new_int64((int64_t)(now - last)));
        json_object_object_add(d, "link", link);
    }

    fx = json_object_new_object();
    if (fx != NULL) {
        json_object_object_add(fx, "valid", json_object_new_boolean(f.valid != 0));
        add_dbl_or_null(fx, "lat", f.latitude);
        add_dbl_or_null(fx, "lon", f.longitude);
        add_dbl_or_null(fx, "alt", f.altitude_m);
        add_dbl_or_null(fx, "speed_kph", f.speed_kph);
        add_dbl_or_null(fx, "course_deg", f.course_deg);
        add_dbl_or_null(fx, "hdop", f.hdop);
        json_object_object_add(fx, "fix_quality", json_object_new_int(f.fix_quality));
        json_object_object_add(fx, "satellites", json_object_new_int(f.satellites));
        json_object_object_add(d, "fix", fx);
    }

    /* UTC（来自 RMC/ZDA/GGA 的合并结果；`have=0` 时下面各字段无意义，前端不显示） */
    tm = json_object_new_object();
    if (tm != NULL) {
        json_object_object_add(tm, "have", json_object_new_boolean(f.have_time != 0));
        json_object_object_add(tm, "year", json_object_new_int(f.year));
        json_object_object_add(tm, "month", json_object_new_int(f.month));
        json_object_object_add(tm, "day", json_object_new_int(f.day));
        json_object_object_add(tm, "hour", json_object_new_int(f.hour));
        json_object_object_add(tm, "minute", json_object_new_int(f.minute));
        json_object_object_add(tm, "second", json_object_new_int(f.second));
        json_object_object_add(tm, "usec", json_object_new_int64((int64_t)f.usec));
        json_object_object_add(d, "utc", tm);
    }

    /* 解析统计：把"没数据"和"数据来了但解析不了"分开（校验错 ≠ 没收到） */
    st = json_object_new_object();
    if (st != NULL) {
        json_object_object_add(st, "sentences_ok",
                               json_object_new_int64((int64_t)g->stats.sentences_ok));
        json_object_object_add(st, "checksum_bad",
                               json_object_new_int64((int64_t)g->stats.checksum_bad));
        json_object_object_add(st, "malformed",
                               json_object_new_int64((int64_t)g->stats.malformed));
        json_object_object_add(st, "too_long",
                               json_object_new_int64((int64_t)g->stats.too_long));
        json_object_object_add(st, "noise_bytes",
                               json_object_new_int64((int64_t)g->stats.noise_bytes));
        json_object_object_add(st, "fix_updates",
                               json_object_new_int64((int64_t)g->stats.fix_updates));
        json_object_object_add(st, "time_sync_ok",
                               json_object_new_int64((int64_t)g->stats.time_sync_ok));
        json_object_object_add(st, "time_sync_rejected",
                               json_object_new_int64(
                                   (int64_t)g->stats.time_sync_rejected));
        json_object_object_add(d, "stats", st);
    }
    return rsp_data(d, out, cap, out_len);
}

/* ============================ cellular.status ============================ */

/* 数值 + "未知值"哨兵 → JSON。未知就输出 **null**，不输出哨兵数值也不输出 0：
 * 前端的"—"只应该来自 null；把 -32768 或 99 透给页面，迟早被当成真值显示。 */
static void add_int_or_null(json_object *o, const char *k, int v, int unknown)
{
    if (o == NULL || k == NULL)
        return;
    if (v == unknown) {
        json_object_object_add(o, k, NULL);
        return;
    }
    json_object_object_add(o, k, json_object_new_int(v));
}

/* 三态布尔：1/0 之外的任何值（含 -1 "未知"）都输出 null。 */
static void add_tri_or_null(json_object *o, const char *k, int v)
{
    if (o == NULL || k == NULL)
        return;
    if (v == 1)
        json_object_object_add(o, k, json_object_new_boolean(1));
    else if (v == 0)
        json_object_object_add(o, k, json_object_new_boolean(0));
    else
        json_object_object_add(o, k, NULL);
}

/* 4G 模组状态（只读）。
 *
 * 输出分块的理由：现场排查的顺序就是**从链路到业务**——
 *   link（AT 口通不通）→ module（是什么模块）→ sim（卡在不在）
 *   → signal（有没有信号）→ reg（注册上没有）→ cell（驻留在哪个小区）
 *   → data（有没有 IP），最后 usable + reason 给结论。
 * 这七层**任何一层都可能是断点**，挤成一个平表就分不清"没信号"与"没插卡"
 * （2026-10-19 板端实测正是后者：有信号、模块正常、SIM 没插）。
 *
 * `usable` 与 `reason` 由 iv_cellular 算好（四层与），本层只搬运 ——
 * 判定只允许有一处实现。
 */
static int m_cell_status(const iv_ctl_ctx_t *ctx, char *out, size_t cap,
                         size_t *out_len)
{
    const iv_cell_t *c;
    iv_cell_snap_t   s;
    json_object     *d;
    json_object     *link;
    json_object     *mod;
    json_object     *sim;
    json_object     *sig;
    json_object     *reg;
    json_object     *cell;
    json_object     *data;
    const char      *path = NULL;
    int              fd   = -1;
    uint64_t         rx   = 0;
    int              lrc;
    uint64_t         now;

    if (ctx->agent == NULL)
        return rsp_err(IV_ESTATE, "agent unavailable", out, cap, out_len);
    c = iv_agent_cell(ctx->agent);
    if (c == NULL)
        return rsp_err(IV_ESTATE, "cellular unavailable", out, cap, out_len);
    iv_cell_snapshot(c, &s);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;

    link = json_object_new_object();
    if (link != NULL) {
        lrc = iv_agent_cell_link(ctx->agent, &path, &fd, &rx);
        json_object_object_add(link, "path",
                               json_object_new_string((path != NULL) ? path : ""));
        json_object_object_add(link, "open", json_object_new_boolean(fd >= 0));
        json_object_object_add(link, "rc", json_object_new_int(lrc));
        json_object_object_add(link, "rx_bytes", json_object_new_int64((int64_t)rx));
        json_object_object_add(link, "valid", json_object_new_boolean(s.valid != 0));
        json_object_object_add(link, "ok_rounds",
                               json_object_new_int((int32_t)s.ok_rounds));
        json_object_object_add(link, "fail_rounds",
                               json_object_new_int((int32_t)s.fail_rounds));
        json_object_object_add(link, "cmd_timeouts",
                               json_object_new_int((int32_t)s.cmd_timeouts));
        json_object_object_add(link, "at_err", json_object_new_int(s.at_err));
        now = iv_clock_monotonic_ms();
        if (s.last_ok_ms == 0u || now < s.last_ok_ms)
            json_object_object_add(link, "age_ms", NULL); /* 从未成功过 */
        else
            json_object_object_add(link, "age_ms",
                                   json_object_new_int64((int64_t)(now - s.last_ok_ms)));
        json_object_object_add(d, "link", link);
    }

    mod = json_object_new_object();
    if (mod != NULL) {
        add_str_or_null(mod, "model", (s.model[0] != '\0') ? s.model : NULL);
        add_str_or_null(mod, "firmware", (s.firmware[0] != '\0') ? s.firmware : NULL);
        json_object_object_add(d, "module", mod);
    }

    sim = json_object_new_object();
    if (sim != NULL) {
        add_tri_or_null(sim, "present", s.sim_present);
        add_str_or_null(sim, "state", (s.sim_state[0] != '\0') ? s.sim_state : NULL);
        add_str_or_null(sim, "iccid", (s.iccid[0] != '\0') ? s.iccid : NULL);
        add_str_or_null(sim, "imsi", (s.imsi[0] != '\0') ? s.imsi : NULL);
        json_object_object_add(d, "sim", sim);
    }

    sig = json_object_new_object();
    if (sig != NULL) {
        add_int_or_null(sig, "csq", s.csq, 99);
        add_int_or_null(sig, "dbm", s.dbm, IV_CELL_UNKNOWN);
        add_int_or_null(sig, "level", s.level, -1);
        add_int_or_null(sig, "rsrp", s.rsrp, IV_CELL_UNKNOWN);
        add_int_or_null(sig, "rsrq", s.rsrq, IV_CELL_UNKNOWN);
        add_int_or_null(sig, "sinr", s.sinr, IV_CELL_UNKNOWN);
        json_object_object_add(d, "signal", sig);
    }

    reg = json_object_new_object();
    if (reg != NULL) {
        add_int_or_null(reg, "status", s.cereg_stat, -1);
        add_str_or_null(reg, "text", (s.reg_text[0] != '\0') ? s.reg_text : NULL);
        add_str_or_null(reg, "operator",
                        (s.operator_name[0] != '\0') ? s.operator_name : NULL);
        add_str_or_null(reg, "act", (s.act[0] != '\0') ? s.act : NULL);
        add_int_or_null(reg, "mcc", s.mcc, IV_CELL_UNKNOWN);
        add_int_or_null(reg, "mnc", s.mnc, IV_CELL_UNKNOWN);
        json_object_object_add(d, "reg", reg);
    }

    cell = json_object_new_object();
    if (cell != NULL) {
        add_int_or_null(cell, "tac", s.tac, IV_CELL_UNKNOWN);
        if (s.cell_id == 0u)
            json_object_object_add(cell, "cell_id", NULL);
        else
            json_object_object_add(cell, "cell_id",
                                   json_object_new_int64((int64_t)s.cell_id));
        add_int_or_null(cell, "pci", s.pci, IV_CELL_UNKNOWN);
        add_int_or_null(cell, "band", s.band, IV_CELL_UNKNOWN);
        add_int_or_null(cell, "earfcn", s.earfcn, IV_CELL_UNKNOWN);
        json_object_object_add(d, "cell", cell);
    }

    data = json_object_new_object();
    if (data != NULL) {
        add_tri_or_null(data, "attached", s.attached);
        /* 三态：1 有上下文 / 0 明确没有 / -1 没读到（ECM 常态）⇒ null，不编值 */
        add_tri_or_null(data, "active", s.pdp_active);
        add_str_or_null(data, "ip", (s.pdp_ip[0] != '\0') ? s.pdp_ip : NULL);
        add_str_or_null(data, "apn", (s.apn[0] != '\0') ? s.apn : NULL);
        add_str_or_null(data, "dns1", (s.dns1[0] != '\0') ? s.dns1 : NULL);
        add_str_or_null(data, "dns2", (s.dns2[0] != '\0') ? s.dns2 : NULL);
        json_object_object_add(d, "data", data);
    }

    json_object_object_add(d, "usable", json_object_new_boolean(s.usable != 0));
    add_str_or_null(d, "reason", (s.reason[0] != '\0') ? s.reason : NULL);

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
    if (ctx->store != NULL) {
        char sd[40];

        (void)snprintf(sd, sizeof sd, "wall=%lld", (long long)ts);
        (void)iv_store_audit_add(ctx->store, "system.time", sd, 0,
                                 (int64_t)time(NULL));
    }
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
    /* S9 延伸：高风险操作审计（重启本体不可逆，先留痕再执行）。 */
    if (ctx->store != NULL)
        (void)iv_store_audit_add(ctx->store, "system.reboot", "confirm", 0,
                                 (int64_t)time(NULL));
    (void)ctx->do_reboot(ctx->reboot_arg);
    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "accepted", json_object_new_boolean(1));
    return rsp_data(d, out, cap, out_len);
}

/* ============================ 恢复出厂 / 擦除存储 ============================ */

/* 解析 `erase`：可以是单个字符串 `"media"`、字符串数组 `["media","log"]`，
 * 也可以不给（＝只恢复配置，不删任何数据）。未知名字直接 `IV_EINVAL` ——
 * 拼错一个词就静默不清，比报错更坏。 */
static int parse_erase(json_object *p, unsigned *mask)
{
    json_object *v = NULL;
    int          i, n;

    *mask = 0u;
    if (p == NULL || !json_object_object_get_ex(p, "erase", &v) || v == NULL)
        return IV_OK;

    if (json_object_is_type(v, json_type_string)) {
        unsigned bit = iv_factory_erase_bit(json_object_get_string(v));

        if (bit == 0u)
            return IV_EINVAL;
        *mask = bit;
        return IV_OK;
    }
    if (json_object_is_type(v, json_type_array)) {
        n = (int)json_object_array_length(v);
        if (n > 3)
            return IV_EINVAL;
        for (i = 0; i < n; i++) {
            json_object *e    = json_object_array_get_idx(v, i);
            unsigned     bit;

            if (e == NULL || !json_object_is_type(e, json_type_string))
                return IV_EINVAL;
            bit = iv_factory_erase_bit(json_object_get_string(e));
            if (bit == 0u)
                return IV_EINVAL;
            *mask |= bit;
        }
        return IV_OK;
    }
    return IV_EINVAL;
}

static int m_factory_reset(const iv_ctl_ctx_t *ctx, json_object *p,
                           char *out, size_t cap, size_t *out_len)
{
    iv_factory_cfg_t    fc;
    iv_factory_report_t rep;
    json_object        *v       = NULL;
    json_object        *d;
    json_object        *arr;
    unsigned            mask = 0u;
    int                 confirm = 0;
    int                 rc;
    unsigned            bit;

    /* 高风险操作：与 system.reboot 同一道 confirmed 门（不可撤销，必须显式确认） */
    if (p != NULL && json_object_object_get_ex(p, "confirm", &v) && v != NULL)
        confirm = json_object_get_boolean(v);
    if (!confirm)
        return rsp_err(IV_EPROTO, "confirm required", out, cap, out_len);

    rc = parse_erase(p, &mask);
    if (rc != IV_OK)
        return rsp_err(IV_EINVAL, "bad erase (media|log|db)", out, cap, out_len);

    memset(&fc, 0, sizeof(fc));
    fc.data_root  = ctx->factory_data_root;
    fc.media_root = ctx->factory_media_root;

    rc = iv_factory_reset(ctx->cfg, ctx->store, &fc, mask, &rep);
    if (rc == IV_ENOTSUP)
        return rsp_err(IV_ENOTSUP, "erase target not wired", out, cap, out_len);
    if (rc != IV_OK)
        return rsp_err(rc, (rep.cfg_saved != 0) ? "reset partly done" : "reset failed",
                       out, cap, out_len);

    if (ctx->store != NULL) {
        char ad[80];

        (void)snprintf(ad, sizeof ad, "erase=0x%x cfg=%u entries=%lu db=%lld", mask,
                       rep.cfg_changed, rep.entries_removed, (long long)rep.db_rows_removed);
        (void)iv_store_audit_add(ctx->store, "sys.factory_reset", ad, 0,
                                 (int64_t)time(NULL));
    }

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "cfg_changed", json_object_new_int((int)rep.cfg_changed));
    json_object_object_add(d, "version",
                           json_object_new_int64((int64_t)rep.cfg_version));
    json_object_object_add(d, "entries_removed",
                           json_object_new_int64((int64_t)rep.entries_removed));
    json_object_object_add(d, "db_rows_removed",
                           json_object_new_int64(rep.db_rows_removed));
    arr = json_object_new_array();
    if (arr != NULL) {
        for (bit = IV_FACTORY_ERASE_MEDIA; bit <= IV_FACTORY_ERASE_DB; bit <<= 1) {
            if ((mask & bit) != 0u) {
                const char *nm = iv_factory_erase_name(bit);

                json_object_array_add(arr, json_object_new_string((nm != NULL) ? nm : "?"));
            }
        }
        json_object_object_add(d, "erase", arr);
    }
    /* Web 用这个字段决定"要不要把口令重置 + 强制改密"：恢复出厂必然为真 */
    json_object_object_add(d, "password_reset_required", json_object_new_boolean(1));
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

/*
 * 查询某一路是否在录。
 *
 * ============================ D-10：三种"没在录"必须分开 ============================
 * 原实现把 ops 的任何非 IV_OK 都当业务失败整体回 `{"ok":false,"e":<rc>}`，于是
 * `IV_ECONN(-22)`（media 进程没起 / 桥连不上）到了页面上等于"参数错"，而错误码
 * 表里 -22 的语义是"连不上"—— 两处都对不上，现场只会往摄像机配置上去查。
 *
 * 现在的口径（三种case分开）：
 *   · 没接 ops（`ctx->media == NULL`）→ `IV_ENOTSUP`：**"能调但没接"**，与 media
 *     完全没编进本固件是同一件事，必须与下面的运行时状态分开（见 iv_ctl.h 的
 *     ctx 注释，这条口径全模块一致）。
 *   · 接上了但桥查不到（`IV_ECONN` / `IV_ESTATE`）→ **`ok:true` ＋ `recording:false`
 *     ＋ `known:false` ＋ `reason`**：查不到不等于出错，更不等于参数错。
 *     `recording:false` 取**保守值**（不谎称在录）；`known:false` 才是真话——
 *     "我们不知道"，页面据此显示"未获取"而不是"未在录"。
 *   · 其它 rc 仍当业务失败（真出错要暴露）。
 *   · `id` 越界仍 `IV_EINVAL`，且**根本不去调 ops**（先校验再动作）。
 */
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
    if (rc != IV_OK && rc != IV_ECONN && rc != IV_ESTATE)
        return rsp_err(rc, "record status failed", out, cap, out_len);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "id", json_object_new_int((int)id));
    json_object_object_add(d, "recording",
                           json_object_new_boolean((rc == IV_OK && rec) ? 1 : 0));
    json_object_object_add(d, "known", json_object_new_boolean(rc == IV_OK ? 1 : 0));
    if (rc != IV_OK) {
        /* 文案只描述**这一层知道的事实**，不替 media 猜原因（"进程没起"和
         * "端口没监听"在 ctl 这层分不出来，猜错会把人带偏）。 */
        json_object_object_add(d, "reason", json_object_new_string(
            (rc == IV_ECONN) ? "media bridge unreachable" : "media not ready"));
    }
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

/* ============================ tunnel.status / enable / disable（M8-S8.3） ============================ */

/*
 * 这一组方法**只碰闸门，不碰进程**：开通道＝渲染 `frpc.toml` + 置标记，真正起 frpc 的
 * 是 `scripts/monitor.sh` 的守护段（模块层不该 fork 外部进程，那会让单测变成"起真
 * 进程"）。响应里也**绝不出现钥匙**：secretKey 只落在 0600 的配置文件里，令牌连读都不读。
 *
 * 时间用 `time(NULL)`：本模块不是被测对象，装配层才是；`iv_tunnel` 自己坚持"时间由
 * 调用方注入"，这里就是那个调用方。
 */

/* 服务集合 → JSON 字符串数组（渲染失败的名跳过，不因此整条失败） */
static json_object *svc_arr(const iv_tunnel_svc_t *svc, unsigned n)
{
    json_object *arr = json_object_new_array();
    unsigned     i;
    char         s[IV_TUNNEL_SVC_MAX];

    if (arr == NULL)
        return NULL;
    for (i = 0u; i < n; i++) {
        if (iv_tunnel_svc_format(&svc[i], s, sizeof s) != IV_OK)
            continue;
        json_object_array_add(arr, json_object_new_string(s));
    }
    return arr;
}

static int m_tunnel_status(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    iv_tunnel_t        *t = ctx->tunnel;
    iv_tunnel_stat_t    st;
    iv_tunnel_session_t last;
    json_object        *d;
    json_object        *s;
    uint64_t            now = (uint64_t)time(NULL);
    uint32_t            caps;

    if (t == NULL)
        return IV_ENOTSUP;

    caps = iv_tunnel_caps(t);
    iv_tunnel_stat(t, &st);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "on", json_object_new_boolean(
                                   iv_tunnel_state(t) == IV_TUNNEL_ON ? 1 : 0));
    json_object_object_add(d, "remain_s", json_object_new_int(
                                  (int)iv_tunnel_remain_s(t, now)));
    json_object_object_add(d, "ttl_s", json_object_new_int((int)t->ttl_s));
    json_object_object_add(d, "svcs", svc_arr(t->svc, t->n_svc));
    json_object_object_add(d, "caps", json_object_new_int((int)caps));
    /* 能力位打成名字，前端不必记位号；ENABLED＝"身份/服务端/映射/落点全就位" */
    json_object_object_add(d, "enabled", json_object_new_boolean(
                                   (caps & IV_TUNNEL_CAP_ENABLED) != 0u ? 1 : 0));
    json_object_object_add(d, "sn", json_object_new_string(t->sn));
    json_object_object_add(d, "server", json_object_new_string(t->server_addr));
    json_object_object_add(d, "server_port", json_object_new_int((int)t->server_port));
    json_object_object_add(d, "conf", json_object_new_string(t->conf_path));
    json_object_object_add(d, "flag", json_object_new_string(t->flag_path));

    s = json_object_new_object();
    if (s != NULL) {
        json_object_object_add(s, "opens", json_object_new_int64((int64_t)st.opens));
        json_object_object_add(s, "denies", json_object_new_int64((int64_t)st.denies));
        json_object_object_add(s, "expires", json_object_new_int64((int64_t)st.expires));
        json_object_object_add(s, "uptime_s", json_object_new_int64((int64_t)st.uptime_s));
        json_object_object_add(d, "stat", s);
    }

    if (iv_tunnel_last_session(t, &last) == IV_OK) {
        s = json_object_new_object();
        if (s != NULL) {
            json_object_object_add(s, "open_s", json_object_new_int64((int64_t)last.open_s));
            json_object_object_add(s, "close_s", json_object_new_int64((int64_t)last.close_s));
            json_object_object_add(s, "ttl_s", json_object_new_int((int)last.ttl_s));
            json_object_object_add(s, "secs", json_object_new_int64(
                                          (int64_t)(last.close_s - last.open_s)));
            json_object_object_add(s, "stop", json_object_new_string(
                                          iv_tunnel_stop_reason_name((int)last.stop_reason)));
            json_object_object_add(s, "svcs", svc_arr(last.svc, last.n_svc));
            json_object_object_add(d, "last", s);
        }
    }

    return rsp_data(d, out, cap, out_len);
}

/* 解析 `svcs` 数组：白名单外的名字、重复项、数量越界一律拒（不静默忽略）。 */
static int parse_svcs(json_object *p, iv_tunnel_svc_t *svcs, unsigned *n)
{
    json_object *arr = NULL;
    int          cnt;
    int          i;

    if (p == NULL || !json_object_object_get_ex(p, "svcs", &arr) || arr == NULL ||
        !json_object_is_type(arr, json_type_array))
        return IV_EINVAL;
    cnt = json_object_array_length(arr);
    if (cnt <= 0 || (unsigned)cnt > IV_TUNNEL_PROXIES_MAX)
        return IV_ERANGE;
    for (i = 0; i < cnt; i++) {
        json_object *e = json_object_array_get_idx(arr, i);
        const char  *s;
        unsigned     k;

        if (e == NULL || !json_object_is_type(e, json_type_string))
            return IV_EINVAL;
        s = json_object_get_string(e);
        if (s == NULL || iv_tunnel_svc_parse(s, strlen(s), &svcs[i]) != IV_OK)
            return IV_EINVAL;
        /* 重复项：同一服务开两次只会让 frpc 报重名，不如当场说清楚 */
        for (k = 0u; k < (unsigned)i; k++) {
            if (svcs[k].kind == svcs[i].kind && svcs[k].index == svcs[i].index)
                return IV_EINVAL;
        }
    }
    *n = (unsigned)cnt;
    return IV_OK;
}

static int m_tunnel_enable(const iv_ctl_ctx_t *ctx, json_object *p, char *out, size_t cap,
                           size_t *out_len)
{
    iv_tunnel_t         *t = ctx->tunnel;
    iv_tunnel_svc_t      svcs[IV_TUNNEL_PROXIES_MAX];
    unsigned             n = 0u;
    int64_t              ttl = 0;
    iv_tunnel_reason_t   why = IV_TUNNEL_OK;
    json_object         *d;
    int                  rc;

    if (t == NULL)
        return IV_ENOTSUP;
    rc = parse_svcs(p, svcs, &n);
    if (rc == IV_EINVAL)
        return rsp_err(IV_EINVAL, "svcs must be a non-empty array of service names",
                       out, cap, out_len);
    if (rc == IV_ERANGE)
        return rsp_err(IV_ERANGE, "too many services", out, cap, out_len);

    /* 有效期：显式 `ttl` > 配置 `sys.tunnel.ttl_s` > 编译期默认 3600。 */
    if (param_i64(p, "ttl", &ttl) != IV_OK || ttl <= 0) {
        int64_t v = 0;

        if (ctx->cfg != NULL &&
            iv_config_get_int(ctx->cfg, "sys.tunnel.ttl_s", &v) == IV_OK && v > 0)
            ttl = v;
        else
            ttl = (int64_t)IV_TUNNEL_TTL_DEF;
    }

    rc = iv_tunnel_enable(t, svcs, n, (uint32_t)ttl, (uint64_t)time(NULL), &why);
    if (rc != IV_OK)
        return rsp_err(rc, iv_tunnel_reason_name(why), out, cap, out_len);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "on", json_object_new_boolean(1));
    json_object_object_add(d, "ttl_s", json_object_new_int((int)t->ttl_s));
    json_object_object_add(d, "remain_s", json_object_new_int(
                                  (int)iv_tunnel_remain_s(t, (uint64_t)time(NULL))));
    json_object_object_add(d, "svcs", svc_arr(t->svc, t->n_svc));
    json_object_object_add(d, "note", json_object_new_string(
        "frpc 由 scripts/monitor.sh 按标记文件拉起；到期后标记被清，进程随之收掉"));
    return rsp_data(d, out, cap, out_len);
}

static int m_tunnel_disable(const iv_ctl_ctx_t *ctx, char *out, size_t cap, size_t *out_len)
{
    iv_tunnel_t *t = ctx->tunnel;
    json_object *d;
    int          rc;

    if (t == NULL)
        return IV_ENOTSUP;
    rc = iv_tunnel_disable(t, (uint64_t)time(NULL), IV_TUNNEL_STOP_LOCAL);
    if (rc != IV_OK)
        return rsp_err(rc, iv_tunnel_reason_name(IV_TUNNEL_ESTATE), out, cap, out_len);

    d = json_object_new_object();
    if (d == NULL)
        return IV_ENOMEM;
    json_object_object_add(d, "on", json_object_new_boolean(0));
    json_object_object_add(d, "stop", json_object_new_string("local"));
    json_object_object_add(d, "note", json_object_new_string(
        "配置文件保留（0600），只清标记；monitor 下一圈停掉 frpc"));
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
        rc = m_snapshot(ctx, out, out_cap, out_len);
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
    else if (strcmp(method, "gps.status") == 0)
        rc = m_gps_status(ctx, out, out_cap, out_len);
    else if (strcmp(method, "cellular.status") == 0)
        rc = m_cell_status(ctx, out, out_cap, out_len);
    else if (strcmp(method, "board.status") == 0)
        rc = m_board_status(ctx, out, out_cap, out_len);
    else if (strcmp(method, "netthr.status") == 0)
        rc = m_netthr_status(ctx, out, out_cap, out_len);
    else if (strcmp(method, "relay.set") == 0)
        rc = m_relay_set(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "device.identity.set") == 0)
        rc = m_device_identity_set(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "camera.search") == 0)
        rc = m_camera_search(ctx, out, out_cap, out_len);
    else if (strcmp(method, "camera.test") == 0)
        rc = m_camera_test(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "system.time") == 0)
        rc = m_system_time(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "system.reboot") == 0)
        rc = m_system_reboot(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "sys.factory_reset") == 0)
        rc = m_factory_reset(ctx, params, out, out_cap, out_len);
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
    else if (strcmp(method, "tunnel.status") == 0)
        rc = m_tunnel_status(ctx, out, out_cap, out_len);
    else if (strcmp(method, "tunnel.enable") == 0)
        rc = m_tunnel_enable(ctx, params, out, out_cap, out_len);
    else if (strcmp(method, "tunnel.disable") == 0)
        rc = m_tunnel_disable(ctx, out, out_cap, out_len);
    else
        rc = IV_ENOTSUP; /* **不写 out**，由调用方静默丢弃 */

    if (req != NULL)
        json_object_put(req);
    return rc;
}
