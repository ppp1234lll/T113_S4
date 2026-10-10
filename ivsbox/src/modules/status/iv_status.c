/*
 * 采集板状态镜像实现（功能开发计划 M2-S2.6）
 *
 * 设计口径与协议事实见 include/ivsbox/iv_status.h 文件头与架构 §18.2。
 *
 * JSON 提取：先校验完整的扁平对象，再提取固定键；不处理嵌套和转义。
 * 全量应答事务式替换镜像，缺失字段失效；坏应答保留上一份镜像。
 *
 * 单写者：装配层（Reactor 单线程）调入口，内部零锁。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ivsbox/iv_frame.h" /* IV_FRAME_CMD_DOOR / IV_FRAME_CMD_EVENT */
#include "ivsbox/iv_status.h"

/* ---- 极简 JSON 提取 ---- */

/* 找到 `"<key>"`（允许多余空白紧邻）后跳到冒号，再跳过空白，读一个数。
 * 返回 1=成功 / 0=未找到。out 出错或不存在时不被改动。 */
static int json_get_double(const char *json, const char *key, double *out)
{
    char pat[24];
    const char *p;
    char *endp;

    if (json == NULL || key == NULL || out == NULL)
        return 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (p == NULL) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p < '0' && *p != '-' && *p != '.') return 0;
    *out = strtod(p, &endp);
    if (endp == p) return 0;
    return 1;
}

static int json_get_long(const char *json, const char *key, long *out)
{
    char pat[24];
    const char *p;
    char *endp;

    if (json == NULL || key == NULL || out == NULL)
        return 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (p == NULL) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p < '0' && *p != '-') return 0;
    *out = strtol(p, &endp, 10);
    if (endp == p) return 0;
    return 1;
}

/* 抓字符串值（双引号包围，可含负号开头的数字文本；只取一段可打印文本） */
static int json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pat[24];
    const char *p, *q;
    size_t n;

    if (json == NULL || key == NULL || out == NULL || out_size == 0) return 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (p == NULL) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    q = p;
    while (*q && *q != '"') q++;
    n = (size_t)(q - p);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

/* 本协议应答仅有扁平键值：先验完整结构，再做固定键提取。 */
static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static int json_flat_complete(const char *json)
{
    const char *p = json_skip_ws(json);
    char *endp;

    if (*p++ != '{') return 0;
    p = json_skip_ws(p);
    if (*p == '}') return *json_skip_ws(p + 1) == '\0';
    for (;;) {
        if (*p++ != '"') return 0;
        while (*p != '\0' && *p != '"') {
            if (*p == '\\') return 0; /* 固定键不使用转义 */
            p++;
        }
        if (*p++ != '"') return 0;
        p = json_skip_ws(p);
        if (*p++ != ':') return 0;
        p = json_skip_ws(p);
        if (*p == '"') {
            p++;
            while (*p != '\0' && *p != '"') {
                if (*p == '\\') return 0;
                p++;
            }
            if (*p++ != '"') return 0;
        } else {
            if (*p != '-' && (*p < '0' || *p > '9')) return 0;
            (void)strtod(p, &endp);
            if (endp == p) return 0;
            p = endp;
        }
        p = json_skip_ws(p);
        if (*p == '}') return *json_skip_ws(p + 1) == '\0';
        if (*p++ != ',') return 0;
        p = json_skip_ws(p);
    }
}

/* ---- 公开 API ---- */

void iv_status_init(iv_status_t *st)
{
    if (st == NULL) return;
    memset(st, 0, sizeof(*st));
}

void iv_status_handle_query(iv_status_t *st, const uint8_t *json, uint16_t len)
{
    char buf[1024];
    iv_status_t next;
    unsigned relay_seen = 0, chv_seen = 0, cha_seen = 0;
    unsigned power_seen = 0, elec_seen = 0;
    double d;
    long l;
    int i;

    if (st == NULL || json == NULL || len == 0) return;
    /* 全量应答不能截断后当成完整快照；坏应答保留上一份镜像。 */
    if (len >= sizeof(buf) || memchr(json, '\0', len) != NULL) return;
    memcpy(buf, json, len);
    buf[len] = '\0';
    if (!json_flat_complete(buf)) return;

    memset(&next, 0, sizeof(next));
    if (json_get_double(buf, "V",  &d)) { next.data.V = (float)d; next.valid.V = 1; }
    if (json_get_double(buf, "A",  &d)) { next.data.A = (float)d; next.valid.A = 1; }
    if (json_get_double(buf, "H",  &d)) { next.data.H = (float)d; next.valid.H = 1; }
    if (json_get_double(buf, "T",  &d)) { next.data.T = (float)d; next.valid.T = 1; }
    if (json_get_long  (buf, "DS", &l)) { next.data.DS = (int32_t)l; next.valid.DS = 1; }
    if (json_get_long  (buf, "P",  &l)) { next.data.P  = (int32_t)l; next.valid.P  = 1; }
    if (json_get_long  (buf, "SPD",&l)) { next.data.SPD= (int32_t)l; next.valid.SPD= 1; }
    if (json_get_long  (buf, "PA", &l)) { next.data.PA = (int32_t)l; next.valid.PA = 1; }
    if (json_get_long  (buf, "PV", &l)) { next.data.PV = (int32_t)l; next.valid.PV = 1; }
    if (json_get_string(buf, "APOWER", next.data.APOWER, sizeof(next.data.APOWER))) {
        /* "已填"由首字节非零判定 */
    }
    if (json_get_string(buf, "AKW",    next.data.AKW,    sizeof(next.data.AKW))) {
        /* 同上 */
    }

    if (json_get_long(buf, "hv", &l)) { next.data.hv = (int32_t)l; next.valid.hv = 1; }
    if (json_get_long(buf, "lv", &l)) { next.data.lv = (int32_t)l; next.valid.lv = 1; }
    if (json_get_long(buf, "ov", &l)) { next.data.ov = (int32_t)l; next.valid.ov = 1; }
    if (json_get_long(buf, "tu", &l)) { next.data.tu = (int32_t)l; next.valid.tu = 1; }
    if (json_get_long(buf, "tl", &l)) { next.data.tl = (int32_t)l; next.valid.tl = 1; }
    if (json_get_long(buf, "hu", &l)) { next.data.hu = (int32_t)l; next.valid.hu = 1; }
    if (json_get_long(buf, "hl", &l)) { next.data.hl = (int32_t)l; next.valid.hl = 1; }
    if (json_get_long(buf, "sl", &l)) { next.data.sl = (int32_t)l; next.valid.sl = 1; }
    if (json_get_long(buf, "ld", &l)) { next.data.ld = (int32_t)l; next.valid.ld = 1; }

    /* 继电器（3 个，键为 RELAY1/2/3 / CHV1/2/3 / CHA1/2/3 / POWER1/2/3 / ELEC1/2/3） */
    for (i = 0; i < 3; i++) {
        char k[16];
        snprintf(k, sizeof(k), "RELAY%d", i + 1);
        if (json_get_long(buf, k, &l)) { next.data.RELAY[i] = (int32_t)l; relay_seen++; }
        snprintf(k, sizeof(k), "CHV%d", i + 1);
        if (json_get_double(buf, k, &d)) { next.data.CHV[i] = (float)d; chv_seen++; }
        snprintf(k, sizeof(k), "CHA%d", i + 1);
        if (json_get_double(buf, k, &d)) { next.data.CHA[i] = (float)d; cha_seen++; }
        snprintf(k, sizeof(k), "POWER%d", i + 1);
        if (json_get_double(buf, k, &d)) { next.data.POWER[i] = (float)d; power_seen++; }
        snprintf(k, sizeof(k), "ELEC%d", i + 1);
        if (json_get_double(buf, k, &d)) { next.data.ELEC[i] = (float)d; elec_seen++; }
    }
    next.valid.RELAY = relay_seen == 3;
    next.valid.CHV = chv_seen == 3;
    next.valid.CHA = cha_seen == 3;
    next.valid.POWER = power_seen == 3;
    next.valid.ELEC = elec_seen == 3;
    /* 没有任何已识别字段时视为坏应答，保留上次有效镜像。 */
    if (!(next.valid.V || next.valid.A || next.valid.H || next.valid.T ||
          next.valid.DS || next.valid.P || next.valid.SPD || next.valid.PA ||
          next.valid.PV || next.valid.hv || next.valid.lv || next.valid.ov ||
          next.valid.tu || next.valid.tl || next.valid.hu || next.valid.hl ||
          next.valid.sl || next.valid.ld || next.data.APOWER[0] ||
          next.data.AKW[0] || relay_seen || chv_seen || cha_seen ||
          power_seen || elec_seen))
        return;
    *st = next;
}

void iv_status_handle_upstream(uint8_t cmd, const uint8_t *data, uint16_t len, void *arg)
{
    iv_status_t *st = (iv_status_t *)arg;
    char buf[256];

    if (st == NULL) return;
    if (cmd == IV_FRAME_CMD_DOOR) {
        /* data = 0x01；门状态统一映射 DS=2（开） */
        st->data.DS = 2;
        st->valid.DS = 1;
        return;
    }
    if (cmd != IV_FRAME_CMD_EVENT || data == NULL || len == 0) return;
    if (len >= sizeof(buf)) len = (uint16_t)(sizeof(buf) - 1);
    memcpy(buf, data, len);
    buf[len] = '\0';

    /* 0xC2 通用形如 {"key":"mcuEvent","<TAG>":<value>} —— 只关心 TAG
     * 后的那一个数值，按 TAG 覆盖对应字段。 */
    if (strstr(buf, "\"mcuEvent\"") == NULL) return;
    {
        char tag[8] = {0};
        char val[32] = {0};
        const char *p = strstr(buf, "\"mcuEvent\"");
        if (p == NULL) return;
        p += strlen("\"mcuEvent\"");
        while (*p && *p != '}') {
            const char *colon, *comma, *e;
            size_t n;
            while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n' || *p == '\t') p++;
            if (*p == '}') break;
            colon = strchr(p, ':');
            if (colon == NULL) break;
            n = (size_t)(colon - p - 2);
            if (n >= sizeof(tag)) n = sizeof(tag) - 1;
            {
                const char *q = p + 1;
                size_t k;
                for (k = 0; k < n && *q != '"'; k++, q++) tag[k] = *q;
                tag[k] = '\0';
            }
            p = colon + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                e = p;
                while (*e && *e != '"') e++;
                n = (size_t)(e - p);
                if (n >= sizeof(val)) n = sizeof(val) - 1;
                memcpy(val, p, n);
                val[n] = '\0';
                p = (*e) ? e + 1 : e;
            } else {
                e = p;
                while (*e && *e != ',' && *e != '}') e++;
                n = (size_t)(e - p);
                if (n >= sizeof(val)) n = sizeof(val) - 1;
                memcpy(val, p, n);
                val[n] = '\0';
                p = e;
            }
            /* 按 TAG 映射（对齐 §18.2 事件 TAG 表） */
            if      (strcmp(tag, "DS") == 0)   { st->data.DS  = (int32_t)atol(val); st->valid.DS  = 1; }
            else if (strcmp(tag, "P") == 0)    { st->data.P   = (int32_t)atol(val); st->valid.P   = 1; }
            else if (strcmp(tag, "SPD") == 0)  { st->data.SPD = (int32_t)atol(val); st->valid.SPD = 1; }
            else if (strcmp(tag, "T") == 0)    { st->data.T   = (float)atof(val);  st->valid.T   = 1; }
            else if (strcmp(tag, "H") == 0)    { st->data.H   = (float)atof(val);  st->valid.H   = 1; }
            else if (strcmp(tag, "PA") == 0)   { st->data.PA  = (int32_t)atol(val); st->valid.PA  = 1; }
            else if (strcmp(tag, "PV") == 0)   { st->data.PV  = (int32_t)atol(val); st->valid.PV  = 1; }
            /* 其余 TAG（OV/OCPS/MIU/WATER/TS）由应用层按需映射，参见架构 §18.2 */
            (void)val;
            (void)n;
            (void)e;
            comma = strchr(p, ',');
            if (comma == NULL) break;
            p = comma + 1;
        }
    }
}

/* 装配层把这两个函数直接塞进 iv_link_init 的回调位即可。
 * 但本期 S2.6 还在 iv_link 之外单独存在（避免 iv_link 引入新依赖），
 * 故提供"返回回调指针"的写法。 */

/* ---- 快照 ---- */

/* 整数 / 浮点写入辅助：失败时不动目标 */
static int appendf(char **p, char *end, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (p == NULL || *p == NULL || end == NULL) return 0;
    va_start(ap, fmt);
    n = vsnprintf(*p, (size_t)(end - *p), fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    if (n >= end - *p) { *p = end; return 0; } /* 缓冲耗尽：截断，调用方按 ERANGE */
    *p += n;
    return 1;
}

int iv_status_format(const iv_status_t *st, char *buf, size_t buf_size)
{
    char *p = buf, *end;
    int n;

    if (st == NULL) return IV_EINVAL;
    if (buf == NULL) return IV_EINVAL;
    if (buf_size == 0) return IV_ERANGE;
    end = buf + buf_size;

    if (st->valid.V)    appendf(&p, end, "V=%.1f ", st->data.V);
    if (st->valid.A)    appendf(&p, end, "A=%.3f ", st->data.A);
    if (st->valid.T)    appendf(&p, end, "T=%.1f ", st->data.T);
    if (st->valid.H)    appendf(&p, end, "H=%.1f ", st->data.H);
    if (st->valid.DS)   appendf(&p, end, "DS=%ld ", (long)st->data.DS);
    if (st->valid.P)    appendf(&p, end, "P=%ld ", (long)st->data.P);
    if (st->valid.SPD)  appendf(&p, end, "SPD=%ld ", (long)st->data.SPD);
    if (st->valid.PA)   appendf(&p, end, "PA=%ld ", (long)st->data.PA);
    if (st->valid.PV)   appendf(&p, end, "PV=%ld ", (long)st->data.PV);
    if (st->data.APOWER[0])
        appendf(&p, end, "APOWER=%s ", st->data.APOWER);
    if (st->data.AKW[0])
        appendf(&p, end, "AKW=%s ",    st->data.AKW);
    if (st->valid.hv)   appendf(&p, end, "hv=%ld ", (long)st->data.hv);
    if (st->valid.lv)   appendf(&p, end, "lv=%ld ", (long)st->data.lv);
    if (st->valid.ov)   appendf(&p, end, "ov=%ld ", (long)st->data.ov);
    if (st->valid.tu)   appendf(&p, end, "tu=%ld ", (long)st->data.tu);
    if (st->valid.tl)   appendf(&p, end, "tl=%ld ", (long)st->data.tl);
    if (st->valid.hu)   appendf(&p, end, "hu=%ld ", (long)st->data.hu);
    if (st->valid.hl)   appendf(&p, end, "hl=%ld ", (long)st->data.hl);
    if (st->valid.sl)   appendf(&p, end, "sl=%ld ", (long)st->data.sl);
    if (st->valid.ld)   appendf(&p, end, "ld=%ld ", (long)st->data.ld);

    if (buf_size > 0) {
        if (p < end) *p = '\0'; else buf[buf_size - 1] = '\0';
    }
    n = (int)(p - buf);
    if (n == (int)buf_size) return IV_ERANGE;
    return n;
}

/* ---- 回调适配 ----
 *
 * iv_link 三个上行的回调本模块只消费两条：
 *   - on_upstream(cmd, data, len)         ← 0xC1/0xC2 上报
 *   - on_done(IV_OK, cmd, data, len)      ← 0xE1/0xF1/0xD1/0xD2 应答
 *
 * 0xE1 全量应答对镜像而言是"全量刷新"，0xF1/0xD1/0xD2 应答内容由应用层确认
 * 语义（重发、幂等等），**镜像层不消费** —— 故 `on_done` 里只对 cmd==0xE1
 * 转发到 handle_query，其余 cmd 一律丢弃（不影响镜像正确性）。
 *
 * 为避免与 iv_link 互相依赖、且让单测可以直接调"已适配好的回调"，提供
 * 静态"取回调指针"接口；装配层只需把 `iv_status_t*` 装到 user arg 上。
 */
static void on_done_query_only(int rc, uint8_t cmd, const uint8_t *data,
                                uint16_t len, void *arg)
{
    iv_status_t *st = (iv_status_t *)arg;
    if (st == NULL) return;
    if (rc == IV_OK && cmd == IV_FRAME_CMD_QUERY && data != NULL && len != 0)
        iv_status_handle_query(st, data, len);
}

iv_link_upstream_cb iv_status_on_upstream(void) { return iv_status_handle_upstream; }
iv_link_done_cb     iv_status_on_done(void)     { return on_done_query_only; }

/* 让单测/装配层拿到"上层外壳"（无 arg 概念），单测直接调内部 API。 */

int iv_status_snapshot_json(const iv_status_t *st, char *buf, size_t buf_size)
{
    char *p = buf, *end;
    int n;
    int first = 1;

    if (st == NULL) return IV_EINVAL;
    if (buf == NULL) return IV_EINVAL;
    if (buf_size == 0) return IV_ERANGE;
    end = buf + buf_size;

    if (!appendf(&p, end, "{")) return IV_ERANGE;
    if (st->valid.V)  { appendf(&p, end, "%s\"V\":%.1f",    first?"":",", st->data.V); first = 0; }
    if (st->valid.A)  { appendf(&p, end, "%s\"A\":%.3f",    first?"":",", st->data.A); first = 0; }
    if (st->valid.T)  { appendf(&p, end, "%s\"T\":%.1f",    first?"":",", st->data.T); first = 0; }
    if (st->valid.H)  { appendf(&p, end, "%s\"H\":%.1f",    first?"":",", st->data.H); first = 0; }
    if (st->valid.DS) { appendf(&p, end, "%s\"DS\":%ld",    first?"":",", (long)st->data.DS); first = 0; }
    if (st->valid.P)  { appendf(&p, end, "%s\"P\":%ld",     first?"":",", (long)st->data.P);  first = 0; }
    if (st->valid.SPD){ appendf(&p, end, "%s\"SPD\":%ld",   first?"":",", (long)st->data.SPD);first = 0; }
    if (st->valid.PA) { appendf(&p, end, "%s\"PA\":%ld",    first?"":",", (long)st->data.PA); first = 0; }
    if (st->valid.PV) { appendf(&p, end, "%s\"PV\":%ld",    first?"":",", (long)st->data.PV); first = 0; }
    if (st->data.APOWER[0])
        { appendf(&p, end, "%s\"APOWER\":\"%s\"", first?"":",", st->data.APOWER); first = 0; }
    if (st->data.AKW[0])
        { appendf(&p, end, "%s\"AKW\":\"%s\"",    first?"":",", st->data.AKW);    first = 0; }
    if (st->valid.hv) { appendf(&p, end, "%s\"hv\":%ld",    first?"":",", (long)st->data.hv); first = 0; }
    if (st->valid.lv) { appendf(&p, end, "%s\"lv\":%ld",    first?"":",", (long)st->data.lv); first = 0; }
    if (st->valid.ov) { appendf(&p, end, "%s\"ov\":%ld",    first?"":",", (long)st->data.ov); first = 0; }
    if (st->valid.tu) { appendf(&p, end, "%s\"tu\":%ld",    first?"":",", (long)st->data.tu); first = 0; }
    if (st->valid.tl) { appendf(&p, end, "%s\"tl\":%ld",    first?"":",", (long)st->data.tl); first = 0; }
    if (st->valid.hu) { appendf(&p, end, "%s\"hu\":%ld",    first?"":",", (long)st->data.hu); first = 0; }
    if (st->valid.hl) { appendf(&p, end, "%s\"hl\":%ld",    first?"":",", (long)st->data.hl); first = 0; }
    if (st->valid.sl) { appendf(&p, end, "%s\"sl\":%ld",    first?"":",", (long)st->data.sl); first = 0; }
    if (st->valid.ld) { appendf(&p, end, "%s\"ld\":%ld",    first?"":",", (long)st->data.ld); first = 0; }
    if (st->valid.RELAY) {
        int i;
        for (i = 0; i < 3; i++) {
            appendf(&p, end, "%s\"RELAY%d\":%ld", first?"":",", i + 1,
                    (long)st->data.RELAY[i]);
            first = 0;
        }
    }
    if (st->valid.CHV) {
        int i;
        for (i = 0; i < 3; i++) {
            appendf(&p, end, "%s\"CHV%d\":%.1f", first?"":",", i + 1,
                    st->data.CHV[i]);
            first = 0;
        }
    }
    if (!appendf(&p, end, "}")) return IV_ERANGE;
    (void)first;
    n = (int)(p - buf);
    if (n >= (int)buf_size) return IV_ERANGE;
    return n;
}
