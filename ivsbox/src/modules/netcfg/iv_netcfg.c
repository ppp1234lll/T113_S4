/*
 * 本机 IPv4 静态地址 —— 实现（平台 0xF4「配置设备 IP、网关」的落地层）
 *
 * 设计取舍见 iv_netcfg.h 的文件头，这里只补实现层面必须说清的三件事：
 *
 * ============================ 1. "网络序"到底存的是什么 ============================
 * `iv_netcfg_t` 的三个字段按 `struct in_addr.s_addr` 的表示存 —— 也就是
 * **内存里的字节顺序就是线上的字节顺序**（`memcpy(&v, data, 4)` 出来那个值），
 * 在 x86 上它的数值看着像反的（192.168.2.121 存成 0x7902A8C0），但这正是
 * `inet_ntop` / `ntohl` 期望的形态，直接用即可，不必也不该再 `htonl` 一次。
 * **判断、移位、掩码运算一律先 `ntohl()` 转主机序**，见 parse 里每个 `_h` 后缀。
 *
 * ============================ 2. 为什么调外部 `/sbin/ip` ============================
 * 见头文件。这里只强调一句：`fork()` 之后到 `execv()` 之间**只允许
 * async-signal-safe 调用**（本项目是多线程的，fork 出来的子进程只继承调用线程，
 * 库锁可能处于不一致状态）。所以子进程分支里只有 `open/dup2/close/execv/_exit`，
 * 没有日志、没有 malloc。
 *
 * ============================ 3. 断链是预期行为，不是缺陷 ============================
 * `apply` 一执行，本机立刻失去旧地址，平台那条 TCP 与现场 SSH 全断。所以
 * 调用方必须**先把 `0x01` 发出去**再调本函数 —— 这也是为什么 `main.c` 的
 * `on_plat_local_net()` 里 ack 在 apply 之前，中间还夹一个短延迟。
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_netcfg.h"
#include "ivsbox/iv_ret.h"

#define NC_MOD "netcfg"

/* 可用的 iproute2 落点，按优先级试（板端实测是 /sbin/ip，iproute2-ss181023）。 */
static const char *const k_ip_candidates[] = {
    "/sbin/ip", "/usr/sbin/ip", "/bin/ip", "/usr/bin/ip"
};

static const char *find_ip_bin(void)
{
    size_t i;

    for (i = 0u; i < sizeof k_ip_candidates / sizeof k_ip_candidates[0]; i++) {
        if (access(k_ip_candidates[i], X_OK) == 0)
            return k_ip_candidates[i];
    }
    return NULL;
}

/* 跑一条命令，等它结束。返回 IV_OK 仅当退出码为 0。
 * 子进程的 stdout/stderr 全部丢到 /dev/null：ivsboxd 不往 stdout 打日志，
 * 让 `ip` 的错误串漏进来只会污染现场日志，真正有用的是退出码（下面记了）。 */
static int run_argv(char *const argv[])
{
    pid_t pid;
    int   st = 0;

    pid = fork();
    if (pid < 0) {
        IV_LOG_E(NC_MOD, "fork failed: %d", (int)pid);
        return IV_EIO;
    }
    if (pid == 0) {
        /* ---- 子进程：只做 async-signal-safe 的事，见文件头第 2 条 ---- */
        int fd = open("/dev/null", O_WRONLY);

        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                (void)close(fd);
        }
        execv(argv[0], argv);
        _exit(127); /* execv 失败：用一个不会和正常退出码撞的值 */
    }
    if (waitpid(pid, &st, 0) < 0) {
        IV_LOG_E(NC_MOD, "waitpid failed for %s", argv[0]);
        return IV_EIO;
    }
    if (!WIFEXITED(st)) {
        IV_LOG_E(NC_MOD, "%s killed by signal %d", argv[0], WTERMSIG(st));
        return IV_EIO;
    }
    if (WEXITSTATUS(st) != 0) {
        IV_LOG_E(NC_MOD, "%s exited %d", argv[0], WEXITSTATUS(st));
        return IV_EIO;
    }
    return IV_OK;
}

int iv_netcfg_prefix(uint32_t mask_be)
{
    uint32_t m = ntohl(mask_be);
    uint32_t t;
    int      n = 0;

    if (m == 0u)
        return -1;
    /* 判据：**先数前导 1 的个数 n，再要求掩码恰好等于 `0xFFFFFFFF << (32-n)`**。
     * 不用 `m & (m+1) == 0` 那种写法 —— 它对 `/24`（`0xFFFFFF00`）就不成立，
     * 是个看上去很像对的错判据（本文件初版就栽在这上面）。
     * n 取到 32 时 `32-n == 0`，移位合法；n 为 0 时 m 必为 0，上面已提前返回，
     * 所以不会出现 `<< 32` 这种未定义行为。 */
    t = m;
    while ((t & 0x80000000u) != 0u) {
        n++;
        t <<= 1;
    }
    if (m != (0xFFFFFFFFu << (32 - n)))
        return -1;
    return n;
}

int iv_netcfg_parse(const uint8_t *data, size_t len, iv_netcfg_t *out)
{
    iv_netcfg_t c;
    uint32_t    ip_h;
    uint32_t    gw_h;
    int         prefix;

    if (data == NULL || out == NULL)
        return IV_EINVAL;
    /* 不足 12 字节 ⇒ 一定是坏帧。**多于 12 字节时只取前 12**：协议规定这三个
     * 地址在固定偏移上，平台若在尾部追加字段，按前 12 解析仍是对的；
     * 而"多一个字节就整体拒绝"会让平台一升级、这条命令就全瘫。记录一下即可。 */
    if (len < IV_NETCFG_PAYLOAD_LEN)
        return IV_EINVAL;
    if (len > IV_NETCFG_PAYLOAD_LEN) {
        IV_LOG_W(NC_MOD, "0xF4 payload %u bytes (expect %u), using the first %u",
                 (unsigned)len, (unsigned)IV_NETCFG_PAYLOAD_LEN,
                 (unsigned)IV_NETCFG_PAYLOAD_LEN);
    }

    (void)memset(&c, 0, sizeof c);
    (void)memcpy(&c.ip, data, 4u);      /* 网络序，见文件头第 1 条 */
    (void)memcpy(&c.mask, data + 4, 4u);
    (void)memcpy(&c.gw, data + 8, 4u);

    /* ---- 1. IP 本身不能是"等于没填"或明显非法的值 ---- */
    ip_h = ntohl(c.ip);
    if (ip_h == 0u) {
        IV_LOG_W(NC_MOD, "0xF4 ip=0.0.0.0 rejected");
        return IV_EINVAL;
    }
    if ((ip_h >> 24) == 127u || ip_h == 0xFFFFFFFFu ||
        (ip_h & 0xF0000000u) == 0xE0000000u) { /* 127/8 回环、受限广播、224/4 组播 */
        IV_LOG_W(NC_MOD, "0xF4 ip=0x%08x is loopback/broadcast/multicast", (unsigned)ip_h);
        return IV_EINVAL;
    }

    /* ---- 2. 掩码必须是连续前缀，且不能是 /0 ---- */
    prefix = iv_netcfg_prefix(c.mask);
    if (prefix < 1 || prefix > 32) {
        IV_LOG_W(NC_MOD, "0xF4 mask=0x%08x is not a contiguous prefix",
                 (unsigned)ntohl(c.mask));
        return IV_ERANGE;
    }

    /* ---- 3. /30 及更长的前缀里，IP 不能是网络地址或广播地址 ----
     * `/31`（RFC 3021 点对点）与 `/32`（单主机）没有这两个概念，放行。 */
    if (prefix <= 30) {
        uint32_t host = 0xFFFFFFFFu >> prefix; /* 主机位全 1 */
        uint32_t net  = ip_h & ~host;

        if (ip_h == net || ip_h == (net | host)) {
            IV_LOG_W(NC_MOD, "0xF4 ip=0x%08x is the network/broadcast address of /%d",
                     (unsigned)ip_h, prefix);
            return IV_EINVAL;
        }
    }

    /* ---- 4. 网关：0 表示不设默认路由；非 0 时同样不能是回环/组播 ---- */
    gw_h = ntohl(c.gw);
    if (gw_h != 0u &&
        ((gw_h >> 24) == 127u || gw_h == 0xFFFFFFFFu ||
         (gw_h & 0xF0000000u) == 0xE0000000u)) {
        IV_LOG_W(NC_MOD, "0xF4 gw=0x%08x is loopback/broadcast/multicast", (unsigned)gw_h);
        return IV_EINVAL;
    }

    *out = c;
    return IV_OK;
}

/* 单播主机地址判定：返回 1 合法 / 0 非法。
 * `allow_zero` 决定 `0.0.0.0` 算不算合法 —— `0xF4` 的本机 IP 不允许（等于没填），
 * `0xF8` 的检测目标允许（0 ＝ 这一路不检测，界面清空就是把它填 0）。 */
static int host_ip_ok(uint32_t ip_h, int allow_zero)
{
    if (ip_h == 0u)
        return allow_zero;
    if ((ip_h >> 24) == 127u || ip_h == 0xFFFFFFFFu ||
        (ip_h & 0xF0000000u) == 0xE0000000u) /* 127/8 回环、受限广播、224/4 组播 */
        return 0;
    return 1;
}

int iv_netcfg_parse_targets(const uint8_t *data, size_t len,
                            uint32_t *t1, uint32_t *t2)
{
    uint32_t a;
    uint32_t b;

    if (data == NULL || t1 == NULL || t2 == NULL)
        return IV_EINVAL;
    if (len < 8u)
        return IV_EINVAL;
    /* 多于 8 字节只取前 8，理由同 `iv_netcfg_parse()`：平台若在尾部追加字段，
     * 按固定偏移解析仍然是对的，"多一字节就整体拒绝"会让平台一升级就全瘫。 */
    if (len > 8u) {
        IV_LOG_W(NC_MOD, "0xF8 payload %u bytes (expect 8), using the first 8",
                 (unsigned)len);
    }

    (void)memcpy(&a, data, 4u);
    (void)memcpy(&b, data + 4, 4u);
    if (!host_ip_ok(ntohl(a), 1)) {
        IV_LOG_W(NC_MOD, "0xF8 target1=0x%08x is loopback/broadcast/multicast",
                 (unsigned)ntohl(a));
        return IV_EINVAL;
    }
    if (!host_ip_ok(ntohl(b), 1)) {
        IV_LOG_W(NC_MOD, "0xF8 target2=0x%08x is loopback/broadcast/multicast",
                 (unsigned)ntohl(b));
        return IV_EINVAL;
    }
    *t1 = a;
    *t2 = b;
    return IV_OK;
}

int iv_netcfg_if_ip_equals(const char *ifname, uint32_t ip_be)
{
    struct ifaddrs *list = NULL;
    struct ifaddrs *p;
    const char     *ifn;
    int             found = 0;

    ifn = (ifname != NULL && ifname[0] != '\0') ? ifname : IV_NETCFG_IFNAME_DEFAULT;
    if (getifaddrs(&list) != 0)
        return -1;
    for (p = list; p != NULL; p = p->ifa_next) {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (p->ifa_name == NULL || strcmp(p->ifa_name, ifn) != 0)
            continue;
        found = 1;
        if (((const struct sockaddr_in *)(const void *)p->ifa_addr)->sin_addr.s_addr == ip_be) {
            (void)freeifaddrs(list);
            return 1;
        }
    }
    (void)freeifaddrs(list);
    return (found != 0) ? 0 : -1;
}

int iv_netcfg_apply(const iv_netcfg_t *c, const char *ifname)
{
    char        ipbuf[INET_ADDRSTRLEN]  = "";
    char        gwbuf[INET_ADDRSTRLEN]  = "";
    char        addrbuf[INET_ADDRSTRLEN + 4];
    const char *ifn;
    const char *ipbin;
    int         prefix;
    int         rc;

    if (c == NULL)
        return IV_EINVAL;
    ifn = (ifname != NULL && ifname[0] != '\0') ? ifname : IV_NETCFG_IFNAME_DEFAULT;

    prefix = iv_netcfg_prefix(c->mask);
    if (prefix < 1 || prefix > 32)
        return IV_EINVAL;
    if (inet_ntop(AF_INET, &c->ip, ipbuf, sizeof ipbuf) == NULL ||
        inet_ntop(AF_INET, &c->gw, gwbuf, sizeof gwbuf) == NULL)
        return IV_EINVAL;
    (void)snprintf(addrbuf, sizeof addrbuf, "%s/%d", ipbuf, prefix);

    /* 测试口子：只记账不改网络。**产线绝不设置** —— 设了就等于平台这条命令
     * 永远不生效、而界面还显示成功，是最坏的一种"假成功"。 */
    if (getenv("IVSBOX_NETCFG_DRY") != NULL) {
        IV_LOG_W(NC_MOD, "DRY RUN (IVSBOX_NETCFG_DRY): would set %s %s gw %s",
                 ifn, addrbuf, (ntohl(c->gw) != 0u) ? gwbuf : "(none)");
        return IV_OK;
    }

    ipbin = find_ip_bin();
    if (ipbin == NULL) {
        IV_LOG_E(NC_MOD, "no iproute2 binary found (tried /sbin/ip, /usr/sbin/ip, ...)");
        return IV_ENOENT;
    }

    IV_LOG_I(NC_MOD, "setting %s -> %s gw %s", ifn, addrbuf,
             (ntohl(c->gw) != 0u) ? gwbuf : "(none)");

    /* ---- 顺序有讲究 ----
     * 1) 先 flush 再 add：只 `addr replace` 的话，新旧地址不是同一个地址，
     *    结果是两个并存（现场会看到"改完旧 IP 还能连"，以为没生效）。
     *    flush 与 add 之间有几毫秒没有地址 —— 可接受，改地址本来就要断链。
     * 2) 先把地址加上再换默认路由：否则 `route replace` 会因为"网关不在任何
     *    直连网段"而被内核拒（ENETUNREACH）。
     * 3) 最后确保 link up（flush 不会 down 网口，但若之前是 down 的就白改了）。 */
    {
        char *argv[8];

        argv[0] = (char *)ipbin;
        argv[1] = (char *)"-4";
        argv[2] = (char *)"addr";
        argv[3] = (char *)"flush";
        argv[4] = (char *)ifn;
        argv[5] = (char *)"scope";
        argv[6] = (char *)"global";
        argv[7] = NULL;
        rc = run_argv(argv);
        if (rc != IV_OK)
            return rc;
    }
    {
        char *argv[8];

        argv[0] = (char *)ipbin;
        argv[1] = (char *)"-4";
        argv[2] = (char *)"addr";
        argv[3] = (char *)"add";
        argv[4] = addrbuf;
        argv[5] = (char *)"dev";
        argv[6] = (char *)ifn;
        argv[7] = NULL;
        rc = run_argv(argv);
        if (rc != IV_OK)
            return rc;
    }
    if (ntohl(c->gw) != 0u) {
        char *argv[10];

        argv[0] = (char *)ipbin;
        argv[1] = (char *)"-4";
        argv[2] = (char *)"route";
        argv[3] = (char *)"replace";
        argv[4] = (char *)"default";
        argv[5] = (char *)"via";
        argv[6] = gwbuf;
        argv[7] = (char *)"dev";
        argv[8] = (char *)ifn;
        argv[9] = NULL;
        rc = run_argv(argv);
        if (rc != IV_OK)
            return rc;
    }
    {
        char *argv[6];

        argv[0] = (char *)ipbin;
        argv[1] = (char *)"link";
        argv[2] = (char *)"set";
        argv[3] = (char *)ifn;
        argv[4] = (char *)"up";
        argv[5] = NULL;
        rc = run_argv(argv);
        if (rc != IV_OK)
            return rc;
    }
    return IV_OK;
}
