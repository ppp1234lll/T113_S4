/*
 * IVSBox SD 卡存储策略实现（libivmodules，功能开发计划 M6-S6.5）
 *
 * 契约、水位口径、可注入 IO 的用法见 include/ivsbox/iv_storage.h 文件头。
 * 本文件只做两件事：
 *   1) `iv_store_decide()` —— 纯决策（无 IO / 无时间 / 无全局量）；
 *   2) `iv_storage_*()`   —— 把 IO 与决策按周期串起来，外加 POSIX 缺省 IO。
 *
 * 分层：modules 层，允许 libc ＋ iv_log；**不引 hal / 不碰配置模块**（水位线是
 * 模块常量，§18.5 已拍板"SD 容量 statfs 实读、不占配置键"）。
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_storage.h"

#define ST_MOD "storage"

/* ---------------------------------------------------------------------------
 * 小工具（全部走有界拷贝，**不用 snprintf 往定长成员里拼未知长度的源** ——
 * 那会被 -Wformat-truncation 断在 -Werror 上，且截断是真语义损失）
 * ------------------------------------------------------------------------- */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (dst == NULL || cap == 0u)
        return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap)
        n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* dst = a (+ "/" + b)。b 为空则 dst = a。装不下则 dst 为空串（调用方跳过该项）。 */
static void join_path(char *dst, size_t cap, const char *a, const char *b)
{
    size_t na, nb;

    if (dst == NULL || cap == 0u)
        return;
    dst[0] = '\0';
    if (a == NULL)
        return;
    na = strlen(a);
    nb = (b != NULL) ? strlen(b) : 0u;
    if (nb == 0u) {
        copy_str(dst, cap, a);
        return;
    }
    if (na + 1u + nb >= cap)
        return;
    memcpy(dst, a, na);
    dst[na] = '/';
    memcpy(dst + na + 1u, b, nb);
    dst[na + 1u + nb] = '\0';
}

/* 取有效水位线配置（0 = 用默认）并把梯子夹紧到自洽区间。 */
static void level_cfg(const iv_storage_cfg_t *cfg, uint32_t *clean, uint32_t *reject,
                      uint32_t *stop, uint32_t *recover)
{
    *clean   = (cfg->clean_pct   != 0u) ? cfg->clean_pct   : IV_STORAGE_CLEAN_PCT;
    *reject  = (cfg->reject_pct  != 0u) ? cfg->reject_pct  : IV_STORAGE_REJECT_PCT;
    *stop    = (cfg->stop_pct    != 0u) ? cfg->stop_pct    : IV_STORAGE_STOP_PCT;
    *recover = (cfg->recover_pct != 0u) ? cfg->recover_pct : IV_STORAGE_RECOVER_PCT;
    if (*stop > 100u)
        *stop = 100u;
    if (*reject > *stop)
        *reject = *stop;
    if (*clean > *reject)
        *clean = *reject;
    if (*recover > *clean)
        *recover = *clean;
}

/* "该删顺序"：类别高（REC→ALARM→SNAP）的先删；同类 mtime 旧者先删；再按名字定序
 * （让同一 mtime 秒内的两条也有确定顺序，用例才能逐条断言）。 */
static int file_precedes(const iv_store_file_t *a, const iv_store_file_t *b)
{
    if (a->cls != b->cls)
        return (a->cls > b->cls) ? 1 : 0;
    if (a->mtime_s != b->mtime_s)
        return (a->mtime_s < b->mtime_s) ? 1 : 0;
    return (strncmp(a->name, b->name, IV_STORAGE_NAME_MAX) < 0) ? 1 : 0;
}

/* 把候选塞进有界窗口：窗口未满就放；满了则与"最不该删"的一条比较后替换。
 * 这样 SD 上有上万条时，窗口里留下的始终是**最该删的 64 条**（按上述次序）。 */
static void cand_push(iv_store_probe_t *out, const iv_store_file_t *f)
{
    int i;
    int worst;

    if (out->n_files < (int)IV_STORAGE_FILES_MAX) {
        out->files[out->n_files] = *f;
        out->n_files++;
        return;
    }
    worst = 0;
    for (i = 1; i < (int)IV_STORAGE_FILES_MAX; i++) {
        /* files[worst] 比 files[i] 更该删 ⇒ i 相对更"不该删" */
        if (file_precedes(&out->files[worst], &out->files[i]))
            worst = i;
    }
    if (file_precedes(f, &out->files[worst]))
        out->files[worst] = *f;
}

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

void iv_storage_cfg_default(iv_storage_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->clean_pct       = IV_STORAGE_CLEAN_PCT;
    cfg->reject_pct      = IV_STORAGE_REJECT_PCT;
    cfg->stop_pct        = IV_STORAGE_STOP_PCT;
    cfg->recover_pct     = IV_STORAGE_RECOVER_PCT;
    cfg->interval_ms     = IV_STORAGE_INTERVAL_MS;
    cfg->keep_youngest_s = IV_STORAGE_KEEP_YOUNGEST_S;
    cfg->max_delete      = IV_STORAGE_DEL_MAX;
    cfg->root[0]         = '\0';
    copy_str(cfg->dir[0].sub, sizeof(cfg->dir[0].sub), IV_STORAGE_SNAP_SUBDIR);
    cfg->dir[0].cls = (uint8_t)IV_STORE_CLS_SNAP;
    copy_str(cfg->dir[1].sub, sizeof(cfg->dir[1].sub), IV_STORAGE_REC_SUBDIR);
    cfg->dir[1].cls = (uint8_t)IV_STORE_CLS_REC;
    cfg->n_dir      = 2;
}

int iv_storage_init(iv_storage_t *s, const iv_storage_cfg_t *cfg,
                    const iv_storage_ops_t *ops)
{
    iv_storage_ops_t def;

    if (s == NULL || cfg == NULL)
        return IV_EINVAL;
    if (s->inited)
        return IV_ESTATE;

    memset(s, 0, sizeof(*s));
    s->cfg = *cfg; /* 值语义（含 dir[] 数组，无指针） */
    if (s->cfg.n_dir < 0)
        s->cfg.n_dir = 0;
    if (s->cfg.n_dir > (int)IV_STORAGE_DIR_MAX)
        s->cfg.n_dir = (int)IV_STORAGE_DIR_MAX;

    if (ops != NULL) {
        s->ops = *ops;
    } else {
        iv_storage_ops_default(&def);
        s->ops = def;
    }
    if (s->ops.scan == NULL || s->ops.unlink_file == NULL)
        return IV_EINVAL;

    s->level  = (uint8_t)IV_STORE_OK;
    s->inited = 1u;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 纯决策
 * ------------------------------------------------------------------------- */

int iv_store_decide(const iv_storage_cfg_t *cfg, const iv_store_probe_t *pr,
                    uint64_t now_s, uint8_t cur_level, iv_store_plan_t *out)
{
    uint32_t clean, reject, stop, recover;
    uint32_t max_del;
    uint32_t keep_young;
    uint64_t total, free_b;
    uint32_t pct;
    uint32_t tgt;
    uint8_t  lv;
    int      order[IV_STORAGE_FILES_MAX];
    int      n_ord = 0;
    int      n_files;
    uint64_t need  = 0u;
    uint64_t freed = 0u;
    int      i;
    int      j;

    if (cfg == NULL || pr == NULL || out == NULL)
        return IV_EINVAL;
    memset(out, 0, sizeof(*out));

    level_cfg(cfg, &clean, &reject, &stop, &recover);
    max_del   = (cfg->max_delete != 0u) ? cfg->max_delete : (uint32_t)IV_STORAGE_DEL_MAX;
    if (max_del > (uint32_t)IV_STORAGE_DEL_MAX)
        max_del = (uint32_t)IV_STORAGE_DEL_MAX;
    keep_young = (cfg->keep_youngest_s != 0u) ? cfg->keep_youngest_s
                                              : IV_STORAGE_KEEP_YOUNGEST_S;

    out->fault = (uint8_t)((!pr->mounted || pr->readonly || pr->io_err) ? 1 : 0);

    /* 使用率：整数**下取整**。取整方向刻意选下取整 —— 水位线是"到 85% 才动手"，
     * 上取整会让 84.0001% 也触发清理，比晚一拍更坏（清理是不可逆动作）。 */
    total  = pr->total_bytes;
    free_b = (pr->free_bytes > total) ? total : pr->free_bytes;
    pct    = (total == 0u) ? 0u : (uint32_t)(((total - free_b) * 100u) / total);
    if (pct > 100u)
        pct = 100u;
    out->used_pct = pct;

    tgt = (pct >= stop)     ? (uint32_t)IV_STORE_STOP
        : (pct >= reject)   ? (uint32_t)IV_STORE_REJECT
        : (pct >= clean)    ? (uint32_t)IV_STORE_CLEAN
                            : (uint32_t)IV_STORE_OK;

    if (tgt > (uint32_t)cur_level)
        lv = (uint8_t)tgt;  /* 升档：立即生效（水位涨了必须马上挡） */
    else if (pct <= recover)
        lv = (uint8_t)IV_STORE_OK; /* 降档：只有回到恢复线才解除 */
    else
        lv = cur_level;     /* 迟滞区（recover, stop）：维持原档，防抖 */

    out->recovered = (uint8_t)((cur_level != (uint8_t)IV_STORE_OK &&
                                lv == (uint8_t)IV_STORE_OK) ? 1 : 0);
    out->level = lv;

    out->allow_new = (uint8_t)((out->fault || lv >= (uint8_t)IV_STORE_REJECT) ? 0 : 1);
    out->stop_rec  = (uint8_t)((out->fault || lv >= (uint8_t)IV_STORE_STOP) ? 1 : 0);
    out->alarm     = out->stop_rec;

    /* ---- 清理：CLEAN 及以上、且非故障（未挂载时没有可删的目标）---- */
    if (lv >= (uint8_t)IV_STORE_CLEAN && !out->fault && total != 0u) {
        uint64_t want_free = (total / 100u) * (uint64_t)recover +
                             ((total % 100u) * (uint64_t)recover) / 100u;

        need = (want_free > free_b) ? (want_free - free_b) : 0u;
    }

    if (need > 0u) {
        n_files = pr->n_files;
        if (n_files > (int)IV_STORAGE_FILES_MAX)
            n_files = (int)IV_STORAGE_FILES_MAX;

        /* 插入排序（n ≤ 64）：按"该删顺序"排好索引 */
        for (i = 0; i < n_files; i++) {
            int pos = n_ord;

            while (pos > 0 && file_precedes(&pr->files[i], &pr->files[order[pos - 1]]))
                pos--;
            for (j = n_ord; j > pos; j--)
                order[j] = order[j - 1];
            order[pos] = i;
            n_ord++;
        }

        for (i = 0; i < n_ord && (uint32_t)out->n_delete < max_del && freed < need; i++) {
            const iv_store_file_t *f = &pr->files[order[i]];

            /* 太新的切片不删：正在写的分片 mtime 就是"现在" */
            if (now_s != 0u && f->mtime_s != 0u &&
                f->mtime_s + (uint64_t)keep_young > now_s)
                continue;
            out->del[out->n_delete] = f;
            out->n_delete++;
            freed += f->size;
        }
    }

    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 周期驱动
 * ------------------------------------------------------------------------- */

int iv_storage_run_once(iv_storage_t *s, uint64_t now_ms)
{
    iv_store_plan_t plan;
    uint8_t         was_level;
    uint8_t         was_fault;
    uint64_t        bytes = 0u;
    int             n_del = 0;
    int             i;
    int             rc;

    if (s == NULL)
        return IV_EINVAL;
    if (!s->inited)
        return IV_ESTATE;

    was_level = s->level;
    was_fault = s->fault;

    memset(&s->probe, 0, sizeof(s->probe));
    if (s->ops.scan(s->ops.arg, &s->cfg, &s->probe) != IV_OK)
        s->probe.io_err = 1u; /* scan 自己报错也走同一条故障路径 */

    rc = iv_store_decide(&s->cfg, &s->probe, now_ms / 1000u, s->level, &plan);
    if (rc != IV_OK)
        return rc;

    s->level    = plan.level;
    s->fault    = plan.fault;
    s->alarm    = plan.alarm;
    s->used_pct = plan.used_pct;

    for (i = 0; i < plan.n_delete; i++) {
        if (s->ops.unlink_file(s->ops.arg, plan.del[i]->path) == IV_OK) {
            n_del++;
            bytes += plan.del[i]->size;
            IV_LOG_I(ST_MOD, "sd cleanup: removed %s (%llu B), level=%s used=%u%%",
                     plan.del[i]->path, (unsigned long long)plan.del[i]->size,
                     iv_storage_level_name((int)plan.level), (unsigned)plan.used_pct);
        } else {
            IV_LOG_W(ST_MOD, "sd cleanup: unlink failed: %s (%s)", plan.del[i]->path,
                     strerror(errno));
        }
    }

    s->st.deletes += (uint64_t)n_del;
    s->st.del_bytes += bytes;
    s->st.last_deleted_n = n_del;
    s->st.last_deleted_bytes = bytes;
    s->st.last_used_pct = plan.used_pct;
    s->st.last_round_ms = now_ms;
    s->st.rounds++;
    if (plan.fault)
        s->st.faults++;

    /* 只在"档位/故障状态变化"与"本轮真删了东西"时刷 INFO；故障新发时刷 WARN。
     * 30 s 一轮、每轮都刷会把日志淹掉（SD 没插时尤其）。 */
    if (plan.fault && !was_fault)
        IV_LOG_W(ST_MOD, "storage fault: mounted=%d readonly=%d io_err=%d (recording gated)",
                 (int)s->probe.mounted, (int)s->probe.readonly, (int)s->probe.io_err);
    else if ((int)plan.level != (int)was_level)
        IV_LOG_I(ST_MOD, "storage level %s -> %s (used=%u%% free=%llu/%llu B)",
                 iv_storage_level_name((int)was_level),
                 iv_storage_level_name((int)plan.level), (unsigned)plan.used_pct,
                 (unsigned long long)s->probe.free_bytes,
                 (unsigned long long)s->probe.total_bytes);

    s->last_run_ms = now_ms;
    return IV_OK;
}

int iv_storage_step(iv_storage_t *s, uint64_t now_ms)
{
    uint32_t ivl;

    if (s == NULL)
        return IV_EINVAL;
    if (!s->inited)
        return IV_ESTATE;
    ivl = (s->cfg.interval_ms != 0u) ? s->cfg.interval_ms : IV_STORAGE_INTERVAL_MS;
    if (s->last_run_ms != 0u && (now_ms - s->last_run_ms) < ivl)
        return IV_OK;
    return iv_storage_run_once(s, now_ms);
}

/* ---------------------------------------------------------------------------
 * 读数
 * ------------------------------------------------------------------------- */

iv_store_level_t iv_storage_level(const iv_storage_t *s)
{
    return (s != NULL) ? (iv_store_level_t)s->level : IV_STORE_OK;
}

int iv_storage_allow_new(const iv_storage_t *s)
{
    if (s == NULL)
        return 0;
    return (s->fault || s->level >= (uint8_t)IV_STORE_REJECT) ? 0 : 1;
}

int iv_storage_stop_rec(const iv_storage_t *s)
{
    if (s == NULL)
        return 0;
    return (s->fault || s->level >= (uint8_t)IV_STORE_STOP) ? 1 : 0;
}

int iv_storage_alarm(const iv_storage_t *s)
{
    return (s != NULL) ? (int)s->alarm : 0;
}

int iv_storage_fault(const iv_storage_t *s)
{
    return (s != NULL) ? (int)s->fault : 0;
}

int iv_storage_used_pct(const iv_storage_t *s)
{
    return (s != NULL) ? (int)s->used_pct : 0;
}

void iv_storage_stats(const iv_storage_t *s, iv_storage_stats_t *out)
{
    if (out == NULL)
        return;
    if (s == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s->st;
}

int iv_storage_enabled(const iv_storage_t *s)
{
    return (s != NULL && s->inited) ? 1 : 0;
}

const char *iv_storage_level_name(int level)
{
    switch (level) {
    case IV_STORE_OK:     return "ok";
    case IV_STORE_CLEAN:  return "clean";
    case IV_STORE_REJECT: return "reject";
    case IV_STORE_STOP:   return "stop";
    default:              return "?";
    }
}

/* ---------------------------------------------------------------------------
 * POSIX 缺省 IO
 * ------------------------------------------------------------------------- */

void iv_storage_root_resolve(const char *cfg_root, char *out, size_t cap)
{
    const char *env;

    if (out == NULL || cap == 0u)
        return;
    out[0] = '\0';
    if (cfg_root != NULL && cfg_root[0] != '\0') {
        copy_str(out, cap, cfg_root);
        return;
    }
    env = getenv("IVSBOX_SD_ROOT"); /* 开发/测试用；产线不设 */
    if (env != NULL && env[0] != '\0') {
        copy_str(out, cap, env);
        return;
    }
    copy_str(out, cap, IV_STORAGE_ROOT_DEFAULT);
}

/* 有界遍历：root 下一层（snap / video），再进一层日期目录（video/<YYYYMMDD>/）。
 * 目录不存在不算错（SD 未插时 root 就已经不在 ⇒ mounted=0）；打开失败算 io_err。 */
static int walk_dir(const char *dir, uint8_t cls, unsigned depth, iv_store_probe_t *out)
{
    DIR           *d;
    struct dirent *e;
    char           path[IV_STORAGE_PATH_MAX];

    d = opendir(dir);
    if (d == NULL)
        return (errno == ENOENT) ? IV_OK : IV_EIO;

    while ((e = readdir(d)) != NULL) {
        struct stat sb;
        int         is_alarm;

        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;

        join_path(path, sizeof(path), dir, e->d_name);
        if (path[0] == '\0')
            continue; /* 路径过长：跳过，不让一个怪名字带坏整轮 */
        if (stat(path, &sb) != 0)
            continue;

        if (S_ISDIR(sb.st_mode)) {
            if (depth + 1u < IV_STORAGE_DEPTH_MAX)
                (void)walk_dir(path, cls, depth + 1u, out);
            continue;
        }
        if (!S_ISREG(sb.st_mode))
            continue;

        {
            iv_store_file_t f;

            memset(&f, 0, sizeof(f));
            copy_str(f.path, sizeof(f.path), path);
            copy_str(f.name, sizeof(f.name), e->d_name);
            f.size    = (uint64_t)sb.st_size;
            f.mtime_s = (sb.st_mtime > 0) ? (uint64_t)sb.st_mtime : 0u;
            is_alarm  = (strncmp(e->d_name, IV_STORAGE_ALARM_PREFIX,
                                 strlen(IV_STORAGE_ALARM_PREFIX)) == 0) ? 1 : 0;
            f.cls     = (uint8_t)(is_alarm ? IV_STORE_CLS_ALARM : cls);
            cand_push(out, &f);
        }
    }

    (void)closedir(d);
    return IV_OK;
}

static int posix_scan(void *arg, const iv_storage_cfg_t *cfg, iv_store_probe_t *out)
{
    char           root[IV_STORAGE_PATH_MAX];
    char           dir[IV_STORAGE_PATH_MAX];
    struct stat    sb;
    struct statfs  sf;
    int            i;
    int            n_dir;

    (void)arg;
    memset(out, 0, sizeof(*out));

    iv_storage_root_resolve(cfg->root, root, sizeof(root));
    if (root[0] == '\0')
        return IV_EINVAL;

    if (stat(root, &sb) != 0 || !S_ISDIR(sb.st_mode))
        return IV_OK; /* mounted=0 ⇒ 决策走故障路径 */

    if (statfs(root, &sf) != 0)
        return IV_EIO;
    out->mounted     = 1u;
    out->total_bytes = (uint64_t)sf.f_bsize * (uint64_t)sf.f_blocks;
    out->free_bytes  = (uint64_t)sf.f_bsize * (uint64_t)sf.f_bavail;
#ifdef ST_RDONLY
    if ((sf.f_flags & (unsigned long)ST_RDONLY) != 0u)
        out->readonly = 1u;
#endif

    n_dir = cfg->n_dir;
    if (n_dir <= 0) { /* 配置没给目录表 ⇒ 退回默认两目录 */
        static const struct {
            const char *sub;
            uint8_t     cls;
        } def[2] = { { IV_STORAGE_SNAP_SUBDIR, (uint8_t)IV_STORE_CLS_SNAP },
                     { IV_STORAGE_REC_SUBDIR,  (uint8_t)IV_STORE_CLS_REC } };

        for (i = 0; i < 2; i++) {
            join_path(dir, sizeof(dir), root, def[i].sub);
            if (dir[0] != '\0' && walk_dir(dir, def[i].cls, 0u, out) != IV_OK)
                out->io_err = 1u;
        }
        return IV_OK;
    }

    if (n_dir > (int)IV_STORAGE_DIR_MAX)
        n_dir = (int)IV_STORAGE_DIR_MAX;
    for (i = 0; i < n_dir; i++) {
        join_path(dir, sizeof(dir), root, cfg->dir[i].sub);
        if (dir[0] == '\0') {
            out->io_err = 1u;
            continue;
        }
        if (walk_dir(dir, cfg->dir[i].cls, 0u, out) != IV_OK)
            out->io_err = 1u;
    }
    return IV_OK;
}

static int posix_unlink(void *arg, const char *path)
{
    (void)arg;
    if (path == NULL)
        return IV_EINVAL;
    return (unlink(path) == 0) ? IV_OK : IV_EIO;
}

void iv_storage_ops_default(iv_storage_ops_t *ops)
{
    if (ops == NULL)
        return;
    ops->scan        = posix_scan;
    ops->unlink_file = posix_unlink;
    ops->arg         = NULL;
}
