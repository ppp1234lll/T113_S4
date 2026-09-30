/*
 * iv_db 单测（libivcore，功能开发计划 M1-S9）
 *
 * 覆盖：open/close、建表、exec、事务回滚、WAL、标量查询、
 *       损坏库恢复（header 非法 -> *.corrupt 重建）、integrity_check。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ivsbox/iv_db.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

static int  g_fail;
static char g_dir[160];

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void path_of(char *out, size_t cap, const char *rel)
{
    size_t dl = strlen(g_dir);
    size_t rl = strlen(rel);

    if (dl + 1u + rl + 1u > cap) {
        chk(0, "path_of: buffer too small");
        out[0] = '\0';
        return;
    }
    memcpy(out, g_dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, rel, rl + 1u);
}

static int write_file(const char *path, const void *data, size_t len)
{
    int    fd  = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    size_t off = 0u;

    if (fd < 0)
        return -1;
    while (off < len) {
        ssize_t n = write(fd, (const char *)data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    (void)close(fd);
    return 0;
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* 在 base 后追加 suffix（如 "-wal" / ".corrupt"），避免 snprintf 的 -Wformat-truncation */
static void path_cat(char *out, size_t cap, const char *base, const char *suffix)
{
    size_t bl = strlen(base);
    size_t sl = strlen(suffix);

    if (bl + sl + 1u > cap) {
        chk(0, "path_cat: buffer too small");
        out[0] = '\0';
        return;
    }
    memcpy(out, base, bl);
    memcpy(out + bl, suffix, sl + 1u);
}

static void quiet_sink(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

/* ---------------------------------------------------------------------------
 * 用例 1：目录不存在时自动创建，能建表、插入、查询
 * ------------------------------------------------------------------------- */
static void case_open_and_basic(void)
{
    char       path[240];
    char       sub[240];
    iv_db_t   *db;
    int64_t    v;

    path_of(sub, sizeof(sub), "sub");
    path_of(path, sizeof(path), "sub/control.db");
    (void)unlink(path);
    (void)rmdir(sub);

    chk(iv_db_open(path, &db) == IV_OK, "d1: open with autocreate");
    chk(db != NULL, "d1: handle not null");

    chk(iv_db_exec(db, "CREATE TABLE t1(id INTEGER PRIMARY KEY, val INTEGER);") == IV_OK,
        "d1: create table");
    chk(iv_db_exec(db, "INSERT INTO t1(val) VALUES(42), (77);") == IV_OK,
        "d1: insert rows");
    chk(iv_db_scalar_int(db, "SELECT COUNT(*) FROM t1;", &v) == IV_OK && v == 2,
        "d1: count rows");

    iv_db_close(db);
    (void)unlink(path);
    (void)rmdir(sub);
}

/* ---------------------------------------------------------------------------
 * 用例 2：事务回滚
 * --------------------------------------------------------------------------- */
static void case_transaction_rollback(void)
{
    char       path[240];
    iv_db_t   *db;
    int64_t    v;

    path_of(path, sizeof(path), "tx.db");
    (void)unlink(path);

    chk(iv_db_open(path, &db) == IV_OK, "d2: open");
    chk(iv_db_exec(db, "CREATE TABLE t2(id INTEGER PRIMARY KEY, val INTEGER);") == IV_OK,
        "d2: create table");
    chk(iv_db_exec(db, "INSERT INTO t2(val) VALUES(1);") == IV_OK,
        "d2: seed row");

    chk(iv_db_begin(db) == IV_OK, "d2: begin");
    chk(iv_db_exec(db, "INSERT INTO t2(val) VALUES(2);") == IV_OK,
        "d2: insert in tx");
    chk(iv_db_rollback(db) == IV_OK, "d2: rollback");

    chk(iv_db_scalar_int(db, "SELECT COUNT(*) FROM t2;", &v) == IV_OK && v == 1,
        "d2: rollback left only seed row");

    iv_db_close(db);
    (void)unlink(path);
}

/* ---------------------------------------------------------------------------
 * 用例 3：WAL 模式生效（-wal 文件出现）
 * --------------------------------------------------------------------------- */
static void case_wal_enabled(void)
{
    char       path[240];
    char       wal[260];
    char       shm[260];
    iv_db_t   *db;

    path_of(path, sizeof(path), "wal.db");
    (void)unlink(path);

    chk(iv_db_open(path, &db) == IV_OK, "d3: open");
    chk(iv_db_exec(db, "CREATE TABLE t3(id INTEGER PRIMARY KEY);") == IV_OK,
        "d3: create table");
    chk(iv_db_exec(db, "INSERT INTO t3 VALUES(1);") == IV_OK,
        "d3: insert");

    path_cat(wal, sizeof(wal), path, "-wal");
    path_cat(shm, sizeof(shm), path, "-shm");
    chk(file_exists(wal), "d3: wal file exists");

    iv_db_close(db);
    (void)unlink(path);
    (void)unlink(wal);
    (void)unlink(shm);
}

/* ---------------------------------------------------------------------------
 * 用例 4：损坏数据库被重命名为 *.corrupt 并重建
 * --------------------------------------------------------------------------- */
static void case_corrupt_recovery(void)
{
    char       path[240];
    char       corrupt[260];
    iv_db_t   *db;
    int64_t    v;
    const char garbage[] = "this is not a sqlite database";

    path_of(path, sizeof(path), "corrupt.db");
    (void)unlink(path);

    chk(write_file(path, garbage, strlen(garbage)) == 0, "d4: write garbage");

    chk(iv_db_open(path, &db) == IV_OK, "d4: open recovers from corrupt file");
    chk(db != NULL, "d4: handle not null after recovery");

    path_cat(corrupt, sizeof(corrupt), path, ".corrupt");
    chk(file_exists(corrupt), "d4: corrupt backup exists");

    chk(iv_db_exec(db, "CREATE TABLE t4(id INTEGER PRIMARY KEY);") == IV_OK,
        "d4: create table in fresh db");
    chk(iv_db_exec(db, "INSERT INTO t4 VALUES(123);") == IV_OK,
        "d4: insert in fresh db");
    chk(iv_db_scalar_int(db, "SELECT id FROM t4;", &v) == IV_OK && v == 123,
        "d4: read back");

    iv_db_close(db);
    (void)unlink(path);
    (void)unlink(corrupt);
}

/* ---------------------------------------------------------------------------
 * 用例 5：integrity_check
 * --------------------------------------------------------------------------- */
static void case_integrity(void)
{
    char       path[240];
    iv_db_t   *db;

    path_of(path, sizeof(path), "integ.db");
    (void)unlink(path);

    chk(iv_db_open(path, &db) == IV_OK, "d5: open");
    chk(iv_db_check_integrity(db) == IV_OK, "d5: integrity ok");

    iv_db_close(db);
    (void)unlink(path);
}

/* ---------------------------------------------------------------------------
 * 用例 6：错误输入
 * --------------------------------------------------------------------------- */
static void case_invalid_inputs(void)
{
    iv_db_t *db = NULL;

    chk(iv_db_open(NULL, NULL) == IV_EINVAL, "d6: open with NULL out");
    chk(iv_db_open("/tmp/ivsbox_test.db", &db) == IV_OK,
        "d6: open default path works");
    chk(iv_db_exec(db, NULL) == IV_EINVAL, "d6: exec NULL sql");
    chk(iv_db_scalar_int(db, "SELECT 1;", NULL) == IV_EINVAL,
        "d6: scalar NULL out");
    chk(iv_db_check_integrity(NULL) == IV_EINVAL, "d6: integrity NULL db");
    iv_db_close(db);
    (void)unlink("/tmp/ivsbox_test.db");
}

int main(void)
{
    char   tmpl[] = "/tmp/ivdb_XXXXXX";
    char  *d;
    size_t dl;

    iv_log_set_root(NULL);
    iv_log_set_sink(quiet_sink, NULL);

    d = mkdtemp(tmpl);
    if (d == NULL) {
        fprintf(stderr, "FAIL: mkdtemp(): %s\n", strerror(errno));
        return 1;
    }
    dl = strlen(d);
    if (dl + 1u > sizeof(g_dir)) {
        fprintf(stderr, "FAIL: temp dir name too long\n");
        return 1;
    }
    memcpy(g_dir, d, dl + 1u);

    case_open_and_basic();
    case_transaction_rollback();
    case_wal_enabled();
    case_corrupt_recovery();
    case_integrity();
    case_invalid_inputs();

    (void)rmdir(g_dir);

    if (g_fail != 0) {
        fprintf(stderr, "test_db failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_db passed (open, tx, wal, corrupt recovery, integrity, invalid)\n");
    return 0;
}
