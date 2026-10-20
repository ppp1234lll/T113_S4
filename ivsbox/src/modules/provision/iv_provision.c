/*
 * IVSBox 出厂身份与组播工装支撑 —— 实现（M6/S5.5 协议定稿：JSON over 组播）
 * 协议口径、两条已实现指令的语义、以及"为什么不用 json-c"见 iv_provision.h 文件头。
 *
 * 实现上的四条纪律：
 *   1. **字节序显式小端**（记录编解码）：不靠"本机恰好小端"的假设。
 *   2. **解析器只服务这两条报文**：不是通用 JSON 解析器 —— 只按 `info`/`data`
 *      两个对象取键、只取字符串值、键序无关、空白与尾逗号一律跳过。遇到未闭合的
 *      字符串 / 缺对象 / 值超长一律 IV_EPROTO，**不做任何猜测性修复**。
 *   3. **决策与 IO 分离**：`iv_prov_decide()` 是纯函数（无 socket、无文件），
 *      工具只负责"收包→问决策→照做→回包"，单测据此覆盖全部分支。
 *   4. **组包输出严格 JSON**（无尾逗号）：容错是"读"的责任，"写"只写最保守的形态。
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_provision.h"
#include "ivsbox/iv_ret.h"

/* 固定 XOR key（见文件头：只防明文浏览，不是加密） */
static const uint8_t XOR_KEY[4] = { 0x5Au, 0xA5u, 0x3Cu, 0xC3u };

/* --------------------------------------------------------------------------
 * 基础工具
 * -------------------------------------------------------------------------- */
uint32_t iv_prov_checksum(const uint8_t *buf, size_t len)
{
    uint32_t s = 0u;
    size_t   i;

    if (buf == NULL)
        return 0u;
    for (i = 0u; i < len; i++)
        s = (s + (uint32_t)buf[i]) & 0xFFFFFFFFu;
    return s;
}

void iv_prov_xor(uint8_t *buf, size_t len)
{
    size_t i;

    if (buf == NULL)
        return;
    for (i = 0u; i < len; i++)
        buf[i] ^= XOR_KEY[i & 3u];
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void copy_fixed_str(char *dst, size_t dst_cap, const uint8_t *src, size_t src_len)
{
    size_t n = (src_len < dst_cap - 1u) ? src_len : (dst_cap - 1u);

    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* --------------------------------------------------------------------------
 * tid / mac / date
 * -------------------------------------------------------------------------- */
void iv_prov_tid_from_u32(uint32_t devid, char out[IV_PROV_TID_MAX])
{
    if (out == NULL)
        return;
    (void)snprintf(out, IV_PROV_TID_MAX, "%06X", (unsigned)(devid & 0xFFFFFFu));
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int iv_prov_tid_to_u32(const char *tid, uint32_t *out)
{
    uint32_t v = 0u;
    int      i;

    if (tid == NULL || out == NULL)
        return IV_EINVAL;
    for (i = 0; i < 6; i++) {
        int h = hex_val(tid[i]);

        if (h < 0)
            return IV_EINVAL;
        v = (v << 4) | (uint32_t)h;
    }
    if (tid[6] != '\0')
        return IV_EINVAL;
    *out = v;
    return IV_OK;
}

int iv_prov_mac_parse(const char *s, uint8_t mac[6])
{
    size_t i = 0u;
    int    n = 0;

    if (s == NULL || mac == NULL)
        return IV_EINVAL;

    for (; s[i] != '\0'; i++) {
        int  hi;
        int  lo;

        if (s[i] == ':' || s[i] == '-' || s[i] == '.')
            continue;
        hi = hex_val(s[i]);
        if (hi < 0)
            return IV_EINVAL;
        i++;
        if (s[i] == '\0')
            return IV_EINVAL; /* 落单的半字节 */
        lo = hex_val(s[i]);
        if (lo < 0)
            return IV_EINVAL;
        if (n >= 6)
            return IV_EINVAL; /* 多了 */
        mac[n] = (uint8_t)((hi << 4) | lo);
        n++;
    }
    return (n == 6) ? IV_OK : IV_EINVAL;
}

void iv_prov_mac_format(const uint8_t mac[6], char out[IV_PROV_MAC_MAX])
{
    if (mac == NULL || out == NULL)
        return;
    (void)snprintf(out, IV_PROV_MAC_MAX, "%02x:%02x:%02x:%02x:%02x:%02x",
                   (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
                   (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

/* --------------------------------------------------------------------------
 * 终端序列号（平台 `tsn`）
 * --------------------------------------------------------------------------
 * 只认"芯片唯一号"这一路事实，**绝不从 devid/tid 派生** —— 参考实现里 tsn 来自
 * MCU 的 chipid（`start_get_device_id_str()` = "%04X%04X%04X"），tid 来自设备
 * 配置 ID。两者混用会让平台把同一台设备认成两台（或反过来）。
 *
 * 解析一律"找键 → 跳冒号 → 跳空白 → 连取 HEX 到 12 位"，不用 sscanf 的格式串：
 * `/proc/cpuinfo` 是 `Serial\t\t: xxx`、sunxi 是 `sunxi_chipid      : xxx`，
 * 冒号前的空白数量不固定，写死格式串在这块板子上必然踩空一次。
 * -------------------------------------------------------------------------- */
static int hex12_from_file(const char *path, const char *key,
                           char out[IV_PROV_TSN_MAX])
{
    const int want = (int)IV_PROV_TSN_MAX - 1; /* 12 */
    char      line[256];
    FILE     *fp;

    if (path == NULL || key == NULL || out == NULL)
        return 0;
    out[0] = '\0';

    fp = fopen(path, "r");
    if (fp == NULL)
        return 0;

    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        const char *p = strstr(line, key);
        int         n = 0;

        if (p == NULL)
            continue;
        p += strlen(key);
        while (*p != '\0' && *p != ':')
            p++;
        if (*p == ':')
            p++;
        while (*p != '\0' && isspace((unsigned char)*p))
            p++;
        while (hex_val(*p) >= 0 && n < want) {
            out[n] = (char)toupper((unsigned char)*p);
            n++;
            p++;
        }
        out[n] = '\0';
        if (n == want) {
            (void)fclose(fp);
            return 1;
        }
        out[0] = '\0'; /* 这一行的 HEX 不够 12 位，继续找下一行 */
    }
    (void)fclose(fp);
    return 0;
}

int iv_prov_tsn(char out[IV_PROV_TSN_MAX])
{
    const char *path = "/sys/class/sunxi_info/sys_info";
    const char *ov = getenv("IVSBOX_CHIPID_FILE");

    if (out == NULL)
        return 0;
    out[0] = '\0';
    /* 覆盖口只为单测/联调：Host 上没有 sunxi 的 sysfs，真值只能在板上验证。
     * 与 IVSBOX_CELL_DEV / IVSBOX_GPS_DEV 同一口径 —— 产线不设，也不进配置键。 */
    if (ov != NULL && ov[0] != '\0')
        path = ov;

    if (hex12_from_file(path, "sunxi_chipid", out) != 0)
        return 1;
    if (hex12_from_file(path, "Serial", out) != 0)
        return 1;
    if (ov == NULL || ov[0] == '\0') {
        if (hex12_from_file("/proc/cpuinfo", "Serial", out) != 0)
            return 1;
    }
    return 0;
}

void iv_prov_date_now(char out[IV_PROV_DATE_MAX], int with_time)
{
    time_t    t;
    struct tm tmv;

    if (out == NULL)
        return;
    out[0] = '\0';
    t = time(NULL);
    if (t == (time_t)-1 || localtime_r(&t, &tmv) == NULL) {
        (void)snprintf(out, IV_PROV_DATE_MAX, "00000000");
        return;
    }
    (void)snprintf(out, IV_PROV_DATE_MAX, with_time ? "%04d%02d%02d%02d%02d%02d"
                                                    : "%04d%02d%02d",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* --------------------------------------------------------------------------
 * 记录编解码
 * -------------------------------------------------------------------------- */
int iv_prov_id_encode(const iv_prov_id_t *id, uint8_t out[IV_PROV_REC_LEN])
{
    char srv[IV_PROV_SERVER_MAX];

    if (id == NULL || out == NULL)
        return IV_EINVAL;

    memset(out, 0, IV_PROV_REC_LEN);
    put_u32le(out + 0, IV_PROV_REC_MAGIC);
    out[4] = (uint8_t)IV_PROV_REC_VER;
    out[5] = id->suite_id;
    /* 6..7 reserved */
    put_u32le(out + 8, id->devid);
    memcpy(out + 12, id->mac, 6u);
    /* 18..19 reserved */
    memset(srv, 0, sizeof(srv));
    (void)memcpy(srv, id->server, sizeof(srv));
    srv[sizeof(srv) - 1u] = '\0';
    memcpy(out + 20, srv, IV_PROV_SERVER_MAX);
    put_u32le(out + 52, iv_prov_checksum(out, 52u));
    return IV_OK;
}

int iv_prov_id_decode(const uint8_t in[IV_PROV_REC_LEN], iv_prov_id_t *out)
{
    if (in == NULL || out == NULL)
        return IV_EINVAL;
    if (get_u32le(in) != IV_PROV_REC_MAGIC)
        return IV_EPROTO;
    if (in[4] != (uint8_t)IV_PROV_REC_VER)
        return IV_EPROTO;
    if (get_u32le(in + 52) != iv_prov_checksum(in, 52u))
        return IV_EPROTO;

    memset(out, 0, sizeof(*out));
    out->suite_id = in[5];
    out->devid    = get_u32le(in + 8);
    memcpy(out->mac, in + 12, 6u);
    copy_fixed_str(out->server, sizeof(out->server), in + 20, IV_PROV_SERVER_MAX);
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 裸分区读写
 * -------------------------------------------------------------------------- */
int iv_prov_private_write(const char *dev, const iv_prov_id_t *id)
{
    const char *path = (dev != NULL) ? dev : IV_PROV_PRIVATE_DEV_DEFAULT;
    uint8_t     blk[IV_PROV_SECTOR];
    int         fd;
    ssize_t     n;

    if (id == NULL)
        return IV_EINVAL;

    memset(blk, 0xFF, sizeof(blk)); /* 未被记录占用的字节保持擦除态 */
    if (iv_prov_id_encode(id, blk) != IV_OK)
        return IV_EINVAL;
    iv_prov_xor(blk, IV_PROV_REC_LEN);

    /* 刻意无 O_CREAT：root 下 O_CREAT 会把缺失的分区设备悄悄变成普通文件，
     * 身份"写成功"却落在内存盘，比重接报错危险。 */
    fd = open(path, O_WRONLY | O_SYNC);
    if (fd < 0)
        return (errno == ENOENT) ? IV_ENOENT : IV_EIO;

    n = write(fd, blk, sizeof(blk));
    if (n != (ssize_t)sizeof(blk)) {
        (void)close(fd);
        return IV_EIO;
    }
    if (fsync(fd) != 0) { /* 裸分区没有文件系统兜底，自己 fsync */
        (void)close(fd);
        return IV_EIO;
    }
    (void)close(fd);
    return IV_OK;
}

int iv_prov_private_read(const char *dev, iv_prov_id_t *out)
{
    const char *path = (dev != NULL) ? dev : IV_PROV_PRIVATE_DEV_DEFAULT;
    uint8_t     blk[IV_PROV_SECTOR];
    int         fd;
    ssize_t     n;

    if (out == NULL)
        return IV_EINVAL;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return (errno == ENOENT) ? IV_ENOENT : IV_EIO;
    n = read(fd, blk, sizeof(blk));
    (void)close(fd);
    if (n < (ssize_t)IV_PROV_REC_LEN)
        return IV_EIO;

    iv_prov_xor(blk, IV_PROV_REC_LEN);
    /* 解出来不是合法记录 ⇒ 这块分区还没被工厂写过，按"不存在"上报。 */
    return (iv_prov_id_decode(blk, out) == IV_OK) ? IV_OK : IV_ENOENT;
}

void iv_prov_devinfo_from_id(const iv_prov_id_t *id, iv_prov_devinfo_t *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (id == NULL)
        return;
    iv_prov_tid_from_u32(id->devid, out->tid);
    iv_prov_mac_format(id->mac, out->mac);
}

/* --------------------------------------------------------------------------
 * 报文解析：容错扫描器（只服务 search / update_mac 两条）
 * -------------------------------------------------------------------------- */
static int is_ws(char c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

static size_t skip_ws(const char *s, size_t i, size_t e)
{
    while (i < e && is_ws(s[i]))
        i++;
    return i;
}

/* 从 i 起取出一个字符串字面量（i 必须指向开引号），*end 收到右引号之后的下标。
 * 支持 \" \\ \/ \n \r \t 转义；其余 \x 原样保留 x。越界/超长返回 -1。 */
static int read_string(const char *s, size_t e, size_t i, char *out, size_t cap,
                       size_t *end)
{
    size_t o = 0u;

    if (i >= e || s[i] != '"')
        return -1;
    i++;
    while (i < e) {
        char c = s[i];

        if (c == '\\') {
            if (i + 1u >= e)
                return -1;
            i++;
            c = s[i];
            if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
            /* \" \\ \/ 及其它一律按字面字符落地 */
        } else if (c == '"') {
            out[o] = '\0';
            *end = i + 1u;
            return (int)o;
        }
        if (o + 1u >= cap)
            return -1; /* 值超长：拒收，不截断 */
        out[o++] = c;
        i++;
    }
    return -1; /* 未闭合 */
}

/* 跳过（校验但不落地）一个字符串字面量；*end 收到右引号之后的下标。
 * 用于"只想知道对象边界"的场合 —— 这里不设长度上限，因为对方可能带长字段。 */
static int skip_string(const char *s, size_t e, size_t i, size_t *end)
{
    if (i >= e || s[i] != '"')
        return -1;
    i++;
    while (i < e) {
        if (s[i] == '\\') {
            if (i + 1u >= e)
                return -1;
            i += 2u;
            continue;
        }
        if (s[i] == '"') {
            *end = i + 1u;
            return 0;
        }
        i++;
    }
    return -1;
}

/* 在 [b,e) 里找 `"key"`，返回值的起点下标；找不到返回 -1。 */
static long find_key(const char *s, size_t b, size_t e, const char *key, size_t *colon)
{
    size_t klen = strlen(key);
    size_t i;

    for (i = b; i + klen + 2u <= e; i++) {
        size_t j;

        if (s[i] != '"')
            continue;
        for (j = 0u; j < klen; j++) {
            if (s[i + 1u + j] != key[j])
                break;
        }
        if (j != klen || s[i + 1u + klen] != '"')
            continue;
        {
            size_t p = skip_ws(s, i + 2u + klen, e);

            if (p >= e || s[p] != ':')
                continue;
            *colon = p + 1u;
            return (long)i;
        }
    }
    return -1;
}

/* 取 `"key":"value"` 形式的字符串值。返回 1=找到，0=没有这个键，-1=格式错。 */
static int obj_get_string(const char *s, size_t b, size_t e, const char *key,
                          char *out, size_t cap, int *found)
{
    size_t colon = 0u;
    size_t v;
    size_t end;

    if (found != NULL)
        *found = 0;
    if (find_key(s, b, e, key, &colon) < 0)
        return 0;
    v = skip_ws(s, colon, e);
    if (v >= e) {
        return -1;
    }
    if (read_string(s, e, v, out, cap, &end) < 0)
        return -1;
    if (found != NULL)
        *found = 1;
    return 1;
}

/* 取出 `"key":{ ... }` 的对象**内部**区间 [*ob,*oe)。返回 1/0/-1。 */
static int obj_get_object(const char *s, size_t b, size_t e, const char *key,
                          size_t *ob, size_t *oe)
{
    size_t colon = 0u;
    size_t v;
    size_t i;
    int    depth = 0;

    if (find_key(s, b, e, key, &colon) < 0)
        return 0;
    v = skip_ws(s, colon, e);
    if (v >= e || s[v] != '{') {
        return -1;
    }
    *ob = v + 1u;
    for (i = v; i < e; i++) {
        char c = s[i];

        if (c == '"') { /* 跳过字符串，里面的大括号不算层级 */
            size_t end;

            if (skip_string(s, e, i, &end) < 0)
                return -1;
            i = end - 1u;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                *oe = i;
                return 1;
            }
        }
    }
    return -1;
}

int iv_prov_msg_parse(const char *json, size_t len, iv_prov_msg_t *out)
{
    size_t ib, ie, db, de;
    int    found;

    if (json == NULL || out == NULL)
        return IV_EINVAL;
    if (len == 0u || len > IV_PROV_JSON_MAX)
        return IV_EPROTO;

    memset(out, 0, sizeof(*out));

    if (obj_get_object(json, 0u, len, "info", &ib, &ie) != 1)
        return IV_EPROTO;
    if (obj_get_object(json, 0u, len, "data", &db, &de) != 1)
        return IV_EPROTO;

    if (obj_get_string(json, ib, ie, "tid", out->tid, sizeof(out->tid), &found) < 0)
        return IV_EPROTO;
    if (!found)
        return IV_EPROTO; /* 文档要求 info.tid 必在（可为空串） */

    /* date 可选：文档里是存在字段，但设备回复时不依赖它 */
    if (obj_get_string(json, ib, ie, "date", out->date, sizeof(out->date), NULL) < 0)
        return IV_EPROTO;

    if (obj_get_string(json, db, de, "types", out->types, sizeof(out->types), &found) < 0)
        return IV_EPROTO;
    if (!found || out->types[0] == '\0')
        return IV_EPROTO;

    if (obj_get_string(json, db, de, "mac", out->mac, sizeof(out->mac), &found) < 0)
        return IV_EPROTO;
    out->has_mac = found;
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 决策
 * -------------------------------------------------------------------------- */
static int tid_is_bcast(const char *tid)
{
    return (tid != NULL && strcmp(tid, IV_PROV_TID_BROADCAST) == 0);
}

static int tid_equal_ci(const char *a, const char *b)
{
    size_t i;

    if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0')
        return 0;
    for (i = 0u; a[i] != '\0' && b[i] != '\0'; i++) {
        char x = a[i];
        char y = b[i];

        if (x >= 'a' && x <= 'z')
            x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z')
            y = (char)(y - 'a' + 'A');
        if (x != y)
            return 0;
    }
    return (a[i] == '\0' && b[i] == '\0') ? 1 : 0;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0u)
        return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, cap, "%s", src);
}

int iv_prov_decide(const char *json, size_t len, const char *own_tid,
                   iv_prov_plan_t *out)
{
    iv_prov_msg_t m;
    int           rc;

    if (out == NULL)
        return IV_EINVAL;
    memset(out, 0, sizeof(*out));
    out->act = IV_PROV_ACT_NONE;

    rc = iv_prov_msg_parse(json, len, &m);
    if (rc != IV_OK)
        return rc;

    if (strcmp(m.types, IV_PROV_TYPE_SEARCH) == 0) {
        /* 广播搜索，或点我的名 */
        if (tid_is_bcast(m.tid) || tid_equal_ci(m.tid, own_tid)) {
            out->act = IV_PROV_ACT_SEARCH;
            copy_str(out->new_tid, sizeof(out->new_tid),
                     (own_tid != NULL && own_tid[0] != '\0') ? own_tid : m.tid);
        }
        return IV_OK;
    }

    if (strcmp(m.types, IV_PROV_TYPE_UPDATE_MAC) == 0) {
        uint32_t v = 0u;

        out->act    = IV_PROV_ACT_UPDATE_MAC;
        out->mac_ok = (m.has_mac && m.mac[0] != '\0' &&
                       iv_prov_mac_parse(m.mac, out->new_mac) == IV_OK) ? 1 : 0;

        if (tid_is_bcast(m.tid) || tid_equal_ci(m.tid, own_tid)) {
            /* 广播 / 点自己的名 ⇒ 只改 MAC，不动设备 ID */
            copy_str(out->new_tid, sizeof(out->new_tid),
                     (own_tid != NULL && own_tid[0] != '\0') ? own_tid : m.tid);
        } else if (iv_prov_tid_to_u32(m.tid, &v) == IV_OK) {
            /* tid 即要写入的新设备 ID（口径见 iv_provision.h 文件头） */
            copy_str(out->new_tid, sizeof(out->new_tid), m.tid);
        } else {
            /* 既不是广播、又不像设备 ID ⇒ 不是发给我的，静默 */
            out->act = IV_PROV_ACT_NONE;
        }
        return IV_OK;
    }

    /* inquiry / update_relay / update_ip / update_server / update_np /
     * update_runaddr：本轮未实现 ⇒ 不回复、不动作（登记在案）。 */
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 组包（严格 JSON；写失败返回 <0，不留半个包）
 * -------------------------------------------------------------------------- */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    ovf;
} jw_t;

static void jw_raw(jw_t *w, const char *s, size_t n)
{
    if (w->ovf)
        return;
    if (s == NULL || w->len + n >= w->cap) {
        w->ovf = 1;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void jw_puts(jw_t *w, const char *s)
{
    if (s != NULL)
        jw_raw(w, s, strlen(s));
}

/* 输出一个 JSON 字符串（带引号、转义 " \ 与 <0x20 控制符）。 */
static void jw_str(jw_t *w, const char *s)
{
    char esc[8];

    jw_puts(w, "\"");
    if (s != NULL) {
        for (; *s != '\0'; s++) {
            unsigned char c = (unsigned char)*s;

            if (c == '"' || c == '\\') {
                esc[0] = '\\';
                esc[1] = (char)c;
                esc[2] = '\0';
            } else if (c == '\n' || c == '\r' || c == '\t') {
                esc[0] = '\\';
                esc[1] = (c == '\n') ? 'n' : ((c == '\r') ? 'r' : 't');
                esc[2] = '\0';
            } else if (c < 0x20u) {
                (void)snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
            } else {
                esc[0] = (char)c;
                esc[1] = '\0';
            }
            jw_puts(w, esc);
        }
    }
    jw_puts(w, "\"");
}

/* "key":"value" 一对（value 走转义）。 */
static void jw_kv(jw_t *w, const char *key, const char *val)
{
    jw_puts(w, ",");
    jw_str(w, key);
    jw_puts(w, ":");
    jw_str(w, val);
}

/* 设备信息字段（文档示例 JSON 的次序；字段表里另列了 name/password，但两侧
 * 示例 JSON 都没有 ⇒ 按示例，不带它们，见文件头登记）。 */
static void jw_devinfo(jw_t *w, const iv_prov_devinfo_t *d)
{
    jw_kv(w, "tnam", d->tnam);
    jw_kv(w, "mcu", d->mcu);
    jw_kv(w, "mod", d->mod);
    jw_kv(w, "sv", d->sv);
    jw_kv(w, "mac", d->mac);
    jw_kv(w, "ip", d->ip);
    jw_kv(w, "nm", d->nm);
    jw_kv(w, "gw", d->gw);
    jw_kv(w, "inip", d->inip);
    jw_kv(w, "outip", d->outip);
    jw_kv(w, "iccid", d->iccid);
}

static int jw_done(jw_t *w)
{
    return w->ovf ? -1 : (int)w->len;
}

int iv_prov_build_search_query(char *out, size_t cap)
{
    jw_t w;
    char date[IV_PROV_DATE_MAX];

    if (out == NULL || cap == 0u)
        return -1;
    out[0] = '\0';
    iv_prov_date_now(date, 0);

    w.buf = out; w.cap = cap; w.len = 0u; w.ovf = 0;
    jw_puts(&w, "{\"info\":{\"tid\":\"" IV_PROV_TID_BROADCAST "\",\"date\":");
    jw_str(&w, date);
    jw_puts(&w, "},\"data\":{\"types\":\"" IV_PROV_TYPE_SEARCH "\"}}");
    return jw_done(&w);
}

int iv_prov_build_search_reply(const iv_prov_devinfo_t *self, char *out, size_t cap)
{
    jw_t w;
    char date[IV_PROV_DATE_MAX];

    if (self == NULL || out == NULL || cap == 0u)
        return -1;
    out[0] = '\0';
    iv_prov_date_now(date, 0);

    w.buf = out; w.cap = cap; w.len = 0u; w.ovf = 0;
    jw_puts(&w, "{\"info\":{\"tid\":");
    jw_str(&w, self->tid);
    jw_puts(&w, ",\"date\":");
    jw_str(&w, date);
    jw_puts(&w, "},\"data\":{\"types\":\"" IV_PROV_TYPE_SEARCH "\"");
    jw_devinfo(&w, self);
    jw_puts(&w, "}}");
    return jw_done(&w);
}

int iv_prov_build_update_mac_query(const char *tid, const char *mac,
                                   char *out, size_t cap)
{
    jw_t w;
    char date[IV_PROV_DATE_MAX];

    if (out == NULL || cap == 0u)
        return -1;
    out[0] = '\0';
    iv_prov_date_now(date, 0);

    w.buf = out; w.cap = cap; w.len = 0u; w.ovf = 0;
    jw_puts(&w, "{\"info\":{\"tid\":");
    jw_str(&w, (tid != NULL) ? tid : IV_PROV_TID_BROADCAST);
    jw_puts(&w, ",\"date\":");
    jw_str(&w, date);
    jw_puts(&w, "},\"data\":{\"types\":\"" IV_PROV_TYPE_UPDATE_MAC "\"");
    jw_kv(&w, "mac", (mac != NULL) ? mac : "");
    jw_puts(&w, "}}");
    return jw_done(&w);
}

int iv_prov_build_update_mac_reply(const iv_prov_devinfo_t *self, int ok,
                                   char *out, size_t cap)
{
    jw_t w;
    char date[IV_PROV_DATE_MAX];

    if (self == NULL || out == NULL || cap == 0u)
        return -1;
    out[0] = '\0';
    iv_prov_date_now(date, 0);

    w.buf = out; w.cap = cap; w.len = 0u; w.ovf = 0;
    jw_puts(&w, "{\"info\":{\"tid\":");
    jw_str(&w, self->tid);
    jw_puts(&w, ",\"date\":");
    jw_str(&w, date);
    jw_puts(&w, "},\"data\":{\"types\":\"" IV_PROV_TYPE_UPDATE_MAC "\"");
    jw_kv(&w, "result", ok ? IV_PROV_RESULT_OK : IV_PROV_RESULT_FAULT);
    jw_devinfo(&w, self);
    jw_puts(&w, "}}");
    return jw_done(&w);
}

/* --------------------------------------------------------------------------
 * 工厂标记
 * -------------------------------------------------------------------------- */
int iv_prov_marker_present(const char *path)
{
    const char  *p = (path != NULL) ? path : IV_PROV_MARKER_DEFAULT;
    struct stat  sb;

    return (stat(p, &sb) == 0) ? 1 : 0;
}
