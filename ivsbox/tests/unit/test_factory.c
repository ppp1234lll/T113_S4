/*
 * iv_factory 单测 —— 恢复出厂 / 擦除存储（2026-10-22 单片机 web 对齐第 ⑥ 项）
 *
 * 覆盖：
 *   f01 路径护栏（空串 / 相对路径 / `/` / 含 `..`）—— 每一条都是"差点删整机"
 *   f02 递归擦除：多层目录被清空、**符号链接只 unlink 不跟随**（目标目录还在）
 *   f03 恢复出厂：配置回默认 + 落盘 + 版本自增（真临时目录、真 json 文件）
 *   f04 擦日志：`<data_root>/log` 清空、根保留
 *   f05 未接的擦除位返回 IV_ENOTSUP（不是"假装成功"）
 *   f06 擦数据库 = 删行不删文件（表还在、行清零、可继续写）
 *   f07 非法掩码返回 IV_EINVAL
 *   f08 **顺序**：配置写盘失败时一条数据都不擦（半成品最坏）
 *
 * 全部用 mkdtemp 下的真实文件系统树，不 mock：擦除这种东西，mock 是自欺欺人。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_config.h"
#include "ivsbox/iv_db.h"
#include "ivsbox/iv_device.h"
#include "ivsbox/iv_factory.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_store.h"

static int  g_fail;
static char g_root[200]; /* mkdtemp 出来的根 */

/* 单测里把日志落盘关掉（与 test_config 同口径），只留一个空 sink */
static void quiet_sink(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void path_of(char *out, size_t cap, const char *rel)
{
    (void)snprintf(out, cap, "%s/%s", g_root, rel);
}

/* 建目录（只支持一层，父目录先自己建） */
static int mkdir_one(const char *path)
{
    return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int mkfile(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL)
        return -1;
    if (body != NULL)
        (void)fwrite(body, 1u, strlen(body), f);
    (void)fclose(f);
    return 0;
}

static void rm_rf(const char *path)
{
    char cmd[256];

    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) {
        /* 清理失败不影响判据，只提示 */
        fprintf(stderr, "warn: cleanup '%s' failed\n", path);
    }
}

/* =========================================================================== */
static void f01_path_guards(void)
{
    unsigned long n = 0u;

    chk(iv_factory_wipe(NULL, &n) == IV_EINVAL, "f01a: NULL rejected");
    chk(iv_factory_wipe("", &n) == IV_EINVAL, "f01b: empty rejected");
    chk(iv_factory_wipe("/", &n) == IV_EINVAL, "f01c: '/' rejected");
    chk(iv_factory_wipe("tmp/rel", &n) == IV_EINVAL, "f01d: relative rejected");
    chk(iv_factory_wipe("/tmp/../etc", &n) == IV_EINVAL, "f01e: '..' rejected");
    chk(access("/etc/passwd", R_OK) == 0, "f01f: /etc/passwd untouched by the guards");
}

static void f02_wipe_tree(void)
{
    char          media[220];
    char          sub[220];
    char          deep[220];
    char          f1[240];
    char          f2[240];
    char          f3[240];
    char          link[240];
    char          keep[220];
    char          keepf[240];
    unsigned long removed = 0u;

    path_of(media, sizeof(media), "media");
    (void)mkdir_one(media);
    path_of(sub, sizeof(sub), "media/sub");
    (void)mkdir_one(sub);
    path_of(deep, sizeof(deep), "media/sub/deep");
    (void)mkdir_one(deep);
    path_of(f1, sizeof(f1), "media/a.mp4");
    path_of(f2, sizeof(f2), "media/sub/b.jpg");
    path_of(f3, sizeof(f3), "media/sub/deep/c.mp4");
    (void)mkfile(f1, "aaa");
    (void)mkfile(f2, "bbb");
    (void)mkfile(f3, "ccc");

    /* 陷阱：media 里有一个指向"外部目录"的符号链接。跟随它就会删到外面去。 */
    path_of(keep, sizeof(keep), "keepdir");
    (void)mkdir_one(keep);
    path_of(keepf, sizeof(keepf), "keepdir/precious.txt");
    (void)mkfile(keepf, "do not delete");
    path_of(link, sizeof(link), "media/escape");
    chk(symlink(keep, link) == 0, "f02a: symlink created");

    chk(iv_factory_wipe(media, &removed) == IV_OK, "f02b: wipe returns ok");
    chk(removed >= 4u, "f02c: wipe counted files and dirs");
    chk(access(f1, F_OK) != 0, "f02d: top-level file gone");
    chk(access(f3, F_OK) != 0, "f02e: nested file gone");
    chk(access(sub, F_OK) != 0, "f02f: nested dir gone");
    chk(access(media, F_OK) == 0, "f02g: the root itself is kept (mount point)");
    chk(access(keepf, R_OK) == 0, "f02h: symlink target NOT followed (external file survives)");

    /* 根不存在："没有可擦的东西"不等于错误；但根非法必须报错（f01 已钉） */
    removed = 0u;
    path_of(media, sizeof(media), "nope");
    chk(iv_factory_wipe(media, &removed) == IV_OK && removed == 0u,
        "f02i: missing root is ok with 0 removed");
}

static void f03_reset_config(void)
{
    char                dir[220];
    char                f[240];
    iv_config_t        *cfg;
    iv_factory_cfg_t    fc;
    iv_factory_report_t rep;
    const char         *s = NULL;
    uint64_t            ver0;
    unsigned            n = 9u;

    path_of(dir, sizeof(dir), "data");
    (void)mkdir_one(dir);
    path_of(f, sizeof(f), "data/ivsbox.json");
    (void)mkfile(f,
                 "{\"_meta\":{\"version\":3},"
                 "\"sys\":{\"device\":{\"name\":\"SITE-B\"}},"
                 "\"camera\":{\"ch\":[{\"ip\":\"192.168.2.10\"}]}}\n");

    cfg = iv_config_open(dir, "ivsbox");
    chk(cfg != NULL, "f03a: config open");
    if (cfg == NULL)
        return;
    ver0 = iv_config_version(cfg);

    memset(&fc, 0, sizeof(fc));
    memset(&rep, 0, sizeof(rep));
    /* 不擦任何数据，只恢复配置 */
    chk(iv_factory_reset(cfg, NULL, &fc, 0u, &rep) == IV_OK, "f03b: reset ok");
    chk(rep.cfg_saved == 1, "f03c: config saved");
    chk(rep.cfg_changed > 0u, "f03d: changed keys reported");
    chk(rep.cfg_version == ver0 + 1u, "f03e: version bumped once");
    chk(iv_config_get_str(cfg, "sys.device.name", &s) == IV_OK && s[0] == '\0',
        "f03f: device name back to default");
    chk(iv_config_array_len(cfg, "camera.ch", &n) == IV_OK && n == 0u,
        "f03g: camera.ch dropped");
    iv_config_close(cfg);

    /* 落盘要能重新读回来 */
    cfg = iv_config_open(dir, "ivsbox");
    chk(cfg != NULL, "f03h: reopen");
    if (cfg == NULL)
        return;
    chk(iv_config_array_len(cfg, "camera.ch", &n) == IV_OK && n == 0u,
        "f03i: still empty after reload");
    chk(iv_config_get_str(cfg, "sys.device.name", &s) == IV_OK && s[0] == '\0',
        "f03j: default persisted");
    iv_config_close(cfg);
}

static void f04_erase_log(void)
{
    char                data[220];
    char                d1[240];
    char                lf[260];
    iv_config_t        *cfg;
    iv_factory_cfg_t    fc;
    iv_factory_report_t rep;

    path_of(data, sizeof(data), "data2");
    (void)mkdir_one(data);
    path_of(d1, sizeof(d1), "data2/log");
    (void)mkdir_one(d1);
    path_of(d1, sizeof(d1), "data2/log/2026-10-22");
    (void)mkdir_one(d1);
    path_of(lf, sizeof(lf), "data2/log/2026-10-22/18.log");
    (void)mkfile(lf, "log line\n");

    cfg = iv_config_open(data, "ivsbox");
    chk(cfg != NULL, "f04a: config open");
    if (cfg == NULL)
        return;

    memset(&fc, 0, sizeof(fc));
    fc.data_root = data;
    chk(iv_factory_reset(cfg, NULL, &fc, IV_FACTORY_ERASE_LOG, &rep) == IV_OK,
        "f04b: reset+erase log ok");
    chk(access(lf, F_OK) != 0, "f04c: log file gone");
    chk(access(d1, F_OK) != 0, "f04d: per-day dir gone");
    path_of(d1, sizeof(d1), "data2/log");
    chk(access(d1, F_OK) == 0, "f04e: log root kept");
    chk(rep.entries_removed >= 1u, "f04f: entries counted");
    chk(rep.erase_mask == IV_FACTORY_ERASE_LOG, "f04g: erase mask echoed");
    iv_config_close(cfg);
}

static void f05_not_wired(void)
{
    char                data[220];
    iv_config_t        *cfg;
    iv_factory_cfg_t    fc;
    iv_factory_report_t rep;

    path_of(data, sizeof(data), "data3");
    (void)mkdir_one(data);
    cfg = iv_config_open(data, "ivsbox");
    chk(cfg != NULL, "f05a: config open");
    if (cfg == NULL)
        return;

    memset(&fc, 0, sizeof(fc));
    /* 没给根 ⇒ 明确"能调但没接"，不假装擦过了 */
    chk(iv_factory_reset(cfg, NULL, &fc, IV_FACTORY_ERASE_MEDIA, &rep) == IV_ENOTSUP,
        "f05b: erase media without root -> ENOTSUP");
    chk(iv_factory_reset(cfg, NULL, NULL, IV_FACTORY_ERASE_MEDIA, &rep) == IV_ENOTSUP,
        "f05c: NULL cfg struct -> ENOTSUP");
    chk(iv_factory_reset(cfg, NULL, &fc, IV_FACTORY_ERASE_DB, &rep) == IV_ENOTSUP,
        "f05d: erase db without store -> ENOTSUP");
    /* 非法掩码 */
    chk(iv_factory_reset(cfg, NULL, &fc, 0x8u, &rep) == IV_EINVAL,
        "f05e: unknown erase bit -> EINVAL");
    chk(iv_factory_reset(NULL, NULL, &fc, 0u, &rep) == IV_EINVAL,
        "f05f: NULL config -> EINVAL");
    chk(iv_factory_erase_bit("media") == IV_FACTORY_ERASE_MEDIA, "f05g: name->bit media");
    chk(iv_factory_erase_bit("MEDIA") == 0u, "f05h: name match is exact");
    chk(strcmp(iv_factory_erase_name(IV_FACTORY_ERASE_DB), "db") == 0, "f05i: bit->name db");
    iv_config_close(cfg);
}

static void f06_purge_db(void)
{
    char                dbp[240];
    iv_db_t            *db  = NULL;
    iv_store_t         *st  = NULL;
    iv_dev_desc_t       d;
    int64_t             removed = -1;
    int64_t             rows    = -1;

    path_of(dbp, sizeof(dbp), "db/control.db");
    chk(iv_db_open(dbp, &db) == IV_OK, "f06a: db open");
    chk(iv_store_open(db, &st) == IV_OK, "f06b: store open");
    if (st == NULL)
        return;

    memset(&d, 0, sizeof(d));
    (void)memcpy(d.mac, "\x02\x00\x00\x00\x00\x01", 6u);
    (void)snprintf(d.model, sizeof(d.model), "%s", "DS-2CD");
    chk(iv_store_device_upsert(st, &d, 100) == IV_OK, "f06c: one device row");
    (void)iv_store_audit_add(st, "test", "detail", 0, 101);
    chk(iv_store_rows(st, "device", &rows) == IV_OK && rows == 1, "f06d: device has 1 row");
    chk(iv_store_rows(st, "audit", &rows) == IV_OK && rows >= 1, "f06e: audit has rows");

    chk(iv_store_purge_all(st, &removed) == IV_OK, "f06f: purge ok");
    chk(removed >= 2, "f06g: purge counted rows");
    chk(iv_store_rows(st, "device", &rows) == IV_OK && rows == 0, "f06h: device empty");
    chk(iv_store_rows(st, "audit", &rows) == IV_OK && rows == 0, "f06i: audit empty");
    chk(iv_store_rows(st, "snmp_latest", &rows) == IV_OK && rows == 0,
        "f06j: snmp_latest empty");
    chk(iv_store_rows(st, "net_event", &rows) == IV_OK && rows == 0, "f06k: net_event empty");

    /* 清完还能继续写：证明只是删行、表结构没坏 */
    chk(iv_store_device_upsert(st, &d, 200) == IV_OK, "f06l: store usable after purge");
    chk(iv_store_purge_all(NULL, &removed) == IV_EINVAL, "f06m: NULL store -> EINVAL");

    iv_store_close(st);
    iv_db_close(db);
}

/* f08：配置写不出去时，**一条数据都不能擦** */
static void f08_order_config_first(void)
{
    char                dir[220];
    char                media[220];
    char                mf[240];
    iv_config_t        *cfg;
    iv_factory_cfg_t    fc;
    iv_factory_report_t rep;
    int                 rc;

    path_of(dir, sizeof(dir), "data4");
    (void)mkdir_one(dir);
    path_of(media, sizeof(media), "media4");
    (void)mkdir_one(media);
    path_of(mf, sizeof(mf), "media4/keep.mp4");
    (void)mkfile(mf, "video");

    cfg = iv_config_open(dir, "ivsbox");
    chk(cfg != NULL, "f08a: config open");
    if (cfg == NULL)
        return;

    /* 把配置目录设成不可写 ⇒ save 的 open(tmp) 必失败。
     * **root 会绕过权限**，所以 root 下这条用例没有区分力，直接跳过。 */
    if (geteuid() == 0) {
        printf("SKIP: running as root, cannot make the config dir read-only\n");
        iv_config_close(cfg);
        return;
    }
    chk(chmod(dir, 0555) == 0, "f08b: chmod config dir 0555");

    memset(&fc, 0, sizeof(fc));
    fc.media_root = media;
    rc            = iv_factory_reset(cfg, NULL, &fc, IV_FACTORY_ERASE_MEDIA, &rep);
    chk(rc != IV_OK, "f08c: reset fails when config cannot be saved");
    chk(rep.cfg_saved == 0, "f08d: report says not saved");
    chk(access(mf, R_OK) == 0, "f08e: media NOT erased when config save failed");

    (void)chmod(dir, 0755);
    iv_config_close(cfg);
}

int main(void)
{
    char   tmpl[] = "/tmp/ivfac_XXXXXX";
    char  *d;
    size_t dl;

    /* 本模块经 iv_config / iv_db / iv_log 落盘：单测里关掉落盘、改走空 sink，
     * 免得在宿主机留下 /mnt/UDISK/log 之类的副作用（与 test_config 同口径）。 */
    iv_log_set_root(NULL);
    iv_log_set_sink(quiet_sink, NULL);

    d = mkdtemp(tmpl);
    if (d == NULL) {
        fprintf(stderr, "FAIL: mkdtemp(): %s\n", strerror(errno));
        return 1;
    }
    dl = strlen(d);
    if (dl + 1u > sizeof(g_root)) {
        fprintf(stderr, "FAIL: temp dir name too long\n");
        return 1;
    }
    (void)memcpy(g_root, d, dl + 1u);

    f01_path_guards();
    f02_wipe_tree();
    f03_reset_config();
    f04_erase_log();
    f05_not_wired();
    f06_purge_db();
    f08_order_config_first();

    rm_rf(g_root);

    if (g_fail != 0) {
        fprintf(stderr, "test_factory failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_factory passed (path guards, recursive wipe w/o symlink follow, "
           "config reset+save+reload, erase log, unwired ENOTSUP, db purge, "
           "config-first ordering)\n");
    return 0;
}
