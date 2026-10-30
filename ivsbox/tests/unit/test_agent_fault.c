/*
 * 故障码装配链单测（2026-10-28；**2026-10-30 按新口径重写**）
 * `env_feed()` → `iv_report_env_t::err`
 *
 * 背景：故障码判定与合并已经由 `test_fault` 钉死在纯函数层；本用例钉的是
 * **装配层那几行接线**——采集板镜像里的 `ERR` 原文有没有被喂进去、**两个
 * 「主网检测 IP」的结论有没有被一起算**、结果指针是不是指在 agent 自己的缓冲上
 * （`iv_report_set_env()` 只存指针，指向栈上临时量会在出报那一刻变垃圾）。
 *
 * ⚠ 2026-10-30 口径更正（现场事故）：**「主网1 / 主网2」＝`probe.target1/2`
 *   这两个「主网检测 IP」的 ping 结论**，不是 4G / 有线两条上行链路。
 *   依据：指令表 0xF8"前四个字节为主网1IP、后四个字节为主网2IP"，正常上报
 *   `MN` 的取值表也是"0=未指定IP 1=正常 2=断开 3=网线断开(只第一路) 4=延时"。
 *   改前按 `mn_state_of()`（有线/4G）映射，于是现场两个检测 IP 都不通、
 *   4G 却好好的 ⇒ 只上报了「主网1故障」。k06 就是这条的回归用例。
 *
 * 用例壳与 `test_agent_mn.c` 同款：不调 `iv_agent_open()`（那要串口 / netlink /
 * TCP），手摆 `wan_ifindex` / `wan_link_up` / `wan_probe_idx` 三个输入；
 * 「主网检测 IP」的结论直接摆 `pb.item[]` 的 `round_*` 四件（`round_state_of()`
 * 只看这四个 ＋ `cfg.probe_delay_ms`），不起 socket、不需要 root。
 *
 * 覆盖：
 *   k01 有线载波掉 ＋ 采集板两条 ⇒ 合并升序（主网码**不**由链路状态产生）
 *   k02 采集板送网络码 ⇒ 被剔除（网络以主板为准）
 *   k03 全正常 ⇒ `0`（与参考实现 `ERR=0;` 一致）
 *   k04 老固件（0xE1 应答没有 ERR 键）⇒ 只报主板网络码，不拿旧值冒充
 *   k05 指针落在 agent 内部缓冲上（出报时仍可读）
 *   k06 **两个检测 IP 都不通 ⇒ 主网1故障 ＋ 主网2故障同时在**（现场 bug 回归）
 *   k07 检测 IP 延时 ⇒ 主网N延时告警（证明"3 延时"被翻成 4）
 *   k08 检测 IP 丢包（部分响应）⇒ 主网1丢包告警（参考固件对 LOSS 也置码）
 *   k09 上报帧 `MN=` 两位 = 两个检测 IP 的状态（含网线断开→3/延时→4/丢包→5）
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

/* 摆第 i 个「主网检测 IP」的结论（只动 `round_*` 四件，与 `round_state_of()`
 * 的输入完全一致）：
 *   ok=4 lost=0 ⇒ 通      ok=0      ⇒ 不通（4 包全无响应）
 *   ok=3 lost=1 ⇒ 丢包    avg > 门限 ⇒ 延时 */
static void tgt_set(iv_agent_t *a, int i, uint32_t ok, uint32_t lost,
                    uint32_t avg)
{
    a->pb.item[i].round_ok         = ok;
    a->pb.item[i].round_lost       = lost;
    a->pb.item[i].round_avg_rtt_ms = avg;
    a->pb.item[i].round_seq        = 1u; /* 已结算过一轮（0＝未知，不判） */
    if (a->pb.n_items < i + 1)
        a->pb.n_items = i + 1;
    a->tgt_probe_idx[i]    = i;
    a->cfg.probe_target_be[i] = 0xC0000201u + (uint32_t)i; /* 192.0.2.1 / .2 */
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

    /* ---- k01 载波掉（LAN 口故障）＋ 采集板两条 ⇒ 合并升序 ----
     * **主网码不由链路状态产生**：这一壳里两个检测 IP 都没配（`tgt_probe_idx` 全 -1），
     * 所以一条主网码都不该出现 —— 这正是 10-30 改掉的那条错误映射。 */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 0);     /* 有线：载波掉 ⇒ LAN 口码 */
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);  /* 4G：无探测项也不会再出主网2 故障 */
    board_err(&a, "2,30100000,10100000");
    step(&a, 1000u);
    chk(strcmp(a.rp.env.err, "3,10100000,20100000,30100000") == 0,
        "k01 LAN + board codes merged ascending, no main-net code from link state");
    chk(a.rp.env.err == a.err_buf, "k01 err points at the agent-owned buffer");

    /* ---- k02 采集板送网络码 ⇒ 剔除（网络只由主板出） ---- */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    board_err(&a, "2,20100000,30100000"); /* 其中 20100000 是网络码 */
    step(&a, 1000u);
    chk(a.flt.board_rejected == 1u, "k02 board network code rejected");
    chk(strcmp(a.rp.env.err, "1,30100000") == 0,
        "k02 output has the board's sensor code only");

    /* ---- k03 全正常 ⇒ "0" ---- */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    a.nm.wan_up[IV_WAN_WIRED] = 1u;
    a.nm.wan_up[IV_WAN_WIRELESS] = 1u;
    tgt_set(&a, 0, 4u, 0u, 3u);
    tgt_set(&a, 1, 4u, 0u, 4u);
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
        chk(strstr(seg, "ERR=2,10100000,20100000;") != NULL,
            "k05 ERR segment carries the merged list");
    }

    /* ---- k06 【现场回归】两个检测 IP 都不通 ⇒ 主网1故障 ＋ 主网2故障 ----
     * 2026-10-30 现场：界面上两个检测 IP 都显示"不通"，平台上只有「主网1故障」。
     * 这条用例就是那个场景：**两条上行链路都正常**（载波在、netmgr 判 up），
     * 只有两个检测 IP 不可达。 */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    a.nm.wan_up[IV_WAN_WIRED] = 1u;
    a.nm.wan_up[IV_WAN_WIRELESS] = 1u;
    tgt_set(&a, 0, 0u, 4u, 0u); /* 检测 IP 1：4 包全无响应 ⇒ 不通 */
    tgt_set(&a, 1, 0u, 4u, 0u); /* 检测 IP 2：同上 */
    step(&a, 1000u);
    chk(strcmp(a.rp.env.err, "2,20200000,20300000") == 0,
        "k06 both probe targets dead -> main1 AND main2 fault");
    chk(iv_fault_has(&a.flt, IV_FAULT_NET_MAIN_FAULT),
        "k06 has 0x20200000 main1 fault");
    chk(iv_fault_has(&a.flt, IV_FAULT_NET_MAIN2_FAULT),
        "k06 has 0x20300000 main2 fault");

    /* ---- k07 检测 IP 延时 ⇒ 主网N延时告警（证明 3=延时 被翻成 4） ----
     * 若照抄四态码（3）传下去，`iv_fault_apply_net()` 会把它当"网线断开"，
     * 主网1 那条就**不置码**了 —— 慢链路被显示成"没故障"。 */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    a.nm.wan_up[IV_WAN_WIRED] = 1u;
    a.nm.wan_up[IV_WAN_WIRELESS] = 1u;
    a.cfg.probe_delay_ms = 100u;
    tgt_set(&a, 0, 4u, 0u, 260u); /* 全响应、平均 260 ms > 100 ms 门限 */
    tgt_set(&a, 1, 4u, 0u, 20u);  /* 正常 */
    step(&a, 1000u);
    chk(strcmp(a.rp.env.err, "1,20500000") == 0,
        "k07 slow target -> main1 delay warning (0x20500000), not a fault");
    chk(!iv_fault_has(&a.flt, IV_FAULT_NET_MAIN_FAULT),
        "k07 delay must not be reported as main1 fault");

    /* ---- k08 丢包（部分响应）⇒ 主网1丢包告警 ----
     * 参考固件 `det.c` 对 `NET_STATUS_LOSS(5)` 是 `Error_Set(NET_MAIN_LOSS)`，
     * 所以丢包**要**置码（2026-10-30 对齐）。原先这里断言 `"0"`，是照"丢包只检测
     * 不上报"那套自创口径写的。 */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    a.nm.wan_up[IV_WAN_WIRED] = 1u;
    a.nm.wan_up[IV_WAN_WIRELESS] = 1u;
    tgt_set(&a, 0, 3u, 1u, 8u); /* 4 包里回了 3 个 */
    tgt_set(&a, 1, 4u, 0u, 8u);
    step(&a, 1000u);
    chk(strcmp(a.rp.env.err, "1,20800000") == 0,
        "k08 packet loss -> main1 loss warning 0x20800000");
    chk(iv_fault_has(&a.flt, IV_FAULT_NET_MAIN_LOSS), "k08 has loss code");
    chk(!iv_fault_has(&a.flt, IV_FAULT_NET_MAIN_FAULT),
        "k08 loss must not be reported as main1 fault");

    /* ---- k09 上报帧 `MN=` 两位 = 两个「主网检测 IP」的状态 ----
     * 与参考固件 `com_report_get_main_network_status(0/1)` 逐值对齐：
     *   0 无IP(没配) / 1 正常 / 2 故障 / 3 网线断开(**只主网1**) / 4 延时 / 5 丢包
     * 2026-10-30 之前这里灌的是 4G / 有线两条链路的结论，于是现场两个检测 IP 都
     * 不通、两条链路都好好的时候，平台看到的是 "MN=3,x"（网线断开）＋ 只报主网1故障。 */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    tgt_set(&a, 0, 4u, 0u, 8u); /* 检测 IP 1：通 */
    tgt_set(&a, 1, 0u, 4u, 0u); /* 检测 IP 2：4 包全无响应 ⇒ 故障 */
    step(&a, 1000u);
    chk(a.rp.env.mn[0] == 1 && a.rp.env.mn[1] == 2,
        "k09 MN = 1,2 from the two probe targets (not from wan links)");

    /* 网线断开 ⇒ 主网1 报 3（参考固件载波 down 直接给 3，不看 ping 结论），
     * 主网2 没有网线这回事 ⇒ 仍是自己的 ping 结论。 */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 0); /* 有线载波掉 */
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    tgt_set(&a, 0, 4u, 0u, 8u);
    tgt_set(&a, 1, 4u, 0u, 8u);
    step(&a, 1000u);
    chk(a.rp.env.mn[0] == 3, "k09 wired carrier down -> MN[0]=3 (main1 only)");
    chk(a.rp.env.mn[1] == 1, "k09 MN[1] unaffected by the cable");

    /* 延时 ⇒ 4、丢包 ⇒ 5（与四态码 3＝延时 / 4＝丢包 **不同**，这里必须翻译） */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    a.cfg.probe_delay_ms = 100u;
    tgt_set(&a, 0, 4u, 0u, 260u); /* 全响应、平均超门限 ⇒ 延时 */
    tgt_set(&a, 1, 3u, 1u, 8u);   /* 只回了一部分 ⇒ 丢包 */
    step(&a, 1000u);
    chk(a.rp.env.mn[0] == 4 && a.rp.env.mn[1] == 5,
        "k09 MN encodes delay as 4 and packet-loss as 5");

    /* 检测 IP 没配 ⇒ 0（未指定 IP），不是"故障" */
    make_agent(&a);
    wan_set(&a, IV_WAN_WIRED, 12, 1);
    wan_set(&a, IV_WAN_WIRELESS, 11, 1);
    step(&a, 1000u);
    chk(a.rp.env.mn[0] == 0 && a.rp.env.mn[1] == 0,
        "k09 unconfigured targets -> MN=0,0 (no IP)");

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_fault FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_agent_fault passed (merge / board-reject / healthy / old-fw / "
           "env ptr / both-targets-down / delay / loss / MN encoding)\n");
    return 0;
}
