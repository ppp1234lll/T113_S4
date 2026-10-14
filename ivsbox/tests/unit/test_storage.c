/*
 * iv_storage 单测（libivmodules，功能开发计划 M6-S6.5）
 *
 * 覆盖四类：
 *   1) **纯决策**（`iv_store_decide`）：水位阶梯 85/90/95/70 与**迟滞**、故障路径、
 *      删除优先级（REC→ALARM→SNAP）、同类最旧优先、最近切片保护、单轮条数上限。
 *   2) **真目录 IO**（`iv_storage_ops_default`）：造一棵 `snap/` ＋ `video/<日期>/` 的
 *      真树，跑一轮，断言**真的删对了那几条**（含 `alarm_` 前缀归 ALARM 类）。
 *   3) **窗口有界**：候选远超 `IV_STORAGE_FILES_MAX` 时，留下的必须是"最该删的 64 条"
 *      （类别高者优先），不能把内存吃爆也不能挑错人。
 *   4) 周期节流（`iv_storage_step`）与根目录解析（`IVSBOX_SD_ROOT`）。
 *
 * 反向证伪（本用例内建）：
 *   f1 把水位判据从 `>=` 改成 `>` ⇒ c02 的 85/90/95 三个边界点全部不升档；
 *   f2 去掉迟滞（回落到 clean 以下就降档）⇒ c02 的"降到 80% 仍 STOP"段会提前回 OK；
 *   f3 把删除排序的类别比较反过来（SNAP 先删）⇒ c04/c07 的删除顺序断言反了；
 *   f4 去掉"最近切片保护" ⇒ c05 会把刚写的那条删掉。
 * 每条都有精确断言钉着（顺序/条数/文件是否还在），错一个都红。
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_storage.h"

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

/* ---------------- 小工具 ---------------- */
/* 有界拷贝：**别用 `snprintf(dst,"%s",src)` 往定长成员里拷不确定长度的源** ——
 * GCC 能算出最坏长度（源是 char[N] 时）并断在 -Wformat-truncation/-Werror 上。 */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if (n >= cap)
        n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void j2(char *dst, size_t cap, const char *a, const char *b)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);

    if (na + 1u + nb >= cap) {
        dst[0] = '\0';
        return;
    }
    memcpy(dst, a, na);
    dst[na] = '/';
    memcpy(dst + na + 1u, b, nb);
    dst[na + 1u + nb] = '\0';
}

static void mkdir_one(const char *p)
{
    (void)mkdir(p, 0700);
}

/* 造一个 size 字节、mtime = now-age 的文件 */
static void mkfile(const char *path, size_t size, long age_s)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL)
        return;
    if (size > 0u) {
        char buf[64];

        memset(buf, 'x', sizeof(buf));
        while (size > 0u) {
            size_t n = (size > sizeof(buf)) ? sizeof(buf) : size;

            if (fwrite(buf, 1u, n, f) != n)
                break;
            size -= n;
        }
    }
    (void)fclose(f);
    if (age_s > 0) {
        struct utimbuf ut;

        ut.actime  = (time_t)(time(NULL) - age_s);
        ut.modtime = (time_t)(time(NULL) - age_s);
        (void)utime(path, &ut);
    }
}

static void rm_rf(const char *path)
{
    DIR *d = opendir(path);

    if (d != NULL) {
        struct dirent *e;

        while ((e = readdir(d)) != NULL) {
            char p[512];

            if (e->d_name[0] == '.' &&
                (e->d_name[1] == '\0' ||
                 (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                continue;
            j2(p, sizeof(p), path, e->d_name);
            if (p[0] != '\0')
                rm_rf(p);
        }
        (void)closedir(d);
    }
    if (unlink(path) != 0)
        (void)rmdir(path);
}

static int exists(const char *path)
{
    struct stat sb;

    return (stat(path, &sb) == 0) ? 1 : 0;
}

/* ---------------- probe 构造 ---------------- */
static void probe_reset(iv_store_probe_t *p, uint64_t total, uint32_t used_pct)
{
    memset(p, 0, sizeof(*p));
    p->mounted     = 1u;
    p->total_bytes = total;
    p->free_bytes  = total - (total / 100u) * used_pct - ((total % 100u) * used_pct) / 100u;
}

static void probe_add(iv_store_probe_t *p, const char *name, uint8_t cls, uint64_t size,
                      uint64_t mtime_s)
{
    iv_store_file_t *f;

    if (p->n_files >= (int)IV_STORAGE_FILES_MAX)
        return;
    f = &p->files[p->n_files];
    memset(f, 0, sizeof(*f));
    (void)snprintf(f->path, sizeof(f->path), "/x/%s", name);
    (void)snprintf(f->name, sizeof(f->name), "%s", name);
    f->size    = size;
    f->mtime_s = mtime_s;
    f->cls     = cls;
    p->n_files++;
}

/* ---------------- c01/c02 水位阶梯与迟滞 ---------------- */
static void case_levels(void)
{
    iv_storage_cfg_t cfg;
    iv_store_probe_t pr;
    iv_store_plan_t  pl;
    uint8_t          lv = (uint8_t)IV_STORE_OK;

    iv_storage_cfg_default(&cfg);
    chk(cfg.clean_pct == 85u && cfg.reject_pct == 90u && cfg.stop_pct == 95u &&
        cfg.recover_pct == 70u, "c01: watermark defaults 85/90/95/70");
    chk(cfg.n_dir == 2 && cfg.dir[0].cls == (uint8_t)IV_STORE_CLS_SNAP &&
        cfg.dir[1].cls == (uint8_t)IV_STORE_CLS_REC, "c01: default dir table snap/video");

    /* 84% → OK */
    probe_reset(&pr, 1000000u, 84u);
    chk(iv_store_decide(&cfg, &pr, 0u, lv, &pl) == IV_OK, "c02: decide ok");
    chk(pl.level == (uint8_t)IV_STORE_OK, "c02: 84% stays OK");
    chk(pl.allow_new == 1u && pl.stop_rec == 0u && pl.alarm == 0u, "c02: 84% allows new");
    chk(pl.n_delete == 0, "c02: no cleanup below clean line");
    lv = pl.level;

    /* 85% → CLEAN（升档立即；仍允许开新录，只是开始清理） */
    probe_reset(&pr, 1000000u, 85u);
    (void)iv_store_decide(&cfg, &pr, 0u, lv, &pl);
    chk(pl.level == (uint8_t)IV_STORE_CLEAN, "c02: 85% -> CLEAN");
    chk(pl.allow_new == 1u && pl.stop_rec == 0u, "c02: CLEAN still allows new");
    lv = pl.level;

    /* 89% → 仍 CLEAN（目标档不比当前高 ⇒ 不升） */
    probe_reset(&pr, 1000000u, 89u);
    (void)iv_store_decide(&cfg, &pr, 0u, lv, &pl);
    chk(pl.level == (uint8_t)IV_STORE_CLEAN, "c02: 89% stays CLEAN");
    lv = pl.level;

    /* 90% → REJECT（拒新任务） */
    probe_reset(&pr, 1000000u, 90u);
    (void)iv_store_decide(&cfg, &pr, 0u, lv, &pl);
    chk(pl.level == (uint8_t)IV_STORE_REJECT, "c02: 90% -> REJECT");
    chk(pl.allow_new == 0u && pl.stop_rec == 0u, "c02: REJECT gates new tasks only");
    lv = pl.level;

    /* 94% → 仍 REJECT */
    probe_reset(&pr, 1000000u, 94u);
    (void)iv_store_decide(&cfg, &pr, 0u, lv, &pl);
    chk(pl.level == (uint8_t)IV_STORE_REJECT, "c02: 94% stays REJECT");
    lv = pl.level;

    /* 95% → STOP（停录 ＋ 告警） */
    probe_reset(&pr, 1000000u, 95u);
    (void)iv_store_decide(&cfg, &pr, 0u, lv, &pl);
    chk(pl.level == (uint8_t)IV_STORE_STOP, "c02: 95% -> STOP");
    chk(pl.stop_rec == 1u && pl.alarm == 1u && pl.allow_new == 0u, "c02: STOP stops+alarms");
    lv = pl.level;

    /* 降到 80% → **仍 STOP**（迟滞：只有回到 70% 才解除） */
    probe_reset(&pr, 1000000u, 80u);
    (void)iv_store_decide(&cfg, &pr, 0u, lv, &pl);
    chk(pl.level == (uint8_t)IV_STORE_STOP, "c02: hysteresis keeps STOP down to recover line");
    chk(pl.recovered == 0u && pl.stop_rec == 1u, "c02: still stopped at 80%");
    lv = pl.level;

    /* 71% → 仍 STOP */
    probe_reset(&pr, 1000000u, 71u);
    (void)iv_store_decide(&cfg, &pr, 0u, lv, &pl);
    chk(pl.level == (uint8_t)IV_STORE_STOP, "c02: 71% still STOP");
    lv = pl.level;

    /* 70% → 回 OK，并置 recovered */
    probe_reset(&pr, 1000000u, 70u);
    (void)iv_store_decide(&cfg, &pr, 0u, lv, &pl);
    chk(pl.level == (uint8_t)IV_STORE_OK, "c02: 70% recovers to OK");
    chk(pl.recovered == 1u, "c02: recovered flag raised");
    chk(pl.allow_new == 1u && pl.stop_rec == 0u && pl.alarm == 0u, "c02: full release at 70%");

    /* 从 OK 直冲 100% → STOP（升档无需中间档） */
    probe_reset(&pr, 1000000u, 100u);
    (void)iv_store_decide(&cfg, &pr, 0u, (uint8_t)IV_STORE_OK, &pl);
    chk(pl.level == (uint8_t)IV_STORE_STOP, "c02: 100% jumps straight to STOP");
}

/* ---------------- c03 故障路径 ---------------- */
static void case_fault(void)
{
    iv_storage_cfg_t cfg;
    iv_store_probe_t pr;
    iv_store_plan_t  pl;

    iv_storage_cfg_default(&cfg);

    /* 未挂载 */
    memset(&pr, 0, sizeof(pr));
    (void)iv_store_decide(&cfg, &pr, 0u, (uint8_t)IV_STORE_OK, &pl);
    chk(pl.fault == 1u, "c03: unmounted -> fault");
    chk(pl.allow_new == 0u && pl.stop_rec == 1u && pl.alarm == 1u, "c03: fault gates + stops");
    chk(pl.n_delete == 0, "c03: no cleanup when nothing is mounted");

    /* 只读挂载（水位很低也必须停录） */
    probe_reset(&pr, 1000000u, 10u);
    pr.readonly = 1u;
    probe_add(&pr, "a.mp4", (uint8_t)IV_STORE_CLS_REC, 1000u, 100u);
    (void)iv_store_decide(&cfg, &pr, 0u, (uint8_t)IV_STORE_OK, &pl);
    chk(pl.fault == 1u && pl.stop_rec == 1u, "c03: read-only -> fault even at 10%");

    /* 扫描出错 */
    probe_reset(&pr, 1000000u, 10u);
    pr.io_err = 1u;
    (void)iv_store_decide(&cfg, &pr, 0u, (uint8_t)IV_STORE_OK, &pl);
    chk(pl.fault == 1u && pl.allow_new == 0u, "c03: scan error -> fault");
}

/* ---------------- c04 删除优先级 ---------------- */
static const char *nth_del(const iv_store_plan_t *pl, int i)
{
    return (i >= 0 && i < pl->n_delete) ? pl->del[i]->name : "(none)";
}

static void case_priority(void)
{
    iv_storage_cfg_t cfg;
    iv_store_probe_t pr;
    iv_store_plan_t  pl;

    iv_storage_cfg_default(&cfg);
    cfg.max_delete = 16u;

    /* 95% 用了，recover 70% ⇒ need = 25% ⇒ 按 1000 B/条需要 250 条，实际只给 6 条 */
    probe_reset(&pr, 1000000u, 95u);
    probe_add(&pr, "rec_a.mp4", (uint8_t)IV_STORE_CLS_REC, 1000u, 300u);
    probe_add(&pr, "rec_b.mp4", (uint8_t)IV_STORE_CLS_REC, 1000u, 100u);
    probe_add(&pr, "rec_c.mp4", (uint8_t)IV_STORE_CLS_REC, 1000u, 200u);
    probe_add(&pr, "alarm_1.mp4", (uint8_t)IV_STORE_CLS_ALARM, 1000u, 50u);
    probe_add(&pr, "snap_x.jpg", (uint8_t)IV_STORE_CLS_SNAP, 1000u, 10u);
    probe_add(&pr, "snap_y.jpg", (uint8_t)IV_STORE_CLS_SNAP, 1000u, 20u);

    chk(iv_store_decide(&cfg, &pr, 5000u, (uint8_t)IV_STORE_OK, &pl) == IV_OK, "c04: decide ok");
    chk(pl.n_delete == 6, "c04: all six candidates picked (need > 6 kB)");
    chk(strcmp(nth_del(&pl, 0), "rec_b.mp4") == 0, "c04: oldest REC first");
    chk(strcmp(nth_del(&pl, 1), "rec_c.mp4") == 0, "c04: then next-oldest REC");
    chk(strcmp(nth_del(&pl, 2), "rec_a.mp4") == 0, "c04: newest REC last within class");
    chk(strcmp(nth_del(&pl, 3), "alarm_1.mp4") == 0, "c04: ALARM after all REC");
    chk(strcmp(nth_del(&pl, 4), "snap_x.jpg") == 0, "c04: SNAP only after REC+ALARM");
    chk(strcmp(nth_del(&pl, 5), "snap_y.jpg") == 0, "c04: snapshots last, oldest first");
}

/* ---------------- c05 最近切片保护 ---------------- */
static void case_youngest(void)
{
    iv_storage_cfg_t cfg;
    iv_store_probe_t pr;
    iv_store_plan_t  pl;

    iv_storage_cfg_default(&cfg);
    cfg.max_delete     = 16u;
    cfg.keep_youngest_s = 120u;

    probe_reset(&pr, 1000000u, 95u);
    probe_add(&pr, "rec_new.mp4", (uint8_t)IV_STORE_CLS_REC, 1000u, 9000u); /* 刚写 */
    probe_add(&pr, "rec_old.mp4", (uint8_t)IV_STORE_CLS_REC, 1000u, 100u);

    /* now_s = 9001 ⇒ rec_new 在保护窗内（9000+120 > 9001），必须跳过 */
    (void)iv_store_decide(&cfg, &pr, 9001u, (uint8_t)IV_STORE_OK, &pl);
    chk(pl.n_delete == 1, "c05: only the old slice is deletable");
    chk(strcmp(nth_del(&pl, 0), "rec_old.mp4") == 0, "c05: the slice being written is spared");

    /* now_s = 0 ⇒ 关闭保护（测试/离线用），两条都进候选 */
    (void)iv_store_decide(&cfg, &pr, 0u, (uint8_t)IV_STORE_OK, &pl);
    chk(pl.n_delete == 2, "c05: protection off when now_s==0");

    /* mtime 未知（0）视作可删，不受保护影响 */
    probe_reset(&pr, 1000000u, 95u);
    probe_add(&pr, "rec_nomtime.mp4", (uint8_t)IV_STORE_CLS_REC, 1000u, 0u);
    (void)iv_store_decide(&cfg, &pr, 9001u, (uint8_t)IV_STORE_OK, &pl);
    chk(pl.n_delete == 1 && strcmp(nth_del(&pl, 0), "rec_nomtime.mp4") == 0,
        "c05: unknown mtime is deletable");
}

/* ---------------- c06 单轮上限 ---------------- */
static void case_max_delete(void)
{
    iv_storage_cfg_t cfg;
    iv_store_probe_t pr;
    iv_store_plan_t  pl;
    int              i;

    iv_storage_cfg_default(&cfg);
    cfg.max_delete = 3u;

    probe_reset(&pr, 1000000u, 99u);
    for (i = 0; i < 20; i++)
        probe_add(&pr, "rec.mp4", (uint8_t)IV_STORE_CLS_REC, 100u, (uint64_t)(100 + i));

    (void)iv_store_decide(&cfg, &pr, 5000u, (uint8_t)IV_STORE_OK, &pl);
    chk(pl.n_delete == 3, "c06: per-round delete cap honoured");

    /* 上限被夹到 IV_STORAGE_DEL_MAX */
    cfg.max_delete = 999u;
    (void)iv_store_decide(&cfg, &pr, 5000u, (uint8_t)IV_STORE_OK, &pl);
    chk(pl.n_delete == 20, "c06: cap clamped to DEL_MAX, then limited by candidates");
}

/* ---------------- c07 真目录：走真 scan/unlink，看真删了谁 ---------------- */
static int  g_scan_files;
static uint64_t g_scan_total;
static uint64_t g_scan_free;

/* 包一层：文件列表来自真目录，容量事实由用例指定（/tmp 的水位没法摆布） */
static int test_scan(void *arg, const iv_storage_cfg_t *cfg, iv_store_probe_t *out)
{
    iv_storage_ops_t def;
    int              rc;

    (void)arg;
    iv_storage_ops_default(&def);
    rc = def.scan(NULL, cfg, out);
    out->total_bytes = g_scan_total;
    out->free_bytes  = g_scan_free;
    g_scan_files     = out->n_files;
    return rc;
}

static void case_real_dir(void)
{
    char             tmpl[] = "/tmp/ivstore_XXXXXX";
    char             snap[256];
    char             vid[256];
    char             day[256];
    char             p[512];
    iv_storage_cfg_t cfg;
    iv_storage_ops_t ops;
    iv_storage_t     st;
    iv_storage_stats_t stats;
    uint64_t         now_ms;

    if (mkdtemp(tmpl) == NULL) {
        chk(0, "c07: mkdtemp failed");
        return;
    }
    j2(snap, sizeof(snap), tmpl, IV_STORAGE_SNAP_SUBDIR);
    j2(vid, sizeof(vid), tmpl, IV_STORAGE_REC_SUBDIR);
    j2(day, sizeof(day), vid, "20261014");
    mkdir_one(snap);
    mkdir_one(vid);
    mkdir_one(day);

    j2(p, sizeof(p), snap, "snap_0001.jpg");
    mkfile(p, 40000u, 3600);
    j2(p, sizeof(p), day, "channel0_010101.mp4");
    mkfile(p, 300000u, 7200);
    j2(p, sizeof(p), day, "channel1_010102.mp4");
    mkfile(p, 300000u, 5400);
    j2(p, sizeof(p), day, "alarm_0_010103.mp4");
    mkfile(p, 200000u, 9000);
    j2(p, sizeof(p), day, "channel0_140000.mp4");
    mkfile(p, 300000u, 0); /* 刚写：必须被"最近切片保护"放过 */

    iv_storage_cfg_default(&cfg);
    copy_str(cfg.root, sizeof(cfg.root), tmpl);
    cfg.max_delete = 3u;   /* 只删 3 条 ⇒ 正好排空 REC(2) + ALARM(1)，留下 SNAP 与在写的 */

    g_scan_total = 1000000u;
    g_scan_free  = 50000u; /* 95% ⇒ STOP：既要删，也要停录 */
    ops.scan        = test_scan;
    ops.unlink_file = NULL;
    ops.arg         = NULL;
    {
        iv_storage_ops_t def;

        iv_storage_ops_default(&def);
        ops.unlink_file = def.unlink_file;
    }

    chk(iv_storage_init(&st, &cfg, &ops) == IV_OK, "c07: init ok");
    /* now_ms 用**真实墙钟**：目录里的 mtime 就是真实 epoch 秒，"最近切片保护"
     * 只有在 now 与 mtime 同一时间轴上才有意义。 */
    now_ms = (uint64_t)time(NULL) * 1000u;
    chk(iv_storage_run_once(&st, now_ms) == IV_OK, "c07: run_once ok");
    chk(g_scan_files == 5, "c07: real walk found all five regular files");
    chk(iv_storage_stop_rec(&st) == 1, "c07: 95% -> stop recording");
    chk(iv_storage_allow_new(&st) == 0, "c07: 95% -> no new tasks");

    iv_storage_stats(&st, &stats);
    chk(stats.rounds == 1u, "c07: one round accounted");
    chk(stats.deletes == 3u, "c07: three files really unlinked");
    chk(stats.last_deleted_bytes == 800000u, "c07: 300k+300k+200k bytes freed");

    j2(p, sizeof(p), day, "channel1_010102.mp4");
    chk(exists(p) == 0, "c07: both ordinary recordings removed");
    j2(p, sizeof(p), day, "channel0_010101.mp4");
    chk(exists(p) == 0, "c07: second ordinary recording removed");
    j2(p, sizeof(p), day, "alarm_0_010103.mp4");
    chk(exists(p) == 0, "c07: ALARM deleted after REC (alarm_ prefix classified)");
    j2(p, sizeof(p), day, "channel0_140000.mp4");
    chk(exists(p) == 1, "c07: freshly written slice spared");
    j2(p, sizeof(p), snap, "snap_0001.jpg");
    chk(exists(p) == 1, "c07: snapshot kept (lowest deletion priority, cap reached)");

    /* ---- c08 窗口有界：候选远超 FILES_MAX 时，窗口里只留"最该删的" ---- */
    {
        int i;

        for (i = 0; i < 100; i++) {
            char name[64];

            (void)snprintf(name, sizeof(name), "bulk_%03d.mp4", i);
            j2(p, sizeof(p), vid, name);
            mkfile(p, 1024u, 100000); /* 全是很旧的普通录像 */
        }
        g_scan_files = 0;
        chk(iv_storage_run_once(&st, now_ms + (uint64_t)IV_STORAGE_INTERVAL_MS) == IV_OK,
            "c08: rerun ok");
        chk(g_scan_files == (int)IV_STORAGE_FILES_MAX, "c08: candidate window is bounded");
        {
            int all_rec = 1;

            for (i = 0; i < g_scan_files; i++)
                if (st.probe.files[i].cls != (uint8_t)IV_STORE_CLS_REC)
                    all_rec = 0;
            chk(all_rec == 1, "c08: window keeps only the most-deletable (REC) entries");
        }
    }

    rm_rf(tmpl);
}

/* ---------------- c09 周期节流 ---------------- */
static void case_step(void)
{
    char             tmpl[] = "/tmp/ivstore_XXXXXX";
    char             miss[256];
    iv_storage_cfg_t cfg;
    iv_storage_ops_t ops;
    iv_storage_t     st;
    iv_storage_stats_t stats;

    if (mkdtemp(tmpl) == NULL) {
        chk(0, "c09: mkdtemp failed");
        return;
    }
    /* 指向 temp 下**不存在**的子目录 ⇒ 未挂载 ⇒ 故障路径（step 也要能跑故障） */
    j2(miss, sizeof(miss), tmpl, "missing");
    iv_storage_cfg_default(&cfg);
    copy_str(cfg.root, sizeof(cfg.root), miss);
    cfg.interval_ms = 1000u;

    iv_storage_ops_default(&ops);
    chk(iv_storage_init(&st, &cfg, &ops) == IV_OK, "c09: init ok");
    chk(iv_storage_init(&st, &cfg, &ops) == IV_ESTATE, "c09: double init rejected");
    chk(iv_storage_init(NULL, &cfg, &ops) == IV_EINVAL, "c09: NULL handle rejected");

    /* 首次必然跑（last_run_ms == 0） */
    chk(iv_storage_step(&st, 5000u) == IV_OK, "c09: first step runs");
    iv_storage_stats(&st, &stats);
    chk(stats.rounds == 1u, "c09: first round counted");
    chk(iv_storage_fault(&st) == 1, "c09: missing root -> fault (not mounted)");

    /* 未到期：不跑 */
    chk(iv_storage_step(&st, 5500u) == IV_OK, "c09: step within interval ok");
    iv_storage_stats(&st, &stats);
    chk(stats.rounds == 1u, "c09: throttled, no extra round");
    chk(iv_storage_level(&st) == IV_STORE_OK, "c09: level readable");
    chk(iv_storage_alarm(&st) == 1, "c09: fault raises alarm");

    /* 到期：再跑 */
    chk(iv_storage_step(&st, 6000u) == IV_OK, "c09: step after interval ok");
    iv_storage_stats(&st, &stats);
    chk(stats.rounds == 2u, "c09: second round counted");
    chk(stats.faults >= 2u, "c09: fault rounds accumulated");

    rm_rf(tmpl);
}

/* ---------------- c10 根目录解析 ---------------- */
static void case_root(void)
{
    char buf[IV_STORAGE_PATH_MAX];

    iv_storage_root_resolve("/mnt/x", buf, sizeof(buf));
    chk(strcmp(buf, "/mnt/x") == 0, "c10: explicit cfg root wins");

    (void)unsetenv("IVSBOX_SD_ROOT");
    iv_storage_root_resolve("", buf, sizeof(buf));
    chk(strcmp(buf, IV_STORAGE_ROOT_DEFAULT) == 0, "c10: default root");

    (void)setenv("IVSBOX_SD_ROOT", "/tmp/media", 1);
    iv_storage_root_resolve("", buf, sizeof(buf));
    chk(strcmp(buf, "/tmp/media") == 0, "c10: env override");
    iv_storage_root_resolve("/mnt/x", buf, sizeof(buf));
    chk(strcmp(buf, "/mnt/x") == 0, "c10: cfg still wins over env");
    (void)unsetenv("IVSBOX_SD_ROOT");

    iv_storage_root_resolve("", NULL, 0u);
    chk(1, "c10: NULL out tolerated");

    chk(strcmp(iv_storage_level_name(IV_STORE_STOP), "stop") == 0, "c10: level names");
}

int main(void)
{
    iv_log_set_sink(noop_sink, NULL);

    case_levels();
    case_fault();
    case_priority();
    case_youngest();
    case_max_delete();
    case_real_dir();
    case_step();
    case_root();

    if (g_fail != 0) {
        fprintf(stderr, "test_storage FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_storage passed (watermark ladder+hysteresis, fault path, "
           "delete priority, bounded window, throttle)\n");
    return 0;
}
