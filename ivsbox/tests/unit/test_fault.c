/*
 * iv_fault 单测（2026-10-28，故障码表与合并）
 *
 * 覆盖：
 *   f01 健康态：空表 ⇒ 输出 `0`（与参考实现 `ERR=0;` 一致）
 *   f02 采集板原文 `ERR=2,10100000,30100000;` ⇒ 2 条、原值直出
 *   f03 不带前缀/尾分号同样认（采集板怎么拼都得认）
 *   f04 网络码（0x2）一律剔除：只采信电量/传感器 ⇒ 且计入 board_rejected
 *   f05 重复码去重、`0x` 前缀也认
 *   f06 垃圾 token 跳过但不影响其它码（不整段丢）
 *   f07 数量字段与实际码数不符 ⇒ 照码处理 ＋ board_mismatch++
 *   f08 整体替换：再收到一次（含空串）就按新值重建，不是累加
 *   f09 码数超上限 ⇒ 收满 32 条 ＋ board_overflow++
 *   f10 网络码映射：主网1 故障/延时/丢包、主网2 故障/延时/丢包、主网1 网线断开只报 LAN 口
 *   f11 载波未知（负数）不报 LAN 口故障 —— 开机误报的防线
 *   f12 摄像机 2⇒故障 4⇒延时 5⇒丢包（三条都置码）0（无 IP）不置码，序号 1 基
 *   f13 合并：board ∪ net 去重 ＋ 升序（同一组故障每次拼出来一样）
 *   f14 cap 不够返回 IV_ERANGE（不写半截）
 *   f15 NULL 入参安全
 *
 * 反向证伪（撤掉实现后哪条会红）：
 *   F1 采信条件改成"类 1/2/3 全收" ⇒ f04 红（网络码进来，条数从 1 变 2）；
 *   F2 `token_is_count` 去掉长度上限 ⇒ f02 的 `2` 被当成码值 0x00000002 ⇒ 变 3 条；
 *   F3 去掉"整体替换"改累加 ⇒ f08 红（旧码残留）；
 *   F4 主网 3 也置主网故障 ⇒ f10 红（多出 20200000）；
 *   F5 载波未知按 0 处理 ⇒ f11 红；
 *   F6 摄像机下标用 0 基 ⇒ f12 红（码变成 20400000/20420000）。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_fault.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 组串并返回（失败返回空串，便于断言直接比对） */
static const char *fmt(const iv_fault_t *f, char *buf, size_t cap)
{
    if (iv_fault_format(f, buf, cap) != IV_OK) return "(ERANGE)";
    return buf;
}

static void f01_healthy(void)
{
    iv_fault_t   f;
    char         s[IV_FAULT_STR_MAX];

    iv_fault_init(&f);
    (void)iv_fault_apply_board(&f, NULL, 0u);
    chk(strcmp(fmt(&f, s, sizeof s), "0") == 0, "f01 empty table -> \"0\"");
    chk(iv_fault_count(&f) == 0, "f01 count 0");
}

static void f02_board_raw(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];

    iv_fault_init(&f);
    chk(iv_fault_apply_board(&f, "ERR=2,10100000,30100000;", 0u) == 2,
        "f02 two codes accepted");
    chk(strcmp(fmt(&f, s, sizeof s), "2,10100000,30100000") == 0,
        "f02 raw value -> n,c1,c2 (uppercase hex, no prefix/suffix)");
    chk(f.board_rounds == 1u, "f02 one round counted");
}

static void f03_variants(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];

    iv_fault_init(&f);
    chk(iv_fault_apply_board(&f, "10100000,30100000", 0u) == 2,
        "f03 no ERR= prefix, no trailing ';'");
    chk(strcmp(fmt(&f, s, sizeof s), "2,10100000,30100000") == 0,
        "f03 same output for the bare form");

    chk(iv_fault_apply_board(&f, "ERR=0;", 0u) == 0, "f03 ERR=0 -> zero codes");
    chk(strcmp(fmt(&f, s, sizeof s), "0") == 0, "f03 ERR=0 -> \"0\"");
}

static void f04_net_codes_rejected(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];

    iv_fault_init(&f);
    /* 采集板若也送网络码（0x2），主板不采信 —— 网络以主板结论为准 */
    chk(iv_fault_apply_board(&f, "3,20100000,10100000,30100000", 0u) == 2,
        "f04 only elec/sensor codes accepted");
    chk(f.board_rejected == 1u, "f04 network code counted as rejected");
    chk(strcmp(fmt(&f, s, sizeof s), "2,10100000,30100000") == 0,
        "f04 network code absent from output");
    chk(iv_fault_has(&f, IV_FAULT_NET_LAN_PORT) == 0, "f04 has() says no LAN code");
}

static void f05_dedup_prefix(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];

    iv_fault_init(&f);
    chk(iv_fault_apply_board(&f, "3,10100000,10100000,0x30100000", 0u) == 2,
        "f05 duplicates collapse, 0x prefix accepted");
    chk(strcmp(fmt(&f, s, sizeof s), "2,10100000,30100000") == 0,
        "f05 dedup output");
}

static void f06_garbage(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];

    iv_fault_init(&f);
    chk(iv_fault_apply_board(&f, "2,XYZ,30100000", 0u) == 1,
        "f06 garbage token skipped, good code kept");
    chk(f.board_rejected == 1u, "f06 garbage counted");
    chk(strcmp(fmt(&f, s, sizeof s), "1,30100000") == 0, "f06 output");
}

static void f07_count_mismatch(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];

    iv_fault_init(&f);
    /* 参考实现按缓冲区上限会截断码、但数量字段是完整的 ⇒ 数量不可采信 */
    chk(iv_fault_apply_board(&f, "5,10100000", 0u) == 1, "f07 trust codes, not count");
    chk(f.board_mismatch == 1u, "f07 mismatch counted");
    chk(strcmp(fmt(&f, s, sizeof s), "1,10100000") == 0, "f07 output");
}

static void f08_replace(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];

    iv_fault_init(&f);
    (void)iv_fault_apply_board(&f, "2,10100000,30100000", 0u);
    chk(iv_fault_apply_board(&f, "1,30100000", 0u) == 1, "f08 new round replaces");
    chk(strcmp(fmt(&f, s, sizeof s), "1,30100000") == 0,
        "f08 stale code gone (整体替换, not accumulate)");

    chk(iv_fault_apply_board(&f, "", 0u) == 0, "f08 empty field clears");
    chk(strcmp(fmt(&f, s, sizeof s), "0") == 0, "f08 cleared -> \"0\"");
    chk(f.board_rounds == 3u, "f08 every round counted (含空串)");
}

static void f09_overflow(void)
{
    iv_fault_t f;
    char       in[512];
    size_t     n = 0;
    int        i;

    iv_fault_init(&f);
    for (i = 1; i <= 40; i++) {
        int w = snprintf(in + n, sizeof in - n, ",%08X",
                         (unsigned)(IV_FAULT_ELEC_MAIN_AC + ((uint32_t)i << 20)));
        if (w <= 0) break;
        n += (size_t)w;
    }
    (void)iv_fault_apply_board(&f, in, 0u);
    chk(f.nb == IV_FAULT_MAX, "f09 table capped at IV_FAULT_MAX");
    chk(f.board_overflow == 1u, "f09 overflow counted");
}

static void f10_main_net(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];

    /* 主网1 断开（2）⇒ NET_MAIN_FAULT */
    iv_fault_init(&f);
    iv_fault_apply_net(&f, 2, 1, NULL, 0, 1);
    chk(iv_fault_has(&f, IV_FAULT_NET_MAIN_FAULT), "f10 mn1=2 -> MAIN_FAULT");
    chk(strcmp(fmt(&f, s, sizeof s), "1,20200000") == 0, "f10 output");

    /* 主网1 延时（4）⇒ NET_MAIN_DELAY */
    iv_fault_apply_net(&f, 4, 1, NULL, 0, 1);
    chk(iv_fault_has(&f, IV_FAULT_NET_MAIN_DELAY), "f10 mn1=4 -> MAIN_DELAY");
    chk(!iv_fault_has(&f, IV_FAULT_NET_MAIN_FAULT), "f10 delay clears the fault code");

    /* 主网1 网线断开（3）⇒ 只报 LAN 口，不重复报主网故障 */
    iv_fault_apply_net(&f, 3, 1, NULL, 0, 0);
    chk(iv_fault_has(&f, IV_FAULT_NET_LAN_PORT), "f10 mn1=3 -> LAN_PORT");
    chk(!iv_fault_has(&f, IV_FAULT_NET_MAIN_FAULT),
        "f10 mn1=3 does NOT also raise MAIN_FAULT");

    /* 主网1 丢包（5）⇒ NET_MAIN_LOSS：参考固件 `det_set_main_network_status()`
     * 对 `NET_STATUS_LOSS` 是 `Error_Set(NET_MAIN_LOSS)`（2026-10-30 对齐） */
    iv_fault_apply_net(&f, 5, 1, NULL, 0, 1);
    chk(iv_fault_has(&f, IV_FAULT_NET_MAIN_LOSS), "f10 mn1=5 -> MAIN_LOSS");
    chk(strcmp(fmt(&f, s, sizeof s), "1,20800000") == 0, "f10 loss output");

    /* 主网2（`probe.target2`）：2/3 都是故障，4 延时，5 丢包 */
    iv_fault_apply_net(&f, 1, 2, NULL, 0, 1);
    chk(iv_fault_has(&f, IV_FAULT_NET_MAIN2_FAULT), "f10 mn2=2 -> MAIN2_FAULT");
    iv_fault_apply_net(&f, 1, 3, NULL, 0, 1);
    chk(iv_fault_has(&f, IV_FAULT_NET_MAIN2_FAULT), "f10 mn2=3 -> MAIN2_FAULT");
    iv_fault_apply_net(&f, 1, 4, NULL, 0, 1);
    chk(iv_fault_has(&f, IV_FAULT_NET_MAIN2_DELAY), "f10 mn2=4 -> MAIN2_DELAY");
    chk(!iv_fault_has(&f, IV_FAULT_NET_MAIN2_FAULT), "f10 delay replaces fault");
    iv_fault_apply_net(&f, 1, 5, NULL, 0, 1);
    chk(iv_fault_has(&f, IV_FAULT_NET_MAIN2_LOSS), "f10 mn2=5 -> MAIN2_LOSS");

    /* 全正常 ⇒ 空表 */
    iv_fault_apply_net(&f, 1, 1, NULL, 0, 1);
    chk(strcmp(fmt(&f, s, sizeof s), "0") == 0, "f10 all normal -> \"0\"");
}

static void f11_lan_unknown(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];

    iv_fault_init(&f);
    /* 载波未知（未绑接口 / netlink 还没回事件）：不许凭空报 LAN 口故障 */
    iv_fault_apply_net(&f, 0, 0, NULL, 0, -1);
    chk(strcmp(fmt(&f, s, sizeof s), "0") == 0,
        "f11 unknown carrier -> no LAN code (boot-time false alarm)");
}

static void f12_cameras(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];
    int        cns[6];
    int        i;

    for (i = 0; i < 6; i++) cns[i] = 1;

    iv_fault_init(&f);
    cns[0] = 2; /* 摄像机1 故障 */
    cns[2] = 4; /* 摄像机3 延时 */
    cns[4] = 5; /* 摄像机5 丢包：参考固件 det_set_camera_status() 也置码 */
    iv_fault_apply_net(&f, 1, 1, cns, 6, 1);
    chk(iv_fault_has(&f, IV_FAULT_NET_CAM_FAULT(1)), "f12 ch0(1) fault -> 20410000");
    chk(iv_fault_has(&f, IV_FAULT_NET_CAM_DELAY(3)), "f12 ch2(3) delay -> 20730000");
    chk(iv_fault_has(&f, IV_FAULT_NET_CAM_LOSS(5)), "f12 ch4(5) loss -> 20A50000");
    chk(iv_fault_count(&f) == 3, "f12 exactly three codes");
    chk(strcmp(fmt(&f, s, sizeof s), "3,20410000,20730000,20A50000") == 0,
        "f12 output");

    /* 无 IP（0）与正常（1）都不置码 */
    for (i = 0; i < 6; i++) cns[i] = (i == 1) ? 0 : 1;
    iv_fault_apply_net(&f, 1, 1, cns, 6, 1);
    chk(strcmp(fmt(&f, s, sizeof s), "0") == 0, "f12 no-ip/normal -> \"0\"");
}

static void f13_merge_sorted(void)
{
    iv_fault_t f;
    char       s[IV_FAULT_STR_MAX];
    int        cns[6];
    int        i;

    for (i = 0; i < 6; i++) cns[i] = 1;
    cns[1] = 2; /* 摄像机2 故障 = 20420000 */

    iv_fault_init(&f);
    /* 采集板报电量 + 传感器（故意乱序），主板报摄像机 —— 合并后必须升序 */
    (void)iv_fault_apply_board(&f, "2,30100000,10100000", 0u);
    iv_fault_apply_net(&f, 2, 1, cns, 6, 1); /* 主网1 故障 = 20200000 */
    chk(iv_fault_count(&f) == 4, "f13 four codes merged");
    chk(strcmp(fmt(&f, s, sizeof s),
               "4,10100000,20200000,20420000,30100000") == 0,
        "f13 merged output is ascending");

    /* 同一个码两边都有 ⇒ 只出现一次 */
    iv_fault_init(&f);
    (void)iv_fault_apply_board(&f, "1,10100000", 0u);
    iv_fault_apply_net(&f, 1, 1, NULL, 0, -1);
    chk(iv_fault_count(&f) == 1, "f13 single code");
}

static void f14_erange(void)
{
    iv_fault_t f;
    char       tiny[6];

    iv_fault_init(&f);
    (void)iv_fault_apply_board(&f, "2,10100000,30100000", 0u);
    chk(iv_fault_format(&f, tiny, sizeof tiny) == IV_ERANGE,
        "f14 small buffer -> ERANGE (no half string)");
    chk(iv_fault_format(&f, tiny, 0u) == IV_EINVAL, "f14 cap 0 -> EINVAL");
}

static void f15_null(void)
{
    char s[IV_FAULT_STR_MAX];

    chk(iv_fault_apply_board(NULL, "1,10100000", 0u) == IV_EINVAL, "f15 NULL f");
    iv_fault_apply_net(NULL, 2, 2, NULL, 0, 0); /* 不炸即通过 */
    chk(iv_fault_format(NULL, s, sizeof s) == IV_EINVAL, "f15 format NULL f");
    chk(iv_fault_count(NULL) == 0, "f15 count NULL -> 0");
    chk(iv_fault_has(NULL, IV_FAULT_ELEC_MAIN_AC) == 0, "f15 has NULL -> 0");
    iv_fault_init(NULL); /* 不炸即通过 */
}

int main(void)
{
    f01_healthy();
    f02_board_raw();
    f03_variants();
    f04_net_codes_rejected();
    f05_dedup_prefix();
    f06_garbage();
    f07_count_mismatch();
    f08_replace();
    f09_overflow();
    f10_main_net();
    f11_lan_unknown();
    f12_cameras();
    f13_merge_sorted();
    f14_erange();
    f15_null();

    if (g_fail != 0) {
        fprintf(stderr, "test_fault FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_fault passed (parse/replace/reject/mismatch/overflow/net/camera/"
           "merge/sort/erange/null)\n");
    return 0;
}
