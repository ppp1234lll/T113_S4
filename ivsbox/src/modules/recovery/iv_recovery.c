/*
 * 自愈：故障沿 → 电源通道断电重启（实现，功能开发计划 M4-S4.4）
 * 口径、边界、待发队列的由来与"与现网代码的差异"见 iv_recovery.h 文件头。
 *
 * 实现要点（四条）：
 *   1. **纯逻辑**：不碰 fd / socket / 线程 / 时钟；时间由 now_ms 传入，报文由
 *      tx 回调交出。因此"拔网线 → 继电器动作"整条链在单测里可完全无硬件驱动。
 *   2. **状态机最小**：每通道只有 空闲 / OFF待发 / 断电中 / ON待发 四相位；
 *      没有"规则引擎/熔断/上游关联"。
 *   3. **待发 FIFO ＋ 每拍最多 1 条**：`iv_link` 同 cmd 在飞唯一，一次摄像机
 *      故障的 7 条 0xD1 必须串行下发（见头文件）。
 *   4. **失败不改状态**：只有 OFF 真的发出去才扣限次、才进断电相位；
 *      `IV_EBUSY` 不算失败（留队头等下一拍），其它负码丢弃该条并计数。
 */
#include "ivsbox/iv_recovery.h"

#include <string.h>

#include "ivsbox/iv_log.h"

#define REC_MOD "recovery"

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

/* 无符号十进制写入（返回写入字节数）。不用 snprintf：目标缓冲由调用方给定，
 * 上限由本函数自己掌控，避免 `-Wformat-truncation` 在 `-Werror` 下断编译。 */
static size_t put_u32(char *p, uint32_t v)
{
    char   t[10];
    size_t n = 0;
    size_t i;

    if (v == 0u) {
        p[0] = '0';
        return 1u;
    }
    while (v != 0u) {
        t[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n; i++)
        p[i] = t[n - 1u - i];
    return n;
}

static int port_ok(int port) { return port >= 0 && port < (int)IV_RECOVERY_PORT_N; }

static int status_ok(int status)
{
    return status == (int)IV_RECOVERY_PWR_OFF || status == (int)IV_RECOVERY_PWR_ON ||
           status == (int)IV_RECOVERY_PWR_RESTART;
}

/* ---------------------------------------------------------------------------
 * 载荷构造
 * ------------------------------------------------------------------------- */
int iv_recovery_build_payload(int port, int status, char *out, size_t cap)
{
    static const char pre[] = "{\"port\":";
    static const char mid[] = ",\"status\":";
    size_t            n     = 0;

    if (out == NULL || cap == 0u)
        return IV_EINVAL;
    if (!port_ok(port) || !status_ok(status))
        return IV_EINVAL;
    /* 最长 {"port":7,"status":2} = 21 B ＋ NUL */
    if (cap < 24u)
        return IV_ENOSPC;

    memcpy(out + n, pre, sizeof(pre) - 1u);
    n += sizeof(pre) - 1u;
    n += put_u32(out + n, (uint32_t)port);
    memcpy(out + n, mid, sizeof(mid) - 1u);
    n += sizeof(mid) - 1u;
    n += put_u32(out + n, (uint32_t)status);
    out[n++] = '}';
    out[n]   = '\0';
    return (int)n;
}

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */
void iv_recovery_cfg_default(iv_recovery_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->reboot_max  = 0; /* 现网出厂 net_reload = 0 ⇒ 自愈默认禁用 */
    cfg->cooldown_ms = IV_RECOVERY_COOLDOWN_MS_DEFAULT;
    cfg->off_ms      = IV_RECOVERY_OFF_MS_DEFAULT;
}

static void limits_apply(iv_recovery_t *r, int reboot_max, uint32_t cooldown_ms)
{
    int i;

    r->cfg.reboot_max = (reboot_max < 0) ? 0 : reboot_max;
    if (r->cfg.reboot_max > 255)
        r->cfg.reboot_max = 255; /* credit 是 uint8_t */
    r->cfg.cooldown_ms =
        (cooldown_ms == 0u) ? IV_RECOVERY_COOLDOWN_MS_DEFAULT : cooldown_ms;
    for (i = 0; i < (int)IV_RECOVERY_PORT_N; i++)
        r->ch[i].credit = (uint8_t)r->cfg.reboot_max;
}

void iv_recovery_init(iv_recovery_t *r, const iv_recovery_cfg_t *cfg)
{
    if (r == NULL)
        return;
    memset(r, 0, sizeof(*r));
    iv_recovery_cfg_default(&r->cfg);
    if (cfg != NULL) {
        r->cfg.tx        = cfg->tx;
        r->cfg.tx_arg    = cfg->tx_arg;
        r->cfg.on_action = cfg->on_action;
        r->cfg.cb_arg    = cfg->cb_arg;
        r->cfg.off_ms =
            (cfg->off_ms == 0u) ? IV_RECOVERY_OFF_MS_DEFAULT : cfg->off_ms;
    }
    limits_apply(r, (cfg != NULL) ? cfg->reboot_max : 0,
                 (cfg != NULL) ? cfg->cooldown_ms : 0u);
    r->opened = 1;
}

int iv_recovery_set_limits(iv_recovery_t *r, int reboot_max, uint32_t cooldown_ms)
{
    if (r == NULL)
        return IV_EINVAL;
    limits_apply(r, reboot_max, cooldown_ms);
    return IV_OK;
}

int iv_recovery_reset(iv_recovery_t *r)
{
    int i;

    if (r == NULL)
        return IV_EINVAL;
    for (i = 0; i < (int)IV_RECOVERY_PORT_N; i++) {
        r->ch[i].phase        = IV_RECOVERY_ST_IDLE;
        r->ch[i].off_until_ms = 0u;
        r->ch[i].cd_until_ms  = 0u;
        r->ch[i].credit       = (uint8_t)r->cfg.reboot_max;
    }
    r->pend_n = 0u;
    return IV_OK;
}

void iv_recovery_close(iv_recovery_t *r)
{
    if (r == NULL)
        return;
    r->opened = 0;
}

/* ---------------------------------------------------------------------------
 * 待发队列
 * ------------------------------------------------------------------------- */
static int pend_push(iv_recovery_t *r, int port, int status)
{
    if (r->pend_n >= (uint8_t)IV_RECOVERY_PEND_MAX) {
        r->pend_full++;
        return IV_EFULL;
    }
    r->pend[r->pend_n].port   = (uint8_t)port;
    r->pend[r->pend_n].status = (uint8_t)status;
    r->pend_n++;
    return IV_OK;
}

static void pend_pop(iv_recovery_t *r)
{
    uint8_t i;

    if (r->pend_n == 0u)
        return;
    for (i = 1u; i < r->pend_n; i++)
        r->pend[i - 1u] = r->pend[i];
    r->pend_n--;
}

/* 下发一条 0xD1。返回 tx 回调的返回码（tx 为 NULL ⇒ IV_OK，干跑只记账）。 */
static int emit(iv_recovery_t *r, int port, int status)
{
    char payload[IV_RECOVERY_PAYLOAD_MAX];

    if (iv_recovery_build_payload(port, status, payload, sizeof(payload)) < 0)
        return IV_EINVAL;
    if (r->cfg.tx != NULL) {
        int rc = r->cfg.tx(payload, (uint16_t)strlen(payload), r->cfg.tx_arg);

        if (rc != IV_OK)
            return rc;
    }
    r->actions++;
    if (r->cfg.on_action != NULL)
        r->cfg.on_action(port, status, r->cfg.cb_arg);
    IV_LOG_I(REC_MOD, "0xD1 %s port=%d",
             (status == (int)IV_RECOVERY_PWR_OFF) ? "OFF" : "ON", port);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 触发：查"禁用 / 进行中 / 冷却 / 限次"四道闸，过了才排队断电
 * ------------------------------------------------------------------------- */
static int trigger(iv_recovery_t *r, int port, uint64_t now_ms)
{
    if (r->cfg.reboot_max <= 0) {
        r->disabled++;
        return IV_ESTATE; /* 禁用自愈：连日志都不刷（现网也是直接返回） */
    }
    if (r->ch[port].phase != IV_RECOVERY_ST_IDLE)
        return IV_EBUSY; /* 该通道正在断电/排队中：不叠加 */
    if (now_ms < r->ch[port].cd_until_ms) {
        r->blocked_cool++;
        IV_LOG_I(REC_MOD, "port=%d in cooldown, skip", port);
        return IV_EBUSY;
    }
    if (r->ch[port].credit == 0u) {
        r->blocked_lim++;
        IV_LOG_W(REC_MOD, "port=%d reload limit exhausted, alarm only", port);
        return IV_ERANGE;
    }
    if (pend_push(r, port, (int)IV_RECOVERY_PWR_OFF) != IV_OK)
        return IV_EFULL;
    r->ch[port].phase = IV_RECOVERY_ST_Q_OFF;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 状态沿入口
 * ------------------------------------------------------------------------- */
void iv_recovery_on_master_change(iv_recovery_t *r, int idx, int old_st, int new_st,
                                  uint64_t now_ms)
{
    if (r == NULL || !r->opened)
        return;
    if (idx < 0 || idx >= (int)IV_RECOVERY_MASTER_N)
        return;
    if (old_st != IV_RECOVERY_NORMAL || new_st != IV_RECOVERY_FAULT)
        return; /* 单沿：只有 正常 → 故障 */
    (void)trigger(r, (int)IV_RECOVERY_PORT_MASTER, now_ms);
}

void iv_recovery_on_camera_change(iv_recovery_t *r, int idx, int old_st, int new_st,
                                  uint64_t now_ms)
{
    int p;

    if (r == NULL || !r->opened)
        return;
    if (idx < 0 || idx >= (int)IV_RECOVERY_CAM_N)
        return;
    if (old_st != IV_RECOVERY_NORMAL || new_st != IV_RECOVERY_FAULT)
        return;
    /* 任一摄像机故障 ⇒ **整排**（继电器 2~8）重启，不定位单台 */
    for (p = (int)IV_RECOVERY_PORT_CAM_LO; p <= (int)IV_RECOVERY_PORT_CAM_HI; p++)
        (void)trigger(r, p, now_ms);
}

/* ---------------------------------------------------------------------------
 * 驱动
 * ------------------------------------------------------------------------- */
int iv_recovery_tick(iv_recovery_t *r, uint64_t now_ms)
{
    int i;
    int sent = 0;

    if (r == NULL)
        return IV_EINVAL;
    if (!r->opened)
        return IV_ESTATE;

    /* ① 断电到点 ⇒ 该通道的 ON 入队（队列满就下一拍再试，**不丢**） */
    for (i = 0; i < (int)IV_RECOVERY_PORT_N; i++) {
        if (r->ch[i].phase != IV_RECOVERY_ST_OFF)
            continue;
        if (now_ms < r->ch[i].off_until_ms)
            continue;
        if (pend_push(r, i, (int)IV_RECOVERY_PWR_ON) == IV_OK)
            r->ch[i].phase = IV_RECOVERY_ST_Q_ON;
    }

    /* ② 最多下发 1 条：EBUSY ⇒ 留队头，下一拍再试 */
    if (r->pend_n != 0u) {
        int port = (int)r->pend[0].port;
        int st   = (int)r->pend[0].status;
        int rc   = emit(r, port, st);

        if (rc == IV_OK) {
            pend_pop(r);
            sent = 1;
            if (st == (int)IV_RECOVERY_PWR_OFF) {
                r->ch[port].credit--;
                r->ch[port].phase        = IV_RECOVERY_ST_OFF;
                r->ch[port].off_until_ms = now_ms + (uint64_t)r->cfg.off_ms;
            } else {
                /* 上电成功才起冷却；失败时停在断电相位等下一拍重试 */
                r->ch[port].phase       = IV_RECOVERY_ST_IDLE;
                r->ch[port].cd_until_ms = now_ms + (uint64_t)r->cfg.cooldown_ms;
                r->cycles++;
            }
        } else if (rc == IV_EBUSY) {
            r->tx_busy++; /* 上一笔还在飞：保持队头 */
        } else {
            /* 其它错误（如载荷非法/发送队列拒绝）：丢弃该条并计数。
             * ON 失败时**停在 Q_ON**，下一拍重新入队重试——不能让继电器
             * 留在断电态。 */
            r->tx_err++;
            if (st == (int)IV_RECOVERY_PWR_ON) {
                r->ch[port].phase = IV_RECOVERY_ST_OFF;
                r->ch[port].off_until_ms = now_ms; /* 立即重排队 */
            } else {
                r->ch[port].phase = IV_RECOVERY_ST_IDLE;
            }
            pend_pop(r);
        }
    }
    return sent;
}

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */
int iv_recovery_port_phase(const iv_recovery_t *r, int port)
{
    if (r == NULL || !port_ok(port))
        return IV_EINVAL;
    return (int)r->ch[port].phase;
}

int iv_recovery_port_credit(const iv_recovery_t *r, int port)
{
    if (r == NULL || !port_ok(port))
        return IV_EINVAL;
    return (int)r->ch[port].credit;
}

int iv_recovery_pending(const iv_recovery_t *r)
{
    if (r == NULL)
        return IV_EINVAL;
    return (int)r->pend_n;
}
