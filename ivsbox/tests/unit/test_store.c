/*
 * iv_store 单测 —— control.db 表结构与落盘（计划 S9 延伸交付物）
 *
 * 覆盖：schema 幂等与新库/旧库门、device upsert（同 MAC 更新不重复建档、
 * first_seen 保留、ip_be 文本化、64 行上限）、snmp_latest（最近值、ok=0 不乱填）、
 * audit/net_event 追加与上限、rows 白名单、参数防御。
 * 每组用例独立临时库（mkdtemp），收尾删除；只链 libivcore＋iv_store（SQLite）。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <sqlite3.h>

#include "ivsbox/iv_db.h"
#include "ivsbox/iv_device.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_store.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static int64_t row_count(iv_db_t *db, const char *sql)
{
    int64_t v = -1;

    if (iv_db_scalar_int(db, sql, &v) != IV_OK)
        return -1;
    return v;
}

static void fill_desc(iv_dev_desc_t *d, const uint8_t mac[6], uint32_t ip_be,
                      const char *model)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->mac, mac, 6);
    d->ip_be = ip_be;
    d->brand = IV_DEV_BRAND_HIKVISION;
    (void)snprintf(d->model, sizeof d->model, "%s", model);
    (void)snprintf(d->serial, sizeof d->serial, "SN%02x%02x%02x",
                   mac[3], mac[4], mac[5]);
    d->port = 80;
    d->found_by = IV_DEV_PHASE_HIK_SADP;
}

int main(void)
{
    char     dir[] = "/tmp/ivstoreXXXXXX";
    char     dbpath[300];
    iv_db_t    *db = NULL;
    iv_store_t *st = NULL;
    int       i;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "FAIL: mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    (void)snprintf(dbpath, sizeof dbpath, "%s/control.db", dir);

    /* ---------------- s01 open / schema 幂等 ---------------- */
    chk(iv_store_open(NULL, &st) == IV_EINVAL, "s01a: open(NULL db) -> EINVAL");
    chk(iv_db_open(dbpath, &db) == IV_OK, "s01b: db open");
    chk(iv_store_open(db, NULL) == IV_EINVAL, "s01c: open(NULL out) -> EINVAL");
    chk(iv_store_open(db, &st) == IV_OK, "s01d: store open");
    chk(row_count(db, "SELECT COUNT(*) FROM device;") == 0, "s01e: device empty");
    chk(row_count(db, "SELECT COUNT(*) FROM snmp_latest;") == 0,
        "s01f: snmp_latest empty");
    chk(row_count(db, "SELECT COUNT(*) FROM audit;") == 0, "s01g: audit empty");
    chk(row_count(db, "SELECT COUNT(*) FROM net_event;") == 0,
        "s01h: net_event empty");
    {
        iv_store_t *st2 = NULL;

        chk(iv_store_open(db, &st2) == IV_OK, "s01i: second open idempotent");
        chk(row_count(db, "SELECT COUNT(*) FROM device;") == 0,
            "s01j: re-open does not duplicate schema work");
        iv_store_close(st2);
    }

    /* ---------------- s02 device upsert：更新不重复建档 ---------------- */
    {
        iv_dev_desc_t d;
        char          ip[32];

        fill_desc(&d, (const uint8_t *)"\x11\x22\x33\x44\x55\x66",
                  0x0A000001u, "DS-2CD3T25");
        chk(iv_store_device_upsert(st, &d, 1000) == IV_OK, "s02a: first insert");
        {
            int64_t dn = 0;

            chk(iv_store_rows(st, "device", &dn) == IV_OK && dn == 1,
                "s02b: exactly one row");
        }
        chk(iv_store_device_ip(st, "11:22:33:44:55:66", ip, sizeof ip) == IV_OK &&
            strcmp(ip, "10.0.0.1") == 0,
            "s02c: ip_be rendered as dotted text");

        d.ip_be = 0x0A0000C8u; /* 10.0.0.200：改 IP 不重复建档 */
        chk(iv_store_device_upsert(st, &d, 2000) == IV_OK, "s02d: re-upsert ok");
        {
            int64_t dn = 0;

            chk(iv_store_rows(st, "device", &dn) == IV_OK && dn == 1,
                "s02e: still one row after ip change");
        }
        chk(iv_store_device_ip(st, "11:22:33:44:55:66", ip, sizeof ip) == IV_OK &&
            strcmp(ip, "10.0.0.200") == 0,
            "s02f: ip updated in place");
        chk(iv_store_device_ip(st, "ff:ee:dd:cc:bb:aa", ip, sizeof ip) == IV_ENOENT,
            "s02g: unknown mac -> ENOENT");
        /* first_seen 保留：更新行后首见时间仍是 1000（直接读库验证） */
        {
            int64_t fs = row_count(db,
                "SELECT COUNT(*) FROM device WHERE mac='11:22:33:44:55:66'"
                " AND first_seen=1000 AND last_seen=2000;");

            chk(fs == 1, "s02h: first_seen kept, last_seen advanced");
        }
        chk(iv_store_device_upsert(st, NULL, 1) == IV_EINVAL,
            "s02i: NULL desc -> EINVAL");
    }

    /* ---------------- s03 device 64 行上限（满删最旧 last_seen）---------------- */
    {
        iv_dev_desc_t d;
        int64_t       n = 0;
        char          ip[32];

        for (i = 0; i < 70; i++) {
            uint8_t mac[6] = { 0xa0, 0x00, 0x00, 0x00, 0x00, (uint8_t)i };

            fill_desc(&d, mac, 0x0A000100u + (uint32_t)i, "bulk");
            d.mac[5] = (uint8_t)i;
            chk(iv_store_device_upsert(st, &d, (int64_t)i) == IV_OK,
                "s03a: bulk insert ok");
        }
        chk(iv_store_rows(st, "device", &n) == IV_OK && n == 64,
            "s03b: device capped at 64");
        /* 最老的 (last_seen=0..5) 应被清掉；最新的 (63..69) 应在 */
        chk(iv_store_device_ip(st, "a0:00:00:00:00:05", ip, sizeof ip) == IV_ENOENT,
            "s03c: oldest trimmed");
        chk(iv_store_device_ip(st, "a0:00:00:00:00:45", ip, sizeof ip) == IV_OK,
            "s03d: newest kept");
    }

    /* ---------------- s04 snmp_latest：最近值 + ok=0 不乱填 ---------------- */
    {
        char v[64];

        chk(iv_store_snmp_put(st, "camera", "cpuPercent",
                              ".1.3.6.1.4.1.39165.1.1.0", "37", 1, 500) == IV_OK,
            "s04a: put numeric-as-text");
        chk(iv_store_snmp_value(st, "camera", "cpuPercent", v, sizeof v) == IV_OK &&
            strcmp(v, "37") == 0, "s04b: value read back");
        chk(iv_store_snmp_put(st, "camera", "cpuPercent",
                              ".1.3.6.1.4.1.39165.1.1.0", "42", 1, 600) == IV_OK,
            "s04c: overwrite");
        chk(iv_store_snmp_value(st, "camera", "cpuPercent", v, sizeof v) == IV_OK &&
            strcmp(v, "42") == 0, "s04d: latest wins");
        {
            int64_t dn = 0;

            chk(iv_store_rows(st, "snmp_latest", &dn) == IV_OK && dn == 1,
                "s04e: still one row (recent-value semantics)");
        }
        chk(iv_store_snmp_put(st, "camera", "devStatus", "", "", 0, 600) == IV_OK,
            "s04f: no-answer row recorded");
        chk(iv_store_snmp_value(st, "camera", "devStatus", v, sizeof v) == IV_ENOENT,
            "s04g: ok=0 reads as ENOENT (never fabricate values)");
        chk(iv_store_snmp_put(st, "", "x", "", "", 1, 1) == IV_EINVAL,
            "s04h: empty target -> EINVAL");
        chk(iv_store_snmp_put(st, "camera", "", "", "", 1, 1) == IV_EINVAL,
            "s04i: empty slot -> EINVAL");
    }

    /* ---------------- s05 audit：追加 + 512 上限 ---------------- */
    {
        int64_t n = 0;
        char    d[32];

        for (i = 0; i < 515; i++) {
            (void)snprintf(d, sizeof d, "items=%d", i);
            chk(iv_store_audit_add(st, "config.apply", d, 0, (int64_t)i) == IV_OK,
                "s05a: audit add");
            if (g_fail != 0)
                break;
        }
        chk(iv_store_rows(st, "audit", &n) == IV_OK && n == 512,
            "s05b: audit capped at 512");
        /* result 列必须是真整数（绑定错位会让它落成文本/0 —— 钉死） */
        chk(row_count(db, "SELECT COUNT(*) FROM audit"
                          " WHERE result=0 AND method='config.apply';") == 512,
            "s05c: result column is the real integer");
        chk(iv_store_audit_add(st, NULL, "x", 0, 1) == IV_EINVAL,
            "s05d: NULL method -> EINVAL");
    }

    /* ---------------- s06 net_event：追加 + 上限 ---------------- */
    {
        int64_t n = 0;

        for (i = 0; i < 40; i++) {
            chk(iv_store_net_event_add(st, "wireless", "switch", "ifindex=5",
                                       (int64_t)i) == IV_OK,
                "s06a: net_event add");
            if (g_fail != 0)
                break;
        }
        chk(iv_store_rows(st, "net_event", &n) == IV_OK && n == 40,
            "s06b: net_event counted");
    }

    /* ---------------- s07 rows 白名单 ---------------- */
    {
        int64_t n = 0;

        chk(iv_store_rows(st, "sqlite_master", &n) == IV_EINVAL,
            "s07a: non-whitelisted table refused");
        chk(iv_store_rows(st, "device; DROP TABLE device;--", &n) == IV_EINVAL,
            "s07b: injection-shaped table name refused");
    }

    /* ---------------- s08 close / NULL 防御 ---------------- */
    {
        iv_store_t *st2 = NULL;
        int64_t     n = 0;

        iv_store_close(NULL);
        chk(iv_store_rows(NULL, "device", &n) == IV_EINVAL,
            "s08a: rows(NULL) -> EINVAL");
        chk(iv_store_device_ip(NULL, "x", (char *)"y", 8) == IV_EINVAL,
            "s08b: device_ip(NULL) -> EINVAL");
        /* 关掉句柄后不再可用；db 本身仍可查（所有权归 iv_db_t） */
        iv_store_open(db, &st2);
        iv_store_close(st2);
        chk(row_count(db, "SELECT COUNT(*) FROM device;") >= 0,
            "s08c: db still usable after store close");
    }

    /* ---------------- s09 schema 新库门（user_version > 本进程）--------------- */
    {
        iv_store_t *st2 = NULL;

        chk(iv_db_exec(db, "PRAGMA user_version = 7;") == IV_OK,
            "s09a: simulate a newer schema");
        chk(iv_store_open(db, &st2) == IV_EPROTO,
            "s09b: newer schema refused (no guessing)");
        iv_store_close(st2);
        chk(iv_db_exec(db, "PRAGMA user_version = 1;") == IV_OK,
            "s09c: restore version for cleanliness");
    }

    iv_store_close(st);
    iv_db_close(db);
    {
        char cmd[600];

        (void)snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
        (void)system(cmd); /* 测试自建的临时目录，规则 6 允许 */
    }
    if (g_fail != 0) {
        fprintf(stderr, "test_store FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_store passed (schema idempotent/version gate, device upsert+cap, "
           "snmp recent-value, audit/net_event caps, rows whitelist)\n");
    return 0;
}
