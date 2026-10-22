/*
 * IVSBox control.db 表结构与落盘实现（计划 S9 延伸交付物）
 *
 * 写路径全部走 **sqlite3_bind**（不经拼串）：device 的 model/serial、audit 的
 * detail、net_event 的文本都来自报文/外部输入，拼串 = 注入面。只读出口的
 * 表名走白名单（iv_store_rows）。
 * 追加型表（audit/net_event）每次追加后顺手清到上限内 —— 一条 DELETE，量级
 * 只有几百行，UDISK 上可忽略；不另起"维护线程"（单写者原则下线程只会添乱）。
 */
#include "ivsbox/iv_store.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

#define ST_MOD "store"

#define IV_STORE_SCHEMA_VERSION 1

struct iv_store {
    sqlite3 *db; /* 借用；所有权归 iv_db_t */
};

/* ---------------------------------------------------------------------------
 * schema
 * ------------------------------------------------------------------------- */
static const char *const s_schema_sql[] = {
    /* S4.2 发现档案：MAC 主键；IP 是可变属性（改 IP 不重复建档） */
    "CREATE TABLE IF NOT EXISTS device ("
    " mac        TEXT PRIMARY KEY,"
    " ip         TEXT NOT NULL DEFAULT '',"
    " brand      INTEGER NOT NULL DEFAULT 0,"
    " model      TEXT NOT NULL DEFAULT '',"
    " serial     TEXT NOT NULL DEFAULT '',"
    " port       INTEGER NOT NULL DEFAULT 0,"
    " found_by   INTEGER NOT NULL DEFAULT 0,"
    " first_seen INTEGER NOT NULL DEFAULT 0,"
    " last_seen  INTEGER NOT NULL DEFAULT 0);",

    /* S4.3 采集结果：最近值语义（REPLACE 覆盖） */
    "CREATE TABLE IF NOT EXISTS snmp_latest ("
    " target TEXT NOT NULL,"
    " slot   TEXT NOT NULL,"
    " oid    TEXT NOT NULL DEFAULT '',"
    " value  TEXT NOT NULL DEFAULT '',"
    " ok     INTEGER NOT NULL DEFAULT 0,"
    " ts     INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY (target, slot));",

    /* S5.2 写操作审计 */
    "CREATE TABLE IF NOT EXISTS audit ("
    " id     INTEGER PRIMARY KEY AUTOINCREMENT,"
    " ts     INTEGER NOT NULL DEFAULT 0,"
    " method TEXT NOT NULL,"
    " detail TEXT NOT NULL DEFAULT '',"
    " result INTEGER NOT NULL DEFAULT 0);",

    /* 网络历史 */
    "CREATE TABLE IF NOT EXISTS net_event ("
    " id     INTEGER PRIMARY KEY AUTOINCREMENT,"
    " ts     INTEGER NOT NULL DEFAULT 0,"
    " wan    TEXT NOT NULL DEFAULT '',"
    " event  TEXT NOT NULL DEFAULT '',"
    " detail TEXT NOT NULL DEFAULT '');",
};

static int schema_ensure(sqlite3 *db)
{
    int64_t ver = 0;
    int     rc;
    size_t  i;

    {
        sqlite3_stmt *stmt;

        rc = sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return IV_EIO;
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
            ver = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_ROW)
            return IV_EIO;
    }
    if (ver > IV_STORE_SCHEMA_VERSION) {
        IV_LOG_E(ST_MOD, "control.db schema v%lld is newer than this build "
                         "(v%d) -- refusing to touch it",
                 (long long)ver, IV_STORE_SCHEMA_VERSION);
        return IV_EPROTO; /* 库比程序新：不猜、不降级读写 */
    }
    if (ver == IV_STORE_SCHEMA_VERSION)
        return IV_OK; /* 快路径：已就位 */

    /* ver==0：新库，建表并落版本号（幂等：表已存在也不会再建坏） */
    for (i = 0; i < sizeof(s_schema_sql) / sizeof(s_schema_sql[0]); i++) {
        char *err = NULL;

        if (sqlite3_exec(db, s_schema_sql[i], NULL, NULL, &err) != SQLITE_OK) {
            IV_LOG_E(ST_MOD, "schema exec failed: %s", (err != NULL) ? err : "?");
            sqlite3_free(err);
            return IV_EIO;
        }
        sqlite3_free(err);
    }
    {
        char setver[64];

        (void)snprintf(setver, sizeof setver, "PRAGMA user_version = %d;",
                       IV_STORE_SCHEMA_VERSION);
        if (sqlite3_exec(db, setver, NULL, NULL, NULL) != SQLITE_OK)
            return IV_EIO;
    }
    IV_LOG_I(ST_MOD, "control.db schema created (v%d)", IV_STORE_SCHEMA_VERSION);
    return IV_OK;
}

int iv_store_open(iv_db_t *db, iv_store_t **out)
{
    iv_store_t *st;
    int         rc;

    if (out == NULL)
        return IV_EINVAL;
    *out = NULL;
    if (db == NULL)
        return IV_EINVAL;
    if (iv_db_handle(db) == NULL)
        return IV_EINVAL;

    st = (iv_store_t *)calloc(1u, sizeof(*st));
    if (st == NULL)
        return IV_ENOMEM;
    st->db = (sqlite3 *)iv_db_handle(db);

    rc = schema_ensure(st->db);
    if (rc != IV_OK) {
        free(st);
        return rc;
    }
    *out = st;
    return IV_OK;
}

void iv_store_close(iv_store_t *st)
{
    if (st == NULL)
        return;
    free(st); /* 不关 db：所有权归 iv_db_t */
}

/* ---------------------------------------------------------------------------
 * 内部小工具
 * ------------------------------------------------------------------------- */
/* 追加/档案表的"保最近 rows_max 行"清理。
 * ⚠ device 表主键是 mac、**没有 id 列** —— 键列与排序列必须按表给，不能一刀切
 * （首轮单测当场抓出：device 的 DELETE 引用 id ⇒ SQL 错 ⇒ 全部 upsert 假败）。 */
static int trim_table(sqlite3 *db, const char *table, const char *keycol,
                      const char *ordercol, int rows_max)
{
    char sql[192];

    (void)snprintf(sql, sizeof sql,
                   "DELETE FROM %s WHERE %s NOT IN"
                   " (SELECT %s FROM %s ORDER BY %s DESC LIMIT %d);",
                   table, keycol, keycol, table, ordercol, rows_max);
    return (sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK) ? IV_OK : IV_EIO;
}

/* 安全文本：NULL/空串与非法长度统一处理，返回可 bind 的指针（恒 NUL 结尾）。 */
static const char *txt(const char *s)
{
    return (s != NULL) ? s : "";
}

/* ---------------------------------------------------------------------------
 * device（S4.2）
 * ------------------------------------------------------------------------- */
static void mac_to_text(const uint8_t mac[6], char *out, size_t cap)
{
    (void)snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int iv_store_device_upsert(iv_store_t *st, const iv_dev_desc_t *d, int64_t now_s)
{
    sqlite3_stmt *stmt = NULL;
    char          mactxt[18];
    char          iptxt[INET_ADDRSTRLEN];
    int           rc;

    if (st == NULL || d == NULL)
        return IV_EINVAL;
    mac_to_text(d->mac, mactxt, sizeof mactxt);

    /* IP 文本化（R-04）：`ip_be` 的语义是 **`in_addr.s_addr` 的值**（内存里那
     * 4 个字节就是网络序），不是"把点分十进制当整数看"。原先按
     * `ip_be >> 24` 取首字节，在小端板上等于**从最后一个字节开始读** ——
     * control.db 里 192.168.2.102 被写成 `102.2.168.192`、192.168.1.200 写成
     * `200.1.168.192`（板上实测四条全反）。内存里的 Web 走 inet_ntop 所以是对的，
     * 于是"页面正确、库里错序"，只看页面永远发现不了。
     * 统一交给 inet_ntop：它对 `&s_addr` 的解释就是"4 个网络序字节"，跨端序都对。 */
    iptxt[0] = '\0';
    if (d->ip_be != 0u)
        (void)inet_ntop(AF_INET, &d->ip_be, iptxt, sizeof iptxt);

    /* UPDATE 优先、改不动（无此行）再 INSERT —— 不用 UPSERT 语法（板端 SQLite
     * 版本未知，UPSERT 要 ≥3.24）；first_seen 只在 INSERT 时写，UPDATE 分支
     * 天然不碰它（"同 MAC 改 IP ＝ 更新不重复建档"，first_seen 保留）。 */
    if (sqlite3_prepare_v2(st->db,
                           "UPDATE device SET ip = ?2, brand = ?3, model = ?4,"
                           " serial = ?5, port = ?6, found_by = ?7,"
                           " last_seen = ?8 WHERE mac = ?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return IV_EIO;
    (void)sqlite3_bind_text(stmt, 1, mactxt, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(stmt, 2, iptxt, -1, SQLITE_STATIC);
    (void)sqlite3_bind_int(stmt, 3, (int)d->brand);
    (void)sqlite3_bind_text(stmt, 4, txt(d->model), -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(stmt, 5, txt(d->serial), -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int(stmt, 6, (int)d->port);
    (void)sqlite3_bind_int(stmt, 7, (int)d->found_by);
    (void)sqlite3_bind_int64(stmt, 8, now_s);
    rc = (sqlite3_step(stmt) == SQLITE_DONE) ? IV_OK : IV_EIO;
    sqlite3_finalize(stmt);
    if (rc != IV_OK)
        return rc;
    if (sqlite3_changes(st->db) != 0) {
        return trim_table(st->db, "device", "mac", "last_seen",
                          IV_STORE_DEVICE_ROWS_MAX);
    }

    /* 无此行 ⇒ 建档 */
    if (sqlite3_prepare_v2(st->db,
                           "INSERT INTO device"
                           " (mac, ip, brand, model, serial, port, found_by,"
                           "  first_seen, last_seen)"
                           " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?8);",
                           -1, &stmt, NULL) != SQLITE_OK)
        return IV_EIO;
    (void)sqlite3_bind_text(stmt, 1, mactxt, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(stmt, 2, iptxt, -1, SQLITE_STATIC);
    (void)sqlite3_bind_int(stmt, 3, (int)d->brand);
    (void)sqlite3_bind_text(stmt, 4, txt(d->model), -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(stmt, 5, txt(d->serial), -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int(stmt, 6, (int)d->port);
    (void)sqlite3_bind_int(stmt, 7, (int)d->found_by);
    (void)sqlite3_bind_int64(stmt, 8, now_s);
    rc = (sqlite3_step(stmt) == SQLITE_DONE) ? IV_OK : IV_EIO;
    sqlite3_finalize(stmt);
    if (rc != IV_OK)
        return rc;
    return trim_table(st->db, "device", "mac", "last_seen",
                       IV_STORE_DEVICE_ROWS_MAX);
}

int iv_store_device_ip(iv_store_t *st, const char *mac, char *out, size_t cap)
{
    sqlite3_stmt *stmt = NULL;
    int           rc;

    if (st == NULL || mac == NULL || out == NULL || cap == 0u)
        return IV_EINVAL;
    out[0] = '\0';
    if (sqlite3_prepare_v2(st->db, "SELECT ip FROM device WHERE mac = ?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return IV_EIO;
    (void)sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *ip = (const char *)sqlite3_column_text(stmt, 0);

        (void)snprintf(out, cap, "%s", (ip != NULL) ? ip : "");
        rc = IV_OK;
    } else if (rc == SQLITE_DONE) {
        rc = IV_ENOENT;
    } else {
        rc = IV_EIO;
    }
    sqlite3_finalize(stmt);
    return rc;
}

/* ---------------------------------------------------------------------------
 * snmp_latest（S4.3）
 * ------------------------------------------------------------------------- */
int iv_store_snmp_put(iv_store_t *st, const char *target, const char *slot,
                      const char *oid, const char *value, int ok, int64_t ts)
{
    sqlite3_stmt *stmt = NULL;
    int           rc;

    if (st == NULL || target == NULL || slot == NULL)
        return IV_EINVAL;
    if (target[0] == '\0' || slot[0] == '\0')
        return IV_EINVAL;

    if (sqlite3_prepare_v2(st->db,
                           "INSERT OR REPLACE INTO snmp_latest"
                           " (target, slot, oid, value, ok, ts)"
                           " VALUES (?1, ?2, ?3, ?4, ?5, ?6);",
                           -1, &stmt, NULL) != SQLITE_OK)
        return IV_EIO;
    (void)sqlite3_bind_text(stmt, 1, target, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(stmt, 2, slot, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(stmt, 3, txt(oid), -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(stmt, 4, (ok != 0) ? txt(value) : "", -1,
                            SQLITE_TRANSIENT);
    (void)sqlite3_bind_int(stmt, 5, (ok != 0) ? 1 : 0);
    (void)sqlite3_bind_int64(stmt, 6, ts);
    rc = (sqlite3_step(stmt) == SQLITE_DONE) ? IV_OK : IV_EIO;
    sqlite3_finalize(stmt);
    return rc;
}

int iv_store_snmp_value(iv_store_t *st, const char *target, const char *slot,
                        char *out, size_t cap)
{
    sqlite3_stmt *stmt = NULL;
    int           rc;

    if (st == NULL || target == NULL || slot == NULL || out == NULL || cap == 0u)
        return IV_EINVAL;
    out[0] = '\0';
    if (sqlite3_prepare_v2(st->db,
                           "SELECT value FROM snmp_latest"
                           " WHERE target = ?1 AND slot = ?2 AND ok = 1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return IV_EIO;
    (void)sqlite3_bind_text(stmt, 1, target, -1, SQLITE_STATIC);
    (void)sqlite3_bind_text(stmt, 2, slot, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);

        (void)snprintf(out, cap, "%s", (v != NULL) ? v : "");
        rc = IV_OK;
    } else if (rc == SQLITE_DONE) {
        rc = IV_ENOENT;
    } else {
        rc = IV_EIO;
    }
    sqlite3_finalize(stmt);
    return rc;
}

/* ---------------------------------------------------------------------------
 * audit（S5.2）与 net_event
 * ------------------------------------------------------------------------- */
/* 占位符与绑定必须一一对应（首轮单测的教训：绑了个不存在的 ?5 还嫌它"无害"
 * —— audit 的 result 列因此一直落到文本参数上，错误被静默）。 */
static int audit_insert(iv_store_t *st, const char *method, const char *detail,
                        int result, int64_t ts)
{
    sqlite3_stmt *stmt = NULL;
    int           rc;

    if (sqlite3_prepare_v2(st->db,
                           "INSERT INTO audit (ts, method, detail, result)"
                           " VALUES (?1, ?2, ?3, ?4);",
                           -1, &stmt, NULL) != SQLITE_OK)
        return IV_EIO;
    (void)sqlite3_bind_int64(stmt, 1, ts);
    (void)sqlite3_bind_text(stmt, 2, txt(method), -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(stmt, 3, txt(detail), -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int(stmt, 4, result);
    rc = (sqlite3_step(stmt) == SQLITE_DONE) ? IV_OK : IV_EIO;
    sqlite3_finalize(stmt);
    return rc;
}

static int net_event_insert(iv_store_t *st, const char *wan, const char *event,
                            const char *detail, int64_t ts)
{
    sqlite3_stmt *stmt = NULL;
    int           rc;

    if (sqlite3_prepare_v2(st->db,
                           "INSERT INTO net_event (ts, wan, event, detail)"
                           " VALUES (?1, ?2, ?3, ?4);",
                           -1, &stmt, NULL) != SQLITE_OK)
        return IV_EIO;
    (void)sqlite3_bind_int64(stmt, 1, ts);
    (void)sqlite3_bind_text(stmt, 2, txt(wan), -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(stmt, 3, txt(event), -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(stmt, 4, txt(detail), -1, SQLITE_TRANSIENT);
    rc = (sqlite3_step(stmt) == SQLITE_DONE) ? IV_OK : IV_EIO;
    sqlite3_finalize(stmt);
    return rc;
}

int iv_store_audit_add(iv_store_t *st, const char *method, const char *detail,
                       int result, int64_t ts)
{
    int rc;

    if (st == NULL || method == NULL)
        return IV_EINVAL;
    rc = audit_insert(st, method, detail, result, ts);
    if (rc != IV_OK)
        return rc;
    return trim_table(st->db, "audit", "id", "id", IV_STORE_AUDIT_ROWS_MAX);
}

int iv_store_net_event_add(iv_store_t *st, const char *wan, const char *event,
                           const char *detail, int64_t ts)
{
    int rc;

    if (st == NULL || wan == NULL || event == NULL)
        return IV_EINVAL;
    rc = net_event_insert(st, wan, event, detail, ts);
    if (rc != IV_OK)
        return rc;
    return trim_table(st->db, "net_event", "id", "id", IV_STORE_NET_ROWS_MAX);
}

int iv_store_rows(iv_store_t *st, const char *table, int64_t *out)
{
    static const char *const k_tables[] = {
        "device", "snmp_latest", "audit", "net_event",
    };
    char sql[64];
    size_t i;
    sqlite3_stmt *stmt = NULL;
    int           rc;

    if (st == NULL || table == NULL || out == NULL)
        return IV_EINVAL;
    for (i = 0; i < sizeof k_tables / sizeof k_tables[0]; i++) {
        if (strcmp(table, k_tables[i]) == 0)
            break;
    }
    if (i >= sizeof k_tables / sizeof k_tables[0])
        return IV_EINVAL; /* 白名单外一律拒：绝不拼进 SQL */

    (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s;", table);
    if (sqlite3_prepare_v2(st->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return IV_EIO;
    rc = sqlite3_step(stmt);
    *out = (rc == SQLITE_ROW) ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? IV_OK : IV_EIO;
}
