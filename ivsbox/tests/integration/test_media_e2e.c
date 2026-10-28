/*
 * 集成用例：ivsbox-media（收编 bird_media）× iv_media 桥接 端到端（M6-S6.1）
 *
 * 与 test_web_http 同模式：自己 fork+execl 拉起 build/host/ivsbox-media
 * （**不需要 ivsboxd**，媒体进程独立监听 18000），经桥接模块打真实接口：
 *   e01 media 进程起来后，record.status 能读到 0（HTTP 链路通）
 *   e02 record.start 到无效 RTSP：对端受理 → 数秒内 RTSP 连接失败自动停录
 *       → 保活轮询把会话表清掉（桥接↔对端状态同步的端到端证据）
 *   e03 capture：VM 无 /dev/video0 ⇒ 对端 200 但文件不在（或设备 open 失败）
 *       ⇒ 桥接判 fail；若环境恰好有摄像头则判 ok —— 只断言"请求被处理且
 *       结论与磁盘一致"，两种环境都不假红
 *   e04 会话表与统计的最终一致性
 *
 * SKIP 语义（退出 0）：二进制/静态目录缺失、18000 被占（已有 media 在跑）。
 * ⚠ 本用例固定端口 18000（bird_media 无命令行参数）；并行跑两份会假红。
 */
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_media.h"
#include "ivsbox/iv_ret.h"

static pid_t   g_srv_pid = -1;
static char    g_dir[160];
static iv_media_t g_m;

static void noop_sink(int level, const char *module, const char *line, void *user)
{
    (void)level; (void)module; (void)line; (void)user;
}

/* 18000 是否已有监听者（有 ⇒ SKIP） */
static int port_busy(int port)
{
    int                s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    int                rc;

    if (s < 0)
        return 1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons((uint16_t)port);
    rc = (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    close(s);
    return rc;
}

static int srv_up(void)
{
    int                s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    int                rc;

    if (s < 0)
        return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons(18000);
    rc = (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    close(s);
    return rc;
}

static int cond_sessions_zero(void *arg)
{
    return iv_media_sessions((const iv_media_t *)arg, NULL, NULL, 0) == 0;
}

static int cond_srv_up(void *arg)
{
    (void)arg;
    return srv_up();
}

static int wait_cond(int (*cond)(void *), void *arg, int ms)
{
    int waited = 0;

    while (!cond(arg)) {
        if (waited >= ms)
            return -1;
        usleep(50000);
        waited += 50;
    }
    return 0;
}

int main(void)
{
    const char     *bin  = "build/host/ivsbox-media";
    char            tmpl[] = "/tmp/ivme2e_XXXXXX";
    iv_media_cfg_t  cfg;
    iv_media_stats_t st;
    int              fail = 0;

#define CHK(cond, what) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", what); fail++; } } while (0)

    iv_log_set_sink(noop_sink, NULL);

    if (access(bin, X_OK) != 0) {
        printf("SKIP %s: %s missing (run `make host` first)\n", "test_media_e2e", bin);
        return 0;
    }
    if (port_busy(18000)) {
        printf("SKIP %s: port 18000 busy (ivsbox-media already running?)\n",
               "test_media_e2e");
        return 0;
    }
    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    (void)snprintf(g_dir, sizeof(g_dir), "%s", tmpl);

    g_srv_pid = fork();
    if (g_srv_pid < 0)
        return 1;
    if (g_srv_pid == 0) {
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }

    /* 等对端监听（bird_media 起得很快，给 5 s） */
    if (wait_cond(cond_srv_up, NULL, 5000) != 0) {
        int st2 = 0;
        waitpid(g_srv_pid, &st2, WNOHANG);
        fprintf(stderr, "FAIL: ivsbox-media did not listen on 18000 (exit=%d)\n", st2);
        kill(g_srv_pid, SIGKILL);
        waitpid(g_srv_pid, &st2, 0);
        return 1;
    }

    iv_media_cfg_default(&cfg);
    cfg.port         = 18000;
    cfg.ka_period_ms = 1000; /* 缩短保活，让 e02 的同步在秒级可见 */
    cfg.snap_dir     = g_dir;
    if (iv_media_init(&g_m, &cfg) != 0) {
        fprintf(stderr, "FAIL: bridge init\n");
        kill(g_srv_pid, SIGKILL);
        return 1;
    }

    /* e01 状态读数（HTTP 链路通） */
    {
        int rec = -1;
        CHK(iv_media_record_status(&g_m, 0, &rec) == IV_OK,
            "e01: status round-trip");
        CHK(rec == 0, "e01: channel 0 idle at start");
    }

    /* e02 无效 RTSP：受理 → 对端自动停 → 会话表被清
     * 2026-10-28：start 前会做 TCP 预检 ⇒ 地址的 host:port 必须**连得上**，
     * 否则会先被预检拦掉（那是 e02b 的覆盖）。这里指 media 自己的 18000：
     * TCP 通、但不是 RTSP，ffmpeg 握手失败后自动停录。 */
    CHK(iv_media_record_start(&g_m, 0, "rtsp://127.0.0.1:18000/nope", NULL, 0) == IV_OK,
        "e02: start accepted for bogus rtsp");
    /* e02b 预检拦截（2026-10-28 新行为）：连不上的地址在发 start 之前就被拒，
     * 并把原因写进 why —— 这是"录像没起来"能被现场读懂的前提。 */
    {
        char why[IV_MEDIA_WHY_MAX];

        why[0] = '\0';
        CHK(iv_media_record_start(&g_m, 1, "rtsp://127.0.0.1:1/nope", why,
                                  sizeof(why)) == IV_ECONN,
            "e02b: unreachable camera rejected by the precheck");
        CHK(why[0] != '\0', "e02b: precheck explains why in plain words");
    }
    {
        int rec = -1;
        CHK(iv_media_record_status(&g_m, 0, &rec) == IV_OK,
            "e02: status after start");
        /* bird_media 的 start 只置标志，此时状态可能还是 1；不断言其值 */
    }
    CHK(wait_cond(cond_sessions_zero, &g_m, 20000) == 0,
        "e02: peer auto-stopped on rtsp failure, keepalive synced session table");
    {
        int rec = -1;
        CHK(iv_media_record_status(&g_m, 0, &rec) == IV_OK && rec == 0,
            "e02: peer reports stopped");
    }

    /* e03 capture：结论与磁盘一致（VM 无摄像头 ⇒ fail；有 ⇒ ok） */
    {
        char path[IV_MEDIA_URL_MAX];
        uint64_t ok0, fail0;

        iv_media_stats(&g_m, &st);
        ok0   = st.snap_ok;
        fail0 = st.snap_fail;
        CHK(iv_media_capture_async(&g_m, NULL, path, sizeof(path)) == IV_OK,
            "e03: capture enqueued");
        {
            int waited = 0;
            for (;;) {
                iv_media_stats(&g_m, &st);
                if (st.snap_ok + st.snap_fail > ok0 + fail0)
                    break;
                if (waited >= 10000) {
                    CHK(0, "e03: capture result never arrived");
                    break;
                }
                usleep(50000);
                waited += 50;
            }
        }
        iv_media_stats(&g_m, &st);
        {
            struct stat sb;
            int on_disk = (stat(path, &sb) == 0);
            CHK(on_disk ? (st.snap_ok > ok0) : (st.snap_fail > fail0),
                "e03: verdict matches disk state");
            if (on_disk)
                (void)unlink(path); /* 规则 6：临时目录不留物 */
        }
    }

    /* e04 统计一致性 */
    iv_media_stats(&g_m, &st);
    CHK(st.snap_req >= 1, "e04: snap_req counted");
    CHK(st.rec_starts == 1 && st.rec_stops == 0, "e04: record counters");

    iv_media_fini(&g_m);
    kill(g_srv_pid, SIGTERM);
    {
        int st3;
        waitpid(g_srv_pid, &st3, 0);
    }
    (void)rmdir(g_dir); /* 规则 6（目录应为空） */

    if (fail != 0) {
        fprintf(stderr, "test_media_e2e FAILED (%d)\n", fail);
        return 1;
    }
    printf("test_media_e2e passed (status, rtsp-fail auto-stop sync, capture verdict, stats)\n");
    return 0;
}
