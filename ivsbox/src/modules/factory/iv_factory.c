/*
 * 恢复出厂 / 擦除存储实现。口径、范围与安全约束见 include/ivsbox/iv_factory.h。
 *
 * 本文件只做两件事：调 `iv_config_reset_defaults()` 回默认并落盘、按根路径递归
 * 删条目。**不认识口令**（归 ivsbox-web）、**不重启设备**（归调用方）。
 */
#include "ivsbox/iv_factory.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"

#define FAC_MOD "factory"

const char *iv_factory_erase_name(unsigned bit)
{
    switch (bit) {
    case IV_FACTORY_ERASE_MEDIA: return "media";
    case IV_FACTORY_ERASE_LOG:   return "log";
    case IV_FACTORY_ERASE_DB:    return "db";
    default:                     return NULL;
    }
}

unsigned iv_factory_erase_bit(const char *name)
{
    if (name == NULL)
        return 0u;
    if (strcmp(name, "media") == 0)
        return IV_FACTORY_ERASE_MEDIA;
    if (strcmp(name, "log") == 0)
        return IV_FACTORY_ERASE_LOG;
    if (strcmp(name, "db") == 0)
        return IV_FACTORY_ERASE_DB;
    return 0u;
}

/* ---------------------------------------------------------------------------
 * 根路径护栏：这里每一条都是防"把系统删了"
 * ------------------------------------------------------------------------- */
static int root_ok(const char *r)
{
    size_t n;

    if (r == NULL)
        return 0;
    n = strlen(r);
    if (n < 2u)                    /* "" 与 "/" 都不行 */
        return 0;
    if (r[0] != '/')
        return 0;                  /* 必须是绝对路径：相对路径会跟着 cwd 跑 */
    if (strstr(r, "..") != NULL)
        return 0;                  /* 不接受任何上跳 */
    if (strcmp(r, "/") == 0)
        return 0;
    return 1;
}

/* 删 `dir` 下的全部条目；`dir` 本身不动。`removed` 计文件+目录条数。 */
static int wipe_under(const char *dir, unsigned depth, unsigned long *removed)
{
    DIR           *d;
    struct dirent *e;

    if (depth > IV_FACTORY_WIPE_DEPTH_MAX)
        return IV_ERANGE;

    d = opendir(dir);
    if (d == NULL)
        return (errno == ENOENT) ? IV_ENOENT : IV_EIO;

    while ((e = readdir(d)) != NULL) {
        char        path[IV_FACTORY_PATH_MAX];
        struct stat st;
        int         n;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            IV_LOG_W(FAC_MOD, "skip too-long path under %s", dir);
            continue;
        }
        /* lstat 而不是 stat：符号链接只 unlink、绝不跟进去 —— 一个指向 "/" 的
         * 链接就能把整机删干净，这条不是洁癖。 */
        if (lstat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            int rc = wipe_under(path, depth + 1u, removed);

            if (rc == IV_OK || rc == IV_ENOENT) {
                if (rmdir(path) == 0 && removed != NULL)
                    (*removed)++;
            } else {
                IV_LOG_W(FAC_MOD, "wipe %s failed (rc=%d), keep it", path, rc);
            }
        } else {
            if (unlink(path) == 0 && removed != NULL)
                (*removed)++;
        }
    }
    (void)closedir(d);
    return IV_OK;
}

int iv_factory_wipe(const char *root, unsigned long *removed)
{
    int rc;

    if (!root_ok(root)) {
        /* 参数错不是"没东西可删"：静默返回 OK 会让"用户以为擦干净了" */
        IV_LOG_E(FAC_MOD, "refuse to wipe '%s' (path guard)", (root != NULL) ? root : "(null)");
        return IV_EINVAL;
    }
    rc = wipe_under(root, 0u, removed);
    /* 根不存在 = 这台机器上没配这一项（例如没插 SD 卡），不算错误 */
    return (rc == IV_ENOENT) ? IV_OK : rc;
}

/* ---------------------------------------------------------------------------
 * 恢复出厂
 * ------------------------------------------------------------------------- */
int iv_factory_reset(iv_config_t *cfg, iv_store_t *store, const iv_factory_cfg_t *fc,
                     unsigned erase_mask, iv_factory_report_t *rep)
{
    iv_factory_report_t local;
    char                detail[160];
    int                 rc;

    memset(&local, 0, sizeof(local));
    if (rep != NULL)
        *rep = local;

    if (cfg == NULL)
        return IV_EINVAL;
    if ((erase_mask & ~IV_FACTORY_ERASE_ALL) != 0u)
        return IV_EINVAL;
    /* 先判根：真到了擦除阶段才发现"没有根"就晚了（配置已经复位） */
    if ((erase_mask & IV_FACTORY_ERASE_MEDIA) != 0u &&
        (fc == NULL || !root_ok(fc->media_root)))
        return IV_ENOTSUP;
    if ((erase_mask & IV_FACTORY_ERASE_LOG) != 0u && (fc == NULL || !root_ok(fc->data_root)))
        return IV_ENOTSUP;
    if ((erase_mask & IV_FACTORY_ERASE_DB) != 0u && store == NULL)
        return IV_ENOTSUP;

    rc = iv_config_reset_defaults(cfg, &local.cfg_changed);
    if (rc != IV_OK)
        return rc;

    detail[0] = '\0';
    rc        = iv_config_save(cfg, detail, sizeof(detail));
    local.cfg_version = iv_config_version(cfg);
    local.cfg_saved   = (rc == IV_OK) ? 1 : 0;
    if (rep != NULL)
        *rep = local;
    if (rc != IV_OK) {
        /* 配置没落盘就**一条数据都不擦**：半成品（配置没复位、录像却没了）最坏 */
        IV_LOG_E(FAC_MOD, "reset aborted: config save failed rc=%d (%s)", rc,
                 (detail[0] != '\0') ? detail : "-");
        return rc;
    }

    if ((erase_mask & IV_FACTORY_ERASE_MEDIA) != 0u) {
        unsigned long before = local.entries_removed;

        rc = iv_factory_wipe(fc->media_root, &local.entries_removed);
        IV_LOG_W(FAC_MOD, "erase media: %s -> %lu entr(ies) removed, rc=%d", fc->media_root,
                 local.entries_removed - before, rc);
        if (rc != IV_OK)
            return rc;
    }
    if ((erase_mask & IV_FACTORY_ERASE_LOG) != 0u) {
        char          path[IV_FACTORY_PATH_MAX];
        int           n;
        unsigned long before = local.entries_removed;

        n = snprintf(path, sizeof(path), "%s/log", fc->data_root);
        if (n <= 0 || (size_t)n >= sizeof(path))
            return IV_ERANGE;
        rc = iv_factory_wipe(path, &local.entries_removed);
        IV_LOG_W(FAC_MOD, "erase log: %s -> %lu entr(ies) removed, rc=%d", path,
                 local.entries_removed - before, rc);
        if (rc != IV_OK)
            return rc;
    }
    if ((erase_mask & IV_FACTORY_ERASE_DB) != 0u) {
        rc = iv_store_purge_all(store, &local.db_rows_removed);
        if (rc != IV_OK)
            return rc;
    }

    local.erase_mask = erase_mask;
    if (rep != NULL)
        *rep = local;
    IV_LOG_W(FAC_MOD, "factory reset done: cfg_changed=%u version=%llu erase=0x%x "
             "entries=%lu db_rows=%lld",
             local.cfg_changed, (unsigned long long)local.cfg_version, erase_mask,
             local.entries_removed, (long long)local.db_rows_removed);
    return IV_OK;
}
