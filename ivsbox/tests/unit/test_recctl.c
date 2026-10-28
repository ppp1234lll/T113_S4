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

    (void)id;
    f->status_calls++;
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

    if (g_fail != 0) {
        fprintf(stderr, "test_recctl FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_recctl passed (main-link criterion, hold/debounce, backoff retry, "
           "start confirm, failure reason, manual stop, storage gate, config swap)\n");
    return 0;
}
