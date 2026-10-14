/*
 * ivsbox-updater —— 升级安装 / 双版本切换 / 回滚与启动包装的命令行入口
 * （功能开发计划 M7-S7.2 ＋ S7.3）
 *
 * ============================ 分工 ============================
 * 真正的逻辑全在 `src/modules/upd/iv_upd.c`（进 libivmodules，可单测）；本文件只做
 * **参数解析 ＋ 输出**，不自己实现任何安装语义。这样"升级的判据"只有一份实现，
 * 单测钉住的是板端真正跑的那份代码。
 *
 * ============================ 为什么要有命令行这一层 ============================
 * 启动包装是 **shell**（`scripts/ivsbox-run.sh`）：它必须在"程序自身可能已经坏掉"的
 * 前提下拉起程序，所以不能依赖被拉起的那份程序里的任何逻辑。它需要问两件事：
 *   `boot-begin` → "这次该起哪个目录"（stdout 一行绝对路径）
 *   `boot-ok` / `boot-fail` → "自检过了没有"
 * 这些子命令**必须由引导副本执行**（`<app_root>/bin/ivsbox-updater`，不参与版本轮换），
 * 详见 `scripts/ivsbox-run.sh` 的文件头。
 *
 * ============================ 退出码 ============================
 *   0  成功
 *   1  用法/参数错
 *   2  操作失败（具体原因打到 stderr，形如 `install: failed (bad-package/ESIG)`）
 * `boot-begin` 成功时 **stdout 只有一行**（app_dir），供 `$(...)` 取用；
 * 其余诊断一律走 stderr，避免污染命令替换的结果。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ivsbox/iv_ota.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_upd.h"
#include "ivsbox/iv_version.h"

#define EXIT_USAGE 1
#define EXIT_FAIL 2

static const char *const PROG = "ivsbox-updater";

static void usage(void)
{
    fprintf(stderr,
            "用法: %s <命令> [选项]\n"
            "\n"
            "命令:\n"
            "  install <包>          验签 → 暂存 → 准入检查 → 装到 releases/<ver> → 原子切 current\n"
            "  adopt                 首次升级前把裸布局 bin/+scripts 收养成 releases/<当前版本>\n"
            "  boot-begin            启动包装第一步：stdout 输出这次该用的程序目录（一行）\n"
            "  boot-ok               自检通过：pending → confirmed\n"
            "  boot-fail             自检失败：立刻回滚到 previous\n"
            "  rollback              手动回滚到 previous\n"
            "  status [--pending]    打印状态；--pending 时有待确认升级退出码为 0，否则 1\n"
            "\n"
            "选项:\n"
            "  --app-root <目录>     程序根（默认 %s）\n"
            "  --data-root <目录>    数据根（默认 %s）\n"
            "  --lib-paths <表>      NEEDED 搜索目录，':' 分隔（默认内置候选）\n"
            "  --model <型号>        本机产品型号（默认 %s）\n"
            "  --hw <n>              本机硬件版本（默认 %u）\n"
            "  --max-boot <n>        bootcount 上限（默认 %u）\n"
            "  --pubkey-hex <64hex>  显式指定发布公钥 —— **仅联调/产线**，量产流程不要用\n"
            "  --fault-at <阶段>     掉电注入：verify/stage/install/switch/pending（S7.4 用）\n"
            "  --restart             装完/回滚后重启业务（/etc/init.d/S65ivsboxd restart）\n"
            "  --reason <串>         rollback / boot-fail 的原因（默认 manual / selftest-failed）\n"
            "  --version             打印版本\n"
            "  -h, --help            本帮助\n",
            PROG, IV_UPD_APP_ROOT_DEFAULT, IV_UPD_DATA_ROOT_DEFAULT, IV_UPD_DEV_MODEL_DEFAULT,
            (unsigned)IV_UPD_DEV_HW_DEFAULT, (unsigned)IV_UPD_MAX_BOOT_DEFAULT);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex32(const char *s, uint8_t out[IV_OTA_PUBKEY_LEN])
{
    unsigned i;

    if (s == NULL || strlen(s) != IV_OTA_PUBKEY_LEN * 2u)
        return -1;
    for (i = 0u; i < IV_OTA_PUBKEY_LEN; i++) {
        int hi = hexval(s[i * 2u]);
        int lo = hexval(s[i * 2u + 1u]);

        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* 固定命令串（不含任何外部输入，无拼接 ⇒ 无注入面）。 */
static int restart_business(void)
{
    int rc = system("/etc/init.d/S65ivsboxd restart");

    return (rc == 0) ? 0 : -1;
}

static int report(const char *what, int rc, iv_upd_reason_t why)
{
    if (rc != IV_OK) {
        fprintf(stderr, "%s: failed (%s/%s) rc=%d\n", what, iv_upd_reason_name(why),
                iv_upd_ret_name(rc), rc);
        return EXIT_FAIL;
    }
    return 0;
}

static void print_status(const iv_upd_status_t *st)
{
    char ver[IV_UPD_VER_MAX];
    char prev[IV_UPD_VER_MAX];

    iv_ota_ver_format(st->version, ver, sizeof ver);
    iv_ota_ver_format(st->previous, prev, sizeof prev);
    printf("pending=%s\n", st->pending ? "true" : "false");
    printf("active=%s\n", st->active ? "true" : "false");
    printf("state=%s\n", st->state[0] != '\0' ? st->state : "-");
    printf("version=%s\n", ver);
    printf("previous=%s\n", prev);
    printf("bootcount=%u\n", (unsigned)st->bootcount);
    printf("max_boot=%u\n", (unsigned)st->max_boot);
    printf("rollbacks=%u\n", (unsigned)st->rollbacks);
    printf("last_event=%s\n", st->last_event[0] != '\0' ? st->last_event : "-");
    printf("last_reason=%s\n", st->last_reason[0] != '\0' ? st->last_reason : "-");
    printf("current=%s\n", st->current_dir[0] != '\0' ? st->current_dir : "-");
}

int main(int argc, char *argv[])
{
    iv_upd_cfg_t     cfg;
    iv_upd_reason_t  why = IV_UPD_OK;
    uint8_t          pub[IV_OTA_PUBKEY_LEN];
    const char      *cmd = NULL;
    const char      *arg = NULL;
    const char      *reason = NULL;
    const char      *fault = NULL;
    const char      *model = NULL;
    const char      *app_root = NULL;
    const char      *data_root = NULL;
    const char      *lib_paths = NULL;
    const char      *pubhex = NULL;
    unsigned         hw = 0u;
    unsigned         max_boot = 0u;
    int              restart = 0;
    int              only_pending = 0;
    int              want_version = 0;
    int              i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            want_version = 1;
            continue;
        } else if (strcmp(a, "--app-root") == 0 && i + 1 < argc) {
            app_root = argv[++i];
        } else if (strcmp(a, "--data-root") == 0 && i + 1 < argc) {
            data_root = argv[++i];
        } else if (strcmp(a, "--lib-paths") == 0 && i + 1 < argc) {
            lib_paths = argv[++i];
        } else if (strcmp(a, "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(a, "--hw") == 0 && i + 1 < argc) {
            hw = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--max-boot") == 0 && i + 1 < argc) {
            max_boot = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--pubkey-hex") == 0 && i + 1 < argc) {
            pubhex = argv[++i];
        } else if (strcmp(a, "--fault-at") == 0 && i + 1 < argc) {
            fault = argv[++i];
        } else if (strcmp(a, "--reason") == 0 && i + 1 < argc) {
            reason = argv[++i];
        } else if (strcmp(a, "--restart") == 0) {
            restart = 1;
        } else if (strcmp(a, "--pending") == 0) {
            only_pending = 1;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "%s: 未知选项 %s\n", PROG, a);
            return EXIT_USAGE;
        } else if (cmd == NULL) {
            cmd = a;
        } else if (arg == NULL) {
            arg = a;
        } else {
            fprintf(stderr, "%s: 多余参数 %s\n", PROG, a);
            return EXIT_USAGE;
        }
    }

    if (want_version) {
        printf("%s %s\n", PROG, iv_version_string());
        return 0;
    }
    if (cmd == NULL) {
        usage();
        return EXIT_USAGE;
    }

    iv_upd_cfg_default(&cfg);
    if (app_root != NULL)
        cfg.app_root = app_root;
    if (data_root != NULL)
        cfg.data_root = data_root;
    if (lib_paths != NULL)
        cfg.lib_paths = lib_paths;
    if (model != NULL)
        cfg.dev_model = model;
    if (hw != 0u)
        cfg.dev_hw = (uint32_t)hw;
    if (max_boot != 0u)
        cfg.max_boot = (uint32_t)max_boot;
    if (fault != NULL)
        cfg.fault_at = fault;
    if (pubhex != NULL) {
        if (parse_hex32(pubhex, pub) != 0) {
            fprintf(stderr, "%s: --pubkey-hex 需要 64 个小写/大写十六进制字符\n", PROG);
            return EXIT_USAGE;
        }
        cfg.pubkey = pub;
        fprintf(stderr, "%s: 警告：使用 --pubkey-hex 指定的联调公钥（量产流程不得使用）\n",
                PROG);
    }

    if (strcmp(cmd, "install") == 0) {
        int rc;

        if (arg == NULL) {
            fprintf(stderr, "%s: install 需要给出升级包路径\n", PROG);
            return EXIT_USAGE;
        }
        rc = iv_upd_install(arg, &cfg, &why);
        if (rc != IV_OK)
            return report("install", rc, why);
        if (restart != 0) {
            fprintf(stderr, "%s: 重启业务服务…\n", PROG);
            if (restart_business() != 0) {
                fprintf(stderr, "%s: restart failed (升级本身已成功，请手工重启服务)\n", PROG);
                return EXIT_FAIL;
            }
        }
        printf("install: ok\n");
        return 0;
    }

    if (strcmp(cmd, "adopt") == 0) {
        int rc = iv_upd_adopt(&cfg, &why);

        if (rc != IV_OK && rc != IV_ENOENT)
            return report("adopt", rc, why);
        printf("adopt: %s\n", (rc == IV_OK) ? "ok" : "nothing-to-adopt");
        return 0;
    }

    if (strcmp(cmd, "boot-begin") == 0) {
        char app_dir[IV_UPD_PATH_MAX];
        int  rolled = 0;
        int  rc = iv_upd_boot_begin(&cfg, app_dir, sizeof app_dir, &rolled, &why);

        if (rc != IV_OK)
            return report("boot-begin", rc, why);
        if (rolled != 0)
            fprintf(stderr, "%s: 本次启动已自动回滚到 previous（%s）\n", PROG,
                    iv_upd_reason_name(why));
        printf("%s\n", app_dir);
        return 0;
    }

    if (strcmp(cmd, "boot-ok") == 0) {
        int rc = iv_upd_boot_ok(&cfg, &why);

        if (rc != IV_OK)
            return report("boot-ok", rc, why);
        return 0;
    }

    if (strcmp(cmd, "boot-fail") == 0) {
        int rc = iv_upd_boot_fail(&cfg, &why);

        if (rc != IV_OK)
            return report("boot-fail", rc, why);
        if (restart != 0) {
            fprintf(stderr, "%s: 回滚完成，重启业务服务…\n", PROG);
            if (restart_business() != 0)
                return EXIT_FAIL;
        }
        return 0;
    }

    if (strcmp(cmd, "rollback") == 0) {
        int rc = iv_upd_rollback(&cfg, reason, &why);

        if (rc != IV_OK)
            return report("rollback", rc, why);
        if (restart != 0) {
            fprintf(stderr, "%s: 回滚完成，重启业务服务…\n", PROG);
            if (restart_business() != 0)
                return EXIT_FAIL;
        }
        return 0;
    }

    if (strcmp(cmd, "status") == 0) {
        iv_upd_status_t st;

        iv_upd_status(&cfg, &st);
        if (only_pending != 0)
            return st.active ? 0 : 1;
        print_status(&st);
        return 0;
    }

    fprintf(stderr, "%s: 未知命令 %s\n", PROG, cmd);
    usage();
    return EXIT_USAGE;
}
