/*
 * 摄像机信息探测（多品牌发现）（libivmodules，功能开发计划 M4-S4.2）
 *
 * ============================ 它是什么 ============================
 * 在网内**主动发现**摄像机并建档案（品牌 / IP / MAC / 型号），供
 *   - Web「摄像机搜索」与开机一次自动搜索；
 *   - S4.3 SNMP 选 OID 套（按发现到的品牌决定用海康还是大华那套）；
 *   - 平台/Web 展示设备档案。
 *
 * 按品牌协议**顺序**轮询（参考项目 `ONVIF_Search_API` 状态机顺序）：
 *   海康 SADP（二层 EtherType 0x8033）＋ 海康 UDP 探测（239.255.255.250:37020）
 *   → 大华（255.255.255.255:5050 二进制 ＋ 239.255.255.251:{37810,37820,8087}）
 *   → 宇视 UNV（**实际走标准 ONVIF**：255.255.255.255:3702）
 *   → ONVIF WS-Discovery（239.255.255.250:3702）
 *
 * ============================ 与参考项目的关系（口径来源） ============================
 * 报文内容**逐字照抄**参考项目 `General_Version/main`（GD32F460＋FreeRTOS＋lwIP）：
 *   - SADP Inquiry 66 字节体与 16 位反码校验和：`onvif/sadp.c`
 *   - 海康 UDP 探测 XML、大华两套探测帧、ONVIF Probe SOAP：`onvif/onvif.c`
 *   - 响应解析的品牌判定与字段提取：`onvif/onvif.c: onvif_get_ipc_param_info()`
 * 这些是**协议事实**，与 OS 无关。**不搬** lwIP 回调框架 / 任务自删除等单片机机制；
 * 传输层全部改用 Linux 标准 socket（UDP 组播/广播）＋ `AF_PACKET`（SADP 二层）。
 *
 * ============================ 已登记的口径差异（不掩盖） ============================
 *   1. **宇视走标准 ONVIF**：参考项目里宇视私有 Probe 已被注释掉、当前不生效，
 *      UNV 分支实际发送的就是标准 WS-Discovery Probe（目标 `255.255.255.255:3702`）。
 *      本模块照此实现；`UNW` 品牌靠响应里的 `/macaddr/` 关键字回判。
 *   2. **ONVIF 应答按关键字解析**：参考项目**没有**结构化解析 ProbeMatches
 *      （工程内无 `XAddrs`/`ProbeMatches` 命中），而是用关键字匹配（`<MAC>` /
 *      `/macaddr/` / `/MAC/` / `hardware`）回判品牌并抽 MAC/型号。本模块沿用同一
 *      关键字口径，**另加**一条 `XAddrs` 取 IP 的补充路径（更稳，不冲突）。
 *   3. **轮询节奏按参考项目量级拍平**：参考项目每品牌 3 轮、每轮发 1 包等 ~100 ms；
 *      本模块按 tick 推进（同一量级），不逐字复刻 `vTaskDelay` 序列。
 *   4. **SADP 收侧不校验 checksum**：与参考项目一致（收侧只判魔数/类型）。
 *
 * ============================ 传输与权能 ============================
 *   - UDP 组播/广播：普通 `socket(AF_INET, SOCK_DGRAM)` ＋ `IP_ADD_MEMBERSHIP`
 *     / `SO_BROADCAST`；无特权要求；
 *   - SADP：`socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))` 绑本网口，
 *     收 `EtherType = 0x8033` 的帧（需 `CAP_NET_RAW`，与 ICMP 探活同级；
 *     `ivsboxd` 以 root 运行本就具备）。**拿不到权能时该路整体降级**
 *     （`iv_device_sadp_available()` 返 0），其余三路照常。
 *
 * ============================ 并发与生命周期 ============================
 *   - **单写者**：全部接口只在装配层（Reactor 单线程）调用，内部无锁；
 *   - 零 malloc：`iv_device_t` 由调用方持有（约 4 KB，含收包缓冲）；
 *   - 时间一律由调用方以 `now_ms` 传入（单测可注入时间轴）；
 *   - 全部 socket `O_NONBLOCK`：只交 fd（`iv_device_fd_at()`）给调用方挂
 *     Reactor；可读时调 `iv_device_on_readable()`，周期调 `iv_device_tick()`。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK / IV_EINVAL / IV_ESTATE（未 open 就 tick / 重复 open）/ IV_EFULL（档案满）
 */
#ifndef IVSBOX_IV_DEVICE_H
#define IVSBOX_IV_DEVICE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h> /* socklen_t */

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量
 * ------------------------------------------------------------------------- */

/* 档案上限（现网 `camera[6]` / `IPC_NUM_MAX` 量级；本产品取 6）。 */
#define IV_DEV_MAX 6

/* 型号 / 序列号字符串长度（含 NUL）。 */
#define IV_DEV_MODEL_MAX 32
#define IV_DEV_SERIAL_MAX 32

/* SADP 报文长度（负载体）。 */
#define IV_SADP_INQUIRY_LEN 66u
#define IV_SADP_ETH_HDR_LEN 14u
#define IV_SADP_FRAME_LEN (IV_SADP_ETH_HDR_LEN + IV_SADP_INQUIRY_LEN)

/* EtherType SADP。 */
#define IV_SADP_ETH_TYPE 0x8033u

/* 各品牌探测报文缓冲上限（最长的 ONVIF Probe 约 700 字节）。 */
#define IV_DEV_PROBE_MAX 1024u

/* 收包缓冲上限（ONVIF ProbeMatches 可能上千字节，取 2 KB）。 */
#define IV_DEV_RX_MAX 2048u

/* ---------------------------------------------------------------------------
 * 枚举
 * ------------------------------------------------------------------------- */

/* 品牌。数值稳定，可落库。 */
typedef enum {
    IV_DEV_BRAND_UNKNOWN = 0,
    IV_DEV_BRAND_HIKVISION = 1, /* 海康 */
    IV_DEV_BRAND_DAHUA = 2,     /* 大华 */
    IV_DEV_BRAND_UNV = 3,       /* 宇视 */
    IV_DEV_BRAND_ONVIF = 4      /* 泛 ONVIF（未识别到具体厂商） */
} iv_dev_brand_t;

/* 发现阶段（顺序即枚举序）。 */
typedef enum {
    IV_DEV_PHASE_IDLE = 0,
    IV_DEV_PHASE_HIK_SADP,   /* 海康二层 SADP */
    IV_DEV_PHASE_HIK_UDP,    /* 海康 UDP 37020 */
    IV_DEV_PHASE_DAHUA_5050, /* 大华 5050 广播 */
    IV_DEV_PHASE_DAHUA_MCAST,/* 大华 239.255.255.251 三端口 */
    IV_DEV_PHASE_UNV,        /* 宇视（标准 ONVIF，255.255.255.255:3702） */
    IV_DEV_PHASE_ONVIF,      /* ONVIF WS-Discovery 组播 */
    IV_DEV_PHASE_DONE
} iv_dev_phase_t;

/* ---------------------------------------------------------------------------
 * 档案条目
 * ------------------------------------------------------------------------- */

typedef struct {
    iv_dev_brand_t brand;
    uint32_t       ip_be;                /* 网络序 IPv4；0=未知 */
    uint8_t        mac[6];
    char           model[IV_DEV_MODEL_MAX];
    char           serial[IV_DEV_SERIAL_MAX];
    uint16_t       port;                 /* 访问端口（ONVIF 默认 80） */
    iv_dev_phase_t found_by;             /* 从哪个阶段发现（诊断） */
} iv_dev_desc_t;

/* ---------------------------------------------------------------------------
 * I/O 注入点（单测用；生产走 iv_device_default_iops()）
 * ------------------------------------------------------------------------- */
typedef struct {
    int      (*socket)(int domain, int type, int proto);
    int      (*bind)(int fd, const struct sockaddr *addr, socklen_t len);
    int      (*setsockopt)(int fd, int level, int optname, const void *optval,
                           socklen_t optlen);
    ssize_t  (*sendto)(int fd, const void *buf, size_t len, int flags,
                       const struct sockaddr *addr, socklen_t alen);
    ssize_t  (*recvfrom)(int fd, void *buf, size_t len, int flags,
                         struct sockaddr *addr, socklen_t *alen);
    int      (*close)(int fd);
    uint64_t (*now_ms)(void); /* 默认 iv_clock_monotonic_ms() */
} iv_device_iops_t;

/* 默认注入点：真 libc socket/bind/setsockopt/sendto/recvfrom/close ＋ iv_clock。
 * 永不返回 NULL。 */
const iv_device_iops_t *iv_device_default_iops(void);

/* ---------------------------------------------------------------------------
 * 配置
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *ifname;     /* SADP 二层收发绑定的网口名（如 eth0）；NULL → 无 SADP */
    uint32_t    local_ip_be;/* 本机 IP（SADP Inquiry 字段）；0 → 0.0.0.0 */
    uint8_t     local_mac[6]; /* 本机 MAC（SADP 字段与 EUI-64）；全 0 → 随机/占位 */
    uint32_t    round_ms;   /* 每阶段单轮等待（默认 300ms）；0 → 默认 */
    uint32_t    rounds;     /* 每阶段轮数（默认 3）；0 → 默认 */
    const iv_device_iops_t *iops; /* NULL → 默认 */

    /* 发现回调（每新增/更新一条档案调一次，可 NULL）。 */
    void (*on_found)(const iv_dev_desc_t *d, void *arg);
    void *cb_arg;
} iv_device_cfg_t;

/* 填默认：ifname=NULL / ip=0 / mac 全 0 / round=300ms / rounds=3 / iops=NULL。 */
void iv_device_cfg_default(iv_device_cfg_t *cfg);

/* ---------------------------------------------------------------------------
 * 纯报文函数（无 I/O，单测直接钉字节）
 * ------------------------------------------------------------------------- */

/* SADP 16 位反码校验和（参考 `sadp_checksum_safe`，按 LE uint16 累加）。
 * prefix 为待校验字节数（Inquiry 固定 66）。*/
uint16_t iv_sadp_checksum(const uint8_t *body, uint32_t prefix);

/* 构造 SADP Inquiry **负载体**（66 字节）。counter 初值建议 0x1000。返回长度。 */
int iv_sadp_build_inquiry(uint8_t out[IV_SADP_INQUIRY_LEN], uint16_t counter,
                          const uint8_t mac[6], const uint8_t ip[4]);

/* 解析一帧完整的以太网帧（14 字节头 ＋ SADP 体）。成功返回 IV_OK 并填 *out
 * （brand=HIKVISION，ip/mac/model/serial），非 SADP/非法返回负码。 */
int iv_sadp_parse(const uint8_t *eth_frame, size_t len, iv_dev_desc_t *out);

/* 构造海康 UDP 探测文本（`<Probe><Uuid>..</Uuid><Types>inquiry</Types></Probe>`,
 * 含 XML 声明）。uuid 为 36 字符 UUID 串；为 NULL 用固定占位。返回写入长度。 */
int iv_hik_probe_build(char *out, size_t cap, const char *uuid);

/* 构造大华探测帧。kind=0 → 5050 广播 32 字节二进制；kind=1 → 组播 105 字节。
 * 返回写入长度，或负码（cap 不足）。 */
int iv_dahua_probe_build(int kind, uint8_t *out, size_t cap);

/* 解析大华响应（跳过 32 字节 DHIP 头后按 JSON 关键字抽 MAC/型号）。 */
int iv_dahua_parse(const uint8_t *buf, size_t len, iv_dev_desc_t *out);

/* 构造 ONVIF WS-Discovery Probe SOAP 文本（固定报文，逐字照抄）。返回长度。 */
int iv_onvif_probe_build(char *out, size_t cap);

/* 关键字解析 ONVIF/海康UDP/宇视应答：按响应内容回判品牌并抽 MAC/型号/IP。
 * 返回 IV_OK 表示识别成功（brand 非 UNKNOWN）。 */
int iv_onvif_parse(const char *buf, size_t len, iv_dev_desc_t *out);

/* 厂商品牌名（诊断/日志/落库用，永不返回 NULL）。 */
const char *iv_dev_brand_name(iv_dev_brand_t b);
const char *iv_dev_phase_name(iv_dev_phase_t p);

/* 大华组播端口表（只读，4 项：5050/37810/37820/8087）。 */
const uint16_t *iv_dahua_port_table(void);
#define IV_DAHUA_PORT_N 4u

/* ---------------------------------------------------------------------------
 * 句柄（内部字段直接暴露，符合本工程零 malloc 风格；调用方只读）
 * ------------------------------------------------------------------------- */
typedef struct {
    iv_device_cfg_t cfg;

    iv_dev_desc_t   item[IV_DEV_MAX];
    int             n_items;

    iv_dev_phase_t  phase;
    uint32_t        round;        /* 本相位已发次数（0＝还没发） */
    uint8_t         phase_hits;   /* 本相位收到的应答数（0 ⇒ 半程补发一次） */
    int             fd4;          /* UDP v4 socket，-1=无 */
    int             fd_pkt;       /* AF_PACKET socket，-1=无 */
    int             sadp_ok;      /* AF_PACKET 是否可用 */
    uint16_t        sadp_counter; /* SADP 事务计数（初值 0x1000） */
    uint64_t        next_rx_ms;   /* 本阶段本轮收包窗口截止 */
    uint64_t        next_round_ms;/* **本相位截止时刻**（2026-10-27 由"下一轮起点"改为时间窗驱动） */
    uint32_t        dport_idx;    /* 大华组播：当前端口下标 */
    char            tx[IV_DEV_PROBE_MAX];
    size_t          tx_len;
    uint8_t         rx[IV_DEV_RX_MAX];

    int             searching;
    int             opened;
    uint64_t        now_ms;
    uint64_t        sent;         /* 累计发包数（诊断） */
    uint64_t        rx_pkts;      /* 累计收包数（诊断） */
    uint64_t        dup;          /* 重复/无效应答计数（诊断） */
} iv_device_t;

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

/* 初始化：清零 + 档案清空 + fd=-1 + phase=IDLE。cfg 为 NULL 取默认。 */
void iv_device_init(iv_device_t *d, const iv_device_cfg_t *cfg);

/* 启动：建 UDP socket（必需）与 AF_PACKET socket（可选，失败降级）。 */
int iv_device_open(iv_device_t *d, const iv_device_cfg_t *cfg, uint64_t now_ms);

/* 关闭：停搜索、关全部 socket。幂等。 */
void iv_device_close(iv_device_t *d);

/* 清空档案（Web「重新搜索」前调用）。 */
void iv_device_clear(iv_device_t *d);

/* ---------------------------------------------------------------------------
 * 驱动
 * ------------------------------------------------------------------------- */

/* 开始一轮搜索（从海康 SADP 起）。已在搜索中则忽略。 */
int iv_device_start_search(iv_device_t *d, uint64_t now_ms);

/* 停止搜索（保留已发现档案）。 */
void iv_device_stop_search(iv_device_t *d);

/* fd 可读：把该 fd 的应答收干并解析入档。返回本次收包数（>=0）。 */
int iv_device_on_readable(iv_device_t *d, int fd, uint64_t now_ms);

/* 周期驱动（装配层 200 ms）：推进阶段/轮次，发出到期探测，排空收包。 */
int iv_device_tick(iv_device_t *d, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */
int                 iv_device_is_open(const iv_device_t *d);
int                 iv_device_is_searching(const iv_device_t *d);
iv_dev_phase_t      iv_device_phase(const iv_device_t *d);
int                 iv_device_count(const iv_device_t *d);
int                 iv_device_get(const iv_device_t *d, int i, iv_dev_desc_t *out);
int                 iv_device_fd_count(const iv_device_t *d);
int                 iv_device_fd_at(const iv_device_t *d, int i);
int                 iv_device_sadp_available(const iv_device_t *d);
uint64_t            iv_device_sent(const iv_device_t *d);
uint64_t            iv_device_rx_pkts(const iv_device_t *d);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_DEVICE_H */
