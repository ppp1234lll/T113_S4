/*
 * S6.5 前置「配置接线」端到端 —— 集成测试（配置 → main → iv_agent → iv_recctl）
 *
 * ============================ 它验证什么 ============================
 * 单测能覆盖"配置层存得下 8 路"（test_config）、"状态机跳过空串路"（test_recctl），
 * 但**覆盖不到装配**：把 `camera.ch.N.url` / `.record` 与 `camera.rec.hold_ms`
 * / `.debounce_ms` 从配置里读出来、组成录像源表再交给状态机的，是
 * `src/app/main.c` 的 `cfg_rec_sources()` —— app 层 `.c` **不进静态库**，单测链接
 * 不到它。这类"接线"缺陷只有真拉起主控进程才抓得到（与 M4-S4.4 "两种驱动模式
 * 装配不一致"、S3.6 "on_done 没挂上" 同族）。
 *
 * 做法：在私有 user＋mount ns 里把 `/mnt` 与 `/var/run` 挂成 tmpfs，写一份**含
 * 8 路 `camera.ch` 数组**的 `ivsbox.json`，真拉起 `build/host/ivsboxd`，再从它落盘
 * 的日志里断言 agent 装配那一刻打的那行：
 *
 *     media bridge up, rec urls=<有效路数>/<表长> hold=<ms> debounce=<ms>
 *
 * **一行日志同时钉死四处**：
 *   ① `camera.ch` 的 **8 个元素**必须被收下 ⇒ 表长 = 8（`IV_CFG_ARR_MAX` 6→8 的
 *      端到端证据；若被当超长截断，第 8 路那条 URL 会消失、有效路数变 0）；
 *   ② 只有 **url 非空 且 record 为真** 的那一路进表 ⇒ 有效路数 = 1（本例另设两路
 *      反例：只配 url、只配 record）；
 *   ③ hold/debounce 取自 `camera.rec.*`（本例 7000/20000，**与出厂默认不同**，
 *      所以"没接线"与"接了但取默认"能被区分开）；
 *   ④ 8 路数组**没有**被判超长拒收（日志里不得出现 `extras rejected`）。
 *
 * ============================ 反向证伪锚点 ============================
 *   f1 不调 `cfg_rec_sources()`（接线断）      ⇒ 表长 0 ⇒ ①②③ 全 FAIL
 *   f2 `record` 判定去掉（只看 url 非空）      ⇒ 有效路数 2 ⇒ ② FAIL
 *   f3 `IV_CFG_ARR_MAX` 改回 6                 ⇒ 第 8 路被截断 ⇒ ② FAIL
 *   f4 `camera.rec.*` 不读（回落默认 10/60 s） ⇒ ③ FAIL
 *   变异必须**编译干净**（撞 `-Werror` 的 BUILD FAILED 不算证据）。
 *
 * ============================ 跑法与降级 ============================
 *   - 宿主机：`make itest`（本文件属 `ITEST_SRCS` 通配，自动纳入）。
 *   - 需要 `build/host/ivsboxd`（先 `make host`）；缺了打印 SKIP 并退出 0。
 *   - `unshare -rm` 或 tmpfs 挂载不可用（板端无 unshare）⇒ SKIP 退出 0：环境受限
 *     不该算代码错（与 test_agent_switch / test_web_http 同口径）。
 *   - 只用 ns 内的 `/var/run` 与 `/mnt`，退出即消失，不与宿主机既有进程抢资源。
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
#include <unistd.h>

#define NS_ENV      "IV_CFG_MEDIA_NS"
#define LOG_ROOT    "/mnt/UDISK/log"
#define CFG_DIR     "/mnt/UDISK/ivsbox/config"
#define CFG_FILE    CFG_DIR "/ivsbox.json"
#define CTL_DIR     "/var/run/ivsbox"
#define WAIT_MS     30000 /* 等主控起来并打出装配日志的上限 */

static int g_fail = 0;

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
            return -1; /* 已存在不是错；其余错误留给 stat 判定 */
        buf[i] = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;
    return dir_ok(buf) ? 0 : -1;
}

/* 把 dir 挂成 tmpfs。**必须在私有 user+mount ns 里调用**（否则没权限）。
 * 返回 0 = 成功；调用方把失败当 SKIP（环境受限），不当代码错。 */
static int mount_tmpfs(const char *dir)
{
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;
    if (!dir_ok(dir))
        return -1;
    if (mount("none", dir, "tmpfs", 0, "mode=0755") == 0)
        return 0;
    return errno == EBUSY ? 0 : -1; /* EBUSY = 已经被挂上了，可接受 */
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

/* 读一个文件，追加进 buf（buf 视作已 NUL 结尾的字符串）。返回写入字节数。 */
static int append_file(char *buf, size_t cap, const char *path)
{
    FILE *f = fopen(path, "r");
    size_t used = strlen(buf);
    size_t got;

    if (f == NULL || used + 1u >= cap)
        return 0;
    got = fread(buf + used, 1u, cap - used - 1u, f);
    buf[used + got] = '\0';
    (void)fclose(f);
    return (int)got;
}

/* 汇总 /mnt/UDISK/log/<日期>/<小时>.log 的全部内容（只有一个文件时也走这条路）。 */
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

static void sleep_ms(int ms)
{
    (void)usleep((useconds_t)ms * 1000u);
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
 * 场景本体（已在 ns 内）
 * ------------------------------------------------------------------------- */
/* 8 路的用例矩阵（**顺序即路号**）：
 *   0 只给 url（record 出厂默认 false）      ⇒ 应被排除
 *   1 只给 record=true（无 url）             ⇒ 应被排除
 *   2~6 只给识别字段（不录像）               ⇒ 应被排除
 *   7 给 url ＋ record=true                  ⇒ **唯一进表** 的那一路
 * 数组长度 8 ⇒ 顺带钉死 `IV_CFG_ARR_MAX`＝8（6 时代第 8 路会被截断）。 */
static const char CFG_TEXT[] =
    "{\"_meta\":{\"version\":1},\n"
    " \"camera\":{\n"
    "   \"rec\":{\"hold_ms\":7000,\"debounce_ms\":20000},\n"
    "   \"ch\":[\n"
    "     {\"url\":\"rtsp://u0:p0@127.0.0.1:554/Streaming/Channels/101\"},\n"
    "     {\"record\":true},\n"
    "     {\"vendor\":\"hik\",\"ip\":\"10.0.0.9\"},\n"
    "     {\"vendor\":\"dahua\"},\n"
    "     {\"vendor\":\"uniview\"},\n"
    "     {\"vendor\":\"brand5\"},\n"
    "     {\"vendor\":\"brand6\"},\n"
    "     {\"url\":\"rtsp://admin:pw@127.0.0.1:554/Streaming/Channels/102\","
    "\"record\":true}\n"
    "   ]}\n"
    "}\n";

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
    static char log[64 * 1024];
    pid_t       pid;
    int         waited = 0;
    int         found  = 0;

    /* ---- 0. 私有挂载点 ---- */
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

    /* ---- 1. 写 8 路配置 ---- */
    chk(write_file(CFG_FILE, CFG_TEXT) == 0, "write ivsbox.json (8 camera slots)");

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

    /* ---- 3. 等装配日志（agent 起来那一刻就打） ---- */
    while (waited < WAIT_MS) {
        int   st = 0;
        pid_t r;

        if (read_log(log, sizeof log) && contains(log, "media bridge up, rec urls=")) {
            found = 1;
            break;
        }
        /* 主控提前退出（配置被判非法 / 端口冲突…）⇒ 立刻失败并打印日志尾巴，
         * 不要傻等满 30 s。 */
        r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            g_fail++;
            printf("FAIL: ivsboxd exited before the media bridge came up (exit=%d)\n",
                   WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            (void)read_log(log, sizeof log);
            printf("---- log tail ----\n%.2000s\n------------------\n", log);
            return 1;
        }
        sleep_ms(200);
        waited += 200;
    }

    if (!found) {
        g_fail++;
        printf("FAIL: no 'media bridge up' line within %d ms\n", WAIT_MS);
        (void)read_log(log, sizeof log);
        printf("---- log tail ----\n%.2000s\n------------------\n", log);
    } else {
        /* ① + ② + ③：8 路数组收下（表长 8）、只有 url＋record 那一路进表（有效 1）、
         *             时间参数取自 camera.rec.*（7000/20000 ≠ 出厂 10000/60000）。 */
        chk(contains(log, "media bridge up, rec urls=1/8 hold=7000ms debounce=20000ms"),
            "agent took 8 slots, 1 effective URL, hold/debounce from camera.rec.*");
        /* ④ 8 路没被当超长数组拒收 */
        chk(!contains(log, "extras rejected"),
            "an 8-element camera.ch array is NOT treated as oversized");
    }

    (void)stop_child(pid);

    if (g_fail == 0)
        printf("test_cfg_media_e2e passed (8 slots accepted, url+record AND, "
               "hold/debounce from config, wired through the real ivsboxd)\n");
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
            printf("test_cfg_media_e2e SKIPPED: %s not found "
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
