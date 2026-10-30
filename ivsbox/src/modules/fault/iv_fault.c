/*
 * 故障码表与合并实现（2026-10-28）。
 * 口径、分工与判定表见 include/ivsbox/iv_fault.h；这里只有解析与拼装。
 *
 * 纪律：零 malloc、零 I/O、无全局状态；全部判定都是"输入 → 输出"的纯映射，
 * 因此单测不需要桩件（test_fault 直接喂字符串与状态码）。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_fault.h"

/* 分隔符：`,` 是参考实现的写法；`;` 是 `Error_Get_Codesbuf()` 的结尾；
 * 空白与换行容忍采集板把它拼成多段（如 `ERR=1,10100000; `）。 */
static int is_sep(char c)
{
    return c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' ||
           c == '\n';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 整数型 token（全十进制数字，长度 <= 3）——只可能是"数量"字段。
 * 为什么要限长度：故障码一律 8 位十六进制（`%08X`），3 位以内不可能是码，
 * 也就不会把 `2`（数量）误当成码值 0x00000002。 */
static int token_is_count(const char *t, size_t n)
{
    size_t i;

    if (n == 0 || n > 3) return 0;
    for (i = 0; i < n; i++)
        if (t[i] < '0' || t[i] > '9') return 0;
    return 1;
}

/* 解析一个码 token（可带 `0x` 前缀，1..8 位十六进制，整段必须吃完）。
 * 返回 1 = 是合法码；0 = 不是（垃圾/十进制/超长）。 */
static int token_code(const char *t, size_t n, uint32_t *out)
{
    size_t   i = 0;
    uint32_t v = 0;

    if (n >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) i = 2;
    if (i >= n || n - i > 8) return 0;
    for (; i < n; i++) {
        int h = hex_val(t[i]);

        if (h < 0) return 0;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return 1;
}

/* 返回 1 = 新加进表 / 0 = 表里已有（去重）/ -1 = 表满 */
static int board_add(iv_fault_t *f, uint32_t code)
{
    int i;

    for (i = 0; i < (int)f->nb; i++)
        if (f->board[i] == code) return 0;
    if (f->nb >= IV_FAULT_MAX) return -1;
    f->board[f->nb++] = code;
    return 1;
}

int iv_fault_apply_board(iv_fault_t *f, const char *err_field, uint64_t now_ms)
{
    const char *s = err_field;
    unsigned    declared = 0;
    int         have_declared = 0;
    int         codes_seen = 0;
    int         accepted = 0;
    int         overflow = 0;
    int         first = 1;

    if (f == NULL) return IV_EINVAL;

    f->nb = 0;
    f->board_rounds++;
    f->board_ms = now_ms;

    if (s == NULL || s[0] == '\0') return 0;

    /* 前缀 `ERR=` / `err=`（采集板若原样 strcat 了 Error_Get_Codesbuf 的缓冲区，
     * 送来的就是 `ERR=2,10100000,...;`）。大小写都认，只认开头这一处。 */
    if ((s[0] == 'E' || s[0] == 'e') && (s[1] == 'R' || s[1] == 'r') &&
        (s[2] == 'R' || s[2] == 'r') && s[3] == '=')
        s += 4;

    while (*s != '\0') {
        const char *t;
        size_t      n;
        uint32_t    code;

        while (*s != '\0' && is_sep(*s)) s++;
        if (*s == '\0') break;
        t = s;
        while (*s != '\0' && !is_sep(*s)) s++;
        n = (size_t)(s - t);

        if (first) {
            first = 0;
            if (token_is_count(t, n)) {
                unsigned k = 0;
                size_t   i;

                for (i = 0; i < n; i++) k = k * 10u + (unsigned)(t[i] - '0');
                declared = k;
                have_declared = 1;
                continue;
            }
        }

        if (!token_code(t, n, &code)) {
            f->board_rejected++;
            continue;
        }
        codes_seen++;
        /* 只采信电量(0x1)/传感器(0x3)——网络码以主板结论为准（见头文件分工）。
         * 0 与未知类别同样是垃圾，一并剔除并留痕。 */
        if (IV_FAULT_TYPE_OF(code) != 1u && IV_FAULT_TYPE_OF(code) != 3u) {
            f->board_rejected++;
            continue;
        }
        {
            int rc = board_add(f, code);

            if (rc < 0) {
                overflow = 1;
                continue;
            }
            if (rc > 0) accepted++; /* 0 = 表里已有（去重），不重复计数 */
        }
    }

    if (overflow) f->board_overflow++;
    if (have_declared && (int)declared != codes_seen) f->board_mismatch++;
    return accepted;
}

/* 往 net[] 里加一条（去重；满则静默丢弃 —— 网络码最多 15 条，够用） */
static void net_add(uint32_t *tab, int *n, uint32_t code)
{
    int i;

    for (i = 0; i < *n; i++)
        if (tab[i] == code) return;
    if (*n >= IV_FAULT_MAX) return;
    tab[(*n)++] = code;
}

void iv_fault_apply_net(iv_fault_t *f, int mn1, int mn2,
                        const int *cns, int cns_n, int lan_link)
{
    uint32_t tmp[IV_FAULT_MAX];
    int      n = 0;
    int      i;

    if (f == NULL) return;

    /* LAN 口（运维网口 = eth0）：只在"明确知道载波掉"时置码。
     * `lan_link < 0` = 未绑接口/还没收到 netlink 事件 ⇒ 不置码（否则开机即误报）。 */
    if (lan_link == 0) net_add(tmp, &n, IV_FAULT_NET_LAN_PORT);

    /* 主网1（`probe.target1` 的 ping 结论）：2 故障 / 4 延时 / 5 丢包 —— 与参考固件
     * `det.c:det_set_main_network_status()` 的三个 `Error_Set()` 一一对应。
     * 3（网线断开）不置主网码：那是 LAN 口载波掉，由上面的 `NET_LAN_PORT` 表达
     * 同一条物理事实，两个码只会让平台多出一条不相干的条目。生产方
     * （`iv_agent.c:tgt_fault_state()`）只把"检测 IP 的 3＝延时"翻成 4，
     * 所以这里收到 3 只可能是网线断开。 */
    if (mn1 == 2)
        net_add(tmp, &n, IV_FAULT_NET_MAIN_FAULT);
    else if (mn1 == 4)
        net_add(tmp, &n, IV_FAULT_NET_MAIN_DELAY);
    else if (mn1 == 5)
        net_add(tmp, &n, IV_FAULT_NET_MAIN_LOSS);

    /* 主网2（`probe.target2`）：没有"网线断开"这一态，3 与 2 同等对待 */
    if (mn2 == 2 || mn2 == 3)
        net_add(tmp, &n, IV_FAULT_NET_MAIN2_FAULT);
    else if (mn2 == 4)
        net_add(tmp, &n, IV_FAULT_NET_MAIN2_DELAY);
    else if (mn2 == 5)
        net_add(tmp, &n, IV_FAULT_NET_MAIN2_LOSS);

    /* 摄像机 1..6：2 故障 / 4 延时 / 5 丢包（参考固件 `det_set_camera_status()`
     * 三个 `Error_Set()`）；0（无 IP）/1 不置码。 */
    if (cns != NULL) {
        if (cns_n > IV_FAULT_MAX) cns_n = IV_FAULT_MAX;
        for (i = 0; i < cns_n; i++) {
            if (cns[i] == 2)
                net_add(tmp, &n, IV_FAULT_NET_CAM_FAULT(i + 1));
            else if (cns[i] == 4)
                net_add(tmp, &n, IV_FAULT_NET_CAM_DELAY(i + 1));
            else if (cns[i] == 5)
                net_add(tmp, &n, IV_FAULT_NET_CAM_LOSS(i + 1));
        }
    }

    memcpy(f->net, tmp, (size_t)n * sizeof(tmp[0]));
    f->nn = (uint8_t)n;
}

/* 合并（board ∪ net）去重后按升序写进 out；返回条数（cap 不足时返回 -1） */
static int merge_sorted(const iv_fault_t *f, uint32_t *out, int cap)
{
    int n = 0;
    int i, j;

    for (i = 0; i < (int)f->nb + (int)f->nn; i++) {
        int      dup = 0;
        uint32_t code = (i < (int)f->nb) ? f->board[i] : f->net[i - f->nb];
        uint32_t v;

        for (j = 0; j < n; j++)
            if (out[j] == code) { dup = 1; break; }
        if (dup) continue;
        if (n >= cap) return -1;

        /* 插入排序（n 很小，最多 64）：升序让同一组故障每次拼出来都一样 */
        v = code;
        for (j = n; j > 0 && out[j - 1] > v; j--) out[j] = out[j - 1];
        out[j] = v;
        n++;
    }
    return n;
}

int iv_fault_count(const iv_fault_t *f)
{
    uint32_t tmp[IV_FAULT_MAX * 2];
    int      n;

    if (f == NULL) return 0;
    n = merge_sorted(f, tmp, (int)(sizeof(tmp) / sizeof(tmp[0])));
    return (n < 0) ? 0 : n;
}

int iv_fault_has(const iv_fault_t *f, uint32_t code)
{
    int i;

    if (f == NULL) return 0;
    for (i = 0; i < (int)f->nb; i++)
        if (f->board[i] == code) return 1;
    for (i = 0; i < (int)f->nn; i++)
        if (f->net[i] == code) return 1;
    return 0;
}

int iv_fault_format(const iv_fault_t *f, char *out, size_t cap)
{
    uint32_t tmp[IV_FAULT_MAX * 2];
    size_t   n = 0;
    int      i, cnt;

    if (f == NULL || out == NULL || cap == 0u) return IV_EINVAL;

    cnt = merge_sorted(f, tmp, (int)(sizeof(tmp) / sizeof(tmp[0])));
    if (cnt < 0) return IV_ERANGE;

    /* 空表 = `0`（与参考实现健康态 `ERR=0;` 逐字一致） */
    {
        int w = snprintf(out, cap, "%d", cnt);

        if (w < 0 || (size_t)w >= cap) return IV_ERANGE;
        n = (size_t)w;
    }
    for (i = 0; i < cnt; i++) {
        int w = snprintf(out + n, cap - n, ",%08X", (unsigned)tmp[i]);

        if (w < 0 || (size_t)w >= cap - n) return IV_ERANGE;
        n += (size_t)w;
    }
    return IV_OK;
}

void iv_fault_init(iv_fault_t *f)
{
    if (f == NULL) return;
    memset(f, 0, sizeof(*f));
}
