/*
 * 断网录像状态机实现（契约见 iv_recctl.h 文件头）
 *
 * 判据：只看主网（有线 WAN）。4G 与平台链路不参与。
 * 失败：start 带原因，status 复查真实状态，原因留成人能看懂的中文。
 * 手动：force 标记绕过主网判据（见 iv_recctl_manual）。
 */
#include "ivsbox/iv_recctl.h"

#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_log.h"

#define RC_MOD "recctl"

/* 无原因时的兜底文案（绝不写空串：空串在界面上等于"没原因"，
 * 而这一路确实起不来，用户需要知道"系统自己也没拿到原因"）。 */
#define WHY_FALLBACK "录像通道启动被拒（对端未给出原因）"

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
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;
    return IV_OK;
}

static void set_why(iv_recctl_t *r, int id, const char *why)
{
    if (id < 0 || id >= IV_RECCTL_CH_N)
        return;
    r->ch_failed[id] = 1;
    (void)snprintf(r->ch_why[id], sizeof(r->ch_why[id]), "%s",
                   (why != NULL && why[0] != '\0') ? why : WHY_FALLBACK);
}

static void clear_why(iv_recctl_t *r, int id)
{
    if (id < 0 || id >= IV_RECCTL_CH_N)
        return;
    r->ch_failed[id]  = 0;
    r->ch_why[id][0]  = '\0';
}

/* 把**第一路**失败原因抄出去（手动启动失败时给页面一句话）。
 * 没有失败原因时写兜底文案 —— 调用方拿到的永远不是空串。 */
static void copy_first_why(const iv_recctl_t *r, char *why, size_t cap)
{
    int i;

    if (why == NULL || cap == 0u)
        return;
    for (i = 0; i < r->cfg.n_url && i < IV_RECCTL_CH_N; i++) {
        if (!r->ch_failed[i])
            continue;
        (void)snprintf(why, cap, "%s",
                       (r->ch_why[i][0] != '\0') ? r->ch_why[i] : WHY_FALLBACK);
        return;
    }
    (void)snprintf(why, cap, "%s", WHY_FALLBACK);
}

/* `manual` 只影响日志措辞：自动路径的日志读作"main link down"，
 * 手动路径要是也这么说，现场看日志会以为主网又断了。 */
static int start_all(iv_recctl_t *r, uint64_t now_ms, const iv_recctl_ops_t *ops,
                     int manual)
{
    int i;
    int ok = 0;

    for (i = 0; i < r->cfg.n_url && i < IV_RECCTL_CH_N; i++) {
        char why[IV_RECCTL_WHY_MAX];
        int  rc;

        if (r->cfg.url[i][0] == '\0')
            continue; /* 空槽跳过（允许表中间留洞） */
        why[0] = '\0';
        rc     = ops->start(i, r->cfg.url[i], why, sizeof(why), ops->arg);
        if (rc == IV_OK) {
            clear_why(r, i);
            ok++;
        } else {
            set_why(r, i, why);
            /* 一行讲清"第几路、为什么"，现场照这条就能修 */
            IV_LOG_W(RC_MOD, "record ch%d start failed (%d): %s", i, rc,
                     r->ch_why[i]);
        }
    }

    if (ok > 0) {
        r->recording     = 1;
        r->down_since_ms = 0;
        r->up_since_ms   = 0;
        r->next_try_ms   = 0;
        /* 受理 ≠ 在录：3 s 后由 confirm_pending() 复查真实状态 */
        r->pending    = 1;
        r->pending_ms = now_ms;
        IV_LOG_I(RC_MOD, "%s: recording started (%d/%d ch), confirming in %ums",
                 manual ? "manual" : "main link down", ok, r->cfg.n_url,
                 (unsigned)IV_RECCTL_CONFIRM_MS);
        return 1;
    }

    /* 一路都没受理 ⇒ 不置位 recording，按退避重试（预检要连 TCP，
     * 每拍重来会拖住 Reactor 单线程）。down_since 保留，"主网断"计时连续。 */
    r->next_try_ms = now_ms + IV_RECCTL_RETRY_MS;
    IV_LOG_W(RC_MOD, "%s but no channel could start; retry in %us",
             manual ? "manual start" : "main link down",
             (unsigned)(IV_RECCTL_RETRY_MS / 1000u));
    return 0;
}

/*
 * 启动确认：受理 3 s 后复查真实状态。
 * **这是账密错误唯一能被发现的路径** —— 桥接层预检只看 TCP 通不通，
 * 账密错时 TCP 是通的、/record/start 也回 200，只有 ffmpeg 真拉流才失败。
 */
static void confirm_pending(iv_recctl_t *r, uint64_t now_ms,
                            const iv_recctl_ops_t *ops)
{
    int i;
    int alive = 0;

    if (!r->pending)
        return;
    /* `iv_recctl_set_cfg()` 重开时没有 now（只有 ops），传 0 表示"从下一拍起算"。
     * 若不补这一记，重开后的第一拍就会立刻确认 —— 那时 ffmpeg 还没建好流，
     * 必然误报"未进入录像"。 */
    if (r->pending_ms == 0) {
        r->pending_ms = now_ms;
        return;
    }
    if (now_ms - r->pending_ms < IV_RECCTL_CONFIRM_MS)
        return;
    r->pending = 0;
    if (ops->status == NULL)
        return; /* 没有复查回调：保持"受理即成功"的降级口径 */

    for (i = 0; i < r->cfg.n_url && i < IV_RECCTL_CH_N; i++) {
        int st;

        if (r->cfg.url[i][0] == '\0')
            continue;
        st = ops->status(i, ops->arg);
        if (st > 0) {
            clear_why(r, i);
            alive++;
        } else if (st == 0) {
            set_why(r, i, "已受理但未进入录像（拉流失败：账密/编码/路径不匹配）");
            IV_LOG_W(RC_MOD, "record ch%d confirmed NOT recording: %s", i,
                     r->ch_why[i]);
        }
        /* st < 0：媒体桥查不到（进程没了等），既不算活也不判死 —— 原因留空，
         * 由 record.status 的 reason 字段报"media 不可达"。 */
    }

    if (alive == 0 && r->recording) {
        r->recording   = 0;
        r->next_try_ms = now_ms + IV_RECCTL_RETRY_MS;
        IV_LOG_W(RC_MOD, "no channel actually recording after confirm; "
                         "retry in %us", (unsigned)(IV_RECCTL_RETRY_MS / 1000u));
    } else if (alive > 0) {
        IV_LOG_I(RC_MOD, "record confirm: %d ch actually recording", alive);
    }
}

/* 逐路停 + 清全部状态。**强制态在这里一并清掉**：stop_all 是"整机停录"的唯一
 * 出口（人工停 / 闸门停 / 换配置停都走它），停了就没人再强制。 */
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
    r->pending       = 0;
    r->next_try_ms   = 0;
    r->force         = 0;
    IV_LOG_I(RC_MOD, "recording stopped (%d/%d ch ok)", ok, r->cfg.n_url);
    return 1;
}

int iv_recctl_step(iv_recctl_t *r, int main_up, uint64_t now_ms,
                   const iv_recctl_ops_t *ops)
{
    if (r == NULL || ops == NULL || ops->start == NULL || ops->stop == NULL)
        return IV_EINVAL;
    if (r->cfg.n_url <= 0)
        return 0; /* 没配录像源：状态机旁路（但缓存不更新也无妨） */

    /* S6.5 容量闸门（兜底路径）：闸门要求停录就立刻停，不等恢复防抖 ——
     * SD 满/故障时继续录只会把写失败刷满日志。装配层通常已用 gate() 先处理过，
     * 这里兜住"调用方漏调 gate"的情形。 */
    if (r->gate_stop && r->recording)
        return stop_all(r, ops);

    /* 启动确认（与是否在录无关：它只关心"上一轮受理的那批到底在不在录"） */
    confirm_pending(r, now_ms, ops);

    if (r->recording) {
        /* 手动态：主网通不通都不影响（用户口径"按下按钮就得一直录"）。
         * 只把 up_since 清零，免得将来退出强制态时拿着一个陈旧的"恢复时刻"
         * 立刻触发防抖停录。 */
        if (r->force) {
            r->up_since_ms = 0;
            return 0;
        }
        if (!main_up) {
            r->up_since_ms = 0; /* 主网还断着：恢复计时清零 */
            return 0;
        }
        /* 主网恢复：防抖 */
        if (r->up_since_ms == 0) {
            r->up_since_ms = now_ms;
            return 0;
        }
        if (now_ms - r->up_since_ms >= r->cfg.debounce_ms)
            return stop_all(r, ops);
        return 0;
    }

    /* 手动态但没在录（启动全失败、或确认后判死）：按退避重试，**不看主网**。
     * 少了这一段，"主网正常时点开始录像"会变成点一次亮一下、再也不动 ——
     * 因为下面的自动路径一判 main_up 就直接 return 了。 */
    if (r->force) {
        if (r->gate_block)
            return 0;
        if (r->next_try_ms != 0 && now_ms < r->next_try_ms)
            return 0;
        return start_all(r, now_ms, ops, 1);
    }

    /* 未在录（自动路径） */
    if (main_up) {
        /* 主网回来了 ⇒ 人工"停止录像"的压制到此解除（"这次故障过去了"）。
         * 不解除的话，一次点击会把"断网录像"这条安全功能永久关掉 —— 那与产品
         * 口径冲突（见头文件）。 */
        r->manual_off    = 0;
        r->down_since_ms = 0;
        return 0;
    }
    /* 人工压制：主网断着，但用户按过"停止录像" ⇒ 不起自动录像。
     * 2026-10-28 板端实测过不压的情形：点停止后 10 s（hold）又自己录起来，
     * 现场只会认为按钮坏了。 */
    if (r->manual_off)
        return 0;
    if (r->down_since_ms == 0) {
        r->down_since_ms = now_ms;
        return 0;
    }
    if (now_ms - r->down_since_ms < r->cfg.hold_ms)
        return 0;
    /* S6.5：没空间（水位 ≥90% 或 SD 故障）时不**开**新录像 —— 开了也写不下去。
     * down_since 保留：腾出空间后下一拍闸门放开即开录，不必重新等 hold。 */
    if (r->gate_block)
        return 0;
    /* 失败退避：刚试过一轮且全失败，等退避到期再来 */
    if (r->next_try_ms != 0 && now_ms < r->next_try_ms)
        return 0;
    return start_all(r, now_ms, ops, 0);
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
    if (ops == NULL || ops->stop == NULL)
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
    r->pending       = 0;
    r->next_try_ms   = 0;
    r->force         = 0;
    IV_LOG_W(RC_MOD, "storage gate: recording stopped immediately (%d ch)", n);
    return n;
}

int iv_recctl_stop_all(iv_recctl_t *r, const iv_recctl_ops_t *ops)
{
    if (r == NULL || ops == NULL || ops->stop == NULL)
        return IV_EINVAL;
    if (!r->recording) {
        r->pending = 0;
        r->force   = 0; /* 没在录也要退出强制态，否则下一拍又会自己开录 */
        return 0;
    }
    (void)stop_all(r, ops);
    return 1;
}

int iv_recctl_recording(const iv_recctl_t *r)
{
    return (r != NULL) ? (int)r->recording : 0;
}

int iv_recctl_forced(const iv_recctl_t *r)
{
    return (r != NULL) ? (int)r->force : 0;
}

int iv_recctl_held_off(const iv_recctl_t *r)
{
    return (r != NULL) ? (int)r->manual_off : 0;
}

/* ---------------------------------------------------------------------------
 * 手动（强制）录像 —— 语义见头文件"手动（强制）录像"一节
 *
 * 与自动路径**共用** start_all / stop_all：走的仍是同一条预检＋两级确认，
 * 所以手动启动失败给出的原因与自动路径逐字相同（现场不用记两套说法）。
 * 唯一的差别是 `force` 标记 —— 它让 step 的自动判据绕开这一轮录像。
 * -------------------------------------------------------------------------- */
int iv_recctl_manual(iv_recctl_t *r, int on, uint64_t now_ms,
                     const iv_recctl_ops_t *ops, char *why, size_t whycap)
{
    int n;

    if (why != NULL && whycap > 0u)
        why[0] = '\0';
    if (r == NULL || ops == NULL || ops->start == NULL || ops->stop == NULL)
        return IV_EINVAL;
    /* 表空 ＝ 配置上没有任何一路"既配了地址又打开了录像开关"。
     * 这不是"按错了"，是**该去配置页**：把这句话原样给到页面。 */
    if (r->cfg.n_url <= 0) {
        if (why != NULL && whycap > 0u)
            (void)snprintf(why, whycap,
                           "没有可录的通道：需同时配好 camera.ch.N.url 与 "
                           "camera.ch.N.record");
        return IV_ESTATE;
    }

    if (on) {
        r->manual_off = 0; /* 人工"开始"当然要撤掉上一次的压制 */
        if (r->recording) {
            /* 已经在录（自动态）⇒ 升格成强制态，不重发 start：
             * 桥接层对"已在录"的那一路本就回 OK，但重发会多一轮无谓的
             * HTTP + 预检，而结果完全相同。 */
            r->force       = 1;
            r->up_since_ms = 0;
            r->next_try_ms = 0;
            IV_LOG_I(RC_MOD, "manual recording: already recording, forced on");
            return 1;
        }
        r->force         = 1;
        r->down_since_ms = 0; /* 手动态与"主网断"计时无关 */
        r->next_try_ms   = 0;
        n = start_all(r, now_ms, ops, 1);
        if (n == 0) {
            /* 一路都没受理：**立刻退出强制态**。留着的话 step 每 30 s 空转
             * 一轮预检，而页面已经明确告诉用户"没起来"，不需要后台偷偷重试。 */
            r->force = 0;
            copy_first_why(r, why, whycap);
            return IV_ESTATE;
        }
        return 1;
    }

    r->force = 0;
    if (!r->recording) {
        r->pending     = 0;
        r->next_try_ms = 0;
        /* 本来没在录也要置压制位：主网断着的话，不置它 10 s 后自动判据就会开录，
         * 用户看到的现象就是"按了停止，过一会儿又自己录了"。 */
        if (r->manual_off) {
            IV_LOG_I(RC_MOD, "manual recording: already stopped");
            return 0;
        }
        r->manual_off = 1;
        IV_LOG_I(RC_MOD, "manual recording: auto start suppressed until the "
                         "main link comes back");
        return 1;
    }
    (void)stop_all(r, ops);
    /* stop_all 只清 force；压制位在这里补上（顺序不能反：stop_all 之后置，
     * 免得将来有人给它加一句"顺手清 manual_off"） */
    r->manual_off = 1;
    IV_LOG_I(RC_MOD, "manual recording: stopped, auto start suppressed until the "
                     "main link comes back");
    return 1;
}

int iv_recctl_fail_reason(const iv_recctl_t *r, int id, char *buf, size_t cap)
{
    if (r == NULL || buf == NULL || cap == 0)
        return -1;
    if (id < 0 || id >= IV_RECCTL_CH_N)
        return -1;
    buf[0] = '\0';
    if (!r->ch_failed[id])
        return 0;
    /* 头文件承诺"永不空串"：写入侧已有兜底，读取侧再兜一次 ——
     * 空字符串在界面上等于"没原因"，而这一路确实起不来。 */
    (void)snprintf(buf, cap, "%s",
                   (r->ch_why[id][0] != '\0') ? r->ch_why[id] : WHY_FALLBACK);
    return 1;
}

int iv_recctl_failed(const iv_recctl_t *r, int id)
{
    if (r == NULL || id < 0 || id >= IV_RECCTL_CH_N)
        return 0;
    return (int)r->ch_failed[id];
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
                      const iv_recctl_ops_t *ops, uint64_t now_ms)
{
    int was;

    if (r == NULL || cfg == NULL)
        return IV_EINVAL;
    was = r->recording;
    if (was && (ops == NULL || ops->start == NULL || ops->stop == NULL))
        return IV_EINVAL;

    /* 热更新会丢掉强制态（`stop_all` 清 force，`init` 也清）：新表重开是一次
     * 全新的开始，人工那份"强制"不该跨过配置变更继续生效 —— 否则用户改了地址
     * 之后录像行为"改不回来"，还得再点一次停止才能回到自动。 */
    if (was)
        (void)iv_recctl_stop_all(r, ops);
    if (iv_recctl_init(r, cfg) != IV_OK)
        return IV_EINVAL;
    if (was)
        (void)start_all(r, now_ms, ops, 0); /* 新表重开；一路都没起来时 recording 保持 0 */
    return IV_OK;
}
