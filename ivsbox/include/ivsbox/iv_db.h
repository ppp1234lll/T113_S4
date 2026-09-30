/*
 * IVSBox SQLite 单写者封装（M1-S9）
 *
 * 职责边界：
 *   - 只给 `ivsboxd` 主控直接打开 `control.db`，Web / 其他模块通过本地通道查询，
 *     不直接读写数据库（架构 §10.4 单写者原则）。
 *   - 库文件默认落 `/mnt/UDISK/ivsbox/db/control.db`（eMMC UDISK 持久分区）。
 *   - 打开时自动建目录、开 WAL、设 busy_timeout；遇到 header 损坏时把原库改名
 *     `*.corrupt` 再重建，不覆盖现场。
 *   - 本层只提供最常用封装：open/close、exec、整型标量、完整性检查、事务宏。
 *     需要复杂查询的模块可自行 include <sqlite3.h>，但 `sqlite3_open_v2` 建议只由
 *     本封装调用，避免所有权混乱。
 */
#ifndef IVSBOX_IV_DB_H
#define IVSBOX_IV_DB_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 默认库路径 */
#ifndef IV_DB_PATH_DEFAULT
#define IV_DB_PATH_DEFAULT "/mnt/UDISK/ivsbox/db/control.db"
#endif

/* 默认 busy timeout（毫秒） */
#ifndef IV_DB_BUSY_TIMEOUT_MS
#define IV_DB_BUSY_TIMEOUT_MS 5000
#endif

/* 数据库句柄（不透明） */
typedef struct iv_db iv_db_t;

/*
 * 打开或创建数据库。
 *   path 为 NULL 时使用 IV_DB_PATH_DEFAULT；
 *   目录不存在时自动创建；
 *   文件 header 非法或 SQLite 报损坏时，原文件被重命名为 `*.corrupt`，
 *   然后新建空库；
 *   成功后会启用 WAL 并设置 busy_timeout。
 * 失败返回 IV_EINVAL / IV_EIO / IV_ENOMEM 等。
 */
int iv_db_open(const char *path, iv_db_t **out);

/* 关闭数据库。NULL 安全。 */
void iv_db_close(iv_db_t *db);

/*
 * 执行一条 SQL（DDL / DML 均可）。
 * 事务宏内部就是调用它；busy_timeout 会自动处理 SQLITE_BUSY。
 */
int iv_db_exec(iv_db_t *db, const char *sql);

/*
 * 执行查询并取第一行第一列的整数值。
 * 没有行返回 => IV_ENOENT；类型不是整数 => IV_EPROTO。
 */
int iv_db_scalar_int(iv_db_t *db, const char *sql, int64_t *out);

/* 运行 PRAGMA integrity_check，返回 IV_OK 表示通过。 */
int iv_db_check_integrity(iv_db_t *db);

/* 短事务 helper */
#define iv_db_begin(db)    iv_db_exec((db), "BEGIN IMMEDIATE;")
#define iv_db_commit(db)   iv_db_exec((db), "COMMIT;")
#define iv_db_rollback(db) iv_db_exec((db), "ROLLBACK;")

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_DB_H */
