/*
 * `netthr.*` 热生效（`iv_agent_reload_netthr` / `iv_agent_netthr_view`）单测
 * 2026-10-22
 *
 * 覆盖：
 *   n01 入参守卫：NULL / 未 open / 三项全被跳过（`-1` + `0` + `0`）
 *   n02 限次与冷却真的落到 `iv_recovery`（模块内部值，不是 `a->cfg` 自说自话）
 *   n03 **同值早返回**：`iv_recovery_set_limits()` 会重置每通道 `credit`，
 *       整类回调（改个别的不相干键也会进来）绝不能把自愈次数偷偷补满
 *   n04 只改冷却时 `reboot_max` 不动；限次变了则按模块规格重新发次数
 *   n05 `reboot_max = 0` 是**合法值**（禁用自愈），`-1` 才是"这次不动"
 *   n06 view 读的是模块真值，不是配置值（两者不一致正是本轮修复对象）
 *   n07 门限写入失败要如实上抛，不能"配置改了、模块没改"却回 OK
 *
 * 为什么这一层能测：`iv_agent_reload_netthr()` 的恢复分支只依赖 `a->rec`
 * （`iv_recovery_init()` 就能建好），不需要 `iv_agent_open()` 的串口/netlink。
 * 切换门限那半边需要 `iv_netmgr` 已 open，由装配层 e2e
 * （`tests/integration/netthr_e2e.py`）真拉进程取证。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_agent.h"
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

/* 造一个"已 open"的 agent 壳：只 init 自愈句柄，其余字段清零。
 * 刻意不调 `iv_agent_open()` —— 那要串口/netlink，与本次被测逻辑无关。 */
static void make_agent(iv_agent_t *a)
{
    memset(a, 0, sizeof(*a));
    iv_agent_init(a);
    iv_recovery_init(&a->rec, NULL); /* 默认：reboot_max=0（禁用）、cooldown=60 s */
    a->opened = 1;
}

static void n01_guards(void)
{
    iv_agent_t a;
    iv_agent_netthr_view_t v;

    chk(iv_agent_reload_netthr(NULL, 1, 60u, 3u) == IV_EINVAL,
        "n01 reload(NULL) -> EINVAL");

    make_agent(&a);
    a.opened = 0;
    chk(iv_agent_reload_netthr(&a, 1, 60u, 3u) == IV_ESTATE,
        "n01 reload before open -> ESTATE");
    chk(iv_agent_netthr_view(&a, &v) == IV_ESTATE,
        "n01 view before open -> ESTATE");

    make_agent(&a);
    /* 三项全被跳过 ⇒ 调用方参数错了，别假装成功 */
    chk(iv_agent_reload_netthr(&a, -1, 0u, 0u) == IV_EINVAL,
        "n01 all args skipped -> EINVAL");

    chk(iv_agent_netthr_view(NULL, &v) == IV_EINVAL, "n01 view(NULL a) -> EINVAL");
    chk(iv_agent_netthr_view(&a, NULL) == IV_EINVAL, "n01 view(NULL out) -> EINVAL");
}

static void n02_recovery_applied(void)
{
    iv_agent_t a;
    int        i;

    make_agent(&a);
    chk(a.rec.cfg.reboot_max == 0, "n02 default reboot_max 0 (自愈默认关)");
    chk(a.rec.cfg.cooldown_ms == IV_RECOVERY_COOLDOWN_MS_DEFAULT,
        "n02 default cooldown = 60 s");

    chk(iv_agent_reload_netthr(&a, 2, 30u, 0u) == IV_OK, "n02 reload -> OK");
    chk(a.rec.cfg.reboot_max == 2, "n02 recovery reboot_max = 2 (模块真值)");
    chk(a.rec.cfg.cooldown_ms == 30000u, "n02 recovery cooldown = 30000 ms");
    chk(a.cfg.netthr_reboot_max == 2u, "n02 snapshot reboot_max = 2");
    chk(a.cfg.netthr_reboot_interval_s == 30u, "n02 snapshot interval_s = 30");
    for (i = 0; i < (int)IV_RECOVERY_PORT_N; i++) {
        if (iv_recovery_port_credit(&a.rec, i) != 2) {
            chk(0, "n02 credit refilled to 2 on the first apply");
            return;
        }
    }
    chk(1, "n02 credit refilled to 2 on the first apply");
}

static void n03_same_value_early_return(void)
{
    iv_agent_t a;
    int        i;

    make_agent(&a);
    (void)iv_agent_reload_netthr(&a, 2, 30u, 0u);

    /* 模拟"已经用掉两次"：改配置之外，只有真的动作过才会消耗 credit。
     * 这里直接改模块字段就够 —— 被测的是"同值会不会被重置"。 */
    a.rec.ch[0].credit = 0;
    a.rec.ch[1].credit = 1;

    chk(iv_agent_reload_netthr(&a, 2, 30u, 0u) == IV_OK,
        "n03 same values -> OK");
    chk(iv_recovery_port_credit(&a.rec, 0) == 0,
        "n03 credit NOT reset when nothing changed (ch0)");
    chk(iv_recovery_port_credit(&a.rec, 1) == 1,
        "n03 credit NOT reset when nothing changed (ch1)");

    /* 对照组：只把冷却改一下 ⇒ 按 `iv_recovery_set_limits()` 的规格重新发满。
     * 这条同时说明"同值早返回"不是白加的：换值确实会重置。 */
    chk(iv_agent_reload_netthr(&a, -1, 45u, 0u) == IV_OK, "n03 change cooldown -> OK");
    for (i = 0; i < (int)IV_RECOVERY_PORT_N; i++) {
        if (iv_recovery_port_credit(&a.rec, i) != 2) {
            chk(0, "n03 changing limits DOES refill credit (对照)");
            return;
        }
    }
    chk(1, "n03 changing limits DOES refill credit (对照)");
    chk(a.rec.cfg.reboot_max == 2, "n03 reboot_max untouched when only cooldown changes");
    chk(a.rec.cfg.cooldown_ms == 45000u, "n03 cooldown -> 45000 ms");
}

static void n04_zero_means_disable(void)
{
    iv_agent_t a;

    make_agent(&a);
    (void)iv_agent_reload_netthr(&a, 3, 60u, 0u);
    chk(a.rec.cfg.reboot_max == 3, "n04 set reboot_max 3");

    /* `0` 是合法值 = 禁用自愈（对齐现网 net_reload=0），不是"不改" */
    chk(iv_agent_reload_netthr(&a, 0, 0u, 0u) == IV_OK, "n04 reboot_max 0 -> OK");
    chk(a.rec.cfg.reboot_max == 0, "n04 reboot_max 0 = 禁用自愈 (不是 skipped)");
    chk(iv_recovery_port_credit(&a.rec, 0) == 0, "n04 credit 0 when disabled");
    chk(a.rec.cfg.cooldown_ms == 60000u, "n04 cooldown 保持（0 秒 = 这次不动）");
}

static void n05_view_reads_modules(void)
{
    iv_agent_t a;
    iv_agent_netthr_view_t v;

    make_agent(&a);
    (void)iv_agent_reload_netthr(&a, 4, 15u, 0u);

    /* 只改模块内部字段，不碰 `a->cfg`：view 若去读配置就会看到 0。 */
    a.nm.cfg.fail_n = 9u;
    a.pb.cfg.fail_n = 5u;
    a.pb.cfg.degrade_rtt_ms = 1500u;

    chk(iv_agent_netthr_view(&a, &v) == IV_OK, "n05 view -> OK");
    chk(v.nm_fail_n == 9, "n05 view reads iv_netmgr (not config)");
    chk(v.probe_fail_n == 5, "n05 view reads iv_probe (not config)");
    chk(v.probe_delay_ms == 1500,
        "n05 view reads the live delay threshold (prove 0xFE really applied)");
    /* 没有探测项（`make_agent` 里 `wan_probe_idx` 全 -1）⇒ -1，不是 0。
     * 报 0 会让现场以为"0 ms，快得很"，把"没在测"伪装成"测得很好"。 */
    chk(v.probe_rtt_ms[0] == -1 && v.probe_rtt_ms[1] == -1,
        "n05 view reports -1 rtt when a WAN has no probe item");
    chk(v.reboot_max == 4, "n05 view reboot_max = 4");
    chk(v.cooldown_ms == 15000u, "n05 view cooldown = 15000 ms");
    chk(v.credit[0] == 4, "n05 view credit mirrors the recovery channels");
}

static void n06_threshold_error_propagates(void)
{
    iv_agent_t a;

    make_agent(&a);
    /* `iv_netmgr` 未 open ⇒ `iv_netmgr_set_fail_n()` 回 IV_ESTATE。
     * 必须如实上抛，且**不能**把 `a->cfg.nm_fail_n` 改成 5（那会让下次比对
     * 以为"已经是 5 了"，从此再也不重试 —— 静默不生效最难查）。 */
    chk(iv_agent_reload_netthr(&a, -1, 0u, 5u) == IV_ESTATE,
        "n06 netmgr not open -> ESTATE propagated");
    chk(a.cfg.nm_fail_n == 0u, "n06 snapshot NOT updated on failure");
}

int main(void)
{
    n01_guards();
    n02_recovery_applied();
    n03_same_value_early_return();
    n04_zero_means_disable();
    n05_view_reads_modules();
    n06_threshold_error_propagates();

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_netthr FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_agent_netthr passed (guards, recovery apply, same-value early "
           "return, 0=disable, view reads modules, error propagation)\n");
    return 0;
}
