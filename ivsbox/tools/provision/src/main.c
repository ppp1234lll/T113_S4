/*
 * ivsbox-provision 出厂身份写入工具（M5-S5.5）
 *
 * ============================ 它怎么用 ============================
 * 产线上有两种角色，都是同一个二进制：
 *   1. **工装侧（发送）**：`--send --devid 0x1234 --mac xx:.. --server 1.2.3.4`
 *      把一条身份报文播到组播组；
 *   2. **设备侧（接收）**：不带 `--send` 就是监听模式，收下报文写进自己的
 *      `private` 裸分区与 `secure/` 目录。
 *
 * **只有工厂标记存在时才允许运行**（`/mnt/UDISK/ivsbox/factory/enable_provision`）。
 * 量产后把这个普通文件删掉，入口即关闭。`--force` 可以忽略标记，**只供验证用**，
 * 正常流程不要带。
 *
 * ============================ 为什么是组播而不是逐台连接 ============================
 * 用户 2026-10-13 口径：产线上一次播报、多台同时收，省掉逐台插线/连串口。
 * 代价是"同一批设备会收到同一个身份" —— 所以工装侧要按台改 `--devid/--mac`
 * 再播（或者由产线 MES 逐条发）。**这不是本工具的缺陷，是组播方案的固有约束**，
 * 写在这里免得后面当成 bug 查。
 *
 * ============================ 待用户冻结（见 iv_provision.h 文件头） ============================
 *   ① 组播默认地址/端口；② 报文协议格式。当前用参考项目默认值与自定义最小格式
 *   占位 —— **只保证自己发的自己能收，不声称与任何现网格式兼容**。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_provision.h"
#include "ivsbox/iv_ret.h"

#define PROV_MOD "provision"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  Receive mode (default): join the group and write identity to private partition.\n"
        "  Send mode (--send):     broadcast one identity packet (factory fixture side).\n"
        "\n"
        "  --ip A.B.C.D      multicast group      (default %s)\n"
        "  --port N          multicast port       (default %d)\n"
        "  --local-port N    local listen port    (default %d)\n"
        "  --dev PATH        private partition     (default %s)\n"
        "  --secure DIR      secure dir           (default %s)\n"
        "  --marker PATH     factory marker file  (default %s)\n"
        "  --once            listen: exit after one accepted packet\n"
        "  --timeout S       listen: exit after S seconds (0 = forever)\n"
        "  --dry-run         parse only, write nothing\n"
        "  --force           ignore the factory marker (verification only)\n"
        "\n"
        "  --send            send one packet instead of listening\n"
        "  --devid HEX       device id   (send mode)\n"
        "  --mac aa:bb:cc:dd:ee:ff\n"
        "  --server STR      debug server address (send mode)\n"
        "  --suite N         crypto suite id (send mode, default 5)\n"
        "  --version\n",
        argv0, IV_PROV_MCAST_IP_DEFAULT, IV_PROV_MCAST_PORT_DEFAULT,
        IV_PROV_MCAST_LOCAL_PORT_DEFAULT, IV_PROV_PRIVATE_DEV_DEFAULT,
        IV_PROV_SECURE_DIR_DEFAULT, IV_PROV_MARKER_DEFAULT);
}

/* 解析 "aa:bb:cc:dd:ee:ff"。成功返回 0。 */
static int parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    int      i;

    if (s == NULL || sscanf(s, "%x:%x:%x:%x:%x:%x",
                            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (i = 0; i < 6; i++) {
        if (v[i] > 0xFFu)
            return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

static int make_mcast_socket(const char *ip, int port, int local_port)
{
    struct sockaddr_in addr;
    struct ip_mreq     mreq;
    int                fd;
    int                on = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* 组播端口必须绑 INADDR_ANY 才能收 */
    addr.sin_port        = htons((uint16_t)local_port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        (void)close(fd);
        return -1;
    }

    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(ip);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY); /* 任选出口，板端多网口时不挑 */
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        (void)close(fd);
        return -1;
    }
    (void)port;
    return fd;
}

/* secure/ 目录落盘：套件号与身份摘要（0600）。
 * **登记欠账**：架构 §10 的密钥分离（F8）本轮未实现，这里只把目录与权限就位，
 * 真正的调试令牌 / SM4 密钥不进本工具（没有对应条目的报文定义）。 */
static int write_secure(const char *dir, const iv_prov_id_t *id)
{
    char  path[256];
    int   fd;
    int   n;
    char  buf[256];

    if (dir == NULL)
        return IV_EINVAL;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return IV_EIO;

    if (snprintf(path, sizeof(path), "%s/identity.txt", dir) >= (int)sizeof(path))
        return IV_EIO;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return IV_EIO;
    n = snprintf(buf, sizeof(buf), "devid=0x%08x\nmac=%02x:%02x:%02x:%02x:%02x:%02x\nsuite=%u\nserver=%s\n",
                 id->devid, id->mac[0], id->mac[1], id->mac[2], id->mac[3], id->mac[4],
                 id->mac[5], (unsigned)id->suite_id, id->server);
    if (n < 0 || write(fd, buf, (size_t)n) != (ssize_t)n) {
        (void)close(fd);
        return IV_EIO;
    }
    if (fsync(fd) != 0) {
        (void)close(fd);
        return IV_EIO;
    }
    (void)close(fd);
    return IV_OK;
}

static int run_send(int fd, const char *ip, int port, const iv_prov_id_t *id)
{
    uint8_t            pkt[IV_PROV_PKT_LEN];
    struct sockaddr_in dst;
    ssize_t            n;

    if (iv_prov_pkt_build(id, pkt) != IV_OK)
        return 1;

    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = inet_addr(ip);
    dst.sin_port        = htons((uint16_t)port);

    n = sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
    if (n != (ssize_t)sizeof(pkt)) {
        fprintf(stderr, "sendto failed: %s\n", strerror(errno));
        return 1;
    }
    printf("sent %zu bytes to %s:%d (devid=0x%08x)\n", sizeof(pkt), ip, port, id->devid);
    return 0;
}

static int run_listen(int fd, const char *dev, const char *secure_dir,
                      int once, int timeout_s, int dry)
{
    time_t t0 = time(NULL);

    for (;;) {
        struct pollfd      pfd;
        struct sockaddr_in from;
        socklen_t          fl = sizeof(from);
        uint8_t            buf[512];
        ssize_t            n;
        int                prc;
        iv_prov_id_t       id;

        if (g_stop)
            return 0;
        if (timeout_s > 0 && (int)(time(NULL) - t0) >= timeout_s) {
            printf("listen timeout (%ds), exiting\n", timeout_s);
            return 0;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 1000) <= 0)
            continue;

        n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "recvfrom failed: %s\n", strerror(errno));
            return 1;
        }

        prc = iv_prov_pkt_parse(buf, (size_t)n, &id);
        if (prc != IV_OK) {
            /* 校验不过的报文一律拒收（计划 S5.5 的验证项之一）。 */
            printf("rejected %zd-byte packet from %s (rc=%d)\n",
                   n, inet_ntoa(from.sin_addr), prc);
            continue;
        }

        printf("accepted identity from %s: devid=0x%08x mac=%02x:%02x:%02x:%02x:%02x:%02x suite=%u server=%s\n",
               inet_ntoa(from.sin_addr), id.devid,
               id.mac[0], id.mac[1], id.mac[2], id.mac[3], id.mac[4], id.mac[5],
               (unsigned)id.suite_id, id.server);

        if (!dry) {
            int rc = iv_prov_private_write(dev, &id);

            printf("  private partition write: %s\n",
                   rc == IV_OK ? "ok" : (rc == IV_ENOENT ? "device missing" : "failed"));
            rc = write_secure(secure_dir, &id);
            printf("  secure dir write: %s\n", rc == IV_OK ? "ok" : "failed");
        } else {
            printf("  (dry-run: nothing written)\n");
        }

        if (once)
            return 0;
    }
}

int main(int argc, char *argv[])
{
    const char   *ip          = IV_PROV_MCAST_IP_DEFAULT;
    const char   *dev         = IV_PROV_PRIVATE_DEV_DEFAULT;
    const char   *secure_dir  = IV_PROV_SECURE_DIR_DEFAULT;
    const char   *marker      = IV_PROV_MARKER_DEFAULT;
    int           port        = IV_PROV_MCAST_PORT_DEFAULT;
    int           local_port  = IV_PROV_MCAST_LOCAL_PORT_DEFAULT;
    int           once = 0, timeout_s = 0, dry = 0, force = 0, send = 0;
    iv_prov_id_t  id;
    int           fd;
    int           rc;
    int           i;

    memset(&id, 0, sizeof(id));
    id.suite_id = 5; /* 与姊妹工程 bird 的 SuiteID 对齐，仅作发送模式的默认值 */
    (void)snprintf(id.server, sizeof(id.server), "0.0.0.0");

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("ivsbox-provision\n");
            return 0;
        } else if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            ip = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--local-port") == 0 && i + 1 < argc) {
            local_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            dev = argv[++i];
        } else if (strcmp(argv[i], "--secure") == 0 && i + 1 < argc) {
            secure_dir = argv[++i];
        } else if (strcmp(argv[i], "--marker") == 0 && i + 1 < argc) {
            marker = argv[++i];
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry = 1;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "--send") == 0) {
            send = 1;
        } else if (strcmp(argv[i], "--devid") == 0 && i + 1 < argc) {
            id.devid = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--mac") == 0 && i + 1 < argc) {
            if (parse_mac(argv[++i], id.mac) != 0) {
                fprintf(stderr, "bad --mac\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            (void)snprintf(id.server, sizeof(id.server), "%.*s", 31, argv[++i]);
        } else if (strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
            id.suite_id = (uint8_t)atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (port <= 0 || port > 65535 || local_port <= 0 || local_port > 65535) {
        usage(argv[0]);
        return 1;
    }

    /* 工厂模式闸门：标记不在就不干活。**发模式也检查** —— 工装侧没有标记文件，
     * 故发模式默认要求 --force（或让它显式带 --marker 指向一个存在的文件）。 */
    if (!send && !force && !iv_prov_marker_present(marker)) {
        fprintf(stderr, "provision disabled: marker not found: %s\n", marker);
        fprintf(stderr, "  (create it to enter factory mode, or pass --force for verification)\n");
        return 2;
    }

    (void)iv_log_init("ivsbox-provision");
    iv_log_set_stderr(1);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    fd = make_mcast_socket(ip, port, local_port);
    if (fd < 0) {
        fprintf(stderr, "multicast socket setup failed (%s:%d local %d): %s\n",
                ip, port, local_port, strerror(errno));
        return 1;
    }

    if (send)
        rc = run_send(fd, ip, port, &id);
    else
        rc = run_listen(fd, dev, secure_dir, once, timeout_s, dry);

    (void)close(fd);
    return rc;
}
