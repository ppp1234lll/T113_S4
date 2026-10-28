/*
 * 故障码装配链单测（2026-10-28）：`env_feed()` → `iv_report_env_t::err`
 *
 * 背景：故障码判定与合并已经由 `test_fault` 钉死在纯函数层；本用例钉的是
 * **装配层那几行接线**——采集板镜像里的 `ERR` 原文有没有被喂进去、主网/载波的
 * 事实有没有被一起算、结果指针是不是指在 agent 自己的缓冲上（`iv_report_set_env()`
 * 只存指针，指向栈上临时量会在出报那一刻变垃圾）。
 *
 * 用例壳与 `test_agent_mn.c` 同款：不调 `iv_agent_open()`（那要串口 / netlink /
 * TCP），手摆 `wan_ifindex` / `wan_link_up` / `wan_probe_idx` 三个输入。
 *
 * 覆盖：
 *   k01 有线载波掉 + 采集板报电量/传感器 ⇒ 两半合并且升序
 *   k02 采集板送网络码 ⇒ 被剔除（网络以主板为准）
 *   k03 全正常 ⇒ `0`（与参考实现 `ERR=0;` 一致）
 *   k04 老固件（0xE1 应答没有 ERR 键）⇒ 只报主板网络码，不拿旧值冒充
 *   k05 指针落在 agent 内部缓冲上（出报时仍可读）
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_fault.h"
#include "ivsbox/iv_netmgr.h"
#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_recovery.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* "已 open"的 agent 壳：只建 probe / recovery，不碰串口与 netlink。 */
static void make_agent(iv_agent_t *a)
{
    iv_probe_cfg_t pc;

    memset(a, 0, sizeof(*a));
    iv_agent_init(a);
    iv_recovery_init(&a->rec, NULL);
    iv_probe_cfg_default(&pc);
    (void)iv_probe_init(&a->pb, &pc);
    a->opened = 1;
}

/* 摆一路 WAN 的输入（同 `test_agent_mn` 的做法，只是不建探测项）。 */
static void wan_set(iv_agent_t *a, int w, int ifindex, int link_up)
{
    a->wan_ifindex[w] = ifindex;
    a->wan_link_up[w] = (uint8_t)link_up;
    a->wan_probe_idx[w] = -1; /* 无探测项 ⇒ 状态机判"不可用"（mn=2） */
}

static void board_err(iv_agent_t *a, const char *raw)
{
    (void)snprintf(a->rp.st.data.ERR, sizeof a->rp.st.data.ERR, "%s", raw);
    a->rp.st.valid.ERR = 1;
}

static void step(iv_agent_t *a, uint64_t now)
{
    chk(iv_agent_step(a, now) == IV_OK, "step -> OK");
}

int main(void)
{
    iv_agent_t a;

    /* ---- k01 载波掉（LAN 口故障）＋ 采集板两条 ⇒ 合并升序 ---- */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 0);     /* 有线：载波掉 ⇒ mn=3 + LAN 口码 */
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);  /* 4G：无探测项 ⇒ mn=2（主网2 故障） */
    board_err(&a, "2,30100000,10100000");
    step(&a, 1000u);
    chk(strcmp(a.rp.env.err, "4,10100000,20100000,20300000,30100000") == 0,
        "k01 LAN + main2 + board codes merged ascending");
    chk(a.rp.env.err == a.err_buf, "k01 err points at the agent-owned buffer");

    /* ---- k02 采集板送网络码 ⇒ 剔除（网络只由主板出） ---- */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);    /* 载波在、但状态机判死 ⇒ mn=2 */
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    board_err(&a, "2,20100000,30100000"); /* 其中 20100000 是网络码 */
    step(&a, 1000u);
    chk(a.flt.board_rejected == 1u, "k02 board network code rejected");
    chk(strcmp(a.rp.env.err, "3,20200000,20300000,30100000") == 0,
        "k02 output has main-net codes from the board's own logic only");

    /* ---- k03 全正常 ⇒ "0" ---- */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    a.nm.wan_up[IV_WAN_WIRED] = 1u;
    a.nm.wan_up[IV_WAN_WIRELESS] = 1u;
    board_err(&a, "0");
    step(&a, 1000u);
    chk(strcmp(a.rp.env.err, "0") == 0, "k03 healthy -> \"0\"");

    /* ---- k04 老固件没有 ERR 键 ⇒ 只报网络码，不去拿上次的值 ---- */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    a.nm.wan_up[IV_WAN_WIRED] = 1u;
    a.nm.wan_up[IV_WAN_WIRELESS] = 1u;
    step(&a, 1000u); /* 镜像里 valid.ERR 一直是 0 */
    chk(strcmp(a.rp.env.err, "0") == 0, "k04 no ERR key -> network half only");

    /* ---- k05 出报那一刻仍可读（指针不悬空） ---- */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 0);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    board_err(&a, "1,10100000");
    step(&a, 1000u);
    {
        char  seg[IV_REPORT_SEG_MAX];
        int   n = iv_report_build_seg(&a.rp.st, &a.rp.env, 0u, 0x101u,
                                      "20250101000000", seg, sizeof seg);

        chk(n > 0, "k05 seg built from the persisted env");
        chk(strstr(seg, "ERR=3,10100000,20100000,20300000;") != NULL,
            "k05 ERR segment carries the merged list");
    }

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_fault FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_agent_fault passed (merge / board-reject / healthy / old-fw / env ptr)\n");
    return 0;
}
