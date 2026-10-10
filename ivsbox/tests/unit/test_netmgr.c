/*
 * iv_netmgr 单测（libivmodules，M3-S3.3）
 *
 * ============================ 测法 ============================
 * 全程**注入式假七步事务**（fake_t）：把 §7.2 的每个执行点换成"计数器 + 可脚本
 * 返回值"，于是状态迁移、迟滞判定、事务顺序与参数、失败回滚、hold_s 回切全部
 * 可确定性断言 —— 不用真实网卡、不要权限，本机 / VM / 板端结果一致。时间一律
 * 由 now_ms 显式喂入，不取墙钟。
 * **真实默认路由切换（netlink）不在此做**：按本工程既有惯例，用一次性驱动在编译
 * VM 的 netns 内实测后删除（见本轮 docs/修改记录.md）。
 *
 * ============================ 反向证伪点（每条对应一个真机制） ============================
 *   c03 七步事务必须**按序**执行，且 `close_old_tcp` 收到的必须是**旧** ifindex ——
 *       若把旧口传成新口、或把关连接排到切路由之前，seq / 参数断言必红
 *       （关错连接 = 平台连接不重建）；
 *   c04 媒体恢复**只能在稳定期到点后**由 tick 触发 —— 若切完立即恢复，c04 的
 *       "未到点不恢复"必红；
 *   c05 事务失败必须**回滚 active 并设重试窗口** —— 若失败仍把 active 切过去、或
 *       窗口内每个 tick 都重试，c05 的 active / switch_calls 断言必红；
 *   c06 回切必须**等满 hold_s** —— 若去掉 hold 判断，首个"未到点"的 tick 就会切回，必红；
 *   c02 fail_n / ok_n 必须**迟滞**（连续 N 次才翻转）—— 若改成单次探活即翻，c02 必红。
 *
 * 临时产物：无（全内存、无文件、无 fork）。AGENTS.md 规则 6。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_netmgr.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 假七步事务各步的调用序号（用于断言"按序"） */
#define SEQ_FREEZE 1
#define SEQ_SWITCH 2
#define SEQ_CLOSE  3
#define SEQ_RECONN 4
#define SEQ_RESUME 5

typedef struct {
    iv_netmgr_txn_ops_t ops; /* 回调经它注入，arg 指向本结构 */

    int      seq[24];
    int      seq_n;

    int      freeze_ret, switch_ret, close_ret, reconnect_ret, resume_ret;

    int      freeze_calls, switch_calls, close_calls, reconnect_calls, resume_calls;

    int      switch_from, switch_to;
    uint32_t switch_gw;
    int      close_if;
    int      reconn_if;
    int      resume_if;
} fake_t;

static void fo_push(fake_t *f, int tag)
{
    if (f->seq_n < (int)(sizeof(f->seq) / sizeof(f->seq[0]))) {
        f->seq[f->seq_n++] = tag;
    }
}

static int fo_freeze(void *arg)
{
    fake_t *f = (fake_t *)arg;
    fo_push(f, SEQ_FREEZE);
    f->freeze_calls++;
    return f->freeze_ret;
}

static int fo_switch(void *arg, int from_ifindex, int to_ifindex, uint32_t to_gateway_be)
{
    fake_t *f = (fake_t *)arg;
    fo_push(f, SEQ_SWITCH);
    f->switch_calls++;
    f->switch_from = from_ifindex;
    f->switch_to   = to_ifindex;
    f->switch_gw   = to_gateway_be;
    return f->switch_ret;
}

static int fo_close(void *arg, int old_ifindex)
{
    fake_t *f = (fake_t *)arg;
    fo_push(f, SEQ_CLOSE);
    f->close_calls++;
    f->close_if = old_ifindex;
    return f->close_ret;
}

static int fo_reconn(void *arg, int new_ifindex)
{
    fake_t *f = (fake_t *)arg;
    fo_push(f, SEQ_RECONN);
    f->reconnect_calls++;
    f->reconn_if = new_ifindex;
    return f->reconnect_ret;
}

static int fo_resume(void *arg, int ifindex)
{
    fake_t *f = (fake_t *)arg;
    fo_push(f, SEQ_RESUME);
    f->resume_calls++;
    f->resume_if = ifindex;
    return f->resume_ret;
}

static void fake_init(fake_t *f)
{
    (void)memset(f, 0, sizeof(*f));
    f->ops.freeze_low_prio = fo_freeze;
    f->ops.switch_route    = fo_switch;
    f->ops.close_old_tcp   = fo_close;
    f->ops.reconnect       = fo_reconn;
    f->ops.resume_media    = fo_resume;
    f->ops.arg             = f;
}

/* ---- 固定测试参数（网络序网关字面量：10.0.0.1 / 10.0.0.2） ---- */
#define WL_IF 7
#define WD_IF 9
#define WL_GW 0x0100000Au
#define WD_GW 0x0200000Au

#define FAIL_N 3u
#define OK_N   2u
#define HOLD_S 30u
#define STABLE_S 10u

static void cfg_auto(iv_netmgr_cfg_t *c, fake_t *f)
{
    iv_netmgr_cfg_default(c);
    c->mode     = IV_NETMGR_MODE_AUTO;
    c->fail_n   = FAIL_N;
    c->ok_n     = OK_N;
    c->hold_s   = HOLD_S;
    c->stable_s = STABLE_S;
    c->wan[IV_WAN_WIRELESS].ifindex    = WL_IF;
    c->wan[IV_WAN_WIRELESS].gateway_be = WL_GW;
    (void)strcpy(c->wan[IV_WAN_WIRELESS].name, "usb0");
    c->wan[IV_WAN_WIRED].ifindex       = WD_IF;
    c->wan[IV_WAN_WIRED].gateway_be    = WD_GW;
    (void)strcpy(c->wan[IV_WAN_WIRED].name, "eth0");
    c->ops = &f->ops;
}

/* 连喂 n 次同一结论 */
static void feed_n(iv_netmgr_t *m, iv_wan_t w, int ok, unsigned n, uint64_t t)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        chk(iv_netmgr_note_probe(m, w, ok, t) == IV_OK, "note_probe returns IV_OK");
    }
}

/* 把系统推到 WIRED_UP：无线先可用（open 已置），有线探活 ok_n 次，再让无线 fail_n 次 */
static void drive_to_wired(iv_netmgr_t *m, uint64_t t)
{
    feed_n(m, IV_WAN_WIRED, 1, OK_N, t);                       /* 有线可用 */
    chk(iv_netmgr_wan_is_up(m, IV_WAN_WIRED) == 1, "pre: wired up");
    feed_n(m, IV_WAN_WIRELESS, 0, FAIL_N, t);                  /* 无线掉 -> 切有线 */
    chk((int)iv_netmgr_active_wan(m) == (int)IV_WAN_WIRED, "pre: active wired");
    chk(iv_netmgr_state(m) == IV_NETMGR_WIRED_UP, "pre: state wired up");
}

/* =========================================================================
 * c01 生命周期 / 归一化 / 只读读数
 * ========================================================================= */
static void c01_lifecycle(void)
{
    iv_netmgr_t      m;
    iv_netmgr_cfg_t  c;
    fake_t           f;

    fake_init(&f);
    iv_netmgr_cfg_default(&c);
    chk(c.mode == IV_NETMGR_MODE_AUTO, "c01 default mode AUTO");
    chk(c.fail_n == 3u && c.ok_n == 2u && c.hold_s == 30u && c.stable_s == 10u,
        "c01 default thresholds");
    chk(c.ops == NULL, "c01 default ops NULL");
    iv_netmgr_cfg_default(NULL); /* 不崩 */

    iv_netmgr_init(&m);
    chk(m.opened == 0, "c01 init not opened");

    chk(iv_netmgr_open(NULL, &c, 0) == IV_EINVAL, "c01 open NULL m");
    chk(iv_netmgr_open(&m, NULL, 0) == IV_EINVAL, "c01 open NULL cfg");
    /* 未 open 前驱动应被拒 */
    chk(iv_netmgr_note_probe(&m, IV_WAN_WIRELESS, 1, 0) == IV_EINVAL, "c01 probe before open");
    chk(iv_netmgr_tick(&m, 0) == IV_EINVAL, "c01 tick before open");

    cfg_auto(&c, &f);
    c.mode = (iv_netmgr_mode_t)0; /* 0 -> AUTO */
    c.fail_n = 0u; c.ok_n = 0u; c.hold_s = 0u; c.stable_s = 0u;
    c.ops = NULL;                 /* -> iv_netmgr_ops_default() */
    chk(iv_netmgr_open(&m, &c, 1000) == IV_OK, "c01 open ok");
    chk(m.cfg.mode == IV_NETMGR_MODE_AUTO, "c01 normalize mode");
    chk(m.cfg.fail_n == 3u && m.cfg.ok_n == 2u && m.cfg.hold_s == 30u,
        "c01 normalize thresholds");
    chk(m.cfg.stable_s == 0u, "c01 stable_s 0 kept (legal)");
    chk(m.cfg.ops == iv_netmgr_ops_default(), "c01 normalize ops -> default");
    chk(iv_netmgr_open(&m, &c, 1000) == IV_ESTATE, "c01 reopen ESTATE");

    /* 初始态：无线优先（usb0 有 ifindex 视为可用） */
    chk(iv_netmgr_state(&m) == IV_NETMGR_WIRELESS_UP, "c01 initial state wireless up");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRELESS, "c01 initial active wireless");
    chk(iv_netmgr_active_ifindex(&m) == WL_IF, "c01 active ifindex");
    chk(iv_netmgr_wan_is_up(&m, IV_WAN_WIRELESS) == 1, "c01 wireless up");
    chk(iv_netmgr_wan_is_up(&m, IV_WAN_WIRED) == 0, "c01 wired unknown/down");
    chk(iv_netmgr_media_suspended(&m) == 0, "c01 not suspended");
    chk(iv_netmgr_switches(&m) == 0u && iv_netmgr_txn_fail(&m) == 0u, "c01 counters zero");

    /* 越界 / NULL 读数 */
    chk(iv_netmgr_wan_is_up(&m, (iv_wan_t)5) == 0, "c01 wan_is_up out-of-range");
    chk(iv_netmgr_note_probe(&m, (iv_wan_t)9, 1, 0) == IV_EINVAL, "c01 probe bad wan");
    chk(iv_netmgr_state(NULL) == IV_NETMGR_BOTH_DOWN, "c01 state(NULL)");
    chk((int)iv_netmgr_active_wan(NULL) == (int)IV_WAN_WIRELESS, "c01 active_wan(NULL)");
    chk(iv_netmgr_active_ifindex(NULL) == 0, "c01 active_ifindex(NULL)");
    chk(iv_netmgr_media_suspended(NULL) == 0, "c01 suspended(NULL)");
    chk(iv_netmgr_switches(NULL) == 0u && iv_netmgr_txn_fail(NULL) == 0u, "c01 counters(NULL)");
    chk(iv_netmgr_wan_is_up(NULL, IV_WAN_WIRED) == 0, "c01 wan_is_up(NULL)");

    /* 状态名全覆盖 */
    chk(strcmp(iv_netmgr_state_name(IV_NETMGR_WIRELESS_UP), "WIRELESS_UP") == 0, "c01 name 0");
    chk(strcmp(iv_netmgr_state_name(IV_NETMGR_WIRELESS_DEGRADED), "WIRELESS_DEGRADED") == 0,
        "c01 name 1");
    chk(strcmp(iv_netmgr_state_name(IV_NETMGR_SWITCH_TO_WIRED), "SWITCH_TO_WIRED") == 0, "c01 name 2");
    chk(strcmp(iv_netmgr_state_name(IV_NETMGR_WIRED_UP), "WIRED_UP") == 0, "c01 name 3");
    chk(strcmp(iv_netmgr_state_name(IV_NETMGR_SWITCH_TO_WIRELESS), "SWITCH_TO_WIRELESS") == 0,
        "c01 name 4");
    chk(strcmp(iv_netmgr_state_name(IV_NETMGR_BOTH_DOWN), "BOTH_DOWN") == 0, "c01 name 5");
    chk(strcmp(iv_netmgr_state_name((iv_netmgr_state_t)99), "?") == 0, "c01 name unknown");

    /* 未绑定（ifindex==0）的 WAN 视为不可用 */
    cfg_auto(&c, &f);
    c.wan[IV_WAN_WIRELESS].ifindex = 0;
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c01b open");
    chk(iv_netmgr_wan_is_up(&m, IV_WAN_WIRELESS) == 0, "c01b unbound wireless not up");
}

/* =========================================================================
 * c02 迟滞：fail_n / ok_n 必须连续 N 次才翻转
 * ========================================================================= */
static void c02_hysteresis(void)
{
    iv_netmgr_t     m;
    iv_netmgr_cfg_t c;
    fake_t          f;

    fake_init(&f);
    cfg_auto(&c, &f);
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c02 open");

    /* 无线初始可用；fail_n-1 次失败**不得**判掉 */
    feed_n(&m, IV_WAN_WIRELESS, 0, FAIL_N - 1u, 100);
    chk(iv_netmgr_wan_is_up(&m, IV_WAN_WIRELESS) == 1, "c02 still up after fail_n-1");
    chk(iv_netmgr_state(&m) == IV_NETMGR_WIRELESS_UP, "c02 still WIRELESS_UP");
    /* 第 fail_n 次 -> 掉（有线未确认，转 DEGRADED 而非切走） */
    feed_n(&m, IV_WAN_WIRELESS, 0, 1u, 100);
    chk(iv_netmgr_wan_is_up(&m, IV_WAN_WIRELESS) == 0, "c02 down at fail_n");
    chk(iv_netmgr_switches(&m) == 0u, "c02 no switch (wired not up)");

    /* ok_n-1 次成功**不得**判回 */
    feed_n(&m, IV_WAN_WIRELESS, 1, OK_N - 1u, 200);
    chk(iv_netmgr_wan_is_up(&m, IV_WAN_WIRELESS) == 0, "c02 still down after ok_n-1");
    feed_n(&m, IV_WAN_WIRELESS, 1, 1u, 200);
    chk(iv_netmgr_wan_is_up(&m, IV_WAN_WIRELESS) == 1, "c02 up at ok_n");

    /* 交错：一次成功把 fail_streak 清零 */
    feed_n(&m, IV_WAN_WIRELESS, 0, FAIL_N - 1u, 300);
    feed_n(&m, IV_WAN_WIRELESS, 1, 1u, 300);          /* 清零 fail_streak */
    feed_n(&m, IV_WAN_WIRELESS, 0, FAIL_N - 1u, 300); /* 重新计，仍不足 */
    chk(iv_netmgr_wan_is_up(&m, IV_WAN_WIRELESS) == 1, "c02 interleave clears fail streak");
}

/* =========================================================================
 * c03 切有线：七步顺序 + 参数（from=旧口 / to=新口 / gateway）
 * ========================================================================= */
static void c03_switch_order(void)
{
    iv_netmgr_t     m;
    iv_netmgr_cfg_t c;
    fake_t          f;

    fake_init(&f);
    cfg_auto(&c, &f);
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c03 open");

    feed_n(&m, IV_WAN_WIRED, 1, OK_N, 500);   /* 有线可用 */
    feed_n(&m, IV_WAN_WIRELESS, 0, FAIL_N, 500); /* 无线掉 -> run_txn(WIRED) */

    chk(iv_netmgr_state(&m) == IV_NETMGR_WIRED_UP, "c03 state WIRED_UP");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRED, "c03 active wired");
    chk(iv_netmgr_active_ifindex(&m) == WD_IF, "c03 active ifindex = wired");
    chk(iv_netmgr_switches(&m) == 1u, "c03 switches=1");
    chk(iv_netmgr_txn_fail(&m) == 0u, "c03 no txn fail");

    /* 顺序：freeze -> switch -> close -> reconnect（resume 不在切换内，见 c04） */
    chk(f.seq_n == 4, "c03 four steps");
    chk(f.seq[0] == SEQ_FREEZE && f.seq[1] == SEQ_SWITCH &&
        f.seq[2] == SEQ_CLOSE  && f.seq[3] == SEQ_RECONN, "c03 step order");
    /* 参数：切"旧->新"，close 收**旧** ifindex，reconnect 收**新** ifindex */
    chk(f.switch_from == WL_IF && f.switch_to == WD_IF, "c03 switch from/to");
    chk(f.switch_gw == WD_GW, "c03 switch gateway = wired gw");
    chk(f.close_if == WL_IF, "c03 close_old_tcp gets OLD ifindex");
    chk(f.reconn_if == WD_IF, "c03 reconnect gets NEW ifindex");
    /* 冻结生效 -> 排稳定期恢复 */
    chk(iv_netmgr_media_suspended(&m) == 1, "c03 media suspended after freeze");
    chk(m.media_resume_pending == 1, "c03 resume pending");
    chk(m.media_resume_at == 500u + (uint64_t)STABLE_S * 1000u, "c03 resume_at = now+stable_s");

    /* freeze 为 NULL 时不冻结、不排恢复 */
    {
        iv_netmgr_t     m2;
        iv_netmgr_cfg_t c2;
        fake_t          f2;
        fake_init(&f2);
        f2.ops.freeze_low_prio = NULL;
        cfg_auto(&c2, &f2);
        iv_netmgr_init(&m2);
        chk(iv_netmgr_open(&m2, &c2, 0) == IV_OK, "c03b open");
        feed_n(&m2, IV_WAN_WIRED, 1, OK_N, 700);
        feed_n(&m2, IV_WAN_WIRELESS, 0, FAIL_N, 700);
        chk(iv_netmgr_state(&m2) == IV_NETMGR_WIRED_UP, "c03b switched");
        chk(iv_netmgr_media_suspended(&m2) == 0, "c03b no freeze -> not suspended");
        chk(m2.media_resume_pending == 0, "c03b no resume pending");
    }
}

/* =========================================================================
 * c04 媒体恢复：只能由 tick 在稳定期到点后触发
 * ========================================================================= */
static void c04_media_resume(void)
{
    iv_netmgr_t     m;
    iv_netmgr_cfg_t c;
    fake_t          f;
    uint64_t        at;

    fake_init(&f);
    cfg_auto(&c, &f);
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c04 open");
    drive_to_wired(&m, 1000);
    chk(f.resume_calls == 0, "c04 no resume during switch");

    at = m.media_resume_at; /* = 1000 + stable_s*1000 */
    chk(iv_netmgr_tick(&m, at - 1u) == IV_OK, "c04 tick before deadline");
    chk(f.resume_calls == 0, "c04 not resumed before deadline");
    chk(iv_netmgr_media_suspended(&m) == 1, "c04 still suspended before deadline");

    chk(iv_netmgr_tick(&m, at) == IV_OK, "c04 tick at deadline");
    chk(f.resume_calls == 1, "c04 resumed at deadline");
    chk(f.resume_if == WD_IF, "c04 resume on NEW ifindex");
    chk(iv_netmgr_media_suspended(&m) == 0, "c04 un-suspended after resume");

    /* 再 tick 不重复恢复 */
    chk(iv_netmgr_tick(&m, at + 5000u) == IV_OK, "c04 tick after");
    chk(f.resume_calls == 1, "c04 resume once only");
}

/* =========================================================================
 * c05 事务失败：回滚 active + 重试窗口保护
 * ========================================================================= */
static void c05_txn_rollback(void)
{
    iv_netmgr_t     m;
    iv_netmgr_cfg_t c;
    fake_t          f;

    fake_init(&f);
    f.switch_ret = IV_EIO;     /* 第 2 步（切路由）失败 */
    cfg_auto(&c, &f);
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c05 open");

    feed_n(&m, IV_WAN_WIRED, 1, OK_N, 2000);     /* 有线可用 */
    feed_n(&m, IV_WAN_WIRELESS, 0, FAIL_N, 2000);/* 无线掉 -> 事务失败 */

    chk(iv_netmgr_txn_fail(&m) == 1u, "c05 txn_fail=1");
    chk(iv_netmgr_switches(&m) == 0u, "c05 no successful switch");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRELESS, "c05 active UNCHANGED");
    chk(iv_netmgr_media_suspended(&m) == 0, "c05 freeze rolled back");
    chk(iv_netmgr_state(&m) == IV_NETMGR_BOTH_DOWN, "c05 both down after rollback");
    chk(f.switch_calls == 1 && f.close_calls == 0 && f.reconnect_calls == 0,
        "c05 stopped at failing step (no later steps run)");

    /* 重试窗口内：tick 不得再打事务 */
    chk(iv_netmgr_tick(&m, 2000u + 1000u) == IV_OK, "c05 tick within window");
    chk(f.switch_calls == 1, "c05 no retry within window");

    /* 窗口到点后允许再试（此时为 BOTH_DOWN 且有有线 -> 切有线） */
    chk(iv_netmgr_tick(&m, 2000u + 5000u) == IV_OK, "c05 tick at window edge");
    chk(f.switch_calls == 2, "c05 retries after window");
    chk(iv_netmgr_txn_fail(&m) == 2u, "c05 txn_fail=2");

    /* 放开路由：再过后一次 tick 应成功切换，计数器不再涨 */
    f.switch_ret = IV_OK;
    chk(iv_netmgr_tick(&m, 2000u + 10000u) == IV_OK, "c05 tick after");
    chk(iv_netmgr_switches(&m) == 1u, "c05 now switched");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRED, "c05 active now wired");
}

/* =========================================================================
 * c06 hold_s：无线稳定满 hold_s 才回切
 * ========================================================================= */
static void c06_hold_back(void)
{
    iv_netmgr_t     m;
    iv_netmgr_cfg_t c;
    fake_t          f;
    uint64_t        t_up;

    fake_init(&f);
    cfg_auto(&c, &f);
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c06 open");
    drive_to_wired(&m, 3000);
    chk(iv_netmgr_switches(&m) == 1u, "c06 switched to wired once");

    /* 无线恢复：ok_n 次 -> wan_up，记 since=t_up */
    t_up = 100000u;
    feed_n(&m, IV_WAN_WIRELESS, 1, OK_N, t_up);
    chk(iv_netmgr_wan_is_up(&m, IV_WAN_WIRELESS) == 1, "c06 wireless back up");
    chk(iv_netmgr_switches(&m) == 1u, "c06 not switched yet on recovery");

    /* 未满 hold_s：不得回切 */
    chk(iv_netmgr_tick(&m, t_up + (uint64_t)HOLD_S * 1000u - 1u) == IV_OK, "c06 tick before hold");
    chk(iv_netmgr_switches(&m) == 1u, "c06 no switch before hold_s");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRED, "c06 still on wired");

    /* 满 hold_s：回切无线 */
    chk(iv_netmgr_tick(&m, t_up + (uint64_t)HOLD_S * 1000u) == IV_OK, "c06 tick at hold");
    chk(iv_netmgr_switches(&m) == 2u, "c06 switched back at hold_s");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRELESS, "c06 active wireless");
    chk(iv_netmgr_state(&m) == IV_NETMGR_WIRELESS_UP, "c06 state wireless up");
    chk(f.switch_from == WD_IF && f.switch_to == WL_IF, "c06 switch wired->wireless");
    chk(f.close_if == WD_IF, "c06 close old = wired ifindex");
}

/* =========================================================================
 * c07 有线断：立即回切（不等 hold_s）
 * ========================================================================= */
static void c07_wired_down_immediate(void)
{
    iv_netmgr_t     m;
    iv_netmgr_cfg_t c;
    fake_t          f;

    fake_init(&f);
    cfg_auto(&c, &f);
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c07 open");
    drive_to_wired(&m, 4000);

    /* 无线先恢复可用（但仍未满 hold_s），紧接着有线掉 */
    feed_n(&m, IV_WAN_WIRELESS, 1, OK_N, 50000);
    chk(iv_netmgr_switches(&m) == 1u, "c07 still on wired");
    feed_n(&m, IV_WAN_WIRED, 0, FAIL_N, 50000);

    chk(iv_netmgr_switches(&m) == 2u, "c07 immediate switch back");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRELESS, "c07 active wireless");
}

/* =========================================================================
 * c08 双断 BOTH_DOWN 与恢复
 * ========================================================================= */
static void c08_both_down(void)
{
    iv_netmgr_t     m;
    iv_netmgr_cfg_t c;
    fake_t          f;

    fake_init(&f);
    cfg_auto(&c, &f);
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c08 open");

    /* 无线掉、有线也没起 -> DEGRADED；再 tick -> BOTH_DOWN */
    feed_n(&m, IV_WAN_WIRELESS, 0, FAIL_N, 6000);
    chk(iv_netmgr_state(&m) == IV_NETMGR_WIRELESS_DEGRADED, "c08 degraded");
    chk(iv_netmgr_switches(&m) == 0u, "c08 no switch to a down wan");
    chk(iv_netmgr_tick(&m, 6001) == IV_OK, "c08 tick");
    chk(iv_netmgr_state(&m) == IV_NETMGR_BOTH_DOWN, "c08 both down");

    /* 无线恢复 -> 切回无线 */
    feed_n(&m, IV_WAN_WIRELESS, 1, OK_N, 7000);
    chk(iv_netmgr_switches(&m) == 1u, "c08 switched to wireless after recovery");
    chk(iv_netmgr_state(&m) == IV_NETMGR_WIRELESS_UP, "c08 wireless up");
}

/* =========================================================================
 * c09 三种"不自动切换"模式
 * ========================================================================= */
static void c09_modes(void)
{
    iv_netmgr_t     m;
    iv_netmgr_cfg_t c;
    fake_t          f;

    /* WIRED_ONLY：起始即有线的稳态，任何探活都不切换 */
    fake_init(&f);
    cfg_auto(&c, &f);
    c.mode = IV_NETMGR_MODE_WIRED_ONLY;
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c09 wired-only open");
    chk(iv_netmgr_state(&m) == IV_NETMGR_WIRED_UP, "c09 wired-only initial");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRED, "c09 wired-only active");
    feed_n(&m, IV_WAN_WIRELESS, 0, FAIL_N, 100);
    feed_n(&m, IV_WAN_WIRED, 0, FAIL_N, 100);
    chk(iv_netmgr_switches(&m) == 0u, "c09 wired-only never switches");
    chk(iv_netmgr_state(&m) == IV_NETMGR_BOTH_DOWN, "c09 wired-only down state");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRED, "c09 wired-only stays wired");

    /* WIRELESS_ONLY：只用无线，掉线转 DEGRADED，不切有线 */
    fake_init(&f);
    cfg_auto(&c, &f);
    c.mode = IV_NETMGR_MODE_WIRELESS_ONLY;
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c09 wireless-only open");
    feed_n(&m, IV_WAN_WIRED, 1, OK_N, 200);       /* 有线就算可用也不许用 */
    feed_n(&m, IV_WAN_WIRELESS, 0, FAIL_N, 200);
    chk(iv_netmgr_switches(&m) == 0u, "c09 wireless-only never switches");
    chk(iv_netmgr_state(&m) == IV_NETMGR_WIRELESS_DEGRADED, "c09 wireless-only degraded");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRELESS, "c09 wireless-only active");

    /* BOTH：双链路启用但不自动切换，活动口恒为无线 */
    fake_init(&f);
    cfg_auto(&c, &f);
    c.mode = IV_NETMGR_MODE_BOTH;
    iv_netmgr_init(&m);
    chk(iv_netmgr_open(&m, &c, 0) == IV_OK, "c09 both open");
    feed_n(&m, IV_WAN_WIRED, 1, OK_N, 300);
    feed_n(&m, IV_WAN_WIRELESS, 0, FAIL_N, 300);
    chk(iv_netmgr_switches(&m) == 0u, "c09 both never switches");
    chk(iv_netmgr_state(&m) == IV_NETMGR_WIRELESS_DEGRADED, "c09 both degraded");
    chk((int)iv_netmgr_active_wan(&m) == (int)IV_WAN_WIRELESS, "c09 both active wireless");
}

int main(void)
{
    c01_lifecycle();
    c02_hysteresis();
    c03_switch_order();
    c04_media_resume();
    c05_txn_rollback();
    c06_hold_back();
    c07_wired_down_immediate();
    c08_both_down();
    c09_modes();

    if (g_fail != 0) {
        fprintf(stderr, "test_netmgr FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_netmgr passed (lifecycle/normalize, hysteresis, switch-order+args, "
           "media-resume, txn-rollback+retry-window, hold-s-return, wired-down-immediate, "
           "both-down-recovery, modes)\n");
    return 0;
}
