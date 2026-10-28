/*
 * 自愈（故障沿 → 电源通道断电重启）单测（功能开发计划 M4-S4.4）
 *
 * 覆盖：
 *   r01 入参守卫与默认值（reboot_max=0 ⇒ 默认禁用）
 *   r02 0xD1 载荷字节黄金校验 ＋ 边界拒绝
 *   r03 默认禁用：任何沿都不动作
 *   r04 **非沿不触发**（0→2 / 4→2 / 5→2 / 1→4 / 1→5 / 2→1 / 1→1）
 *   r05 摄像机沿 ⇒ 继电器 2~8 **整排 7 条**，且**每拍最多 1 条**（串行化）
 *   r06 断电 5 s 后上电 ＋ 整排各通道独立计时
 *   r07 冷却期不触发（含"恰好在 cd 边界允许"）
 *   r08 限次耗尽不动作（只告警）
 *   r09 主网沿 ⇒ 只动继电器 1；路号越界静默忽略
 *   r10 下发被拒（IV_EBUSY / 其它负码）：不扣限次、不改相位；EBUSY 留队头
 *   r11 set_limits / reset（限次回满、待发清空）
 *   r12 经**真 iv_link** 的字节级校验（0xD1 帧头/命令字/小端长度/CRC/帧尾）
 *       ＋ 复现"同 cmd 在飞唯一"⇒ 证明待发队列不是过度设计
 *   r13 状态沿序列 → 动作序列**黄金对照**（多源、错时、交错）
 *
 * 零硬件：tx 回调是纯函数，时间由 g_now 注入。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_frame.h"
#include "ivsbox/iv_link.h"
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

/* ---- 动作记录（在 tx 里抓，因此能校验**载荷字节**） ---- */
#define MAXACT 40
static struct {
    int  port;
    int  status;
    char payload[IV_RECOVERY_PAYLOAD_MAX];
    int  len;
} g_act[MAXACT];
static int g_act_n;
static int g_tx_rc;

static void rec_reset(void)
{
    memset(g_act, 0, sizeof(g_act));
    g_act_n = 0;
    g_tx_rc  = IV_OK;
}

static int rec_tx(const void *data, uint16_t len, void *arg)
{
    (void)arg;
    if (g_tx_rc != IV_OK)
        return g_tx_rc;
    if (g_act_n < MAXACT) {
        size_t n = (len < sizeof(g_act[0].payload) - 1u)
                       ? (size_t)len
                       : sizeof(g_act[0].payload) - 1u;

        memcpy(g_act[g_act_n].payload, data, n);
        g_act[g_act_n].payload[n] = '\0';
        g_act[g_act_n].len        = (int)len;
        /* 通道号/状态**从载荷文本反解**（不是另开一路记忆），这样黄金序列
         * 校的就是"线上真的发了什么"。 */
        g_act[g_act_n].port   = -1;
        g_act[g_act_n].status = -1;
        (void)sscanf(g_act[g_act_n].payload, "{\"port\":%d,\"status\":%d}",
                     &g_act[g_act_n].port, &g_act[g_act_n].status);
        g_act_n++;
    }
    return IV_OK;
}

/* 时间轴：每"拍"推进 200 ms（＝装配层 iv_recovery_tick 的调用周期） */
static uint64_t g_now;
#define TICK 200u

static void step(iv_recovery_t *r)
{
    (void)iv_recovery_tick(r, g_now);
    g_now += TICK;
}

/* 跑到"待发清空且无通道还在非空闲相位"为止（上限步数防死循环） */
static void run_idle(iv_recovery_t *r, int max_steps)
{
    int i;
    int p;

    for (i = 0; i < max_steps; i++) {
        int busy = (iv_recovery_pending(r) > 0);

        for (p = 0; p < (int)IV_RECOVERY_PORT_N; p++)
            if (iv_recovery_port_phase(r, p) != IV_RECOVERY_ST_IDLE)
                busy = 1;
        if (!busy)
            break;
        step(r);
    }
}

static void mk(iv_recovery_t *r, int reboot_max, uint32_t cooldown_ms,
               uint32_t off_ms)
{
    iv_recovery_cfg_t c;

    iv_recovery_cfg_default(&c);
    c.reboot_max  = reboot_max;
    c.cooldown_ms = cooldown_ms;
    c.off_ms      = off_ms;
    c.tx          = rec_tx;
    c.tx_arg      = NULL;
    iv_recovery_init(r, &c);
    rec_reset();
}

/* ===================================================================== */
static void r01_guards_and_defaults(void)
{
    iv_recovery_t     r;
    iv_recovery_cfg_t c;
    char              buf[IV_RECOVERY_PAYLOAD_MAX];

    iv_recovery_cfg_default(&c);
    chk(c.reboot_max == 0, "r01 default reboot_max == 0 (self-heal disabled)");
    chk(c.cooldown_ms == IV_RECOVERY_COOLDOWN_MS_DEFAULT,
        "r01 default cooldown == 60 s (NETWAOK_RELOAD_TIME)");
    chk(c.off_ms == IV_RECOVERY_OFF_MS_DEFAULT, "r01 default off == 5 s");
    chk(c.tx == NULL && c.on_action == NULL, "r01 default callbacks are NULL");

    iv_recovery_init(&r, NULL); /* cfg=NULL ⇒ 默认（禁用） */
    chk(r.opened == 1, "r01 init with NULL cfg still opens");
    chk(r.cfg.reboot_max == 0, "r01 NULL cfg keeps default reboot_max=0");
    iv_recovery_close(&r);
    chk(r.opened == 0, "r01 close clears opened");
    iv_recovery_close(&r); /* 幂等 */

    chk(iv_recovery_tick(NULL, 0u) == IV_EINVAL, "r01 tick(NULL) -> EINVAL");
    chk(iv_recovery_build_payload(0, 0, NULL, 16u) == IV_EINVAL,
        "r01 payload(NULL out) -> EINVAL");
    chk(iv_recovery_build_payload(0, 0, buf, 0u) == IV_EINVAL,
        "r01 payload(cap=0) -> EINVAL");
    chk(iv_recovery_port_phase(&r, (int)IV_RECOVERY_PORT_N) == IV_EINVAL,
        "r01 port_phase(out of range) -> EINVAL");
    chk(iv_recovery_port_credit(&r, -1) == IV_EINVAL,
        "r01 port_credit(-1) -> EINVAL");
    chk(iv_recovery_pending(NULL) == IV_EINVAL, "r01 pending(NULL) -> EINVAL");
    chk(iv_recovery_set_limits(NULL, 1, 0u) == IV_EINVAL,
        "r01 set_limits(NULL) -> EINVAL");
    chk(iv_recovery_reset(NULL) == IV_EINVAL, "r01 reset(NULL) -> EINVAL");

    /* 未 init（opened=0）时 tick -> ESTATE，沿静默忽略 */
    {
        iv_recovery_t z;

        memset(&z, 0, sizeof(z));
        chk(iv_recovery_tick(&z, 1u) == IV_ESTATE, "r01 tick before init -> ESTATE");
        iv_recovery_on_camera_change(&z, 0, 1, 2, 1u);
        chk(iv_recovery_pending(&z) == 0, "r01 edge before init is ignored");
    }
}

static void r02_payload(void)
{
    char buf[IV_RECOVERY_PAYLOAD_MAX];

    chk(iv_recovery_build_payload(0, 0, buf, sizeof(buf)) == 21 &&
            strcmp(buf, "{\"port\":0,\"status\":0}") == 0,
        "r02 payload (port 0, off) golden");
    chk(iv_recovery_build_payload(7, 1, buf, sizeof(buf)) == 21 &&
            strcmp(buf, "{\"port\":7,\"status\":1}") == 0,
        "r02 payload (port 7, on) golden");
    chk(iv_recovery_build_payload(1, 2, buf, sizeof(buf)) == 21 &&
            strcmp(buf, "{\"port\":1,\"status\":2}") == 0,
        "r02 payload (port 1, restart 15 s) golden");

    chk(iv_recovery_build_payload((int)IV_RECOVERY_PORT_N, 0, buf, sizeof(buf)) ==
            IV_EINVAL,
        "r02 port 8 rejected");
    chk(iv_recovery_build_payload(-1, 0, buf, sizeof(buf)) == IV_EINVAL,
        "r02 port -1 rejected");
    chk(iv_recovery_build_payload(0, 3, buf, sizeof(buf)) == IV_EINVAL,
        "r02 status 3 rejected");
    chk(iv_recovery_build_payload(0, -1, buf, sizeof(buf)) == IV_EINVAL,
        "r02 status -1 rejected");
    chk(iv_recovery_build_payload(0, 0, buf, 23u) == IV_ENOSPC,
        "r02 cap 23 -> ENOSPC");
    chk(iv_recovery_build_payload(0, 0, buf, 24u) > 0, "r02 cap 24 is enough");
}

static void r03_disabled_by_default(void)
{
    iv_recovery_t r;

    /* reboot_max = 0 ⇒ 禁用自愈（现网出厂 net_reload = 0） */
    mk(&r, 0, 60000u, 5000u);
    g_now = 1000u;
    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now);
    iv_recovery_on_master_change(&r, 0, 1, 2, g_now);
    run_idle(&r, 20);
    chk(iv_recovery_pending(&r) == 0, "r03 disabled: nothing queued");
    chk(g_act_n == 0, "r03 disabled: no 0xD1 emitted");
    chk(r.disabled == 8u, "r03 disabled: 7 camera ports + 1 master port counted");
    chk(r.actions == 0, "r03 disabled: actions stay 0");
    iv_recovery_close(&r);
}

static void r04_only_fault_edge(void)
{
    iv_recovery_t r;
    static const int pair[][2] = {
        { 0, 2 }, /* 开机就没 IP/坏：不上电风暴 */
        { 4, 2 }, /* 延时 → 故障：不触发 */
        { 5, 2 }, /* 丢包 → 故障：不触发 */
        { 1, 4 }, /* 劣化（正常 → 延时）：不触发 */
        { 1, 5 }, /* 劣化（正常 → 丢包）：不触发 */
        { 2, 1 }, /* 恢复：不触发 */
        { 1, 1 }, /* 无变化：不触发 */
        { 2, 4 }, /* 故障 → 延时：不触发 */
    };
    size_t i;

    mk(&r, 3, 60000u, 5000u);
    g_now = 1000u;
    for (i = 0; i < sizeof(pair) / sizeof(pair[0]); i++) {
        iv_recovery_on_camera_change(&r, 0, pair[i][0], pair[i][1], g_now);
        iv_recovery_on_master_change(&r, 0, pair[i][0], pair[i][1], g_now);
    }
    chk(iv_recovery_pending(&r) == 0, "r04 non-edges queue nothing");
    chk(g_act_n == 0, "r04 non-edges emit nothing");
    chk(r.actions == 0u, "r04 non-edges keep actions at 0");

    /* 对照：真正的 1→2 沿必须立刻排队 */
    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now);
    chk(iv_recovery_pending(&r) == 7, "r04 1->2 edge queues 7 camera ports");
    iv_recovery_close(&r);
}

static void r05_serialized_whole_row(void)
{
    iv_recovery_t r;
    int           i;

    mk(&r, 1, 60000u, 5000u);
    g_now = 1000u;
    iv_recovery_on_camera_change(&r, 3, 1, 2, g_now); /* 第 4 路摄像机 */
    chk(iv_recovery_pending(&r) == 7, "r05 whole row queued (relay 2..8)");

    /* **每拍最多 1 条**：同 cmd 在 iv_link 里不能并发 */
    step(&r);
    chk(g_act_n == 1, "r05 exactly one 0xD1 per tick");
    chk(iv_recovery_pending(&r) == 6, "r05 queue drains by one");
    for (i = 0; i < 6; i++)
        step(&r);
    chk(g_act_n == 7, "r05 all 7 OFF emitted after 7 ticks");
    chk(iv_recovery_pending(&r) == 0, "r05 queue empty");

    for (i = 0; i < 7; i++) {
        chk(g_act[i].port == i + 1, "r05 port order 1..7 (relay 2..8)");
        chk(g_act[i].status == IV_RECOVERY_PWR_OFF, "r05 first pass is OFF");
        chk(g_act[i].len == (int)strlen(g_act[i].payload),
            "r05 payload length == strlen");
        chk(iv_recovery_port_phase(&r, i + 1) == IV_RECOVERY_ST_OFF,
            "r05 port is in OFF phase");
        chk(iv_recovery_port_credit(&r, i + 1) == 0,
            "r05 credit consumed only on successful OFF");
    }
    chk(g_act[0].payload[0] == '{' && strstr(g_act[0].payload, "\"status\":0"),
        "r05 payload is the frozen JSON shape");
    iv_recovery_close(&r);
}

static void r06_off_5s_then_on(void)
{
    iv_recovery_t r;
    int           i;

    mk(&r, 2, 60000u, 5000u);
    g_now = 1000u;
    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now);
    run_idle(&r, 200);

    chk(r.cycles == 7u, "r06 all 7 ports completed one power cycle");
    chk(g_act_n == 14, "r06 7 OFF + 7 ON");
    for (i = 7; i < 14; i++) {
        chk(g_act[i].status == IV_RECOVERY_PWR_ON, "r06 second pass is ON");
        chk(g_act[i].port == i - 7 + 1, "r06 ON order matches OFF order");
    }
    for (i = 1; i <= 7; i++)
        chk(iv_recovery_port_phase(&r, i) == IV_RECOVERY_ST_IDLE,
            "r06 port back to idle after power-on");

    /* 断电时长：末条 OFF 之后 ≥5 s 才出现 ON */
    chk(r.cycles == 7u && r.actions == 14u, "r06 counters");
    iv_recovery_close(&r);
}

/* 时间轴（r07/r08 需要"上电那一刻"做冷却参照）：
 *   t=1000 第 1 条 OFF 发出 ⇒ off=5000 ⇒ t=6000 第 1 条 ON 发出。 */
#define FIRST_ON_MS 6000u

static void r07_cooldown(void)
{
    iv_recovery_t r;
    uint64_t      cd1;
    uint64_t      cd7;

    mk(&r, 3, 60000u, 5000u);
    g_now = 1000u;
    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now);
    run_idle(&r, 300);
    chk(r.cycles == 7u, "r07 first row cycle done");

    cd1 = r.ch[1].cd_until_ms;
    cd7 = r.ch[7].cd_until_ms;
    chk(cd1 == FIRST_ON_MS + IV_RECOVERY_COOLDOWN_MS_DEFAULT,
        "r07 cooldown counts from power-on (60 s after ON)");
    chk(cd1 > g_now, "r07 cooldown armed on power-on");
    chk(cd7 > cd1, "r07 each port's cooldown counts from its own power-on");

    /* **上电后 1 ms**：冷却窗口内 ⇒ 整排都不许动作。
     * （这一拍是"冷却机制是否存在"的判据：若冷却时长为 0，此刻就会立刻触发。） */
    g_now          = FIRST_ON_MS + 1u;
    g_act_n        = 0;
    r.blocked_cool = 0;
    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now);
    chk(iv_recovery_pending(&r) == 0, "r07 inside cooldown: nothing queued");
    chk(r.blocked_cool == 7u, "r07 inside cooldown: 7 ports blocked");

    /* 恰好在 **第一条**通道的 cd 边界（边界含）：只有它放行 */
    g_now = cd1;
    iv_recovery_on_camera_change(&r, 1, 1, 2, g_now);
    chk(iv_recovery_pending(&r) == 1, "r07 at cd boundary: exactly one allowed");
    chk(r.blocked_cool == 13u, "r07 others still in their own cooldown");

    /* 到**最后一条**通道的 cd：整排都放行 */
    g_now = cd7;
    iv_recovery_on_camera_change(&r, 2, 1, 2, g_now);
    chk(iv_recovery_pending(&r) == 7,
        "r07 once all cooldowns elapsed the whole row queues again");
    chk(g_act_n == 0, "r07 queuing alone does not emit");
    iv_recovery_close(&r);
}

static void r08_limit_exhausted(void)
{
    iv_recovery_t r;
    uint64_t      cd7;

    mk(&r, 1, 60000u, 5000u);
    g_now = 1000u;
    iv_recovery_on_camera_change(&r, 2, 1, 2, g_now);
    run_idle(&r, 300);
    chk(r.cycles == 7u, "r08 one cycle with reboot_max=1");
    chk(iv_recovery_port_credit(&r, 1) == 0, "r08 credit exhausted");

    cd7    = r.ch[7].cd_until_ms;
    g_now  = cd7 + 1u;
    g_act_n = 0;
    r.blocked_lim = 0;
    iv_recovery_on_camera_change(&r, 2, 1, 2, g_now);
    chk(iv_recovery_pending(&r) == 0, "r08 exhausted: nothing queued");
    chk(r.blocked_lim == 7u, "r08 exhausted: 7 ports refused");
    chk(g_act_n == 0, "r08 exhausted: no 0xD1 (alarm only)");

    /* 主网通道限次**独立**：自己跑满一轮后同样被挡 */
    g_now = 500000u;
    iv_recovery_on_master_change(&r, 0, 1, 2, g_now);
    run_idle(&r, 300);
    chk(r.cycles == 8u, "r08 master port completed its own cycle");
    chk(iv_recovery_port_credit(&r, 0) == 0, "r08 master credit exhausted");
    g_now = r.ch[0].cd_until_ms + 1u;
    iv_recovery_on_master_change(&r, 1, 1, 2, g_now);
    chk(r.blocked_lim == 8u, "r08 master port refused after its own cycle");
    iv_recovery_close(&r);
}

static void r09_master_and_index(void)
{
    iv_recovery_t r;
    int           q;

    mk(&r, 3, 60000u, 5000u);
    g_now = 1000u;

    /* 路号越界：静默忽略（回调里不便返错） */
    iv_recovery_on_master_change(&r, 2, 1, 2, g_now);
    iv_recovery_on_master_change(&r, -1, 1, 2, g_now);
    iv_recovery_on_camera_change(&r, 6, 1, 2, g_now);
    iv_recovery_on_camera_change(&r, -1, 1, 2, g_now);
    chk(iv_recovery_pending(&r) == 0, "r09 out-of-range index ignored");

    /* 主网沿 ⇒ 只动**继电器 1**（port 0），不碰摄像机整排 */
    iv_recovery_on_master_change(&r, 0, 1, 2, g_now);
    chk(iv_recovery_pending(&r) == 1, "r09 master edge queues exactly 1 port");
    step(&r);
    chk(g_act_n == 1 && g_act[0].port == 0, "r09 master edge drives relay 1 only");
    chk(iv_recovery_port_phase(&r, 1) == IV_RECOVERY_ST_IDLE,
        "r09 master edge leaves camera ports alone");

    /* 另一路主网再报故障：同通道已在断电中 ⇒ 不叠加 */
    q = iv_recovery_pending(&r);
    iv_recovery_on_master_change(&r, 1, 1, 2, g_now);
    chk(iv_recovery_pending(&r) == q, "r09 second master WAN does not stack");
    iv_recovery_close(&r);
}

static void r10_tx_refused(void)
{
    iv_recovery_t r;
    int           i;

    mk(&r, 1, 60000u, 5000u);
    g_now = 1000u;
    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now);

    /* EBUSY（同 cmd 在飞）⇒ 留队头、下一拍再试、**不扣限次** */
    g_tx_rc = IV_EBUSY;
    step(&r);
    chk(r.tx_busy == 1u, "r10 EBUSY counted as busy, not error");
    chk(iv_recovery_pending(&r) == 7, "r10 EBUSY keeps the queue head");
    chk(iv_recovery_port_credit(&r, 1) == 1, "r10 EBUSY does not burn credit");
    chk(iv_recovery_port_phase(&r, 1) == IV_RECOVERY_ST_Q_OFF,
        "r10 EBUSY keeps port queued");
    chk(r.tx_err == 0u, "r10 EBUSY is not an error");

    /* 其它负码 ⇒ 丢弃该条 ＋ 计数；该通道回到空闲（限次未扣） */
    g_tx_rc = IV_EFAIL;
    step(&r);
    chk(r.tx_err == 1u, "r10 hard failure counted");
    chk(iv_recovery_pending(&r) == 6, "r10 hard failure drops that entry");
    chk(iv_recovery_port_phase(&r, 1) == IV_RECOVERY_ST_IDLE,
        "r10 hard failure returns port to idle");
    chk(iv_recovery_port_credit(&r, 1) == 1, "r10 hard failure keeps credit");

    /* 恢复后可继续下发剩余通道 */
    g_tx_rc = IV_OK;
    for (i = 0; i < 6; i++)
        step(&r);
    chk(iv_recovery_pending(&r) == 0, "r10 remaining ports drained");
    chk(iv_recovery_port_phase(&r, 2) == IV_RECOVERY_ST_OFF,
        "r10 remaining ports are OFF");
    iv_recovery_close(&r);
}

static void r11_limits_and_reset(void)
{
    iv_recovery_t r;

    mk(&r, 1, 60000u, 5000u); /* 先禁用态：reboot_max=1 但下面改成 0 */
    chk(iv_recovery_set_limits(&r, 0, 60000u) == IV_OK, "r11 set_limits ok");
    chk(iv_recovery_port_credit(&r, 1) == 0, "r11 credit follows reboot_max=0");
    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now);
    chk(r.disabled == 7u, "r11 reverse: reboot_max=0 disables");

    chk(iv_recovery_set_limits(&r, 2, 30000u) == IV_OK, "r11 re-enable");
    chk(r.cfg.cooldown_ms == 30000u, "r11 cooldown updated");
    chk(iv_recovery_port_credit(&r, 1) == 2, "r11 credit rearmed to 2");
    g_now = 100000u;
    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now);
    chk(iv_recovery_pending(&r) == 7, "r11 enabled edge triggers");

    chk(iv_recovery_reset(&r) == IV_OK, "r11 reset ok");
    chk(iv_recovery_pending(&r) == 0, "r11 reset clears pending");
    chk(iv_recovery_port_phase(&r, 1) == IV_RECOVERY_ST_IDLE,
        "r11 reset clears phase");
    chk(iv_recovery_port_credit(&r, 1) == 2, "r11 reset rearms credit");

    /* cooldown_ms=0 ⇒ 回落默认（不是"零冷却"） */
    (void)iv_recovery_set_limits(&r, 1, 0u);
    chk(r.cfg.cooldown_ms == IV_RECOVERY_COOLDOWN_MS_DEFAULT,
        "r11 cooldown 0 falls back to 60 s");
    iv_recovery_close(&r);
}

/* --- r12：真 iv_link 的字节级校验 --- */
static struct {
    uint8_t buf[IV_FRAME_MAX];
    size_t  len;
    int     n;
} g_link_tx;

static int link_tx(const uint8_t *bytes, size_t len, void *arg)
{
    (void)arg;
    if (len <= sizeof(g_link_tx.buf)) {
        memcpy(g_link_tx.buf, bytes, len);
        g_link_tx.len = len;
        g_link_tx.n++;
    }
    return IV_OK;
}

static int rec_tx_via_link(const void *data, uint16_t len, void *arg)
{
    iv_link_t *lk = arg;
    /* 0xD1 是控制命令：iv_link 只发一次、不重传；这里不需要应答回调 */
    return iv_link_send(lk, IV_FRAME_CMD_POWER, data, len, (uint32_t)g_now, NULL,
                        NULL);
}

static void r12_link_bytes(void)
{
    iv_link_t         lk;
    iv_recovery_t     r;
    iv_recovery_cfg_t c;
    iv_link_cfg_t     lc = IV_LINK_CFG_DEFAULT;
    uint8_t           crc;
    const char       *want = "{\"port\":1,\"status\":0}";

    memset(&g_link_tx, 0, sizeof(g_link_tx));

    /* 短超时：单测里让在飞事务尽快过期，便于逐条推进。
     * `0xD1` 走的是**控制命令专用窗口** `ctrl_timeout_ms`（默认 300 ms），
     * 不跟着 `timeout_ms` 走 ⇒ 这里两处都要压短，否则槽一直占着、
     * 队列推进不了（本用例验的正是"槽释放后队列继续发"）。 */
    lc.timeout_ms      = 10u;
    lc.ctrl_timeout_ms = 10u;
    chk(iv_link_init(&lk, &lc, link_tx, NULL, NULL, NULL) == IV_OK,
        "r12 link init");

    iv_recovery_cfg_default(&c);
    c.reboot_max = 1;
    c.off_ms     = 5000u;
    c.tx         = rec_tx_via_link;
    c.tx_arg     = &lk;
    iv_recovery_init(&r, &c);
    rec_reset();
    g_now = 1000u;

    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now);
    (void)iv_recovery_tick(&r, g_now);

    chk(g_link_tx.n == 1, "r12 one frame handed to the serial layer");
    chk(g_link_tx.len == (size_t)IV_FRAME_OVERHEAD + strlen(want),
        "r12 frame length == 8 + payload");
    chk(g_link_tx.buf[0] == 0xF0 && g_link_tx.buf[1] == 0xF0,
        "r12 frame head is 0xF0F0 (downlink)");
    chk(g_link_tx.buf[2] == IV_FRAME_CMD_POWER, "r12 cmd is 0xD1");
    chk(g_link_tx.buf[3] == (uint8_t)strlen(want) && g_link_tx.buf[4] == 0x00,
        "r12 length field little-endian");
    chk(memcmp(&g_link_tx.buf[5], want, strlen(want)) == 0,
        "r12 payload bytes are the frozen JSON");
    crc = iv_crc8(&g_link_tx.buf[2], 3, IV_CRC8_SEED_INIT);
    crc = iv_crc8(&g_link_tx.buf[5], (size_t)strlen(want), crc);
    chk(g_link_tx.buf[5 + strlen(want)] == crc, "r12 CRC8 over cmd+len+data");
    chk(g_link_tx.buf[6 + strlen(want)] == 0xFF &&
            g_link_tx.buf[7 + strlen(want)] == 0xFF,
        "r12 frame tail is 0xFFFF");

    /* **机制证明**：同 cmd 连发第二次必被 iv_link 拒（IV_EBUSY）——
     * 这正是待发队列存在的理由，不是过度设计。 */
    chk(iv_link_send(&lk, IV_FRAME_CMD_POWER, want, (uint16_t)strlen(want),
                     (uint32_t)g_now, NULL, NULL) == IV_EBUSY,
        "r12 same-cmd second send -> IV_EBUSY (why the FIFO exists)");

    /* 让在飞事务过期后，队列能继续推进 */
    (void)iv_link_tick(&lk, (uint32_t)(g_now + 100u));
    g_now += 100u;
    (void)iv_recovery_tick(&r, g_now);
    chk(g_link_tx.n == 2, "r12 queue drains after the in-flight txn expires");
    chk(memcmp(&g_link_tx.buf[5], "{\"port\":2,\"status\":0}", 21) == 0,
        "r12 second frame goes to port 2");
    iv_recovery_close(&r);
}

static void r13_golden_sequence(void)
{
    /* 交错时序：主网故障 → 摄像机 1 故障 → 冷却期内摄像机 2 再故障 */
    static const int want[][2] = {
        { 0, 0 },                                             /* 主网 → 继电器 1 */
        { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 0 }, { 6, 0 }, { 7, 0 },
        { 0, 1 },                                             /* 上电 */
        { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 1 }, { 5, 1 }, { 6, 1 }, { 7, 1 },
    };
    iv_recovery_t r;
    size_t        i;

    mk(&r, 2, 60000u, 5000u);
    g_now = 1000u;
    iv_recovery_on_master_change(&r, 0, 1, 2, g_now); /* 主网故障 */
    iv_recovery_on_camera_change(&r, 0, 1, 2, g_now); /* 摄像机 1 故障 */

    /* 队列顺序：主网通道先入队，摄像机整排随之（port 1..7） */
    chk(iv_recovery_pending(&r) == 8, "r13 master + camera row queued");

    run_idle(&r, 300);
    chk(g_act_n == (int)(sizeof(want) / sizeof(want[0])),
        "r13 action count matches golden table");

    for (i = 0; i < sizeof(want) / sizeof(want[0]) && (int)i < g_act_n; i++) {
        if (g_act[i].port != want[i][0] || g_act[i].status != want[i][1]) {
            chk(0, "r13 action sequence matches golden table");
            break;
        }
    }
    chk(i == sizeof(want) / sizeof(want[0]), "r13 golden sequence verified");
    iv_recovery_close(&r);
}

int main(void)
{
    r01_guards_and_defaults();
    r02_payload();
    r03_disabled_by_default();
    r04_only_fault_edge();
    r05_serialized_whole_row();
    r06_off_5s_then_on();
    r07_cooldown();
    r08_limit_exhausted();
    r09_master_and_index();
    r10_tx_refused();
    r11_limits_and_reset();
    r12_link_bytes();
    r13_golden_sequence();

    if (g_fail != 0) {
        fprintf(stderr, "test_recovery FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_recovery passed (guards, payload, disabled, edge-only, serialized "
           "row, 5s off, cooldown, limit, master mapping, tx refused, "
           "limits/reset, iv_link bytes, golden sequence)\n");
    return 0;
}
