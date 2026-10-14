/*
 * 集成用例：ivsbox-provision 设备侧 × 工装侧 端到端（M6/S5.5 协议定稿）
 *
 * 与 test_media_e2e 同模式：**自包含、零外部前置** —— 自己 fork+execl 拉起
 * `build/host/ivsbox-provision` 的设备侧（`--dry-run`，不写裸分区），自己扮演
 * 工装侧（回环 UDP ＋ 直接用 libivmodules 的组包函数），验的是"真进程真链路"：
 *   e01 搜索：发 search（tid=000000）→ 设备**回** search 应答，且设备信息字段
 *       （tid/tnam/mod/mac/ip）与启动参数一致；
 *   e02 配置：发 update_mac（tid=12DEFF ＋ 文档样例的 `-` 分隔 MAC）→ 设备回
 *       result=success 且 tid 已变成新的 12DEFF（dry-run 下"将写入"的值）；
 *   e03 坏 MAC：回 result=fault（且**不影响**后续请求 —— 进程不被坏输入带崩）；
 *   e04 未实现的指令（update_relay）：**一个字节都不回**（协议要求静默）；
 *   e05 垃圾数据：静默丢弃，之后仍能正常应答（证明解析器没有"收到脏包就罢工"）。
 *
 * 为什么用 `--reply-unicast`：VM 上组播回包依赖路由与多播支持，工装侧收不到就
 * 会变成"环境假红"；改成单播回请求来源，链路确定性由测试掌握（产线默认仍发组播，
 * 该开关只是回包地址不同，协议语义不变）。
 *
 * SKIP 语义（退出 0）：二进制缺失、端口全被占（连着试 37201~37210）。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_provision.h"
#include "ivsbox/iv_ret.h"

#define PROV_BIN "build/host/ivsbox-provision"

static pid_t g_child = -1;
static int   g_pipe = -1; /* 子进程 stdout 读端（只为等"已就绪"那一行） */
static char  g_dir[160];

static void noop_sink(int level, const char *module, const char *line, void *user)
{
    (void)level; (void)module; (void)line; (void)user;
}

static void cleanup(void)
{
    if (g_child > 0) {
        (void)kill(g_child, SIGTERM);
        (void)waitpid(g_child, NULL, 0);
        g_child = -1;
    }
    if (g_pipe >= 0) {
        (void)close(g_pipe);
        g_pipe = -1;
    }
}

/* 拉起设备侧；返回 0（ok）/ -1（起不来） */
static int spawn_device(int port)
{
    int   pfd[2];
    pid_t pid;
    char  port_s[16];
    char  sec[220];

    if (pipe(pfd) != 0)
        return -1;
    (void)snprintf(port_s, sizeof(port_s), "%d", port);
    (void)snprintf(sec, sizeof(sec), "%s/secure", g_dir);

    pid = fork();
    if (pid < 0) {
        (void)close(pfd[0]);
        (void)close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        /* 子进程：stdout/stderr 都接到管道，避免污染测试输出 */
        (void)close(pfd[0]);
        (void)dup2(pfd[1], STDOUT_FILENO);
        (void)dup2(pfd[1], STDERR_FILENO);
        if (pfd[1] != STDOUT_FILENO && pfd[1] != STDERR_FILENO)
            (void)close(pfd[1]);
        (void)execl(PROV_BIN, "ivsbox-provision",
                    "--ip", "127.0.0.1",          /* 非组播地址 ⇒ 不做 join */
                    "--port", port_s,
                    "--local-port", port_s,
                    "--force",                    /* 工厂标记不参与本用例 */
                    "--reply-unicast",            /* 回请求来源，链路确定 */
                    "--dry-run",                  /* 不写裸分区 */
                    "--timeout", "60",
                    "--tid", "1234FF",
                    "--mac", "3a:82:b4:1d:3c:bf",
                    "--tnam", "E2E",
                    "--mod", "FN-E2E",
                    "--iface", "lo",
                    "--secure", sec,
                    (char *)NULL);
        _exit(127);
    }
    (void)close(pfd[1]);
    g_child = pid;
    g_pipe  = pfd[0];
    return 0;
}

/* 等子进程打出"listening"行；若它先退出（端口被占）返回 -1 */
static int wait_ready(int timeout_ms)
{
    time_t t0 = time(NULL);

    for (;;) {
        struct pollfd p;
        char          buf[512];
        ssize_t       n;

        if ((int)((time(NULL) - t0) * 1000) > timeout_ms)
            return -1;
        p.fd = g_pipe;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 100) <= 0) {
            int st;

            if (waitpid(g_child, &st, WNOHANG) == g_child)
                return -1; /* 已退出 ⇒ 起不来 */
            continue;
        }
        n = read(g_pipe, buf, sizeof(buf) - 1u);
        if (n <= 0)
            return -1;
        buf[n] = '\0';
        if (strstr(buf, "listening") != NULL)
            return 0;
    }
}

/* 收一条 UDP 应答，返回长度；超时返回 -1 */
static int recv_reply(int fd, char *out, size_t cap, int timeout_ms)
{
    struct pollfd p;
    ssize_t       n;

    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, timeout_ms) <= 0)
        return -1;
    n = recvfrom(fd, out, cap - 1u, 0, NULL, NULL);
    if (n <= 0)
        return -1;
    out[n] = '\0';
    return (int)n;
}

static int send_to_dev(int fd, int port, const char *msg, int len)
{
    struct sockaddr_in dst;

    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port        = htons((uint16_t)port);
    return (sendto(fd, msg, (size_t)len, 0, (struct sockaddr *)&dst, sizeof(dst)) == len)
               ? 0 : -1;
}

int main(void)
{
    char     tmpl[] = "/tmp/ivprovE2E_XXXXXX";
    int      fd;
    int      port = 0;
    int      i;
    int      fail = 0;
    char     json[IV_PROV_JSON_MAX];
    char     reply[IV_PROV_JSON_MAX];
    int      n;

    iv_log_set_sink(noop_sink, NULL);

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    (void)snprintf(g_dir, sizeof(g_dir), "%s", tmpl);

    if (access(PROV_BIN, X_OK) != 0) {
        printf("SKIP: %s not built\n", PROV_BIN);
        (void)rmdir(g_dir);
        return 0;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("SKIP: no udp socket\n");
        (void)rmdir(g_dir);
        return 0;
    }

    /* 挑一个能起来的端口（被占 ⇒ 换一个） */
    for (i = 0; i < 10; i++) {
        port = 37201 + i;
        if (spawn_device(port) == 0 && wait_ready(3000) == 0)
            break;
        cleanup();
        port = 0;
    }
    if (port == 0) {
        printf("SKIP: could not start device side on 37201..37210\n");
        (void)close(fd);
        (void)rmdir(g_dir);
        return 0;
    }
    printf("device side up on 127.0.0.1:%d\n", port);

    /* ---------------- e01 搜索 ---------------- */
    n = iv_prov_build_search_query(json, sizeof(json));
    if (n <= 0 || send_to_dev(fd, port, json, n) != 0) {
        fprintf(stderr, "FAIL: e01 send search\n");
        fail++;
    } else {
        iv_prov_msg_t m;

        n = recv_reply(fd, reply, sizeof(reply), 2000);
        if (n <= 0) {
            fprintf(stderr, "FAIL: e01 no search reply\n");
            fail++;
        } else if (iv_prov_msg_parse(reply, (size_t)n, &m) != IV_OK ||
                   strcmp(m.types, IV_PROV_TYPE_SEARCH) != 0) {
            fprintf(stderr, "FAIL: e01 reply not a search reply: %s\n", reply);
            fail++;
        } else if (strcmp(m.tid, "1234FF") != 0 || strcmp(m.mac, "3a:82:b4:1d:3c:bf") != 0) {
            fprintf(stderr, "FAIL: e01 reply identity mismatch: %s\n", reply);
            fail++;
        } else if (strstr(reply, "\"tnam\":\"E2E\"") == NULL ||
                   strstr(reply, "\"mod\":\"FN-E2E\"") == NULL ||
                   strstr(reply, "\"ip\":\"127.0.0.1\"") == NULL) {
            fprintf(stderr, "FAIL: e01 device-info fields missing: %s\n", reply);
            fail++;
        } else {
            printf("e01 ok: search answered (%s)\n", reply);
        }
    }

    /* ---------------- e02 配置设备 ID＋MAC ---------------- */
    n = iv_prov_build_update_mac_query("12DEFF", "02-00-00-44-6c-40", json, sizeof(json));
    if (n <= 0 || send_to_dev(fd, port, json, n) != 0) {
        fprintf(stderr, "FAIL: e02 send update_mac\n");
        fail++;
    } else {
        n = recv_reply(fd, reply, sizeof(reply), 2000);
        if (n <= 0) {
            fprintf(stderr, "FAIL: e02 no update_mac reply\n");
            fail++;
        } else if (strstr(reply, "\"result\":\"success\"") == NULL ||
                   strstr(reply, "\"tid\":\"12DEFF\"") == NULL ||
                   strstr(reply, "\"mac\":\"02:00:00:44:6c:40\"") == NULL) {
            fprintf(stderr, "FAIL: e02 unexpected reply: %s\n", reply);
            fail++;
        } else {
            printf("e02 ok: update_mac accepted, id+mac applied\n");
        }
    }

    /* ---------------- e03 坏 MAC ⇒ fault ---------------- */
    n = iv_prov_build_update_mac_query("12DEFF", "zz:00:00:44:6c:40", json, sizeof(json));
    if (n > 0 && send_to_dev(fd, port, json, n) == 0) {
        n = recv_reply(fd, reply, sizeof(reply), 2000);
        if (n <= 0 || strstr(reply, "\"result\":\"fault\"") == NULL) {
            fprintf(stderr, "FAIL: e03 expected fault, got %s\n", (n > 0) ? reply : "(none)");
            fail++;
        } else {
            printf("e03 ok: bad mac rejected with fault\n");
        }
    } else {
        fprintf(stderr, "FAIL: e03 send\n");
        fail++;
    }

    /* ---------------- e04 未实现指令 ⇒ 静默 ---------------- */
    (void)snprintf(json, sizeof(json),
                   "{\"info\":{\"tid\":\"1234FF\",\"date\":\"20230531\"},"
                   "\"data\":{\"types\":\"update_relay\",\"pa1\":\"1\"}}");
    if (send_to_dev(fd, port, json, (int)strlen(json)) != 0) {
        fprintf(stderr, "FAIL: e04 send\n");
        fail++;
    } else {
        n = recv_reply(fd, reply, sizeof(reply), 800);
        if (n > 0) {
            fprintf(stderr, "FAIL: e04 unimplemented command answered: %s\n", reply);
            fail++;
        } else {
            printf("e04 ok: unimplemented command stays silent\n");
        }
    }

    /* ---------------- e05 垃圾数据 ⇒ 静默且不罢工 ---------------- */
    (void)snprintf(json, sizeof(json), "{ this is not json at all ");
    (void)send_to_dev(fd, port, json, (int)strlen(json));
    n = recv_reply(fd, reply, sizeof(reply), 500);
    if (n > 0) {
        fprintf(stderr, "FAIL: e05 garbage answered\n");
        fail++;
    } else if (iv_prov_build_search_query(json, sizeof(json)) <= 0 ||
               send_to_dev(fd, port, json, (int)strlen(json)) != 0) {
        fprintf(stderr, "FAIL: e05 resend search\n");
        fail++;
    } else {
        n = recv_reply(fd, reply, sizeof(reply), 2000);
        if (n <= 0 || strstr(reply, "\"types\":\"search\"") == NULL) {
            fprintf(stderr, "FAIL: e05 device stopped answering after garbage\n");
            fail++;
        } else {
            printf("e05 ok: garbage ignored, device still alive\n");
        }
    }

    cleanup();
    (void)close(fd);
    (void)rmdir(g_dir);

    if (fail != 0) {
        fprintf(stderr, "test_prov_e2e FAILED (%d)\n", fail);
        return 1;
    }
    printf("test_prov_e2e passed (search / update_mac / fault / silence)\n");
    return 0;
}
