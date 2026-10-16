/*
 * ivsbox-web 账号 / 改密 / 出厂默认口令闸门 —— 集成测试（M10，2026-10-16）
 *
 * ============================ 为什么必须是集成测试 ============================
 * 本步的三条硬要求都落在"进程真的起来一次 + 真的走一趟 HTTP"这个层面上：
 *   ① `must_change` 是由**启动期有没有读到 `<secure>/web.passwd`** 决定的 —— 不跑进程
 *      就只能在测试里假设它；
 *   ② 改密要**落盘**，而"改完重启仍生效、出厂口令不再能用"这条只有**真的重启一次
 *      进程**才验得了。在单测里 mock 一个文件系统，恰好把最该验的那一步（真写盘、
 *      真读回）换成了自己的假设；
 *   ③ 文件权限口径（非 0600 拒收）只有真文件才有意义。
 * 所以本用例自己 fork/exec `ivsbox-web`，用裸 socket 按字节驱动，并**重启两次**进程
 * （一次验"新口令活过重启"，一次验"宽权限文件被拒收 ⇒ 退回出厂口径"）。
 *
 * ============================ 不需要 ivsboxd ============================
 * 覆盖的路径（登录、改密、闸门）全在 Web 进程内闭环。只有 p06 会碰一下本地通道，
 * 而通道失败也是 200 + `{"ok":false}`，不影响断言 —— 本用例在 CI 里不会因"没起主控"
 * 假红。
 *
 * ============================ 用法 ============================
 *   test_web_passwd [ivsbox-web 二进制路径]
 * 省略时依次尝试环境变量 `IV_WEB_BIN`、`build/host/ivsbox-web`。二进制不存在 ⇒
 * **明确报 SKIP 并以 0 退出**（不给含义模糊的失败）。
 * 本用例只 kill 自己拉起的子进程，收尾删掉自己在 /tmp 下的临时目录（AGENTS 规则 6）。
 *
 * ⚠ 本文件里有**中文字符串字面量**（要逐字比对服务端回的提示语）。它们只在
 *   `strstr` 里用，**不进任何 printf/fprintf** —— 控制台输出一律 ASCII，
 *   否则 GBK 终端下抓下来的日志没法看。断言标签同理全用 ASCII。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS    5000
#define CONNECT_RETRY 5000
#define CONNECT_STEP  100
#define RSP_CAP       32768

/* 出厂口径（与 web/src/iv_web.h 的 IV_WEB_USER_DEF / IV_WEB_PASS_DEF 同源） */
#define DEF_USER "admin"
#define DEF_PASS "88888888"

/* 服务端回的提示语（JSON 里换行是两字符转义），**逐字**来自参考实现
 * web_server/makefsdata-4g/fs/index.shtml:976 的 alert()。改一个字这里就必须红。 */
static const char *HINT_JSON =
    "请输入正确的密码格式：\\n密码长度:8-12\\n密码由数字+大写英文字母+小写英文字母组成";

static int   g_fail;
static pid_t g_child = -1;

static char g_rsp[RSP_CAP];
static char g_req[4096];
static char g_tok[64];

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static void on_sig(int sig)
{
    (void)sig;
}

/* --------------------------------------------------------------------------
 * 起停被测进程
 * -------------------------------------------------------------------------- */
static int find_free_port(void)
{
    struct sockaddr_in a;
    socklen_t          alen = sizeof(a);
    int                fd = socket(AF_INET, SOCK_STREAM, 0);
    int                port = -1;

    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &alen) == 0)
        port = (int)ntohs(a.sin_port);
    (void)close(fd);
    return port;
}

/*
 * 拉起 ivsbox-web。**显式给 `--pass-file`**：本用例要验落盘，不能让被测进程去写
 * 板上的 /mnt/UDISK/ivsbox/secure。另给 `--heartbeat-ms 0`（关掉 /tmp 心跳，
 * 免得多个用例互相踩）与 `--cam-port 0`（不占 8081，免得与 cam 用例抢端口）。
 */
static int spawn_web(const char *bin, int port, const char *pass_file)
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0) {
            (void)dup2(devnull, 1);
            (void)dup2(devnull, 2);
            if (devnull > 2)
                (void)close(devnull);
        }
        {
            char pbuf[16];

            (void)snprintf(pbuf, sizeof(pbuf), "%d", port);
            (void)execl(bin, bin, "--port", pbuf, "--root", ".",
                        "--user", DEF_USER, "--pass", DEF_PASS,
                        "--pass-file", pass_file, "--heartbeat-ms", "0",
                        "--cam-port", "0", (char *)NULL);
        }
        _exit(127);
    }
    return (int)pid;
}

static void stop_web(pid_t pid)
{
    int st = 0;
    int waited = 0;

    if (pid <= 0)
        return;
    (void)kill(pid, SIGTERM);
    while (waitpid(pid, &st, WNOHANG) == 0 && waited < 2000) {
        sleep_ms(100);
        waited += 100;
    }
    if (waited >= 2000) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &st, 0);
    }
}

/* --------------------------------------------------------------------------
 * 裸 socket 往返
 * -------------------------------------------------------------------------- */
static int connect_retry(int port)
{
    int waited = 0;

    while (waited <= CONNECT_RETRY) {
        struct sockaddr_in a;
        int                fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd >= 0) {
            memset(&a, 0, sizeof(a));
            a.sin_family      = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port        = htons((uint16_t)port);
            if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
                return fd;
            (void)close(fd);
        }
        sleep_ms(CONNECT_STEP);
        waited += CONNECT_STEP;
    }
    return -1;
}

static int exchange(int port, const char *req, size_t len, int *status)
{
    struct timeval tv;
    size_t         sent = 0;
    size_t         total = 0;
    int            fd = connect_retry(port);

    *status = -1;
    if (fd < 0)
        return -1;
    tv.tv_sec  = TIMEOUT_MS / 1000;
    tv.tv_usec = (TIMEOUT_MS % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    while (sent < len) {
        ssize_t n = send(fd, req + sent, len - sent, MSG_NOSIGNAL);

        if (n <= 0)
            goto out;
        sent += (size_t)n;
    }
    for (;;) {
        ssize_t n;

        if (total + 1u >= RSP_CAP)
            break;
        n = recv(fd, g_rsp + total, RSP_CAP - total - 1u, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
out:
    g_rsp[total] = '\0';
    (void)close(fd);
    if (total > 12u && memcmp(g_rsp, "HTTP/1.1 ", 9u) == 0)
        *status = atoi(g_rsp + 9);
    return (int)total;
}

static int has(const char *s)
{
    return strstr(g_rsp, s) != NULL;
}

static int get_token(char *out, size_t cap)
{
    const char *p = strstr(g_rsp, "ivs_sid=");
    size_t      i = 0;

    out[0] = '\0';
    if (p == NULL)
        return 0;
    p += 8;
    while (p[i] != '\0' && p[i] != ';' && p[i] != '\r' && i + 1u < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0u;
}

static int build_login(char *buf, size_t cap, const char *user, const char *pass)
{
    char body[160];
    int  blen = snprintf(body, sizeof(body),
                         "{\"user\":\"%s\",\"pass\":\"%s\"}", user, pass);
    int  n = snprintf(buf, cap,
                      "POST /api/login HTTP/1.1\r\nHost: x\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);

    memcpy(buf + n, body, (size_t)blen);
    return n + blen;
}

/* 带会话 Cookie 的请求；body 可为 NULL（无体）。 */
static int build_api(char *buf, size_t cap, const char *method, const char *path,
                     const char *token, const char *body)
{
    int blen = (body != NULL) ? (int)strlen(body) : 0;
    int n;

    if (blen > 0) {
        n = snprintf(buf, cap,
                     "%s %s HTTP/1.1\r\nHost: x\r\n"
                     "Content-Type: application/json\r\n"
                     "Cookie: ivs_sid=%s\r\nContent-Length: %d\r\n"
                     "Connection: close\r\n\r\n", method, path, token, blen);
        memcpy(buf + n, body, (size_t)blen);
        return n + blen;
    }
    return snprintf(buf, cap, "%s %s HTTP/1.1\r\nHost: x\r\n"
                     "Cookie: ivs_sid=%s\r\nConnection: close\r\n\r\n",
                     method, path, token);
}

static int do_login(int port, const char *user, const char *pass, int *status)
{
    int len = build_login(g_req, sizeof(g_req), user, pass);

    g_tok[0] = '\0';
    len = exchange(port, g_req, (size_t)len, status);
    if (len > 0 && *status == 200)
        (void)get_token(g_tok, sizeof(g_tok));
    return len;
}

static int api_call(int port, const char *method, const char *path,
                    const char *token, const char *body, int *status)
{
    int len = build_api(g_req, sizeof(g_req), method, path, token, body);

    return exchange(port, g_req, (size_t)len, status);
}

/* 造一个文件并显式 fchmod（不受 umask 影响）。 */
static int write_file(const char *path, const char *content, mode_t mode)
{
    int    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    size_t n  = strlen(content);

    if (fd < 0)
        return -1;
    if (write(fd, content, n) != (ssize_t)n) {
        (void)close(fd);
        return -1;
    }
    (void)fchmod(fd, mode);
    (void)close(fd);
    return 0;
}

/* -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    const char *bin = NULL;
    char        dir[160];
    char        passfile[192];
    int         port;
    int         status;
    int         i;

    if (argc > 1)
        bin = argv[1];
    if (bin == NULL)
        bin = getenv("IV_WEB_BIN");
    if (bin == NULL)
        bin = "build/host/ivsbox-web";

    {
        struct stat st;

        if (stat(bin, &st) != 0) {
            printf("test_web_passwd SKIPPED: %s not found "
                   "(build with `make host`, or set IV_WEB_BIN)\n", bin);
            return 0;
        }
    }

    port = find_free_port();
    if (port <= 0) {
        fprintf(stderr, "FAIL: no free port\n");
        return 1;
    }

    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGCHLD, on_sig);

    (void)snprintf(dir, sizeof(dir), "/tmp/wb-webpass-%d", (int)getpid());
    (void)snprintf(passfile, sizeof(passfile), "%s/web.passwd", dir);
    if (mkdir(dir, 0700) != 0) {
        fprintf(stderr, "FAIL: cannot mkdir %s\n", dir);
        return 1;
    }
    (void)unlink(passfile); /* 确保"出厂状态"＝没有密码文件 */

    g_child = (pid_t)spawn_web(bin, port, passfile);
    if (g_child < 0) {
        fprintf(stderr, "FAIL: cannot fork ivsbox-web\n");
        return 1;
    }

    /* ---------------- p01 出厂账号/口令登录：200 + must_change ---------------- */
    {
        char tok_def[64];

        (void)do_login(port, DEF_USER, DEF_PASS, &status);
        chk(status == 200, "p01a: factory user/pass login -> 200");
        chk(has("\"must_change\":true"), "p01b: login body says must_change=true");
        chk(has("Set-Cookie: ivs_sid="), "p01c: login issues a session cookie");
        chk(g_tok[0] != '\0', "p01d: session token parsed");
        (void)snprintf(tok_def, sizeof(tok_def), "%s", g_tok);

        /* ---------------- p02 账号错：401，且与"口令错"同一句话 ---------------- */
        (void)do_login(port, "root", DEF_PASS, &status);
        chk(status == 401, "p02a: wrong account -> 401");
        chk(has("bad password"), "p02b: same message as wrong password (no account probing)");

        /* ---------------- p03 待改密闸门：除改密/登出外一律 403 ---------------- */
        (void)api_call(port, "GET", "/api/overview", tok_def, NULL, &status);
        chk(status == 403, "p03a: must_change -> /api/overview 403");
        chk(has("must change password"), "p03b: 403 explains why");
        (void)api_call(port, "GET", "/api/config?all", tok_def, NULL, &status);
        chk(status == 403, "p03c: must_change -> /api/config 403 as well");
        (void)api_call(port, "GET", "/api/devices", tok_def, NULL, &status);
        chk(status == 403, "p03d: must_change -> /api/devices 403");
        (void)api_call(port, "GET", "/media/x.mp4", tok_def, NULL, &status);
        chk(status == 403, "p03e: must_change -> /media 403 (recordings too)");
        /* 登出仍在放行名单里。**另开一个会话去登出**：登出会作废 token，
         * 而后面 p04~p06 还要用 tok_def，别把它弄掉。 */
        (void)do_login(port, DEF_USER, DEF_PASS, &status);
        (void)api_call(port, "POST", "/api/logout", g_tok, "{}", &status);
        chk(status == 200, "p03f: logout is still allowed while must_change");

        /* ---------------- p04 复杂度不合规：400 + 逐字提示 ---------------- */
        {
            static const char *bad[] = {
                "Ab1",              /* 太短（3 位） */
                "abcdefgh1",        /* 无大写 */
                "ABCDEFGH1",        /* 无小写 */
                "Abcdefgh",         /* 无数字 */
                "Abcdefghijkl1",    /* 太长（13 位） */
            };
            char body[128];

            for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
                (void)snprintf(body, sizeof(body), "{\"new\":\"%s\"}", bad[i]);
                (void)api_call(port, "POST", "/api/passwd", tok_def, body, &status);
                if (status != 400) {
                    fprintf(stderr, "FAIL: p04a: weak password #%d not rejected (status %d)\n",
                            i, status);
                    g_fail++;
                }
                chk(has(HINT_JSON), "p04b: rejection carries the exact hint text");
            }
            /* 控制字符（JSON 里写成 \\t 转义）：参考实现的正则 `[\\s\\S]{8,12}` 会收下它，
             * 但我们要把口令写进行式文件，'\t' 之类会让格式坏掉 ⇒ 本工程**刻意判不合格**。
             * 这条差异写在 web/src/iv_web_pass.c 的文件头，用例在此钉住它。 */
            (void)api_call(port, "POST", "/api/passwd", tok_def,
                           "{\"new\":\"Abcd123\\t\"}", &status);
            chk(status == 400, "p04c: control chars are rejected (deliberate, not in reference)");
        }

        /* ---------------- p05 强制改密：不需要 old（刚用出厂口令登进来） ---------------- */
        (void)api_call(port, "POST", "/api/passwd", tok_def,
                       "{\"old\":\"\",\"new\":\"Iv2026xy\"}", &status);
        chk(status == 200, "p05a: forced change without old -> 200");

        /* ---------------- p06 闸门解除：同一会话立刻可用 ---------------- */
        (void)api_call(port, "GET", "/api/overview", tok_def, NULL, &status);
        chk(status == 200, "p06a: gate lifted after the change");
        (void)api_call(port, "POST", "/api/passwd", tok_def, "{\"new\":\"Zz2026ab\"}", &status);
        chk(status == 400, "p06b: voluntary change without old -> 400 old password required");
        chk(has("old password required"), "p06c: says which field is missing");

        /* ---------------- p07 落盘：0600 且**不含明文** ---------------- */
        {
            struct stat sb;
            char        buf[256];
            int         fd;
            ssize_t     n;

            chk(stat(passfile, &sb) == 0, "p07a: password file written");
            if (stat(passfile, &sb) == 0)
                chk((sb.st_mode & 07777) == 0600, "p07b: mode is 0600");
            fd = open(passfile, O_RDONLY);
            buf[0] = '\0';
            n = (fd >= 0) ? read(fd, buf, sizeof(buf) - 1u) : -1;
            if (fd >= 0)
                (void)close(fd);
            if (n > 0)
                buf[n] = '\0';
            chk(n > 0 && strncmp(buf, "v1 ", 3) == 0, "p07c: file has the v1 header");
            chk(n > 0 && strstr(buf, "Iv2026xy") == NULL, "p07d: no plaintext password in file");
        }

        /* ---------------- p08 旧口令作废 / 新口令可用 ---------------- */
        (void)do_login(port, DEF_USER, DEF_PASS, &status);
        chk(status == 401, "p08a: factory password no longer works");
        (void)do_login(port, DEF_USER, "Iv2026xy", &status);
        chk(status == 200, "p08b: new password works");
        chk(has("\"must_change\":false"), "p08c: must_change cleared");
    }

    /* ---------------- p09 重启：新口令必须活下来 ---------------- */
    stop_web(g_child);
    g_child = (pid_t)spawn_web(bin, port, passfile);
    chk(g_child > 0, "p09a: respawned");
    (void)do_login(port, DEF_USER, "Iv2026xy", &status);
    chk(status == 200, "p09b: new password survives a restart (really persisted)");
    chk(has("\"must_change\":false"), "p09c: must_change stays false after restart");
    {
        char tok2[64];

        (void)snprintf(tok2, sizeof(tok2), "%s", g_tok);
        /* ---------------- p10 主动改密必须给对 old ---------------- */
        (void)api_call(port, "POST", "/api/passwd", tok2,
                       "{\"old\":\"WRONGone9\",\"new\":\"Zz2026ab\"}", &status);
        chk(status == 401, "p10a: wrong old -> 401");
        (void)api_call(port, "POST", "/api/passwd", tok2,
                       "{\"old\":\"Iv2026xy\",\"new\":\"Zz2026ab\"}", &status);
        chk(status == 200, "p10b: correct old -> 200");
        (void)do_login(port, DEF_USER, "Iv2026xy", &status);
        chk(status == 401, "p10c: replaced password no longer works");
        (void)do_login(port, DEF_USER, "Zz2026ab", &status);
        chk(status == 200, "p10d: the newest password works");

        /* ---------------- p11 无会话 / 方法限制 ---------------- */
        (void)api_call(port, "POST", "/api/passwd", "", "{\"new\":\"Zz2026ab\"}", &status);
        chk(status == 401, "p11a: /api/passwd without a session -> 401");
        {
            char tok3[64];

            (void)snprintf(tok3, sizeof(tok3), "%s", g_tok);
            (void)api_call(port, "GET", "/api/passwd", tok3, NULL, &status);
            chk(status == 405, "p11b: GET /api/passwd -> 405");
        }
    }

    /* ---------------- p12 宽权限文件拒收 ⇒ 退回出厂口径 ---------------- */
    stop_web(g_child);
    g_child = -1;
    {
        char fake[128];

        /* 造一份"看着合法"的 v1 文件但 0644：服务端必须**整体不采信**。
         * 若它悄悄采信了，重启后 must_change 就会是 false（与出厂口径不符）⇒ p12b 红。 */
        (void)snprintf(fake, sizeof(fake), "v1 %s %s\n", DEF_USER,
                       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        chk(write_file(passfile, fake, 0644) == 0, "p12a: seeded a 0644 password file");
        g_child = (pid_t)spawn_web(bin, port, passfile);
        chk(g_child > 0, "p12b: respawned with the loose file");
        (void)do_login(port, DEF_USER, DEF_PASS, &status);
        chk(status == 200, "p12c: falls back to the factory credentials -> 200");
        chk(has("\"must_change\":true"), "p12d: loose file refused -> must_change again");
        {
            struct stat sb;

            /* 而且不得顺手把它改写成 0600（那等于"采信后再修"） */
            if (stat(passfile, &sb) == 0)
                chk((sb.st_mode & 07777) == 0644, "p12e: loose file left untouched");
        }
    }

    /* ---------------- 收尾 ---------------- */
    stop_web(g_child);
    (void)unlink(passfile);
    (void)rmdir(dir);

    if (g_fail != 0) {
        fprintf(stderr, "test_web_passwd FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_web_passwd passed (account, factory-password gate, complexity rule, "
           "persistence across restart, 0600 enforcement)\n");
    return 0;
}
