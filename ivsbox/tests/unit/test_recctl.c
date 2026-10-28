/*
 * iv_recctl 单测（libivmodules，功能开发计划 M6-S6.3）
 *
 * 状态机是纯逻辑：时间轴由用例注入（now_ms 自拟），动作经假 ops 记录到数组。
 * 覆盖九组：
 *   c01 无 URL 表 ⇒ 状态机旁路
 *   c02 主网断保持 hold 才开录；恢复未达 debounce 不停录
 *   c03 恢复达 debounce 停录；停后可再次触发（down_since 重新计时）
 *   c04 防抖期里主网再断 ⇒ 恢复计时清零（不误停）
 *   c05 start 全被拒 ⇒ 不置位 recording、按**退避**重试（不是每拍重试）
 *   c06 stop_all 手动收尾 + recording 读数
 *   c07 S6.5 容量闸门：关闸抑制开录、开闸即录、水位告急立即停录、step 兜底路径
 *   c08 运行期换配置（`iv_recctl_set_cfg`）：没在录＝等价 init；正在录＝先用旧表
 *       收口再按新表重开；在录却没给 ops 回 `IV_EINVAL`；新表全空则停完不重开
 *   c09 失败原因（2026-10-28）：拒因原样落进 `iv_recctl_fail_reason()`，
 *       成功即清；没给原因时给兜底文案（绝不返回空串）
 *   c10 启动确认（2026-10-28）：受理 ≠ 在录。status 复查发现没在录 ⇒
 *       recording 归 0、原因写明"已受理但未进入录像"；status 说在录 ⇒ 保持
 *   c11 判据只看主网（2026-10-28）：`main_up=1` 就停、`main_up=0` 就录，
 *       **没有任何其它入参**能改变这个结论（旧口径的 4G/平台参数已删除）
 *   c12 手动（强制）录像（2026-10-28）：on=1 立即开录且**不受主网状态影响**、
 *       自动态下按"立即录像"只升格不重发、总失败时回原因并**退出强制态**、
 *       闸门/热更新照样能停它；on=0 还要**压住自动开录**（否则 10 s 后自己又录，
 *       按钮看起来是坏的），且主网恢复一次即解除压制
 *   c13 运行期复检（2026-10-28 板端）：启动确认只做一次，断流之后没人再看 ⇒
 *       录像永久停、页面还显示"录制中"。这里钉三件事：全部路掉线 ⇒ recording
 *       归 0、退避一轮后按原路径重开（且**不重走 hold 保持期**）；部分路掉线 ⇒
 *       立刻重开整批且 recording 保持；复检**节流**（10 s 内不重复查）并且
 *       在启动确认未完成（pending）时不参与
 *
 * 反向证伪（本用例内建）：
 *   f1 若把"保持"判据去掉（到达 hold 前就开录），c02 的保持段会提前动作；
 *   f2 若把"恢复防抖"去掉（恢复沿立即停录），c02 的恢复段会提前动作；
 *   f3 若把"防抖期内再断不清零恢复计时"，c04 会在 60 s 后误停；
 *   f4 若去掉"闸门关时不核准开录"，c07 会在关闸状态下开录；
 *   f5 若去掉"闸门要求停录即立即停"，c07 会一直录到恢复防抖；
 *   f6 若把 `iv_recctl_set_cfg()` 里的"在录先 stop_all"去掉（只换 cfg），
 *      c08 的 `n_stop` 会变 0 —— 被删掉那一路的 recorder 会漏在后台写 SD 卡；
 *   f7 若把失败退避去掉（回到"每拍重试"），c05 的 att 计数会在退避窗口内继续涨；
 *   f8 若把启动确认去掉（受理即算在录），c10 的 recording 会一直是 1，
 *      "账密错"就永远查不出来；
 *   f9 若把原因记录去掉，c09 的 `fail_reason` 会返回 0；
 *   f11 若 `iv_recctl_set_cfg()` 不拿调用方给的 now 设退避（改用 0），c08 的
 *       ⑥ 会红 —— 现场形态是"热更新后 11 s 就重试"，每拍吃一次 500 ms 预检。
 *   f12 若把 force 分支从 `iv_recctl_step()` 里去掉（手动录像照样受主网管），
 *       c12 ③/⑤ 会在主网恢复 60 s 后被停；
 *   f13 若启动总失败后不清 force，c12 ⑥ 那句"后台没有静默重试"会红。
 *   f14 若手动停止不置压制位（只清 force），c12 ⑨ 会在 10 s 后被自动路径重新
 *       拉起来 —— 板端现象就是"按了停止，过一会儿又自己录了"。
 *   f15 若把运行期复检去掉（`if (r->recording)` 里不调 `recheck_channels()`），
 *       c13 ①②③ 全红：`recording` 永远停在 1，`n_start` 不再增长 —— 板端现象
 *       就是"摄像机重启过一次，之后再也不录，界面却说在录"。
 *   f16 若复检不节流（每拍都查），c13 ④ 的 `status_calls` 增量会远超预期。
 * 以上都由对应的 chk 钉死：动作次数与时间是精确断言，多一次/少一次都红。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_recctl.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void noop_sink(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

/* ---------------- 假 ops：动作记录 ---------------- */
#define MAX_ACT 32
typedef struct {
    int  n_start;   /* 受理数 */
    int  n_start_att; /* 尝试数（含被拒） */
    int  n_stop;
    int  start_id[MAX_ACT];
    char start_url[MAX_ACT][IV_RECCTL_URL_MAX];
    int  stop_id[MAX_ACT];
    int  start_fail;      /* 置 1 时 start 一律拒绝 */
    const char *fail_why; /* 拒绝时写进 why 的文案（NULL ⇒ 不写，验兜底） */
    int  status_val;      /* status 回调的返回值：1 在录 / 0 未录 / -1 查不到 */
    int  status_calls;
    /* 逐路状态（c13 用）：`status_ch_set=1` 时按路返回 `status_ch[id]`，
     * 否则一律返回 `status_val`。用来复现"两路里只掉了一路"。 */
    int  status_ch_set;
    int  status_ch[IV_RECCTL_CH_N];
} fake_ops_t;

static int fake_start(int id, const char *url, char *why, size_t whycap,
                      void *arg)
{
    fake_ops_t *f = arg;

    f->n_start_att++;
    if (f->start_fail) {
        if (why != NULL && whycap > 0 && f->fail_why != NULL)
            (void)snprintf(why, whycap, "%s", f->fail_why);
        return IV_ECONN;
    }
    if (f->n_start < MAX_ACT) {
        f->start_id[f->n_start] = id;
        snprintf(f->start_url[f->n_start], IV_RECCTL_URL_MAX, "%s", url);
    }
    f->n_start++;
    return IV_OK;
}

static int fake_stop(int id, void *arg)
{
    fake_ops_t *f = arg;

    if (f->n_stop < MAX_ACT)
        f->stop_id[f->n_stop] = id;
    f->n_stop++;
    return IV_OK;
}

static int fake_status(int id, void *arg)
{
    fake_ops_t *f = arg;

    f->status_calls++;
    if (f->status_ch_set && id >= 0 && id < IV_RECCTL_CH_N)
        return f->status_ch[id];
    return f->status_val;
}

static void ops_of(iv_recctl_ops_t *ops, fake_ops_t *f)
{
    ops->start  = fake_start;
    ops->stop   = fake_stop;
    ops->status = fake_status;
    ops->arg    = f;
}

/* 每次 memset 之后必须调用：默认"确认时确实在录"，否则每条开录用例都会
 * 在 3 s 后被判成"已受理但未进入录像"。 */
static void fake_reset(fake_ops_t *f)
{
    memset(f, 0, sizeof(*f));
    f->status_val = 1;
}

static void cfg_two_urls(iv_recctl_cfg_t *cfg)
{
    iv_recctl_cfg_default(cfg);
    cfg->n_url = 2;
    snprintf(cfg->url[0], sizeof(cfg->url[0]), "rtsp://10.0.0.11/ch0");
    snprintf(cfg->url[1], sizeof(cfg->url[1]), "rtsp://10.0.0.12/ch1");
}

int main(void)
{
    iv_recctl_t     r;
    iv_recctl_cfg_t cfg;
    iv_recctl_ops_t ops;
    fake_ops_t      f;

    iv_log_set_sink(noop_sink, NULL);

    /* ---------------- c01 无 URL 表：旁路 ---------------- */
    iv_recctl_cfg_default(&cfg); /* n_url = 0 */
    chk(iv_recctl_init(&r, &cfg) == IV_OK, "c01: init ok");
    fake_reset(&f);
    ops_of(&ops, &f);
    chk(iv_recctl_step(&r, 0, 1000000, &ops) == 0,
        "c01: no urls -> step is a no-op even after ages of outage");
    chk(f.n_start == 0 && f.n_stop == 0, "c01: no actions fired");
    chk(iv_recctl_recording(&r) == 0, "c01: not recording");

    /* ---------------- c02 保持开录 / 防抖停录 ---------------- */
    cfg_two_urls(&cfg);
    chk(iv_recctl_init(&r, &cfg) == IV_OK, "c02: init ok");
    fake_reset(&f);

    /* 主网断的第 0 拍：只起计时 */
    chk(iv_recctl_step(&r, 0, 10000, &ops) == 0, "c02: down edge times only");
    chk(f.n_start == 0, "c02: no immediate start on edge");
    /* hold=10s：差 1ms 也不许动作（f1） */
    chk(iv_recctl_step(&r, 0, 10000 + 9999, &ops) == 0, "c02: below hold");
    chk(f.n_start == 0, "c02: no start below hold");
    /* 到 hold：逐路开录 */
    chk(iv_recctl_step(&r, 0, 10000 + 10000, &ops) == 1, "c02: start on hold");
    chk(f.n_start == 2, "c02: both channels started");
    chk(f.start_id[0] == 0 && f.start_id[1] == 1, "c02: ids in table order");
    chk(strcmp(f.start_url[0], "rtsp://10.0.0.11/ch0") == 0, "c02: url passed through");
    chk(iv_recctl_recording(&r) == 1, "c02: recording flag set");
    /* 仍断着：无动作（这一拍同时完成启动确认，status 说在录 ⇒ 保持） */
    chk(iv_recctl_step(&r, 0, 30000, &ops) == 0, "c02: steady outage");
    chk(f.n_stop == 0, "c02: no stop while outage persists");
    chk(iv_recctl_recording(&r) == 1, "c02: confirm keeps recording when status says so");
    chk(f.status_calls > 0, "c02: confirm actually queried the bridge");
    /* 主网恢复沿：只起防抖 */
    chk(iv_recctl_step(&r, 1, 31000, &ops) == 0, "c02: recovery edge debounces");
    chk(f.n_stop == 0, "c02: no immediate stop on recovery (f2)");
    /* debounce=60s：差一点不停 */
    chk(iv_recctl_step(&r, 1, 31000 + 59999, &ops) == 0, "c02: below debounce");
    chk(f.n_stop == 0, "c02: no stop below debounce");
    /* 到 debounce：停录 */
    chk(iv_recctl_step(&r, 1, 31000 + 60000, &ops) == 1, "c02: stop on debounce");
    chk(f.n_stop == 2, "c02: both channels stopped");
    chk(iv_recctl_recording(&r) == 0, "c02: recording cleared");

    /* ---------------- c03 停后再触发 ---------------- */
    fake_reset(&f);
    chk(iv_recctl_step(&r, 0, 200000, &ops) == 0, "c03: down edge re-armed");
    chk(f.n_start == 0, "c03: hold re-armed from zero");
    chk(iv_recctl_step(&r, 0, 209999, &ops) == 0, "c03: below hold again");
    chk(iv_recctl_step(&r, 0, 210000, &ops) == 1, "c03: start again on hold");
    chk(f.n_start == 2, "c03: restarted both channels");
    chk(iv_recctl_recording(&r) == 1, "c03: recording again");

    /* ---------------- c04 防抖期里再断：恢复计时清零（f3） ---------------- */
    fake_reset(&f);
    chk(iv_recctl_step(&r, 1, 300000, &ops) == 0, "c04: recovery edge starts debounce");
    chk(f.n_stop == 0, "c04: no stop on the recovery edge");
    /* 防抖期里主网又断：恢复计时清零，回到"仍断着" */
    chk(iv_recctl_step(&r, 0, 330000, &ops) == 0, "c04: outage again resets debounce");
    chk(f.n_stop == 0, "c04: no stop after outage resumes");
    chk(r.up_since_ms == 0, "c04: up_since cleared when the link drops again (f3)");
    /* 再恢复：防抖从头计 60 s，到点才停 */
    chk(iv_recctl_step(&r, 1, 340000, &ops) == 0, "c04: recovery re-arms the debounce");
    chk(iv_recctl_step(&r, 1, 340000 + 59999, &ops) == 0, "c04: below debounce again");
    chk(f.n_stop == 0, "c04: debounce restarted from zero (f3)");
    chk(iv_recctl_step(&r, 1, 340000 + 60000, &ops) == 1, "c04: stop after full debounce");
    chk(f.n_stop == 2, "c04: stopped");

    /* ---------------- c05 start 全被拒：不置位、按退避重试（f7） ---------------- */
    fake_reset(&f);
    f.start_fail = 1;
    f.fail_why   = "摄像机网络不可达：10.0.0.11:554 连接超时（500ms）";
    chk(iv_recctl_init(&r, &cfg) == IV_OK, "c05: init ok");
    (void)iv_recctl_step(&r, 0, 1000, &ops);  /* 起计时 */
    (void)iv_recctl_step(&r, 0, 11000, &ops); /* 到 hold：全拒 */
    chk(f.n_start_att == 2, "c05: start attempted for both channels");
    chk(iv_recctl_recording(&r) == 0, "c05: recording NOT set on failure");
    /* 退避窗口内（30 s）：一拍都不许再试（f7） */
    (void)iv_recctl_step(&r, 0, 12000, &ops);
    (void)iv_recctl_step(&r, 0, 21000, &ops);
    (void)iv_recctl_step(&r, 0, 40999, &ops);
    chk(f.n_start_att == 2, "c05: no retry inside the backoff window (f7)");
    /* 退避到期：重试 */
    (void)iv_recctl_step(&r, 0, 41000, &ops);
    chk(f.n_start_att == 4, "c05: retried after the backoff expired");
    /* 恢复后停手 */
    f.start_fail = 0;
    (void)iv_recctl_step(&r, 1, 50000, &ops);
    chk(iv_recctl_recording(&r) == 0, "c05: clean state after recovery");

    /* ---------------- c06 stop_all 手动收尾 ---------------- */
    fake_reset(&f);
    chk(iv_recctl_init(&r, &cfg) == IV_OK, "c06: init ok");
    (void)iv_recctl_step(&r, 0, 1000, &ops);
    (void)iv_recctl_step(&r, 0, 11000, &ops);
    chk(iv_recctl_recording(&r) == 1, "c06: recording before manual stop");
    chk(iv_recctl_stop_all(&r, &ops) == 1, "c06: stop_all reports a transition");
    chk(f.n_stop == 2, "c06: both channels stopped");
    chk(iv_recctl_recording(&r) == 0, "c06: recording cleared");
    chk(iv_recctl_stop_all(&r, &ops) == 0, "c06: stop_all idempotent when idle");
    chk(f.n_stop == 2, "c06: no extra stop when idle");

    /* ---------------- c07 S6.5 容量闸门 ---------------- */
    fake_reset(&f);
    chk(iv_recctl_init(&r, &cfg) == IV_OK, "c07: init ok");

    /* 关闸（allow_new=0）：断网并保持 hold 之后**仍不**开录 */
    chk(iv_recctl_gate(&r, 0, 0, &ops) == 0, "c07: gate(block) reports nothing to stop");
    chk(iv_recctl_step(&r, 0, 1000, &ops) == 0, "c07: down edge armed");
    chk(iv_recctl_step(&r, 0, 11000, &ops) == 0, "c07: blocked start suppressed (f4)");
    chk(f.n_start == 0, "c07: no start while the storage gate blocks (f4)");
    chk(iv_recctl_recording(&r) == 0, "c07: not recording while blocked");

    /* 开闸：同一断网沿（down_since 连续）下一拍即开录，不必重新等 hold */
    chk(iv_recctl_gate(&r, 1, 0, &ops) == 0, "c07: gate(open)");
    chk(iv_recctl_step(&r, 0, 12000, &ops) == 1, "c07: start once the gate opens");
    chk(f.n_start == 2, "c07: both channels started after unblock");
    chk(iv_recctl_recording(&r) == 1, "c07: recording after unblock");

    /* 录制中水位告急（stop_rec=1）：立即停录，不等 debounce */
    fake_reset(&f);
    chk(iv_recctl_gate(&r, 0, 1, &ops) == 2, "c07: gate(stop) stops both channels (f5)");
    chk(f.n_stop == 2, "c07: immediate stop when storage is full (f5)");
    chk(iv_recctl_recording(&r) == 0, "c07: recording cleared by the gate");

    /* 闸门归位后不残留：需重新满足开录条件（hold 重新计时） */
    fake_reset(&f);
    chk(iv_recctl_gate(&r, 1, 0, &ops) == 0, "c07: gate reset");
    chk(iv_recctl_step(&r, 0, 20000, &ops) == 0, "c07: hold re-armed after gate stop");
    chk(f.n_start == 0, "c07: no instant restart after a gate stop");
    chk(iv_recctl_step(&r, 0, 30000, &ops) == 1, "c07: restart requires a fresh hold");
    chk(f.n_start == 2, "c07: restarted after hold");

    /* step() 兜底：绕过 gate 直接置位也要停录 */
    fake_reset(&f);
    r.gate_stop = 1;
    chk(iv_recctl_step(&r, 0, 31000, &ops) == 1, "c07: step honours gate_stop");
    chk(f.n_stop == 2, "c07: stopped via the step safe path");
    chk(iv_recctl_recording(&r) == 0, "c07: recording cleared by the safe path");
    r.gate_stop = 0;
    chk(iv_recctl_step(&r, 0, 40000, &ops) == 0, "c07: clean state after safe-path stop");

    /* ---------------- c08 运行期换配置（iv_recctl_set_cfg） ---------------- */
    {
        iv_recctl_cfg_t one;
        iv_recctl_cfg_t none;

        iv_recctl_cfg_default(&one);
        one.n_url = 1;
        snprintf(one.url[0], sizeof(one.url[0]), "rtsp://10.0.0.99/ch9");
        iv_recctl_cfg_default(&none); /* n_url = 0：新表全空 */

        /* 入参守卫 */
        chk(iv_recctl_set_cfg(NULL, &one, &ops, 0) == IV_EINVAL, "c08: NULL r -> EINVAL");
        chk(iv_recctl_set_cfg(&r, NULL, &ops, 0) == IV_EINVAL, "c08: NULL cfg -> EINVAL");

        /* ① 没在录：等价 init —— 换表、状态清零、不触发任何动作 */
        fake_reset(&f);
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c08: init with the 2-url table");
        r.down_since_ms = 777; /* 哨兵：init 必须把它清掉 */
        chk(iv_recctl_set_cfg(&r, &one, &ops, 0) == IV_OK, "c08: set_cfg while idle -> OK");
        chk(r.cfg.n_url == 1, "c08: idle swap takes the new table");
        chk(strcmp(r.cfg.url[0], "rtsp://10.0.0.99/ch9") == 0, "c08: idle swap takes the url");
        chk(r.down_since_ms == 0, "c08: idle swap zeroes the state (= init)");
        chk(f.n_start == 0 && f.n_stop == 0, "c08: idle swap fires no action");

        /* ② 在录：先用**旧**表收口（2 路各 stop），再按新表开（1 路）。
         * 只换 cfg 而不 stop 会把被删掉那一路的 recorder 漏在后台（f6）。 */
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c08: re-init with the 2-url table");
        (void)iv_recctl_step(&r, 0, 1000, &ops);  /* 起计时 */
        (void)iv_recctl_step(&r, 0, 11000, &ops); /* 到 hold：2 路开起来 */
        chk(iv_recctl_recording(&r) == 1, "c08: old table recording");
        fake_reset(&f);
        chk(iv_recctl_set_cfg(&r, &one, &ops, 0) == IV_OK, "c08: set_cfg while recording -> OK");
        chk(f.n_stop == 2, "c08: the OLD table is stopped first (2 ch, not 1) (f6)");
        chk(f.stop_id[0] == 0 && f.stop_id[1] == 1, "c08: stop ids follow the old table");
        chk(f.n_start == 1, "c08: the new table is started (1 ch)");
        chk(strcmp(f.start_url[0], "rtsp://10.0.0.99/ch9") == 0,
            "c08: the new url is used for the restart");
        chk(r.cfg.n_url == 1, "c08: cfg swapped while recording");
        chk(iv_recctl_recording(&r) == 1, "c08: still recording after the swap");

        /* ③ 在录却没给 ops ⇒ 不知道该叫谁停，按非法处理且状态一字不动 */
        fake_reset(&f);
        chk(iv_recctl_set_cfg(&r, &one, NULL, 0) == IV_EINVAL,
            "c08: recording but no ops -> EINVAL");
        chk(iv_recctl_recording(&r) == 1, "c08: rejected swap leaves recording alone");
        chk(r.cfg.n_url == 1, "c08: rejected swap leaves cfg alone");
        chk(f.n_stop == 0 && f.n_start == 0, "c08: rejected swap fires nothing");

        /* ④ 新表全空（所有路都被删/关掉）：停完不重开 */
        fake_reset(&f);
        chk(iv_recctl_set_cfg(&r, &none, &ops, 0) == IV_OK, "c08: swap to an empty table -> OK");
        chk(f.n_stop == 1, "c08: the last channel is stopped");
        chk(f.n_start == 0, "c08: nothing restarts with an empty table");
        chk(iv_recctl_recording(&r) == 0, "c08: recording cleared by the empty table");

        /* ⑤ 新表有地址但一路都起不来 ⇒ 不许把 recording 虚报回 1 */
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c08: re-init for the reject case");
        (void)iv_recctl_step(&r, 0, 1000, &ops);
        (void)iv_recctl_step(&r, 0, 11000, &ops);
        chk(iv_recctl_recording(&r) == 1, "c08: old channels recording (reject case)");
        fake_reset(&f);
        f.start_fail = 1;
        chk(iv_recctl_set_cfg(&r, &one, &ops, 11000) == IV_OK,
            "c08: swap with a failing start -> OK (state machine still consistent)");
        chk(f.n_stop == 2, "c08: old channels stopped anyway");
        chk(f.n_start_att == 1, "c08: new channel start attempted once");
        chk(iv_recctl_recording(&r) == 0,
            "c08: recording NOT faked when the new table cannot start");
        /* ⑥ 退避基准＝调用方给的 now（f11）。2026-10-28 现场踩到：这里传 0 会让
         * next_try 立刻过期，下一拍又打一轮预检（每拍 500 ms 卡住 Reactor）。 */
        chk(r.next_try_ms == 11000u + IV_RECCTL_RETRY_MS,
            "c08: backoff is anchored at the given now (f11)");
        {
            int att = f.n_start_att;

            (void)iv_recctl_step(&r, 0, 12000, &ops);
            chk(f.n_start_att == att, "c08: no immediate retry after a failed swap (f11)");
        }
        f.start_fail = 0;
    }

    /* ---------------- c09 失败原因（f9） ---------------- */
    {
        char why[IV_RECCTL_WHY_MAX];

        fake_reset(&f);
        f.start_fail = 1;
        f.fail_why   = "配置错误：拉流地址必须以 rtsp:// 开头";
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c09: init ok");
        why[0] = '\0';
        chk(iv_recctl_fail_reason(&r, 0, why, sizeof(why)) == 0,
            "c09: no reason before any attempt");
        (void)iv_recctl_step(&r, 0, 1000, &ops);
        (void)iv_recctl_step(&r, 0, 11000, &ops);
        chk(iv_recctl_fail_reason(&r, 0, why, sizeof(why)) == 1,
            "c09: reason recorded on failure (f9)");
        chk(strcmp(why, "配置错误：拉流地址必须以 rtsp:// 开头") == 0,
            "c09: the bridge's own wording is kept verbatim");
        chk(iv_recctl_failed(&r, 0) == 1, "c09: failed() agrees");

        /* 没给原因 ⇒ 兜底文案，绝不返回空串 */
        r.ch_why[0][0] = '\0';
        chk(iv_recctl_fail_reason(&r, 0, why, sizeof(why)) == 1,
            "c09: still reports a failure");
        chk(why[0] != '\0', "c09: never yields an empty reason");

        /* 下一次成功即清空，不留陈旧结论 */
        f.start_fail = 0;
        (void)iv_recctl_step(&r, 0, 41000, &ops); /* 退避到期后重试成功 */
        chk(iv_recctl_recording(&r) == 1, "c09: recording after a successful retry");
        chk(iv_recctl_fail_reason(&r, 0, why, sizeof(why)) == 0,
            "c09: success clears the reason");
    }

    /* ---------------- c10 启动确认：受理 ≠ 在录（f8） ---------------- */
    {
        char why[IV_RECCTL_WHY_MAX];

        fake_reset(&f);
        f.status_val = 0; /* 桥接说：已受理，但其实没在录（账密错的形态） */
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c10: init ok");
        (void)iv_recctl_step(&r, 0, 1000, &ops);
        (void)iv_recctl_step(&r, 0, 11000, &ops);
        chk(iv_recctl_recording(&r) == 1, "c10: accepted => optimistically recording");
        /* 3 s 内不复查（躲开正常建流窗口） */
        (void)iv_recctl_step(&r, 0, 13000, &ops);
        chk(f.status_calls == 0, "c10: no confirm inside the build-up window");
        chk(iv_recctl_recording(&r) == 1, "c10: still optimistic at 2 s");
        /* 到 3 s：复查发现没在录 ⇒ 不许再报在录（f8） */
        (void)iv_recctl_step(&r, 0, 14000, &ops);
        chk(f.status_calls > 0, "c10: confirm queried the bridge at 3 s");
        chk(iv_recctl_recording(&r) == 0, "c10: confirmed NOT recording (f8)");
        why[0] = '\0';
        chk(iv_recctl_fail_reason(&r, 0, why, sizeof(why)) == 1,
            "c10: reason set by the confirm step");
        chk(strstr(why, "未进入录像") != NULL,
            "c10: reason says accepted-but-not-recording");
        /* 确认失败后进入退避，不立刻再打一轮 */
        {
            int att = f.n_start_att;

            (void)iv_recctl_step(&r, 0, 15000, &ops);
            (void)iv_recctl_step(&r, 0, 20000, &ops);
            chk(f.n_start_att == att, "c10: backoff after a failed confirm");
        }

        /* 反过来：status 说在录 ⇒ 保持，且不留原因 */
        fake_reset(&f);
        f.status_val = 1;
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c10b: init ok");
        (void)iv_recctl_step(&r, 0, 1000, &ops);
        (void)iv_recctl_step(&r, 0, 11000, &ops);
        (void)iv_recctl_step(&r, 0, 14000, &ops);
        chk(f.status_calls > 0, "c10b: confirm ran");
        chk(iv_recctl_recording(&r) == 1, "c10b: still recording when the bridge agrees");
        chk(iv_recctl_fail_reason(&r, 0, why, sizeof(why)) == 0,
            "c10b: no reason when everything is fine");

        /* ops->status 为空 ⇒ 跳过确认（降级口径，不许崩） */
        fake_reset(&f);
        ops.status = NULL;
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c10c: init ok");
        (void)iv_recctl_step(&r, 0, 1000, &ops);
        (void)iv_recctl_step(&r, 0, 11000, &ops);
        (void)iv_recctl_step(&r, 0, 14000, &ops);
        chk(iv_recctl_recording(&r) == 1, "c10c: no status callback => no confirm");
        ops.status = fake_status;
    }

    /* ---------------- c11 判据只看主网（f10） ---------------- */
    {
        /* 旧口径的"4G/平台"参数已从接口删除：这里只能传 main_up。
         * 断言的是语义本身 —— 主网断（0）必录、主网通（1）必不录，
         * 与任何其它外部状态无关（编译期就保证没有别的入参）。 */
        fake_reset(&f);
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c11: init ok");
        (void)iv_recctl_step(&r, 1, 1000, &ops);
        (void)iv_recctl_step(&r, 1, 200000, &ops);
        chk(f.n_start == 0, "c11: main link up => never starts, no matter how long");
        (void)iv_recctl_step(&r, 0, 201000, &ops);
        chk(iv_recctl_step(&r, 0, 211000, &ops) == 1,
            "c11: main link down => starts after hold, with no other condition");
        chk(iv_recctl_recording(&r) == 1, "c11: recording driven by the main link alone");
    }

    /* ---------------- c12 手动（强制）录像（f12 / f13） ---------------- */
    {
        char why[IV_RECCTL_WHY_MAX];

        /* ① 没有可录的通道：这不是"按错了"，是**该去配置页** —— why 必须说清 */
        iv_recctl_cfg_default(&cfg); /* n_url = 0 */
        fake_reset(&f);
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c12: init with an empty table");
        why[0] = '\0';
        chk(iv_recctl_manual(&r, 1, 5000, &ops, why, sizeof(why)) == IV_ESTATE,
            "c12: manual start without any source -> ESTATE");
        chk(why[0] != '\0', "c12: an empty table still explains itself");
        chk(f.n_start == 0, "c12: nothing attempted on an empty table");
        chk(iv_recctl_forced(&r) == 0, "c12: not left in force mode");

        /* ② on=1 立刻开录：**不等 hold**（hold 是"等主网稳定"，人工不该等） */
        cfg_two_urls(&cfg);
        fake_reset(&f);
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c12: init ok");
        why[0] = '\0';
        chk(iv_recctl_manual(&r, 1, 5000, &ops, why, sizeof(why)) == 1,
            "c12: manual start reports a transition");
        chk(f.n_start == 2, "c12: both channels started immediately");
        chk(iv_recctl_recording(&r) == 1, "c12: recording flag set");
        chk(iv_recctl_forced(&r) == 1, "c12: force flag set");

        /* ③ 主网**通着**，且远超 debounce(60 s)：手动录像不许被停（f12） */
        (void)iv_recctl_step(&r, 1, 10000, &ops);
        (void)iv_recctl_step(&r, 1, 200000, &ops);
        chk(f.n_stop == 0, "c12: manual recording survives main-link recovery (f12)");
        chk(iv_recctl_recording(&r) == 1, "c12: still recording with force on (f12)");

        /* ④ on=0 ⇒ 停 + 退出强制态 */
        fake_reset(&f);
        chk(iv_recctl_manual(&r, 0, 201000, &ops, why, sizeof(why)) == 1,
            "c12: manual stop reports a transition");
        chk(f.n_stop == 2, "c12: both channels stopped");
        chk(iv_recctl_recording(&r) == 0, "c12: recording cleared");
        chk(iv_recctl_forced(&r) == 0, "c12: force cleared");
        chk(iv_recctl_manual(&r, 0, 202000, &ops, why, sizeof(why)) == 0,
            "c12: stopping an idle machine is a no-op");

        /* ⑤ 自动态（主网断）下按"立即录像"：只升格成强制，不重发 start */
        fake_reset(&f);
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c12: init for the upgrade case");
        (void)iv_recctl_step(&r, 0, 300000, &ops);
        (void)iv_recctl_step(&r, 0, 310000, &ops);
        chk(iv_recctl_recording(&r) == 1, "c12: auto recording first");
        {
            int ns = f.n_start;

            chk(iv_recctl_manual(&r, 1, 311000, &ops, why, sizeof(why)) == 1,
                "c12: manual start while already recording -> ok");
            chk(f.n_start == ns, "c12: no redundant start when already recording");
            chk(f.n_stop == 0, "c12: and no stop either");
            chk(iv_recctl_forced(&r) == 1, "c12: upgraded to the forced state");
        }
        /* 升格之后主网恢复同样停不下来（f12 的第二个侧面） */
        (void)iv_recctl_step(&r, 1, 400000, &ops);
        (void)iv_recctl_step(&r, 1, 600000, &ops);
        chk(f.n_stop == 0, "c12: an upgraded forced recording ignores recovery too (f12)");

        /* ⑥ 一路都起不来：ESTATE + 原因透传 + **不残留强制态**（f13） */
        fake_reset(&f);
        f.start_fail = 1;
        f.fail_why   = "摄像机网络不可达：10.0.0.11:554 连接超时（500ms）";
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c12: init for the reject case");
        why[0] = '\0';
        chk(iv_recctl_manual(&r, 1, 700000, &ops, why, sizeof(why)) == IV_ESTATE,
            "c12: all channels refused -> ESTATE");
        chk(strcmp(why, "摄像机网络不可达：10.0.0.11:554 连接超时（500ms）") == 0,
            "c12: the first channel's reason is handed back verbatim");
        chk(iv_recctl_recording(&r) == 0, "c12: not recording after a total failure");
        chk(iv_recctl_forced(&r) == 0, "c12: force is NOT left on after a total failure (f13)");
        {
            int att = f.n_start_att;

            (void)iv_recctl_step(&r, 1, 731000, &ops);
            (void)iv_recctl_step(&r, 1, 800000, &ops);
            chk(f.n_start_att == att,
                "c12: no silent retry loop behind the user's back (f13)");
        }
        f.start_fail = 0;

        /* ⑦ 存储闸门要停就停，并顺手清掉强制态 */
        fake_reset(&f);
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c12: init for the gate case");
        chk(iv_recctl_manual(&r, 1, 900000, &ops, why, sizeof(why)) == 1,
            "c12: manual start before the gate");
        chk(iv_recctl_forced(&r) == 1, "c12: forced");
        chk(iv_recctl_gate(&r, 0, 1, &ops) == 2, "c12: the storage gate stops it anyway");
        chk(iv_recctl_recording(&r) == 0, "c12: recording cleared by the gate");
        chk(iv_recctl_forced(&r) == 0, "c12: force cleared by the gate");
        chk(iv_recctl_gate(&r, 1, 0, &ops) == 0, "c12: gate reset");

        /* ⑧ 人工停止必须压得住自动开录（f14）：主网断着也不能"10 s 后自己又开始录"，
         * 但**主网恢复一次即解除**（一次点击不该把断网录像这条安全功能永久关掉）。 */
        fake_reset(&f);
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c12: init for the suppress case");
        (void)iv_recctl_step(&r, 0, 1000000, &ops); /* 主网断：起计时 */
        (void)iv_recctl_step(&r, 0, 1010000, &ops); /* 到 hold：自动开录 */
        chk(iv_recctl_recording(&r) == 1, "c12: auto recording before the manual stop");
        chk(iv_recctl_manual(&r, 0, 1011000, &ops, why, sizeof(why)) == 1,
            "c12: manual stop while the main link is still down");
        chk(iv_recctl_held_off(&r) == 1, "c12: auto start is suppressed (f14)");
        {
            int att = f.n_start_att;

            (void)iv_recctl_step(&r, 0, 1020000, &ops);
            (void)iv_recctl_step(&r, 0, 2000000, &ops);
            chk(f.n_start_att == att,
                "c12: the auto path cannot restart it while suppressed (f14)");
            chk(iv_recctl_recording(&r) == 0, "c12: still stopped while suppressed");
        }
        /* 主网恢复一拍 ⇒ 压制解除；再断网仍按 hold 自动录（回到安全功能的常态） */
        (void)iv_recctl_step(&r, 1, 2001000, &ops);
        chk(iv_recctl_held_off(&r) == 0, "c12: a recovery clears the suppression");
        (void)iv_recctl_step(&r, 0, 2002000, &ops);
        (void)iv_recctl_step(&r, 0, 2012000, &ops);
        chk(iv_recctl_recording(&r) == 1,
            "c12: the next outage records again on the auto path");
        chk(iv_recctl_forced(&r) == 0, "c12: and that one is auto, not forced");
        (void)iv_recctl_stop_all(&r, &ops); /* 收尾，别把状态带进后面的用例 */

        /* ⑨ 入参守卫 */
        chk(iv_recctl_manual(NULL, 1, 0, &ops, NULL, 0) == IV_EINVAL,
            "c12: NULL state -> EINVAL");
        chk(iv_recctl_manual(&r, 1, 0, NULL, NULL, 0) == IV_EINVAL,
            "c12: NULL ops -> EINVAL");
    }

    /* ---------------- c13 运行期复检（f15 / f16） ----------------
     * 板端形态：12:16:46 两路开录 → 12:23:09 与 13:04:09 各被对端关掉一次
     * （`peer reports stopped`）→ 之后录像永久停，`record.page` 仍报每路
     * `on:true`。根因就是"启动确认只做一次"，见头文件"在录 ≠ 一直在录"。 */
    {
        char why[IV_RECCTL_WHY_MAX];

        /* ① 正常开录并确认（两路都在录） */
        cfg_two_urls(&cfg); /* hold=10s debounce=60s */
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c13: init ok");
        fake_reset(&f);
        (void)iv_recctl_step(&r, 0, 1000, &ops);    /* 起计时 */
        (void)iv_recctl_step(&r, 0, 11000, &ops);   /* 到 hold：2 路开录 */
        (void)iv_recctl_step(&r, 0, 14000, &ops);   /* 3 s 后确认：都真在录 */
        chk(f.n_start == 2, "c13: two channels started");
        chk(iv_recctl_recording(&r) == 1, "c13: recording after confirm");

        /* ② 两路都断流。复检节流：距上次（14000 那拍）不到 10 s 不许再查 */
        f.status_val = 0;
        {
            int calls = f.status_calls;

            (void)iv_recctl_step(&r, 0, 20000, &ops);
            chk(f.status_calls == calls, "c13: recheck is throttled (f16)");
            chk(iv_recctl_recording(&r) == 1, "c13: still believes it records (f16)");

            /* 到点：复检发现两路都不在录 ⇒ recording 归 0、退避一轮 */
            (void)iv_recctl_step(&r, 0, 24000, &ops);
            chk(iv_recctl_recording(&r) == 0,
                "c13: all channels dropped -> recording cleared (f15)");
            chk(f.n_start == 2, "c13: no immediate restart, backoff first");
            chk(iv_recctl_fail_reason(&r, 0, why, sizeof(why)) == 1 && why[0] != '\0',
                "c13: the dropped channel explains itself");
            /* 退避窗口内不许重开（摄像机整机重启期间每拍打预检没人受得了） */
            (void)iv_recctl_step(&r, 0, 25000, &ops);
            chk(f.n_start == 2, "c13: no retry inside the backoff window");
        }

        /* ③ 摄像机回来了：退避到期即开录，且**不必重走 hold 保持期**
         *    （"主网断"这件事从没中断过 —— 谁要是让 down_since 归零，
         *     这里会再等 10 s，n_start 就停在 2） */
        f.status_val = 1;
        (void)iv_recctl_step(&r, 0, 34000, &ops);
        chk(f.n_start == 4, "c13: restart once the backoff expires (f15)");
        chk(iv_recctl_recording(&r) == 1, "c13: recording again");
        (void)iv_recctl_step(&r, 0, 37000, &ops); /* 确认这一批 */
        chk(iv_recctl_recording(&r) == 1, "c13: confirmed again");

        /* ④ 部分掉线：只有第 2 路断流 ⇒ 立刻重开整批，recording 保持 1。
         *    注意"原因"的生命周期：重开被受理后原因先被清掉（不留陈旧结论），
         *    3 s 后的启动确认发现它还是没在录，才写下真正的那句原因 —— 这条
         *    链路正是页面 `failed`/`why` 的来源。 */
        f.status_ch_set = 1;
        f.status_ch[0]   = 1; /* ch0 还在录 */
        f.status_ch[1]   = 0; /* ch1 断了 */
        (void)iv_recctl_step(&r, 0, 48000, &ops);
        chk(f.n_start == 6, "c13: a partial drop restarts the batch (f15)");
        chk(iv_recctl_recording(&r) == 1, "c13: still recording (one channel alive)");
        chk(iv_recctl_fail_reason(&r, 0, why, sizeof(why)) == 0,
            "c13: the live channel has no stale reason");
        (void)iv_recctl_step(&r, 0, 51000, &ops); /* 3 s 后确认：ch1 还是没起来 */
        chk(iv_recctl_fail_reason(&r, 1, why, sizeof(why)) == 1,
            "c13: the dropped channel carries a reason after confirm");
        chk(strstr(why, "未进入录像") != NULL,
            "c13: reason explains it never got into recording");
        chk(iv_recctl_recording(&r) == 1, "c13: a partial drop never stops the batch");

        /* ⑤ 启动确认未完成时（pending）不参与复检：此刻 ffmpeg 可能正在建流，
         *    查了必然"未在录"，会把正常启动误判成掉线 */
        fake_reset(&f);
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c13: init for the pending case");
        (void)iv_recctl_step(&r, 0, 1000, &ops);
        (void)iv_recctl_step(&r, 0, 11000, &ops); /* 刚发起，pending=1 */
        {
            int calls = f.status_calls;

            (void)iv_recctl_step(&r, 0, 12000, &ops);
            chk(f.status_calls == calls,
                "c13: no recheck while the start confirmation is pending");
            chk(iv_recctl_recording(&r) == 1,
                "c13: a fresh start is not mistaken for a drop");
        }

        /* ⑥ 没有 status 回调时复检静默跳过（保持"受理即成功"的降级口径） */
        fake_reset(&f);
        ops.status = NULL;
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c13: init without a status hook");
        (void)iv_recctl_step(&r, 0, 1000, &ops);
        (void)iv_recctl_step(&r, 0, 11000, &ops);
        (void)iv_recctl_step(&r, 0, 30000, &ops);
        (void)iv_recctl_step(&r, 0, 90000, &ops);
        chk(iv_recctl_recording(&r) == 1, "c13: no hook -> nothing to recheck");
        chk(f.n_stop == 0, "c13: and no spurious stop");
        ops.status = fake_status; /* 还给后面的用例 */
        (void)iv_recctl_stop_all(&r, &ops);
    }

    if (g_fail != 0) {
        fprintf(stderr, "test_recctl FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_recctl passed (main-link criterion, hold/debounce, backoff retry, "
           "start confirm, running recheck, failure reason, manual stop, "
           "storage gate, config swap)\n");
    return 0;
}
