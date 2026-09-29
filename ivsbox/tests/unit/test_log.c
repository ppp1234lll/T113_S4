/*
 * iv_log 落盘与过期清理单测（计划 M1-S2）
 *
 * 覆盖：
 *   1) 落盘路径 <root>/YYYY-MM-DD/HH.log 与行内容（时间戳 / 级别 / 模块 / 正文）；
 *   2) 保留 N 天：过期日期目录连文件一并删除，保留期内不动；
 *   3) 非日期目录、日期名普通文件与日期名符号链接不被误删或跟随；
 *   4) keep_days = 0 关闭清理；
 *   5) root 长度越界被拒；
 *   6) 单小时文件字节上限：触顶丢弃、标记行恰好一次、0 关闭后恢复写入；
 *   7) 多线程并发写入、修改设置与 root 快照查询无数据竞争或撕裂。
 *
 * 全程只在 /tmp 下的临时目录里活动，不碰默认的 /mnt/UDISK/log；风暴抑制的完整用例在
 * tests/unit/test_basic.c（本文件不重复）。
 */
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_err.h"
#include "ivsbox/iv_log.h"

#define P_MAX 512

static int  g_fail;
static char g_root[256];
static char g_today[16]; /* 用例期间的当天日期，字节上限用例据此定位小时文件 */
static volatile unsigned g_sink_lines;
static volatile unsigned g_thread_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void concurrent_sink(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)user;
    if (module != NULL && line != NULL)
        (void)__atomic_add_fetch(&g_sink_lines, 1u, __ATOMIC_RELAXED);
}

static void *log_writer_thread(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < 4000; i++) {
        IV_LOG_I(NULL, "concurrent line %d", i);
        if ((i % 17) == 0)
            iv_log_code(LOG_WARNING, NULL, IV_ERR_NET_CAMERA1_FAULT,
                        "concurrent coded line %d", i);
    }
    return NULL;
}

static void *log_config_thread(void *arg)
{
    int  i;
    char root[256];

    (void)arg;
    for (i = 0; i < 2000; i++) {
        iv_log_set_level((i & 1) ? LOG_DEBUG : LOG_INFO);
        (void)iv_log_set_root(NULL);
        (void)iv_log_init((i & 1) ? "writer-a" : "writer-b");
        if (iv_log_get_root(root, sizeof(root)) != 0 || root[0] != '\0')
            (void)__atomic_add_fetch(&g_thread_fail, 1u, __ATOMIC_RELAXED);
        (void)iv_log_set_root(g_root);
        if (iv_log_get_root(root, sizeof(root)) != 0 || strcmp(root, g_root) != 0)
            (void)__atomic_add_fetch(&g_thread_fail, 1u, __ATOMIC_RELAXED);
    }
    return NULL;
}

/* ------------------------------------------------------------------ 工具 */

static int dir_exists(const char *p)
{
    struct stat st;

    return (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}

static int file_exists(const char *p)
{
    struct stat st;

    return (stat(p, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

static int is_symlink(const char *p)
{
    struct stat st;

    return (lstat(p, &st) == 0 && S_ISLNK(st.st_mode)) ? 1 : 0;
}

static void rm_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return;

    if (S_ISDIR(st.st_mode)) {
        DIR           *d = opendir(path);
        struct dirent *e;

        if (d) {
            while ((e = readdir(d)) != NULL) {
                char sub[P_MAX];

                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                if (snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) <= 0)
                    continue;
                rm_tree(sub);
            }
            closedir(d);
        }
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

/* 挑 hour=12 做基准，避免午夜/DST 边界把日历日算歪 */
static void day_ago(int n, char *out, size_t cap)
{
    time_t    now = time(NULL);
    struct tm tmv;

    if (!localtime_r(&now, &tmv)) {
        out[0] = '\0';
        return;
    }
    tmv.tm_hour  = 12;
    tmv.tm_min   = 0;
    tmv.tm_sec   = 0;
    tmv.tm_isdst = -1;
    tmv.tm_mday -= n;

    now = mktime(&tmv);
    if (now == (time_t)-1 || !localtime_r(&now, &tmv)) {
        out[0] = '\0';
        return;
    }
    (void)strftime(out, cap, "%Y-%m-%d", &tmv);
}

/* 在 root 下造一个日期目录，里面放一个日志文件 */
static int make_day(const char *day, const char *file)
{
    char dir[P_MAX];
    char p[P_MAX];
    FILE *f;

    if (snprintf(dir, sizeof(dir), "%s/%s", g_root, day) <= 0)
        return -1;
    if (mkdir(dir, 0755) != 0)
        return -1;
    if (snprintf(p, sizeof(p), "%s/%s", dir, file) <= 0)
        return -1;
    f = fopen(p, "w");
    if (!f)
        return -1;
    fputs("stale\n", f);
    fclose(f);
    return 0;
}

/* 把目录内所有 HH.log 内容串起来，返回文件个数，<0 为打不开 */
static int concat_logs(const char *dir, char *buf, size_t cap)
{
    DIR           *d = opendir(dir);
    struct dirent *e;
    size_t         used = 0;
    int            files = 0;

    if (!d)
        return -1;

    buf[0] = '\0';
    while ((e = readdir(d)) != NULL) {
        char   p[P_MAX];
        FILE  *f;
        size_t got;

        if (strlen(e->d_name) != 6 || strncmp(e->d_name + 2, ".log", 4) != 0)
            continue;
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        if (e->d_name[1] < '0' || e->d_name[1] > '9')
            continue;
        if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) <= 0)
            continue;

        f = fopen(p, "r");
        if (!f)
            continue;
        got = fread(buf + used, 1, cap - 1u - used, f);
        used += got;
        buf[used] = '\0';
        fclose(f);
        files++;
    }
    closedir(d);
    return files;
}

/* 取当天目录里的 HH.log 路径（本测试同一天目录内只有当前小时一个日志文件） */
static int today_log_path(char *out, size_t cap)
{
    char          dir[P_MAX];
    DIR          *d;
    struct dirent *e;
    int           found = 0;

    if (snprintf(dir, sizeof(dir), "%s/%s", g_root, g_today) <= 0)
        return -1;
    d = opendir(dir);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strlen(e->d_name) != 6 || strncmp(e->d_name + 2, ".log", 4) != 0)
            continue;
        if (snprintf(out, cap, "%s/%s", dir, e->d_name) <= 0)
            continue;
        found = 1;
        break;
    }
    closedir(d);
    return found ? 0 : -1;
}

/* ------------------------------------------------------------------ 用例 */

int main(void)
{
    char  today[16];
    char  d3[16];
    char  d5[16];
    char  d2[16];
    char  d9[16];
    char  p[P_MAX];
    char  outside[P_MAX];
    char  outside_file[P_MAX];
    char  symlink_path[P_MAX];
    char  buf[4096];
    char  big[600];
    int   files;
    int   n;

    if (snprintf(g_root, sizeof(g_root), "/tmp/ivlog_test_%ld", (long)getpid()) <= 0) {
        fprintf(stderr, "FAIL: build temp root\n");
        return 1;
    }
    rm_tree(g_root);
    if (snprintf(outside, sizeof(outside), "%s_outside", g_root) <= 0) {
        fprintf(stderr, "FAIL: build outside temp root\n");
        return 1;
    }
    rm_tree(outside);

    /* 0) root 长度越界必须被拒 */
    memset(big, 'a', sizeof(big) - 1u);
    big[0]                = '/';
    big[sizeof(big) - 1u] = '\0';
    chk(iv_log_set_root(big) == -1, "over-long root rejected");

    chk(iv_log_set_root(g_root) == 0, "iv_log_set_root ok");
    chk(iv_log_get_root(buf, sizeof(buf)) == 0 && strcmp(buf, g_root) == 0,
        "iv_log_get_root round-trip");
    chk(iv_log_get_root(NULL, 0u) == -1, "iv_log_get_root rejects NULL output");
    chk(iv_log_get_keep_days() == 3u, "default keep days is 3");
    iv_log_set_stderr(0);
    chk(iv_log_init("test_log") == 0, "iv_log_init ok");

    /* 1) 路径与内容 */
    day_ago(0, today, sizeof(today));
    memcpy(g_today, today, sizeof(g_today)); /* 记下当天，供字节上限用例定位文件 */
    IV_LOG_I("app", "MARKER_A plain info");
    IV_LOG_W("netmgr", "MARKER_B warn line");
    iv_log_code(LOG_ERR, "link", IV_ERR_NET_CAMERA1_FAULT, "MARKER_C cam1 timeout");

    chk(snprintf(p, sizeof(p), "%s/%s", g_root, today) > 0, "build today dir path");
    chk(dir_exists(p) == 1, "today dir auto-created");

    files = concat_logs(p, buf, sizeof(buf));
    chk(files >= 1, "at least one HH.log written");
    chk(strstr(buf, "MARKER_A plain info") != NULL, "info line persisted");
    chk(strstr(buf, "INFO [app] MARKER_A") != NULL, "level + module tag present");
    chk(strstr(buf, "MARKER_C") != NULL, "code log persisted");
    chk(strstr(buf, "NET_CAMERA1_FAULT(0x20410000)") != NULL, "code name + value present");
    chk(strchr(buf, ':') != NULL, "timestamp present");

    /* 2) 过期清理：keep=3 ⇒ 保留 today/today-1/today-2，删 today-3 及更早 */
    day_ago(2, d2, sizeof(d2));
    day_ago(3, d3, sizeof(d3));
    day_ago(5, d5, sizeof(d5));
    day_ago(9, d9, sizeof(d9));
    chk(make_day(d2, "01.log") == 0, "make 2-day-old dir");
    chk(make_day(d3, "01.log") == 0, "make 3-day-old dir");
    chk(make_day(d5, "01.log") == 0, "make 5-day-old dir");
    chk(make_day(d9, "01.log") == 0, "make 9-day-old dir");

    /* 干扰项：非日期目录、日期名普通文件、日期名 symlink 都不该被删；
     * symlink 指向 root 外的目录，能直接证伪 purge 越界跟随。 */
    {
        char nd[P_MAX];
        char df[P_MAX];

        snprintf(nd, sizeof(nd), "%s/notadate", g_root);
        snprintf(df, sizeof(df), "%s/2000-01-01", g_root);
        mkdir(nd, 0755);
        {
            FILE *f = fopen(df, "w");
            if (f) {
                fputs("plain file\n", f);
                fclose(f);
            }
        }
    }
    chk(mkdir(outside, 0755) == 0, "make outside directory");
    chk(snprintf(outside_file, sizeof(outside_file), "%s/keep.txt", outside) > 0,
        "build outside file path");
    {
        FILE *f = fopen(outside_file, "w");

        chk(f != NULL, "make outside sentinel file");
        if (f) {
            fputs("must survive purge\n", f);
            fclose(f);
        }
    }
    chk(snprintf(symlink_path, sizeof(symlink_path), "%s/1999-12-31", g_root) > 0,
        "build date symlink path");
    chk(symlink(outside, symlink_path) == 0, "make expired-date symlink");

    n = iv_log_purge();
    chk(n == 3, "purge removed exactly 3 expired dirs (d3/d5/d9)");
    chk(dir_exists(p) == 1, "today dir kept");
    chk(snprintf(p, sizeof(p), "%s/%s", g_root, d2) > 0 && dir_exists(p) == 1,
        "2-day-old dir kept (inside window)");
    chk(snprintf(p, sizeof(p), "%s/%s", g_root, d3) > 0 && dir_exists(p) == 0,
        "3-day-old dir removed");
    chk(snprintf(p, sizeof(p), "%s/%s", g_root, d5) > 0 && dir_exists(p) == 0,
        "5-day-old dir removed");
    chk(snprintf(p, sizeof(p), "%s/notadate", g_root) > 0 && dir_exists(p) == 1,
        "non-date dir untouched");
    chk(snprintf(p, sizeof(p), "%s/2000-01-01", g_root) > 0 && file_exists(p) == 1,
        "date-named regular file untouched");
    chk(is_symlink(symlink_path) == 1, "expired-date symlink untouched");
    chk(dir_exists(outside) == 1 && file_exists(outside_file) == 1,
        "purge does not follow date symlink outside root");

    /* 3) keep_days = 0 ⇒ 不清理（d9 重建后仍应在） */
    chk(make_day(d9, "01.log") == 0, "re-create 9-day-old dir");
    iv_log_set_keep_days(0);
    chk(iv_log_get_keep_days() == 0u, "keep days set to 0");
    chk(iv_log_purge() == 0, "keep_days=0 disables purge");
    chk(snprintf(p, sizeof(p), "%s/%s", g_root, d9) > 0 && dir_exists(p) == 1,
        "old dir survives when purge disabled");

    /* 4) 恢复默认保留天数 */
    iv_log_set_keep_days(3);
    chk(iv_log_get_keep_days() == 3u, "keep days restored to 3");

    /* 5) 单小时文件字节上限：触顶丢弃、标记行恰好一次、0 关闭后恢复写入。
     * 沿用同一临时 root 与当前小时文件（前面用例已写入 MARKER_* 数行）。
     * 边界假设：用例执行中不跨自然小时（与现有用例同一假设量级）。 */
    {
        struct stat st;
        char   lp[P_MAX];
        char   line[120];
        long   size_at_cap;
        long   size_frozen;
        int    i;
        int    marks;
        char  *m;

        iv_log_set_file_max(256u);

        /* 连续写约 100 字节/条的长行，把文件顶过 256 上限 */
        memset(line, 'x', sizeof(line) - 1u);
        line[sizeof(line) - 1u] = '\0';
        for (i = 0; i < 32; i++)
            IV_LOG_I("app", "CAPTEST %s", line);

        chk(today_log_path(lp, sizeof(lp)) == 0, "cap case: locate hour log");
        chk(stat(lp, &st) == 0, "cap case: stat hour log");
        size_at_cap = (long)st.st_size;
        /* 触顶判定发生在写入之前，最后写入的那条与标记行都允许超限一次，
         * 总量级上限 = 256 + 一条长行 + 一行标记（含余量） */
        chk(size_at_cap <= 256 + (long)(sizeof(line) - 1u) + 160,
            "cap case: file clamped near the 256-byte limit");

        /* 触顶标记恰好 1 次 */
        chk(snprintf(p, sizeof(p), "%s/%s", g_root, g_today) > 0,
            "cap case: build today dir path");
        files = concat_logs(p, buf, sizeof(buf));
        chk(files >= 1, "cap case: hour log readable");
        marks = 0;
        m = buf;
        while ((m = strstr(m, "file size cap reached")) != NULL) {
            marks++;
            m++;
        }
        chk(marks == 1, "cap marker written exactly once");

        /* 触顶后再写：文件停止增长 */
        size_frozen = size_at_cap;
        for (i = 0; i < 8; i++)
            IV_LOG_I("app", "CAPTEST %s", line);
        chk(stat(lp, &st) == 0 && (long)st.st_size == size_frozen,
            "cap case: file stops growing once capped");

        /* 上限关闭（0）后恢复写入 */
        iv_log_set_file_max(0u);
        IV_LOG_I("app", "CAPTEST resumed after limit disabled");
        chk(stat(lp, &st) == 0 && (long)st.st_size > size_frozen,
            "cap case: writing resumes after limit disabled");
    }

    /* 6) 并发设置/查询/写日志：旧实现把 s_level/s_ident 在锁外读取，并直接把
     * s_root 内部缓冲指针交给调用方，TSan 会报真实数据竞争。这里让四个 writer
     * 与一个配置线程同时运行；sink 只做原子计数，不在回调里再入日志模块。 */
    {
        enum { WRITERS = 4 };
        pthread_t writers[WRITERS];
        pthread_t config;
        int       writer_ok[WRITERS];
        int       config_ok;
        int       i;

        memset(writer_ok, 0, sizeof(writer_ok));
        __atomic_store_n(&g_sink_lines, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&g_thread_fail, 0u, __ATOMIC_RELAXED);
        iv_log_set_sink(concurrent_sink, NULL);

        for (i = 0; i < WRITERS; i++) {
            writer_ok[i] = (pthread_create(&writers[i], NULL, log_writer_thread, NULL) == 0);
            chk(writer_ok[i], "concurrent writer thread created");
        }
        config_ok = (pthread_create(&config, NULL, log_config_thread, NULL) == 0);
        chk(config_ok, "concurrent config thread created");

        for (i = 0; i < WRITERS; i++) {
            if (writer_ok[i])
                (void)pthread_join(writers[i], NULL);
        }
        if (config_ok)
            (void)pthread_join(config, NULL);

        chk(__atomic_load_n(&g_thread_fail, __ATOMIC_RELAXED) == 0u,
            "concurrent root snapshots are complete and untorn");
        chk(__atomic_load_n(&g_sink_lines, __ATOMIC_RELAXED) > 0u,
            "concurrent writers reached the sink");

        iv_log_set_sink(NULL, NULL);
        (void)iv_log_set_root(NULL); /* 关闭仍打开的文件，下面才能安全删测试目录 */
        iv_log_set_level(LOG_DEBUG);
    }

    rm_tree(g_root);
    chk(dir_exists(g_root) == 0, "temp root cleaned up");
    rm_tree(outside);
    chk(dir_exists(outside) == 0, "outside temp root cleaned up");

    if (g_fail) {
        fprintf(stderr, "test_log failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_log passed\n");
    return 0;
}
