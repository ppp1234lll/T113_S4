/*
 * IVSBox 配置模块实现（功能开发计划 M1-S8）
 *
 * 为什么 json-c 只出现在这个文件：
 *   计划 §S8「分层」要求"解析与存储落 `src/modules/config/`；`libivcore` /
 *   `libivhal` 里不得出现 json-c 符号"。本文件属模块层（libivmodules），是
 *   **唯一** include <json-c/json.h> 的地方；对外头文件不暴露任何 json-c 类型。
 *   用到的 json-c API 已按板端 sysroot 头（0.13.1）逐条核对声明，不使用
 *   0.15 才新增的接口（计划 §S8「实现时必读的三个坑」第 1 条）。
 *
 * 为什么不用 malloc 存自己的状态：
 *   句柄走静态池（IV_CFG_HANDLES 个槽）、条目走定长数组。配置份数与键数都是
 *   编译期数得清的（架构 §10.2 共 7 份，本步只落 2 份），动态分配只多出
 *   "分配失败路径"和碎片。json-c 解析时库内部会分配，那部分不受本约束。
 *
 * 抗断电写盘（F5）：写 `<path>.tmp` → fsync 文件 → rename → fsync 目录。
 *   只 fsync 文件不够：rename 这个目录项变更本身也要落盘，否则断电后可能仍是
 *   旧文件（计划 §S8 坑 3）。
 *
 * 单线程契约（与 iv_reactor 同一条纪律）：全部接口只在 reactor 线程内调用，
 * 因此下面两块解析/渲染缓冲直接做成文件级 static，不做重入保护 —— 同头文件的
 * "线程与调用约定"。
 */
#include "ivsbox/iv_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <json-c/json.h> /* 仅模块层允许依赖 json-c（core/hal 不允许） */

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

#define CFG_MOD "cfg"

/* 元数据键名：业务键不得以 '_' 开头（该前缀保留给元数据） */
#define CFG_META_KEY "_meta"

/* 读入文件大小上限：配置是小文件，超限即异常（防误读大文件吃内存） */
#define CFG_FILE_MAX (64u * 1024u)

/* 交叉字段"回退默认值再试"的轮数上限，防死循环 */
#define CFG_CROSSFIX_MAX 4u

/* 单线程契约下的共用缓冲（见文件头） */
static char s_read_buf[CFG_FILE_MAX];
static char s_render_buf[CFG_FILE_MAX];

/* ---------------------------------------------------------------------------
 * 内置默认值表（F4）
 *
 * 字段名一律有文档依据，不自己编：
 *   - `net.wan.fail_n` / `ok_n` / `hold_s`：计划 §S3.3「参数（fail_n/ok_n/hold_s）进配置」；
 *   - `net.wan.mode = wireless-first`：架构 §7.2「无线优先、故障切有线」；
 *   - `platform.proto.reconnect_max_s = 60`：计划 §S2.4「1s→2s→…→60s 封顶」；
 *   - 探活三项：架构 §7.1「统一探活引擎」的间隔／超时／连续失败判据。
 * **`platform.proto.host` / `port` / `device_id` 出厂为空/0（＝未配置）**，等
 * M2-S2.4 平台协议成形后再填；`net.wan.primary_if` / `backup_if` 同理由 §18.3
 * 待确认的真实网卡名决定。这两组是**样板值**，不是已冻结的运行参数。
 *
 * `enum_csv` 非空时对字符串键做枚举校验（"范围"类的字符串形态）。
 * `imin > imax` 表示该整数键不查范围。
 * ------------------------------------------------------------------------- */
typedef struct {
    const char   *name;     /* 配置名（"platform" / "network"） */
    const char   *key;      /* 点分键 */
    iv_cfg_type_t type;
    const char   *sdef;     /* 字符串默认值；布尔用 "true"/"false" */
    const char   *enum_csv; /* 非空则限定取值集合 */
    int64_t       idef;
    int64_t       imin;
    int64_t       imax;
} cfg_default_t;

static const cfg_default_t s_defaults[] = {
    /* ---- platform.json：平台协议（M2-S2.4） ---- */
    { "platform", "platform.proto.host", IV_CFG_T_STR, "", NULL, 0, 0, 0 },
    { "platform", "platform.proto.port", IV_CFG_T_INT, "", NULL, 0, 0, 65535 },
    { "platform", "platform.proto.device_id", IV_CFG_T_STR, "", NULL, 0, 0, 0 },
    { "platform", "platform.proto.heartbeat_s", IV_CFG_T_INT, "", NULL, 30, 5, 3600 },
    { "platform", "platform.proto.reconnect_max_s", IV_CFG_T_INT, "", NULL, 60, 1, 600 },

    /* ---- network.json：双 WAN 与统一探活（架构 §7.1/§7.2，计划 §S3.2/§S3.3） ---- */
    { "network", "net.wan.mode", IV_CFG_T_STR, "wireless-first", "wireless-first,wired-first", 0, 0, 0 },
    { "network", "net.wan.primary_if", IV_CFG_T_STR, "", NULL, 0, 0, 0 },
    { "network", "net.wan.backup_if", IV_CFG_T_STR, "", NULL, 0, 0, 0 },
    { "network", "net.wan.fail_n", IV_CFG_T_INT, "", NULL, 3, 1, 100 },
    { "network", "net.wan.ok_n", IV_CFG_T_INT, "", NULL, 5, 1, 100 },
    { "network", "net.wan.hold_s", IV_CFG_T_INT, "", NULL, 30, 1, 3600 },
    { "network", "net.probe.interval_ms", IV_CFG_T_INT, "", NULL, 5000, 500, 600000 },
    { "network", "net.probe.timeout_ms", IV_CFG_T_INT, "", NULL, 2000, 100, 60000 },
    { "network", "net.probe.max_fail", IV_CFG_T_INT, "", NULL, 3, 1, 100 },
};
#define CFG_DEFAULT_N (sizeof(s_defaults) / sizeof(s_defaults[0]))

/* ---------------------------------------------------------------------------
 * 内存条目与句柄
 * ------------------------------------------------------------------------- */

typedef struct {
    char          key[IV_CFG_KEY_MAX];
    iv_cfg_type_t type;
    uint8_t       origin; /* iv_cfg_origin_t */
    union {
        char    s[IV_CFG_STR_MAX];
        int64_t i;
        int     b;
    } v;
} cfg_item_t;

struct iv_config {
    int  used;
    char dir[IV_CFG_PATH_MAX];
    char name[IV_CFG_NAME_MAX];
    char path[IV_CFG_PATH_MAX];
    char tmppath[IV_CFG_PATH_MAX];

    uint32_t schema;
    uint64_t version;
    int      loaded;
    int      crc_mismatch;
    int      invalid;
    int      dirty;

    unsigned   n_items;
    cfg_item_t items[IV_CFG_ITEMS_MAX];

    int ifd; /* inotify fd，-1 = 未创建 */
    int wd;

    iv_cfg_notify_fn subs_fn[IV_CFG_SUBS_MAX];
    void            *subs_user[IV_CFG_SUBS_MAX];
    unsigned         n_subs;
};

/* 事务式重载用的状态快照（整份内存状态） */
typedef struct {
    cfg_item_t items[IV_CFG_ITEMS_MAX];
    unsigned   n_items;
    uint32_t   schema;
    uint64_t   version;
    int        loaded;
    int        crc_mismatch;
    int        invalid;
    int        dirty;
} cfg_snapshot_t;

static struct iv_config s_pool[IV_CFG_HANDLES];

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

/* 有界字符串拷贝（不用 snprintf，免得 -Wformat-truncation 在 -Werror 下断编译） */
static void str_copy(char *out, size_t cap, const char *src)
{
    size_t n;

    if (cap == 0u)
        return;
    n = strlen(src);
    if (n > cap - 1u)
        n = cap - 1u;
    memcpy(out, src, n);
    out[n] = '\0';
}

/* dir + "/" + name；放不下返回 -1（不截断，避免拼出别的路径） */
static int path_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);

    if (dl + 1u + nl + 1u > cap)
        return -1;
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, name, nl);
    out[dl + 1u + nl] = '\0';
    return 0;
}

static const cfg_default_t *default_find(const char *name, const char *key)
{
    size_t i;

    for (i = 0; i < CFG_DEFAULT_N; i++) {
        if (strcmp(s_defaults[i].name, name) == 0 && strcmp(s_defaults[i].key, key) == 0)
            return &s_defaults[i];
    }
    return NULL;
}

/* 字符串是否落在逗号分隔的枚举集合里（enum_csv 为空视为不限） */
static int enum_ok(const cfg_default_t *d, const char *val)
{
    const char *p, *q;
    size_t      n;

    if (d == NULL || d->enum_csv == NULL || d->enum_csv[0] == '\0')
        return 1;
    for (p = d->enum_csv; *p != '\0'; p = q + 1) {
        q = strchr(p, ',');
        n = (q != NULL) ? (size_t)(q - p) : strlen(p);
        if (strlen(val) == n && strncmp(p, val, n) == 0)
            return 1;
        if (q == NULL)
            break;
    }
    return 0;
}

/* 配置名合法性：非空、不超长、不含 '/' 与 ".."（防拼出目录外的路径） */
static int name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    if (strlen(name) >= (size_t)IV_CFG_NAME_MAX)
        return 0;
    if (strchr(name, '/') != NULL || strstr(name, "..") != NULL)
        return 0;
    return 1;
}

/* 点分键合法性：段数/段长/总长在限内，不允许空段（"a..b"）与首尾点 */
static int key_ok(const char *key)
{
    const char *p = key;
    size_t      depth = 1u;
    size_t      seglen = 0u;

    if (key == NULL || key[0] == '\0' || key[0] == '.')
        return 0;
    if (strlen(key) >= (size_t)IV_CFG_KEY_MAX)
        return 0;
    for (; *p != '\0'; p++) {
        if (*p == '.') {
            if (seglen == 0u || seglen >= (size_t)IV_CFG_SEG_MAX)
                return 0;
            seglen = 0u;
            if (++depth > (size_t)IV_CFG_DEPTH_MAX)
                return 0;
        } else if (++seglen >= (size_t)IV_CFG_SEG_MAX) {
            return 0;
        }
    }
    return seglen > 0u;
}

static int item_cmp(const void *a, const void *b)
{
    return strcmp(((const cfg_item_t *)a)->key, ((const cfg_item_t *)b)->key);
}

static cfg_item_t *item_find(const iv_config_t *c, const char *key)
{
    unsigned i;

    for (i = 0; i < c->n_items; i++) {
        if (strcmp(c->items[i].key, key) == 0)
            return (cfg_item_t *)&c->items[i];
    }
    return NULL;
}

/* 取/建条目；表满返回 NULL。只建空壳，type/值由调用方填。 */
static cfg_item_t *item_touch(iv_config_t *c, const char *key)
{
    cfg_item_t *it = item_find(c, key);

    if (it != NULL)
        return it;
    if (c->n_items >= (unsigned)IV_CFG_ITEMS_MAX)
        return NULL;
    it = &c->items[c->n_items];
    c->n_items++;
    memset(it, 0, sizeof(*it));
    memcpy(it->key, key, strlen(key) + 1u);
    return it;
}

static void item_drop(iv_config_t *c, const char *key)
{
    unsigned i;

    for (i = 0; i < c->n_items; i++) {
        if (strcmp(c->items[i].key, key) == 0) {
            memmove(&c->items[i], &c->items[i + 1u],
                    (c->n_items - i - 1u) * sizeof(cfg_item_t));
            c->n_items--;
            return;
        }
    }
}

/* 把某键写回**内置默认值**；该键不在默认表里则删除条目 */
static void item_reset_to_default(iv_config_t *c, const char *key)
{
    const cfg_default_t *d = default_find(c->name, key);
    cfg_item_t          *it;

    if (d == NULL) {
        item_drop(c, key);
        return;
    }
    it = item_touch(c, key);
    if (it == NULL)
        return;
    it->type   = d->type;
    it->origin = (uint8_t)IV_CFG_FROM_DEFAULT;
    if (d->type == IV_CFG_T_STR)
        str_copy(it->v.s, sizeof(it->v.s), d->sdef);
    else if (d->type == IV_CFG_T_INT)
        it->v.i = d->idef;
    else
        it->v.b = (strcmp(d->sdef, "true") == 0) ? 1 : 0;
}

/* key 是否占着某个已知默认键的**祖先路径**（如 "net" 之于 "net.wan.mode"）。
 * 默认表只列叶子，所以这种键一定是"把子树写成了标量"的形态冲突。 */
static int is_default_parent(const char *name, const char *key)
{
    size_t i, kl = strlen(key);

    for (i = 0; i < CFG_DEFAULT_N; i++) {
        const cfg_default_t *d = &s_defaults[i];

        if (strcmp(d->name, name) != 0)
            continue;
        if (kl < strlen(d->key) && strncmp(d->key, key, kl) == 0 && d->key[kl] == '.')
            return 1;
    }
    return 0;
}

/* 值合法性：已知键查类型/范围/枚举；未知键只认类型（向前兼容，不做业务校验），
 * 但**占着已知键父路径的标量键必须拒绝** —— 否则文件里一句 `"net": 5` 就能让
 * 整棵 `net.*` 默认子树补不进来（补默认时会因前缀冲突被跳过），而热更新还会因为
 * "没发现非法值"而**整份接受**，把运行中的网络配置悄悄清空（直接违反 F6/F7）。
 * 判非法后该键回退默认：默认表里没有它 ⇒ 条目被删除，随后默认子树正常补齐。 */
static int item_check(const iv_config_t *c, const cfg_item_t *it, const char **why)
{
    const cfg_default_t *d = default_find(c->name, it->key);

    if (d == NULL) {
        if (is_default_parent(c->name, it->key)) {
            *why = "scalar where an object is expected";
            return 0;
        }
        return 1;
    }
    if (d->type != it->type) {
        *why = "type mismatch";
        return 0;
    }
    if (d->type == IV_CFG_T_INT && d->imin <= d->imax && (it->v.i < d->imin || it->v.i > d->imax)) {
        *why = "out of range";
        return 0;
    }
    if (d->type == IV_CFG_T_STR && !enum_ok(d, it->v.s)) {
        *why = "not in enum";
        return 0;
    }
    return 1;
}

/*
 * 前缀冲突：一个键是另一个键的父路径（"net" 与 "net.wan.fail_n"）时，写回 JSON
 * 只能二选一 —— 不查就会静默丢掉其中一个。发生条件是"文件里有未知的浅层标量键、
 * 而默认表又要往它下面补字段"，所以必须显式拒绝而不是让它悄悄消失。
 */
static int items_conflict(const iv_config_t *c, const char **k1, const char **k2)
{
    unsigned i, j;

    for (i = 0; i < c->n_items; i++) {
        size_t li = strlen(c->items[i].key);

        for (j = 0; j < c->n_items; j++) {
            if (i == j)
                continue;
            if (strncmp(c->items[i].key, c->items[j].key, li) == 0 &&
                c->items[j].key[li] == '.') {
                *k1 = c->items[i].key;
                *k2 = c->items[j].key;
                return 1;
            }
        }
    }
    return 0;
}

/*
 * 跨字段校验（校验四类里的第 4 类）。本步两条，都是真实业务不变量：
 *   1. 探活超时必须小于探活间隔，否则本轮探测还没结束下一轮就开跑；
 *   2. 双 WAN 主备网卡不得同名（同名等于没有备路）。
 * 失败时回带参与冲突的两个键，供加载路径回退默认、保存路径直接拒绝。
 */
static int cross_check(const iv_config_t *c, const char **k1, const char **k2, const char **why)
{
    const cfg_item_t *ival, *tval, *pif, *bif;

    *k1 = NULL;
    *k2 = NULL;

    ival = item_find(c, "net.probe.interval_ms");
    tval = item_find(c, "net.probe.timeout_ms");
    if (ival != NULL && tval != NULL && ival->type == IV_CFG_T_INT && tval->type == IV_CFG_T_INT &&
        tval->v.i >= ival->v.i) {
        *k1  = "net.probe.timeout_ms";
        *k2  = "net.probe.interval_ms";
        *why = "probe timeout_ms must be < interval_ms";
        return 0;
    }

    pif = item_find(c, "net.wan.primary_if");
    bif = item_find(c, "net.wan.backup_if");
    if (pif != NULL && bif != NULL && pif->type == IV_CFG_T_STR && bif->type == IV_CFG_T_STR &&
        pif->v.s[0] != '\0' && strcmp(pif->v.s, bif->v.s) == 0) {
        *k1  = "net.wan.primary_if";
        *k2  = "net.wan.backup_if";
        *why = "wan primary_if and backup_if must differ";
        return 0;
    }

    return 1;
}

/* 全表校验（保存路径用，严格）：类型/范围/枚举 + 前缀冲突 + 跨字段 */
static int items_check(const iv_config_t *c, const char **bad, const char **why)
{
    unsigned i;

    for (i = 0; i < c->n_items; i++) {
        if (!item_check(c, &c->items[i], why)) {
            *bad = c->items[i].key;
            return 0;
        }
    }
    if (items_conflict(c, bad, why)) {
        *why = "key prefix conflict";
        return 0;
    }
    return 1;
}

/* 条目指纹串（按 key 排序 ⇒ 与 JSON 排版无关）：key=type:value\n */
static size_t items_fingerprint(const iv_config_t *c, char *buf, size_t cap)
{
    cfg_item_t sorted[IV_CFG_ITEMS_MAX];
    size_t     used = 0u;
    unsigned   i;

    memcpy(sorted, c->items, c->n_items * sizeof(cfg_item_t));
    qsort(sorted, c->n_items, sizeof(cfg_item_t), item_cmp);

    for (i = 0; i < c->n_items; i++) {
        char   line[IV_CFG_KEY_MAX + IV_CFG_STR_MAX + 8u];
        size_t n;

        if (sorted[i].type == IV_CFG_T_STR)
            snprintf(line, sizeof(line), "%s=s:%s\n", sorted[i].key, sorted[i].v.s);
        else if (sorted[i].type == IV_CFG_T_INT)
            snprintf(line, sizeof(line), "%s=i:%lld\n", sorted[i].key,
                     (long long)sorted[i].v.i);
        else
            snprintf(line, sizeof(line), "%s=b:%d\n", sorted[i].key, sorted[i].v.b);

        n = strlen(line);
        if (n > cap - used)
            break; /* 截断：指纹退化但不会越界（键数受 ITEMS_MAX 约束，正常走不到） */
        memcpy(buf + used, line, n);
        used += n;
    }
    return used;
}

static uint16_t items_crc(const iv_config_t *c)
{
    /* +8 覆盖每行的 "=s:"/"=i:"/"=b:" 与 "\n"；按上限给足，fingerprint 的
     * 截断分支（见函数内注释）在正常键数下永不触发。 */
    static char buf[IV_CFG_ITEMS_MAX * (IV_CFG_KEY_MAX + IV_CFG_STR_MAX + 8u)];
    size_t      n = items_fingerprint(c, buf, sizeof(buf));

    return iv_crc16_modbus(buf, n, IV_CRC16_MODBUS_SEED_INIT);
}

/* ---------------------------------------------------------------------------
 * F1：解析与加载
 * ------------------------------------------------------------------------- */

/* 把一个 JSON 叶子写进条目表；类型不支持（array/double/null）则丢弃并告警 */
static void flatten_leaf(iv_config_t *c, const char *key, json_object *val, int *invalid)
{
    cfg_item_t *it;
    const char *why = NULL;

    it = item_touch(c, key);
    if (it == NULL) {
        IV_LOG_W(CFG_MOD, "%s: too many keys, '%s' ignored", c->name, key);
        return;
    }

    switch (json_object_get_type(val)) {
    case json_type_string: {
        const char *s = json_object_get_string(val);

        if (s == NULL)
            s = "";
        if (strlen(s) >= (size_t)IV_CFG_STR_MAX) {
            IV_LOG_W(CFG_MOD, "%s: '%s' string too long, rejected", c->name, key);
            (*invalid)++;
            item_reset_to_default(c, key);
            return;
        }
        it->type   = IV_CFG_T_STR;
        it->origin = (uint8_t)IV_CFG_FROM_FILE;
        str_copy(it->v.s, sizeof(it->v.s), s);
        break;
    }
    case json_type_int:
        it->type   = IV_CFG_T_INT;
        it->origin = (uint8_t)IV_CFG_FROM_FILE;
        it->v.i    = (int64_t)json_object_get_int64(val);
        break;
    case json_type_boolean:
        it->type   = IV_CFG_T_BOOL;
        it->origin = (uint8_t)IV_CFG_FROM_FILE;
        it->v.b    = json_object_get_boolean(val) ? 1 : 0;
        break;
    default:
        IV_LOG_W(CFG_MOD, "%s: '%s' has unsupported json type, rejected", c->name, key);
        (*invalid)++;
        item_drop(c, key);
        return;
    }

    /* 文件里的值本身非法 ⇒ 该键回退默认并计数。注意"缺字段"不在此列：那是 F4
     * 的正常补齐，由 fill_missing_defaults 处理，不计入 invalid。 */
    if (!item_check(c, it, &why)) {
        IV_LOG_W(CFG_MOD, "%s: '%s' invalid (%s), fallback to default", c->name, key, why);
        (*invalid)++;
        item_reset_to_default(c, key);
    }
}

/* 递归展平 JSON 对象树；只处理 object，叶子交给 flatten_leaf */
static void flatten(iv_config_t *c, const char *prefix, json_object *obj, unsigned depth,
                    int *invalid)
{
    json_object_object_foreach(obj, key, val)
    {
        char       full[IV_CFG_KEY_MAX];
        size_t     need;
        const char *p = key;

        if (depth > (unsigned)IV_CFG_DEPTH_MAX) {
            IV_LOG_W(CFG_MOD, "%s: '%s' nested too deep, ignored", c->name, key);
            continue;
        }
        if (p[0] == '_' && prefix[0] == '\0')
            continue; /* 顶层元数据键（_meta）不算业务键 */

        if (prefix[0] == '\0') {
            need = strlen(p) + 1u;
            if (need > (size_t)IV_CFG_KEY_MAX) {
                IV_LOG_W(CFG_MOD, "%s: key '%s' too long, ignored", c->name, p);
                continue;
            }
            str_copy(full, sizeof(full), p);
        } else {
            need = strlen(prefix) + 1u + strlen(p) + 1u;
            if (need > (size_t)IV_CFG_KEY_MAX) {
                IV_LOG_W(CFG_MOD, "%s: key '%s.%s' too long, ignored", c->name, prefix, p);
                continue;
            }
            str_copy(full, sizeof(full), prefix);
            full[strlen(prefix)] = '.';
            str_copy(full + strlen(prefix) + 1u, sizeof(full) - strlen(prefix) - 1u, p);
        }

        if (json_object_get_type(val) == json_type_object)
            flatten(c, full, val, depth + 1u, invalid);
        else
            flatten_leaf(c, full, val, invalid);
    }
}

/* 读整个文件到 buf（NUL 结尾）。>=0 为长度，<0 为负的 IV_* 码。 */
static long file_read_all(const char *path, char *buf, size_t cap)
{
    int    fd;
    size_t used = 0u;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        /* 符号链接一律不跟（配置路径被换成链接时宁可退回默认值并告警） */
        if (errno == ELOOP)
            return IV_EAUTH;
        return (errno == ENOENT) ? IV_ENOENT : IV_EIO;
    }

    for (;;) {
        ssize_t n = read(fd, buf + used, cap - 1u - used);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return IV_EIO;
        }
        if (n == 0)
            break;
        used += (size_t)n;
        if (used >= cap - 1u)
            break; /* 超限：交给解析失败处理，不吃更多内存 */
    }
    (void)close(fd);
    buf[used] = '\0';
    return (long)used;
}

/* 从 JSON 文本构建条目表（会先清空）。返回 IV_OK 或负码。 */
static int parse_into(iv_config_t *c, const char *text, unsigned len, uint32_t *schema,
                      uint64_t *version, uint16_t *file_crc, int *has_crc, int *invalid)
{
    struct json_tokener   *tok;
    struct json_object    *root;
    struct json_object    *meta;
    enum json_tokener_error jerr;
    int                    bad = 0;

    *schema   = IV_CFG_SCHEMA_DEFAULT;
    *version  = 0u;
    *file_crc = 0u;
    *has_crc  = 0;

    c->n_items = 0u;

    tok = json_tokener_new();
    if (tok == NULL)
        return IV_ENOMEM;
    root = json_tokener_parse_ex(tok, text, (int)len);
    jerr = json_tokener_get_error(tok);
    json_tokener_free(tok);

    if (jerr != json_tokener_success || root == NULL) {
        if (root != NULL)
            json_object_put(root);
        return IV_ECORRUPT;
    }
    if (json_object_get_type(root) != json_type_object) {
        json_object_put(root);
        return IV_ECORRUPT;
    }

    if (json_object_object_get_ex(root, CFG_META_KEY, &meta) &&
        json_object_get_type(meta) == json_type_object) {
        json_object *v;

        if (json_object_object_get_ex(meta, "schema", &v) &&
            json_object_get_type(v) == json_type_int)
            *schema = (uint32_t)json_object_get_int64(v);
        if (json_object_object_get_ex(meta, "version", &v) &&
            json_object_get_type(v) == json_type_int)
            *version = (uint64_t)json_object_get_int64(v);
        if (json_object_object_get_ex(meta, "crc", &v) &&
            json_object_get_type(v) == json_type_int) {
            *file_crc = (uint16_t)(json_object_get_int64(v) & 0xFFFF);
            *has_crc  = 1;
        }
    }

    flatten(c, "", root, 1u, &bad);
    json_object_put(root);
    *invalid = bad;
    return IV_OK;
}

/* 补齐本配置在默认表里、但内存中没有的键（与前缀冲突的跳过并告警），返回补了几条 */
static int fill_missing_defaults(iv_config_t *c)
{
    size_t i;
    int    filled = 0;

    for (i = 0; i < CFG_DEFAULT_N; i++) {
        const cfg_default_t *d = &s_defaults[i];
        unsigned             j;
        size_t               dl;
        int                  conflict = 0;

        if (strcmp(d->name, c->name) != 0)
            continue;
        if (item_find(c, d->key) != NULL)
            continue;

        /* 已有条目占着它的父路径 ⇒ 补了就会在前缀冲突里丢掉一边，跳过。
         * item_check() 已把这种键判非法并从表里删掉（见 is_default_parent），
         * 所以正常路径走不到这里；保留作防御，免得将来校验放宽后静默丢键。 */
        dl = strlen(d->key);
        for (j = 0; j < c->n_items; j++) {
            size_t lj = strlen(c->items[j].key);

            if (lj < dl && strncmp(d->key, c->items[j].key, lj) == 0 && d->key[lj] == '.') {
                conflict = 1;
                break;
            }
        }
        if (conflict) {
            IV_LOG_W(CFG_MOD, "%s: skip default '%s' (conflicts with existing key)", c->name,
                     d->key);
            continue;
        }
        item_reset_to_default(c, d->key);
        filled++;
    }
    return filled;
}

/* 交叉字段冲突时把参与的两个键回退默认（加载路径用）。返回 1 = 已无冲突。 */
static int cross_fix_by_default(iv_config_t *c)
{
    const char *k1, *k2, *why;
    unsigned    round;

    for (round = 0u; round < CFG_CROSSFIX_MAX; round++) {
        if (cross_check(c, &k1, &k2, &why))
            return 1;
        IV_LOG_W(CFG_MOD, "%s: cross-field conflict (%s), revert '%s'/'%s' to default", c->name,
                 why, k1, k2);
        c->invalid++;
        item_reset_to_default(c, k1);
        item_reset_to_default(c, k2);
    }
    return cross_check(c, &k1, &k2, &why);
}

static void snap_take(const iv_config_t *c, cfg_snapshot_t *s)
{
    memcpy(s->items, c->items, sizeof(s->items));
    s->n_items      = c->n_items;
    s->schema       = c->schema;
    s->version      = c->version;
    s->loaded       = c->loaded;
    s->crc_mismatch = c->crc_mismatch;
    s->invalid      = c->invalid;
    s->dirty        = c->dirty;
}

static void snap_restore(iv_config_t *c, const cfg_snapshot_t *s)
{
    memcpy(c->items, s->items, sizeof(s->items));
    c->n_items      = s->n_items;
    c->schema       = s->schema;
    c->version      = s->version;
    c->loaded       = s->loaded;
    c->crc_mismatch = s->crc_mismatch;
    c->invalid      = s->invalid;
    c->dirty        = s->dirty;
}

/*
 * 从磁盘加载并采用（open 与热重载共用）。
 * 返回 IV_OK / IV_ENOENT / IV_ECORRUPT / IV_EAUTH / IV_EIO / IV_ENOMEM。
 * 成功时 `c->invalid` 被置为**本次加载中因值非法而回退默认的键数**（调用方据此
 * 决定要不要整份拒绝：启动路径接受、重载路径拒绝）。
 */
static int load_from_disk(iv_config_t *c)
{
    long     n;
    uint32_t schema;
    uint64_t version;
    uint16_t fcr;
    int      has_crc = 0;
    int      invalid = 0;
    int      rc;

    n = file_read_all(c->path, s_read_buf, sizeof(s_read_buf));
    if (n < 0)
        return (int)n;
    if (n == 0)
        return IV_ECORRUPT; /* 空文件：按 F4 走默认值 */

    rc = parse_into(c, s_read_buf, (unsigned)n, &schema, &version, &fcr, &has_crc, &invalid);
    if (rc != IV_OK)
        return rc;

    c->schema       = schema;
    c->version      = version;
    c->crc_mismatch = (has_crc && fcr != items_crc(c)) ? 1 : 0;
    if (c->crc_mismatch)
        IV_LOG_W(CFG_MOD, "%s: file crc mismatch (file=0x%04X), accepted with warning", c->name,
                 (unsigned)fcr);

    (void)fill_missing_defaults(c); /* 缺字段是正常补齐，不计入 invalid */
    if (!cross_fix_by_default(c))
        invalid++;
    c->invalid = invalid;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * F5：原子写
 * ------------------------------------------------------------------------- */

/* 把内存条目渲染成 JSON 文本到 s_render_buf；返回长度，负值为错误码 */
static long render_json(const iv_config_t *c, uint64_t version)
{
    json_object *root = json_object_new_object();
    json_object *meta = json_object_new_object();
    cfg_item_t   sorted[IV_CFG_ITEMS_MAX];
    const char  *text;
    size_t       len;
    unsigned     i;
    long         rc = IV_OK;

    if (root == NULL || meta == NULL) {
        if (root != NULL)
            json_object_put(root);
        if (meta != NULL)
            json_object_put(meta);
        return IV_ENOMEM;
    }

    json_object_object_add(meta, "schema", json_object_new_int64((int64_t)c->schema));
    json_object_object_add(meta, "version", json_object_new_int64((int64_t)version));
    json_object_object_add(meta, "crc", json_object_new_int64((int64_t)items_crc(c)));
    json_object_object_add(root, CFG_META_KEY, meta); /* meta 所有权交给 root */

    memcpy(sorted, c->items, c->n_items * sizeof(cfg_item_t));
    qsort(sorted, c->n_items, sizeof(cfg_item_t), item_cmp);

    for (i = 0; i < c->n_items; i++) {
        json_object *cur = root;
        const char  *p   = sorted[i].key;
        char         seg[IV_CFG_SEG_MAX];

        for (;;) {
            const char  *dot = strchr(p, '.');
            json_object *child = NULL;

            if (dot == NULL)
                break;
            memcpy(seg, p, (size_t)(dot - p));
            seg[dot - p] = '\0';

            if (!json_object_object_get_ex(cur, seg, &child) ||
                json_object_get_type(child) != json_type_object) {
                child = json_object_new_object();
                if (child == NULL) {
                    rc = IV_ENOMEM;
                    goto out;
                }
                json_object_object_add(cur, seg, child);
            }
            cur = child;
            p   = dot + 1;
        }

        if (sorted[i].type == IV_CFG_T_STR)
            json_object_object_add(cur, p, json_object_new_string(sorted[i].v.s));
        else if (sorted[i].type == IV_CFG_T_INT)
            json_object_object_add(cur, p, json_object_new_int64(sorted[i].v.i));
        else
            json_object_object_add(cur, p, json_object_new_boolean(sorted[i].v.b ? 1 : 0));
    }

    text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
    if (text == NULL) {
        rc = IV_EFAIL;
        goto out;
    }
    len = strlen(text);
    if (len + 2u > (size_t)CFG_FILE_MAX) {
        rc = IV_ERANGE;
        goto out;
    }
    memcpy(s_render_buf, text, len);
    s_render_buf[len]      = '\n';
    s_render_buf[len + 1u] = '\0';
    rc = (long)(len + 1u);

out:
    json_object_put(root);
    return rc;
}

static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    if (fsync(fd) != 0) {
        (void)close(fd);
        return -1;
    }
    (void)close(fd);
    return 0;
}

/* 抽干进程自己产生的 inotify 事件（保存后调用，避免 F6 把自己的写当外部改动） */
static void watch_drain(iv_config_t *c)
{
    char buf[512];

    if (c->ifd < 0)
        return;
    while (read(c->ifd, buf, sizeof(buf)) > 0)
        ;
}

/* ---------------------------------------------------------------------------
 * 公开 API：F1
 * ------------------------------------------------------------------------- */

iv_config_t *iv_config_open(const char *dir, const char *name)
{
    const char  *d = (dir != NULL && dir[0] != '\0') ? dir : IV_CFG_DIR_DEFAULT;
    iv_config_t *c = NULL;
    unsigned     i;
    int          rc;

    if (!name_ok(name))
        return NULL;

    for (i = 0; i < (unsigned)IV_CFG_HANDLES; i++) {
        if (!s_pool[i].used) {
            c = &s_pool[i];
            break;
        }
    }
    if (c == NULL) {
        IV_LOG_E(CFG_MOD, "no free handle for '%s'", name);
        return NULL;
    }

    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->ifd  = -1;
    c->wd   = -1;
    c->schema = IV_CFG_SCHEMA_DEFAULT;

    if (strlen(d) + 1u > (size_t)IV_CFG_PATH_MAX || path_join(c->dir, sizeof(c->dir), d, "") != 0) {
        memset(c, 0, sizeof(*c));
        c->ifd = -1;
        return NULL;
    }
    /* 去掉 path_join(dir,"") 留下的尾斜杠（根目录 "/" 本身除外） */
    {
        size_t dl = strlen(c->dir);

        if (dl > 1u && c->dir[dl - 1u] == '/')
            c->dir[dl - 1u] = '\0';
    }
    str_copy(c->name, sizeof(c->name), name);

    {
        char   file[IV_CFG_NAME_MAX + 8u]; /* "<name>.json" */
        size_t nl = strlen(name);

        /* 手工拼装而非 snprintf：-Wformat-truncation 在 -Werror 下会因"目标可能
         * 放不下"直接断编译（本文件统一走这个口径）。 */
        memcpy(file, name, nl);
        memcpy(file + nl, ".json", 6u); /* 含结尾 NUL */

        if (path_join(c->path, sizeof(c->path), c->dir, file) != 0) {
            memset(c, 0, sizeof(*c));
            c->ifd = -1;
            return NULL;
        }
        /* 临时文件与目标同目录：rename 必须落在同一文件系统内才是原子的
         * （跨文件系统 rename 直接 EXDEV 失败）。 */
        if (strlen(c->path) + 4u + 1u > (size_t)IV_CFG_PATH_MAX) {
            memset(c, 0, sizeof(*c));
            c->ifd = -1;
            return NULL;
        }
        {
            size_t pl = strlen(c->path);

            memcpy(c->tmppath, c->path, pl);
            memcpy(c->tmppath + pl, ".tmp", 5u); /* 含结尾 NUL */
        }
    }

    /* 目录按需创建：F4 要求"文件不存在也照常启动"，补完默认要有地方落盘 */
    if (mkdir(c->dir, 0755) != 0 && errno != EEXIST)
        IV_LOG_W(CFG_MOD, "mkdir '%s' failed: %s (defaults stay in memory)", c->dir,
                 strerror(errno));

    rc = load_from_disk(c);
    if (rc == IV_OK) {
        c->loaded = 1;
        IV_LOG_I(CFG_MOD, "%s: loaded %u keys (invalid=%d), version=%llu", c->name, c->n_items,
                 c->invalid, (unsigned long long)c->version);
    } else {
        /* 文件不在 / 空 / 坏 / 无权限：一律用内置默认值照常启动 */
        c->loaded       = 0;
        c->crc_mismatch = 0;
        c->invalid      = 0;
        c->version      = 0u;
        c->n_items      = 0u;
        (void)fill_missing_defaults(c);
        (void)cross_fix_by_default(c);
        IV_LOG_W(CFG_MOD, "%s: '%s' unusable (rc=%d), start with %u default keys", c->name,
                 c->path, rc, c->n_items);
    }
    return c;
}

void iv_config_close(iv_config_t *c)
{
    if (c == NULL || !c->used)
        return;
    if (c->ifd >= 0) {
        if (c->wd >= 0)
            (void)inotify_rm_watch(c->ifd, c->wd);
        (void)close(c->ifd);
    }
    memset(c, 0, sizeof(*c));
    c->ifd = -1;
}

/* ---------------------------------------------------------------------------
 * 公开 API：F2
 * ------------------------------------------------------------------------- */

int iv_config_get_str(const iv_config_t *c, const char *key, const char **out)
{
    const cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || out == NULL)
        return IV_EINVAL;
    it = item_find(c, key);
    if (it == NULL)
        return IV_ENOENT;
    if (it->type != IV_CFG_T_STR)
        return IV_EPROTO;
    *out = it->v.s;
    return IV_OK;
}

int iv_config_get_int(const iv_config_t *c, const char *key, int64_t *out)
{
    const cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || out == NULL)
        return IV_EINVAL;
    it = item_find(c, key);
    if (it == NULL)
        return IV_ENOENT;
    if (it->type != IV_CFG_T_INT)
        return IV_EPROTO;
    *out = it->v.i;
    return IV_OK;
}

int iv_config_get_bool(const iv_config_t *c, const char *key, int *out)
{
    const cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || out == NULL)
        return IV_EINVAL;
    it = item_find(c, key);
    if (it == NULL)
        return IV_ENOENT;
    if (it->type != IV_CFG_T_BOOL)
        return IV_EPROTO;
    *out = it->v.b;
    return IV_OK;
}

int iv_config_origin(const iv_config_t *c, const char *key, iv_cfg_origin_t *out)
{
    const cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || out == NULL)
        return IV_EINVAL;
    it = item_find(c, key);
    if (it == NULL)
        return IV_ENOENT;
    *out = (iv_cfg_origin_t)it->origin;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 公开 API：F3
 * ------------------------------------------------------------------------- */

int iv_config_set_str(iv_config_t *c, const char *key, const char *val)
{
    cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || val == NULL || !key_ok(key) || key[0] == '_')
        return IV_EINVAL;
    if (strlen(val) >= (size_t)IV_CFG_STR_MAX)
        return IV_ERANGE;
    it = item_touch(c, key);
    if (it == NULL)
        return IV_EFULL;
    it->type   = IV_CFG_T_STR;
    it->origin = (uint8_t)IV_CFG_FROM_FILE;
    str_copy(it->v.s, sizeof(it->v.s), val);
    c->dirty = 1;
    return IV_OK;
}

int iv_config_set_int(iv_config_t *c, const char *key, int64_t val)
{
    cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || !key_ok(key) || key[0] == '_')
        return IV_EINVAL;
    it = item_touch(c, key);
    if (it == NULL)
        return IV_EFULL;
    it->type   = IV_CFG_T_INT;
    it->origin = (uint8_t)IV_CFG_FROM_FILE;
    it->v.i    = val;
    c->dirty   = 1;
    return IV_OK;
}

int iv_config_set_bool(iv_config_t *c, const char *key, int val)
{
    cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || !key_ok(key) || key[0] == '_')
        return IV_EINVAL;
    it = item_touch(c, key);
    if (it == NULL)
        return IV_EFULL;
    it->type   = IV_CFG_T_BOOL;
    it->origin = (uint8_t)IV_CFG_FROM_FILE;
    it->v.b    = val ? 1 : 0;
    c->dirty   = 1;
    return IV_OK;
}

uint64_t iv_config_version(const iv_config_t *c)
{
    return (c != NULL && c->used) ? c->version : 0u;
}

int iv_config_stat(const iv_config_t *c, iv_cfg_stat_t *out)
{
    unsigned i;

    if (c == NULL || !c->used || out == NULL)
        return IV_EINVAL;
    memset(out, 0, sizeof(*out));
    out->schema       = c->schema;
    out->version      = c->version;
    out->items        = c->n_items;
    out->loaded       = c->loaded;
    out->crc_mismatch = c->crc_mismatch;
    out->dirty        = c->dirty;
    out->invalid      = (uint32_t)c->invalid;
    for (i = 0; i < c->n_items; i++) {
        if (c->items[i].origin == (uint8_t)IV_CFG_FROM_FILE)
            out->from_file++;
        else
            out->from_default++;
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 公开 API：F3 + F5 保存
 * ------------------------------------------------------------------------- */

int iv_config_save(iv_config_t *c, char *detail, size_t detail_cap)
{
    const char *bad = NULL;
    const char *why = NULL;
    const char *k1  = NULL;
    const char *k2  = NULL;
    long        n;
    uint64_t    nver;
    int         fd;

    if (c == NULL || !c->used)
        return IV_EINVAL;
    if (detail != NULL && detail_cap > 0u)
        detail[0] = '\0';

    /* 1) 整体校验：任一条不过，一个字段都不落盘 */
    if (!items_check(c, &bad, &why)) {
        if (detail != NULL && detail_cap > 0u)
            snprintf(detail, detail_cap, "%s: %s", bad, why);
        IV_LOG_E(CFG_MOD, "%s: save refused, '%s': %s", c->name, bad, why);
        return IV_EINVAL;
    }
    if (!cross_check(c, &k1, &k2, &why)) {
        if (detail != NULL && detail_cap > 0u)
            snprintf(detail, detail_cap, "cross-field %s ('%s' vs '%s')", why, k1, k2);
        IV_LOG_E(CFG_MOD, "%s: save refused, cross-field: %s", c->name, why);
        return IV_EINVAL;
    }

    /* 2) 渲染（版本号先算新值，写进文件的就是新版本） */
    nver = c->version + 1u;
    n    = render_json(c, nver);
    if (n < 0)
        return (int)n;

    /* 3) tmp → fsync → rename → fsync(目录) */
    fd = open(c->tmppath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        IV_LOG_E(CFG_MOD, "%s: open tmp failed: %s", c->name, strerror(errno));
        return IV_EIO;
    }
    (void)fchmod(fd, 0644); /* 不受 umask 影响：配置非机密，固定 0644 */
    {
        size_t off = 0u;
        int    rc  = IV_OK;

        while (off < (size_t)n) {
            ssize_t w = write(fd, s_render_buf + off, (size_t)n - off);

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = (errno == ENOSPC) ? IV_ENOSPC : IV_EIO;
                break;
            }
            off += (size_t)w;
        }
        if (rc == IV_OK && fsync(fd) != 0)
            rc = IV_EIO;
        (void)close(fd);
        if (rc != IV_OK) {
            (void)unlink(c->tmppath);
            IV_LOG_E(CFG_MOD, "%s: write tmp failed", c->name);
            return rc;
        }
    }
    if (rename(c->tmppath, c->path) != 0) {
        IV_LOG_E(CFG_MOD, "%s: rename failed: %s", c->name, strerror(errno));
        (void)unlink(c->tmppath);
        return IV_EIO;
    }
    if (fsync_dir(c->dir) != 0)
        IV_LOG_W(CFG_MOD, "%s: fsync dir failed: %s (rename may not be durable)", c->name,
                 strerror(errno));

    c->version = nver;
    c->loaded  = 1;
    c->dirty   = 0;
    c->crc_mismatch = 0;
    watch_drain(c); /* 自己的写不该被 F6 当成外部改动 */
    IV_LOG_I(CFG_MOD, "%s: saved %u keys, version=%llu", c->name, c->n_items,
             (unsigned long long)c->version);

    /* F7：程序主动保存同样要通知依赖方（此时配置一定合法） */
    {
        unsigned i;

        for (i = 0; i < c->n_subs; i++) {
            if (c->subs_fn[i] != NULL)
                c->subs_fn[i](c, c->name, c->subs_user[i]);
        }
    }
    return IV_OK;
}

int iv_config_fill_defaults(iv_config_t *c, int *filled, char *detail, size_t detail_cap)
{
    int n;

    if (c == NULL || !c->used)
        return IV_EINVAL;
    n = fill_missing_defaults(c);
    if (filled != NULL)
        *filled = n;
    if (n > 0)
        c->dirty = 1;
    return iv_config_save(c, detail, detail_cap);
}

/* ---------------------------------------------------------------------------
 * 公开 API：F6 热更新
 * ------------------------------------------------------------------------- */

int iv_config_watch_fd(iv_config_t *c)
{
    int fd, wd, saved;

    if (c == NULL || !c->used)
        return IV_EINVAL;
    if (c->ifd >= 0)
        return c->ifd;

    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        IV_LOG_E(CFG_MOD, "%s: inotify_init1 failed: %s", c->name, strerror(errno));
        return IV_EFAIL;
    }
    /* 必须 watch 目录：原子写走 rename，对旧文件的 watch 第一次保存后就失效
     * （计划 §S8 坑 2）。IN_CLOSE_WRITE 管"原地改写"，IN_MOVED_TO 管 rename。 */
    wd = inotify_add_watch(fd, c->dir, IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) {
        saved = errno;
        (void)close(fd);
        IV_LOG_E(CFG_MOD, "%s: inotify_add_watch '%s' failed: %s", c->name, c->dir,
                 strerror(saved));
        return (saved == ENOENT) ? IV_ENOENT : IV_EFAIL;
    }
    c->ifd = fd;
    c->wd  = wd;
    watch_drain(c); /* 建 watch 后的历史事件不算改动 */
    IV_LOG_I(CFG_MOD, "%s: watching '%s'", c->name, c->dir);
    return c->ifd;
}

int iv_config_watch_poll(iv_config_t *c, uint32_t *reloads)
{
    char  buf[sizeof(struct inotify_event) + IV_CFG_NAME_MAX + 8u];
    char  target[IV_CFG_NAME_MAX + 8u];
    int   hit = 0;
    int   saw = 0;

    if (c == NULL || !c->used)
        return IV_EINVAL;
    if (c->ifd < 0)
        return IV_ESTATE;

    snprintf(target, sizeof(target), "%s.json", c->name);

    for (;;) {
        ssize_t n = read(c->ifd, buf, sizeof(buf));
        char   *p;
        ssize_t left;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break; /* EAGAIN：本轮读干 */
        }
        if (n == 0)
            break;
        saw = 1;

        for (p = buf, left = n; left >= (ssize_t)sizeof(struct inotify_event);) {
            const struct inotify_event *ev = (const struct inotify_event *)p;

            if (ev->len > 0u && strcmp(ev->name, target) == 0)
                hit = 1;
            p += sizeof(struct inotify_event) + ev->len;
            left -= (ssize_t)(sizeof(struct inotify_event) + ev->len);
        }
    }

    if (!saw)
        return IV_EAGAIN;
    if (!hit) {
        if (reloads != NULL)
            *reloads = 0u;
        return IV_OK; /* 目录里有别的文件动了：与本配置无关 */
    }

    /* 事务式重载：先整份解析+校验，全过才替换内存并通知（F7）。
     * 运行期与启动期口径不同：这里只要出现任何一个非法值就整份拒绝、旧值不动。 */
    {
        cfg_snapshot_t snap;
        int            rc;

        snap_take(c, &snap);
        rc = load_from_disk(c);
        if (rc != IV_OK || c->invalid > 0) {
            int badn = c->invalid;

            snap_restore(c, &snap);
            IV_LOG_W(CFG_MOD, "%s: reload rejected (rc=%d, %d invalid key(s)), keep previous",
                     c->name, rc, badn);
            if (reloads != NULL)
                *reloads = 0u;
            return IV_OK;
        }

        {
            unsigned i;

            IV_LOG_I(CFG_MOD, "%s: reloaded %u keys, version=%llu", c->name, c->n_items,
                     (unsigned long long)c->version);
            for (i = 0; i < c->n_subs; i++) {
                if (c->subs_fn[i] != NULL)
                    c->subs_fn[i](c, c->name, c->subs_user[i]);
            }
        }
        if (reloads != NULL)
            *reloads = 1u;
        return IV_OK;
    }
}

/* ---------------------------------------------------------------------------
 * 公开 API：F7 订阅
 * ------------------------------------------------------------------------- */

int iv_config_subscribe(iv_config_t *c, iv_cfg_notify_fn fn, void *user)
{
    if (c == NULL || !c->used || fn == NULL)
        return IV_EINVAL;
    if (c->n_subs >= (unsigned)IV_CFG_SUBS_MAX)
        return IV_EFULL;
    c->subs_fn[c->n_subs]   = fn;
    c->subs_user[c->n_subs] = user;
    c->n_subs++;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 公开 API：F8 密钥分离
 * ------------------------------------------------------------------------- */

int iv_config_secure_read(const char *secure_dir, const char *name, char *out, size_t cap)
{
    char        path[IV_CFG_PATH_MAX];
    struct stat st;
    int         fd;
    size_t      used = 0u;
    int         rc   = IV_OK;

    if (secure_dir == NULL || secure_dir[0] == '\0' || name == NULL || name[0] == '\0' ||
        out == NULL || cap == 0u)
        return IV_EINVAL;
    if (strchr(name, '/') != NULL || strstr(name, "..") != NULL)
        return IV_EINVAL;
    if (path_join(path, sizeof(path), secure_dir, name) != 0)
        return IV_ERANGE;

    /* lstat：符号链接直接拒（不跟着它读到别处去） */
    if (lstat(path, &st) != 0)
        return (errno == ENOENT) ? IV_ENOENT : IV_EIO;
    if (!S_ISREG(st.st_mode))
        return IV_EAUTH;
    if ((st.st_mode & 0777u) != 0600u)
        return IV_EAUTH; /* 组/其他位只要开了一位就拒：0600 是密钥文件的硬要求 */

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return (errno == ELOOP) ? IV_EAUTH : IV_EIO;

    /* 开完再复核一次：防 lstat 与 open 之间被换掉（TOCTOU） */
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0777u) != 0600u) {
        (void)close(fd);
        return IV_EAUTH;
    }

    /* 最多读 cap-1 字节（留 NUL）。读满后再探一个字节：还能读到 ⇒ 文件比缓冲大，
     * 返回 IV_ERANGE。**绝不允许"截断后返回 IV_OK"** —— 截断的令牌会变成一个
     * 看起来合法、实际错误的凭据，比直接报错危险得多。 */
    for (;;) {
        ssize_t n;

        if (used >= cap - 1u) {
            char probe;

            if (read(fd, &probe, 1u) > 0)
                rc = IV_ERANGE;
            break;
        }
        n = read(fd, out + used, cap - 1u - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = IV_EIO;
            break;
        }
        if (n == 0)
            break;
        used += (size_t)n;
    }
    (void)close(fd);
    if (rc != IV_OK)
        return rc;

    out[used] = '\0';
    while (used > 0u && (out[used - 1u] == '\n' || out[used - 1u] == '\r'))
        out[--used] = '\0';
    if (used == 0u)
        return IV_ECORRUPT; /* 空密钥文件按损坏处理，不静默返回空串 */
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 公开 API：默认值查询
 * ------------------------------------------------------------------------- */

int iv_config_default_get(const char *name, const char *key, iv_cfg_type_t *out_type,
                          char *out_text, size_t cap)
{
    const cfg_default_t *d;

    if (name == NULL || key == NULL)
        return IV_EINVAL;
    d = default_find(name, key);
    if (d == NULL)
        return IV_ENOENT;
    if (out_type != NULL)
        *out_type = d->type;
    if (out_text != NULL && cap > 0u) {
        if (d->type == IV_CFG_T_STR)
            str_copy(out_text, cap, d->sdef);
        else if (d->type == IV_CFG_T_INT)
            snprintf(out_text, cap, "%lld", (long long)d->idef);
        else
            snprintf(out_text, cap, "%d", (strcmp(d->sdef, "true") == 0) ? 1 : 0);
    }
    return IV_OK;
}
