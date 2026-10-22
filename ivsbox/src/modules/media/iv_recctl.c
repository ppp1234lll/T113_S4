/*
 * 断网录像状态机实现（契约见 iv_recctl.h 文件头）
 */
#include "ivsbox/iv_recctl.h"

#include <string.h>

#include "ivsbox/iv_log.h"

#define RC_MOD "recctl"

void iv_recctl_cfg_default(iv_recctl_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->hold_ms     = IV_RECCTL_HOLD_MS_DEFAULT;
    cfg->debounce_ms = IV_RECCTL_DEBOUNCE_MS_DEFAULT;
}

int iv_recctl_init(iv_recctl_t *r, const iv_recctl_cfg_t *cfg)
{
    if (r == NULL || cfg == NULL)
        return IV_EINVAL;
    r->cfg           = *cfg;
    r->recording     = 0;
    r->down_since_ms = 0;
    r->up_since_ms   = 0;
    r->gate_block    = 0;
    r->gate_stop     = 0;
    return IV_OK;
}

static int start_all(iv_recctl_t *r, const iv_recctl_ops_t *ops)
{
    int i;
    int ok = 0;

    for (i = 0; i < r->cfg.n_url && i < IV_RECCTL_CH_N; i++) {
        if (r->cfg.url[i][0] == '\0')
            continue; /* 空槽跳过（允许表中间留洞） */
        if (ops->start(i, r->cfg.url[i], ops->arg) == IV_OK)
            ok++;
        else
            IV_LOG_W(RC_MOD, "record start ch%d rejected", i);
    }
    if (ok > 0) {
        r->recording     = 1;
        r->down_since_ms = 0;
        r->up_since_ms   = 0;
        IV_LOG_I(RC_MOD, "offline recording started (%d/%d ch)",
                 ok, r->cfg.n_url);
        return 1;
    }
    /* 一路都没受理（media 不在等）⇒ 不置位 recording，下一拍重试；
     * down_since 保留，保持"全断"计时连续。 */
    IV_LOG_W(RC_MOD, "offline recording start failed on all channels");
    return 0;
}

static int stop_all(iv_recctl_t *r, const iv_recctl_ops_t *ops)
{
    int i;
    int ok = 0;

    for (i = 0; i < r->cfg.n_url && i < IV_RECCTL_CH_N; i++) {
        if (r->cfg.url[i][0] == '\0')
            continue;
        if (ops->stop(i, ops->arg) == IV_OK)
            ok++;
        else
            IV_LOG_W(RC_MOD, "record stop ch%d rejected", i);
    }
    r->recording     = 0;
    r->down_since_ms = 0;
    r->up_since_ms   = 0;
    IV_LOG_I(RC_MOD, "offline recording stopped (%d/%d ch ok)", ok, r->cfg.n_url);
    return 1;
}

int iv_recctl_step(iv_recctl_t *r, int wan_up, int plat_up, uint64_t now_ms,
                   const iv_recctl_ops_t *ops)
{
    int all_down = (wan_up == 0 && plat_up == 0);

    if (r == NULL || ops == NULL)
        return IV_EINVAL;
    if (r->cfg.n_url <= 0)
        return 0; /* 没配录像源：状态机旁路（但缓存不更新也无妨） */

    /* S6.5 容量闸门（兜底路径）：闸门要求停录就立刻停，不等恢复防抖 ——
     * SD 满/故障时继续录只会把写失败刷满日志。装配层通常已用 gate() 先处理过，
     * 这里兜住"调用方漏调 gate"的情形。 */
    if (r->gate_stop && r->recording)
        return stop_all(r, ops);

    if (r->recording) {
        if (all_down) {
            r->up_since_ms = 0; /* 还断着：恢复计时清零 */
            return 0;
        }
        /* 恢复（任一条件活）：防抖 */
        if (r->up_since_ms == 0) {
            r->up_since_ms = now_ms;
            return 0;
        }
        if (now_ms - r->up_since_ms >= r->cfg.debounce_ms)
            return stop_all(r, ops);
        return 0;
    }

    /* 未在录 */
    if (!all_down) {
        r->down_since_ms = 0;
        return 0;
    }
    if (r->down_since_ms == 0) {
        r->down_since_ms = now_ms;
        return 0;
    }
    if (now_ms - r->down_since_ms >= r->cfg.hold_ms) {
        /* S6.5：没空间（水位 ≥90% 或 SD 故障）时不**开**新录像 —— 开了也写不下去。
         * down_since 保留：腾出空间后下一拍闸门放开即开录，不必重新等 hold。 */
        if (r->gate_block)
            return 0;
        return start_all(r, ops);
    }
    return 0;
}

int iv_recctl_gate(iv_recctl_t *r, int allow_new, int stop_rec,
                   const iv_recctl_ops_t *ops)
{
    int i;
    int n = 0;

    if (r == NULL)
        return IV_EINVAL;
    r->gate_block = allow_new ? 0u : 1u;
    r->gate_stop  = stop_rec ? 1u : 0u;

    if (!r->gate_stop || !r->recording)
        return 0;
    if (ops == NULL)
        return IV_EINVAL;

    for (i = 0; i < r->cfg.n_url && i < IV_RECCTL_CH_N; i++) {
        if (r->cfg.url[i][0] == '\0')
            continue;
        if (ops->stop(i, ops->arg) == IV_OK)
            n++;
    }
    r->recording     = 0;
    r->down_since_ms = 0;
    r->up_since_ms   = 0;
    IV_LOG_W(RC_MOD, "storage gate: recording stopped immediately (%d ch)", n);
    return n;
}

int iv_recctl_stop_all(iv_recctl_t *r, const iv_recctl_ops_t *ops)
{
    if (r == NULL || ops == NULL)
        return IV_EINVAL;
    if (!r->recording)
        return 0;
    (void)stop_all(r, ops);
    return 1;
}

int iv_recctl_recording(const iv_recctl_t *r)
{
    return (r != NULL) ? (int)r->recording : 0;
}

/* ---------------------------------------------------------------------------
 * 运行期换配置（2026-10-22：`camera.ch.N.url` / `.record` / `camera.rec.*` 热生效）
 *
 * 两种情形分开处理，**不能一律 re-init**：
 *   - 没在录：直接把新配置装进去，状态清零即可（与 init 等价）；
 *   - 正在录：先用**旧的** ops 把这一轮收口（stop_all），再按新表重开。
 *     否则被改掉/删掉的那一路 recorder 会**漏在后台**继续往 SD 卡写，而状态机
 *     已经忘了它 —— 这种泄漏在板上只能靠 `ps` 抓，日志里一片正常。
 *
 * 返回值：`IV_OK`；`IV_EINVAL` 入参非法（在录却没给 ops 也按非法处理，
 * 因为"不知道该叫谁停"）。
 * -------------------------------------------------------------------------- */
int iv_recctl_set_cfg(iv_recctl_t *r, const iv_recctl_cfg_t *cfg,
                      const iv_recctl_ops_t *ops)
{
    int was;

    if (r == NULL || cfg == NULL)
        return IV_EINVAL;
    was = r->recording;
    if (was && ops == NULL)
        return IV_EINVAL;

    if (was)
        (void)iv_recctl_stop_all(r, ops);
    if (iv_recctl_init(r, cfg) != IV_OK)
        return IV_EINVAL;
    if (was)
        (void)start_all(r, ops); /* 新表重开；一路都没起来时 recording 保持 0 */
    return IV_OK;
}
