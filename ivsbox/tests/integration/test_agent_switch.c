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
 *        `iv_probe_set_link`，probe 状态永不变化，此处必红。
 *
 * ============================ 临时产物 ============================
 * 仅 $TMPDIR 下一个临时目录（持久队列），用例结束前删除；私有 netns 随进程
 * 退出自动回收。AGENTS.md 规则 6。
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_netlink.h"
#include "ivsbox/iv_netmgr.h"
#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_transport.h"

/* 两张 WAN 的接口名（架构 §18.3 冻结：无线=usb0 / 有线=eth0） */
#define WAN_WL        "usb0"
#define WAN_WD        "eth0"
/* 「已进入私有 netns」的环境标记；未置位则自重启 */
#define NS_ENV        "IV_AGENT_TEST_NETNS"
/* 每拍注入的毫秒数（＝ iv_report 的 tick 节拍量级） */
#define STEP_MS       200u

/* iv_agent_t 约 35 KB（内含 iv_report 的队列/传输缓冲）—— 必须放静态区，勿放栈 */
static iv_agent_t g_agent;
static int        g_fail;
static uint64_t   g_now;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
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
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

    if (off + 8u > len)
        return 0u;
    if (buf[off] != '#' || buf[off + 1u] != '#')
        return 0u;
    sl = (size_t)(buf[off + 2u] - '0') * 1000u +
         (size_t)(buf[off + 3u] - '0') * 100u +
         (size_t)(buf[off + 4u] - '0') * 10u + (size_t)(buf[off + 5u] - '0');
    if (off + 8u + sl > len)
        return 0u;
    *seg     = buf + off + 6u;
    *seg_len = sl;
    return 8u + sl;
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

/* 推进 n 拍；每拍把 pty master 上的下行字节抽掉（防 pty 缓冲写满） */
static void pump_agent(int master, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        char scratch[512];

        (void)drain(master, scratch, sizeof scratch, 0);
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
    int             fd_before;
    int             fd_base;
    int             i;
    int             j;

    /* 用例自建任何 fd（pty / listener）**之前**取水位：收尾把自建 fd 全关掉后
     * 应回到这里，k07 用它判装配层有没有漏关（取晚了会把测试自己的句柄算成泄漏）。 */
    fd_base = count_open_fds();

    /* ---- netns 内先造网卡：接口必须先存在，iv_agent 才解析得到 ifindex ---- */
    if (ip_add_dummy(WAN_WL) != 0 || ip_add_dummy(WAN_WD) != 0) {
        printf("SKIP: cannot create dummy interfaces (iproute2 missing?)\n");
        return 0;
    }
    if (ip_set_state("lo", 1) != 0) {
        printf("SKIP: cannot bring lo up\n");
        return 0;
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
    cfg.server_host  = "127.0.0.1";
    cfg.server_port  = port;
    cfg.devid        = 0x101u;
    cfg.mod          = "AGENT-MOD";
    cfg.sv           = "AGENT-SV";
    cfg.report_ms    = 3600u * 1000u; /* 关周期上报：用 trigger 精确催报 */
    cfg.heartbeat_ms = 3600u * 1000u; /* 关心跳：会污染收包断言 */
    cfg.poll_ms      = 3600u * 1000u; /* 关 0xE1 轮询：镜像不参与本用例 */
    (void)snprintf(cfg.wan_name[IV_WAN_WIRELESS], IV_AGENT_WAN_NAME_MAX, "%s", WAN_WL);
    (void)snprintf(cfg.wan_name[IV_WAN_WIRED], IV_AGENT_WAN_NAME_MAX, "%s", WAN_WD);
    cfg.probe_target_be[0] = 0u; /* 无目标 → 建 LINK 项（被动吃 carrier 事件） */
    cfg.probe_target_be[1] = 0u;
    cfg.mode       = IV_NETMGR_MODE_AUTO;
    cfg.nm_fail_n  = 1u; /* 迟滞压到 1：一次结论即判，用例不必灌多次 */
    cfg.nm_ok_n    = 1u;
    cfg.nm_hold_s  = 1u; /* 回切稳定期 1 s（时间轴由用例注入，瞬间可跨） */
    cfg.nm_stable_s = 0u; /* 立即恢复媒体；本用例不驱动媒体步 */

    /* ---- k01 装配起步 ---- */
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
    /* 装配把接口名解析成了 ifindex；解析不到整条链就不成立，必须钉死 */
    chk(g_agent.wan_ifindex[IV_WAN_WIRELESS] > 0, "k01 wireless ifindex resolved");
    chk(g_agent.wan_ifindex[IV_WAN_WIRED] > 0, "k01 wired ifindex resolved");
    chk(g_agent.wan_probe_idx[IV_WAN_WIRELESS] >= 0, "k01 wireless probe item created");
    chk(g_agent.wan_probe_idx[IV_WAN_WIRED] >= 0, "k01 wired probe item created");
    chk(iv_report_serial_fd(&g_agent.rp) >= 0, "k01 serial opened (pty)");

    /* ---- k02 摸底：真内核 LINK_UP → probe UP → netmgr 标记两路可用 ---- */
    if (g_agent.wan_probe_idx[IV_WAN_WIRELESS] < 0 ||
        g_agent.wan_probe_idx[IV_WAN_WIRED] < 0) {
        printf("SKIP: probe items missing, cannot continue\n");
        return 0;
    }
    chk(ip_set_state(WAN_WL, 1) == 0, "k02 ip link set usb0 up");
    chk(ip_set_state(WAN_WD, 1) == 0, "k02 ip link set eth0 up");
    chk(wait_link(IV_WAN_WIRELESS, 1), "k02 wireless probe UP via real kernel link event");
    chk(wait_link(IV_WAN_WIRED, 1), "k02 wired probe UP via real kernel link event");
    pump_agent(master, 2);
    chk(iv_netmgr_wan_is_up(&g_agent.nm, IV_WAN_WIRELESS) == 1, "k02 netmgr: wireless usable");
    chk(iv_netmgr_wan_is_up(&g_agent.nm, IV_WAN_WIRED) == 1, "k02 netmgr: wired usable");
    chk(iv_agent_netmgr_state(&g_agent) == IV_NETMGR_WIRELESS_UP, "k02 still wireless-first");

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
    chk(ip_set_state(WAN_WL, 0) == 0, "k04 ip link set usb0 down");
    chk(wait_link(IV_WAN_WIRELESS, 0), "k04 wireless probe DOWN via real kernel link event");
    pump_agent(master, 4);
    chk(iv_netmgr_switches(&g_agent.nm) == 1u,
        "k04 switch transaction fired (route change AND reconnect both returned OK)");
    chk(iv_agent_active_wan(&g_agent) == IV_WAN_WIRED, "k04 active WAN -> wired");
    chk(iv_agent_netmgr_state(&g_agent) == IV_NETMGR_WIRED_UP, "k04 state WIRED_UP");
    chk(iv_agent_active_ifindex(&g_agent) == g_agent.wan_ifindex[IV_WAN_WIRED],
        "k04 active ifindex == resolved eth0 ifindex");

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
    chk(wait_link(IV_WAN_WIRELESS, 1), "k06 wireless probe back UP");
    g_now += STEP_MS;
    (void)iv_agent_step(&g_agent, g_now);
    chk(iv_netmgr_wan_is_up(&g_agent.nm, IV_WAN_WIRELESS) == 1, "k06 wireless marked usable");
    chk(iv_netmgr_switches(&g_agent.nm) == 1u, "k06 no switch-back before hold_s elapses");

    g_now += (uint64_t)cfg.nm_hold_s * 1000u + 500u; /* 跨过 hold_s */
    pump_agent(master, 3);
    chk(iv_netmgr_switches(&g_agent.nm) == 2u, "k06 switched back to wireless after hold_s");
    chk(iv_agent_active_wan(&g_agent) == IV_WAN_WIRELESS, "k06 active WAN -> wireless");
    chk(iv_agent_netmgr_state(&g_agent) == IV_NETMGR_WIRELESS_UP, "k06 state WIRELESS_UP");

    chk(wait_transport_up(60), "k06 transport UP on the restored platform");
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

    /* 先收掉用例自建的 fd，再比对水位，否则会把自己的句柄算成"泄漏" */
    if (cs >= 0)
        (void)close(cs);
    if (ls >= 0)
        (void)close(ls);
    if (master >= 0)
        (void)close(master);
    rm_tmpdir(qdir);

    /* 只断言"不增"而非"严格相等"：库内可能有惰性缓存；增长才说明装配层漏关。 */
    chk(count_open_fds() <= fd_base, "k07 open/close did not leak fds");

    if (g_fail == 0)
        printf("test_agent_switch passed "
               "(netns: link up/down -> probe, wan switch, real route change, "
               "tcp reconnect, queue backlog, hold_s switch-back, ordered replay)\n");
    else
        printf("test_agent_switch FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char *argv[])
{
    (void)argc;

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
