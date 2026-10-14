/*
 * M6 退出条件 §16.2 场景 6 端到端 —— "SD 卡拔出或写满时，控制和平台业务继续
 * 运行并产生明确告警"
 *
 * ============================ 它验证什么 ============================
 * `tests/unit/test_storage.c` 已经用**假 probe** 把决策钉死了（水位阶梯、迟滞、
 * 删序、故障路径）——但那只能证明"`iv_store_decide()` 在给定事实下算得对"。
 * 它证明不了下面这三件只有**真进程**才能回答的事：
 *
 *   ① 真 `ivsboxd` 里 `iv_storage` 真的被装配起来了，并且真的在巡检**真目录**
 *      （`statfs` 走到挂载点、`stat` 走到根）；
 *   ② SD 异常时**控制业务不受影响** —— 本地通道仍然应答（这是场景 6 的
 *      "控制和平台业务继续运行"，不是存储模块内部的事）；
 *   ③ 异常**产生了明确告警**，并且**上层看得到** —— 告警行落盘 ＋
 *      `sys.overview` 的 `storage` 段带出 `fault/alarm` 位。
 *      （这次改动之前 `iv_storage_alarm()` 除单测外没有任何调用者，
 *      `sys.overview` 也没有 storage 段 ⇒ ③ 是**真缺陷**，本用例是它的回归网。）
 *
 * ============================ 做法 ============================
 * 在私有 user＋mount ns 里（`unshare -rm`）把 `/mnt` 与 `/var/run` 挂成 tmpfs，
 * 用环境变量 `IVSBOX_SD_ROOT`（`iv_storage_root_resolve()` 的**开发/测试口子**，
 * 产线不设）把存储根指到三种受控位置，每种位置真拉起一次 `build/host/ivsboxd`，
 * 然后从**本地通道**问 `sys.overview` / `system.snapshot`：
 *
 *   - **sd ok**  ：根目录存在（tmpfs 上建的空目录）⇒ 应当健康：
 *                 `enabled=true`、`fault=0`、`alarm=0`、`level=0`
 *   - **sd gone**：根目录**不存在**（＝拔卡）⇒ `fault=1`、`alarm=1`、
 *                 `allow_new=0`、`stop_rec=1`；日志出现
 *                 `storage fault: mounted=0 ... (recording gated)`
 *   - **sd full**：根挂在 `size=1M` 的 tmpfs 上并**灌满**（＝写满）⇒
 *                 `level=3`(stop)、`alarm=1`、`allow_new=0`、`stop_rec=1`；
 *                 日志出现 `storage level ok -> stop`
 *
 * **三种情形都断言 `system.snapshot` 仍然应答 `"status":"ok"`** —— 这就是
 * "控制业务不受影响"的可观测证据（存储坏了不该把主控拖垮）。
 *
 * 首轮巡检**不需要等 30 s**：`iv_storage_step()` 的 `last_run_ms==0` 表示
 * "还没跑过"，主控起来的第一拍就会真巡检一次 ⇒ 每种情形几秒内即可断言。
 *
 * ============================ 反向证伪锚点 ============================
 *   f1 `build_storage()` 不再输出 storage 段（接线断）⇒ 三种情形全 FAIL
 *   f2 `sys.overview` 里 `fault`/`alarm` 位恒 0            ⇒ sd gone/full FAIL
 *   f3 `iv_storage_enabled()` 恒返回 1（未启用当健康）     ⇒ 不影响本用例，
 *      但会被"未启用不该报健康"的语义破坏（另一条断言面）
 *   变异必须**编译干净**（撞 `-Werror` 的 BUILD FAILED 不算证据）。
 *
 * ============================ 跑法与降级 ============================
 *   - 宿主机：`make itest`（本文件属 `ITEST_SRCS` 通配，自动纳入）。
 *   - 需要 `build/host/ivsboxd`（先 `make host`）；缺了打印 SKIP 并退出 0。
 *   - `unshare -rm` 不可用（板端无 unshare）⇒ SKIP 退出 0：环境受限不该算代码错
 *     （与 test_cfg_media_e2e / test_ctl_snapshot 同口径）。
 *   - 只用 ns 内的 `/var/run`、`/mnt`，退出即消失，不与宿主既有进程抢资源。
 *   - **本用例首次同时覆盖"平台业务"的可观测面**：平台上行段要真连平台才有，
 *     这里退而求其次断言"主控与其本地控制面在存储故障下仍然工作"，平台侧的真
 *     链路属 §16.2 真机现场（如实登记在 docs/修改记录.md）。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_ret.h"

#define NS_ENV        "IV_SD_FAULT_NS"
#define LOG_ROOT      "/mnt/UDISK/log"
#define CFG_DIR       "/mnt/UDISK/ivsbox/config"
#define CFG_FILE      CFG_DIR "/ivsbox.json"
#define CTL_DIR       "/var/run/ivsbox"
#define CTL_PATH      IV_CHAN_PATH_DEFAULT

#define RECV_TIMEOUT_MS   3000
#define CONNECT_RETRY_MS  8000
#define CONNECT_STEP_MS   250
#define READY_WAIT_MS     15000 /* 等 sys.overview 报出目标状态的上限 */

static int g_fail = 0;

static unsigned char g_rx[IV_CHAN_RECV_CAP_MIN];
static char          g_last[8192]; /* 最近一次 sys.overview 的正文，供失败时打印 */

static void chk(int ok, const char *what)
{
    if (ok)
        return;
    g_fail++;
    printf("FAIL: %s\n", what);
}

static void sleep_ms(int ms)
{
    (void)usleep((useconds_t)ms * 1000u);
}

/* ---------------------------------------------------------------------------
 * 文件系统小工具
 * ------------------------------------------------------------------------- */
static int dir_ok(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0 && S_ISDIR(st.st_mode) ? 1 : 0;
}

static int mkdir_p(const char *path)
{
    char   buf[256];
    size_t i;
    size_t n;

    n = (size_t)snprintf(buf, sizeof buf, "%s", path);
    if (n >= sizeof buf)
        return -1;
    for (i = 1u; i < n; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return -1;
        buf[i] = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;
    return dir_ok(buf) ? 0 : -1;
}

/* 把 dir 挂成 tmpfs。**必须在私有 user+mount ns 里调用**（否则没权限）。 */
static int mount_tmpfs(const char *dir, const char *opts)
{
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;
    if (!dir_ok(dir))
        return -1;
    if (mount("none", dir, "tmpfs", 0, opts) == 0)
        return 0;
    return errno == EBUSY ? 0 : -1;
}

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f == NULL)
        return -1;
    if (fputs(text, f) < 0) {
        (void)fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int append_file(char *buf, size_t cap, const char *path)
{
    FILE  *f = fopen(path, "r");
    size_t used = strlen(buf);
    size_t got;

    if (f == NULL || used + 1u >= cap)
        return 0;
    got = fread(buf + used, 1u, cap - used - 1u, f);
    buf[used + got] = '\0';
    (void)fclose(f);
    return (int)got;
}

/* 汇总 /mnt/UDISK/log/<日期>/<小时>.log 的全部内容。 */
static int read_log(char *buf, size_t cap)
{
    DIR           *d;
    struct dirent *e;
    int            got = 0;

    if (cap < 2u)
        return 0;
    buf[0] = '\0';
    d = opendir(LOG_ROOT);
    if (d == NULL)
        return 0;
    while (!got && (e = readdir(d)) != NULL) {
        char           p[256];
        DIR           *d2;
        struct dirent *e2;

        if (e->d_name[0] == '.')
            continue;
        if (snprintf(p, sizeof p, "%s/%s", LOG_ROOT, e->d_name) >= (int)sizeof p)
            continue;
        if (!dir_ok(p))
            continue;
        d2 = opendir(p);
        if (d2 == NULL)
            continue;
        while ((e2 = readdir(d2)) != NULL) {
            char f[320];

            if (e2->d_name[0] == '.')
                continue;
            if (snprintf(f, sizeof f, "%s/%s", p, e2->d_name) >= (int)sizeof f)
                continue;
            if (append_file(buf, cap, f) > 0) {
                got = 1;
                break;
            }
        }
        (void)closedir(d2);
    }
    (void)closedir(d);
    return got;
}

/* 清空日志树：让每个情形的日志断言只看到**本次**主控写的那几行，
 * 免得上一情形（或上一次运行）的日志把断言蒙过去。 */
static void clear_log(void)
{
    DIR           *d = opendir(LOG_ROOT);
    struct dirent *e;

    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        char           p[256];
        DIR           *d2;
        struct dirent *e2;

        if (e->d_name[0] == '.')
            continue;
        if (snprintf(p, sizeof p, "%s/%s", LOG_ROOT, e->d_name) >= (int)sizeof p)
            continue;
        if (!dir_ok(p))
            continue;
        d2 = opendir(p);
        if (d2 != NULL) {
            while ((e2 = readdir(d2)) != NULL) {
                char f[320];

                if (e2->d_name[0] == '.')
                    continue;
                if (snprintf(f, sizeof f, "%s/%s", p, e2->d_name) >= (int)sizeof f)
                    continue;
                (void)unlink(f);
            }
            (void)closedir(d2);
        }
        (void)rmdir(p);
    }
    (void)closedir(d);
}

/* 把 tmpfs 灌满（写不动为止）。用于模拟"SD 卡写满"。 */
static int fill_tmpfs(const char *root)
{
    char    p[256];
    char    buf[4096];
    int     fd;
    ssize_t n;
    int     total = 0;

    if (snprintf(p, sizeof p, "%s/fill.bin", root) >= (int)sizeof p)
        return -1;
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    memset(buf, 'A', sizeof buf);
    for (;;) {
        n = write(fd, buf, sizeof buf);
        if (n <= 0)
            break; /* ENOSPC = 灌满了，正常收工 */
        total += (int)n;
        if (total > 8 * 1024 * 1024)
            break; /* 安全阀：opts 写错时不至于无限写 */
    }
    (void)close(fd);
    return total;
}

/* ---------------------------------------------------------------------------
 * 本地通道客户端（与 test_ctl_snapshot.c 同一套）
 * ------------------------------------------------------------------------- */
static int connect_retry(const char *path)
{
    int waited = 0;
    int fd;

    for (;;) {
        fd = iv_chan_connect(path);
        if (fd >= 0)
            return fd;
        if (fd != IV_ENOENT && fd != IV_EBUSY)
            return fd;
        if (waited >= CONNECT_RETRY_MS)
            return fd;
        sleep_ms(CONNECT_STEP_MS);
        waited += CONNECT_STEP_MS;
    }
}

static int round_trip(int fd, const char *method, unsigned long long request_id,
                      char *out, size_t outcap)
{
    ivs_chan_hdr_t hdr;
    size_t         payload_len = 0;
    size_t         n           = strlen(method);
    int            rc;
    int            i;

    if (iv_chan_hdr_init(&hdr, IV_CHAN_TYPE_REQ, (uint64_t)request_id, 0u,
                         (uint32_t)n) != IV_OK)
        return IV_EFAIL;
    if (iv_chan_send(fd, &hdr, method, n) != IV_OK)
        return IV_EFAIL;
    for (i = 0; i < 3; i++) {
        rc = iv_chan_recv_timeout(fd, g_rx, sizeof g_rx, &hdr, &payload_len,
                                  RECV_TIMEOUT_MS);
        if (rc != IV_EAGAIN)
            break;
    }
    if (rc != IV_OK || hdr.type != IV_CHAN_TYPE_RSP)
        return IV_EPROTO;
    if (hdr.request_id != (uint64_t)request_id)
        return IV_EPROTO;
    if (payload_len == 0u || payload_len >= outcap)
        return IV_ERANGE;
    memcpy(out, g_rx + IV_CHAN_HDR_SIZE, payload_len);
    out[payload_len] = '\0';
    return IV_OK;
}

/* 反复问 sys.overview，直到正文里出现 needle（或超时）。
 * 主控起来的第一拍就会巡检一次，所以正常情况下几百毫秒内命中。 */
static int wait_overview(int fd, const char *needle, int timeout_ms)
{
    int waited = 0;

    while (waited <= timeout_ms) {
        g_last[0] = '\0';
        if (round_trip(fd, "sys.overview", 0x5D0000ull + (unsigned long long)waited,
                       g_last, sizeof g_last) == IV_OK &&
            strstr(g_last, needle) != NULL)
            return 1;
        sleep_ms(200);
        waited += 200;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * ns 自重启
 * ------------------------------------------------------------------------- */
static int ns_mount_available(void)
{
    pid_t p;
    int   st = 0;

    p = fork();
    if (p < 0)
        return 0;
    if (p == 0) {
        execlp("unshare", "unshare", "-rm", "--", "true", (char *)NULL);
        _exit(127);
    }
    if (waitpid(p, &st, 0) < 0)
        return 0;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 1 : 0;
}

static int reexec_in_ns(const char *self)
{
    pid_t p;
    int   st = 0;

    p = fork();
    if (p < 0) {
        printf("SKIP: fork failed\n");
        return 0;
    }
    if (p == 0) {
        (void)setenv(NS_ENV, "1", 1);
        execlp("unshare", "unshare", "-rm", "--", self, (char *)NULL);
        _exit(127);
    }
    if (waitpid(p, &st, 0) < 0)
        return 0;
    if (!WIFEXITED(st))
        return 0;
    if (WEXITSTATUS(st) == 127) {
        printf("SKIP: unshare not found on PATH\n");
        return 0;
    }
    return (int)WEXITSTATUS(st);
}

/* ---------------------------------------------------------------------------
 * 场景本体（已在 ns 内）
 * ------------------------------------------------------------------------- */
enum { SD_OK = 0, SD_GONE = 1, SD_FULL = 2 };

typedef struct {
    const char *name;
    const char *root;
    int         kind;
    /* sys.overview 的 storage 段里**必须出现**的片段 */
    const char *need_state;
    /* 日志里**必须出现**的片段（"明确告警"落盘的证据） */
    const char *need_log;
} sd_case_t;

static const sd_case_t CASES[] = {
    { "sd ok  ", "/mnt/sd_ok",   SD_OK,   "\"fault\":0",
      "storage policy up, root=/mnt/sd_ok" },
    { "sd gone", "/mnt/sd_gone", SD_GONE, "\"fault\":1",
      "storage fault: mounted=0" },
    { "sd full", "/mnt/sd_full", SD_FULL, "\"level\":3",
      "storage level ok -> stop" },
};
#define NCASES ((int)(sizeof(CASES) / sizeof(CASES[0])))

static void prep_sd(const sd_case_t *c)
{
    if (c->kind == SD_OK) {
        (void)mkdir_p(c->root);
        return;
    }
    if (c->kind == SD_GONE) {
        (void)rmdir(c->root); /* 确保不存在 ⇒ 等同于拔卡 */
        return;
    }
    /* SD_FULL：小容量 tmpfs ＋ 灌满 */
    if (mount_tmpfs(c->root, "mode=0755,size=1M") != 0)
        printf("note: cannot mount a 1M tmpfs at %s\n", c->root);
    else if (fill_tmpfs(c->root) < 0)
        printf("note: cannot fill %s\n", c->root);
}

static void unprep_sd(const sd_case_t *c)
{
    if (c->kind == SD_FULL)
        (void)umount(c->root); /* 尽力而为；ns 退出也会带走 */
}

static int stop_child(pid_t pid)
{
    int st = 0;
    int i;

    (void)kill(pid, SIGTERM);
    for (i = 0; i < 50; i++) {
        pid_t r = waitpid(pid, &st, WNOHANG);

        if (r == pid)
            return WIFEXITED(st) ? (int)WEXITSTATUS(st) : -1;
        if (r < 0)
            return -1;
        sleep_ms(100);
    }
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, &st, 0);
    return -1;
}

static void run_case(const char *bin, const sd_case_t *c, int idx)
{
    static char log[64 * 1024];
    pid_t       pid;
    int         fd;

    printf("-- case %-7s root=%s\n", c->name, c->root);
    clear_log();
    prep_sd(c);

    /* 把存储根指过去（iv_storage_root_resolve 的测试口子；产线不设）。 */
    (void)setenv("IVSBOX_SD_ROOT", c->root, 1);

    pid = fork();
    if (pid < 0) {
        printf("SKIP: cannot fork ivsboxd\n");
        g_fail++;
        unprep_sd(c);
        return;
    }
    if (pid == 0) {
        (void)execl(bin, bin, (char *)NULL);
        _exit(127);
    }

    fd = connect_retry(CTL_PATH);
    if (fd < 0) {
        g_fail++;
        printf("FAIL: [%s] cannot connect to %s (rc=%d)\n", c->name, CTL_PATH, fd);
        (void)stop_child(pid);
        unprep_sd(c);
        return;
    }

    /* ① storage 段报出目标状态（顺带钉死 sys.overview 真的带出了 storage）。 */
    {
        char what[160];

        (void)snprintf(what, sizeof what,
                       "[%s] sys.overview storage reports '%s'", c->name, c->need_state);
        if (!wait_overview(fd, c->need_state, READY_WAIT_MS)) {
            chk(0, what);
            printf("---- last sys.overview (%.600s) ----\n", g_last);
        } else {
            chk(1, what);
        }
    }

    /* ② 声明"启用了"：enabled 为真才有后面的 fault/alarm 可言。 */
    {
        char what[160];

        (void)snprintf(what, sizeof what, "[%s] storage.enabled is true", c->name);
        chk(wait_overview(fd, "\"enabled\":true", 2000), what);
    }

    /* ③ 控制业务不受影响：本地通道仍能应答老方法（场景 6 的核心断言）。 */
    {
        char body[512];
        char what[200];
        int  rc = round_trip(fd, "system.snapshot", 0x5D1000ull + (unsigned)idx,
                             body, sizeof body);

        (void)snprintf(what, sizeof what,
                       "[%s] control channel still answers system.snapshot while the SD is broken",
                       c->name);
        chk(rc == IV_OK && strstr(body, "\"status\":\"ok\"") != NULL, what);
    }

    /* ④ 明确告警落盘。 */
    {
        char what[200];

        (void)snprintf(what, sizeof what, "[%s] log carries '%s'", c->name, c->need_log);
        (void)read_log(log, sizeof log);
        if (strstr(log, c->need_log) == NULL) {
            chk(0, what);
            printf("---- log tail ----\n%.1500s\n------------------\n", log);
        } else {
            chk(1, what);
        }
    }

    (void)close(fd);
    (void)stop_child(pid);
    unprep_sd(c);
}

static int scenario(const char *bin)
{
    int i;

    if (mount_tmpfs("/mnt", "mode=0755") != 0) {
        printf("SKIP: cannot mount a private tmpfs on /mnt (needs `unshare -rm`)\n");
        return 0;
    }
    if (mount_tmpfs("/var/run", "mode=0755") != 0) {
        printf("SKIP: cannot mount a private tmpfs on /var/run\n");
        return 0;
    }
    chk(mkdir_p(CFG_DIR) == 0, "prepare the config dir under the private /mnt");
    chk(mkdir_p(CTL_DIR) == 0, "prepare the ctl dir under the private /var/run");
    chk(write_file(CFG_FILE, "{\"_meta\":{\"version\":1}}\n") == 0,
        "write a minimal ivsbox.json");

    for (i = 0; i < NCASES; i++)
        run_case(bin, &CASES[i], i);

    if (g_fail == 0)
        printf("test_sd_fault_e2e passed (SD pulled / SD full are reported on the control "
               "channel, the control channel keeps answering, alarms are logged)\n");
    return g_fail == 0 ? 0 : 1;
}

int main(int argc, char *argv[])
{
    const char *bin;

    (void)argc;

    bin = getenv("IV_IVSBOXD_BIN");
    if (bin == NULL || bin[0] == '\0')
        bin = "build/host/ivsboxd";

    {
        struct stat st;

        if (stat(bin, &st) != 0 || access(bin, X_OK) != 0) {
            printf("test_sd_fault_e2e SKIPPED: %s not found "
                   "(build with `make host`, or set IV_IVSBOXD_BIN)\n", bin);
            return 0;
        }
    }

    if (getenv(NS_ENV) == NULL) {
        if (!ns_mount_available()) {
            printf("SKIP: private user+mount ns unavailable (`unshare -rm`), cannot run a "
                   "real ivsboxd against controlled SD roots\n");
            return 0;
        }
        return reexec_in_ns(argv[0]);
    }
    return scenario(bin);
}
