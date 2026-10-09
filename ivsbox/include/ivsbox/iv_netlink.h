/*
 * IVSBox rtnetlink 网络事件监听（libivhal，功能开发计划 M3-S3.1）
 *
 * ============================ 它是什么 ============================
 * 链路 / 地址 / 路由变化的**内核事件源**：订阅 NETLINK_ROUTE 组播组，
 * 把内核推来的 rtnetlink 报文解析成三类事件交给上层（S3.2 探活引擎、
 * S3.3 双 WAN 状态机都以此为链路层判据）：
 *
 *   - carrier up/down（RTM_NEWLINK/DELLINK，含内核原始 flags）；
 *   - IPv4 地址增删（RTM_NEWADDR/DELADDR）；
 *   - IPv4 默认路由增删（RTM_NEWROUTE/DELROUTE，dst_len==0）。
 *
 * **它不理解策略**：不判"哪个 WAN 该用"、不碰路由表、不发任何 netlink
 * 请求（无 dump、无 set）—— 那些是 netmgr（S3.3）的事。本模块只把内核
 * 已经发生的事实翻译成事件。
 *
 * ============================ 为什么在 hal 层 ============================
 * 只依赖 Linux 的 `socket(AF_NETLINK)` 与 rtnetlink 报文格式，不含任何
 * 业务策略，因此落 `src/hardware/`、进 `libivhal.a`（架构 §15.5 依赖方向：
 * `libivcore` -> `libivhal` -> `libivmodules`）。与本层其它模块同一条纪律：
 * **不引入 pthread**，也**不引用 iv_reactor** —— 只交出 fd
 * （`iv_netlink_fd()`），由调用方注册进自己的事件循环；socket 已设
 * `O_NONBLOCK`，可读时调 `iv_netlink_poll()` 拉干即可。
 *
 * ============================ 计划口径与两条取舍 ============================
 * 计划 §S3.1 原文："订阅 RTMGRP_LINK/IPv4_IFADDR/IPv4_ROUTE；解析事件挂
 * Reactor；对外回调 carrier up/down、地址增删、默认路由变更。禁止轮询
 * 解析 ifconfig。" 本实现与其完全一致，另有两条明确取舍：
 *
 * 1) **carrier 判据 = `IFF_UP && IFF_LOWER_UP`**（不是 IFF_RUNNING）。
 *    理由：dummy / loopback 等虚拟接口的 operstate 是 UNKNOWN，
 *    `IFF_RUNNING` 在其上不稳定；而 `IFF_LOWER_UP` 语义是"有物理承载"：
 *    `ip link set eth0 down` 清 `IFF_UP`、拔线清 `IFF_LOWER_UP`，两种
 *    故障都能被它抓到，且对 dummy/lo 天然成立（VM 联调不误报）。
 *    内核原始 flags 全量放在 `link_flags` 里，上层想用 RUNNING 自行取。
 *    （`IFF_LOWER_UP` 是内核私有位，glibc `<net/if.h>` 不定义，实现里
 *    有 `#ifndef` 兜底定义 0x40000。）
 *
 * 2) **LINK 事件带去重状态（初见即报，此后仅变化才报）**。
 *    内核对 qdisc / MTU 等任何链路属性变化都会重发 RTM_NEWLINK（flags
 *    不变）。不去重会把"噪音"当成事件灌给 S3.3 状态机；去重后第一个
 *    NEWLINK 天然充当**初始状态同步**（netlink 订阅不回放现状，初见即报
 *    正是"开机摸底"的实现方式）。代价是本模块持有一张 16 槽接口表
 *    （仅 ifindex + 1 bit，零 malloc）。地址 / 路由事件**不去重**——
 *    同接口多地址、同目的多指标默认路由都是合法并存，上层必须全见。
 *
 * ============================ 溢出语义（必须记住） ============================
 * netlink 是通知不是队列：突发（路由风暴、容器增删）会**丢消息**。两条
 * 通路都会被翻译成 `IV_EFULL`：
 *   - `recvmsg()` 返回 `ENOBUFS`（内核接收队列溢出）；
 *   - 报文流里的 `NLMSG_ERROR` 且 `error == -ENOBUFS`（内核显式告知丢弃）。
 * 收到 `IV_EFULL` 时，本轮已经回调过的事件仍然有效，但**此前可能有事件
 * 已丢失** —— 调用方（S3.3）必须做一次全量状态重同步（重新 dump 接口 /
 * 地址 / 路由），不能当无事发生。返回 `IV_EFULL` 时 errno 保留 ENOBUFS。
 *
 * ============================ 测试与重放 ============================
 * `iv_netlink_feed()` 把一段**原始 rtnetlink 字节**喂进解析器，与
 * `iv_netlink_poll()` 走完全同一条解析路径 —— 单测靠它做字节级注入
 * （M2 已验证的口径：字节级注入单测 ＋ 真实内核事件联调），不开 socket
 * 也能驱动全部解析逻辑。
 *
 * ============================ 并发与生命周期 ============================
 *   - 单写者：全部接口只在装配层的一个线程内调用（与 Reactor 单线程模型
 *     一致），内部无锁；
 *   - 事件指针只在回调执行期间有效，需留存由调用方拷贝；
 *   - 零 malloc：iv_netlink_t 由调用方持有（约 300 B）。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK      成功（poll/feed 返回值另有语义，见各自注释）
 *   IV_EINVAL  参数非法（NULL / on_evt 为空）
 *   IV_ESTATE  状态不允许（未 attach 就 poll / 重复 attach / 重复 close）
 *   IV_EAUTH   socket 被拒（EACCES/EPERM，理论上 NETLINK_ROUTE 不需要特权）
 *   IV_ENOMEM  socket 资源不足（EMFILE/ENFILE/ENOMEM）
 *   IV_EFULL   内核事件队列溢出，必须全量重同步（见上）
 *   IV_EIO     其它系统调用失败；**errno 保留原值**便于诊断
 *   poll/feed 的成功返回值 = 本次交付的事件数（>= 0）。
 */
#ifndef IVSBOX_IV_NETLINK_H
#define IVSBOX_IV_NETLINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量（编译期定死，风格同 iv_link：能力可配、默认值不留现场改）
 * ------------------------------------------------------------------------- */

/* 接口去重表槽数。T113 板上物理接口 < 5（eth0 / 4G WAN / lo / 摄像机侧口），
 * 16 槽余量充足。表满后的降级行为：新接口不再入表 => 其 LINK 事件每次都报
 * （同 flags 重复报），事件不丢只是有噪音，比"静默不报"安全。 */
#define IV_NETLINK_IFACE_MAX 16

/* poll() 的单个 netlink 报文缓冲（栈上，零 malloc）。iproute2 同量级（8K）；
 * 单条 rtnetlink 报文通常 < 1 KB，超长报文按 MSG_TRUNC 丢弃（见 iv_netlink.c）。*/
#define IV_NETLINK_BUF_SIZE 8192u

/* 接口名长度（含结尾 NUL），与内核 IF_NAMESIZE 一致；头文件不引入 <net/if.h> */
#define IV_NETLINK_IFNAME_LEN 16u

/* ---------------------------------------------------------------------------
 * 事件
 * ------------------------------------------------------------------------- */

typedef enum {
    IV_NLEVT_LINK_UP = 1,  /* 接口 carrier 就绪（初见即报；此后仅变化才报） */
    IV_NLEVT_LINK_DOWN,    /* carrier 丢失 / 接口被移除（RTM_DELLINK 同样报 DOWN） */
    IV_NLEVT_ADDR_ADD,     /* IPv4 地址新增 */
    IV_NLEVT_ADDR_DEL,     /* IPv4 地址删除 */
    IV_NLEVT_ROUTE_ADD,    /* IPv4 默认路由新增（dst_len==0 且 type==UNICAST） */
    IV_NLEVT_ROUTE_DEL     /* IPv4 默认路由删除 */
} iv_netlink_kind_t;

typedef struct {
    iv_netlink_kind_t kind;
    int      ifindex;    /* LINK/ADDR：接口自身；ROUTE：出接口 RTA_OIF（无则 0） */
    char     ifname[IV_NETLINK_IFNAME_LEN]; /* **仅 LINK 事件填**，其余全 0 */
    uint32_t link_flags; /* **仅 LINK**：内核原始 ifi_flags（IFF_UP/RUNNING/LOWER_UP…） */
    uint8_t  family;     /* ADDR/ROUTE：恒 AF_INET（本模块只订阅 IPv4 组） */
    uint8_t  prefixlen;  /* ADDR：前缀长度；ROUTE：恒 0（默认路由 dst_len==0） */
    uint8_t  addr[4];    /* ADDR：IPv4 地址（网络序原样）；ROUTE_ADD：网关（无网关全 0） */
    uint32_t metric;     /* **仅 ROUTE**：RTA_PRIORITY（无则 0）；同目的多默认路由按它分主备 */
    uint32_t table;      /* **仅 ROUTE**：RTA_TABLE（缺省回落 rtm_table）；策略路由 S3.3 要用 */
} iv_netlink_evt_t;

/* 事件回调。evt 只在回调执行期间有效；在回调里调用本模块其它接口是安全的。 */
typedef void (*iv_netlink_cb)(const iv_netlink_evt_t *evt, void *arg);

/* ---------------------------------------------------------------------------
 * 解析器（零 malloc：调用方持有，风格同 iv_link_t）
 * ------------------------------------------------------------------------- */
typedef struct iv_netlink iv_netlink_t;

struct iv_netlink {
    iv_netlink_cb on_evt;
    void         *arg;
    int           fd; /* >= 0 = 已 attach；-1 = 未 attach（feed 注入模式） */
    struct {
        int     ifindex; /* 0 = 空槽（内核接口 ifindex 恒 > 0） */
        uint8_t up;      /* 上次播报的 carrier 状态（去重用） */
    } seen[IV_NETLINK_IFACE_MAX];
    int evt_count; /* 内部：本轮 feed/poll 已交付事件数（随返回值交出，调用方勿用） */
};

/* 初始化。on_evt==NULL 报 IV_EINVAL（没有出口的事件源无意义）。
 * 不开 socket：要收真实内核事件再调 iv_netlink_attach()；纯注入场景
 * （单测 / 重放）init 后直接 feed。 */
int iv_netlink_init(iv_netlink_t *nl, iv_netlink_cb on_evt, void *arg);

/* 忘记全部已见接口：下一个 NEWLINK 重新按"初见"上报（全量重同步后调用；
 * 单测复位用）。不动 fd 与回调。nl==NULL 直接返回。 */
void iv_netlink_reset(iv_netlink_t *nl);

/* 打开 NETLINK_ROUTE socket 并订阅 RTMGRP_LINK / IPV4_IFADDR / IPV4_ROUTE，
 * 置 `O_NONBLOCK | O_CLOEXEC`。已 attach（fd>=0）再调返回 IV_ESTATE。
 * 成功后清空去重表（新订阅 = 新起点，初见即报充当初始同步）。 */
int iv_netlink_attach(iv_netlink_t *nl);

/* 给 Reactor 注册用的 fd；未 attach 返回 -1。fd 所有权归本模块，
 * 调用方只读、只把它挂进事件循环，**不要自行 close**。 */
int iv_netlink_fd(const iv_netlink_t *nl);

/* 拉干 socket：recvmsg 循环直到 EAGAIN，逐条报文走与 feed 相同的解析。
 * 返回 = 交付的事件数（>= 0）；IV_EFULL = 溢出须重同步（事件可能已部分
 * 交付）；IV_ESTATE = 未 attach；其余 IV_* 见文件头。 */
int iv_netlink_poll(iv_netlink_t *nl);

/* 字节级注入：把一段原始 rtnetlink 报文（可含多条消息）喂进解析器。
 * 与 poll() 同一条解析路径；返回 = 交付的事件数，IV_EFULL = 见上。
 * len==0 返回 0（无意义的调用不算错，便于循环喂包的调用方省分支）。 */
int iv_netlink_feed(iv_netlink_t *nl, const void *buf, size_t len);

/* 关闭 socket。成功后 fd 复位为 -1，可再次 attach。
 * 未 attach（fd<0）时调用返回 IV_ESTATE —— 重复关闭是调用方 bug，
 * 不掩盖（同 iv_serial_close 的口径）。nl==NULL 返回 IV_EINVAL。 */
int iv_netlink_close(iv_netlink_t *nl);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_NETLINK_H */
