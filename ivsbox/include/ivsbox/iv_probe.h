/*
 * IVSBox 统一探活引擎（libivmodules，功能开发计划 M3-S3.2）
 *
 * ============================ 它是什么 ============================
 * 主网 / 双 WAN / 摄像机共用的**四层探活与统计判定**引擎。上层（S3.3 双 WAN
 * 状态机、M4 设备管理、recovery）不管探测细节，只问这一个模块"某目标现在
 * 可用不可用、为什么、质量多差"。
 *
 * 四层语义（层与层不是"都必须过"，而是**按需组合、逐层降级**）：
 *   - LINK  链路层：**不主动发探测**，被动接收 S3.1 `iv_netlink` 的 carrier
 *           up/down（`iv_probe_set_link()`）。链路一断，绑定该接口的目标**立即**
 *           判 DOWN（不等计数）——拔网线要秒级可见。
 *   - ICMP  网络层：raw socket ICMP echo（需 `CAP_NET_RAW`），RTT 用回包时间算。
 *           **探测报文按项的 `ifindex` 指定出口网卡**（`IP_PKTINFO.ipi_ifindex`）。
 *           不这么做的话，两条 WAN 的探测都会走系统默认路由，判出来的根本不是
 *           各自的通断 —— 见 `struct iv_probe` 里 `icmp_bind_ok` 的说明。
 *   - TCP   传输层：非阻塞 connect 到 `addr:port`，**connect 完成即断开**（不做
 *           任何应用层交互，只验证"路通、端口开"）。
 *   - APP   应用层：**引擎不探测**，由上层（RTSP OPTIONS / SNMP GET / 心跳）
 *           自己做完，把结果经 `iv_probe_note_result()` 喂进来。
 *
 * **ICMP 被禁（无 CAP 或被墙）不判死**：raw socket 开不出来（EPERM/EACCES）或
 * 发送被拒时，引擎把 ICMP 标记为不可用（`iv_probe_icmp_available()` 返 0），
 * 该层不参与判定、**不计失败**；只要 TCP / APP 层报成功，目标仍判可用。
 * 这是计划 §S3.2 第 3 条的硬要求。
 *
 * ============================ 统一输出（计划 §S3.2 第 2 条） ============================
 * `iv_probe_stat_t` 是给上层的唯一读数：状态、原因码、最近 RTT、滑动平均 RTT、
 * 丢包率（千分比）、连续成功/失败次数、最近成功时刻、累计收发。上层**不要**
 * 自己再维护一套统计。
 *
 * ============================ 为什么在 libivmodules ============================
 * 用到 socket（libc）与 `iv_clock`（libivhal），按架构 §15.5 依赖方向落在
 * `libivmodules.a`（`core -> hal -> modules`）。与本层其它模块同一条纪律：
 * **不引入 pthread、不引用 iv_reactor** —— 只交出 fd，由调用方挂进自己的事件
 * 循环；所有 socket 都是 `O_NONBLOCK`，可读/可写时分别调
 * `iv_probe_on_readable()` / `iv_probe_on_writable()`，周期性调
 * `iv_probe_tick()` 发起到期探测并做超时判定。
 *
 * ============================ 调度与超时 ============================
 * 每个主动层（ICMP/TCP）的项有 `interval_ms`（探测间隔）与 `timeout_ms`（单次
 * 超时）。`iv_probe_tick(now)` 做两件事：
 *   1. 发起到期且不在探测中的项；
 *   2. 把已发起但 `now - sent_at > timeout_ms` 的项判为超时失败。
 * 因此 tick 必须被**周期性**调用（建议 100~500 ms 一次），间隔与超时都以调用方
 * 传入的 `now_ms` 为准——引擎**不自己取时**（便于单测注入时间轴）。
 *
 * ============================ 状态判定 ============================
 *   - 未探测过            => UNKNOWN；
 *   - 绑定接口的链路 down => DOWN（reason=LINK_DOWN），立即，不等计数；
 *   - 连续失败 >= fail_n  => DOWN；
 *   - 连续成功 >= ok_n    => UP；
 *   - 判 UP 但 avg RTT / 丢包率超阈值 => DEGRADED（可用但劣化）。
 * fail_n / ok_n / 劣化阈值都在 `iv_probe_cfg_t`，可配；默认见 `iv_probe_cfg_default()`。
 *
 * ============================ 并发与生命周期 ============================
 *   - 单写者：全部接口只在装配层的一个线程内调用（与 Reactor 单线程模型一致），
 *     内部无锁；
 *   - 零 malloc：`iv_probe_t` 由调用方持有（约 1 KB）；活动 socket 由引擎按需
 *     创建/关闭，句柄只经 `iv_probe_fd_at()` 交出，调用方只读、**不要自行 close**；
 *   - `iv_probe_stat_t` 是值拷贝，取出后不与引擎共享内存。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK      成功
 *   IV_EINVAL  参数非法（NULL / 未 init / idx 越界）
 *   IV_ESTATE  状态不允许（重复 start / 未 start 就 tick / fd 不认识）
 *   IV_EFULL   项表或链路表已满
 *   IV_EEXIST  同一 (层, 目标) 重复添加
 *   IV_ECONN   传输层连接失败（仅出现在探测结果归纳，不改函数返回码）
 *   IV_ENOTSUP ICMP 层不可用（`iv_probe_icmp_available()` 返 0 时的显式查询）
 *   IV_EIO     其它系统调用失败；**errno 保留原值**
 */
#ifndef IVSBOX_IV_PROBE_H
#define IVSBOX_IV_PROBE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h> /* socklen_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量（编译期定死，风格同 iv_link / iv_netlink）
 * ------------------------------------------------------------------------- */

/* 项表上限。主网 + 双 WAN + 若干摄像机目标，8 项够用；满则 add 返回 IV_EFULL，
 * 不扩容、不排队。 */
#define IV_PROBE_MAX_ITEMS 8

/* 链路表上限。T113 板上物理接口 < 5，8 槽余量充足；满则新接口不入表（该接口
 * 只按"未知"处理，不误判 DOWN）。 */
#define IV_PROBE_MAX_LINKS 8

/* 统计滑窗长度（最近 N 次探测）。RTT 滑动平均与丢包率都在此窗上算。 */
#define IV_PROBE_WIN_LEN 16

/* 「一轮」＝连发几个包（2026-10-29 用户口径）。
 *
 * 原来的语义是"每一个包就是一个结论"：单包丢了立刻算一次失败，于是现场一个包
 * 被交换机丢在队列里，界面就闪一下"不通"。用户要的是「每 ping 一次发 4 个包，
 * 判断是否全部响应，算 4 个包的平均响应时间」——
 *   4 包全响应 且 平均 ≤ 门限 → 正常
 *   4 包全响应 且 平均 >  门限 → 延时
 *   部分响应                  → 丢包
 *   全无响应                  → 故障
 *
 * 与「轮间隔」的关系：配置键 `probe.interval_ms` 指的是**轮间隔**，引擎内部单包
 * 间隔由装配层换算成 `轮间隔 / IV_PROBE_ROUND_SAMPLES`（见 `iv_agent` 的
 * `probe_cfg_of()`）—— 一个参数管两件事，不让人在两个地方各调一个数。 */
#define IV_PROBE_ROUND_SAMPLES 4u

/* 项名长度（含 NUL）。仅用于日志/诊断，引擎不解析其内容。 */
#define IV_PROBE_NAME_LEN 16

/* ICMP echo 载荷字节数（含 8 字节时间戳余量）。 */
#define IV_PROBE_ICMP_PAYLOAD 32

/* ---------------------------------------------------------------------------
 * 枚举：层 / 状态 / 原因码
 * ------------------------------------------------------------------------- */

typedef enum {
    IV_PROBE_LINK = 0, /* 链路层：被动，接 S3.1 事件 */
    IV_PROBE_ICMP,     /* 网络层：raw ICMP echo */
    IV_PROBE_TCP,      /* 传输层：非阻塞 connect 完成即断开 */
    IV_PROBE_APP       /* 应用层：上层注入结果 */
} iv_probe_layer_t;

typedef enum {
    IV_PROBE_UNKNOWN = 0, /* 还没探测过（或 ICMP 层被禁且无其它层结论） */
    IV_PROBE_UP,          /* 可用 */
    IV_PROBE_DOWN,        /* 不可用 */
    IV_PROBE_DEGRADED     /* 可用但劣化（丢包 / 高 RTT 超阈值） */
} iv_probe_state_t;

typedef enum {
    IV_PROBE_R_NONE = 0,  /* 无（未探测） */
    IV_PROBE_R_OK,        /* 最近一次探测成功 */
    IV_PROBE_R_TIMEOUT,   /* 探测超时 */
    IV_PROBE_R_REFUSED,   /* 连接被拒（ECONNREFUSED） */
    IV_PROBE_R_UNREACH,   /* 不可达（ENETUNREACH/EHOSTUNREACH） */
    IV_PROBE_R_NOPERM,    /* 无权限做本层探测（raw socket EPERM/EACCES） */
    IV_PROBE_R_LINK_DOWN, /* 链路层判死（绑定接口 carrier down） */
    IV_PROBE_R_APP_FAIL,  /* 应用层返回失败 */
    IV_PROBE_R_IOERR      /* 其它 I/O 错误 */
} iv_probe_reason_t;

/* ---------------------------------------------------------------------------
 * 统一输出结构（计划 §S3.2 第 2 条）
 * ------------------------------------------------------------------------- */
typedef struct {
    iv_probe_state_t  state;
    iv_probe_reason_t reason;
    uint32_t          last_rtt_ms;      /* 最近一次成功探测的 RTT */
    uint32_t          avg_rtt_ms;       /* 滑窗内成功探测的平均 RTT；无成功样本为 0 */
    uint32_t          loss_permille;    /* 滑窗丢包率，0~1000 */
    uint32_t          ok_streak;        /* 连续成功次数 */
    uint32_t          fail_streak;      /* 连续失败次数 */
    uint64_t          last_ok_ms;       /* 最近成功时刻（调用方时钟，0=从未） */
    uint32_t          sent;             /* 累计完成探测次数（不含 LINK） */
    uint32_t          lost;             /* 累计失败次数 */

    /* ---- 轮统计（最近一个 IV_PROBE_ROUND_SAMPLES 包的结算）----
     *
     * 上层的四态判定（通/不通/延时/丢包）只看这四个字段，不要自己再拿滑窗去凑：
     * `avg_rtt_ms` 是 16 窗平均，抖动被摊平了，"本轮 4 包平均超门限"在它上面
     * 永远触发得晚一拍；而"4 包里丢了 1 个"在它上面也算不出丢包。 */
    uint32_t          round_ok;          /* 最近一轮成功包数（0..ROUND_SAMPLES） */
    uint32_t          round_lost;        /* 最近一轮失败包数 */
    uint32_t          round_avg_rtt_ms;  /* 最近一轮成功包的平均 RTT；全失败为 0 */
    uint32_t          round_seq;         /* 已结算轮数，0=还没跑完第一轮 */
} iv_probe_stat_t;

/* ---------------------------------------------------------------------------
 * I/O 注入点（单测用；生产走 iv_probe_default_iops()）
 * 全部 socket 操作都经它转发，单测可替换以覆盖 EPERM / 超时 / REFUSED 等分支，
 * 不依赖真实网络与 CAP_NET_RAW。
 * ------------------------------------------------------------------------- */
typedef struct {
    int      (*socket)(int domain, int type, int proto);
    int      (*connect)(int fd, const struct sockaddr *addr, socklen_t len);
    ssize_t  (*sendto)(int fd, const void *buf, size_t len, int flags,
                       const struct sockaddr *addr, socklen_t alen);
    ssize_t  (*recv)(int fd, void *buf, size_t len, int flags);
    int      (*getsockopt)(int fd, int level, int optname, void *optval,
                           socklen_t *optlen);
    int      (*close)(int fd);
    uint64_t (*now_ms)(void); /* 默认 iv_clock_monotonic_ms() */

    /* 「按出口网卡发 ICMP」这一能力的两个可选钩子（2026-10-29 加）。
     * **任一为 NULL 就退回 `sendto()`（不绑出口）**，行为与加这两个钩子之前完全
     * 一致 —— 单测里显式给 NULL 即可保持老路径。生产默认注入点两个都提供，见
     * `iv_probe_default_iops()`。为什么用 sendmsg+IP_PKTINFO 而不是
     * SO_BINDTODEVICE，见 `iv_probe.c` 的 `icmp_bind_enable()`。 */
    ssize_t  (*sendmsg)(int fd, const struct msghdr *msg, int flags);
    int      (*setsockopt)(int fd, int level, int optname, const void *optval,
                           socklen_t optlen);

    /* 「ICMP 回包按入口网卡认领」用的可选钩子（2026-10-29 加）。
     * 为 NULL 时退回 `recv()`：仍按源地址校验，但**拿不到入口 ifindex**，
     * 于是同目标的两路 WAN 只能靠 seq 唯一性区分（见 `icmp_reject`）。 */
    ssize_t  (*recvmsg)(int fd, struct msghdr *msg, int flags);
} iv_probe_iops_t;

/* 默认注入点：真 libc socket / getsockopt / close + iv_clock 时钟。永不返回 NULL。 */
const iv_probe_iops_t *iv_probe_default_iops(void);

/* ---------------------------------------------------------------------------
 * 配置
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t interval_ms;           /* 默认探测间隔（ms），0 视为默认 */
    /* 单次探测"等回包"超时（ms），0 视为默认。
     * ⚠ 本板**不开放配置**：装配层按"间隔的一半"推导（见 `iv_agent` 的
     * `probe_io_timeout_of()`）。配置键 `probe.timeout_ms` 指的是下面那个
     * **延时门限**，不是这一项 —— 两者都叫 timeout 是历史包袱，别混。 */
    uint32_t timeout_ms;
    uint32_t fail_n;                /* 连续失败阈值 -> DOWN，0 视为默认 */
    uint32_t ok_n;                  /* 连续成功阈值 -> UP，0 视为默认 */
    /* avg RTT 超此值判 DEGRADED（"延时严重"），0=不判。
     * 本板的值来自配置键 `probe.timeout_ms`，也就是平台 0xFE
     * 「有线网络超时时间(ms)」/ 0xE1 的 `pn` —— **用户口径（2026-10-26）**：
     * 设备每轮 ping 拿到平均时延，与它比较；大于＝延时严重，小于＝正常。 */
    uint32_t degrade_rtt_ms;
    uint32_t degrade_loss_permille; /* 丢包率超此值判 DEGRADED，0=不判 */
    const iv_probe_iops_t *iops;    /* NULL -> iv_probe_default_iops() */
} iv_probe_cfg_t;

/* 填默认：interval=5000 / timeout=1000 / fail_n=3 / ok_n=2 /
 * degrade_rtt=800 / degrade_loss=300 / iops=NULL。 */
void iv_probe_cfg_default(iv_probe_cfg_t *cfg);

/* ---------------------------------------------------------------------------
 * 项（内部字段直接暴露，符合本工程零 malloc 风格；调用方只读）
 * ------------------------------------------------------------------------- */
typedef struct {
    iv_probe_layer_t  layer;
    char              name[IV_PROBE_NAME_LEN];
    int               ifindex;   /* 0 = 不绑定接口（只随全局链路判定） */
    uint32_t          dst_be;    /* ICMP/TCP：目标 IPv4，网络序原样 */
    uint16_t          port;      /* TCP：目标端口（主机序） */
    uint64_t          interval_ms;
    uint64_t          next_due_ms;
    uint64_t          sent_at_ms;
    int               inflight;  /* 0/1：已发起待结果 */
    int               fd;        /* ICMP/TCP 进行中的 socket，-1 = 无 */
    uint16_t          icmp_echo_seq; /* 勿叫 icmp_seq：glibc <netinet/ip_icmp.h> 里是宏 */
    iv_probe_state_t  state;
    iv_probe_reason_t reason;
    uint8_t           win_ok[IV_PROBE_WIN_LEN];
    uint32_t          win_rtt[IV_PROBE_WIN_LEN];
    int               win_pos;   /* 下一写入槽 */
    int               win_cnt;   /* 窗内样本数，上限 WIN_LEN */
    uint32_t          last_rtt_ms;
    uint32_t          avg_rtt_ms;
    uint32_t          loss_permille;
    uint32_t          ok_streak;
    uint32_t          fail_streak;
    uint64_t          last_ok_ms;
    uint32_t          sent;
    uint32_t          lost;

    /* 轮统计：见 `iv_probe_stat_t` 同名字段。`rnd_*` 是进行中的一轮，
     * `round_*` 是最近一个**已结算**的轮（上层只读这组）。 */
    uint32_t          rnd_n;
    uint32_t          rnd_ok;
    uint32_t          rnd_rtt_sum;
    uint32_t          round_ok;
    uint32_t          round_lost;
    uint32_t          round_avg_rtt_ms;
    uint32_t          round_seq;
} iv_probe_item_t;

struct iv_probe {
    iv_probe_cfg_t  cfg;
    iv_probe_item_t item[IV_PROBE_MAX_ITEMS];
    int             n_items;
    int             started;
    int             icmp_fd;       /* 引擎级共享 ICMP socket，-1 = 无 */
    int             icmp_disabled; /* 1 = raw ICMP 不可用（降级，见文件头） */
    /* 1 = ICMP 已按项的 `ifindex` 指定出口网卡（`IP_PKTINFO` 开成功）。
     *
     * 为什么需要（2026-10-29 现场事故）：**ICMP 是一条第 3 层 socket，`sendto()`
     * 不带 `ipi_ifindex` 时由内核按"系统默认路由"选出口**，与项上那个 `ifindex`
     * 一点关系都没有 —— `ifindex` 原先只用来判"这块网卡 carrier 掉了没有"。
     * 现场于是出现：4G 明明能到平台（`ping -I usb0` 3/3），但两块 WAN 的探测报文
     * 全从 eth0 出去、全打不通 ⇒ 两条一起判 DOWN ⇒ netmgr 进 BOTH_DOWN ⇒ 按设计
     * "双断只告警不动作" ⇒ 卡在没网的出口上，平台断联 4 小时。
     *
     * 这个标志为 0 时退回"共用一个出口"的老行为，`iv_probe_start()` 会打一条
     * WARN —— 别再让它静默降级，上面那次事故就是这么被掩盖的。 */
    int             icmp_bind_ok;
    /* ICMP echo 序号**引擎级**单调递增（2026-10-22 起）。
     * 病根：序号原先挂在项上、每项各自从 0 起，于是同一引擎的两个 ICMP 项
     * 发出的回显请求**序号一模一样**，而回包只按 `seq + inflight` 认领
     * （`icmp_find_by_seq()`）⇒ 两路一旦错拍，A 的回包会被记到 B 头上，
     * RTT 算成 `now - B.sent_at` 可能为负、转成 uint32 后是个天文数字，
     * 现场表现就是"0.47 ms 的摄像机被判定延时 4"。改成引擎级共享计数器后，
     * 同一时刻在飞的两个请求序号必不相同，认领唯一。 */
    uint32_t        icmp_seq_next;
    /* 被丢弃的 ICMP 回包计数（2026-10-29 加）：`(id, seq)` 对得上，但
     * **源地址不是该项的 dst**、或**入口网卡不是该项的 ifindex**。
     *
     * 为什么必须挡：raw ICMP socket 收**全部** ICMP 报文，原先的认领条件只有
     * `(id, seq) + inflight`，既不看源地址也不看入口接口。现场实测抓到：
     * [source,text]
     * ----
     * seq=703 ifi=8 usb0 src=47.104.250.225 dst=10.146.160.2   ← 平台真回包
     * seq=703 ifi=4 eth0 src=192.168.1.200  dst=192.168.1.120  ← 局域网另一台设备
     * ----
     * 局域网里任何机器 ping 时的 seq 也是递增小整数，撞上就该线路"判通" ——
     * 现场于是出现「有线到平台 100% 丢包，界面却显示主网1 正常」。
     * 这个计数不涨才是正常的；涨了说明现场有别的流量在撞号，日志会 WARN 一次。 */
    uint32_t        icmp_reject;
    int             link_ifindex[IV_PROBE_MAX_LINKS]; /* 0 = 空槽 */
    uint8_t         link_up[IV_PROBE_MAX_LINKS];
};
typedef struct iv_probe iv_probe_t;

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

/* 初始化。cfg==NULL 时取默认。清空项表/链路表，不开 socket。 */
int iv_probe_init(iv_probe_t *p, const iv_probe_cfg_t *cfg);

/* 忘记全部统计与活动 socket（调用方确保已 stop 或接受 fd 泄漏由 stop 收回）；
 * 保留配置与项定义。单测复位用。p==NULL 直接返回。 */
void iv_probe_reset(iv_probe_t *p);

/* ---------------------------------------------------------------------------
 * 项管理（返回项下标 >= 0，或负返回码）
 * ------------------------------------------------------------------------- */

/* 链路层项：被动接收 ifindex 的 carrier 事件。name 仅诊断用（可 NULL）。
 * 注：LINK 项本身不占用 socket；它的"探测"由 iv_probe_set_link() 驱动。 */
int iv_probe_add_link(iv_probe_t *p, int ifindex, const char *name);

/* 网络层项：raw ICMP echo 到 dst（网络序 IPv4）。 */
int iv_probe_add_icmp(iv_probe_t *p, uint32_t dst_be, int ifindex, const char *name);

/* 传输层项：非阻塞 connect 到 dst:port（port 主机序）。 */
int iv_probe_add_tcp(iv_probe_t *p, uint32_t dst_be, uint16_t port, int ifindex,
                     const char *name);

/* 应用层项：引擎不探测，结果由 iv_probe_note_result() 注入。 */
int iv_probe_add_app(iv_probe_t *p, int ifindex, const char *name);

/* ---------------------------------------------------------------------------
 * 启停
 * ------------------------------------------------------------------------- */

/* 打开需要的 socket：
 *   - 有 ICMP 项时开一条共享 raw socket；开不出（EPERM/EACCES 等）**不算失败**，
 *     引擎置 icmp_disabled 并继续（降级，见文件头）；
 *   - TCP 项按需在 tick 发起时创建。
 * 已 start 再调返回 IV_ESTATE。 */
int iv_probe_start(iv_probe_t *p, uint64_t now_ms);

/* 关闭全部活动 socket（含进行中的 TCP），复位 inflight。未 start 返回 IV_ESTATE。 */
int iv_probe_stop(iv_probe_t *p);

/* ---------------------------------------------------------------------------
 * Reactor 接线（只交 fd，不引 Reactor）
 * ------------------------------------------------------------------------- */

/* 当前活动 fd 数量（含共享 ICMP fd 与进行中的 TCP fd）。 */
int iv_probe_fd_count(const iv_probe_t *p);

/* 第 i 个活动 fd（0 <= i < fd_count）。调用方只读，不要自行 close。
 * 越界返回 -1。 */
int iv_probe_fd_at(const iv_probe_t *p, int i);

/* ---------------------------------------------------------------------------
 * 事件驱动
 * ------------------------------------------------------------------------- */

/* ICMP fd 可读：收干回包，匹配 echo reply 更新对应项。返回处理的回包数（>=0），
 * IV_ESTATE = fd 不认识，其余 IV_* 见文件头。 */
int iv_probe_on_readable(iv_probe_t *p, int fd, uint64_t now_ms);

/* TCP fd 可写：读 SO_ERROR 判定连接结果，随后关闭该 socket。返回 1（有结论）/
 * 0（无变化）/ 负码。 */
int iv_probe_on_writable(iv_probe_t *p, int fd, uint64_t now_ms);

/* 周期驱动：发起到期探测 + 超时判定。返回本次"状态发生变化"的项数（>=0）。 */
int iv_probe_tick(iv_probe_t *p, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 结果 / 链路事件注入
 * ------------------------------------------------------------------------- */

/* 注入一次探测结果（APP 层必用；ICMP/TCP 也可以由上层在特殊场景直接喂）。
 * ok!=0 视为成功，rtt_ms 有效；ok==0 视为失败（计入 fail_streak）。
 * 返回 IV_OK，或 IV_EINVAL（idx 越界/未 init）。*/
int iv_probe_note_result(iv_probe_t *p, int idx, int ok, uint32_t rtt_ms,
                         uint64_t now_ms);

/* 链路层事件（装配层从 S3.1 iv_netlink 回调转接）。ifindex 首次出现即入表。
 * up!=0 = carrier 就绪。会即时重判绑定该接口的项。 */
int iv_probe_set_link(iv_probe_t *p, int ifindex, int up, uint64_t now_ms);

/* 运行期调参：节拍、超时、失败阈值（`probe.interval_ms` / `probe.timeout_ms` /
 * `probe.max_fail` 的热生效入口）。
 *
 * 为什么需要：这三个键原先只在 `iv_agent_open()` 里读一次，之后无论从网页改还是
 * 平台下发 `0xF2`，都**必须重启主控**才生效 —— 与 `sys.report.interval_s` /
 * `sys.transport.mode` 是同一类坑（改配置要查"有没有人订阅"）。
 *
 * 为什么叫 retune 而不是 set_timing：`fail_n` 不是节拍（所以不进项的
 * `interval_ms`），但它和节拍是**同一批配置**、同一时刻生效；分成两个入口会让
 * `probe.max_fail` 出"半生效"——probe 换了阈值、netmgr 还是旧的，而这两处的
 * `fail_n` 本就是从同一个键取的（同源，见 `iv_agent_cfg_t.probe_fail_n`）。
 *
 * 为什么不"重建探测表"：重建会把滑窗、连续成功/失败计数全清零，一次改配置就让
 * netmgr 的迟滞重新从零开始、正在判定的链路暂时"没证据"。这几个键只影响节奏与
 * 门限、不影响"探的是谁"，没必要付这个代价。（目标 IP 变了才必须重建，那条路径
 * 在 `iv_agent_reload_probe()` 里，因为 ICMP 项与被动 LINK 项不是同一种东西。）
 *
 * 语义：
 *   - 任一个传 0，或 `timeout_ms >= interval_ms` ⇒ `IV_EINVAL`
 *     （默认值表下限就是 500/100/1，help 文本也写明"超时必须小于探测周期"；
 *     给 0 在 init 里是"取默认"，运行期给 0 只能是配错了，不能当默认处理）；
 *   - `fail_n` 写进 `p->cfg.fail_n`，并**按现有证据立刻重判一次**（连败 2 次、
 *     阈值从 3 降到 2，此刻结论就该是 DOWN）；已经累计的 `fail_streak` 不清零
 *     —— 阈值问的是"要连续几次"，不是"已经几次"；
 *   - 每项的 `interval_ms` 一起改，并把 `next_due_ms` 按新周期从 `now_ms` 重排
 *     —— 否则把 5 s 改成 300 s 时旧 `next_due` 已排在过去，会立刻再探一次；
 *     正在探测中（inflight）的项不动，让它按原样收完这一次；
 *   - 值没变 ⇒ `IV_OK` 且什么都不做（反复下发同一配置不该抖）。
 * 未 init 返回 `IV_ESTATE`。 */
int iv_probe_retune(iv_probe_t *p, uint32_t interval_ms, uint32_t timeout_ms,
                    uint32_t fail_n, uint64_t now_ms);

/* 运行期改两个劣化门限（`probe.timeout_ms` → `degrade_rtt_ms` 的热生效入口）。
 *
 * 本板用的是 RTT 那一维：设备每轮 ping 得到**平均时延**，与 `rtt_ms` 比较，
 * 大于 ⇒ `IV_PROBE_DEGRADED`（上报 `MN=4`「延时」），小于 ⇒ 正常。`0` 表示
 * 该维度不判（与 `iv_probe_cfg_default()` 的默认值无关，运行期 0 是合法值）。
 *
 * `loss_permille` 同批下发，本板传 0（丢包维度不参与判定：链路质量看延时，
 * 丢包由 `fail_n` 那条线走"断开"）。同值早返回；写完按现有证据立刻重判一次。
 * 未 init 返回 `IV_ESTATE`。 */
int iv_probe_set_degrade(iv_probe_t *p, uint32_t rtt_ms, uint32_t loss_permille);

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */

/* 取统一输出。idx 越界返回 IV_EINVAL。 */
int iv_probe_stat_get(const iv_probe_t *p, int idx, iv_probe_stat_t *out);

/* 取项的状态（便捷）。idx 越界返回 IV_PROBE_UNKNOWN。 */
iv_probe_state_t iv_probe_state(const iv_probe_t *p, int idx);

/* 项总数。 */
int iv_probe_count(const iv_probe_t *p);

/* ICMP 层是否可用：1=可用/未尝试，0=已被内核拒绝（降级）。 */
int iv_probe_icmp_available(const iv_probe_t *p);

/* 枚举转名（诊断/日志用，永不返回 NULL）。 */
const char *iv_probe_state_name(iv_probe_state_t s);
const char *iv_probe_reason_name(iv_probe_reason_t r);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_PROBE_H */
