/*
 * ivsbox-provision —— 出厂身份工装（M6/S5.5 协议定稿版）
 *
 * ============================ 它怎么用 ============================
 * 同一个二进制两种角色：
 *
 *   1. **设备侧（默认）**：加入组播组，收 `search` 就回设备信息、收 `update_mac`
 *      就把设备 ID/MAC 写进 `private` 裸分区并回声：
 *        ivsbox-provision                       # 常驻监听，Ctrl-C/SIGTERM 退出
 *        ivsbox-provision --timeout 60 --once   # 收一条就退（产线脚本用）
 *        ivsbox-provision --dry-run             # 只解析/决策，不落盘（验证用）
 *
 *   2. **工装侧（软件角色）**：发搜索、发配置：
 *        ivsbox-provision --search                          # 搜索（默认收 3 s 应答）
 *        ivsbox-provision --search --watch                  # 每 60 s 重发（对齐打印软件）
 *        ivsbox-provision --set-id 1234FF --mac aa:bb:...   # 配置设备 ID＋MAC
 *        ivsbox-provision --set-id 1234FF --target 192.168.1.30   # 单播直连某台
 *
 * 设备侧**只有工厂标记存在时才干活**（`/mnt/UDISK/ivsbox/factory/enable_provision`），
 * 量产后删掉该文件即关闭入口；`--force` 可忽略标记，**仅验证用**。工装侧不检查标记。
 *
 * ============================ 字段来源（诚实登记） ============================
 * `search` 应答里的字段，本工具能就地取到的就填，取不到的一律**空字符串**
 * （协议文档明确"不存在直接返回空字符串即可"）：
 *   tid   ← `private` 分区身份记录，或 `--tid` 覆盖
 *   mac   ← 身份记录里的 MAC，或 `--mac`
 *   sv    ← `iv_version_string()`，或 `--sv`
 *   ip/nm ← `--iface`（默认尝试 eth0）实际地址；gw ← `/proc/net/route` 默认路由
 *   tnam/mcu/mod/inip/outip/iccid/name/password ← 仅 `--xxx` 覆盖，默认空
 * **未接（登记）**：iccid（需 AT/无卡检测）、mcu（需主板 EEPROM）、inip/outip
 * （需接平台配置键）、name/password（需接 Web 口令）。接上只需在 `fill_devinfo()`
 * 里多写一行，协议侧不用动。
 *
 * ============================ 安全边界 ============================
 * 报文明文、组播可达即能改身份 —— 这是协议固有属性（见 iv_provision.h）。
 * 组播 TTL 固定 1，不跨路由。`--keep-id` 是现场安全阀：忽略报文里的新设备 ID，
 * 只改 MAC（防止"tid 口径"万一判断有误把设备 ID 改乱）。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_provision.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_version.h"

#define PROV_MOD "provision"

typedef struct {
    const char *ip;          /* 组播组（发送目标 / join 目标） */
    const char *target;      /* 工装侧：单播直连目标 IP（给了就不发组播） */
    const char *dev;
    const char *secure;
    const char *marker;
    const char *iface;
    int         port;        /* 组播端口 / 目标端口 */
    int         local_port;  /* 本机绑定端口 */
    int         timeout_s;
    int         once;
    int         dry;
    int         force;
    int         keep_id;
    int         reply_unicast;
    int         watch;
    int         search;
    int         set_id;
    const char *tid;
    const char *tnam, *mcu, *mod, *sv, *inip, *outip, *iccid, *name, *password;
    const char *set_mac;
    char        mac_sep;
} opts_t;

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
        "  Device side (default): join group, answer search, apply update_mac.\n"
        "  Fixture side: --search | --set-id TID [--mac MAC]\n"
        "\n"
        "  --ip A.B.C.D      multicast group   (default %s)\n"
        "  --port N          group/target port (default %d)\n"
        "  --local-port N    local bind port   (default = --port)\n"
        "  --target A.B.C.D  fixture: send unicast to this host (skip group)\n"
        "  --dev PATH        private partition (default %s)\n"
        "  --secure DIR      secure dir        (default %s)\n"
        "  --marker PATH     factory marker    (default %s)\n"
        "  --iface NAME      fill ip/nm from this interface (default eth0)\n"
        "  --tid HEX         own device id override (6 hex digits)\n"
        "  --mac SEP:HEX     own MAC (also used to fill 'mac' in replies)\n"
        "  --mac-sep C       separator used when sending MAC (default ':')\n"
        "  --tnam/--mcu/--mod/--sv/--inip/--outip/--iccid/--name/--password STR\n"
        "                    device-info field overrides (default empty)\n"
        "  --keep-id         ignore the new device id in update_mac (MAC only)\n"
        "  --reply-unicast   reply to the sender instead of the group\n"
        "  --once            device: exit after one handled request\n"
        "  --timeout S       exit/collect window in seconds (0 = forever)\n"
        "  --dry-run         decide only, write nothing\n"
        "  --force           ignore the factory marker (verification only)\n"
        "  --search          fixture: send a search query\n"
        "  --watch           fixture: repeat every 60 s\n"
        "  --set-id HEX      fixture: send update_mac with this new device id\n"
        "  --version\n",
        argv0, IV_PROV_MCAST_ADDR, IV_PROV_MCAST_PORT, IV_PROV_PRIVATE_DEV_DEFAULT,
        IV_PROV_SECURE_DIR_DEFAULT, IV_PROV_MARKER_DEFAULT);
}

/* --------------------------------------------------------------------------
 * 小工具
 * -------------------------------------------------------------------------- */
static int is_mcast_addr(const char *ip)
{
    struct in_addr a;
    uint32_t       h;

    if (ip == NULL || inet_aton(ip, &a) == 0)
        return 0;
    h = ntohl(a.s_addr) >> 24;
    return (h >= 224u && h <= 239u) ? 1 : 0;
}

static void mac_to_buf(const char *mac, char sep, char *out, size_t cap)
{
    uint8_t b[6];

    out[0] = '\0';
    if (mac == NULL || iv_prov_mac_parse(mac, b) != IV_OK)
        return;
    (void)snprintf(out, cap, "%02x%c%02x%c%02x%c%02x%c%02x%c%02x",
                   (unsigned)b[0], sep, (unsigned)b[1], sep, (unsigned)b[2], sep,
                   (unsigned)b[3], sep, (unsigned)b[4], sep, (unsigned)b[5]);
}

/* 取接口地址/掩码（失败留空）。 */
static void iface_addr(const char *ifname, char *ip, size_t ipc, char *nm, size_t nmc)
{
    struct ifreq ifr;
    int          fd;

    ip[0] = '\0';
    nm[0] = '\0';
    if (ifname == NULL || ifname[0] == '\0')
        return;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    memset(&ifr, 0, sizeof(ifr));
    (void)snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)&ifr.ifr_addr;

        (void)snprintf(ip, ipc, "%s", inet_ntoa(sin->sin_addr));
    }
    memset(&ifr, 0, sizeof(ifr));
    (void)snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(fd, SIOCGIFNETMASK, &ifr) == 0) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)&ifr.ifr_netmask;

        (void)snprintf(nm, nmc, "%s", inet_ntoa(sin->sin_addr));
    }
    (void)close(fd);
}

/* 默认网关：读 /proc/net/route 第一条 Destination=00000000 的记录（网关是小端 hex）。 */
static void default_gw(char *out, size_t cap)
{
    FILE *f;
    char  line[256];

    out[0] = '\0';
    f = fopen("/proc/net/route", "r");
    if (f == NULL)
        return;
    (void)fgets(line, sizeof(line), f); /* 表头 */
    while (fgets(line, sizeof(line), f) != NULL) {
        char    iface[32];
        char    dest[16];
        char    gw[16];
        uint32_t g;

        if (sscanf(line, "%31s %15s %15s", iface, dest, gw) != 3)
            continue;
        if (strcmp(dest, "00000000") != 0)
            continue;
        g = (uint32_t)strtoul(gw, NULL, 16);
        (void)snprintf(out, cap, "%u.%u.%u.%u",
                       (unsigned)(g & 0xFFu), (unsigned)((g >> 8) & 0xFFu),
                       (unsigned)((g >> 16) & 0xFFu), (unsigned)((g >> 24) & 0xFFu));
        break;
    }
    (void)fclose(f);
}

static void fill_devinfo(iv_prov_devinfo_t *d, const char *own_tid,
                         const iv_prov_id_t *id, const opts_t *o)
{
    memset(d, 0, sizeof(*d));
    if (own_tid != NULL && own_tid[0] != '\0')
        (void)snprintf(d->tid, sizeof(d->tid), "%s", own_tid);
    else if (id != NULL)
        iv_prov_tid_from_u32(id->devid, d->tid);

    if (id != NULL)
        iv_prov_mac_format(id->mac, d->mac);

    /* sv：给了 --sv 就用，否则用本工程版本串 */
    if (o->sv != NULL)
        (void)snprintf(d->sv, sizeof(d->sv), "%s", o->sv);
    else if (iv_version_string() != NULL)
        (void)snprintf(d->sv, sizeof(d->sv), "%s", iv_version_string());

    if (o->tnam != NULL)     (void)snprintf(d->tnam, sizeof(d->tnam), "%s", o->tnam);
    if (o->mcu != NULL)      (void)snprintf(d->mcu, sizeof(d->mcu), "%s", o->mcu);
    if (o->mod != NULL)      (void)snprintf(d->mod, sizeof(d->mod), "%s", o->mod);
    if (o->inip != NULL)     (void)snprintf(d->inip, sizeof(d->inip), "%s", o->inip);
    if (o->outip != NULL)    (void)snprintf(d->outip, sizeof(d->outip), "%s", o->outip);
    if (o->iccid != NULL)    (void)snprintf(d->iccid, sizeof(d->iccid), "%s", o->iccid);
    if (o->name != NULL)     (void)snprintf(d->name, sizeof(d->name), "%s", o->name);
    if (o->password != NULL) (void)snprintf(d->password, sizeof(d->password), "%s", o->password);

    iface_addr((o->iface != NULL) ? o->iface : "eth0",
               d->ip, sizeof(d->ip), d->nm, sizeof(d->nm));
    default_gw(d->gw, sizeof(d->gw));
}

/* secure/ 目录落盘：身份摘要（0600）。F8 密钥分离未实现 ⇒ 这里只放摘要。 */
static int write_secure(const char *dir, const iv_prov_id_t *id)
{
    char path[256];
    char buf[256];
    int  fd;
    int  n;

    if (dir == NULL)
        return IV_EINVAL;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return IV_EIO;
    if (snprintf(path, sizeof(path), "%.200s/identity.txt", dir) >= (int)sizeof(path))
        return IV_EIO;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return IV_EIO;
    n = snprintf(buf, sizeof(buf),
                 "devid=%06X\nmac=%02x:%02x:%02x:%02x:%02x:%02x\nsuite=%u\nserver=%s\n",
                 (unsigned)(id->devid & 0xFFFFFFu),
                 (unsigned)id->mac[0], (unsigned)id->mac[1], (unsigned)id->mac[2],
                 (unsigned)id->mac[3], (unsigned)id->mac[4], (unsigned)id->mac[5],
                 (unsigned)id->suite_id, id->server);
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

/* --------------------------------------------------------------------------
 * socket
 * -------------------------------------------------------------------------- */
static int make_socket(const opts_t *o, int join)
{
    struct sockaddr_in addr;
    int                fd;
    int                on = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* 组播端口必须绑 INADDR_ANY 才收得到 */
    addr.sin_port        = htons((uint16_t)o->local_port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        (void)close(fd);
        return -1;
    }

    if (is_mcast_addr(o->ip)) {
        int ttl = IV_PROV_TTL;

        (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }

    if (join) {
        struct ip_mreq mreq;

        memset(&mreq, 0, sizeof(mreq));
        (void)inet_aton(o->ip, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY); /* 任选出口，多网口不挑 */
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            /* **join 失败不致命**：单播/直连场景（验证、点对点刷机）照样能收；
             * 板端网卡未起时也不该让工装直接退出。 */
            fprintf(stderr, "warn: IP_ADD_MEMBERSHIP(%s) failed: %s "
                            "(continue, unicast still works)\n",
                    o->ip, strerror(errno));
        }
    }
    return fd;
}

static int send_msg(int fd, const char *dst_ip, int dst_port, const char *msg, int len)
{
    struct sockaddr_in dst;
    ssize_t            n;

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    (void)inet_aton(dst_ip, &dst.sin_addr);
    dst.sin_port = htons((uint16_t)dst_port);

    n = sendto(fd, msg, (size_t)len, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (n != (ssize_t)len) {
        fprintf(stderr, "sendto %s:%d failed: %s\n", dst_ip, dst_port, strerror(errno));
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * 设备侧
 * -------------------------------------------------------------------------- */
static int handle_request(const opts_t *o, const char *buf, size_t len,
                          const char *own_tid, iv_prov_id_t *id, int *id_valid,
                          struct sockaddr_in *from, char *reply, size_t reply_cap,
                          int *reply_len)
{
    iv_prov_plan_t plan;
    int            rc;

    rc = iv_prov_decide(buf, len, own_tid, &plan);
    if (rc != IV_OK) {
        printf("rejected %zu-byte datagram from %s (rc=%d)\n",
               len, inet_ntoa(from->sin_addr), rc);
        return 0;
    }
    if (plan.act == IV_PROV_ACT_NONE) {
        /* 未实现的指令 / 不是给我的：静默丢弃（协议要求不回复）。 */
        return 0;
    }

    if (plan.act == IV_PROV_ACT_SEARCH) {
        iv_prov_devinfo_t d;

        /* tid 一律以本机 tid（`--tid` / 身份记录）为准：身份记录里 devid 可能是 0
         * （只给了 `--mac` 的板子），用它会把回包 tid 变成 000000。 */
        fill_devinfo(&d, own_tid, *id_valid ? id : NULL, o);
        *reply_len = iv_prov_build_search_reply(&d, reply, reply_cap);
        printf("search from %s -> reply tid=%s mac=%s\n",
               inet_ntoa(from->sin_addr), d.tid, d.mac);
        return (*reply_len > 0) ? 1 : 0;
    }

    /* update_mac */
    {
        iv_prov_devinfo_t d;
        iv_prov_id_t      next;
        char              new_tid[IV_PROV_TID_MAX];
        int               ok = 0;

        if (plan.new_tid[0] != '\0')
            (void)snprintf(new_tid, sizeof(new_tid), "%s", plan.new_tid);
        else
            (void)snprintf(new_tid, sizeof(new_tid), "%s", own_tid);

        memset(&next, 0, sizeof(next));
        if (*id_valid)
            next = *id; /* 保留 suite/server */
        next.suite_id = (*id_valid) ? id->suite_id : 5u;
        if (plan.mac_ok) {
            memcpy(next.mac, plan.new_mac, 6u);
            if (o->keep_id) {
                (void)snprintf(new_tid, sizeof(new_tid), "%s", own_tid);
                if (*id_valid)
                    next.devid = id->devid;
            } else if (iv_prov_tid_to_u32(new_tid, &next.devid) != IV_OK) {
                new_tid[0] = '\0'; /* 不可能：decide 已校验过 */
            }
        }

        if (!plan.mac_ok) {
            printf("update_mac from %s: bad mac -> fault\n", inet_ntoa(from->sin_addr));
        } else if (o->dry) {
            ok = 1;
            printf("update_mac from %s: (dry-run) would write tid=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                   inet_ntoa(from->sin_addr), new_tid,
                   next.mac[0], next.mac[1], next.mac[2], next.mac[3], next.mac[4], next.mac[5]);
        } else {
            int wr = iv_prov_private_write(o->dev, &next);

            ok = (wr == IV_OK) ? 1 : 0;
            printf("update_mac from %s: tid=%s mac=%02x:%02x:%02x:%02x:%02x:%02x "
                   "private-write=%s\n",
                   inet_ntoa(from->sin_addr), new_tid,
                   next.mac[0], next.mac[1], next.mac[2], next.mac[3], next.mac[4],
                   next.mac[5], (wr == IV_OK) ? "ok" : "FAILED");
            if (ok) {
                (void)write_secure(o->secure, &next);
                *id = next;
                *id_valid = 1;
            }
        }

        /* 应答里的设备信息：身份已改就用新值；失败则原样回报。 */
        if (ok) {
            iv_prov_id_t shown = next;

            fill_devinfo(&d, new_tid, &shown, o);
        } else {
            fill_devinfo(&d, own_tid, (*id_valid) ? id : NULL, o);
        }
        *reply_len = iv_prov_build_update_mac_reply(&d, ok, reply, reply_cap);
        return (*reply_len > 0) ? 1 : 0;
    }
}

static int run_device(const opts_t *o, const char *own_tid_opt, iv_prov_id_t *id,
                      int *id_valid)
{
    char               own_tid[IV_PROV_TID_MAX];
    struct sockaddr_in self;
    int                fd;
    time_t             t0 = time(NULL);
    int                handled = 0;

    if (own_tid_opt != NULL && own_tid_opt[0] != '\0') {
        uint32_t v = 0u;

        if (iv_prov_tid_to_u32(own_tid_opt, &v) != IV_OK) {
            fprintf(stderr, "bad --tid: %s\n", own_tid_opt);
            return 1;
        }
        (void)snprintf(own_tid, sizeof(own_tid), "%s", own_tid_opt);
    } else if (*id_valid) {
        iv_prov_tid_from_u32(id->devid, own_tid);
    } else {
        (void)snprintf(own_tid, sizeof(own_tid), "%s", IV_PROV_TID_BROADCAST);
        printf("warn: no identity in %s and no --tid; own tid = %s\n",
               o->dev, own_tid);
    }

    memset(&self, 0, sizeof(self));
    self.sin_family = AF_INET;
    (void)inet_aton(o->ip, &self.sin_addr);
    self.sin_port = htons((uint16_t)o->port);

    fd = make_socket(o, 1);
    if (fd < 0) {
        fprintf(stderr, "socket setup failed (local port %d): %s\n",
                o->local_port, strerror(errno));
        return 1;
    }
    printf("device side listening: group %s:%d, local %d, tid=%s%s\n",
           o->ip, o->port, o->local_port, own_tid, o->dry ? " (dry-run)" : "");

    for (;;) {
        struct pollfd      pfd;
        struct sockaddr_in from;
        socklen_t          fl = sizeof(from);
        char               buf[IV_PROV_JSON_MAX + 1u];
        char               reply[IV_PROV_JSON_MAX];
        ssize_t            n;
        int                reply_len = 0;

        if (g_stop)
            break;
        if (o->timeout_s > 0 && (int)(time(NULL) - t0) >= o->timeout_s) {
            printf("listen timeout (%ds), exiting\n", o->timeout_s);
            break;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 500) <= 0)
            continue;

        n = recvfrom(fd, buf, sizeof(buf) - 1u, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "recvfrom failed: %s\n", strerror(errno));
            break;
        }
        buf[n] = '\0';

        if (handle_request(o, buf, (size_t)n, own_tid, id, id_valid, &from,
                           reply, sizeof(reply), &reply_len)) {
            struct sockaddr_in dst = self;

            if (o->reply_unicast)
                dst = from; /* 回给请求来源（单播；验证与点对点刷机都走这条） */
            if (sendto(fd, reply, (size_t)reply_len, 0, (struct sockaddr *)&dst,
                       sizeof(dst)) == (ssize_t)reply_len) {
                printf("  replied %d bytes to %s:%u\n", reply_len,
                       inet_ntoa(dst.sin_addr), (unsigned)ntohs(dst.sin_port));
            } else {
                fprintf(stderr, "  reply sendto failed: %s\n", strerror(errno));
            }
            handled++;
            if (o->once)
                break;
        }
    }

    (void)close(fd);
    printf("device side done (%d request(s) handled)\n", handled);
    return 0;
}

/* --------------------------------------------------------------------------
 * 工装侧
 * -------------------------------------------------------------------------- */
static int run_fixture(const opts_t *o)
{
    const char *dst_ip = (o->target != NULL) ? o->target : o->ip;
    int         dst_port = o->port;
    char        msg[IV_PROV_JSON_MAX];
    int         fd;
    int         len;
    int         round = 0;
    int         seen = 0;

    fd = make_socket(o, 0);
    if (fd < 0) {
        fprintf(stderr, "socket setup failed (local port %d): %s\n",
                o->local_port, strerror(errno));
        return 1;
    }

    for (;;) {
        time_t t0 = time(NULL);

        if (o->set_id) {
            char mac[IV_PROV_MAC_MAX];

            mac_to_buf(o->set_mac, o->mac_sep, mac, sizeof(mac));
            len = iv_prov_build_update_mac_query(o->tid, mac, msg, sizeof(msg));
            if (len <= 0) {
                fprintf(stderr, "build update_mac query failed\n");
                (void)close(fd);
                return 1;
            }
        } else {
            len = iv_prov_build_search_query(msg, sizeof(msg));
            if (len <= 0) {
                fprintf(stderr, "build search query failed\n");
                (void)close(fd);
                return 1;
            }
        }

        if (send_msg(fd, dst_ip, dst_port, msg, len) != 0) {
            (void)close(fd);
            return 1;
        }
        printf("[%d] sent to %s:%d: %s\n", round + 1, dst_ip, dst_port, msg);

        /* 收集窗口 */
        for (;;) {
            struct pollfd pfd;
            char          buf[IV_PROV_JSON_MAX + 1u];
            ssize_t       n;
            iv_prov_msg_t m;

            if (o->timeout_s > 0 && (int)(time(NULL) - t0) >= o->timeout_s)
                break;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, 200) <= 0) {
                if (o->timeout_s <= 0)
                    break; /* 0 = 只收一次轮询窗口 */
                continue;
            }
            n = recvfrom(fd, buf, sizeof(buf) - 1u, 0, NULL, NULL);
            if (n <= 0)
                continue;
            buf[n] = '\0';
            if (iv_prov_msg_parse(buf, (size_t)n, &m) == IV_OK)
                printf("  <= tid=%s types=%s mac=%s\n", m.tid, m.types, m.has_mac ? m.mac : "-");
            else
                printf("  <= (unparsable) %s\n", buf);
            printf("     %s\n", buf);
            seen++;
        }

        round++;
        if (!o->watch) {
            printf("done: %d reply(ies) in %d round(s)\n", seen, round);
            break;
        }
        printf("watch: sleeping 60s before next round (Ctrl-C to stop)\n");
        {
            int slept = 0;

            while (slept < 60 && !g_stop) {
                sleep(1);
                slept++;
            }
        }
        if (g_stop)
            break;
    }

    (void)close(fd);
    return 0;
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    opts_t      o;
    iv_prov_id_t id;
    int         id_valid = 0;
    int         i;

    memset(&o, 0, sizeof(o));
    memset(&id, 0, sizeof(id));
    o.ip         = IV_PROV_MCAST_ADDR;
    o.dev        = IV_PROV_PRIVATE_DEV_DEFAULT;
    o.secure     = IV_PROV_SECURE_DIR_DEFAULT;
    o.marker     = IV_PROV_MARKER_DEFAULT;
    o.port       = IV_PROV_MCAST_PORT;
    o.local_port = -1; /* 未指定 ⇒ 与 --port 相同 */
    o.timeout_s  = 3;
    o.mac_sep    = ':';

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--version") == 0) {
            printf("ivsbox-provision\n");
            return 0;
        } else if (strcmp(a, "--ip") == 0 && i + 1 < argc) {
            o.ip = argv[++i];
        } else if (strcmp(a, "--target") == 0 && i + 1 < argc) {
            o.target = argv[++i];
        } else if (strcmp(a, "--port") == 0 && i + 1 < argc) {
            o.port = atoi(argv[++i]);
        } else if (strcmp(a, "--local-port") == 0 && i + 1 < argc) {
            o.local_port = atoi(argv[++i]);
        } else if (strcmp(a, "--dev") == 0 && i + 1 < argc) {
            o.dev = argv[++i];
        } else if (strcmp(a, "--secure") == 0 && i + 1 < argc) {
            o.secure = argv[++i];
        } else if (strcmp(a, "--marker") == 0 && i + 1 < argc) {
            o.marker = argv[++i];
        } else if (strcmp(a, "--iface") == 0 && i + 1 < argc) {
            o.iface = argv[++i];
        } else if (strcmp(a, "--tid") == 0 && i + 1 < argc) {
            o.tid = argv[++i];
        } else if (strcmp(a, "--mac") == 0 && i + 1 < argc) {
            o.set_mac = argv[++i];
        } else if (strcmp(a, "--mac-sep") == 0 && i + 1 < argc) {
            o.mac_sep = argv[++i][0];
        } else if (strcmp(a, "--tnam") == 0 && i + 1 < argc) {
            o.tnam = argv[++i];
        } else if (strcmp(a, "--mcu") == 0 && i + 1 < argc) {
            o.mcu = argv[++i];
        } else if (strcmp(a, "--mod") == 0 && i + 1 < argc) {
            o.mod = argv[++i];
        } else if (strcmp(a, "--sv") == 0 && i + 1 < argc) {
            o.sv = argv[++i];
        } else if (strcmp(a, "--inip") == 0 && i + 1 < argc) {
            o.inip = argv[++i];
        } else if (strcmp(a, "--outip") == 0 && i + 1 < argc) {
            o.outip = argv[++i];
        } else if (strcmp(a, "--iccid") == 0 && i + 1 < argc) {
            o.iccid = argv[++i];
        } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
            o.name = argv[++i];
        } else if (strcmp(a, "--password") == 0 && i + 1 < argc) {
            o.password = argv[++i];
        } else if (strcmp(a, "--keep-id") == 0) {
            o.keep_id = 1;
        } else if (strcmp(a, "--reply-unicast") == 0) {
            o.reply_unicast = 1;
        } else if (strcmp(a, "--once") == 0) {
            o.once = 1;
        } else if (strcmp(a, "--timeout") == 0 && i + 1 < argc) {
            o.timeout_s = atoi(argv[++i]);
        } else if (strcmp(a, "--dry-run") == 0) {
            o.dry = 1;
        } else if (strcmp(a, "--force") == 0) {
            o.force = 1;
        } else if (strcmp(a, "--search") == 0) {
            o.search = 1;
        } else if (strcmp(a, "--watch") == 0) {
            o.watch = 1;
        } else if (strcmp(a, "--set-id") == 0 && i + 1 < argc) {
            o.set_id = 1;
            o.tid = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (o.local_port < 0)
        o.local_port = o.port;
    if (o.port <= 0 || o.port > 65535 || o.local_port <= 0 || o.local_port > 65535) {
        usage(argv[0]);
        return 1;
    }
    if (o.set_id) {
        uint32_t v = 0u;

        if (iv_prov_tid_to_u32(o.tid, &v) != IV_OK) {
            fprintf(stderr, "bad --set-id (want 6 hex digits): %s\n", o.tid);
            return 1;
        }
    }

    (void)iv_log_init("ivsbox-provision");
    iv_log_set_stderr(1);
    /* stdout 接管道时默认**全缓冲**（4 KiB），产线脚本/集成用例靠"listening"
     * 这行判断进程已就绪 ⇒ 必须行缓冲，否则那行会卡在缓冲区里看不到。 */
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    /* 工装侧（发查询）不检查工厂标记；设备侧（会被改身份）必须过闸门。 */
    if (!o.search && !o.set_id && !o.force && !iv_prov_marker_present(o.marker)) {
        fprintf(stderr, "provision disabled: marker not found: %s\n", o.marker);
        fprintf(stderr, "  (create it to enter factory mode, or pass --force for verification)\n");
        return 2;
    }

    if (o.search || o.set_id)
        return run_fixture(&o); /* 工装侧：发查询、收应答，设备身份半点不碰 */

    /* 设备侧：先把身份读出来（读不到不算错：可能是没写过的新板子）。 */
    if (iv_prov_private_read(o.dev, &id) == IV_OK)
        id_valid = 1;
    else
        printf("note: no identity in %s yet\n", o.dev);

    /* --mac 在设备侧也用来补 'mac' 字段（身份里没有 MAC 时） */
    if (o.set_mac != NULL) {
        uint8_t b[6];

        if (iv_prov_mac_parse(o.set_mac, b) == IV_OK) {
            memcpy(id.mac, b, 6u);
            id_valid = 1;
        } else {
            fprintf(stderr, "bad --mac: %s\n", o.set_mac);
            return 1;
        }
    }

    return run_device(&o, o.tid, &id, &id_valid);
}
