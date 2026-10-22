/*
 * 恢复出厂 / 擦除存储 端到端 —— 集成测试（2026-10-22 单片机 web 对齐第 ⑥ 项）
 *
 * ============================ 为什么必须是集成测试 ============================
 * 三块单测已经各自证明了自己的那一半：
 *   - `test_config`  c31a~c31q：`iv_config_reset_defaults()` 把键回默认、把
 *     `camera.ch` **整段删掉**（只清值会让重载后 `camera_count` 回填成 8）、
 *     扩展键保留、save 后版本只 +1；
 *   - `test_factory` f01~f08：递归擦除的形状（符号链接不跟随、根不可删）、
 *     四个表的 `purge_all`、"配置写盘失败就一条数据都不擦"的顺序；
 *   - `test_ctl`     c25c~c25p：方法名在表里、`confirm` 门、`erase` 名字校验、
 *     未接线回 `ENOTSUP`、审计行。
 *
 * 但**装配层谁也覆盖不到**：`src/app/main.c` 那句
 *     g_ctl_ctx.factory_data_root  = "/mnt/UDISK/ivsbox";
 *     g_ctl_ctx.factory_media_root = iv_storage_root_resolve("", ...);
 * 是 app 层代码，**不进静态库**，单测链接不到它。这与 M4-S4.4「两种驱动模式
 * 装配不一致」、S6.5「配置接线」是同一族缺陷：漏一行编译不报、日志不报，只在
 * 真机上表现为"网页点了恢复出厂，配置回去了、录像没删"。所以本用例真拉起
 * `build/host/ivsboxd`，从**外部**用 ctl 协议走一遍。
 *
 * ============================ 它钉死四件事 ============================
 *   ① **两个根真的接上了**：响应 `erase` 回显 ["media","log","db"]。任何一个根
 *      没赋值，`iv_factory_reset()` 会回 `IV_ENOTSUP` ⇒ 这里直接红（见 f01）。
 *   ② **媒体与日志真被擦掉、且只擦根下**：`/mnt/sdcard` 还在、`/mnt/sdcard/video`
 *      与 `/mnt/sdcard/snap` 没了；预置的旧日期目录 `/mnt/UDISK/log/2020-01-01`
 *      没了（日志根还在 —— 擦的是 root 的孩子，不是 root）。
 *   ③ **配置真回默认并落盘**：读**文件**（不是响应）里不得再出现 `SITE-A`、
 *      `10.9.9.9`、`10.0.0.9`；`camera` 整段消失（数组删掉了，不是清值）。
 *   ④ **擦完还能用**：再调一次 `sys.factory_reset` ⇒ `cfg_changed` 为 0（幂等）；
 *      再调 `system.snapshot` 仍回 `ok:true`（主控没被打死、配置重载没炸）。
 *
 * ============================ 反向证伪锚点 ============================
 *   F1 删掉 `main.c` 里 `factory_media_root = ...` 那行 ⇒ 擦媒体回 `IV_ENOTSUP`
 *      ⇒ ① 与 ② 红（这条是本用例存在的**唯一理由**，单测抓不到）。
 *   F2 把 `iv_factory_reset()` 里"先存配置、后擦数据"调换 ⇒ 断掉写盘权限后
 *      数据先没、配置没回去（由 `test_factory` f08 钉，本用例不重复）。
 *   F3 `iv_config_reset_defaults()` 把 `camera.ch` 改成"只清值不删条目" ⇒ ③ 的
 *      `camera` 消失断言红（重载后 `camera_count` 会回填）。
 *   F4 把 `iv_factory_wipe()` 的根改成"连 root 一起删" ⇒ ② 的"`/mnt/sdcard`
 *      还在"红。
 *   变异必须**编译干净**（撞 `-Werror` 的 BUILD FAILED 不算证据）。
 *
 * ============================ 跑法与降级 ============================
 *   - 宿主机 `make itest`（本文件属 `ITEST_SRCS` 通配，自动纳入）。
 *   - 需要 `build/host/ivsboxd`；缺了打印 SKIP 退出 0。
 *   - 在私有 user+mount ns（`unshare -rm`）里把 `/mnt` 与 `/var/run` 挂成
 *     tmpfs，**不碰宿主机既有配置与日志**；环境不支持 ns ⇒ SKIP 退出 0
 *     （与 test_cfg_media_e2e / test_agent_switch 同口径：环境受限不算代码错）。
 *   - 用例只删自己种下的文件，退出时把主控收掉（AGENTS.md 规则 6）。
 */
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_ret.h"

#define NS_ENV        "IV_FACTORY_NS"
#define DATA_ROOT     "/mnt/UDISK/ivsbox"
#define CFG_DIR       DATA_ROOT "/config"
#define CFG_FILE      CFG_DIR "/ivsbox.json"
#define LOG_ROOT      "/mnt/UDISK/log"
#define MEDIA_ROOT    "/mnt/sdcard"
#define CTL_DIR       "/var/run/ivsbox"
#define CTL_PATH      CTL_DIR "/ctl"

#define CONNECT_RETRY_MS 20000 /* 主控装配（配置/SQLite/Reactor/ctl）要一点时间 */
#define CONNECT_STEP_MS  200
#define RECV_TIMEOUT_MS  5000
#define CFG_RD_MAX       40960 /* 落盘文件带逐行中文注释，实测 10 KB 量级 */

static int g_fail;

/* 收发缓冲放静态区：iv_chan 的最小接收容量是 64 KiB 量级，不占栈。 */
static unsigned char g_rx[IV_CHAN_RECV_CAP_MIN];
static char          g_cfg[CFG_RD_MAX];

static void chk(int ok, const char *what)
{
    if (ok)
        return;
    g_fail++;
    printf("FAIL: %s\n", what);
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static void sleep_ms(int ms)
{
    (void)usleep((useconds_t)ms * 1000u);
}

/* ---------------------------------------------------------------------------
 * 小工具
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

static int mount_tmpfs(const char *dir)
{
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;
    if (!dir_ok(dir))
        return -1;
    if (mount("none", dir, "tmpfs", 0, "mode=0755") == 0)
        return 0;
    return errno == EBUSY ? 0 : -1; /* EBUSY = 已挂上，可接受 */
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

/* 读整个配置文件到 g_cfg（失败返回 0）。 */
static int read_cfg(void)
{
    FILE  *f = fopen(CFG_FILE, "r");
    size_t got;

    g_cfg[0] = '\0';
    if (f == NULL)
        return 0;
    got = fread(g_cfg, 1u, sizeof g_cfg - 1u, f);
    g_cfg[got] = '\0';
    (void)fclose(f);
    return got > 0u ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * ns 自重启：`unshare -rm`（user＋mount，不需要 root）后自己再来一遍
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
 * ctl 客户端
 * ------------------------------------------------------------------------- */
static int connect_retry(void)
{
    int waited = 0;
    int fd;

    for (;;) {
        fd = iv_chan_connect(CTL_PATH);
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

/* 发一条 JSON 请求，等一条应答，把载荷拷成 C 串。返回 IV_OK/错误码。 */
static int call(int *fd_io, const char *json, char *out, size_t outcap)
{
    ivs_chan_hdr_t hdr;
    size_t         len = strlen(json);
    size_t         plen = 0u;
    unsigned long long req_id = (unsigned long long)time(NULL);
    int            fd = *fd_io;
    int            rc;
    int            i;

    if (fd < 0) {
        fd = connect_retry();
        if (fd < 0)
            return fd;
        *fd_io = fd;
    }

    rc = iv_chan_hdr_init(&hdr, IV_CHAN_TYPE_REQ, req_id, 0u, (uint32_t)len);
    if (rc != IV_OK)
        return rc;
    rc = iv_chan_send(fd, &hdr, json, len);
    if (rc != IV_OK)
        return rc;

    for (i = 0; i < 3; i++) {
        rc = iv_chan_recv_timeout(fd, g_rx, sizeof g_rx, &hdr, &plen,
                                  RECV_TIMEOUT_MS);
        if (rc != IV_EAGAIN)
            break;
    }
    if (rc != IV_OK)
        return rc;
    if (hdr.type != IV_CHAN_TYPE_RSP || hdr.request_id != req_id)
        return IV_EPROTO;
    if (plen == 0u || plen >= outcap)
        return IV_ERANGE;

    memcpy(out, g_rx + IV_CHAN_HDR_SIZE, plen);
    out[plen] = '\0';
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 场景本体（已在 ns 内）
 * ------------------------------------------------------------------------- */
/* 种下去的非默认值，每一处都对应一条"回默认"断言：
 *   sys.device.name  SITE-A    → 字符串型回空
 *   net.ip           10.9.9.9  → IP 型回出厂
 *   camera.ch[0]     ip/record → 数组**整段**消失（不是清值）
 *   probe.max_fail   9         → 整型回 3
 */
static const char CFG_TEXT[] =
    "{\"_meta\":{\"version\":1},\n"
    " \"sys\":{\"device\":{\"name\":\"SITE-A\"}},\n"
    " \"net\":{\"ip\":\"10.9.9.9\"},\n"
    " \"probe\":{\"max_fail\":9},\n"
    " \"camera\":{\"ch\":[{\"ip\":\"10.0.0.9\",\"record\":true}]},\n"
    " \"mine.note\":\"keep-me\"}\n";

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

static int scenario(const char *bin)
{
    static char rsp[8192];
    static char rsp2[8192];
    static char rsp3[8192];
    pid_t       pid;
    int         fd = -1;
    int         rc;

    /* ---- 0. 私有挂载点：/mnt 与 /var/run 都是空的 tmpfs ---- */
    if (mount_tmpfs("/mnt") != 0) {
        printf("SKIP: cannot mount a private tmpfs on /mnt (needs `unshare -rm`)\n");
        return 0;
    }
    if (mount_tmpfs("/var/run") != 0) {
        printf("SKIP: cannot mount a private tmpfs on /var/run\n");
        return 0;
    }
    chk(mkdir_p(CFG_DIR) == 0, "prepare the config dir under the private /mnt");
    chk(mkdir_p(CTL_DIR) == 0, "prepare the ctl dir under the private /var/run");

    /* ---- 1. 种数据：配置 ＋ 媒体两个目录 ＋ 一个"旧日期"日志目录 ---- */
    chk(write_file(CFG_FILE, CFG_TEXT) == 0, "write ivsbox.json (non-default values)");
    chk(mkdir_p(MEDIA_ROOT "/video/20261022") == 0, "prepare media video dir");
    chk(mkdir_p(MEDIA_ROOT "/snap") == 0, "prepare media snap dir");
    chk(write_file(MEDIA_ROOT "/video/20261022/r1.mp4", "MP4DATA") == 0,
        "seed a recording");
    chk(write_file(MEDIA_ROOT "/snap/s1.jpg", "JPGDATA") == 0, "seed a snapshot");
    chk(mkdir_p(LOG_ROOT "/2020-01-01") == 0, "prepare an old log day dir");
    chk(write_file(LOG_ROOT "/2020-01-01/00.log", "old log line\n") == 0,
        "seed an old log file");

    /* ---- 2. 真拉起主控 ---- */
    pid = fork();
    if (pid < 0) {
        printf("SKIP: cannot fork ivsboxd\n");
        return 0;
    }
    if (pid == 0) {
        (void)execl(bin, bin, (char *)NULL);
        _exit(127);
    }

    /* ---- 3. 恢复出厂（三个擦除位全给） ---- */
    rc = call(&fd, "{\"m\":\"sys.factory_reset\",\"p\":{\"confirm\":true,"
                   "\"erase\":[\"media\",\"log\",\"db\"]}}", rsp, sizeof rsp);
    if (rc != IV_OK) {
        int st = 0;

        g_fail++;
        printf("FAIL: sys.factory_reset not answered (rc=%d)\n", rc);
        if (waitpid(pid, &st, WNOHANG) == pid)
            printf("      ivsboxd exited (exit=%d)\n",
                   WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        (void)stop_child(pid);
        return 1;
    }

    /* ① 两个根接上了：三个擦除位必须原样回显 */
    chk(contains(rsp, "\"ok\":true"), "F  : factory reset answered ok:true");
    if (!contains(rsp, "\"ok\":true"))
        printf("---- ctl response ----\n%.1000s\n----------------------\n", rsp);
    chk(contains(rsp, "\"erase\":[\"media\",\"log\",\"db\"]"),
        "f01: both roots are wired (erase echoes media,log,db) -- no IV_ENOTSUP");
    chk(contains(rsp, "\"password_reset_required\":true"),
        "f02: ctl tells the web layer to reset the password");

    /* ② 媒体与日志：根还在，根下的东西没了 */
    chk(dir_ok(MEDIA_ROOT), "f03: the media root itself survives");
    chk(access(MEDIA_ROOT "/video", F_OK) != 0, "f04: media/video is gone");
    chk(access(MEDIA_ROOT "/snap", F_OK) != 0, "f05: media/snap is gone");
    chk(dir_ok(LOG_ROOT), "f06: the log root itself survives");
    chk(access(LOG_ROOT "/2020-01-01", F_OK) != 0, "f07: the old log day dir is gone");

    /* ③ 配置：读**文件**，不是读响应 */
    chk(read_cfg(), "f08: the config file is still there after the reset");
    chk(!contains(g_cfg, "SITE-A"), "f09: sys.device.name is back to default (file)");
    chk(!contains(g_cfg, "10.9.9.9"), "f10: net.ip is back to default (file)");
    chk(!contains(g_cfg, "10.0.0.9"), "f11: camera.ch values are gone (file)");
    chk(!contains(g_cfg, "\"ch\":"),
        "f12: the camera.ch array is dropped (no \"ch\" key), not just cleared");
    chk(contains(g_cfg, "keep-me"),
        "f13: extension keys outside the default table are kept");
    if (!contains(g_cfg, "keep-me"))
        printf("---- config file ----\n%.1000s\n---------------------\n", g_cfg);

    /* ④ 擦完还能用：幂等 ＋ 主控活着 */
    rc = call(&fd, "{\"m\":\"sys.factory_reset\",\"p\":{\"confirm\":true}}",
              rsp2, sizeof rsp2);
    chk(rc == IV_OK && contains(rsp2, "\"ok\":true"), "f14: a 2nd reset still answers ok");
    chk(contains(rsp2, "\"cfg_changed\":0"),
        "f15: the 2nd reset changes nothing (idempotent)");
    rc = call(&fd, "{\"m\":\"system.snapshot\"}", rsp3, sizeof rsp3);
    /* 注意：`system.snapshot` 的应答**没有** `ok` 字段（形状是
     * {"method":..,"version":..,"status":"ok"}），别照抄其它方法的断言。 */
    chk(rc == IV_OK && contains(rsp3, "\"status\":\"ok\""),
        "f16: the daemon still serves other methods (config reload did not break it)");
    if (rc != IV_OK || !contains(rsp3, "\"status\":\"ok\""))
        printf("---- snapshot rc=%d, %.600s ----\n", rc, rsp3);

    if (fd >= 0)
        (void)close(fd);
    (void)stop_child(pid);

    if (g_fail == 0)
        printf("test_factory_e2e passed (both erase roots wired, media+log wiped, "
               "config back to defaults on disk, idempotent, daemon alive)\n");
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
            printf("test_factory_e2e SKIPPED: %s not found "
                   "(build with `make host`, or set IV_IVSBOXD_BIN)\n", bin);
            return 0;
        }
    }

    if (getenv(NS_ENV) == NULL) {
        if (!ns_mount_available()) {
            printf("SKIP: private user+mount ns unavailable (`unshare -rm`), "
                   "cannot run a real ivsboxd against a private /mnt & /var/run\n");
            return 0;
        }
        return reexec_in_ns(argv[0]);
    }
    return scenario(bin);
}
