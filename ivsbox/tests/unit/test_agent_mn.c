/*
 * 主网状态码（上报帧 `MN=` 两路）单测  2026-10-26
 *
 * 背景：`env_feed()` 里 MN 原先只有 0/1/2 三个值（未绑接口 / 状态机说可用 /
 * 其它），协议里定义的 3（网线断开）与 4（延时）从来没人填 —— 现场拔网线只看到
 * "断开"，"延时严重"更是完全看不出来。`iv_report_env_t::mn` 的取值表早就写好了
 * 五个值，是生产者没跟上。
 *
 * 覆盖：
 *   m01 未绑接口 ⇒ 0（平台看就是"未指定 IP"）
 *   m02 状态机可用、无劣化 ⇒ 1
 *   m03 状态机判死 ⇒ 2
 *   m04 **有线** carrier down ⇒ 3，且**优先于**状态机结论（物理事实更硬）
 *   m05 探测 DEGRADED（平均时延超门限）⇒ 4，且**不被**状态机的"可用"盖成 1
 *   m06 4G 掉载波仍报 2（无线没有"网线"这回事，不许误报 3）
 *   m07 故障沿：1→4（延时）**不**触发自愈，4→2 才触发
 *
 * 为什么这一层能测：`probe_drive()` 里 `iv_probe_tick()` 对未 start 的引擎直接
 * 返回 ESTATE ⇒ 本用例**不** start 探测引擎，就不会有真 socket、真计时，探测
 * 结论完全由 `iv_probe_note_result()` 注入 ⇒ 判定链可重复。
 * 状态机的迟滞那一层不在本用例里造（`wan_up[]` 直接摆），因为它已经有
 * `test_netmgr` 与 `test_agent_switch` 各自钉住了；这里只钉"三个输入 → 一个码"。
 *
 * 反向证伪（撤掉对应实现后哪条会红）：
 *   F1 `mn_state_of()` 去掉 carrier 那条 ⇒ m04 变 2；
 *   F2 去掉 DEGRADED 那条 ⇒ m05 变 1；
 *   F3 DEGRADED 那条排在 netmgr 之后 ⇒ m05 仍变 1（顺序敏感，故写成先判劣化）；
 *   F4 carrier 那条不区分有线/无线 ⇒ m06 变 3；
 *   F5 故障沿判据改回 `mn_last == 1 && st == 2` ⇒ m07 的"3/4 不触发"两条红。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_agent.h"
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

/* 目标 IP（10.0.0.1，网络序原样）：只是给 ICMP 项一个 dst，永远不会真发出去
 * —— 引擎不 start，tick 直接返回 ESTATE。 */
#define MN_DST 0x0100000Au

/* "已 open"的 agent 壳：刻意不调 `iv_agent_open()`（那要串口 / netlink / TCP）。
 * 自愈句柄按默认建（`reboot_max = 0` ＝ 禁用 ⇒ 触发时只累加 `disabled`，不发帧），
 * 这样"该不该触发"可以用 `disabled` 计数直接读出来。 */
static void make_agent(iv_agent_t *a)
{
    iv_probe_cfg_t pc;

    memset(a, 0, sizeof(*a));
    iv_agent_init(a);
    iv_recovery_init(&a->rec, NULL);
    iv_probe_cfg_default(&pc);
    pc.degrade_rtt_ms = 100u; /* 门限 100 ms：方便造"延时" */
    (void)iv_probe_init(&a->pb, &pc);
    a->opened = 1;
}

/* 给第 w 路绑一个接口并建一个探测项（与 `agent_probe_build()` 同款：
 * 配了目标就是 ICMP 项）。`link_up` 同时播给 probe 的链路表。 */
static void wan_bind(iv_agent_t *a, int w, int ifindex, int link_up)
{
    a->wan_ifindex[w] = ifindex;
    a->wan_link_up[w] = (uint8_t)link_up;
    (void)iv_probe_set_link(&a->pb, ifindex, link_up, 0u);
    a->wan_probe_idx[w] =
        iv_probe_add_icmp(&a->pb, MN_DST, ifindex, (w == 0) ? "wan usb0" : "wan eth0");
}

/* 灌两次成功探测（`ok_n` 默认 2 才判 UP/DEGRADED） */
static void wan_probe_ok(iv_agent_t *a, int w, uint32_t rtt_ms, uint64_t now)
{
    (void)iv_probe_note_result(&a->pb, a->wan_probe_idx[w], 1, rtt_ms, now);
    (void)iv_probe_note_result(&a->pb, a->wan_probe_idx[w], 1, rtt_ms, now + 1u);
}

static void step(iv_agent_t *a, uint64_t now)
{
    chk(iv_agent_step(a, now) == IV_OK, "step -> OK");
}

/* ---- m01 未绑接口 ---- */
static void m01_unbound(void)
{
    iv_agent_t a;

    make_agent(&a);
    step(&a, 1000u);
    chk(iv_agent_mn(&a, IV_WAN_WIRELESS) == 0, "m01 unbound wireless -> MN=0");
    chk(iv_agent_mn(&a, IV_WAN_WIRED) == 0, "m01 unbound wired -> MN=0");
}

/* ---- m02 正常 ---- */
static void m02_normal(void)
{
    iv_agent_t a;

    make_agent(&a);
    wan_bind(&a, IV_WAN_WIRELESS, 11, 1);
    wan_probe_ok(&a, IV_WAN_WIRELESS, 3u, 10u); /* 平均 3 ms << 门限 100 */
    a.nm.wan_up[IV_WAN_WIRELESS] = 1u;          /* 迟滞结论：可用 */
    step(&a, 1000u);
    chk(iv_agent_mn(&a, IV_WAN_WIRELESS) == 1, "m02 usable + fast rtt -> MN=1");
}

/* ---- m03 状态机判死 ---- */
static void m03_down(void)
{
    iv_agent_t a;

    make_agent(&a);
    wan_bind(&a, IV_WAN_WIRELESS, 11, 1);
    wan_probe_ok(&a, IV_WAN_WIRELESS, 3u, 10u);
    a.nm.wan_up[IV_WAN_WIRELESS] = 0u;
    step(&a, 1000u);
    chk(iv_agent_mn(&a, IV_WAN_WIRELESS) == 2, "m03 not usable -> MN=2");
}

/* ---- m04 有线拔线 ⇒ 3，且压过状态机 ---- */
static void m04_cable_down(void)
{
    iv_agent_t a;

    make_agent(&a);
    wan_bind(&a, IV_WAN_WIRED, 12, 0); /* carrier down */
    /* 状态机还没来得及判死（迟滞）—— 正是"拔线的那一拍"：物理事实已经有了，
     * 状态机还在等连续 N 次失败。这一格以前报 1（或 2），现在报 3。 */
    a.nm.wan_up[IV_WAN_WIRED] = 1u;
    step(&a, 1000u);
    chk(iv_agent_mn(&a, IV_WAN_WIRED) == 3, "m04 wired carrier down -> MN=3 (3 beats netmgr)");
}

/* ---- m05 延时 ⇒ 4，且不被 netmgr 的"可用"盖掉 ---- */
static void m05_delay(void)
{
    iv_agent_t a;

    make_agent(&a);
    wan_bind(&a, IV_WAN_WIRELESS, 11, 1);
    wan_probe_ok(&a, IV_WAN_WIRELESS, 300u, 10u); /* 平均 300 ms > 门限 100 */
    /* netmgr 把 DEGRADED 当"可用"（劣化不切链路）—— 板上就是这样，别改它 */
    a.nm.wan_up[IV_WAN_WIRELESS] = 1u;
    step(&a, 1000u);
    chk(iv_probe_state(&a.pb, a.wan_probe_idx[IV_WAN_WIRELESS]) == IV_PROBE_DEGRADED,
        "m05 probe really is DEGRADED");
    chk(iv_agent_mn(&a, IV_WAN_WIRELESS) == 4, "m05 degraded -> MN=4 (not 1)");
}

/* ---- m06 无线掉载波仍是 2，不是 3 ---- */
static void m06_wireless_no_cable(void)
{
    iv_agent_t a;

    make_agent(&a);
    wan_bind(&a, IV_WAN_WIRELESS, 11, 0);
    a.nm.wan_up[IV_WAN_WIRELESS] = 0u;
    step(&a, 1000u);
    chk(iv_agent_mn(&a, IV_WAN_WIRELESS) == 2,
        "m06 wireless carrier down -> MN=2 (4G has no cable)");
}

/* ---- m07 故障沿：延时不断继电器，真断才断 ---- */
static void m07_fault_edge(void)
{
    iv_agent_t a;

    make_agent(&a);
    wan_bind(&a, IV_WAN_WIRED, 12, 1);
    wan_probe_ok(&a, IV_WAN_WIRED, 3u, 10u);
    a.nm.wan_up[IV_WAN_WIRED] = 1u;
    step(&a, 100u);
    chk(iv_agent_mn(&a, IV_WAN_WIRED) == 1, "m07 baseline MN=1");
    chk(a.rec.disabled == 0u, "m07 no recovery action on the baseline");

    /* 1 → 4（延时严重）：**不**该断继电器。延时只是慢，链路还在；现场被"重启
     * 一次"的代价远大于慢一点。 */
    wan_probe_ok(&a, IV_WAN_WIRED, 300u, 200u);
    step(&a, 200u);
    chk(iv_agent_mn(&a, IV_WAN_WIRED) == 4, "m07 delayed -> MN=4");
    chk(a.rec.disabled == 0u, "m07 1->4 does NOT trigger the master-net relay");

    /* 4 → 2（真的断了）：这才是故障沿，自愈该动作（此壳 reboot_max=0 ⇒ 只记
     * `disabled`，不发 0xD1；"发了没有"由 test_agent_switch k04b 在真链路上钉住）。
     * 造"真断"用探测项判死（链路层事件），不是把门限调回 0 —— 那会走
     * `iv_probe_set_degrade()`，而它要求引擎已 start，本用例刻意不 start。 */
    a.nm.wan_up[IV_WAN_WIRED] = 0u;
    (void)iv_probe_set_link(&a.pb, 12, 0, 300u);
    step(&a, 300u);
    chk(iv_agent_mn(&a, IV_WAN_WIRED) == 2, "m07 back to MN=2");
    chk(a.rec.disabled == 1u, "m07 4->2 DOES trigger the master-net relay");
}

int main(void)
{
    m01_unbound();
    m02_normal();
    m03_down();
    m04_cable_down();
    m05_delay();
    m06_wireless_no_cable();
    m07_fault_edge();

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_mn FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_agent_mn passed (0 unbound / 1 normal / 2 down / 3 cable / 4 delay, "
           "precedence, fault edge)\n");
    return 0;
}
