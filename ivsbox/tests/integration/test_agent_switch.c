/*
 * iv_agent 装配层「切出口」端到端集成测试
 * （功能开发计划 M3 退出条件 —— T113运维终端-系统架构设计.md §16.2 场景 1/2）
 *
 * ============================ 它验的是什么 ============================
 * 单测（tests/unit/test_netlink.c / test_probe.c / test_netmgr.c / test_report.c）
 * 各自证明「模块本身对不对」；本用例证明 **iv_agent 把 S3.1~S3.6 接起来之后，
 * 整条链在真实内核事件下真的会切**：
 *
 *     ip link set usb0 down
 *       └─ 真内核 rtnetlink 事件
 *            └─ iv_netlink_poll → on_nl_evt（装配层转接）
 *                 └─ iv_probe_set_link（LINK 探活即时判死，不等计数）
 *                      └─ netmgr_feed → iv_netmgr 迟滞判定 + §7.2 六态状态机
 *                           └─ 七步事务：
 *                                switch_route = 真 netlink 改默认路由（在专用 netns 内）
 *                                reconnect    = iv_report_reconnect（关旧 TCP、原址重连）
 *                                     └─ 队列里的欠包由取件泵按序补传
 *
 * 覆盖（对照 §16.2）：场景 1「拔线」＝k04；场景 2「弱网/回切/补传」＝k05+k06。
 *   k01 装配起步：接口名解析成 ifindex、探活项建出、初始「无线优先」；
 *   k02 摸底：真内核 LINK_UP → probe 状态 UP → netmgr 标记两条 WAN 可用；
 *   k03 基线上报：平台建连、立即上报经 队列→取件泵→TCP 到平台（## 壳完好）；
 *   k04 拔线：无线 carrier 掉 → 状态机切有线，**事务真的成功**（= 路由真改成功
 *       + 重连真发生：传输层建连计数 +1、平台侧收到第二条 TCP）；
 *   k05 平台断：链路事件之外再断平台 → 上报入队积压、不丢；
 *   k04b M4-S4.4：无线掉线的那条沿 ⇒ MN 变 2 ⇒ 继电器 1 断电重启（pty 上的 0xD1）；
 *   k06 回切 + 补传：无线恢复、跨过 hold_s 自动切回无线，欠包按序补发、队列清空；
 *   k07 收口：资源全释放，不留 fd（规则 6 之外的兜底）。
 *
 * ============================ 为什么必须进私有 netns ============================
 * 三个硬需求，只有私有 netns 能同时满足：
 *   1) **接口名可造**：iv_agent 按名字解析 ifindex（架构 §18.3 冻结 usb0/eth0），
 *      普通环境里这两个接口多半不存在，探活项建不出来 → 整条链不成立；
 *   2) **链路可拨**：`ip link set usb0 down` 需要 CAP_NET_ADMIN，普通用户没有；
 *   3) **路由可改**：iv_agent 把 `switch_route` 接到 `iv_netmgr_ops_default()`，
 *      即**真** netlink 改默认路由 —— 拿「打桩返回 IV_OK」糊弄过去的话，测的
 *      就不是装配层实际接的那条路。私有 netns 里改的是**自己的**路由表，
 *      不碰宿主网络。
 * 所以用例先 `unshare -rmn` 重启到私有 user+net+mount ns（无需 root），
 * 在 ns 内造 usb0/eth0 两张 dummy 网卡。**unshare 不可用时优雅 SKIP（退出 0）**，
 * 绝不让 `make itest` 因环境受限而假红（与 test_e2e_report 的 pty SKIP 同口径）。
 *
 * ============================ 两种模式：netns（默认）/ 真网卡 ============================
 * 默认走上面的私有 netns —— 适合 VM，网卡是造的，对宿主零影响。
 * 板端（TQ113）**没有 unshare、也没有 dummy/veth 模块**；但只要把环境变量
 * `IV_AGENT_TEST_REALNIC=1` 置位，用例就改走「真网卡模式」：不建 ns、不造 dummy，
 * 直接在**真实 usb0/eth0** 上跑同一条场景链（接口必须已存在，否则 SKIP）。
 * 两处差异必须知道：
 *   1) 真接口多半**已经 UP**，而内核只在**状态变化**时才发 RTM_NEWLINK —— 靠的是
 *      **装配层的开局摸底**（`iv_agent_open()` 同步读一次 carrier 播进 probe）把
 *      当前真实状态播出来；k02 因此直接断言"真接口开局就被播成 UP"，不再自己顶
 *      一条事件（那是在补偿装配层"开局不摸底"的缺口，缺口已修）；
 *   2) 用例的 switch_route 步会**真改板子的路由表**（留下两条 prio 100 的默认路由），
 *      k06 掉/起 usb0 后还要 udhcpc 重取地址 —— 收尾（k07）会把这些痕迹清干净。
 * 真网卡模式与 **4G 看护 `S62usb4g` 互斥**：看护每 30 s 会把 usb0 重新拨起来，与本
 * 用例的「拔线」直接打架。检测到看护在跑时用例打印 SKIP 并退出 0。
 * 真网卡模式会**真改板子路由、真断 4G 数据面**，务必在有人值守时跑。
 *
 * ============================ 驱动方式：无 Reactor、注入时间轴 ============================
 * Reactor 只有阻塞式 `iv_reactor_run()`，没有可限时单步的接口；若用它就得把
 * 场景塞进定时器回调、按真实墙钟等待，用例会随宿主负载抖动。
 * 本用例改走装配层自带的 `r = NULL` 路径：自己调 `iv_agent_step(now_ms)` 注入
 * 时间轴（hold_s / 退避窗口可以瞬间跨过），**网络侧仍是真实 I/O**。
 * 唯一「手动」的一步是 netlink 拉取 —— 无 Reactor 时装配层不注册 netlink fd，
 * 调用方须自行 `iv_netlink_poll()`（与 `on_nl_fd` 里的动作等价），这正是
 * `iv_agent.h` 对 `r = NULL` 的口径。
 *
 * ============================ 反向证伪点（每条都对应一个真缺陷） ============================
 *   k04 拔线必须切到有线 —— `netmgr_feed` 若把探活结论恒当成功（ok 恒 1），
 *        链路断了也不切，此处必红；
 *   k04 重连必须真发生 —— `agent_reconnect` 若不调 `iv_report_reconnect`
 *        （直接 return IV_OK），TCP 永远绑在旧出口，此处必红；
 *   k04 链路事件必须被消费 —— `on_nl_evt` 若不再把 LINK_UP/DOWN 转给
 *        `iv_probe_set_link`，probe 状态永不变化，此处必红；
 *   k04b 主网故障沿（M4-S4.4）—— `env_feed` 若改回裸 `iv_probe_state()`（单次
 *        超时即判死）、或干脆不喂 MN、或不把沿接给 `iv_recovery`，
 *        k04b 的"MN 变 2 ＋ 继电器 1 收到 0xD1"必红。
 *
 * ============================ 临时产物 ============================
 * 仅 $TMPDIR 下一个临时目录（持久队列），用例结束前删除；私有 netns 随进程
 * 退出自动回收。AGENTS.md 规则 6。
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h> /* 真网卡模式：取网卡自己的 IPv4 */
#include <netinet/in.h>
#include <poll.h>
#include <signal.h> /* kill()：探测 4G 看护是否在跑 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_db.h"
#include "ivsbox/iv_frame.h" /* S4.5：造 0xC1 上行帧喂给 pty 假采集板 */
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_netlink.h"
#include "ivsbox/iv_netmgr.h"
#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_store.h"
#include "ivsbox/iv_transport.h"

/* 两张 WAN 的接口名（架构 §18.3 冻结：无线=usb0 / 有线=eth0） */
#define WAN_WL        "usb0"
#define WAN_WD        "eth0"
/* 两条出口各自的平台地址（同一监听端口、**两条不同地址**）：
 * 每个地址都落在**被绑定的那张网卡自己身上** —— 用来钉住"换出口时平台地址跟着换"
 * （监听套接字绑 INADDR_ANY，故两个地址都能连上）。
 *
 * ⚠ 不能用 127.0.0.1 / 127.0.0.2（2026-10-21 实测：k03 起 9 条断言全红）：
 * 装配层把 `bind_ifname` 设成**活动出口的网卡名**（iv_agent.c 的 open /
 * agent_reconnect），传输层照做 `SO_BINDTODEVICE`。socket 被钉在 usb0/eth0 上后，
 * 127.0.0.0/8 的 local 路由挂在 **lo** 上 ⇒ oif 不匹配 ⇒ 落到 main 表的
 * `default dev usb0` ⇒ SYN 从 dummy 发出去，dummy 的 ndo_start_xmit 直接丢包，
 * **既没有 RST 也没有 SYN-ACK** ⇒ connect 永远停在 EINPROGRESS（要等 ~127 s 的
 * TCP 重传超时才失败），用例的注入时钟等不到那一天。
 * 换成"本网卡自己的地址"后，table local 里的 `local 10.9.0.1 dev usb0` 与 oif 匹配，
 * 内核按 RTN_LOCAL 走本地投递（dev_out 强制 lo），连接当场建立。 */
#define WL_ADDR       "10.9.0.1/24"
#define WD_ADDR       "10.8.0.1/24"
/* 「已进入私有 netns」的环境标记；未置位则自重启 */
#define NS_ENV        "IV_AGENT_TEST_NETNS"
/* 「真网卡模式」环境标记（板端无 unshare/dummy，直接跑真 usb0/eth0） */
#define REALNIC_ENV   "IV_AGENT_TEST_REALNIC"
/* 每拍注入的毫秒数（＝ iv_report 的 tick 节拍量级） */
#define STEP_MS       200u

/* iv_agent_t 约 35 KB（内含 iv_report 的队列/传输缓冲）—— 必须放静态区，勿放栈 */
static iv_agent_t g_agent;
static int        g_fail;
static int        g_realnic; /* 1 = 真网卡模式（REALNIC_ENV 置位） */
static uint64_t   g_now;
/* 两条出口各自的平台地址（netns：造地址时读回；真网卡：取网卡自己的 IPv4） */
static char       g_srv_wl[INET_ADDRSTRLEN];
static char       g_srv_wd[INET_ADDRSTRLEN];

/* 串口下行字节的**留存副本**：pump_agent 原先是"读了就扔"，M4-S4.4 要证明
 * 主网故障沿真的变成了 pty 上的 0xD1 报文，就得把字节留下。 */
static uint8_t    g_cap[65536];
static size_t     g_capn;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 让日志不走落盘：iv_log 的落盘出口是**进程级单例、懒打开**（第一次写日志时才建），
 * 板端 /mnt/UDISK 可写时会在装配过程中多出一个 fd 并留到进程结束 —— 会把 k07 的
 * 「不泄漏」断言打成假红（2026-10-12 板端实测：多的那个 fd 指向 /mnt/UDISK/log/...
 * 的日志文件）。用日志模块自带的单测注入口关掉落盘，用例才与宿主的日志根目录可写
 * 性无关（VM 上 /mnt/UDISK 不可写、板端可写，两条环境必须给出同一结论）。 */
static void log_discard(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

/* ---------------------------------------------------------------------------
 * 夹具
 * ------------------------------------------------------------------------- */

/* /proc/self/fd 条目数：open/close 一轮不泄漏 fd（同 test_netlink 口径） */
static int count_open_fds(void)
{
    DIR           *d;
    struct dirent *e;
    int            n = 0;

    d = opendir("/proc/self/fd");
    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.')
            n++;
    }
    (void)closedir(d);
    return n;
}

/* 泄漏时把每个句柄的指向打出来（只在断言失败时调）——没有它就只能靠猜。
 * 条目名就是 fd 号，readlink 给出它指向什么（socket / 文件 / 管道）。 */
static void dump_fds(void)
{
    DIR           *d;
    struct dirent *e;

    d = opendir("/proc/self/fd");
    if (d == NULL) {
        fprintf(stderr, "  (cannot open /proc/self/fd)\n");
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char    p[256];
        char    tgt[512];
        ssize_t n;

        if (e->d_name[0] == '.')
            continue;
        if (snprintf(p, sizeof p, "/proc/self/fd/%s", e->d_name) >= (int)sizeof p)
            continue;
        n = readlink(p, tgt, sizeof tgt - 1u);
        if (n < 0)
            continue;
        tgt[n] = '\0';
        fprintf(stderr, "  fd %s -> %s\n", e->d_name, tgt);
    }
    (void)closedir(d);
}

static int pty_pair(char *slave_path, size_t cap)
{
    int m;

    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;
    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        (void)close(m);
        return -1;
    }
    if (ptsname_r(m, slave_path, cap) != 0) {
        (void)close(m);
        return -1;
    }
    if (fcntl(m, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(m);
        return -1;
    }
    return m;
}

/* 在回环上监听指定端口（in_port==0 表示随机挑）；回填**实际端口**。
 * SO_REUSEADDR：k06 要在同一端口上重新监听（上一次的连接的 TIME_WAIT 不该挡住）。 */
static int listen_port(uint16_t in_port, uint16_t *out_port)
{
    int                ls;
    int                on = 1;
    struct sockaddr_in sa;
    socklen_t          sl = sizeof sa;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0)
        return -1;
    (void)setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    /* 绑 INADDR_ANY（而非 INADDR_LOOPBACK）：本用例用两条**分属不同出口网卡**的
     * 地址区分无线口/有线口的平台地址，二者同端口、都指向本套接字。 */
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons(in_port);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        listen(ls, 4) != 0 ||
        getsockname(ls, (struct sockaddr *)&sa, &sl) != 0) {
        (void)close(ls);
        return -1;
    }
    *out_port = ntohs(sa.sin_port);
    return ls;
}

static int accept_ms(int ls, int ms)
{
    struct pollfd p;

    if (ls < 0)
        return -1;
    p.fd      = ls;
    p.events  = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, ms) <= 0)
        return -1;
    return accept(ls, NULL, NULL);
}

/* 读干 fd（pty 与 socket 通用）；ms 为首轮 poll 等待上限，之后一律不阻塞 */
static size_t drain(int fd, char *out, size_t cap, int ms)
{
    size_t n = 0;

    for (;;) {
        struct pollfd p;
        ssize_t       r;

        p.fd      = fd;
        p.events  = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, (n == 0u) ? ms : 0) <= 0)
            break;
        r = read(fd, out + n, cap - n);
        if (r <= 0)
            break; /* EAGAIN / EOF / 错误：都当"没得读了" */
        n += (size_t)r;
        if (n >= cap)
            break;
    }
    return n;
}

static int mk_tmpdir(char *out, size_t cap)
{
    const char *base = getenv("TMPDIR");
    char        tpl[256];

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    if (snprintf(tpl, sizeof tpl, "%s/ivsbox-agent-XXXXXX", base) >=
        (int)sizeof tpl)
        return -1;
    if (mkdtemp(tpl) == NULL)
        return -1;
    if (strlen(tpl) >= cap)
        return -1;
    (void)strcpy(out, tpl);
    return 0;
}

static void rm_tmpdir(const char *dir)
{
    DIR *d;

    d = opendir(dir);
    if (d == NULL)
        return;
    for (;;) {
        struct dirent *e = readdir(d);
        char           p[512];

        if (e == NULL)
            break;
        if (e->d_name[0] == '.')
            continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) < (int)sizeof p)
            (void)unlink(p);
    }
    (void)closedir(d);
    (void)rmdir(dir);
}

/* ---------------------------------------------------------------------------
 * 上报帧解析（口径同 test_e2e_report：`##` + 4 位长度 + 数据段 + 2 位 CRC）
 * ------------------------------------------------------------------------- */

/* 走一条 ## 上报帧，回填 seg 起点与长度；成功返回帧总字节数，否则 0 */
static size_t next_report_frame(const char *buf, size_t len, size_t off,
                                const char **seg, size_t *seg_len)
{
    size_t sl;

    /* 固定开销 10＝`##`2 + 长度域4 + CRC2 + `##`2。末尾 `##` **必须落在帧内**：
     * 以前按 8 算，帧尾被截掉也不报错，线上表现为平台隔一帧丢一帧。 */
    if (off + 10u > len)
        return 0u;
    if (buf[off] != '#' || buf[off + 1u] != '#')
        return 0u;
    sl = (size_t)(buf[off + 2u] - '0') * 1000u +
         (size_t)(buf[off + 3u] - '0') * 100u +
         (size_t)(buf[off + 4u] - '0') * 10u + (size_t)(buf[off + 5u] - '0');
    if (off + 10u + sl > len)
        return 0u;
    if (buf[off + 6u + sl + 2u] != '#' || buf[off + 6u + sl + 3u] != '#')
        return 0u;
    *seg     = buf + off + 6u;
    *seg_len = sl;
    return 10u + sl;
}

/* 取 `CSQ=<n>;` 中的 n；没有则 -1（用例用它给每帧打序号，证补传按序） */
static int seg_csq(const char *seg, size_t seg_len)
{
    int i;

    for (i = 0; i < 100; i++) {
        char        pat[16];
        const char *p;

        (void)snprintf(pat, sizeof pat, "CSQ=%d;", i);
        p = strstr(seg, pat);
        if (p != NULL && (size_t)(p - seg) < seg_len)
            return i;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * iproute2 调用（fork+exec，不经 shell）
 * ------------------------------------------------------------------------- */
static int run_ip(char *const argv[])
{
    pid_t p;
    int   st = 0;

    p = fork();
    if (p < 0)
        return -1;
    if (p == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (waitpid(p, &st, 0) < 0)
        return -1;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static int ip_add_dummy(const char *name)
{
    char *av[] = { (char *)"ip", (char *)"link", (char *)"add", (char *)name,
                   (char *)"type", (char *)"dummy", NULL };
    return run_ip(av);
}

static int ip_set_state(const char *name, int up)
{
    char *av[] = { (char *)"ip", (char *)"link", (char *)"set", (char *)name,
                   (char *)(up ? "up" : "down"), NULL };
    return run_ip(av);
}

/* 给网卡加一个 IPv4（netns 里造出来的 dummy 没有地址，而**没有地址的网卡做不了
 * SO_BINDTODEVICE 的连接目标** —— 理由见文件头 SRV_* 那段）。 */
static int ip_addr_add(const char *name, const char *cidr)
{
    char *av[] = { (char *)"ip", (char *)"addr", (char *)"add", (char *)cidr,
                   (char *)"dev", (char *)name, NULL };
    return run_ip(av);
}

/* 取某网卡的 IPv4（点分十进制）。没有地址返回 0。 */
static int iface_ipv4(const char *name, char *out, size_t cap)
{
    struct ifaddrs *ifs = NULL;
    struct ifaddrs *p;
    int             ok = 0;

    if (getifaddrs(&ifs) != 0)
        return 0;
    for (p = ifs; p != NULL; p = p->ifa_next) {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(p->ifa_name, name) != 0)
            continue;
        if (inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)p->ifa_addr)->sin_addr,
                      out, (socklen_t)cap) == NULL)
            continue;
        ok = 1;
        break;
    }
    freeifaddrs(ifs);
    return ok;
}

/* ---------------------------------------------------------------------------
 * 真网卡模式（板端）专用助手
 * ------------------------------------------------------------------------- */

/* 接口在不在（/sys/class/net 有目录即有）。真网卡模式下 usb0/eth0 必须已存在。 */
static int iface_present(const char *name)
{
    char p[128];

    if (snprintf(p, sizeof p, "/sys/class/net/%s", name) >= (int)sizeof p)
        return 0;
    return access(p, F_OK) == 0;
}

/* 注：真网卡模式**不再自己"顶"一条 RTM_NEWLINK**。早期版本靠一次 promisc 往返
 * 把当前状态顶出来（MTU 往返在板端 eth0/stmmac 上直接 EBUSY），但那是在补偿装配
 * 层"开局不摸底"的缺口（2026-10-12 板端真链路验收查出）。缺口已在 `iv_agent_open()`
 * 里修掉（open 时同步探一次链路可用性播进 probe），故此处只需断言，无需造事件。 */

/* 4G 看护（S62usb4g）是否在跑 —— 它会把 usb0 重新拨起来，与本用例的「拔线」互斥。
 * 判据取脚本自己的 pidfile（/var/run/usb4g.pid），且进程必须还活着。 */
static int usb4g_supervisor_running(void)
{
    FILE *f   = fopen("/var/run/usb4g.pid", "r");
    int   pid = 0;

    if (f == NULL)
        return 0;
    if (fscanf(f, "%d", &pid) != 1)
        pid = 0;
    (void)fclose(f);
    return (pid > 1 && kill((pid_t)pid, 0) == 0) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * 私有 netns：探测可用性 + 自重启
 * ------------------------------------------------------------------------- */

/* `unshare -rmn` 是否可用（内核允许非特权 userns、且 unshare 可执行）。
 * 不可用时用例整体 SKIP，而不是报失败 —— 环境受限不该算代码错。 */
static int netns_available(void)
{
    pid_t p;
    int   st = 0;

    p = fork();
    if (p < 0)
        return 0;
    if (p == 0) {
        execlp("unshare", "unshare", "-rmn", "--", "true", (char *)NULL);
        _exit(127);
    }
    if (waitpid(p, &st, 0) < 0)
        return 0;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 1 : 0;
}

/* 在私有 netns 里重跑自己；子进程退出码原样上抛 */
static int reexec_in_netns(const char *self)
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
        execlp("unshare", "unshare", "-rmn", "--", self, (char *)NULL);
        _exit(127); /* unshare 可执行文件不存在 */
    }
    if (waitpid(p, &st, 0) < 0) {
        printf("SKIP: waitpid failed\n");
        return 0;
    }
    if (!WIFEXITED(st)) {
        printf("SKIP: child terminated abnormally\n");
        return 0;
    }
    if (WEXITSTATUS(st) == 127) {
        printf("SKIP: unshare not found on PATH\n");
        return 0;
    }
    return (int)WEXITSTATUS(st);
}

/* ---------------------------------------------------------------------------
 * 驱动助手
 * ------------------------------------------------------------------------- */

/* 读干 pty master 并把字节追加进 g_cap（溢出即截断：本用例只关心"有没有"）。 */
static void cap_suck(int master)
{
    char   b[1024];
    size_t dn;

    if (master < 0)
        return;
    dn = drain(master, b, sizeof b, 0);
    if (dn == 0u)
        return;
    if (dn > sizeof g_cap - g_capn)
        dn = sizeof g_cap - g_capn;
    memcpy(g_cap + g_capn, b, dn);
    g_capn += dn;
}

/* 自 from 起，g_cap 里是否出现过一条 `F0 F0 <cmd>` 开头的下行帧。 */
static int cap_has_frame_from(size_t from, uint8_t cmd)
{
    size_t i;

    if (from > g_capn)
        from = g_capn;
    for (i = from; i + 2u < g_capn; i++) {
        if (g_cap[i] == 0xF0u && g_cap[i + 1u] == 0xF0u && g_cap[i + 2u] == cmd)
            return 1;
    }
    return 0;
}

/* 推进 n 拍；每拍把 pty master 上的下行字节抽掉（防 pty 缓冲写满） */
static void pump_agent(int master, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        cap_suck(master);
        g_now += STEP_MS;
        (void)iv_agent_step(&g_agent, g_now);
    }
}

/* 拉干 netlink 直到指定 WAN 的探活状态达到期望（最多约 1 s 墙钟）。
 * 状态由**真实内核 LINK 事件**驱动 —— 这正是 on_nl_evt → iv_probe_set_link
 * 的转接；无 Reactor 时拉取由调用方负责（见文件头）。 */
static int wait_link(int wan, int want_up)
{
    int i;
    int idx = g_agent.wan_probe_idx[wan];

    if (idx < 0)
        return 0;
    for (i = 0; i < 100; i++) {
        iv_probe_state_t s = iv_probe_state(&g_agent.pb, idx);

        if (want_up ? (s == IV_PROBE_UP) : (s == IV_PROBE_DOWN))
            return 1;
        (void)iv_netlink_poll(&g_agent.nl);
        (void)usleep(10u * 1000u);
    }
    return 0;
}

/* 等某一路探活项翻 UP。和 `wait_link()` 的区别：现在探活项是 ICMP 探测（探的是
 * 平台地址），**状态要靠 iv_agent_step() 真的把探测发出去、把回包读回来**才能攒够
 * `ok_n` 个成功样本 —— 光拉 netlink 只会停在 UNKNOWN（2026-10-29 语义纠正后
 * `test_agent_switch` 一开始就是栽在这里的）。所以这里推拍（含 pty 抽干）。 */
static int wait_probe_up(int wan, int master, int max_steps)
{
    int i;
    int idx = g_agent.wan_probe_idx[wan];

    if (idx < 0)
        return 0;
    for (i = 0; i < max_steps; i++) {
        (void)iv_netlink_poll(&g_agent.nl); /* 无 Reactor 时 netlink 由调用方拉 */
        if (iv_probe_state(&g_agent.pb, idx) == IV_PROBE_UP)
            return 1;
        pump_agent(master, 1);
    }
    return iv_probe_state(&g_agent.pb, idx) == IV_PROBE_UP;
}

static int wait_transport_up(int max_steps)
{
    int i;

    for (i = 0; i < max_steps; i++) {
        if (iv_transport_state(&g_agent.rp.tr) == IV_TRANSPORT_UP)
            return 1;
        g_now += STEP_MS;
        (void)iv_agent_step(&g_agent, g_now);
    }
    return iv_transport_state(&g_agent.rp.tr) == IV_TRANSPORT_UP;
}

static int wait_queue_empty(int max_steps)
{
    int i;

    for (i = 0; i < max_steps; i++) {
        if (iv_report_queue_count(&g_agent.rp) == 0u)
            return 1;
        g_now += STEP_MS;
        (void)iv_agent_step(&g_agent, g_now);
    }
    return iv_report_queue_count(&g_agent.rp) == 0u;
}

/* ---------------------------------------------------------------------------
 * 主流程（私有 netns 内执行）
 * ------------------------------------------------------------------------- */
static int scenario(void)
{
    char            slave_path[128];
    char            qdir[256];
    int             master = -1;
    int             ls     = -1;
    int             cs     = -1;
    uint16_t        port   = 0;
    uint16_t        port2  = 0;
    iv_agent_cfg_t  cfg;
    iv_report_env_t env;
    iv_db_t        *tdb = NULL;
    iv_store_t     *tstore = NULL;
    char            dbdir[128] = "";
    int             fd_before;
    int             fd_base;
    int             i;
    int             j;
    size_t          cap_mark = 0u;

    /* 先把日志落盘出口摘掉（进程级单例、懒打开，理由见 log_discard），再取水位 ——
     * 否则板端 /mnt/UDISK 可写会凭空多一个 fd，k07 必假红。 */
    iv_log_set_sink(log_discard, NULL);

    /* 用例自建任何 fd（pty / listener）**之前**取水位：收尾把自建 fd 全关掉后
     * 应回到这里，k07 用它判装配层有没有漏关（取晚了会把测试自己的句柄算成泄漏）。 */
    fd_base = count_open_fds();

    /* ---- 网卡准备 ----
     * 默认（netns）：造两张 dummy，状态留给 k02 拨动；
     * 真网卡模式（板端）：沿用真实 usb0/eth0 —— 必须已存在，否则 SKIP。 */
    if (g_realnic) {
        if (!iface_present(WAN_WL) || !iface_present(WAN_WD)) {
            printf("SKIP: real-NIC mode needs existing %s / %s\n", WAN_WL, WAN_WD);
            return 0;
        }
        /* 平台地址＝网卡自己的 IPv4：真 usb0/eth0 已配地址，直接取；取不到说明
         * 链路还没起来，本用例无法造出"可被绑定的连接目标" ⇒ SKIP。 */
        if (!iface_ipv4(WAN_WL, g_srv_wl, sizeof g_srv_wl) ||
            !iface_ipv4(WAN_WD, g_srv_wd, sizeof g_srv_wd)) {
            printf("SKIP: real-NIC mode needs an IPv4 on both %s / %s\n",
                   WAN_WL, WAN_WD);
            return 0;
        }
    } else {
        if (ip_add_dummy(WAN_WL) != 0 || ip_add_dummy(WAN_WD) != 0) {
            printf("SKIP: cannot create dummy interfaces (iproute2 missing?)\n");
            return 0;
        }
        if (ip_set_state("lo", 1) != 0) {
            printf("SKIP: cannot bring lo up\n");
            return 0;
        }
        if (ip_addr_add(WAN_WL, WL_ADDR) != 0 || ip_addr_add(WAN_WD, WD_ADDR) != 0) {
            printf("SKIP: cannot assign addresses to the dummy interfaces\n");
            return 0;
        }
        if (!iface_ipv4(WAN_WL, g_srv_wl, sizeof g_srv_wl) ||
            !iface_ipv4(WAN_WD, g_srv_wd, sizeof g_srv_wd)) {
            printf("SKIP: cannot read back the dummy addresses\n");
            return 0;
        }
    }

    master = pty_pair(slave_path, sizeof slave_path);
    if (master < 0) {
        printf("SKIP: cannot create pty pair\n");
        return 0;
    }
    ls = listen_port(0u, &port);
    if (ls < 0 || mk_tmpdir(qdir, sizeof qdir) != 0) {
        printf("SKIP: cannot set up loopback listener / tmpdir\n");
        return 0;
    }

    /* ---- 配置：平台走回环、采集板走 pty、队列走临时目录 ---- */
    iv_agent_cfg_default(&cfg);
    cfg.serial_path  = slave_path;
    cfg.queue_dir    = qdir;
    cfg.srv_host[IV_WAN_WIRELESS] = g_srv_wl;
    cfg.srv_port[IV_WAN_WIRELESS] = port;
    cfg.srv_host[IV_WAN_WIRED]    = g_srv_wd;
    cfg.srv_port[IV_WAN_WIRED]    = port;
    cfg.devid        = 0x101u;
    cfg.mod          = "AGENT-MOD";
    cfg.sv           = "AGENT-SV";
    cfg.report_ms    = 3600u * 1000u; /* 关周期上报：用 trigger 精确催报 */
    cfg.heartbeat_ms = 3600u * 1000u; /* 关心跳：会污染收包断言 */
    cfg.poll_ms      = 3600u * 1000u; /* 关 0xE1 轮询：镜像不参与本用例 */
    (void)snprintf(cfg.wan_name[IV_WAN_WIRELESS], IV_AGENT_WAN_NAME_MAX, "%s", WAN_WL);
    (void)snprintf(cfg.wan_name[IV_WAN_WIRED], IV_AGENT_WAN_NAME_MAX, "%s", WAN_WD);
    /* 探测目标不再取 `probe_target_be`（2026-10-29 语义纠正）：装配层现在按
     * `iv_agent_probe_target_of()` **逐路探平台地址**，也就是上面那几个
     * `srv_host[]`（各自网卡自己的 IPv4 ⇒ 本地回环直答应答，可控可复现）。
     * `probe_target_be` 只剩"平台写成域名时的兜底"，本用例留 0 即可。
     *
     * 连带后果：探活项从"被动吃 carrier 事件的 LINK 项"变成了"要发 ICMP 的
     * 探测项"，所以状态不再随 LINK 事件瞬时翻 UP —— 得先把 2 个成功样本攒够
     * （见 `wait_probe_up()`）。周期压到库的下界 500 ms，让这段等待短到不会
     * 把 k04/k06 的时间模型带歪。 */
    cfg.probe_target_be[0] = 0u;
    cfg.probe_target_be[1] = 0u;
    cfg.probe_interval_ms   = 500u;
    /* 关掉延时降级门限：本用例的时间是**注入**的（`g_now` 按拍跳 200 ms，k06
     * 还会一次跨过 hold_s），回包被读到的那一刻 RTT 会算成整段跳变量 ⇒ 默认的
     * 800 ms 门限会把好链路判成 DEGRADED，MN 变 4、k02b/k06b 假红。降级阈值
     * 已由 `test_probe` / `test_agent_mn` 单独覆盖，这里不重复测。 */
    cfg.probe_delay_ms      = 60000u;
    cfg.mode       = IV_NETMGR_MODE_AUTO;
    cfg.nm_fail_n  = 1u; /* 迟滞压到 1：一次结论即判，用例不必灌多次 */
    cfg.nm_ok_n    = 1u;
    cfg.nm_hold_s  = 3u; /* 回切稳定期 3 s：要容得下 wait_probe_up() 攒样本那点
                          * 时间（≤2 s），否则 k06 的"hold_s 未到不许回切"会假红。
                          * 时间轴仍由用例注入，瞬间即可跨过。 */
    cfg.nm_stable_s = 0u; /* 立即恢复媒体；本用例不驱动媒体步 */
    /* M4-S4.4：出厂默认 reboot_max=0（禁用自愈）—— 那条路径已经在
     * test_recovery 单测里钉死；本用例要验的是**接线**，故打开它。 */
    cfg.netthr_reboot_max = 2u;
    cfg.netthr_reboot_interval_s = 60u;

    /* ---- k01 装配起步 ---- */
    /* S9 延伸：装配 control.db —— k04 的出口切换要落 net_event。store 的 db
     * 文件句柄是**用例自建 fd**，k07 对账前必须先收掉（见收尾处）。 */
    {
        char dbpath[256];

        if (mk_tmpdir(dbdir, sizeof dbdir) != 0) {
            printf("SKIP: cannot create store tmpdir\n");
            (void)close(master);
            (void)close(ls);
            rm_tmpdir(qdir);
            return 0;
        }
        (void)snprintf(dbpath, sizeof dbpath, "%s/control.db", dbdir);
        if (iv_db_open(dbpath, &tdb) != IV_OK || tdb == NULL ||
            iv_store_open(tdb, &tstore) != IV_OK || tstore == NULL) {
            printf("FAIL: cannot open control.db for net_event assertions\n");
            iv_store_close(tstore);
            iv_db_close(tdb);
            (void)close(master);
            (void)close(ls);
            rm_tmpdir(qdir);
            rm_tmpdir(dbdir);
            return 1;
        }
        cfg.store = tstore;
    }

    fd_before = count_open_fds();
    g_now     = 0u;
    iv_agent_init(&g_agent);
    chk(iv_agent_open(&g_agent, &cfg, NULL, g_now) == IV_OK, "k01 open");
    chk(count_open_fds() > fd_before, "k01 open() acquired fds (serial / netlink / transport)");
    chk(iv_agent_is_open(&g_agent) == 1, "k01 agent is_open");
    chk(iv_agent_report(&g_agent) != NULL, "k01 exposes the report handle");
    chk(iv_agent_netmgr_state(&g_agent) == IV_NETMGR_WIRELESS_UP,
        "k01 initial state WIRELESS_UP (无线优先)");
    chk(iv_agent_active_wan(&g_agent) == IV_WAN_WIRELESS, "k01 initial active = wireless");
    chk(strcmp(iv_agent_report(&g_agent)->srv_host, g_srv_wl) == 0,
        "k01 platform addr = the wireless one (按初始活动出口选址)");
    chk(strcmp(iv_agent_report(&g_agent)->tr.host, g_srv_wl) == 0,
        "k01 transport really dials the wireless addr");
    /* 装配把接口名解析成了 ifindex；解析不到整条链就不成立，必须钉死 */
    chk(g_agent.wan_ifindex[IV_WAN_WIRELESS] > 0, "k01 wireless ifindex resolved");
    chk(g_agent.wan_ifindex[IV_WAN_WIRED] > 0, "k01 wired ifindex resolved");
    chk(g_agent.wan_probe_idx[IV_WAN_WIRELESS] >= 0, "k01 wireless probe item created");
    chk(g_agent.wan_probe_idx[IV_WAN_WIRED] >= 0, "k01 wired probe item created");
    chk(iv_report_serial_fd(&g_agent.rp) >= 0, "k01 serial opened (pty)");

    /* 开局摸底：装配完成时两路 WAN 的 LINK 项就该是**当前真实状态**，而不是
     * UNKNOWN —— netlink 不回放现状，装配层不自己读一次的话，"接口早就 UP" 的
     * 场景（板端开机就是如此）会一直等不到事件。真网卡模式＝真接口本来就 UP
     * ⇒ 应播成 UP；netns 模式＝dummy 刚建出来是 DOWN ⇒ 应播成 DOWN。 */
    {
        /* 探活项现在是 ICMP（探平台地址）：DOWN 那一侧仍是"链路状态直接播进来"
         * （真接口 UP、dummy 刚建是 DOWN）；UP 那一侧要先有回包，开局必然是
         * UNKNOWN —— 所以真网卡模式这里只能断"没被判死"，不能断 UP。 */
        iv_probe_state_t want = g_realnic ? IV_PROBE_UNKNOWN : IV_PROBE_DOWN;

        chk(iv_probe_state(&g_agent.pb, g_agent.wan_probe_idx[IV_WAN_WIRELESS]) == want,
            "k01 wireless probe seeded from current kernel state");
        chk(iv_probe_state(&g_agent.pb, g_agent.wan_probe_idx[IV_WAN_WIRED]) == want,
            "k01 wired probe seeded from current kernel state");
    }

    /* ---- k02 摸底：真内核 LINK_UP → probe UP → netmgr 标记两路可用 ---- */
    if (g_agent.wan_probe_idx[IV_WAN_WIRELESS] < 0 ||
        g_agent.wan_probe_idx[IV_WAN_WIRED] < 0) {
        printf("SKIP: probe items missing, cannot continue\n");
        return 0;
    }
    if (g_realnic) {
        /* 真接口本来就 UP ⇒ 探活项开局不是 DOWN（k01 已钉死），但仍要真的发出
         * ICMP 拿到回包才算 UP。 */
        chk(wait_probe_up(IV_WAN_WIRELESS, master, 10), "k02 wireless UP (platform addr reachable)");
        chk(wait_probe_up(IV_WAN_WIRED, master, 10), "k02 wired UP (platform addr reachable)");
    } else {
        /* netns：dummy 建出来就是 DOWN（摸底应播成 DOWN），再靠**真内核 LINK_UP
         * 事件**把探活项从 LINK_DOWN 放出来，然后由 ICMP 回包判 UP —— 前面这半段
         * 验的是 netlink → probe 的事件转接，后面那半段验的是"探的是平台地址"。 */
        chk(wait_link(IV_WAN_WIRELESS, 0), "k02 wireless starts DOWN (seeded at open)");
        chk(wait_link(IV_WAN_WIRED, 0), "k02 wired starts DOWN (seeded at open)");
        chk(ip_set_state(WAN_WL, 1) == 0, "k02 ip link set usb0 up");
        chk(ip_set_state(WAN_WD, 1) == 0, "k02 ip link set eth0 up");
        chk(wait_probe_up(IV_WAN_WIRELESS, master, 10),
            "k02 wireless probe UP (LINK_UP then ICMP reply)");
        chk(wait_probe_up(IV_WAN_WIRED, master, 10),
            "k02 wired probe UP (LINK_UP then ICMP reply)");
    }
    pump_agent(master, 2);
    chk(iv_netmgr_wan_is_up(&g_agent.nm, IV_WAN_WIRELESS) == 1, "k02 netmgr: wireless usable");
    chk(iv_netmgr_wan_is_up(&g_agent.nm, IV_WAN_WIRED) == 1, "k02 netmgr: wired usable");
    chk(iv_agent_netmgr_state(&g_agent) == IV_NETMGR_WIRELESS_UP, "k02 still wireless-first");
    /* k02b M4-S4.4：两路都可用 ⇒ MN 两路都是 1（上报字段的生产者已在装配层点火） */
    chk(iv_agent_mn(&g_agent, IV_WAN_WIRELESS) == 1u, "k02b wireless MN == 1 (normal)");
    chk(iv_agent_mn(&g_agent, IV_WAN_WIRED) == 1u, "k02b wired MN == 1 (normal)");

    /* ---- k02c M4-S4.5：0xC1 箱门开启 → agent 门开钩子（**端到端**） ----
     * 为什么必须在这里测：`iv_agent` 把 `on_door_open` 挂进 report cfg 的那两行
     * （`rc.on_door` / `rc.on_door_arg`）**没有任何单测能覆盖** —— 单测是直接调
     * `iv_status`、或直接给 `iv_report_cfg_t` 填钩子；只有真的经 agent 开局、
     * 真的从 pty 喂一帧 0xC1 上来，才能证明"钩子挂上了"（**回调没挂上测不出**）。
     * ⚠ **故意放在 k02 之后**：多跑两拍 iv_agent_step 会驱动 netmgr；若放在联路拔起
     * 之前，netmgr 会提前计数 ⇒ k04/k06 的切换与回切时序假红（实测）。 */
    {
        uint8_t up[IV_FRAME_MAX];
        int     un;

        chk(iv_agent_door_events(&g_agent) == 0u, "k02c no door event before 0xC1");
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_DOOR, "\x01", 1u,
                            up, sizeof up);
        chk(un > 0, "k02c upstream 0xC1 frame built");
        chk(write(master, up, (size_t)un) == un, "k02c feed 0xC1 door-open");
        /* 推两拍但**不推进 g_now**：k02 之后的 k03/k04/k06 依赖精确的时间模型
         * （netmgr 迟滞计数、hold_s 回切），多走 400 ms 会把它们带歪；而纯喂一帧不需要时间前进 ——
         * iv_report_step() 每次都会把串口抽干。（**实测结论**：用 pump_agent 推两拍会让 k04 早一次
         * 完成切换事务、k06 提前回切 ⇒ 三条断言假红。） */
        (void)iv_agent_step(&g_agent, g_now);
        (void)iv_agent_step(&g_agent, g_now);
        chk(iv_agent_door_events(&g_agent) == 1u, "k02c agent door hook fired once");
        chk(g_agent.rp.st.valid.DS == 1u && g_agent.rp.st.data.DS == 2,
            "k02c mirror DS=2 after 0xC1");
        chk(iv_agent_door_events(NULL) == 0u, "k02c NULL handle -> 0");
        /* ---- k02c-2 M6-S6.2：门开钩子接到 iv_media 桥接（**端到端**） ----
         * "on_door_open 里换成入队抓拍"同样没有任何单测能覆盖 —— 只有真 agent
         * 开局＋真 pty 喂 0xC1 才能证明接线成立。这里只断言"入队这一步发生了"
         * （snap_req +1，同步计数）；对端 media 进程不在 ⇒ worker 会计 snap_fail，
         * 属预期，不参与断言。 */
        {
            iv_media_stats_t mst;

            iv_media_stats(iv_agent_media(&g_agent), &mst);
            chk(mst.snap_req == 1u, "k02c door hook enqueued one capture (M6 wired)");
        }
    }


    /* ---- k03 基线上报：平台建连 + 一件上报走通 ---- */
    for (j = 0; j < 20 && cs < 0; j++) {
        g_now += STEP_MS;
        (void)iv_agent_step(&g_agent, g_now);
        cs = accept_ms(ls, 0);
    }
    if (cs < 0)
        cs = accept_ms(ls, 2000);
    chk(cs >= 0, "k03 platform accepted the first TCP");
    chk(wait_transport_up(20), "k03 transport UP");
    if (cs >= 0)
        (void)fcntl(cs, F_SETFL, O_NONBLOCK);

    memset(&env, 0, sizeof env);
    env.csq = 9;
    iv_report_set_env(&g_agent.rp, &env);
    chk(iv_report_trigger(&g_agent.rp, g_now) == IV_OK, "k03 trigger accepted");
    chk(iv_report_queue_count(&g_agent.rp) == 1u, "k03 item queued");
    chk(wait_queue_empty(60), "k03 item acked out of queue");
    if (cs >= 0) {
        char        rx[8192];
        size_t      rn;
        size_t      used;
        const char *seg = NULL;
        size_t      sl  = 0u;

        rn = drain(cs, rx, sizeof rx, 1500);
        if (rn < sizeof rx)
            rx[rn] = '\0';
        used = next_report_frame(rx, rn, 0u, &seg, &sl);
        chk(rn > 0u, "k03 platform received bytes");
        chk(used > 0u, "k03 exactly one well-formed ## frame");
        chk(rn == used, "k03 no trailing junk on the wire");
        if (used > 0u)
            chk(seg_csq(seg, sl) == 9, "k03 frame carries the injected CSQ=9");
    }

    /* ---- k04 拔线：真内核 LINK_DOWN → 切有线（事务 = 真改路由 + 真重连） ---- */
    cap_mark = g_capn; /* M4-S4.4 的 0xD1 只可能在这之后出现 */
    chk(ip_set_state(WAN_WL, 0) == 0, "k04 ip link set usb0 down");
    chk(wait_link(IV_WAN_WIRELESS, 0), "k04 wireless probe DOWN via real kernel link event");
    pump_agent(master, 4);
    chk(iv_netmgr_switches(&g_agent.nm) == 1u,
        "k04 switch transaction fired (route change AND reconnect both returned OK)");
    chk(iv_agent_active_wan(&g_agent) == IV_WAN_WIRED, "k04 active WAN -> wired");
    chk(strcmp(iv_agent_report(&g_agent)->srv_host, g_srv_wd) == 0,
        "k04 platform addr switched to the wired one (按活动出口选地址)");
    chk(strcmp(iv_agent_report(&g_agent)->tr.host, g_srv_wd) == 0,
        "k04 transport really re-dials the wired addr");
    chk(iv_agent_netmgr_state(&g_agent) == IV_NETMGR_WIRED_UP, "k04 state WIRED_UP");
    chk(iv_agent_active_ifindex(&g_agent) == g_agent.wan_ifindex[IV_WAN_WIRED],
        "k04 active ifindex == resolved eth0 ifindex");
    /* S9 延伸：此刻 switches==1 ⇒ net_event 恰好一行（agent_reconnect 接线；
     * 行内容无只读出口，行数即接线证据；k06 回切后会变 2，故在此处断）。 */
    {
        int64_t n_ev = -1;

        chk(iv_store_rows(tstore, "net_event", &n_ev) == IV_OK && n_ev == 1,
            "k04 net_event has exactly 1 row (exit switch landed in control.db)");
        if (n_ev != 1)
            printf("k04 DEBUG net_event rows=%lld\n", (long long)n_ev);
    }

    /* ---- k04b M4-S4.4：主网「正常 → 故障」沿 → 继电器 1 断电重启 ----
     * 装配层的 MN 生产者用 netmgr 的**迟滞结论**（`iv_netmgr_wan_is_up()`），
     * 并在 1→2 沿上调 `iv_recovery_on_master_change()`；动作经 iv_report_send_cmd
     * 落到本层自持的 iv_link ⇒ **pty 上应当真的看见一条 0xD1**。 */
    cap_suck(master); /* 把最后一步写进 pty 的字节也捞回来（pump 是先抽后推） */
    chk(iv_agent_mn(&g_agent, IV_WAN_WIRELESS) == 2u,
        "k04b wireless MN -> 2 (hysteresis verdict reached the report env)");
    chk(iv_agent_mn(&g_agent, IV_WAN_WIRED) == 1u,
        "k04b wired MN stays 1 (only the failed path goes to 2)");
    chk(cap_has_frame_from(cap_mark, IV_FRAME_CMD_POWER),
        "k04b master-net fault edge -> 0xD1 on the serial (relay 1 power cycle)");
    chk(iv_recovery_port_credit(&g_agent.rec, IV_RECOVERY_PORT_MASTER) == 1,
        "k04b relay 1 consumed exactly one action (2 -> 1)");
    chk(g_agent.rec.disabled == 0u, "k04b recovery was enabled, not denied");

    chk(wait_transport_up(30), "k04 transport back UP after reconnect");
    {
        /* 重连证据（平台侧）：旧连接被拆、新连接建立。
         * 注意**不能**拿 `iv_transport_connects` 的累计值判"是否重连" ——
         * `iv_transport_open()` 会复位会话统计（connects 归零），重连后它只会
         * 是 1，用它作判据会假红。 */
        int cs2 = accept_ms(ls, 2000);

        chk(cs2 >= 0, "k04 platform accepted a NEW TCP after the WAN switch (reconnect fired)");
        if (cs >= 0) {
            char    b[16];
            ssize_t r = read(cs, b, sizeof b); /* 非阻塞；旧连接应已 EOF/RST */
            chk(r <= 0, "k04 the old TCP was torn down by the reconnect");
            (void)close(cs);
        }
        cs = cs2;
        if (cs >= 0)
            (void)fcntl(cs, F_SETFL, O_NONBLOCK);
    }

    /* ---- k05 平台断：链路之外再断平台 → 上报只能入队积压（不丢） ---- */
    if (cs >= 0) {
        (void)close(cs);
        cs = -1;
    }
    if (ls >= 0) {
        (void)close(ls);
        ls = -1;
    }
    for (j = 0; j < 80 && iv_transport_state(&g_agent.rp.tr) == IV_TRANSPORT_UP; j++) {
        g_now += STEP_MS;
        (void)iv_agent_step(&g_agent, g_now);
    }
    chk(iv_transport_state(&g_agent.rp.tr) != IV_TRANSPORT_UP,
        "k05 transport left UP after platform closed");
    for (i = 11; i <= 12; i++) {
        memset(&env, 0, sizeof env);
        env.csq = i;
        iv_report_set_env(&g_agent.rp, &env);
        chk(iv_report_trigger(&g_agent.rp, g_now) == IV_OK, "k05 trigger while platform down");
        g_now += STEP_MS;
        (void)iv_agent_step(&g_agent, g_now);
    }
    chk(iv_report_queue_count(&g_agent.rp) == 2u, "k05 2 items queued while platform down");
    chk(iv_report_queue_full(&g_agent.rp) == 0u, "k05 nothing was dropped");

    /* ---- k06 无线恢复 + 平台恢复：跨过 hold_s 回切，欠包按序补传 ---- */
    ls = listen_port(port, &port2);
    chk(ls >= 0 && port2 == port, "k06 platform re-listened on the same port");
    chk(ip_set_state(WAN_WL, 1) == 0, "k06 ip link set usb0 up");
    {
        int back = wait_probe_up(IV_WAN_WIRELESS, master, 10);

        chk(back, "k06 wireless probe back UP");
        if (g_realnic) {
            /* 真 usb0 掉/起之后地址要重取（板端没有自动重跑的 DHCP 客户端）—— 只补
             * 地址，不动路由（默认出口归状态机管）。失败不单独断言：地址不是本用例的
             * 目标，真要缺了会在下面的传输层 / 路由断言上炸出来。 */
            char *av_dhcp[] = { (char *)"udhcpc", (char *)"-i", (char *)WAN_WL,
                                (char *)"-n", (char *)"-q", (char *)"-t",
                                (char *)"6", (char *)"-T", (char *)"2", NULL };

            (void)run_ip(av_dhcp);
            /* DHCP 可能把地址换掉：平台地址必须跟着改（agent 在切回时从 a->cfg
             * 取地址），否则 k06 会去连一个已经不存在的地址。 */
            if (iface_ipv4(WAN_WL, g_srv_wl, sizeof g_srv_wl))
                g_agent.cfg.srv_host[IV_WAN_WIRELESS] = g_srv_wl;
            if (!back)
                printf("NOTE: real %s did not return to carrier after link up — the "
                       "EC200G PDP may need AT+QNETDEVCTL=1,1,1 (S62usb4g does that; "
                       "it was stopped for this run)\n", WAN_WL);
        }
    }
    g_now += STEP_MS;
    (void)iv_agent_step(&g_agent, g_now);
    chk(iv_netmgr_wan_is_up(&g_agent.nm, IV_WAN_WIRELESS) == 1, "k06 wireless marked usable");
    chk(iv_netmgr_switches(&g_agent.nm) == 1u, "k06 no switch-back before hold_s elapses");

    g_now += (uint64_t)cfg.nm_hold_s * 1000u + 500u; /* 跨过 hold_s */
    pump_agent(master, 3);
    chk(iv_netmgr_switches(&g_agent.nm) == 2u, "k06 switched back to wireless after hold_s");
    chk(iv_agent_active_wan(&g_agent) == IV_WAN_WIRELESS, "k06 active WAN -> wireless");
    chk(iv_agent_mn(&g_agent, IV_WAN_WIRELESS) == 1u,
        "k06b wireless MN back to 1 after recovery (edge detector re-armed)");
    chk(strcmp(iv_agent_report(&g_agent)->srv_host, g_srv_wl) == 0,
        "k06 platform addr back to the wireless one");
    chk(iv_agent_netmgr_state(&g_agent) == IV_NETMGR_WIRELESS_UP, "k06 state WIRELESS_UP");

    chk(wait_transport_up(60), "k06 transport UP on the restored platform");
    chk(strcmp(iv_agent_report(&g_agent)->tr.host, g_srv_wl) == 0,
        "k06 transport re-dials the wireless addr");
    cs = accept_ms(ls, 2000);
    chk(cs >= 0, "k06 platform accepted the reconnected TCP");
    if (cs >= 0)
        (void)fcntl(cs, F_SETFL, O_NONBLOCK);
    chk(wait_queue_empty(150), "k06 backlog drained (补传)");
    if (cs >= 0) {
        char   rx[8192];
        size_t rn;
        size_t off   = 0u;
        int    seen[4];
        int    nseen = 0;

        rn = drain(cs, rx, sizeof rx, 1500);
        if (rn < sizeof rx)
            rx[rn] = '\0';
        while (nseen < 4) {
            const char *seg = NULL;
            size_t      sl  = 0u;
            size_t      u   = next_report_frame(rx, rn, off, &seg, &sl);

            if (u == 0u)
                break;
            seen[nseen++] = seg_csq(seg, sl);
            off += u;
        }
        chk(nseen == 2, "k06 exactly 2 frames replayed (no dup, no loss)");
        chk(nseen == 2 && seen[0] == 11 && seen[1] == 12,
            "k06 replay order == queue order (11 then 12)");
        chk(off == rn, "k06 no trailing bytes after the replayed frames");
    }

    /* ---- k07 收口 ---- */
    iv_agent_close(&g_agent);
    chk(iv_agent_is_open(&g_agent) == 0, "k07 agent reports closed");
    chk(iv_report_is_open(&g_agent.rp) == 0, "k07 report chain closed");
    chk(iv_netlink_fd(&g_agent.nl) == -1, "k07 netlink socket closed");

    /* 真网卡模式：switch_route 的两条默认路由（prio 100）是**真建在板子上的**，且
     * metric 比板子原有的 204/208 更小 ⇒ 会盖住默认出口。收尾删掉，不留痕（规则 6
     * 口径；netns 模式不用管，ns 随进程回收）。 */
    if (g_realnic) {
        char *av_w[] = { (char *)"ip", (char *)"route", (char *)"del",
                         (char *)"default", (char *)"dev", (char *)WAN_WL,
                         (char *)"metric", (char *)"100", NULL };
        char *av_d[] = { (char *)"ip", (char *)"route", (char *)"del",
                         (char *)"default", (char *)"dev", (char *)WAN_WD,
                         (char *)"metric", (char *)"100", NULL };

        (void)run_ip(av_w);
        (void)run_ip(av_d);
    }

    /* 先收掉用例自建的 fd，再比对水位，否则会把自己的句柄算成"泄漏" */
    if (cs >= 0)
        (void)close(cs);
    if (ls >= 0)
        (void)close(ls);
    if (master >= 0)
        (void)close(master);
    rm_tmpdir(qdir);

    /* store 的 db fd 也是用例自建 fd —— 必须在 k07 的 fd 对账之前收掉 */
    iv_store_close(tstore);
    iv_db_close(tdb);
    rm_tmpdir(dbdir);

    /* 只断言"不增"而非"严格相等"：库内可能有惰性缓存；增长才说明装配层漏关。 */
    {
        int after = count_open_fds();

        if (after > fd_base)
            dump_fds(); /* 泄漏时把每个 fd 的指向打出来，省得回头猜 */
        chk(after <= fd_base, "k07 open/close did not leak fds");
    }

    if (g_fail == 0)
        printf("test_agent_switch passed "
               "(%s: link up/down -> probe, door-event, wan switch, real route "
               "change, tcp reconnect, queue backlog, hold_s switch-back, ordered replay, "
               "MN fault edge -> relay 0xD1; exit switch landed in control.db)\n",
               g_realnic ? "real-NIC" : "netns");
    else
        printf("test_agent_switch FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char *argv[])
{
    (void)argc;

    /* 真网卡模式（板端）：不建 ns、不造 dummy，直接在真 usb0/eth0 上跑。
     * 与 4G 看护互斥（看护会把 usb0 重新拨起来）—— 检测到就 SKIP。 */
    if (getenv(REALNIC_ENV) != NULL) {
        g_realnic = 1;
        if (usb4g_supervisor_running()) {
            printf("SKIP: 4G supervisor (S62usb4g) is running; stop it before the "
                   "real-NIC switch test to avoid interference\n");
            return 0;
        }
        return scenario();
    }

    /* 未进私有 netns：先确认 unshare 可用，再把用例整体重启进去 */
    if (getenv(NS_ENV) == NULL) {
        if (!netns_available()) {
            printf("SKIP: private netns unavailable (unshare -rmn), "
                   "cannot exercise real link switching\n");
            return 0;
        }
        return reexec_in_netns(argv[0]);
    }
    return scenario();
}
