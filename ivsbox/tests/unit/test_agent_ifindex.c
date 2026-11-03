/*
 * WAN 网卡 ifindex 重解析单测  2026-11-03
 *
 * 背景（现场现象）：本地 Web「网络」页同一屏里出现「4G 断开」＋「平台连接 已连接」。
 * 根因不是链路：`wan_ifindex[]` 原先**只在 `iv_agent_open()` 里解析一次**，而 4G
 * 模组开机 80 s 左右会注销再注册 usb0（板端 dmesg：`[2.62] register` →
 * `[80.70] unregister` → `[82.44] register`），ifindex 8 → 9。缓存卡在 8 之后：
 *   · netlink 的 LINK_UP 带新索引 ⇒ 与缓存对不上 ⇒ `wan_link_up[]` 恒 0；
 *   · 探测项绑在旧索引 ⇒ 发不出也收不到 ⇒ 该项恒"不通"；
 *   ⇒ `mn_state_of()` 恒 2 ⇒ 界面「4G 断开」；而平台 TCP 按**网卡名**绑，
 *     内核总能解析到 9 ⇒ 照样连上、照样上报。
 *
 * 覆盖：
 *   i01 索引被重新解析（stale 12345 -> 真实值）并返回变化路数 1
 *   i02 幂等：再调一次返回 0（否则每拍都重建探测表 ⇒ 探测永远攒不满一轮）
 *   i03 **空窗不改缓存**：`if_nametoindex()` 返回 0（注销与再注册之间）时保留旧值
 *   i04 变化后探测表被重建（该项有下标）＋ 链路现状被重播（`wan_link_up` 跟上）
 *   i05 netmgr 侧的 ifindex 同步（切换事务按它删/建默认路由）
 *   i06 未 open 返回 0
 *
 * 为什么用真实网卡名：本函数唯一的外部事实就是 `if_nametoindex()`，注入假解析器
 * 等于把被测的那一步换掉。`lo`（ifindex 1）在任何 Linux 上都在；不存在的名字用
 * `ivsb0x` 造"空窗"。
 *
 * 反向证伪（撤掉实现后哪条会红）：
 *   F1 去掉 `wan_ifindex[w] = ni` 那行        ⇒ i01 红（值仍 12345）
 *   F2 去掉 `ni == 0` 的跳过                  ⇒ i03 红（缓存被清成 0）
 *   F3 去掉 `agent_probe_rebuild()`           ⇒ i04 红（无探测项）
 *   F4 去掉 `nm.cfg.wan[w].ifindex = ni`      ⇒ i05 红
 *   F5 `changed` 恒返回 1                     ⇒ i02 红
 */
#include <net/if.h>
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 已 open 的 agent 壳：刻意不调 `iv_agent_open()`（那要串口 / netlink / TCP）。
 * `iv_agent_init()` 之后自己置 opened，与 test_agent_mn 同一套做法。 */
static void make_agent(iv_agent_t *a)
{
    iv_probe_cfg_t pc;

    memset(a, 0, sizeof(*a));
    iv_agent_init(a);
    iv_probe_cfg_default(&pc);
    (void)iv_probe_init(&a->pb, &pc);
    a->opened = 1;
}

int main(void)
{
    iv_agent_t a;
    int        base;

    /* ---- i01：索引被重新解析 ---- */
    {
        make_agent(&a);
        (void)snprintf(a.cfg.wan_name[IV_WAN_WIRED], sizeof(a.cfg.wan_name[IV_WAN_WIRED]),
                       "%s", "lo");
        a.wan_ifindex[IV_WAN_WIRED] = 12345; /* 模拟"打开时是 8、现在内核是 9" */

        chk(iv_agent_rescan_ifindex(&a, 100u) == 1, "i01 one wan re-resolved");
        base = a.wan_ifindex[IV_WAN_WIRED];
        chk(base > 0 && base != 12345, "i01 ifindex updated to the real one");
        chk(base == (int)if_nametoindex("lo"), "i01 ifindex == if_nametoindex(lo)");

        /* ---- i02：幂等 ---- */
        chk(iv_agent_rescan_ifindex(&a, 200u) == 0, "i02 second call is a no-op");

        /* ---- i04：探测表重建 ＋ 链路现状重播 ---- */
        chk(a.wan_probe_idx[IV_WAN_WIRED] >= 0, "i04 probe item rebuilt");
        chk(a.wan_link_up[IV_WAN_WIRED] == 1u, "i04 link state re-seeded from the new ifindex");

        /* ---- i05：netmgr 侧同步 ---- */
        chk(a.nm.cfg.wan[IV_WAN_WIRED].ifindex == base, "i05 netmgr ifindex synced");
    }

    /* ---- i03：空窗（解析不到 = 0）不许改缓存 ---- */
    {
        make_agent(&a);
        (void)snprintf(a.cfg.wan_name[IV_WAN_WIRELESS],
                       sizeof(a.cfg.wan_name[IV_WAN_WIRELESS]), "%s", "ivsb0x");
        a.wan_ifindex[IV_WAN_WIRELESS] = 8;

        chk(iv_agent_rescan_ifindex(&a, 100u) == 0, "i03 unknown iface -> no change");
        chk(a.wan_ifindex[IV_WAN_WIRELESS] == 8, "i03 cached ifindex kept during the gap");
        chk(a.nm.cfg.wan[IV_WAN_WIRELESS].ifindex == 0,
            "i03 netmgr not told to unbind while the iface is merely re-enumerating");
    }

    /* ---- i06：未 open ---- */
    {
        iv_agent_t b;

        memset(&b, 0, sizeof(b));
        iv_agent_init(&b);
        (void)snprintf(b.cfg.wan_name[IV_WAN_WIRED], sizeof(b.cfg.wan_name[IV_WAN_WIRED]),
                       "%s", "lo");
        b.wan_ifindex[IV_WAN_WIRED] = 12345;
        chk(iv_agent_rescan_ifindex(&b, 100u) == 0, "i06 not open -> 0");
        chk(b.wan_ifindex[IV_WAN_WIRED] == 12345, "i06 not open -> untouched");
        chk(iv_agent_rescan_ifindex(NULL, 100u) == 0, "i06 NULL -> 0");
    }

    /* ---- 真实板端场景的正向复现：ifindex 变了就必须变成"能判通" ---- */
    {
        make_agent(&a);
        (void)snprintf(a.cfg.wan_name[IV_WAN_WIRELESS],
                       sizeof(a.cfg.wan_name[IV_WAN_WIRELESS]), "%s", "lo");
        a.wan_ifindex[IV_WAN_WIRELESS] = 8; /* 板上：open 时 8，模组重枚举后 9 */

        chk(iv_agent_rescan_ifindex(&a, 100u) == 1, "i07 wireless re-enumerated");
        chk(a.wan_ifindex[IV_WAN_WIRELESS] == (int)if_nametoindex("lo"),
            "i07 wireless now points at the new ifindex");
        /* 探测项挂到新索引上：本壳没配平台地址 ⇒ 建的是 LINK 项，同样有下标 */
        chk(a.wan_probe_idx[IV_WAN_WIRELESS] >= 0, "i07 probe item follows the new ifindex");
    }

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_ifindex FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_agent_ifindex passed (re-resolve / idempotent / gap-keeps-cache / "
           "probe rebuild / netmgr sync)\n");
    return 0;
}
