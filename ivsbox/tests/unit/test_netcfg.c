/*
 * iv_netcfg 单测（libivmodules，平台 0xF4 的落地层）
 *
 * ============================ 为什么这块必须单测 ============================
 * 这条命令填错的后果是**设备直接失联、只能现场串口救**。12 字节载荷里塞了三个
 * 大端地址，掩码写错一位就是"整段网络不可达"。所以解析与校验抽成纯函数放这里
 * 钉死，不依赖真实网卡、不要权限，本机 / VM / 板端结果一致。
 *
 * ============================ 测法 ============================
 * 纯函数：直接喂字节，断言返回值与解析结果（主机序数值比较，不比字符串）。
 * `iv_netcfg_apply()` 会真改网口 ⇒ 只在 `IVSBOX_NETCFG_DRY=1` 下测它"不改网络
 * 且回 OK"这一半；"真改"只能在板端做一次（见 docs/修改记录.md）。
 *
 * ============================ 反向证伪点（每条对应一个真机制） ============================
 *   c05 网络地址/广播地址必须被拒 —— 去掉那条判定，`192.168.2.0`、`192.168.2.255`
 *       就会通过，c05 必红；
 *   c04 掩码必须**连续** —— 用 `255.0.255.0` 探，"只要非 0 就收"的实现会放过去，必红；
 *   c07 多余字节要**按前 12 解析**而不是整体拒绝 —— 改成"必须恰好 12"后 c07 必红；
 *   c12 `0xF8` 检测目标允许填 `0.0.0.0`（＝该路不检测），而 `0xF4` 的本机 IP
 *       不允许 —— 把 `host_ip_ok()` 的 `allow_zero` 参数写反，c12/c03 会同时红；
 *   c14 同值判定要**比到前缀长度**，只比 IP 的话「网页上只改掩码」会被判成
 *       "已在位、不动链路" —— 把掩码比较删掉，c14b 必红。
 *
 * 临时产物：无（全内存、无文件、无真实网络）。AGENTS.md 规则 6。
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_netcfg.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 本用例会走 `iv_netcfg_apply()` 的 DRY 分支，那里有 IV_LOG_*。
 * 不拦掉的话日志模块会去按 `/mnt/UDISK/log/...` 开文件（板端路径），
 * 在 VM/宿主机上必然失败并往 stderr 吐一行噪音，掩盖真正的 FAIL。 */
static void noop_sink(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

/* 板端实测抓到的那一帧（2026-10-26 11:08:46）：
 * `f0f0110a000b2cc1 f4 ffffffffffffffff 0c c0a80279 ffffff00 c0a80201 a3 ff...`
 * 只取数据段 12 字节，帧头/长度/CRC 不属于本模块。 */
static const uint8_t k_real[IV_NETCFG_PAYLOAD_LEN] = {
    0xc0, 0xa8, 0x02, 0x79, /* 192.168.2.121 */
    0xff, 0xff, 0xff, 0x00, /* 255.255.255.0 */
    0xc0, 0xa8, 0x02, 0x01  /* 192.168.2.1   */
};

/* 拼一个 12 字节载荷：a.b.c.d / prefix / gw */
static void mk(uint8_t *buf, const char *ip, const char *mask, const char *gw)
{
    struct in_addr a;

    (void)inet_pton(AF_INET, ip, &a);
    (void)memcpy(buf, &a, 4u);
    (void)inet_pton(AF_INET, mask, &a);
    (void)memcpy(buf + 4, &a, 4u);
    (void)inet_pton(AF_INET, gw, &a);
    (void)memcpy(buf + 8, &a, 4u);
}

/* 点分地址 → 网络序（就是 `sin_addr.s_addr` 里那个值，别自己移位拼） */
static uint32_t a2n(const char *s)
{
    struct in_addr a;

    a.s_addr = 0;
    (void)inet_pton(AF_INET, s, &a);
    return a.s_addr;
}

int main(void)
{
    iv_netcfg_t c;
    uint8_t     b[IV_NETCFG_PAYLOAD_LEN + 4u];

    iv_log_set_sink(noop_sink, NULL);

    /* ---------------- c01 板端实测载荷（真实黄金样本） ---------------- */
    (void)memset(&c, 0, sizeof c);
    chk(iv_netcfg_parse(k_real, sizeof k_real, &c) == IV_OK,
        "c01 real 0xF4 payload parses");
    chk(ntohl(c.ip) == 0xC0A80279u, "c01 ip  = 192.168.2.121");
    chk(ntohl(c.mask) == 0xFFFFFF00u, "c01 mask= 255.255.255.0");
    chk(ntohl(c.gw) == 0xC0A80201u, "c01 gw  = 192.168.2.1");
    chk(iv_netcfg_prefix(c.mask) == 24, "c01 mask maps to prefix /24");

    /* ---------------- c02 长度与空指针 ---------------- */
    (void)memset(&c, 0, sizeof c);
    chk(iv_netcfg_parse(NULL, 12u, &c) == IV_EINVAL, "c02 NULL data => EINVAL");
    chk(iv_netcfg_parse(k_real, 12u, NULL) == IV_EINVAL, "c02 NULL out => EINVAL");
    chk(iv_netcfg_parse(k_real, 11u, &c) == IV_EINVAL, "c02 11 bytes => EINVAL");
    chk(iv_netcfg_parse(k_real, 0u, &c) == IV_EINVAL, "c02 0 bytes => EINVAL");

    /* ---------------- c03 IP 本身非法 ---------------- */
    {
        static const char *const bad[] = {
            "0.0.0.0", "127.0.0.1", "224.0.0.1", "255.255.255.255"
        };
        size_t i;

        for (i = 0u; i < sizeof bad / sizeof bad[0]; i++) {
            char what[96];

            mk(b, bad[i], "255.255.255.0", "192.168.2.1");
            (void)snprintf(what, sizeof what, "c03 ip %s rejected", bad[i]);
            chk(iv_netcfg_parse(b, 12u, &c) == IV_EINVAL, what);
        }
    }

    /* ---------------- c04 掩码：非连续 / 全 0 ---------------- */
    mk(b, "192.168.2.121", "255.0.255.0", "192.168.2.1");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_ERANGE,
        "c04 non-contiguous mask 255.0.255.0 => ERANGE");
    mk(b, "192.168.2.121", "0.0.0.0", "192.168.2.1");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_ERANGE, "c04 mask 0 => ERANGE");
    mk(b, "192.168.2.121", "255.255.0.255", "192.168.2.1");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_ERANGE,
        "c04 non-contiguous mask 255.255.0.255 => ERANGE");
    /* `254.0.0.0` 看着像"少一位"，其实是合法的 `/7` —— 别把它误判成非法。
     * 这条是给 prefix 判据本身上的保险：初版用 `m & (m+1) == 0` 判连续，
     * 连 `/24` 都会判成非法，这条与 c08 的 /24 一起会立刻露出来。 */
    mk(b, "192.168.2.121", "254.0.0.0", "192.168.2.1");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_OK && iv_netcfg_prefix(c.mask) == 7,
        "c04 254.0.0.0 is a valid /7 mask, not an error");

    /* ---------------- c05 网络地址 / 广播地址 ---------------- */
    mk(b, "192.168.2.0", "255.255.255.0", "192.168.2.1");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_EINVAL,
        "c05 network address 192.168.2.0/24 rejected");
    mk(b, "192.168.2.255", "255.255.255.0", "192.168.2.1");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_EINVAL,
        "c05 broadcast address 192.168.2.255/24 rejected");
    /* /31 与 /32 没有"网络地址/广播地址"的概念（RFC 3021），必须放行 */
    mk(b, "10.0.0.0", "255.255.255.254", "0.0.0.0");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_OK, "c05 /31 network address allowed");
    mk(b, "10.0.0.7", "255.255.255.255", "0.0.0.0");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_OK, "c05 /32 single host allowed");

    /* ---------------- c06 网关 ---------------- */
    mk(b, "192.168.2.121", "255.255.255.0", "0.0.0.0");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_OK && ntohl(c.gw) == 0u,
        "c06 gw 0.0.0.0 means 'no default route' and is accepted");
    mk(b, "192.168.2.121", "255.255.255.0", "127.0.0.1");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_EINVAL, "c06 loopback gw rejected");
    mk(b, "192.168.2.121", "255.255.255.0", "224.0.0.9");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_EINVAL, "c06 multicast gw rejected");
    /* 网关与 IP 不同网段是合法拓扑（静态路由过去），必须放行 */
    mk(b, "192.168.2.121", "255.255.255.0", "10.9.9.1");
    chk(iv_netcfg_parse(b, 12u, &c) == IV_OK,
        "c06 off-subnet gw is allowed (static-route topologies exist)");

    /* ---------------- c07 多于 12 字节：只取前 12，不是整体拒绝 ---------------- */
    (void)memcpy(b, k_real, 12u);
    b[12] = 0xA3u; /* 后面跟 CRC/尾巴，不该被当成地址 */
    b[13] = 0xFFu;
    (void)memset(&c, 0, sizeof c);
    chk(iv_netcfg_parse(b, 14u, &c) == IV_OK &&
        ntohl(c.ip) == 0xC0A80279u && ntohl(c.gw) == 0xC0A80201u,
        "c07 14-byte payload still parses the first 12");

    /* ---------------- c08 掩码 → 前缀 ---------------- */
    {
        struct { const char *m; int want; } t[] = {
            { "255.0.0.0", 8 },      { "255.255.0.0", 16 },
            { "255.255.255.0", 24 }, { "255.255.255.128", 25 },
            { "255.255.255.252", 30 }, { "255.255.255.254", 31 },
            { "255.255.255.255", 32 }
        };
        size_t i;

        for (i = 0u; i < sizeof t / sizeof t[0]; i++) {
            struct in_addr a;
            char           what[96];

            (void)inet_pton(AF_INET, t[i].m, &a);
            (void)snprintf(what, sizeof what, "c08 %s => /%d", t[i].m, t[i].want);
            chk(iv_netcfg_prefix(a.s_addr) == t[i].want, what);
        }
    }

    /* ---------------- c09 前缀非法 ---------------- */
    {
        struct in_addr a;

        (void)inet_pton(AF_INET, "0.0.0.0", &a);
        chk(iv_netcfg_prefix(a.s_addr) == -1, "c09 0.0.0.0 => -1");
        (void)inet_pton(AF_INET, "255.0.255.0", &a);
        chk(iv_netcfg_prefix(a.s_addr) == -1, "c09 255.0.255.0 => -1");
        (void)inet_pton(AF_INET, "255.255.255.1", &a);
        chk(iv_netcfg_prefix(a.s_addr) == -1, "c09 255.255.255.1 => -1");
    }

    /* ---------------- c10 apply 的参数校验（不碰网络） ---------------- */
    chk(iv_netcfg_apply(NULL, NULL) == IV_EINVAL, "c10 apply(NULL) => EINVAL");
    (void)memset(&c, 0, sizeof c);
    c.ip = htonl(0xC0A80279u);
    chk(iv_netcfg_apply(&c, NULL) == IV_EINVAL,
        "c10 apply with mask 0 => EINVAL (never flush without a valid mask)");

    /* ---------------- c11 DRY：不改网络，但路径要走通并回 OK ---------------- */
    (void)setenv("IVSBOX_NETCFG_DRY", "1", 1);
    (void)memset(&c, 0, sizeof c);
    chk(iv_netcfg_parse(k_real, sizeof k_real, &c) == IV_OK, "c11 setup parse");
    chk(iv_netcfg_apply(&c, NULL) == IV_OK,
        "c11 DRY run returns OK without touching the network");
    chk(iv_netcfg_apply(&c, "eth0") == IV_OK, "c11 DRY run with explicit ifname");
    (void)unsetenv("IVSBOX_NETCFG_DRY");

    /* ---------------- c12 0xF8 主网检测 IP：真实黄金样本 ---------------- */
    {
        /* 板端实抓（2026-10-26 11:40:46）：
         * `f0f0110a000b2cc1 f8 ffffffffffffffff 08 c0a80201 c0a80202 68 ffff` */
        static const uint8_t f8[8] = {
            0xc0, 0xa8, 0x02, 0x01, /* 192.168.2.1 */
            0xc0, 0xa8, 0x02, 0x02  /* 192.168.2.2 */
        };
        uint32_t t1 = 0xdeadbeefu;
        uint32_t t2 = 0xdeadbeefu;
        uint8_t  wide[12];

        chk(iv_netcfg_parse_targets(f8, sizeof f8, &t1, &t2) == IV_OK,
            "c12 real 0xF8 payload parses");
        chk(ntohl(t1) == 0xC0A80201u, "c12 target1 = 192.168.2.1");
        chk(ntohl(t2) == 0xC0A80202u, "c12 target2 = 192.168.2.2");

        /* 两路都填 0 ＝ 界面清空 ＝ 都不检测，**必须放行**。
         * 这条是给 `allow_zero` 参数上的保险：0xF4 的本机 IP 不允许 0，
         * 同一个判定函数在这里必须允许 —— 参数写反了这条就红。 */
        (void)memset(wide, 0, sizeof wide);
        t1 = t2 = 0xdeadbeefu;
        chk(iv_netcfg_parse_targets(wide, 8u, &t1, &t2) == IV_OK &&
            t1 == 0u && t2 == 0u,
            "c12 both-zero targets accepted (means 'stop probing that side')");

        /* 只清一路也是合法的 */
        (void)memcpy(wide, f8, 4u);
        (void)memset(wide + 4, 0, 4u);
        chk(iv_netcfg_parse_targets(wide, 8u, &t1, &t2) == IV_OK &&
            ntohl(t1) == 0xC0A80201u && t2 == 0u,
            "c12 only target2 cleared is fine");
    }

    /* ---------------- c13 0xF8 的长度与地址校验 ---------------- */
    {
        uint8_t  b8[12]; /* mk() 一次写 12 字节，别用 8 字节缓冲接 */
        uint32_t t1;
        uint32_t t2;

        chk(iv_netcfg_parse_targets(NULL, 8u, &t1, &t2) == IV_EINVAL,
            "c13 NULL data => EINVAL");
        chk(iv_netcfg_parse_targets(b8, 8u, NULL, &t2) == IV_EINVAL,
            "c13 NULL t1 => EINVAL");
        mk(b8, "192.168.2.1", "192.168.2.2", "0.0.0.0"); /* 12 字节数组，取前 8 */
        chk(iv_netcfg_parse_targets(b8, 7u, &t1, &t2) == IV_EINVAL,
            "c13 7 bytes => EINVAL");

        mk(b8, "127.0.0.1", "192.168.2.2", "0.0.0.0");
        chk(iv_netcfg_parse_targets(b8, 8u, &t1, &t2) == IV_EINVAL,
            "c13 loopback target1 rejected");
        mk(b8, "192.168.2.1", "224.0.0.9", "0.0.0.0");
        chk(iv_netcfg_parse_targets(b8, 8u, &t1, &t2) == IV_EINVAL,
            "c13 multicast target2 rejected");

        /* 多于 8 字节只取前 8（与 0xF4 同口径） */
        {
            uint8_t wide[12];

            mk(wide, "192.168.2.1", "192.168.2.2", "0.0.0.0");
            wide[8] = 0xAAu;
            wide[9] = 0xBBu;
            chk(iv_netcfg_parse_targets(wide, 10u, &t1, &t2) == IV_OK &&
                ntohl(t1) == 0xC0A80201u && ntohl(t2) == 0xC0A80202u,
                "c13 10-byte payload still parses the first 8");
        }
    }

    /* ---------------- c14 同值判定必须比到**前缀长度** ---------------- */
    {
        /* 反向证伪点：把 `iv_netcfg_if_addr_equals()` 里的掩码比较删掉、只留 IP 比，
         * c14b 必红。它对应的真实缺陷是「网页上只改掩码、IP 不动」——
         * 旧实现判"已在位、不动链路"，用户改了个寂寞而界面显示已保存。
         *
         * 用 `lo`（127.0.0.1/8）当靶子：它一定存在、一定带掩码、且不需要任何权限。 */
        chk(iv_netcfg_if_addr_equals("lo", a2n("127.0.0.1"), a2n("255.0.0.0")) == 1,
            "c14a lo 127.0.0.1/8 => equal");
        chk(iv_netcfg_if_addr_equals("lo", a2n("127.0.0.1"), a2n("255.255.255.0")) == 0,
            "c14b same IP but /24 must NOT count as equal");
        chk(iv_netcfg_if_addr_equals("lo", a2n("127.0.0.2"), a2n("255.0.0.0")) == 0,
            "c14c different IP => not equal");
        chk(iv_netcfg_if_addr_equals("ivno0", a2n("127.0.0.1"), a2n("255.0.0.0")) == -1,
            "c14d unknown interface => -1");
        /* 非法掩码（不连续）没有"相等"可言 ⇒ 必须回 0，让调用方去走 apply 的校验。
         * 写反了（比如内部 `iv_netcfg_prefix()` 回 -1 时当成功）就会把非法配置
         * 悄悄当成"已在位"，永远不报错。 */
        chk(iv_netcfg_if_addr_equals("lo", a2n("127.0.0.1"), a2n("255.0.255.0")) == 0,
            "c14e non-contiguous mask => not equal (never silent-skip)");
    }

    if (g_fail == 0) {
        fprintf(stderr, "test_netcfg passed (parse/prefix/guards/targets/eq/dry-apply)\n");
        return 0;
    }
    fprintf(stderr, "test_netcfg FAILED (%d checks)\n", g_fail);
    return 1;
}
