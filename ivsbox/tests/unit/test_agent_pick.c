/*
 * 平台地址选择链（`iv_agent_pick_host`）单测 —— 2026-10-22
 *
 * 被测的是"备地址"这一步落地后的选址规则：
 *
 *     本口主 → 本口备 → 另一口主 → 另一口备
 *
 * 覆盖：
 *   p01 本口主有效 ⇒ 取主（备地址再全也不参与）
 *   p02 本口主没配、本口备有效 ⇒ 取备
 *   p03 本口主备都空 ⇒ 回退另一口主
 *   p04 只剩另一口备 ⇒ 取另一口备（链尾）
 *   p05 四条全无效 ⇒ NULL ＋ `*port = 0`（不启平台链路）
 *   p06 端口 0 视作"没配"（与既有的 host/port 口径一致），不是"连 0 端口"
 *   p07 **不切换**：主地址有效时，即使备地址"看起来更能连"（这里根本无法表达
 *       "连不上"）也绝不改选 —— 纯函数没有状态，同样的输入永远同样的答案
 *   p08 两个出口的对称性（wired 活动口时链序跟着换）
 *   p09 入参守卫：NULL cfg / 非法出口编号 / NULL 出参
 *
 * 为什么这一层能纯单测：选址是**纯函数**（只读 `iv_agent_cfg_t`，不看 netmgr
 * 状态、不碰 socket）。真正"换出口时重选并生效"的接线属装配层，由
 * `tests/integration/` 的 e2e 取证 —— 单测在这里只钉"选谁"这条规则本身。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_agent.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 每例都从"四条全空"起步，再按需填 —— 免得上一例的残留把下一例伪装成绿的。 */
static void pick_reset(iv_agent_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
}

static void p01_primary_wins(void)
{
    iv_agent_cfg_t c;
    uint16_t       port = 0u;
    const char    *h;

    pick_reset(&c);
    c.srv_host[IV_WAN_WIRELESS]     = "10.0.0.1";
    c.srv_port[IV_WAN_WIRELESS]     = 7001u;
    c.srv_host_bak[IV_WAN_WIRELESS] = "10.0.0.2";
    c.srv_port_bak[IV_WAN_WIRELESS] = 7002u;
    c.srv_host[IV_WAN_WIRED]        = "10.0.1.1";
    c.srv_port[IV_WAN_WIRED]        = 7003u;
    c.srv_host_bak[IV_WAN_WIRED]    = "10.0.1.2";
    c.srv_port_bak[IV_WAN_WIRED]    = 7004u;

    h = iv_agent_pick_host(&c, IV_WAN_WIRELESS, &port);
    chk(h != NULL && strcmp(h, "10.0.0.1") == 0, "p01 wireless active -> own primary");
    chk(port == 7001u, "p01 port follows the chosen host");

    /* 返回值必须是 cfg 内部的指针（调用方按"指针一直有效"用），不是静态缓冲 */
    chk(h == c.srv_host[IV_WAN_WIRELESS], "p01 returns the cfg pointer, not a copy");
}

static void p02_own_backup_when_primary_missing(void)
{
    iv_agent_cfg_t c;
    uint16_t       port = 0u;
    const char    *h;

    pick_reset(&c);
    /* 本口主：host 空串（出厂口径就是空） */
    c.srv_host[IV_WAN_WIRELESS]     = "";
    c.srv_port[IV_WAN_WIRELESS]     = 6012u;
    c.srv_host_bak[IV_WAN_WIRELESS] = "10.0.0.9";
    c.srv_port_bak[IV_WAN_WIRELESS] = 6012u;
    /* 另一口主也配了 —— 必须**不**被选中：本口备优先于另一口主 */
    c.srv_host[IV_WAN_WIRED]        = "10.0.1.1";
    c.srv_port[IV_WAN_WIRED]        = 6012u;

    h = iv_agent_pick_host(&c, IV_WAN_WIRELESS, &port);
    chk(h != NULL && strcmp(h, "10.0.0.9") == 0,
        "p02 own backup beats the other port's primary");
    chk(port == 6012u, "p02 backup port");
}

static void p03_fallback_to_other_primary(void)
{
    iv_agent_cfg_t c;
    uint16_t       port = 0u;
    const char    *h;

    pick_reset(&c);
    c.srv_host[IV_WAN_WIRELESS]     = NULL;
    c.srv_host_bak[IV_WAN_WIRELESS] = "";
    c.srv_host[IV_WAN_WIRED]        = "10.0.1.1";
    c.srv_port[IV_WAN_WIRED]        = 6012u;
    c.srv_host_bak[IV_WAN_WIRED]    = "10.0.1.2";
    c.srv_port_bak[IV_WAN_WIRED]    = 6012u;

    h = iv_agent_pick_host(&c, IV_WAN_WIRELESS, &port);
    chk(h != NULL && strcmp(h, "10.0.1.1") == 0,
        "p03 nothing on the active port -> other port's primary");
    chk(port == 6012u, "p03 fallback port");
}

static void p04_other_backup_is_the_tail(void)
{
    iv_agent_cfg_t c;
    uint16_t       port = 0u;
    const char    *h;

    pick_reset(&c);
    c.srv_host[IV_WAN_WIRELESS]     = "";
    c.srv_host_bak[IV_WAN_WIRELESS] = "";
    c.srv_host[IV_WAN_WIRED]        = "";
    c.srv_port[IV_WAN_WIRED]        = 6012u;
    c.srv_host_bak[IV_WAN_WIRED]    = "10.0.1.2";
    c.srv_port_bak[IV_WAN_WIRED]    = 6100u;

    h = iv_agent_pick_host(&c, IV_WAN_WIRELESS, &port);
    chk(h != NULL && strcmp(h, "10.0.1.2") == 0,
        "p04 the tail of the chain is the other port's backup");
    chk(port == 6100u, "p04 tail port");
}

static void p05_all_empty_disables_the_link(void)
{
    iv_agent_cfg_t c;
    uint16_t       port = 4242u; /* 预置脏值：必须被改写，否则调用方会拿旧端口去连 */

    pick_reset(&c);
    chk(iv_agent_pick_host(&c, IV_WAN_WIRELESS, &port) == NULL,
        "p05 four empty addresses -> NULL");
    chk(port == 0u, "p05 *port zeroed on miss (no stale port leaks out)");

    /* 只有 host 没有 port 也算没配 */
    port = 4242u;
    c.srv_host[IV_WAN_WIRELESS] = "10.0.0.1";
    c.srv_port[IV_WAN_WIRELESS] = 0u;
    chk(iv_agent_pick_host(&c, IV_WAN_WIRELESS, &port) == NULL,
        "p05 host without port is NOT an address");
    chk(port == 0u, "p05 port stays 0 when the host has no port");
}

static void p06_zero_port_skips_to_the_next_link(void)
{
    iv_agent_cfg_t c;
    uint16_t       port = 0u;
    const char    *h;

    pick_reset(&c);
    /* 本口主：有地址但端口写成 0（现场误填）⇒ 跳过，落本口备 */
    c.srv_host[IV_WAN_WIRELESS]     = "10.0.0.1";
    c.srv_port[IV_WAN_WIRELESS]     = 0u;
    c.srv_host_bak[IV_WAN_WIRELESS] = "10.0.0.9";
    c.srv_port_bak[IV_WAN_WIRELESS] = 6012u;

    h = iv_agent_pick_host(&c, IV_WAN_WIRELESS, &port);
    chk(h != NULL && strcmp(h, "10.0.0.9") == 0,
        "p06 port==0 on the primary skips it (not 'connect to port 0')");
    chk(port == 6012u, "p06 port comes from the link that was actually chosen");
}

static void p07_no_failover_state(void)
{
    iv_agent_cfg_t c;
    uint16_t       p1 = 0u;
    uint16_t       p2 = 0u;
    const char    *h1;
    const char    *h2;

    pick_reset(&c);
    c.srv_host[IV_WAN_WIRELESS]     = "10.0.0.1";
    c.srv_port[IV_WAN_WIRELESS]     = 6012u;
    c.srv_host_bak[IV_WAN_WIRELESS] = "10.0.0.9";
    c.srv_port_bak[IV_WAN_WIRELESS] = 6012u;

    /* 「不切换，只作为备份」（用户 2026-10-22 拍板）：主地址"连不上"这件事
     * 根本不作为输入 —— 连调两次必须一模一样。若哪天有人往里塞"主地址失败
     * 计数"做自动切换，第二条断言就会翻（返回值变 10.0.0.9）。 */
    h1 = iv_agent_pick_host(&c, IV_WAN_WIRELESS, &p1);
    h2 = iv_agent_pick_host(&c, IV_WAN_WIRELESS, &p2);
    chk(h1 != NULL && strcmp(h1, "10.0.0.1") == 0, "p07 primary is always chosen");
    chk(h2 == h1 && p2 == p1, "p07 the picker keeps NO state (no failover)");
}

static void p08_both_directions(void)
{
    iv_agent_cfg_t c;
    uint16_t       port = 0u;
    const char    *h;

    pick_reset(&c);
    c.srv_host[IV_WAN_WIRELESS]     = "10.0.0.1";
    c.srv_port[IV_WAN_WIRELESS]     = 6012u;
    c.srv_host_bak[IV_WAN_WIRELESS] = "10.0.0.9";
    c.srv_port_bak[IV_WAN_WIRELESS] = 6012u;
    c.srv_host[IV_WAN_WIRED]        = "10.0.1.1";
    c.srv_port[IV_WAN_WIRED]        = 6012u;
    c.srv_host_bak[IV_WAN_WIRED]    = "10.0.1.9";
    c.srv_port_bak[IV_WAN_WIRED]    = 6012u;

    h = iv_agent_pick_host(&c, IV_WAN_WIRED, &port);
    chk(h != NULL && strcmp(h, "10.0.1.1") == 0,
        "p08 wired active -> own primary (the chain mirrors)");

    /* 有线口自己的两条都空 ⇒ 回退到无线口的**主**，而不是有线口的备 */
    c.srv_host[IV_WAN_WIRED]     = "";
    c.srv_host_bak[IV_WAN_WIRED] = "";
    h = iv_agent_pick_host(&c, IV_WAN_WIRED, &port);
    chk(h != NULL && strcmp(h, "10.0.0.1") == 0,
        "p08 wired empty -> wireless primary");

    /* 再让无线主也空 ⇒ 落到无线备（有线口已无候选） */
    c.srv_host[IV_WAN_WIRELESS] = "";
    h = iv_agent_pick_host(&c, IV_WAN_WIRED, &port);
    chk(h != NULL && strcmp(h, "10.0.0.9") == 0,
        "p08 last resort -> the other port's backup");
}

static void p09_guards(void)
{
    iv_agent_cfg_t c;
    uint16_t       port = 4242u;

    pick_reset(&c);
    c.srv_host[IV_WAN_WIRELESS] = "10.0.0.1";
    c.srv_port[IV_WAN_WIRELESS] = 6012u;

    chk(iv_agent_pick_host(NULL, IV_WAN_WIRELESS, &port) == NULL, "p09 cfg NULL -> NULL");
    chk(iv_agent_pick_host(&c, -1, &port) == NULL, "p09 invalid wan (-1) -> NULL");
    chk(iv_agent_pick_host(&c, IV_WAN_N, &port) == NULL, "p09 wan out of range -> NULL");
    chk(port == 0u, "p09 port zeroed even on the guard path");

    /* port 出参可以为 NULL：只问地址、不关心端口 */
    chk(iv_agent_pick_host(&c, IV_WAN_WIRELESS, NULL) != NULL,
        "p09 NULL port out-param is allowed");
}

int main(void)
{
    p01_primary_wins();
    p02_own_backup_when_primary_missing();
    p03_fallback_to_other_primary();
    p04_other_backup_is_the_tail();
    p05_all_empty_disables_the_link();
    p06_zero_port_skips_to_the_next_link();
    p07_no_failover_state();
    p08_both_directions();
    p09_guards();

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_pick FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_agent_pick passed (4-link priority chain, no failover state, "
           "guards)\n");
    return 0;
}
