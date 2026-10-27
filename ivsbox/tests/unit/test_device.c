/*
 * 摄像机信息探测（多品牌发现）单测（功能开发计划 M4-S4.2）
 *
 * 覆盖：SADP 校验和（偶/奇长度黄金值）、SADP Inquiry 字节布局、SADP 应答解析、
 *       海康 UDP 探测文本、大华两套探测帧黄金字节、大华应答解析、
 *       ONVIF Probe 文本、ONVIF/宇视/海康 关键字应答解析、发现状态机顺序与轮数、
 *       档案去重/更新/满、阶段推进到 DONE。
 * 零硬件：全部 socket 走注入点；不依赖真实网卡与摄像机。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_device.h"

static int g_fail;

static void chk(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 注入点：假 socket + 发送记录 + 收包队列
 * ------------------------------------------------------------------------- */
static int g_next_fd = 100;
static uint8_t g_sent[16][IV_DEV_PROBE_MAX];
static size_t g_sent_len[16];
static int g_sent_n;
static int g_last_sent_fd = -1;

static uint8_t g_queue[8][IV_DEV_RX_MAX];
static size_t g_queue_len[8];
static int g_queue_n;
static int g_queue_i; /* FIFO 读游标 */
static int g_queue_fd = -1; /* 只对某个 fd 出队；-1=任意 */
/* 对端地址（网络序）。0＝recvfrom 不回填，模拟二层 SADP 那类没有 IP 的来源。 */
static uint32_t g_from_ip;

static int io_socket(int d, int t, int p) {
    (void)d;
    (void)t;
    (void)p;
    return g_next_fd++;
}
static int io_bind(int fd, const struct sockaddr *a, socklen_t l) {
    (void)fd;
    (void)a;
    (void)l;
    return 0;
}
static int io_setsockopt(int fd, int lv, int on, const void *v, socklen_t l) {
    (void)fd;
    (void)lv;
    (void)on;
    (void)v;
    (void)l;
    return 0;
}
static ssize_t io_sendto(int fd, const void *buf, size_t len, int flags,
                         const struct sockaddr *a, socklen_t al) {
    (void)flags;
    (void)a;
    (void)al;
    if (g_sent_n < 16 && len <= IV_DEV_PROBE_MAX) {
        memcpy(g_sent[g_sent_n], buf, len);
        g_sent_len[g_sent_n] = len;
        g_sent_n++;
        g_last_sent_fd = fd;
    }
    return (ssize_t)len;
}
static ssize_t io_recvfrom(int fd, void *buf, size_t len, int flags,
                           struct sockaddr *a, socklen_t *al) {
    (void)flags;
    if (g_queue_i >= g_queue_n || (g_queue_fd >= 0 && fd != g_queue_fd)) {
        errno = EAGAIN;
        return -1;
    }
    if (g_queue_len[g_queue_i] > len) {
        errno = EAGAIN;
        return -1;
    }
    memcpy(buf, g_queue[g_queue_i], g_queue_len[g_queue_i]);
    /* 对端地址：只有用例显式设了 g_from_ip 才填（d13 用）。
     * 默认不写，模拟"没有对端地址"——生产代码读 ss_family 前必须自己清零。 */
    if (g_from_ip != 0u && a != NULL) {
        struct sockaddr_in *sin = (struct sockaddr_in *)a;
        memset(sin, 0, sizeof(*sin));
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = g_from_ip;
        if (al != NULL) *al = sizeof(*sin);
    }
    ssize_t r = (ssize_t)g_queue_len[g_queue_i];
    g_queue_i++;
    return r;
}
static int io_close(int fd) {
    (void)fd;
    return 0;
}
static uint64_t g_now;
static uint64_t io_now(void) { return g_now; }

static const iv_device_iops_t g_io = {
    .socket = io_socket,
    .bind = io_bind,
    .setsockopt = io_setsockopt,
    .sendto = io_sendto,
    .recvfrom = io_recvfrom,
    .close = io_close,
    .now_ms = io_now,
};

static void io_reset(void) {
    g_sent_n = 0;
    g_queue_n = 0;
    g_queue_i = 0;
    g_queue_fd = -1;
    g_from_ip = 0;
    g_now = 0;
}
static void io_push(const void *data, size_t len) {
    if (g_queue_n < 8) {
        memcpy(g_queue[g_queue_n], data, len);
        g_queue_len[g_queue_n] = len;
        g_queue_n++;
    }
}

/* ---------------------------------------------------------------------------
 * d01：SADP 校验和（用参考算法手算的黄金值）
 * ------------------------------------------------------------------------- */
static void d01_sadp_checksum(void) {
    /* 偶长度 6 字节：手算结果 0xF3F6 */
    const uint8_t a[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    chk(iv_sadp_checksum(a, 6) == 0xF3F6, "d01 even-len checksum == 0xF3F6");
    /* 奇长度 3 字节：手算结果 0xFDFB（含尾部单字节累加分支） */
    const uint8_t b[3] = {0x01, 0x02, 0x03};
    chk(iv_sadp_checksum(b, 3) == 0xFDFB, "d01 odd-len checksum == 0xFDFB");
}

/* ---------------------------------------------------------------------------
 * d02：SADP Inquiry 字节布局 + 应答解析
 * ------------------------------------------------------------------------- */
static void d02_sadp_build_parse(void) {
    uint8_t mac[6] = {0x3a, 0x82, 0xb4, 0x1d, 0x3c, 0xbf};
    uint8_t ip[4] = {192, 168, 2, 120};
    uint8_t body[IV_SADP_INQUIRY_LEN];
    int n = iv_sadp_build_inquiry(body, 0x1000, mac, ip);
    chk(n == 66, "d02 inquiry len 66");
    chk(body[0] == 0x21 && body[1] == 0x02 && body[2] == 0x01, "d02 magic bytes");
    chk(body[3] == 0x42, "d02 client prefix 0x42");
    chk(body[6] == 0x10 && body[7] == 0x00, "d02 counter BE 0x1000");
    chk(body[10] == 0x03, "d02 pkt type Inquiry");
    chk(memcmp(&body[14], mac, 6) == 0, "d02 src mac at 14");
    chk(memcmp(&body[20], ip, 4) == 0, "d02 src ip at 20");
    chk(memcmp(&body[24], "\xFF\xFF\xFF\xFF\xFF\xFF", 6) == 0, "d02 dst mac bcast");
    /* IPv6 EUI-64 首两字节 FE80、mac[0]^0x02 在第 8 字节 */
    chk(body[38] == 0xFE && body[39] == 0x80, "d02 ipv6 linklocal prefix");
    chk(body[38 + 8] == (uint8_t)(mac[0] ^ 0x02), "d02 ipv6 eui64 bit flip");
    /* 校验和写在大端 12/13，且自校验：把两字节清 0 再算应等于写入值 */
    {
        uint16_t want = (uint16_t)((body[12] << 8) | body[13]);
        uint8_t tmp[IV_SADP_INQUIRY_LEN];
        memcpy(tmp, body, sizeof(tmp));
        tmp[12] = tmp[13] = 0;
        chk(iv_sadp_checksum(tmp, 66) == want, "d02 checksum self-consistent");
    }

    /* 造一帧合法应答：eth(14) + body(>= 354+32) */
    {
        uint8_t frame[IV_SADP_ETH_HDR_LEN + 400];
        iv_dev_desc_t out;
        memset(frame, 0, sizeof(frame));
        frame[0] = 0x00;
        memcpy(&frame[6], mac, 6); /* 源 MAC */
        frame[12] = 0x80;
        frame[13] = 0x33;
        uint8_t *b = &frame[IV_SADP_ETH_HDR_LEN];
        b[0] = 0x21;
        b[3] = 0xF6;
        b[10] = 0x04;
        memcpy(&b[14], mac, 6);
        uint8_t rip[4] = {192, 168, 1, 200};
        memcpy(&b[20], rip, 4);
        memcpy(&b[38], "DS-2CD1234", 10);
        memcpy(&b[354], "DS-2CD1234ABCD", 14);
        chk(iv_sadp_parse(frame, sizeof(frame), &out) == IV_OK, "d02 parse ok");
        chk(out.brand == IV_DEV_BRAND_HIKVISION, "d02 brand hikvision");
        chk(out.ip_be == inet_addr("192.168.1.200"), "d02 ip parsed");
        chk(memcmp(out.mac, mac, 6) == 0, "d02 mac parsed");
        chk(strcmp(out.model, "DS-2CD1234ABCD") == 0, "d02 model at 354");
        /* 旧版类型 0x0B 也应接受 */
        b[10] = 0x0B;
        chk(iv_sadp_parse(frame, sizeof(frame), &out) == IV_OK, "d02 legacy 0x0B ok");
        /* 非 SADP EtherType 应拒绝 */
        frame[12] = 0x08;
        chk(iv_sadp_parse(frame, sizeof(frame), &out) != IV_OK, "d02 reject non-sadp");
        /* IP 全 0 应拒绝 */
        frame[12] = 0x80;
        memset(&b[20], 0, 4);
        chk(iv_sadp_parse(frame, sizeof(frame), &out) != IV_OK, "d02 reject zero ip");
    }
}

/* ---------------------------------------------------------------------------
 * d03：海康 UDP 探测文本
 * ------------------------------------------------------------------------- */
static void d03_hik_probe(void) {
    char buf[512];
    int n = iv_hik_probe_build(buf, sizeof(buf), "11223344-5566-7788-99aa-bbccddeeff00");
    chk(n > 0, "d03 hik probe built");
    chk(strstr(buf, "<?xml version=\"1.0\" encoding=\"utf-8\"?>") != NULL,
        "d03 xml decl");
    chk(strstr(buf, "<Types>inquiry</Types>") != NULL, "d03 types inquiry");
    chk(strstr(buf, "11223344-5566-7788-99aa-bbccddeeff00") != NULL, "d03 uuid in");
    chk(n == (int)strlen(buf), "d03 len == strlen");
}

/* ---------------------------------------------------------------------------
 * d04：大华两套探测帧黄金字节
 * ------------------------------------------------------------------------- */
static void d04_dahua_probe(void) {
    uint8_t buf[128];
    int n0 = iv_dahua_probe_build(0, buf, sizeof(buf));
    chk(n0 == 32, "d04 5050 probe len 32");
    chk(buf[0] == 0xa3 && buf[1] == 0x01 && buf[3] == 0x01, "d04 5050 head");
    chk(buf[16] == 0x02, "d04 5050 byte16 == 0x02");
    int n1 = iv_dahua_probe_build(1, buf, sizeof(buf));
    chk(n1 == 105, "d04 mcast probe len 105");
    chk(buf[0] == 0x20 && memcmp(&buf[4], "DHIP", 4) == 0, "d04 mcast DHIP magic");
    chk(memcmp(&buf[24], "\x49\x00\x00\x00", 4) == 0, "d04 mcast len field");
    chk(strstr((char *)buf + 32, "DHDiscover.search") != NULL, "d04 mcast json method");
    /* 缓冲不足应报 IV_ERANGE */
    chk(iv_dahua_probe_build(1, buf, 10) == IV_ERANGE, "d04 short buf ERANGE");
    const uint16_t *pt = iv_dahua_port_table();
    chk(pt[0] == 5050 && pt[1] == 37810 && pt[2] == 37820 && pt[3] == 8087,
        "d04 port table");
}

/* ---------------------------------------------------------------------------
 * d05：大华应答解析（含 32 字节 DHIP 头跳过）
 * ------------------------------------------------------------------------- */
static void d05_dahua_parse(void) {
    /* 32 字节头 + JSON 文本 */
    uint8_t buf[256];
    iv_dev_desc_t out;
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x20;
    memcpy(&buf[4], "DHIP", 4);
    const char *js = "{\"mac\":\"3c:ef:8c:11:22:33\",\"DeviceType\":\"DH-IPC-HFW1234\",\"ip\":\"192.168.1.106\"}";
    memcpy(buf + 32, js, strlen(js));
    chk(iv_dahua_parse(buf, 32 + strlen(js), &out) == IV_OK, "d05 parse ok");
    chk(out.brand == IV_DEV_BRAND_DAHUA, "d05 brand dahua");
    chk(out.mac[0] == 0x3c && out.mac[1] == 0xef && out.mac[5] == 0x33, "d05 mac");
    chk(strcmp(out.model, "DH-IPC-HFW1234") == 0, "d05 model");
    chk(out.ip_be == inet_addr("192.168.1.106"), "d05 ip");
    /* 无 mac 关键字应拒绝 */
    memset(buf, 'x', sizeof(buf));
    chk(iv_dahua_parse(buf, 64, &out) != IV_OK, "d05 reject no-mac");
}

/* ---------------------------------------------------------------------------
 * d06：ONVIF Probe 文本
 * ------------------------------------------------------------------------- */
static void d06_onvif_probe(void) {
    char buf[IV_DEV_PROBE_MAX];
    int n = iv_onvif_probe_build(buf, sizeof(buf));
    chk(n > 0, "d06 onvif probe built");
    chk(strstr(buf, "<Probe xmlns=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">") != NULL,
        "d06 probe element");
    chk(strstr(buf, "dp0:NetworkVideoTransmitter") != NULL, "d06 types NVT");
    chk(strstr(buf, "urn:schemas-xmlsoap-org:ws:2005:04:discovery") != NULL, "d06 to");
    chk(n == (int)strlen(buf), "d06 len");
}

/* ---------------------------------------------------------------------------
 * d07：ONVIF/宇视/海康 关键字应答解析
 * ------------------------------------------------------------------------- */
static void d07_onvif_parse(void) {
    iv_dev_desc_t out;
    /* 海康：<MAC> + XAddrs */
    const char *hik =
        "<ProbeMatch><MAC>3a:82:b4:1d:3c:bf</MAC>"
        "<XAddrs>http://192.168.1.200/onvif/device_service</XAddrs>"
        "<DeviceType>DS-2CD</DeviceType></ProbeMatch>";
    chk(iv_onvif_parse(hik, strlen(hik), &out) == IV_OK, "d07 hik ok");
    chk(out.brand == IV_DEV_BRAND_HIKVISION, "d07 hik brand");
    chk(out.mac[0] == 0x3a && out.mac[5] == 0xbf, "d07 hik mac");
    chk(out.ip_be == inet_addr("192.168.1.200"), "d07 hik ip from XAddrs");

    /* 宇视：/macaddr/ */
    const char *unv =
        "<ProbeMatch xmlns=\"/macaddr/3a82b41d3cbf\"><XAddrs>http://10.0.0.5/onvif</XAddrs></ProbeMatch>";
    chk(iv_onvif_parse(unv, strlen(unv), &out) == IV_OK, "d07 unv ok");
    chk(out.brand == IV_DEV_BRAND_UNV, "d07 unv brand");
    chk(out.mac[2] == 0xb4 && out.mac[5] == 0xbf, "d07 unv mac no-sep");

    /* 泛 ONVIF：onvif.org/name */
    const char *ov =
        "<d:Scopes>onvif://www.onvif.org/name/IPCamera</d:Scopes>"
        "<d:XAddrs>http://192.168.9.9/onvif/device_service</d:XAddrs>";
    chk(iv_onvif_parse(ov, strlen(ov), &out) == IV_OK, "d07 onvif ok");
    chk(out.brand == IV_DEV_BRAND_ONVIF, "d07 onvif brand");
    chk(strcmp(out.model, "IPCamera") == 0, "d07 onvif model from scopes");
    chk(out.ip_be == inet_addr("192.168.9.9"), "d07 onvif ip");

    /* 无关文本应拒绝 */
    chk(iv_onvif_parse("hello world", 11, &out) != IV_OK, "d07 reject junk");
}

/* ---------------------------------------------------------------------------
 * d08：发现状态机——阶段顺序、轮数与发送数、走到 DONE
 * ------------------------------------------------------------------------- */
static void d08_state_machine(void) {
    iv_device_t d;
    iv_device_cfg_t cfg;
    uint64_t t = 0;
    int guard = 0;

    io_reset();
    iv_device_cfg_default(&cfg);
    cfg.iops = &g_io;
    cfg.ifname = NULL; /* 无 SADP */
    cfg.round_ms = 100;
    cfg.rounds = 2;
    iv_device_init(&d, &cfg);
    chk(iv_device_open(&d, &cfg, 0) == IV_OK, "d08 open ok");
    chk(iv_device_sadp_available(&d) == 0, "d08 sadp unavailable (no ifname)");
    chk(iv_device_fd_count(&d) == 1, "d08 one udp fd");

    chk(iv_device_start_search(&d, 0) == IV_OK, "d08 start ok");
    chk(iv_device_phase(&d) == IV_DEV_PHASE_HIK_UDP, "d08 skip sadp -> hik-udp");

    while (iv_device_is_searching(&d) && guard++ < 200) {
        t += 100;
        iv_device_tick(&d, t);
    }
    chk(!iv_device_is_searching(&d), "d08 search finished");
    chk(iv_device_phase(&d) == IV_DEV_PHASE_DONE, "d08 ended at DONE");
    /* 发送数：hik-udp 2 + dahua-5050 2 + dahua-mcast 2*3 + unv 2 + onvif 2 = 14 */
    chk(iv_device_sent(&d) == 14, "d08 total sends == 14");

    /* 阶段名与品牌名 */
    chk(strcmp(iv_dev_phase_name(IV_DEV_PHASE_DAHUA_MCAST), "dahua-mcast") == 0,
        "d08 phase name");
    chk(strcmp(iv_dev_brand_name(IV_DEV_BRAND_HIKVISION), "hikvision") == 0,
        "d08 brand name");
    iv_device_close(&d);
}

/* ---------------------------------------------------------------------------
 * d09：应答注入 → 入档（经 UDP 收包路径）
 * ------------------------------------------------------------------------- */
static void d09_archive_from_rx(void) {
    iv_device_t d;
    iv_device_cfg_t cfg;
    io_reset();
    iv_device_cfg_default(&cfg);
    cfg.iops = &g_io;
    cfg.ifname = NULL;
    cfg.round_ms = 100;
    cfg.rounds = 1;
    iv_device_init(&d, &cfg);
    iv_device_open(&d, &cfg, 0);

    const char *resp =
        "<ProbeMatch><MAC>3a:82:b4:1d:3c:bf</MAC>"
        "<XAddrs>http://192.168.1.200/onvif/device_service</XAddrs>"
        "<DeviceType>DS-2CD</DeviceType></ProbeMatch>";
    io_push(resp, strlen(resp));

    iv_device_start_search(&d, 0);
    iv_device_tick(&d, 0); /* 收包 + 发一轮 */
    chk(iv_device_count(&d) == 1, "d09 one archived");
    iv_dev_desc_t e;
    chk(iv_device_get(&d, 0, &e) == IV_OK, "d09 get ok");
    chk(e.brand == IV_DEV_BRAND_HIKVISION, "d09 brand");
    chk(e.ip_be == inet_addr("192.168.1.200"), "d09 ip");
    chk(e.found_by == IV_DEV_PHASE_HIK_UDP, "d09 found_by phase");
    iv_device_close(&d);
}

/* ---------------------------------------------------------------------------
 * d10：去重与更新
 * ------------------------------------------------------------------------- */
static void d10_dedup(void) {
    iv_device_t d;
    iv_device_cfg_t cfg;
    io_reset();
    iv_device_cfg_default(&cfg);
    cfg.iops = &g_io;
    cfg.ifname = NULL;
    cfg.rounds = 1;
    iv_device_init(&d, &cfg);
    iv_device_open(&d, &cfg, 0);
    iv_device_start_search(&d, 0);

    /* 同 MAC 两次、第二次带型号与 IP → 应合并为 1 条并更新 */
    const char *r1 = "<MAC>3a:82:b4:1d:3c:bf</MAC><XAddrs>http://192.168.1.200/x</XAddrs>";
    io_push(r1, strlen(r1));
    iv_device_tick(&d, 0);
    const char *r2 = "<MAC>3a:82:b4:1d:3c:bf</MAC><XAddrs>http://192.168.1.201/x</XAddrs>"
                     "<DeviceType>NEW</DeviceType>";
    io_push(r2, strlen(r2));
    iv_device_tick(&d, 100);
    chk(iv_device_count(&d) == 1, "d10 dedup to 1");
    iv_dev_desc_t e;
    iv_device_get(&d, 0, &e);
    chk(e.ip_be == inet_addr("192.168.1.201"), "d10 ip updated");
    chk(strcmp(e.model, "NEW") == 0, "d10 model updated");
    iv_device_close(&d);
}

/* ---------------------------------------------------------------------------
 * d11：档案满 → IV_EFULL；clear 复位
 * ------------------------------------------------------------------------- */
static void d11_full_and_clear(void) {
    iv_device_t d;
    iv_device_cfg_t cfg;
    int i;
    io_reset();
    iv_device_cfg_default(&cfg);
    cfg.iops = &g_io;
    cfg.ifname = NULL;
    iv_device_init(&d, &cfg);
    iv_device_open(&d, &cfg, 0);
    iv_device_start_search(&d, 0);

    for (i = 0; i < IV_DEV_MAX; i++) {
        char pkt[128];
        int n = snprintf(pkt, sizeof(pkt),
                         "<MAC>3a:82:b4:1d:3c:%02x</MAC><XAddrs>http://10.0.0.%d/x</XAddrs>",
                         0xa0 + i, i + 1);
        io_push(pkt, (size_t)n);
        iv_device_tick(&d, (uint64_t)(i * 100));
    }
    chk(iv_device_count(&d) == IV_DEV_MAX, "d11 filled to IV_DEV_MAX");

    iv_device_clear(&d);
    chk(iv_device_count(&d) == 0, "d11 clear resets");
    iv_device_close(&d);
    chk(iv_device_is_open(&d) == 0, "d11 closed");
}

/* ---------------------------------------------------------------------------
 * d12：畸形包不崩、不误入档
 * ------------------------------------------------------------------------- */
static void d12_malformed(void) {
    iv_device_t d;
    iv_device_cfg_t cfg;
    io_reset();
    iv_device_cfg_default(&cfg);
    cfg.iops = &g_io;
    cfg.ifname = NULL;
    iv_device_init(&d, &cfg);
    iv_device_open(&d, &cfg, 0);
    iv_device_start_search(&d, 0);

    const char *junk = "\x00\x01\x02 not a camera";
    io_push(junk, strlen(junk));
    const char *badmac = "<MAC>zz:zz</MAC>"; /* 非法 hex */
    io_push(badmac, strlen(badmac));
    io_push("", 0);                  /* 空 */
    iv_device_tick(&d, 0);
    chk(iv_device_count(&d) == 0, "d12 junk not archived");
    /* 未 open 就 tick → IV_ESTATE */
    {
        iv_device_t e;
        iv_device_init(&e, &cfg);
        chk(iv_device_tick(&e, 0) == IV_ESTATE, "d12 tick before open == ESTATE");
    }
    iv_device_close(&d);
}

/* ---------------------------------------------------------------------------
 * d13：报文里没有 IP 时用对端地址兜底（2026-10-27 现场：ip1..ip5 全空）
 *
 * 板端真帧：平台点「搜索设备」后设备回了 5 条档案，`ip` 全为空串，
 * 平台那张表按 IP 建行 ⇒ 一行都落不下来、"已完成 0 个查询"。
 * 海康 UDP 37020 / 大华组播的应答体里没有可解析的 IP，但应答方一定是从
 * 自己的 IP 发出来的 ⇒ recvfrom 的对端地址就是答案。
 * ------------------------------------------------------------------------- */
static void d13_peer_ip_fallback(void) {
    iv_device_t d;
    iv_device_cfg_t cfg;
    iv_dev_desc_t e;

    /* ① 有对端地址：ONVIF 报文里**故意不带 XAddrs**，IP 只能来自对端 */
    io_reset();
    iv_device_cfg_default(&cfg);
    cfg.iops = &g_io;
    cfg.ifname = NULL;
    cfg.round_ms = 100;
    cfg.rounds = 1;
    iv_device_init(&d, &cfg);
    iv_device_open(&d, &cfg, 0);
    g_from_ip = inet_addr("192.168.2.64");
    const char *noip =
        "<ProbeMatch><MAC>3a:82:b4:1d:3c:bf</MAC>"
        "<DeviceType>DS-2CD3T46</DeviceType></ProbeMatch>";
    io_push(noip, strlen(noip));
    iv_device_start_search(&d, 0);
    iv_device_tick(&d, 0);
    chk(iv_device_count(&d) == 1, "d13 archived without XAddrs");
    chk(iv_device_get(&d, 0, &e) == IV_OK, "d13 get ok");
    chk(e.ip_be == inet_addr("192.168.2.64"), "d13 ip taken from peer address");
    iv_device_close(&d);

    /* ② 没有对端地址（模拟二层 SADP）：不能凭空造 IP，保持 0 */
    io_reset();
    iv_device_init(&d, &cfg);
    iv_device_open(&d, &cfg, 0);
    io_push(noip, strlen(noip));
    iv_device_start_search(&d, 0);
    iv_device_tick(&d, 0);
    chk(iv_device_count(&d) == 1, "d13 archived, no peer");
    chk(iv_device_get(&d, 0, &e) == IV_OK, "d13 get ok 2");
    chk(e.ip_be == 0u, "d13 no peer -> ip stays 0 (不编造)");
    iv_device_close(&d);

    /* ③ 报文自带 IP 时**不被对端地址覆盖**（SADP 应答报文里有真 IP） */
    io_reset();
    iv_device_init(&d, &cfg);
    iv_device_open(&d, &cfg, 0);
    g_from_ip = inet_addr("10.0.0.9"); /* 故意给一个不同的 */
    const char *withip =
        "<ProbeMatch><MAC>3a:82:b4:1d:3c:bf</MAC>"
        "<XAddrs>http://192.168.1.200/onvif/device_service</XAddrs>"
        "<DeviceType>DS-2CD</DeviceType></ProbeMatch>";
    io_push(withip, strlen(withip));
    iv_device_start_search(&d, 0);
    iv_device_tick(&d, 0);
    chk(iv_device_get(&d, 0, &e) == IV_OK, "d13 get ok 3");
    chk(e.ip_be == inet_addr("192.168.1.200"), "d13 payload ip wins over peer");
    iv_device_close(&d);
    io_reset();
}

/* ---------------------------------------------------------------------------
 * d14：海康 UDP 37020 的 XML —— 型号取 `DeviceDescription`、IP 取 `<IPv4Address>`
 *
 * 报文头部逐字取自 2026-10-27 16:24 现场抓包（`192.168.2.100:37020`）。
 * 板端旧实现在这条报文上给出 `model=46672`、`ip=""`：
 *   - `46672` 是 `<DeviceType>`，**类型码不是型号**，真型号是 `DS-7804N-F1(C)`；
 *   - 旧代码只从 `XAddrs` 取 IP，海康 XML 用的是 `<IPv4Address>` ⇒ 全空。
 * 两者一起把平台那张表打空（IP 空 ⇒ 按 IP 建行建不出来）。
 * ------------------------------------------------------------------------- */
static void d14_hik_udp_xml(void) {
    static const char *xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<ProbeMatch>\n"
        "<Uuid>00000000-0000-0000-0000-000000000000</Uuid>\n"
        "<Types>inquiry</Types>\n"
        "<DeviceType>46672</DeviceType>\n"
        "<DeviceDescription>DS-7804N-F1(C)</DeviceDescription>\n"
        "<DeviceSN>DS-7804N-F1(C)0420230310CCRRL27272938WVU</DeviceSN>\n"
        "<CommandPort>8000</CommandPort>\n"
        "<MAC>e0-ba-ad-da-ab-dd</MAC>\n"
        "<IPv4Address>192.168.2.100</IPv4Address>\n"
        "<IPv4SubnetMask>255.255.255.0</IPv4SubnetMask>\n"
        "</ProbeMatch>\n";
    iv_dev_desc_t out;
    chk(iv_onvif_parse(xml, strlen(xml), &out) == IV_OK, "d14 parse ok");
    chk(out.brand == IV_DEV_BRAND_HIKVISION, "d14 brand from <MAC>");
    chk(out.ip_be == inet_addr("192.168.2.100"), "d14 ip from <IPv4Address>");
    chk(strcmp(out.model, "DS-7804N-F1(C)") == 0,
        "d14 model = DeviceDescription（不是 DeviceType 的 46672）");
    chk(strcmp(out.model, "46672") != 0, "d14 model is not the type code");
    chk(strncmp(out.serial, "DS-7804N-F1(C)", 14) == 0, "d14 serial from DeviceSN");
    chk(memcmp(out.mac, "\xE0\xBA\xAD\xDA\xAB\xDD", 6) == 0, "d14 mac dash form");
}

int main(void) {
    d01_sadp_checksum();
    d02_sadp_build_parse();
    d03_hik_probe();
    d04_dahua_probe();
    d05_dahua_parse();
    d06_onvif_probe();
    d07_onvif_parse();
    d08_state_machine();
    d09_archive_from_rx();
    d10_dedup();
    d11_full_and_clear();
    d12_malformed();
    d13_peer_ip_fallback();
    d14_hik_udp_xml();

    if (g_fail != 0) {
        fprintf(stderr, "test_device FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_device passed (sadp csum/build/parse, hik probe, dahua probe/parse, "
           "onvif probe/parse, discovery fsm, archive dedup/full/clear, malformed, "
           "peer-ip fallback, hik-udp xml model/ip)\n");
    return 0;
}
