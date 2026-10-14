/*
 * iv_recctl 单测（libivmodules，功能开发计划 M6-S6.3）
 *
 * 状态机是纯逻辑：时间轴由用例注入（now_ms 自拟），动作经假 ops 记录到数组。
 * 覆盖六组：
 *   c01 无 URL 表 ⇒ 状态机旁路（§18.5 未冻结时的生产形态）
 *   c02 双断保持 hold 才开录；恢复未达 debounce 不停录
 *   c03 恢复达 debounce 停录；停后可再次触发（down_since 重新计时）
 *   c04 期间任一恢复（wan 或 plat 单独活）即打断"全断"计时
 *   c05 start 全被拒 ⇒ 不置位 recording、下拍重试（down_since 连续）
 *   c06 stop_all 手动收尾 + recording 读数
 *   c07 S6.5 容量闸门：关闸抑制开录、开闸即录、水位告急立即停录、step 兜底路径
 *
 * 反向证伪（本用例内建）：
 *   f1 若把"保持"判据去掉（到达 hold 前就开录），c02 的保持段会提前动作；
 *   f2 若把"恢复防抖"去掉（恢复沿立即停录），c02 的恢复段会提前动作；
 *   f3 若把"恢复＝任一条件活"误写成"平台活才算恢复"，c04 的 wan 单独恢复段会
 *      继续计时并把录制停掉。
 *   f4 若去掉"闸门关时不核准开录"（step 里删掉 gate_block 判断），c07 会在关闸
 *      状态下开录；
 *   f5 若去掉"闸门要求停录即立即停"，c07 会一直录到恢复防抖（或根本不录）。
 * 三者都由对应的 chk 钉死：动作次数是精确断言，多一次/少一次都红。
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
#define MAX_ACT 16
typedef struct {
    int  n_start;   /* 受理数 */
    int  n_start_att; /* 尝试数（含被拒） */
    int  n_stop;
    int  start_id[MAX_ACT];
    char start_url[MAX_ACT][IV_RECCTL_URL_MAX];
    int  stop_id[MAX_ACT];
    int  start_fail; /* 置 1 时 start 一律拒绝 */
} fake_ops_t;

static int fake_start(int id, const char *url, void *arg)
{
    fake_ops_t *f = arg;

    f->n_start_att++;
    if (f->start_fail)
        return IV_ECONN;
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

static void ops_of(iv_recctl_ops_t *ops, fake_ops_t *f)
{
    ops->start = fake_start;
    ops->stop  = fake_stop;
    ops->arg   = f;
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
    memset(&f, 0, sizeof(f));
    ops_of(&ops, &f);
    chk(iv_recctl_step(&r, 0, 0, 1000000, &ops) == 0,
        "c01: no urls -> step is a no-op even after ages of outage");
    chk(f.n_start == 0 && f.n_stop == 0, "c01: no actions fired");
    chk(iv_recctl_recording(&r) == 0, "c01: not recording");

    /* ---------------- c02 保持开录 / 防抖停录 ---------------- */
    cfg_two_urls(&cfg);
    chk(iv_recctl_init(&r, &cfg) == IV_OK, "c02: init ok");
    memset(&f, 0, sizeof(f));

    /* 双断第 0 拍：只起计时 */
    chk(iv_recctl_step(&r, 0, 0, 10000, &ops) == 0, "c02: down edge times only");
    chk(f.n_start == 0, "c02: no immediate start on edge");
    /* hold=10s：差 1ms 也不许动作（f1） */
    chk(iv_recctl_step(&r, 0, 0, 10000 + 9999, &ops) == 0, "c02: below hold");
    chk(f.n_start == 0, "c02: no start below hold");
    /* 到 hold：逐路开录 */
    chk(iv_recctl_step(&r, 0, 0, 10000 + 10000, &ops) == 1, "c02: start on hold");
    chk(f.n_start == 2, "c02: both channels started");
    chk(f.start_id[0] == 0 && f.start_id[1] == 1, "c02: ids in table order");
    chk(strcmp(f.start_url[0], "rtsp://10.0.0.11/ch0") == 0, "c02: url passed through");
    chk(iv_recctl_recording(&r) == 1, "c02: recording flag set");
    /* 仍断着：无动作 */
    chk(iv_recctl_step(&r, 0, 0, 30000, &ops) == 0, "c02: steady outage");
    chk(f.n_stop == 0, "c02: no stop while outage persists");
    /* 恢复沿：只起防抖 */
    chk(iv_recctl_step(&r, 1, 0, 31000, &ops) == 0, "c02: recovery edge debounces");
    chk(f.n_stop == 0, "c02: no immediate stop on recovery (f2)");
    /* debounce=60s：差一点不停 */
    chk(iv_recctl_step(&r, 1, 1, 31000 + 59999, &ops) == 0, "c02: below debounce");
    chk(f.n_stop == 0, "c02: no stop below debounce");
    /* 到 debounce：停录 */
    chk(iv_recctl_step(&r, 1, 1, 31000 + 60000, &ops) == 1, "c02: stop on debounce");
    chk(f.n_stop == 2, "c02: both channels stopped");
    chk(iv_recctl_recording(&r) == 0, "c02: recording cleared");

    /* ---------------- c03 停后再触发 ---------------- */
    memset(&f, 0, sizeof(f));
    chk(iv_recctl_step(&r, 0, 0, 200000, &ops) == 0, "c03: down edge re-armed");
    chk(f.n_start == 0, "c03: hold re-armed from zero");
    chk(iv_recctl_step(&r, 0, 0, 209999, &ops) == 0, "c03: below hold again");
    chk(iv_recctl_step(&r, 0, 0, 210000, &ops) == 1, "c03: start again on hold");
    chk(f.n_start == 2, "c03: restarted both channels");
    chk(iv_recctl_recording(&r) == 1, "c03: recording again");

    /* ---------------- c04 恢复＝任一条件活（f3） ---------------- */
    memset(&f, 0, sizeof(f));
    /* 录制中，wan 单独恢复：防抖开始 */
    chk(iv_recctl_step(&r, 1, 0, 300000, &ops) == 0, "c04: wan-only recovery debounces");
    chk(f.n_stop == 0, "c04: wan-only recovery must count (f3)");
    /* 防抖期里又全断：恢复计时清零，回到"仍断着" */
    chk(iv_recctl_step(&r, 0, 0, 330000, &ops) == 0, "c04: outage again resets debounce");
    chk(f.n_stop == 0, "c04: no stop after outage resumes");
    /* 再恢复并撑满防抖：停 */
    chk(iv_recctl_step(&r, 0, 1, 340000, &ops) == 0, "c04: plat-only recovery debounces");
    chk(iv_recctl_step(&r, 1, 1, 340000 + 60000, &ops) == 1, "c04: stop after full debounce");
    chk(f.n_stop == 2, "c04: stopped");

    /* ---------------- c05 start 全被拒：不置位、下拍重试 ---------------- */
    {
        int saw_retry = 0;
        int t;

        memset(&f, 0, sizeof(f));
        f.start_fail = 1;
        chk(iv_recctl_init(&r, &cfg) == IV_OK, "c05: re-init ok");
        (void)iv_recctl_step(&r, 0, 0, 1000, &ops);        /* 起计时 */
        (void)iv_recctl_step(&r, 0, 0, 11000, &ops);       /* 到 hold：全拒 */
        chk(f.n_start_att == 2, "c05: start attempted for both channels");
        chk(iv_recctl_recording(&r) == 0, "c05: recording NOT set on failure");
        /* 下一拍（仍断）：立刻重试，不重新等 hold */
        for (t = 0; t < 3; t++) {
            (void)iv_recctl_step(&r, 0, 0, 11001 + (uint64_t)t * 1000, &ops);
            saw_retry += f.n_start_att;
        }
        chk(saw_retry >= 3, "c05: retries every tick while down");
        /* 恢复后停手 */
        f.start_fail = 0;
        (void)iv_recctl_step(&r, 1, 1, 20000, &ops);
        chk(iv_recctl_recording(&r) == 0, "c05: clean state after recovery");
    }

    /* ---------------- c06 stop_all 手动收尾 ---------------- */
    memset(&f, 0, sizeof(f));
    chk(iv_recctl_init(&r, &cfg) == IV_OK, "c06: init ok");
    (void)iv_recctl_step(&r, 0, 0, 1000, &ops);
    (void)iv_recctl_step(&r, 0, 0, 11000, &ops);
    chk(iv_recctl_recording(&r) == 1, "c06: recording before manual stop");
    chk(iv_recctl_stop_all(&r, &ops) == 1, "c06: stop_all reports a transition");
    chk(f.n_stop == 2, "c06: both channels stopped");
    chk(iv_recctl_recording(&r) == 0, "c06: recording cleared");
    chk(iv_recctl_stop_all(&r, &ops) == 0, "c06: stop_all idempotent when idle");
    chk(f.n_stop == 2, "c06: no extra stop when idle");

    /* ---------------- c07 S6.5 容量闸门 ---------------- */
    memset(&f, 0, sizeof(f));
    chk(iv_recctl_init(&r, &cfg) == IV_OK, "c07: init ok");

    /* 关闸（allow_new=0）：双断并保持 hold 之后**仍不**开录 */
    chk(iv_recctl_gate(&r, 0, 0, &ops) == 0, "c07: gate(block) reports nothing to stop");
    chk(iv_recctl_step(&r, 0, 0, 1000, &ops) == 0, "c07: down edge armed");
    chk(iv_recctl_step(&r, 0, 0, 11000, &ops) == 0, "c07: blocked start suppressed (f4)");
    chk(f.n_start == 0, "c07: no start while the storage gate blocks (f4)");
    chk(iv_recctl_recording(&r) == 0, "c07: not recording while blocked");

    /* 开闸：同一断网沿（down_since 连续）下一拍即开录，不必重新等 hold */
    chk(iv_recctl_gate(&r, 1, 0, &ops) == 0, "c07: gate(open)");
    chk(iv_recctl_step(&r, 0, 0, 12000, &ops) == 1, "c07: start once the gate opens");
    chk(f.n_start == 2, "c07: both channels started after unblock");
    chk(iv_recctl_recording(&r) == 1, "c07: recording after unblock");

    /* 录制中水位告急（stop_rec=1）：立即停录，不等 debounce */
    memset(&f, 0, sizeof(f));
    chk(iv_recctl_gate(&r, 0, 1, &ops) == 2, "c07: gate(stop) stops both channels (f5)");
    chk(f.n_stop == 2, "c07: immediate stop when storage is full (f5)");
    chk(iv_recctl_recording(&r) == 0, "c07: recording cleared by the gate");

    /* 闸门归位后不残留：需重新满足开录条件（hold 重新计时） */
    memset(&f, 0, sizeof(f));
    chk(iv_recctl_gate(&r, 1, 0, &ops) == 0, "c07: gate reset");
    chk(iv_recctl_step(&r, 0, 0, 20000, &ops) == 0, "c07: hold re-armed after gate stop");
    chk(f.n_start == 0, "c07: no instant restart after a gate stop");
    chk(iv_recctl_step(&r, 0, 0, 30000, &ops) == 1, "c07: restart requires a fresh hold");
    chk(f.n_start == 2, "c07: restarted after hold");

    /* step() 兜底：绕过 gate 直接置位也要停录 */
    memset(&f, 0, sizeof(f));
    r.gate_stop = 1;
    chk(iv_recctl_step(&r, 0, 0, 31000, &ops) == 1, "c07: step honours gate_stop");
    chk(f.n_stop == 2, "c07: stopped via the step safe path");
    chk(iv_recctl_recording(&r) == 0, "c07: recording cleared by the safe path");
    r.gate_stop = 0;
    chk(iv_recctl_step(&r, 0, 0, 40000, &ops) == 0, "c07: clean state after safe-path stop");

    if (g_fail != 0) {
        fprintf(stderr, "test_recctl FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_recctl passed (hold/debounce edges, retry-on-reject, manual stop, "
           "storage gate)\n");
    return 0;
}
