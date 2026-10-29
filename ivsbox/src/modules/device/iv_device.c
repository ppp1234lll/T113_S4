/*
 * 摄像机信息探测（多品牌发现）实现（libivmodules，功能开发计划 M4-S4.2）
 *
 * 报文口径逐字照抄参考项目（见 iv_device.h 文件头）；本文件只做三件事：
 *   ① 纯报文构造/解析（无 I/O，单测直接钉字节）；
 *   ② 一个由 tick 推进的**顺序发现状态机**（海康→大华→宇视→ONVIF）；
 *   ③ 档案去重/更新（MAC 优先、其次 IP）。
 *
 * 全部 socket 走 `iv_device_iops_t` 注入点；生产用 `iv_device_default_iops()`。
 * 非阻塞一律用 `recvfrom(..., MSG_DONTWAIT, ...)`，不依赖 fcntl（保持注入点最小）。
 */
#include "ivsbox/iv_device.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h> /* struct sockaddr_ll（AF_PACKET） */
#include <net/if.h>          /* if_nametoindex */
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_log.h"

/* 不引 <linux/if_ether.h>：它与 <net/if.h> 有 struct ifmap 等重定义冲突。
 * ETH_P_ALL = 0x0003 是内核 UAPI 常量，此处本地定义即可。 */
#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif

#define DEV_MOD "device"

/* ---------------------------------------------------------------------------
 * 编译期默认
 * ------------------------------------------------------------------------- */
#define IV_DEV_ROUND_MS_DEFAULT 300u
#define IV_DEV_ROUNDS_DEFAULT   3u

/* SADP 体偏移（相对负载体；参考 sadp.c:16-32） */
#define SADP_OFF_PKT_TYPE   10
#define SADP_OFF_CHECKSUM   12
#define SADP_OFF_SRC_MAC    14
#define SADP_OFF_SRC_IP     20
#define SADP_OFF_DST_MAC    24
#define SADP_OFF_INQ_IPV6   38
#define SADP_OFF_RSP_SN     38
#define SADP_OFF_RSP_MODEL  354
#define SADP_PKT_INQUIRY    0x03
#define SADP_PKT_INQUIRY_RSP 0x04
#define SADP_PKT_INQUIRY_RSP_LEGACY 0x0B
#define SADP_CLIENT_PREFIX  0x42 /* 66 */
#define SADP_SERVER_PREFIX  0xF6

/* 海康 UDP / ONVIF 组播 */
#define HIK_MCAST_ADDR "239.255.255.250"
#define HIK_UDP_PORT   37020u
#define ONVIF_MCAST_ADDR "239.255.255.250"
#define ONVIF_PORT     3702u
#define UNV_BCAST_ADDR "255.255.255.255"
#define UNV_PORT       3702u
#define DAHUA_BCAST_ADDR "255.255.255.255"
#define DAHUA_MCAST_ADDR "239.255.255.251"

/* 参考 onvif.c:34 的固定 Probe 报文（逐字照抄，MessageID 固定）。
 * 拆成两段拼接以避免单行超长。 */
static const char k_onvif_probe_a[] =
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"><s:Header>"
    "<a:Action s:mustUnderstand=\"1\">"
    "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
    "<a:MessageID>uuid:bf3b6784-eb7b-41cf-b096-f902214bb40d</a:MessageID>"
    "<a:ReplyTo><a:Address>"
    "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
    "</a:Address></a:ReplyTo>"
    "<a:To s:mustUnderstand=\"1\">"
    "urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To></s:Header><s:Body>"
    "<Probe xmlns=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
    "<d:Types xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
    "xmlns:dp0=\"http://www.onvif.org/ver10/network/wsdl\">"
    "dp0:NetworkVideoTransmitter</d:Types></Probe></s:Body></s:Envelope>";

/* 海康 UDP 探测模板（参考 onvif.c:457-461） */
static const char k_hik_probe_head[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<Probe><Uuid>";
static const char k_hik_probe_tail[] = "</Uuid><Types>inquiry</Types></Probe>";

/* 大华 5050 广播 32 字节（参考 onvif.c:30-32） */
static const uint8_t k_dahua_5050[32] = {
    0xa3, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* 大华组播 105 字节（参考 onvif.c:21-28，逐字照抄） */
static const uint8_t k_dahua_mcast[105] = {
    0x20, 0x00, 0x00, 0x00, 0x44, 0x48, 0x49, 0x50, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7b, 0x20, 0x22, 0x6d,
    0x65, 0x74, 0x68, 0x6f, 0x64, 0x22, 0x20, 0x3a, 0x20, 0x22, 0x44, 0x48,
    0x44, 0x69, 0x73, 0x63, 0x6f, 0x76, 0x65, 0x72, 0x2e, 0x73, 0x65, 0x61,
    0x72, 0x63, 0x68, 0x22, 0x2c, 0x20, 0x22, 0x70, 0x61, 0x72, 0x61, 0x6d,
    0x73, 0x22, 0x20, 0x3a, 0x20, 0x7b, 0x20, 0x22, 0x6d, 0x61, 0x63, 0x22,
    0x20, 0x3a, 0x20, 0x22, 0x22, 0x2c, 0x20, 0x22, 0x75, 0x6e, 0x69, 0x22,
    0x20, 0x3a, 0x20, 0x31, 0x20, 0x7d, 0x20, 0x7d, 0x0a};

/* 大华组播端口表（参考 onvif.c:37-38；5050 单列，其余三端口组播） */
static const uint16_t k_dahua_ports[IV_DAHUA_PORT_N] = {5050, 37810, 37820,
                                                        8087};

const uint16_t *iv_dahua_port_table(void) { return k_dahua_ports; }

/* ---------------------------------------------------------------------------
 * 默认注入点
 * ------------------------------------------------------------------------- */
static const iv_device_iops_t g_default_iops = {
    .socket = socket,
    .bind = bind,
    .setsockopt = setsockopt,
    .sendto = sendto,
    .recvfrom = recvfrom,
    .close = close,
    .now_ms = iv_clock_monotonic_ms,
};

const iv_device_iops_t *iv_device_default_iops(void) { return &g_default_iops; }

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */
static uint8_t rd8(const uint8_t *p) { return p[0]; }

/* 把 4 字节网络序 IP 写进 out（调用方保证 out 可写 4 字节）。 */
static void wr_ip4(uint8_t *out, uint32_t ip_be) {
    memcpy(out, &ip_be, 4);
}

static uint32_t rd_ip4(const uint8_t *p) {
    /* 与全工程 ip_be 约定一致（= struct in_addr.s_addr 的内存语义）：
     * 直接按本机字节序拷贝 4 字节，不做大小端换算。 */
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* 拷贝可打印 ASCII（0x20~0x7E），遇 0x00/不可打印即停；结果 NUL 结尾。 */
static void copy_printable(char *dst, size_t cap, const uint8_t *src,
                           size_t n) {
    size_t i = 0;
    if (cap == 0) return;
    for (; i + 1 < cap && i < n; i++) {
        uint8_t c = src[i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) break;
        dst[i] = (char)c;
    }
    dst[i] = '\0';
}

/* 十六进制字符 → 值；非法返回 -1。 */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 从 12 个 hex 字符解析 MAC（允许分隔符 ':'/'-'）。返回 0 成功。 */
static int parse_mac_hex(const char *s, uint8_t mac[6]) {
    int nib = 0;
    uint8_t acc[12];
    for (const char *p = s; *p && nib < 12; p++) {
        if (*p == ':' || *p == '-') continue;
        int v = hexval(*p);
        if (v < 0) return -1;
        acc[nib++] = (uint8_t)v;
    }
    if (nib != 12) return -1;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)((acc[i * 2] << 4) | acc[i * 2 + 1]);
    return 0;
}

static int mac_is_zero(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++)
        if (mac[i]) return 0;
    return 1;
}

/*
 * 取"键后值"：找 key，跳过 `:` `"` `>` `=` 与空白，读到 `"` / `<` / 空白前。
 * 同时适配 JSON（`DeviceType":"X"`）与 XML（`<DeviceType>X</DeviceType>`）两种形态。
 * 未命中时把 out[0] 置空。
 */
/* 从 `<Key>1.2.3.4</Key>` 形态的 XML 里取 IPv4。取不到返回 0（保持原值）。 */
static uint32_t ip_after_xml_tag(const char *txt, const char *open_tag) {
    const char *p = strstr(txt, open_tag);
    char        buf[24];
    size_t      i = 0;
    struct in_addr ia;

    if (p == NULL) return 0u;
    p += strlen(open_tag);
    while (p[i] != '\0' && p[i] != '<' && i + 1u < sizeof(buf)) {
        buf[i] = p[i];
        i++;
    }
    buf[i] = '\0';
    if (i == 0u) return 0u;
    ia.s_addr = 0;
    if (inet_pton(AF_INET, buf, &ia) != 1) return 0u;
    return ia.s_addr;
}

static void extract_after_key(const char *txt, const char *key, char *out,
                              size_t cap) {
    const char *p = strstr(txt, key);
    size_t i = 0;
    if (cap == 0) return;
    out[0] = '\0';
    if (p == NULL) return;
    p += strlen(key);
    while (*p == ':' || *p == '"' || *p == '>' || *p == '=' || *p == ' ' ||
           *p == '\t')
        p++;
    while (p[i] && p[i] != '"' && p[i] != '<' && p[i] != ' ' &&
           i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

/* ---------------------------------------------------------------------------
 * SADP：校验和 / 构造 / 解析
 * ------------------------------------------------------------------------- */
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint16_t iv_sadp_checksum(const uint8_t *body, uint32_t prefix) {
    uint32_t checksum = 0, var1 = 0, var2 = 0, index, p = 0;
    if (body == NULL) return 0;
    if (3 < (prefix & 0xFFFFFFFEU)) {
        index = ((prefix - 4) >> 2) + 1;
        do {
            prefix -= 4;
            var1 += le16(body + p);
            var2 += le16(body + p + 2);
            p += 4;
            index--;
        } while (index != 0);
    }
    if (1 < prefix) {
        checksum = le16(body + p);
        p += 2;
        prefix -= 2;
    }
    checksum += var1 + var2;
    if (prefix != 0) checksum += body[p];
    checksum = (checksum >> 16) + (checksum & 0xFFFF);
    return (uint16_t)(~((checksum >> 16) + checksum));
}

/* EUI-64 链路本地 IPv6（参考 sadp.c:273-286）。 */
static void fill_linklocal_ipv6(uint8_t ipv6[16], const uint8_t mac[6]) {
    memset(ipv6, 0, 16);
    ipv6[0] = 0xFE;
    ipv6[1] = 0x80;
    ipv6[8] = (uint8_t)(mac[0] ^ 0x02);
    ipv6[9] = mac[1];
    ipv6[10] = mac[2];
    ipv6[11] = 0xFF;
    ipv6[12] = 0xFE;
    ipv6[13] = mac[3];
    ipv6[14] = mac[4];
    ipv6[15] = mac[5];
}

int iv_sadp_build_inquiry(uint8_t out[IV_SADP_INQUIRY_LEN], uint16_t counter,
                          const uint8_t mac[6], const uint8_t ip[4]) {
    uint16_t csum;
    if (out == NULL) return IV_EINVAL;
    memset(out, 0, IV_SADP_INQUIRY_LEN);
    out[0] = 0x21;
    out[1] = 0x02;
    out[2] = 0x01;
    out[3] = (uint8_t)SADP_CLIENT_PREFIX;
    out[6] = (uint8_t)(counter >> 8);
    out[7] = (uint8_t)(counter & 0xFF);
    out[8] = 0x06;
    out[9] = 0x04;
    out[SADP_OFF_PKT_TYPE] = SADP_PKT_INQUIRY;
    if (mac != NULL) memcpy(&out[SADP_OFF_SRC_MAC], mac, 6);
    if (ip != NULL) memcpy(&out[SADP_OFF_SRC_IP], ip, 4);
    memset(&out[SADP_OFF_DST_MAC], 0xFF, 6);
    if (mac != NULL) fill_linklocal_ipv6(&out[SADP_OFF_INQ_IPV6], mac);
    csum = iv_sadp_checksum(out, SADP_CLIENT_PREFIX);
    out[SADP_OFF_CHECKSUM] = (uint8_t)(csum >> 8);
    out[SADP_OFF_CHECKSUM + 1] = (uint8_t)(csum & 0xFF);
    return (int)IV_SADP_INQUIRY_LEN;
}

int iv_sadp_parse(const uint8_t *eth_frame, size_t len, iv_dev_desc_t *out) {
    const uint8_t *b;
    if (eth_frame == NULL || out == NULL) return IV_EINVAL;
    if (len < IV_SADP_ETH_HDR_LEN + SADP_OFF_RSP_SN) return IV_EPROTO;
    /* 以太类型须为 0x8033 */
    if (eth_frame[12] != 0x80 || eth_frame[13] != 0x33) return IV_EPROTO;
    b = eth_frame + IV_SADP_ETH_HDR_LEN;
    if (rd8(&b[0]) != 0x21) return IV_EPROTO;
    if (rd8(&b[3]) != (uint8_t)SADP_SERVER_PREFIX) return IV_EPROTO;
    if (rd8(&b[SADP_OFF_PKT_TYPE]) != SADP_PKT_INQUIRY_RSP &&
        rd8(&b[SADP_OFF_PKT_TYPE]) != SADP_PKT_INQUIRY_RSP_LEGACY)
        return IV_EPROTO;

    memset(out, 0, sizeof(*out));
    out->brand = IV_DEV_BRAND_HIKVISION;
    memcpy(out->mac, &b[SADP_OFF_SRC_MAC], 6);
    out->ip_be = rd_ip4(&b[SADP_OFF_SRC_IP]);
    out->port = 80;
    if (out->ip_be == 0) return IV_EPROTO; /* 参考 sadp.c:198：IP 全 0 无效 */

    /* 型号：优先取偏移 354 的独立字段（需整帧足够长）。 */
    if (len >= IV_SADP_ETH_HDR_LEN + SADP_OFF_RSP_MODEL + 1) {
        size_t avail = len - (IV_SADP_ETH_HDR_LEN + SADP_OFF_RSP_MODEL);
        if (avail > 31) avail = 31;
        copy_printable(out->model, sizeof(out->model),
                       &b[SADP_OFF_RSP_MODEL], avail);
    }
    /* 序列号：偏移 38 起、直到包尾的可打印串（参考 sadp.c:183-189）。 */
    if (len > IV_SADP_ETH_HDR_LEN + SADP_OFF_RSP_SN) {
        size_t avail = len - (IV_SADP_ETH_HDR_LEN + SADP_OFF_RSP_SN);
        /* 只用前 64 字节做串提取，避免把后面的型号区混进来 */
        size_t sn_max = 64;
        uint8_t tmp[64];
        if (avail > sn_max) avail = sn_max;
        memcpy(tmp, &b[SADP_OFF_RSP_SN], avail);
        copy_printable(out->serial, sizeof(out->serial), tmp, avail);
    }
    if (out->model[0] == '\0') {
        /* 型号空则以品牌名兜底（与参考 sadp.c:381-386 一致）。 */
        strncpy(out->model, "HIKVISION", sizeof(out->model) - 1);
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 海康 UDP 探测文本
 * ------------------------------------------------------------------------- */
static const char k_uuid_placeholder[] = "00000000-0000-0000-0000-000000000000";

int iv_hik_probe_build(char *out, size_t cap, const char *uuid) {
    size_t need;
    const char *u = (uuid && *uuid) ? uuid : k_uuid_placeholder;
    if (out == NULL) return IV_EINVAL;
    need = strlen(k_hik_probe_head) + strlen(u) + strlen(k_hik_probe_tail) + 1;
    if (cap < need) return IV_ERANGE;
    strcpy(out, k_hik_probe_head);
    strcat(out, u);
    strcat(out, k_hik_probe_tail);
    return (int)strlen(out);
}

/* ---------------------------------------------------------------------------
 * 大华探测帧
 * ------------------------------------------------------------------------- */
int iv_dahua_probe_build(int kind, uint8_t *out, size_t cap) {
    if (out == NULL) return IV_EINVAL;
    if (kind == 0) {
        if (cap < sizeof(k_dahua_5050)) return IV_ERANGE;
        memcpy(out, k_dahua_5050, sizeof(k_dahua_5050));
        return (int)sizeof(k_dahua_5050);
    }
    if (cap < sizeof(k_dahua_mcast)) return IV_ERANGE;
    memcpy(out, k_dahua_mcast, sizeof(k_dahua_mcast));
    return (int)sizeof(k_dahua_mcast);
}

int iv_dahua_parse(const uint8_t *buf, size_t len, iv_dev_desc_t *out) {
    const char *txt;
    size_t tlen;
    const char *p;
    if (buf == NULL || out == NULL) return IV_EINVAL;
    memset(out, 0, sizeof(*out));
    out->port = 80;
    /* 跳过 32 字节 DHIP 二进制头（参考 onvif.c:568-572）。 */
    if (len > 32 && buf[0] == 0x20 && buf[4] == 'D' && buf[5] == 'H' &&
        buf[6] == 'I' && buf[7] == 'P') {
        buf += 32;
        len -= 32;
    }
    /* 需要以文本形态找关键字；保证 NUL 结尾。 */
    {
        static char tmp[IV_DEV_RX_MAX + 1];
        size_t n = len < IV_DEV_RX_MAX ? len : IV_DEV_RX_MAX;
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        txt = tmp;
        tlen = n;
    }
    /* MAC：找 "mac" 后的 12 个 hex（允许引号/冒号）。 */
    p = strstr(txt, "\"mac\"");
    if (p == NULL) return IV_EPROTO;
    p += 5;
    /* 跳过非 hex 前缀 */
    while (*p && hexval(*p) < 0) p++;
    if (parse_mac_hex(p, out->mac) != 0) return IV_EPROTO;
    out->brand = IV_DEV_BRAND_DAHUA;
    /* 型号：JSON `DeviceType":"X"` 或 XML `<DeviceType>X</DeviceType>` 皆可 */
    extract_after_key(txt, "DeviceType", out->model, sizeof(out->model));
    /* IP：部分固件在文本里带 "ip":"x.x.x.x" */
    p = strstr(txt, "\"ip\"");
    if (p != NULL) {
        const char *q = strchr(p, ':');
        if (q != NULL) {
            q++;
            while (*q == ' ' || *q == '"') q++;
            char ipbuf[24];
            size_t i = 0;
            while (q[i] && q[i] != '"' && i + 1 < sizeof(ipbuf)) {
                ipbuf[i] = q[i];
                i++;
            }
            ipbuf[i] = '\0';
            struct in_addr ia;
            if (inet_pton(AF_INET, ipbuf, &ia) == 1) out->ip_be = ia.s_addr;
        }
    }
    (void)tlen;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * ONVIF WS-Discovery
 * ------------------------------------------------------------------------- */
int iv_onvif_probe_build(char *out, size_t cap) {
    size_t need = strlen(k_onvif_probe_a) + 1;
    if (out == NULL) return IV_EINVAL;
    if (cap < need) return IV_ERANGE;
    memcpy(out, k_onvif_probe_a, need);
    return (int)(need - 1);
}

/*
 * 关键字解析（对齐参考 onvif.c: onvif_get_ipc_param_info 的品牌判定）：
 *   <MAC>       → 海康
 *   /macaddr/   → 宇视
 *   "mac" / DHIP→ 大华
 *   其余含 onvif/ProbeMatches/NetworkVideoTransmitter → 泛 ONVIF
 * 另加一条 XAddrs 取 IP 的补充路径。
 */
int iv_onvif_parse(const char *buf, size_t len, iv_dev_desc_t *out) {
    static char tmp[IV_DEV_RX_MAX + 1];
    size_t n;
    const char *p;
    if (buf == NULL || out == NULL) return IV_EINVAL;
    n = len < IV_DEV_RX_MAX ? len : IV_DEV_RX_MAX;
    memcpy(tmp, buf, n);
    tmp[n] = '\0';
    memset(out, 0, sizeof(*out));
    out->port = 80;

    /* 品牌判定。
     *
     * **厂商自报的名字优先**（2026-10-29 板端实证）：现场 5 台档案里 4 台被判成
     * 泛 ONVIF，可其中两台的应答里明写着 `"DeviceType":"Dahua"`，另一台海康的
     * ProbeMatch Scopes 里带着 `hardware/DS-2CD3T27EDWD-L`。原判定把 `"mac"` /
     * `hardware` 这类**通用字段**放在厂商名前面，于是只认得出"这是台 ONVIF 设备"、
     * 认不出"是谁家的" ⇒ 网页品牌下拉里没有 onvif 这一项，自动匹配永远落空，
     * 用户看到的就是"搜到了、型号也解析出来了，品牌却还是默认海康"。
     *
     * 判定顺序改成：厂商自报名字 → 私有字段 → 泛 ONVIF 兜底。
     * 三条厂商线索都来自设备自己发的文本，不靠型号前缀猜（`DS-2` 是海康
     * DS-2CD/DS-2DE 系列的固定前缀，大华是 `IPC-`/`DH-`、宇视是 `IPC2xx`，不冲突）。
     */
    if (strstr(tmp, "Dahua") != NULL || strstr(tmp, "DAHUA") != NULL) {
        out->brand = IV_DEV_BRAND_DAHUA;
    } else if (strstr(tmp, "HIKVISION") != NULL ||
               strstr(tmp, "Hikvision") != NULL || strstr(tmp, "DS-2") != NULL) {
        out->brand = IV_DEV_BRAND_HIKVISION;
    } else if (strstr(tmp, "Uniview") != NULL || strstr(tmp, "uniview") != NULL) {
        out->brand = IV_DEV_BRAND_UNV;
    } else if (strstr(tmp, "DHIP") != NULL || strstr(tmp, "\"mac\"") != NULL) {
        out->brand = IV_DEV_BRAND_DAHUA;
    } else if (strstr(tmp, "/macaddr/") != NULL) {
        out->brand = IV_DEV_BRAND_UNV;
    } else if (strstr(tmp, "<MAC>") != NULL) {
        out->brand = IV_DEV_BRAND_HIKVISION;
    } else if (strstr(tmp, "MacAddress") != NULL ||
               strstr(tmp, "/MAC/") != NULL || strstr(tmp, "hardware") != NULL) {
        out->brand = IV_DEV_BRAND_ONVIF;
    } else if (strstr(tmp, "ProbeMatches") != NULL ||
               strstr(tmp, "NetworkVideoTransmitter") != NULL ||
               strstr(tmp, "onvif.org") != NULL) {
        out->brand = IV_DEV_BRAND_ONVIF;
    } else {
        return IV_EPROTO;
    }

    /* MAC：依次尝试 <MAC> / /macaddr/ / /MAC/ / MacAddress */
    p = strstr(tmp, "<MAC>");
    if (p != NULL) {
        if (parse_mac_hex(p + 5, out->mac) != 0) memset(out->mac, 0, 6);
    } else {
        p = strstr(tmp, "/macaddr/");
        if (p != NULL) {
            if (parse_mac_hex(p + 9, out->mac) != 0) memset(out->mac, 0, 6);
        } else {
            p = strstr(tmp, "/MAC/");
            if (p != NULL) {
                if (parse_mac_hex(p + 5, out->mac) != 0) memset(out->mac, 0, 6);
            }
        }
    }

    /* IP：① `<IPv4Address>`（海康 UDP 37020 的 XML 带这一项，2026-10-27 16:24 实测
     * 报文：`<IPv4Address>192.168.2.100</IPv4Address>`）；② XAddrs 里的主机名
     * （ONVIF WS-Discovery：`http://x.x.x.x[:port]/...`）。
     * 原来只有 ② ⇒ 海康 UDP 那一轮的 5 台**全部 ip 为空**，平台按 IP 建行，
     * 结果一行都落不下来（界面"暂无数据"、进度卡在"已完成 0 个查询"）。 */
    {
        uint32_t ip = ip_after_xml_tag(tmp, "<IPv4Address>");
        if (ip != 0u) out->ip_be = ip;
    }
    if (out->ip_be == 0u) {
        p = strstr(tmp, "XAddrs");
        if (p == NULL) p = strstr(tmp, "xaddrs");
        if (p != NULL) {
            const char *q = strstr(p, "http://");
            if (q != NULL) {
                q += 7;
                char ipbuf[24];
                size_t i = 0;
                while (q[i] && q[i] != ':' && q[i] != '/' && i + 1 < sizeof(ipbuf)) {
                    ipbuf[i] = q[i];
                    i++;
                }
                ipbuf[i] = '\0';
                struct in_addr ia;
                if (inet_pton(AF_INET, ipbuf, &ia) == 1) out->ip_be = ia.s_addr;
            }
        }
    }

    /* 型号：`DeviceDescription` 才是型号；`DeviceType` 在海康 UDP 报文里是**类型码**
     * （实测 `<DeviceType>46672</DeviceType>` ＋ `<DeviceDescription>DS-7804N-F1(C)
     * </DeviceDescription>`）—— 拿它当型号，界面上"型号"列就是一串没意义的数字。
     * 大华的 JSON 只有 `DeviceType`（值是型号），所以顺序是 Description → Type → name。 */
    extract_after_key(tmp, "DeviceDescription", out->model, sizeof(out->model));
    if (out->model[0] == '\0')
        extract_after_key(tmp, "DeviceType", out->model, sizeof(out->model));
    if (out->model[0] == '\0')
        extract_after_key(tmp, "onvif.org/name/", out->model,
                          sizeof(out->model));
    /* 序列号：海康 UDP/ONVIF XML 的 `DeviceSN`（建档主键仍是 MAC，这里只补全信息） */
    extract_after_key(tmp, "DeviceSN", out->serial, sizeof(out->serial));
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 品牌/阶段名
 * ------------------------------------------------------------------------- */
const char *iv_dev_brand_name(iv_dev_brand_t b) {
    switch (b) {
        case IV_DEV_BRAND_HIKVISION: return "hikvision";
        case IV_DEV_BRAND_DAHUA: return "dahua";
        case IV_DEV_BRAND_UNV: return "unv";
        case IV_DEV_BRAND_ONVIF: return "onvif";
        default: return "unknown";
    }
}

const char *iv_dev_phase_name(iv_dev_phase_t p) {
    switch (p) {
        case IV_DEV_PHASE_HIK_SADP: return "hik-sadp";
        case IV_DEV_PHASE_HIK_UDP: return "hik-udp";
        case IV_DEV_PHASE_DAHUA_5050: return "dahua-5050";
        case IV_DEV_PHASE_DAHUA_MCAST: return "dahua-mcast";
        case IV_DEV_PHASE_UNV: return "unv";
        case IV_DEV_PHASE_ONVIF: return "onvif";
        case IV_DEV_PHASE_DONE: return "done";
        default: return "idle";
    }
}

/* ---------------------------------------------------------------------------
 * 生命周期 / 配置
 * ------------------------------------------------------------------------- */
void iv_device_cfg_default(iv_device_cfg_t *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->round_ms = IV_DEV_ROUND_MS_DEFAULT;
    cfg->rounds = IV_DEV_ROUNDS_DEFAULT;
    cfg->iops = NULL;
}

void iv_device_init(iv_device_t *d, const iv_device_cfg_t *cfg) {
    if (d == NULL) return;
    memset(d, 0, sizeof(*d));
    if (cfg != NULL)
        d->cfg = *cfg;
    else
        iv_device_cfg_default(&d->cfg);
    if (d->cfg.round_ms == 0) d->cfg.round_ms = IV_DEV_ROUND_MS_DEFAULT;
    if (d->cfg.rounds == 0) d->cfg.rounds = IV_DEV_ROUNDS_DEFAULT;
    if (d->cfg.iops == NULL) d->cfg.iops = iv_device_default_iops();
    d->fd4 = -1;
    d->fd_pkt = -1;
    d->sadp_counter = 0x1000;
    d->phase = IV_DEV_PHASE_IDLE;
}

/* 建一个 UDP 收发 socket：广播使能 + 绑定临时端口。失败返回负码。 */
static int open_udp(iv_device_t *d) {
    const iv_device_iops_t *io = d->cfg.iops;
    struct sockaddr_in a;
    int one = 1;
    int fd = io->socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return IV_EIO;
    io->setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    io->setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0; /* 临时端口；靠对端单播回包 */
    if (io->bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        io->close(fd);
        return IV_EIO;
    }
    return fd;
}

int iv_device_open(iv_device_t *d, const iv_device_cfg_t *cfg, uint64_t now_ms) {
    if (d == NULL) return IV_EINVAL;
    if (d->opened) return IV_ESTATE;
    if (cfg != NULL) {
        d->cfg = *cfg;
        if (d->cfg.round_ms == 0) d->cfg.round_ms = IV_DEV_ROUND_MS_DEFAULT;
        if (d->cfg.rounds == 0) d->cfg.rounds = IV_DEV_ROUNDS_DEFAULT;
        if (d->cfg.iops == NULL) d->cfg.iops = iv_device_default_iops();
    }
    d->now_ms = now_ms;
    d->fd4 = open_udp(d);
    if (d->fd4 < 0) {
        IV_LOG_W(DEV_MOD, "udp socket open failed");
        d->fd4 = -1;
    }
    /* AF_PACKET（SADP）可选：无 ifname 或无 CAP_NET_RAW 时降级。 */
    d->fd_pkt = -1;
    d->sadp_ok = 0;
    if (d->cfg.ifname != NULL && d->cfg.ifname[0] != '\0') {
        struct sockaddr_ll sll;
        int fd = d->cfg.iops->socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd >= 0) {
            memset(&sll, 0, sizeof(sll));
            sll.sll_family = AF_PACKET;
            sll.sll_protocol = htons(ETH_P_ALL);
            sll.sll_ifindex = (int)if_nametoindex(d->cfg.ifname);
            if (sll.sll_ifindex != 0 &&
                d->cfg.iops->bind(fd, (struct sockaddr *)&sll, sizeof(sll)) == 0) {
                d->fd_pkt = fd;
                d->sadp_ok = 1;
            } else {
                d->cfg.iops->close(fd);
            }
        }
        if (!d->sadp_ok) IV_LOG_W(DEV_MOD, "AF_PACKET unavailable, SADP skipped");
    }
    d->opened = 1;
    d->phase = IV_DEV_PHASE_IDLE;
    return IV_OK;
}

void iv_device_close(iv_device_t *d) {
    if (d == NULL || !d->opened) return;
    if (d->fd4 >= 0) d->cfg.iops->close(d->fd4);
    if (d->fd_pkt >= 0) d->cfg.iops->close(d->fd_pkt);
    d->fd4 = -1;
    d->fd_pkt = -1;
    d->sadp_ok = 0;
    d->searching = 0;
    d->opened = 0;
    d->phase = IV_DEV_PHASE_IDLE;
}

void iv_device_clear(iv_device_t *d) {
    if (d == NULL) return;
    d->n_items = 0;
    memset(d->item, 0, sizeof(d->item));
}

/* ---------------------------------------------------------------------------
 * 档案：去重（MAC 优先，其次 IP）
 * ------------------------------------------------------------------------- */
static int add_item(iv_device_t *d, const iv_dev_desc_t *desc) {
    int i;
    int have_mac = !mac_is_zero(desc->mac);
    for (i = 0; i < d->n_items; i++) {
        int same = 0;
        if (have_mac && !mac_is_zero(d->item[i].mac)) {
            same = memcmp(d->item[i].mac, desc->mac, 6) == 0;
        } else if (desc->ip_be != 0 && d->item[i].ip_be == desc->ip_be) {
            same = 1;
        }
        if (same) {
            /* 更新可变属性 */
            if (desc->ip_be != 0) d->item[i].ip_be = desc->ip_be;
            if (desc->model[0] != '\0')
                strncpy(d->item[i].model, desc->model,
                        sizeof(d->item[i].model) - 1);
            if (desc->serial[0] != '\0')
                strncpy(d->item[i].serial, desc->serial,
                        sizeof(d->item[i].serial) - 1);
            /* 品牌**不能被"更弱的结论"覆盖**（2026-10-29 板端）：
             * 同一台设备会被多个相位各答一次 —— 私有协议/SADP 那一次能认出
             * "海康/大华/宇视"，ONVIF Probe 那一次只认得出"这是台 ONVIF 设备"。
             * 原来谁的包后到谁说了算，于是两台海康（型号 `DS-2CD3T27EDWD-L`、
             * `DS-2DC4425DW-D`）的档案被迟到的 ONVIF 应答改写成泛品牌，
             * 网页品牌下拉里又没有 onvif 这一项 ⇒ 自动匹配永远落空。
             * 规则：新结论是泛 ONVIF 而旧结论已是具体厂商 ⇒ 保持旧的；
             * 其余情况照旧覆盖（含"泛 ONVIF → 具体厂商"的升格）。 */
            if (desc->brand != IV_DEV_BRAND_UNKNOWN &&
                !(desc->brand == IV_DEV_BRAND_ONVIF &&
                  d->item[i].brand != IV_DEV_BRAND_UNKNOWN &&
                  d->item[i].brand != IV_DEV_BRAND_ONVIF))
                d->item[i].brand = desc->brand;
            d->dup++;
            return 0;
        }
    }
    if (d->n_items >= IV_DEV_MAX) {
        /* 满了就丢 ⇒ 平台上那台设备"时有时无"，而且**完全不留痕**：2026-10-27 板端
         * 现场 7 台、档案 6 格，被挤掉的是 `.106` 球机，日志里连个影子都没有
         * （`raw[]` 只在入档成功后才打）。这种丢弃必须喊出来，否则下次还是靠猜。 */
        IV_LOG_W(DEV_MOD, "archive full (%d), drop %02x:%02x:%02x:%02x:%02x:%02x",
                 (int)IV_DEV_MAX, desc->mac[0], desc->mac[1], desc->mac[2],
                 desc->mac[3], desc->mac[4], desc->mac[5]);
        return IV_EFULL;
    }
    d->item[d->n_items] = *desc;
    if (d->cfg.on_found != NULL) d->cfg.on_found(&d->item[d->n_items], d->cfg.cb_arg);
    d->n_items++;
    return 1;
}

/* ---------------------------------------------------------------------------
 * 收包
 * ------------------------------------------------------------------------- */
/* 对端 IPv4（网络序）。AF_PACKET 的 sockaddr_ll 没有 IP ⇒ 回 0，由调用方保持原值。
 *
 * 2026-10-27 16:10 板端真帧证据：平台点「搜索设备」，设备回了 5 条档案，
 * `ip1..ip5` **全是空串**（`/api/devices` 同样 ip=""）：
 *   {"num":5,"ip1":"","mac1":"e0:ba:ad:da:ab:dd","bd1":"hikvision","mod1":"46672",…}
 * 平台那张表要按 IP 建行（区域/运维编码/品牌/型号/类型/IP地址/点位描述/状态），
 * IP 空 ⇒ 一行都落不下来 ⇒ 界面永远"暂无数据"、进度永远"已完成 0 个查询"。
 * 根因是海康 UDP 37020 与大华组播的应答体里**没有可解析的 IP 字段**（SADP 有，
 * 所以开机那一轮 4 台是带 IP 的）。而应答方一定是从自己的 IP 发出来的 ——
 * 用 recvfrom 给的对端地址兜底，与品牌/报文格式无关，一次覆盖全部解析器。 */
static void peer_ip_be(const struct sockaddr_storage *from, uint32_t *out) {
    if (from == NULL || out == NULL) return;
    if (from->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)from;
        *out = a->sin_addr.s_addr;
    }
}

/* 解析一包并尝试入档；返回 1=入档/更新，0=丢弃，负码=解析失败。 */
static int handle_packet(iv_device_t *d, const uint8_t *buf, size_t len,
                         const struct sockaddr_storage *from) {
    iv_dev_desc_t desc;
    int rc = 0;
    if (len >= IV_SADP_ETH_HDR_LEN && buf[12] == 0x80 && buf[13] == 0x33) {
        rc = iv_sadp_parse(buf, len, &desc);
    } else if (len > 8 && buf[0] == 0x20 && buf[4] == 'D' && buf[5] == 'H' &&
               buf[6] == 'I' && buf[7] == 'P') {
        rc = iv_dahua_parse(buf, len, &desc);
        if (rc == IV_OK) desc.found_by = d->phase;
    } else {
        rc = iv_onvif_parse((const char *)buf, len, &desc);
    }
    if (rc != IV_OK) {
        d->dup++;
        return 0;
    }
    /* 设备应过答了：本相位不必再补发（内容合不合法是另一回事）。 */
    if (d->phase_hits < 0xFFu) d->phase_hits++;
    /* 报文里没带 IP ⇒ 用发送方地址兜底（SADP 走二层，没有对端 IP，保持报文里的值）。 */
    if (desc.ip_be == 0u) {
        uint32_t pip = 0;
        peer_ip_be(from, &pip);
        if (pip != 0u) desc.ip_be = pip;
    }
    if (desc.ip_be == 0 && mac_is_zero(desc.mac)) {
        d->dup++;
        return 0;
    }
    desc.found_by = d->phase;
    if (add_item(d, &desc) > 0) {
        /* 留一笔原始报文头：海康 UDP 的型号实测是 `46672`/`12345`/`148040` 这种
         * 数字（正文里没有 DeviceType 可取，`extract_after_key()` 抓到了别的字段），
         * 没有原始字节就只能靠猜字段名。只记入档那一包，不刷屏。 */
        char head[97];
        size_t k = len < sizeof(head) - 1u ? len : sizeof(head) - 1u;
        size_t i;
        for (i = 0; i < k; i++) {
            uint8_t c = buf[i];
            head[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        head[k] = '\0';
        IV_LOG_D(DEV_MOD, "raw[%u] %s", (unsigned)len, head);
    }
    return 1;
}

/* 排空 fd 的全部可读报文。返回收包数。 */
static int drain_fd(iv_device_t *d, int fd) {
    int n = 0;
    if (fd < 0) return 0;
    for (;;) {
        struct sockaddr_storage from;
        socklen_t fl = sizeof(from);
        ssize_t r;

        /* 必须清零：`peer_ip_be()` 会读 `ss_family`，而单测注入的 recvfrom
         * 不写这个出参 —— 不清零就是读未初始化栈，家族碰巧等于 AF_INET 时
         * 会给档案塞一个随机 IP，测试随栈内容飘。 */
        memset(&from, 0, sizeof(from));
        r = d->cfg.iops->recvfrom(fd, d->rx, sizeof(d->rx), MSG_DONTWAIT,
                                  (struct sockaddr *)&from, &fl);
        if (r < 0) break; /* EAGAIN 或错误：本轮结束 */
        if (r == 0) break;
        d->rx_pkts++;
        if (handle_packet(d, d->rx, (size_t)r, &from) != 0) n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * 状态机
 * ------------------------------------------------------------------------- */
static iv_dev_phase_t next_phase(iv_dev_phase_t p) {
    switch (p) {
        case IV_DEV_PHASE_IDLE: return IV_DEV_PHASE_HIK_SADP;
        case IV_DEV_PHASE_HIK_SADP: return IV_DEV_PHASE_HIK_UDP;
        case IV_DEV_PHASE_HIK_UDP: return IV_DEV_PHASE_DAHUA_5050;
        case IV_DEV_PHASE_DAHUA_5050: return IV_DEV_PHASE_DAHUA_MCAST;
        case IV_DEV_PHASE_DAHUA_MCAST: return IV_DEV_PHASE_UNV;
        case IV_DEV_PHASE_UNV: return IV_DEV_PHASE_ONVIF;
        default: return IV_DEV_PHASE_DONE;
    }
}

/* 每相位的收包窗口（ms）。实测依据（2026-10-27 16:5x，VM 上复刻同一份探测包）：
 *   - 海康 SADP / UDP：应答 <100 ms；
 *   - **大华：约 300 ms 才回，而且连发探测会被忽略** —— 只发 1 轮时 302 ms 就回；
 *     连发 3 轮（间隔 300 ms）首回应反而要 905 ms。
 * 这正是现场"192.168.1.106 时有时无"的原因：老逻辑窗口＝rounds×round_ms＝3×300
 * ＝900 ms，刚好压在它应答的边界上，抢进窗口就搜到、差一点就漏。
 * ⇒ 大华组播改成"发一次 + 等 1000 ms"，比"连发三次"又快又稳。
 * 顺带把总时长从 9 s 压到 ~3 s（原逻辑每相位都是 3 轮 × 300 ms，而设备根本不看第 2、3 轮）。 */
static uint32_t phase_window_ms(iv_dev_phase_t p) {
    switch (p) {
    case IV_DEV_PHASE_DAHUA_MCAST: return 1000u;
    case IV_DEV_PHASE_HIK_SADP:    return 500u;
    default:                       return 400u;
    }
}

/* 进入阶段并跳过不可用者（无 SADP 权能时跳过 HIK_SADP）。 */
static void enter_phase(iv_device_t *d, iv_dev_phase_t p, uint64_t now_ms) {
    while (p == IV_DEV_PHASE_HIK_SADP && !d->sadp_ok)
        p = next_phase(p);
    d->phase = p;
    d->round = 0;               /* 本相位已发次数（0＝还没发） */
    d->dport_idx = 0;
    d->phase_hits = 0;          /* 本相位收到的应答数（决定要不要补发） */
    /* 注意：next_round_ms 现在的语义是**本相位截止时刻**（不再是"下一轮起点"）。 */
    d->next_round_ms = now_ms + (uint64_t)phase_window_ms(p);
}

/* 构造并发出某 UDP 目的地的一包。 */
static int send_udp(iv_device_t *d, const char *addr, uint16_t port,
                    const void *buf, size_t len) {
    struct sockaddr_in a;
    if (d->fd4 < 0) return IV_ESTATE;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &a.sin_addr) != 1) return IV_EINVAL;
    if (d->cfg.iops->sendto(d->fd4, buf, len, 0, (struct sockaddr *)&a,
                            sizeof(a)) < 0)
        return IV_EIO;
    d->sent++;
    return IV_OK;
}

/* 发 SADP 二层帧（广播）。 */
static int send_sadp(iv_device_t *d) {
    static uint8_t frame[IV_SADP_FRAME_LEN];
    struct sockaddr_ll sll;
    uint8_t ip4[4];
    if (d->fd_pkt < 0) return IV_ESTATE;
    memset(frame, 0xFF, 6); /* 目的 MAC = 广播 */
    if (d->cfg.local_mac[0] || d->cfg.local_mac[1] || d->cfg.local_mac[2] ||
        d->cfg.local_mac[3] || d->cfg.local_mac[4] || d->cfg.local_mac[5])
        memcpy(&frame[6], d->cfg.local_mac, 6);
    frame[12] = 0x80;
    frame[13] = 0x33;
    wr_ip4(ip4, d->cfg.local_ip_be);
    iv_sadp_build_inquiry(&frame[IV_SADP_ETH_HDR_LEN], d->sadp_counter,
                          d->cfg.local_mac, ip4);
    d->sadp_counter++;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(IV_SADP_ETH_TYPE);
    sll.sll_ifindex = (int)if_nametoindex(d->cfg.ifname ? d->cfg.ifname : "");
    sll.sll_halen = 6;
    memset(sll.sll_addr, 0xFF, 6);
    if (d->cfg.iops->sendto(d->fd_pkt, frame, sizeof(frame), 0,
                            (struct sockaddr *)&sll, sizeof(sll)) < 0)
        return IV_EIO;
    d->sent++;
    return IV_OK;
}

/* 发当前阶段的一轮探测。 */
static int send_round(iv_device_t *d) {
    switch (d->phase) {
        case IV_DEV_PHASE_HIK_SADP:
            return send_sadp(d);
        case IV_DEV_PHASE_HIK_UDP: {
            int n = iv_hik_probe_build(d->tx, sizeof(d->tx), NULL);
            if (n < 0) return n;
            return send_udp(d, HIK_MCAST_ADDR, (uint16_t)HIK_UDP_PORT, d->tx,
                            (size_t)n);
        }
        case IV_DEV_PHASE_DAHUA_5050: {
            int n = iv_dahua_probe_build(0, (uint8_t *)d->tx, sizeof(d->tx));
            if (n < 0) return n;
            return send_udp(d, DAHUA_BCAST_ADDR, k_dahua_ports[0], d->tx,
                            (size_t)n);
        }
        case IV_DEV_PHASE_DAHUA_MCAST: {
            int last = IV_OK;
            /* 三个组播端口都发一遍 */
            for (size_t k = 1; k < IV_DAHUA_PORT_N; k++) {
                int n = iv_dahua_probe_build(1, (uint8_t *)d->tx, sizeof(d->tx));
                if (n < 0) return n;
                last = send_udp(d, DAHUA_MCAST_ADDR, k_dahua_ports[k], d->tx,
                                (size_t)n);
            }
            return last;
        }
        case IV_DEV_PHASE_UNV: {
            int n = iv_onvif_probe_build(d->tx, sizeof(d->tx));
            if (n < 0) return n;
            return send_udp(d, UNV_BCAST_ADDR, (uint16_t)UNV_PORT, d->tx,
                            (size_t)n);
        }
        case IV_DEV_PHASE_ONVIF: {
            int n = iv_onvif_probe_build(d->tx, sizeof(d->tx));
            if (n < 0) return n;
            return send_udp(d, ONVIF_MCAST_ADDR, (uint16_t)ONVIF_PORT, d->tx,
                            (size_t)n);
        }
        default:
            return IV_OK;
    }
}

int iv_device_start_search(iv_device_t *d, uint64_t now_ms) {
    if (d == NULL) return IV_EINVAL;
    if (!d->opened) return IV_ESTATE;
    d->searching = 1;
    d->now_ms = now_ms;
    enter_phase(d, IV_DEV_PHASE_HIK_SADP, now_ms);
    IV_LOG_I(DEV_MOD, "search start (%s)", iv_dev_phase_name(d->phase));
    return IV_OK;
}

void iv_device_stop_search(iv_device_t *d) {
    if (d == NULL) return;
    d->searching = 0;
    d->phase = IV_DEV_PHASE_DONE;
}

int iv_device_on_readable(iv_device_t *d, int fd, uint64_t now_ms) {
    if (d == NULL) return IV_EINVAL;
    d->now_ms = now_ms;
    if (fd == d->fd_pkt) return drain_fd(d, d->fd_pkt);
    if (fd == d->fd4) return drain_fd(d, d->fd4);
    return IV_ESTATE;
}

int iv_device_tick(iv_device_t *d, uint64_t now_ms) {
    int n = 0;
    if (d == NULL) return IV_EINVAL;
    if (!d->opened) return IV_ESTATE;
    d->now_ms = now_ms;

    /* 每拍先排空两个 fd（不依赖 Reactor 是否已注册）。 */
    n += drain_fd(d, d->fd_pkt);
    n += drain_fd(d, d->fd4);

    if (!d->searching) return n;

    /* ① 相位刚进入：先发一轮（每个相位至少发一次）。 */
    if (d->round == 0u) {
        (void)send_round(d);
        d->round = 1u;
        return n;
    }
    /* ② 半程仍没收到任何应答，才补发一次 —— 同相位连发只会被设备忽略
     *    （大华实测：连发 3 轮反而比发 1 轮更慢），补发是对"丢包"的兜底，
     *    不是重复轰炸；次数上限仍受 cfg.rounds 约束。 */
    {
        uint32_t half = phase_window_ms(d->phase) / 2u;
        if (d->phase_hits == 0u && d->round < d->cfg.rounds &&
            now_ms + (uint64_t)half >= d->next_round_ms) {
            (void)send_round(d);
            d->round++;
        }
    }
    if (now_ms < d->next_round_ms) return n;

    /* ③ 窗口到点 → 下一相位 */
    {
        iv_dev_phase_t np = next_phase(d->phase);
        if (np == IV_DEV_PHASE_DONE) {
            d->searching = 0;
            d->phase = IV_DEV_PHASE_DONE;
            IV_LOG_I(DEV_MOD, "search done, %d found", d->n_items);
            return n;
        }
        enter_phase(d, np, now_ms);
        IV_LOG_D(DEV_MOD, "phase -> %s", iv_dev_phase_name(d->phase));
        return n;
    }
}

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */
int iv_device_is_open(const iv_device_t *d) { return d ? d->opened : 0; }
int iv_device_is_searching(const iv_device_t *d) {
    return d ? d->searching : 0;
}
iv_dev_phase_t iv_device_phase(const iv_device_t *d) {
    return d ? d->phase : IV_DEV_PHASE_IDLE;
}
int iv_device_count(const iv_device_t *d) { return d ? d->n_items : 0; }
int iv_device_get(const iv_device_t *d, int i, iv_dev_desc_t *out) {
    if (d == NULL || out == NULL) return IV_EINVAL;
    if (i < 0 || i >= d->n_items) return IV_ENOENT;
    *out = d->item[i];
    return IV_OK;
}
int iv_device_fd_count(const iv_device_t *d) {
    int n = 0;
    if (d == NULL) return 0;
    if (d->fd_pkt >= 0) n++;
    if (d->fd4 >= 0) n++;
    return n;
}
int iv_device_fd_at(const iv_device_t *d, int i) {
    if (d == NULL) return -1;
    if (d->fd_pkt >= 0 && i == 0) return d->fd_pkt;
    if (d->fd4 >= 0) {
        int base = (d->fd_pkt >= 0) ? 1 : 0;
        if (i == base) return d->fd4;
    }
    return -1;
}
int iv_device_sadp_available(const iv_device_t *d) { return d ? d->sadp_ok : 0; }
uint64_t iv_device_sent(const iv_device_t *d) { return d ? d->sent : 0; }
uint64_t iv_device_rx_pkts(const iv_device_t *d) { return d ? d->rx_pkts : 0; }
