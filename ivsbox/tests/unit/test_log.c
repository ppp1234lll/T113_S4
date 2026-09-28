/*
 * iv_log 落盘与过期清理单测（计划 M1-S2）
 *
 * 覆盖：
 *   1) 落盘路径 <root>/YYYY-MM-DD/HH.log 与行内容（时间戳 / 级别 / 模块 / 正文）；
 *   2) 保留 N 天：过期日期目录连文件一并删除，保留期内不动；
 *   3) 非日期目录名与"像日期的普通文件"不被误删；
 *   4) keep_days = 0 关闭清理；
 *   5) root 长度越界被拒。
 *
 * 全程只在 /tmp 下的临时目录里活动，不碰 /opt/log；风暴抑制的完整用例在
 * tests/unit/test_basic.c（本文件不重复）。
 */
#include <dirent.h>
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

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
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

/* ------------------------------------------------------------------ 用例 */

int main(void)
{
    char  today[16];
    char  d3[16];
    char  d5[16];
    char  d2[16];
    char  d9[16];
    char  p[P_MAX];
    char  buf[4096];
    char  big[600];
    int   files;
    int   n;

    if (snprintf(g_root, sizeof(g_root), "/tmp/ivlog_test_%ld", (long)getpid()) <= 0) {
        fprintf(stderr, "FAIL: build temp root\n");
        return 1;
    }
    rm_tree(g_root);

    /* 0) root 长度越界必须被拒 */
    memset(big, 'a', sizeof(big) - 1u);
    big[0]                = '/';
    big[sizeof(big) - 1u] = '\0';
    chk(iv_log_set_root(big) == -1, "over-long root rejected");

    chk(iv_log_set_root(g_root) == 0, "iv_log_set_root ok");
    chk(strcmp(iv_log_get_root(), g_root) == 0, "iv_log_get_root round-trip");
    chk(iv_log_get_keep_days() == 3u, "default keep days is 3");
    iv_log_set_stderr(0);
    chk(iv_log_init("test_log") == 0, "iv_log_init ok");

    /* 1) 路径与内容 */
    day_ago(0, today, sizeof(today));
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

    /* 干扰项：非日期目录、以及名为日期的普通文件，都不该被删 */
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

    rm_tree(g_root);
    chk(dir_exists(g_root) == 0, "temp root cleaned up");

    if (g_fail) {
        fprintf(stderr, "test_log failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_log passed\n");
    return 0;
}
