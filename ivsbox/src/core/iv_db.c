/*
 * IVSBox SQLite 单写者封装实现（M1-S9）
 */
#include "ivsbox/iv_db.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sqlite3.h>

#define IV_DB_MOD "db"

/* SQLite 文件魔数（16 字节，含结尾 \0） */
static const char s_sqlite_magic[16] = "SQLite format 3\0";

struct iv_db {
    sqlite3 *handle;
};

/* ---------------------------------------------------------------------------
 * 小工具：逐级建目录
 * ------------------------------------------------------------------------- */
static int mkdir_p(const char *path)
{
    char  buf[512];
    char *p;
    size_t len;

    len = strlen(path);
    if (len == 0 || len >= sizeof(buf))
        return -1;

    memcpy(buf, path, len + 1u);

    /* 去掉末尾斜杠 */
    while (len > 1u && buf[len - 1u] == '/')
        buf[--len] = '\0';

    for (p = buf + 1u; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

/* ---------------------------------------------------------------------------
 * header 校验：文件存在且可识别为 SQLite 数据库
 * ------------------------------------------------------------------------- */
static int header_valid(const char *path)
{
    int    fd;
    char   hdr[16];
    ssize_t n;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        /* 不存在或打不开 => 不算“header 损坏”，让 SQLite 自己处理 */
        return (errno == ENOENT) ? 1 : 0;
    }

    n = read(fd, hdr, sizeof(hdr));
    (void)close(fd);

    if (n < 0)
        return 0;
    if (n == 0)
        return 0; /* 空文件需要重建 */

    return (memcmp(hdr, s_sqlite_magic, sizeof(s_sqlite_magic)) == 0) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * 把损坏的数据库重命名为 *.corrupt
 * ------------------------------------------------------------------------- */
static int backup_corrupt(const char *path)
{
    char  dst[512];
    size_t len;

    len = strlen(path);
    if (len + 8u >= sizeof(dst)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(dst, path, len);
    memcpy(dst + len, ".corrupt", 9u); /* 8 字符 + NUL */

    (void)unlink(dst);
    if (rename(path, dst) != 0)
        return -1;

    IV_LOG_W(IV_DB_MOD, "database corrupt, moved to %s", dst);
    return 0;
}

/* ---------------------------------------------------------------------------
 * SQLite 返回码 -> IV 返回码
 * ------------------------------------------------------------------------- */
static int db_rc(int rc)
{
    switch (rc) {
    case SQLITE_OK:
        return IV_OK;
    case SQLITE_NOMEM:
        return IV_ENOMEM;
    case SQLITE_BUSY:
        return IV_EBUSY;
    case SQLITE_CONSTRAINT:
        return IV_EEXIST;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        return IV_ECORRUPT;
    default:
        return IV_EIO;
    }
}

/* ---------------------------------------------------------------------------
 * 内部：真正尝试打开（不处理 corruption 恢复）
 * ------------------------------------------------------------------------- */
static int do_open(const char *path, sqlite3 **out)
{
    int rc;

    rc = sqlite3_open_v2(path, out,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         NULL);
    if (rc != SQLITE_OK) {
        IV_LOG_E(IV_DB_MOD, "sqlite3_open_v2 failed: %s",
                 sqlite3_errstr(rc));
        return db_rc(rc);
    }

    rc = sqlite3_busy_timeout(*out, IV_DB_BUSY_TIMEOUT_MS);
    if (rc != SQLITE_OK) {
        IV_LOG_E(IV_DB_MOD, "sqlite3_busy_timeout failed: %s",
                 sqlite3_errmsg(*out));
        sqlite3_close(*out);
        *out = NULL;
        return IV_EIO;
    }

    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 打开封装：带目录创建、WAL、损坏恢复
 * ------------------------------------------------------------------------- */
int iv_db_open(const char *path, iv_db_t **out)
{
    const char *p;
    iv_db_t    *db;
    char        dir_buf[512];
    int         rc;

    if (out == NULL)
        return IV_EINVAL;
    *out = NULL;

    p = (path != NULL && path[0] != '\0') ? path : IV_DB_PATH_DEFAULT;

    /* 建目录 */
    {
        const char *slash = strrchr(p, '/');
        size_t      dlen;

        if (slash == NULL || slash == p) {
            /* 没有目录或根目录，不建 */
            dir_buf[0] = '\0';
        } else {
            dlen = (size_t)(slash - p);
            if (dlen >= sizeof(dir_buf))
                return IV_EINVAL;
            memcpy(dir_buf, p, dlen);
            dir_buf[dlen] = '\0';
            if (mkdir_p(dir_buf) != 0) {
                IV_LOG_E(IV_DB_MOD, "failed to create db dir %s: %s",
                         dir_buf, strerror(errno));
                return IV_EIO;
            }
        }
    }

    db = (iv_db_t *)malloc(sizeof(*db));
    if (db == NULL)
        return IV_ENOMEM;
    db->handle = NULL;

    /* 先做一次 header 校验：header 坏就提前备份 */
    if (!header_valid(p)) {
        struct stat st;

        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            if (backup_corrupt(p) != 0) {
                IV_LOG_E(IV_DB_MOD, "failed to backup corrupt db %s: %s",
                         p, strerror(errno));
                free(db);
                return IV_EIO;
            }
        }
    }

    rc = do_open(p, &db->handle);

    /* 如果 SQLite 自己也报损坏，再尝试一次恢复 */
    if (rc == IV_ECORRUPT && db->handle != NULL) {
        sqlite3_close(db->handle);
        db->handle = NULL;
        if (backup_corrupt(p) != 0) {
            free(db);
            return IV_EIO;
        }
        rc = do_open(p, &db->handle);
    }

    if (rc != IV_OK) {
        if (db->handle != NULL)
            sqlite3_close(db->handle);
        free(db);
        return rc;
    }

    /* 启用 WAL */
    rc = sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        IV_LOG_E(IV_DB_MOD, "failed to enable WAL: %s",
                 sqlite3_errmsg(db->handle));
        sqlite3_close(db->handle);
        free(db);
        return IV_EIO;
    }

    *out = db;
    return IV_OK;
}

void iv_db_close(iv_db_t *db)
{
    if (db == NULL)
        return;
    if (db->handle != NULL)
        sqlite3_close(db->handle);
    free(db);
}

int iv_db_exec(iv_db_t *db, const char *sql)
{
    int rc;

    if (db == NULL || db->handle == NULL || sql == NULL)
        return IV_EINVAL;

    rc = sqlite3_exec(db->handle, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        IV_LOG_E(IV_DB_MOD, "exec failed (%s): %s", sql,
                 sqlite3_errmsg(db->handle));
        return db_rc(rc);
    }
    return IV_OK;
}

int iv_db_scalar_int(iv_db_t *db, const char *sql, int64_t *out)
{
    sqlite3_stmt *stmt;
    int           rc;
    int           ret = IV_OK;

    if (db == NULL || db->handle == NULL || sql == NULL || out == NULL)
        return IV_EINVAL;

    rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        IV_LOG_E(IV_DB_MOD, "prepare failed (%s): %s", sql,
                 sqlite3_errmsg(db->handle));
        return db_rc(rc);
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int t = sqlite3_column_type(stmt, 0);
        if (t == SQLITE_INTEGER)
            *out = sqlite3_column_int64(stmt, 0);
        else
            ret = IV_EPROTO;
    } else if (rc == SQLITE_DONE) {
        ret = IV_ENOENT;
    } else {
        ret = db_rc(rc);
    }

    sqlite3_finalize(stmt);
    return ret;
}

int iv_db_check_integrity(iv_db_t *db)
{
    sqlite3_stmt *stmt;
    int           rc;
    int           ret = IV_OK;

    if (db == NULL || db->handle == NULL)
        return IV_EINVAL;

    rc = sqlite3_prepare_v2(db->handle, "PRAGMA integrity_check;", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return db_rc(rc);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *txt = (const char *)sqlite3_column_text(stmt, 0);
        if (txt == NULL || strcmp(txt, "ok") != 0)
            ret = IV_ECORRUPT;
    } else if (rc == SQLITE_DONE) {
        ret = IV_ECORRUPT;
    } else {
        ret = db_rc(rc);
    }

    sqlite3_finalize(stmt);
    return ret;
}
