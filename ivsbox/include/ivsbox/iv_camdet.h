/*
 * 摄像机网络状态探测（libivmodules，功能开发计划 M4-S4.1）
 *
 * ============================ 它是什么 ============================
 * 对 ≤6 路摄像机的**网络状态**探测，产出与现网同口径的**五态编码**，供
 *   - 平台周期上报 `CNS` 字段（6 路逗号分隔，见 iv_report_build_seg）；
 *   - S4.4 断电重启消费"正常→故障"的**状态沿**（本模块只给出沿，不动作）。
 *
 * 三种**互斥**检测方式（现网 `search_mode`＝1/2/3，本工程配置键
 * `camera.detect.mode`，运行期可切换）：
 *   1. PING        —— 每轮每台 4 个 ICMP echo；全部应答且**平均 RTT** < 阈值 →
 *                     正常；全部应答但平均 RTT ≥ 阈值 → 延时；部分应答 → 丢包；
 *                     全无应答 → 故障。
 *   2. 端口扫描     —— 逐台对 7 个常见摄像机端口做非阻塞 connect：任一端口可连 →
 *                     正常；全部不可连 → 故障（本方式与 RTSP 方式**不产生**
 *                     延时/丢包两态，与现网一致）。
 *   3. RTSP        —— 向 `rtsp://<ip>:554/` 发 `OPTIONS`，收到任意响应字节 →
 *                     正常；连接失败或收响应超时 → 故障。
 *
 * ============================ 五态编码（硬口径） ============================
 *   0 = 无 IP（未配置该路）      1 = 正常
 *   2 = 故障                     4 = 延时
 *   5 = 丢包
 *   （**没有 3**：现网 `NET_STATUS_E` 里 3 被主网报文的"网线断开"占用。）
 * 与现网 `com.c` 的 CNS 组装一一对应：`CNS=%01d,%01d,…;`，未配置的槽位填 0。
 *
 * ============================ 与现网的口径对齐（逐条） ============================
 *   判据                现网（GD32F460 `lwip_ping_multi.c` / `port_scan.c` /
 *                       `rtsp_task.c`）        本模块
 *   ------------------  ----------------------  --------------------------------
 *   每轮包数            4 包                    4 个样本（IV_CAMDET_PING_SAMPLES）
 *   轮节奏              4 包 / 20 s             4 包 / 20 s（包间隔 5 s）
 *   单包超时            5000 ms                 5000 ms
 *   延时阈值            200 ms（DEFALUT_NETWORK_DELAY，可配）  配置键
 *                                               `camera.detect.rtt_ms` 默认 200
 *   端口表              {80,554,8000,37777,5000,8080,8899}     逐字照抄
 *   端口 connect 超时   1 s（select）           1000 ms
 *   扫描/RTSP 轮间隔    30 s                    30 s（IV_CAMDET_ROUND_MS_DEFAULT）
 *   RTSP 请求串          `OPTIONS rtsp://<ip>:554/ RTSP/1.0` ＋ CSeq ＋ User-Agent
 *                                               **逐字照抄**（含 LibVLC 那行 UA）
 *   RTSP 成功判据       收到 >0 字节（不校验内容）  同（**不拉流、不解析 SDP**）
 *   网线断开            所有已配 IP 的摄像机报 2  同（`iv_camdet_set_link(0)`）
 *
 * **唯一的采样形态差异（登记，不掩盖）**：现网是"一轮 4 包在 4 s 内发完、
 * 单包超时 5 s"，本模块是"4 包均匀铺在 20 s 内"（包间隔 5 s）。两者**采样率与
 * 结论更新率相同**（4 包/20 s），均值口径一致；均匀采样不会把 4 个包压在同一个
 * 网络抖动窗口里，对质量评估更稳。如需**逐字复刻**现网的突发形态，
 * 把 IV_CAMDET_PING_INTERVAL_MS 改成 1000 并在结算处改为"轮起点＋4 s"即可。
 *
 * ============================ 为什么复用 iv_probe 引擎 ============================
 * PING 方式**不自己写 ICMP**：本模块内嵌一个**独立的 iv_probe 实例**
 * （`iv_probe_init()` 起一份，与 `iv_agent` 给双 WAN 用的那一份互不干扰），
 * 复用它的 raw socket 创建、校验和、回包匹配、超时判定与**无 CAP_NET_RAW 时的
 * 降级**（`icmp_disabled`）。本模块只做"取样 + 五态映射 + 轮结算"，不复刻引擎。
 * 之所以要**独立实例**而不是往 WAN 那份里加项：`IV_PROBE_MAX_ITEMS` 是 8，
 * 双 WAN 已占 2，摄像机 6 路会把项表顶满、且三方式切换要反复增删项（引擎无删项
 * 接口）；独立实例两者都免。
 *
 * **取样为何能精确到"单包 RTT"**：包间隔（5 s）≫ tick 周期（200 ms），因此
 * 每 tick 内 `iv_probe_stat_t.sent`/`lost` 的增量最多 1；`last_rtt_ms` 只在成功
 * 时更新，于是"刚完成的那一个样本"的成功与否、其 RTT，都能在增量出现的那一拍
 * 无歧义地取到。采样器据此把样本逐个压入本模块的 4 包环。
 *
 * ============================ 端口扫描 / RTSP 的执行方式 ============================
 * 两者都是**短交互**（一次 connect、或 connect＋一个请求/响应），本模块用一个
 * 由 tick 推进的**串行状态机**做，非阻塞 socket ＋ 每 tick 查 `SO_ERROR`／可读，
 * **绝不阻塞 Reactor**。未按计划原文放进慢任务池（S5）：慢任务池是给"单次调用
 * 无法拆细的长阻塞"（DNS／SNMP／ONVIF）用的，把 6 台×7 端口的 1 s 超时塞进去会
 * 长期占住 2 个 worker 之一；此处非阻塞状态机的阻塞度为零，反而更省。
 * （计划 §S4.1 第 3 条原话是"RTSP 用慢任务池"，此处按上述理由登记为偏离。）
 *
 * ============================ 并发与生命周期 ============================
 *   - **单写者**：全部接口只在装配层（Reactor 单线程）调用，内部无锁；
 *   - 零 malloc：`iv_camdet_t` 由调用方持有（约 2 KB，含内嵌 iv_probe 实例），
 *     放静态区／堆，勿放栈；
 *   - 时间一律由调用方以 `now_ms` 传入（单测可注入时间轴）；
 *   - `iv_camdet_open()` 后可用 `iv_camdet_set_channels()` 反复刷入新 IP 表
 *     （配置热更新），内部会**重建** PING 项。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK      成功
 *   IV_EINVAL  参数非法（NULL / 通道号越界 / mode 不在 1~3）
 *   IV_ESTATE  未 open 就 tick / 重复 open
 *   IV_ENOTSUP PING 方式但 ICMP 不可用（`iv_probe_icmp_available()` 返 0）
 */
#ifndef IVSBOX_IV_CAMDET_H
#define IVSBOX_IV_CAMDET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h> /* socklen_t */

#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量（编译期定死，风格同 iv_probe）
 * ------------------------------------------------------------------------- */

/* 摄像机路数。现网 `camera[6]` / `ip[6][4]` 同为 6，CNS 字段固定 6 路。 */
#define IV_CAMDET_CH_N 6

/* PING：包间隔、单包超时、每轮样本数。4 包 / 20 s 与现网同节奏。 */
#define IV_CAMDET_PING_INTERVAL_MS 5000u
#define IV_CAMDET_PING_TIMEOUT_MS  5000u
#define IV_CAMDET_PING_SAMPLES     4u

/* 延时判定阈值默认值：现网 `DEFALUT_NETWORK_DELAY` = 200 ms。 */
#define IV_CAMDET_RTT_THR_MS_DEFAULT 200u

/* 扫描 / RTSP：轮间隔与单步超时（对齐现网 30 s 轮、1 s 超时）。 */
#define IV_CAMDET_ROUND_MS_DEFAULT 30000u
#define IV_CAMDET_PORT_TIMEOUT_MS  1000u
#define IV_CAMDET_RTSP_TIMEOUT_MS  1000u
#define IV_CAMDET_RTSP_PORT        554u

/* 常见摄像机端口表（现网 `camera_ports[]` 逐字照抄，7 个）。 */
#define IV_CAMDET_PORT_TABLE_N 7u

/* 通道 IP 点分字符串缓冲（诊断用，最长 "255.255.255.255" + NUL）。 */
#define IV_CAMDET_IP_STR_MAX 16u

/* RTSP 请求串上限：实测约 130 字符，给到 192 留余量。 */
#define IV_CAMDET_RTSP_REQ_MAX 192u

/* ---------------------------------------------------------------------------
 * 枚举
 * ------------------------------------------------------------------------- */

/* 五态编码。**数值即上报编码**，不得改动（现网 NET_STATUS_E）。 */
typedef enum {
    IV_CAMDET_NO_IP  = 0, /* 未配置该路 IP */
    IV_CAMDET_NORMAL = 1, /* 网络正常 */
    IV_CAMDET_FAULT  = 2, /* 网络故障（含"网线断开"覆盖） */
    IV_CAMDET_DELAY  = 4, /* 延时（仅 PING 方式产生） */
    IV_CAMDET_LOSS   = 5  /* 丢包（仅 PING 方式产生） */
} iv_camdet_status_t;

/* 检测方式。数值与现网 `search_mode` 一致（1/2/3）。 */
typedef enum {
    IV_CAMDET_MODE_PING = 1, /* 1 = PING */
    IV_CAMDET_MODE_PORT = 2, /* 2 = 端口扫描 */
    IV_CAMDET_MODE_RTSP = 3  /* 3 = RTSP OPTIONS */
} iv_camdet_mode_t;

/* ---------------------------------------------------------------------------
 * I/O 注入点（单测用；生产走 iv_camdet_default_iops()）
 * 端口扫描 / RTSP 两条路径的全部系统调用都经它转发；PING 走 iv_probe 自己的
 * 注入点（`iv_camdet_cfg_t::pb_iops`）。
 * ------------------------------------------------------------------------- */
typedef struct {
    int      (*socket)(int domain, int type, int proto);
    int      (*connect)(int fd, const struct sockaddr *addr, socklen_t len);
    int      (*getsockopt)(int fd, int level, int optname, void *optval,
                           socklen_t *optlen);
    ssize_t  (*send)(int fd, const void *buf, size_t len, int flags);
    ssize_t  (*recv)(int fd, void *buf, size_t len, int flags);
    int      (*close)(int fd);
    uint64_t (*now_ms)(void); /* 默认 iv_clock_monotonic_ms() */
} iv_camdet_iops_t;

/* 默认注入点：真 libc socket/connect/getsockopt/send/recv/close ＋ iv_clock。
 * 永不返回 NULL。 */
const iv_camdet_iops_t *iv_camdet_default_iops(void);

/* ---------------------------------------------------------------------------
 * 配置
 * ------------------------------------------------------------------------- */
typedef struct {
    int      mode;        /* 1/2/3；0 → 默认 PING */
    uint32_t rtt_thr_ms;  /* 延时阈值；0 → IV_CAMDET_RTT_THR_MS_DEFAULT */
    uint32_t round_ms;    /* 扫描/RTSP 轮间隔；0 → IV_CAMDET_ROUND_MS_DEFAULT */

    /* PING 方式的引擎注入点；NULL → iv_probe_default_iops()（真 socket）。
     * 配置文件模板里的 ICMP 部分由本模块固定为 5 s 间隔 / 5 s 超时，
     * 与该注入点无关。 */
    const iv_probe_iops_t *pb_iops;
    /* 扫描/RTSP 的注入点；NULL → iv_camdet_default_iops()。 */
    const iv_camdet_iops_t *iops;

    /* 状态沿回调：某通道**对外状态**发生变化时调用（含"网线断开"覆盖导致的
     * 变化）。ch 为通道号、old/new 为五态编码。可 NULL。 */
    void (*on_change)(int ch, iv_camdet_status_t old_st,
                      iv_camdet_status_t new_st, void *arg);
    void *cb_arg;
} iv_camdet_cfg_t;

/* 填默认：mode=PING / rtt_thr=200 / round=30 s / 注入点 NULL / 回调 NULL。 */
void iv_camdet_cfg_default(iv_camdet_cfg_t *cfg);

/* ---------------------------------------------------------------------------
 * 句柄（内部字段直接暴露，符合本工程零 malloc 风格；调用方只读）
 * ------------------------------------------------------------------------- */
typedef struct iv_camdet {
    iv_camdet_cfg_t cfg;

    /* PING 方式的引擎实例（独立于 iv_agent 的 WAN 实例） */
    iv_probe_t      pb;
    int             ch_idx[IV_CAMDET_CH_N];   /* 该通道在 pb 中的项下标；-1=无项 */
    uint32_t        ch_ip_be[IV_CAMDET_CH_N]; /* 通道 IP（网络序）；0=未配置 */

    /* 探测结论（不含"网线断开"覆盖）与对外状态（含覆盖） */
    iv_camdet_status_t vd[IV_CAMDET_CH_N];
    iv_camdet_status_t st[IV_CAMDET_CH_N];
    uint64_t           ch_changed_ms[IV_CAMDET_CH_N]; /* 对外状态最近变化时刻 */

    /* PING 取样环：最近 IV_CAMDET_PING_SAMPLES 个样本 */
    uint8_t  pk_ok[IV_CAMDET_CH_N][IV_CAMDET_PING_SAMPLES];
    uint32_t pk_rtt[IV_CAMDET_CH_N][IV_CAMDET_PING_SAMPLES];
    uint8_t  pk_pos[IV_CAMDET_CH_N]; /* 下一写入槽 */
    uint8_t  pk_n[IV_CAMDET_CH_N];   /* 环内样本数（上限 SAMPLES） */
    uint32_t pv_sent[IV_CAMDET_CH_N]; /* 上次取样的引擎 sent（增量基准） */
    uint32_t pv_lost[IV_CAMDET_CH_N]; /* 上次取样的引擎 lost */

    /* 端口扫描 / RTSP 串行状态机 */
    int      sc_on;      /* 1 = 本轮进行中 */
    int      sc_ch;      /* 当前通道 */
    int      sc_step;    /* 0=发起连接 1=等连接结果 2=发请求 3=等响应 */
    int      sc_port;    /* 端口扫描：当前端口下标 */
    uint32_t sc_bits;    /* 端口扫描：本轮已连通端口位图 */
    int      fd;         /* 进行中的 socket，-1=无 */
    uint64_t fd_deadline_ms;
    size_t   tx_len;     /* RTSP：待发请求长度 */
    size_t   tx_off;     /* RTSP：已发偏移 */
    char     tx[IV_CAMDET_RTSP_REQ_MAX];
    uint64_t next_round_ms; /* 扫描/RTSP：下一轮起点（0=立即开一轮） */

    int      link_up;    /* 摄像机所在网口 carrier；1=up */
    int      opened;
    uint64_t now_ms;

    /* 统计（诊断） */
    uint64_t rounds;       /* PING 方式结算轮数 */
    uint64_t scan_rounds;  /* 端口扫描/RTSP 完成轮数 */
    uint64_t dup_ip;       /* 通道 IP 重复导致无项的通道数（累计） */
    uint64_t ch_no_ip;     /* 跳过探测的"无 IP"通道计数（累计） */
} iv_camdet_t;

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

/* 初始化：清零 + 置"无 IP" + 通道下标 -1 + fd=-1。cfg 为 NULL 时取默认。 */
void iv_camdet_init(iv_camdet_t *d, const iv_camdet_cfg_t *cfg);

/* 启动：建内嵌 iv_probe 引擎（PING 用）。cfg 为 NULL 时沿用 init 时的配置。
 * 通道表默认为全"未配置"，需随后调 iv_camdet_set_channels()。
 *   IV_OK / IV_EINVAL / IV_ESTATE（已 open） */
int iv_camdet_open(iv_camdet_t *d, const iv_camdet_cfg_t *cfg, uint64_t now_ms);

/* 关闭：停在飞 socket、关引擎。幂等。 */
void iv_camdet_close(iv_camdet_t *d);

/* ---------------------------------------------------------------------------
 * 运行期设置（均可在 tick 之间随时调用）
 * ------------------------------------------------------------------------- */

/* 刷入通道 IP 表（**网络序**；0=该路未配置）。与当前表一致时是空操作；
 * 有变化则重建 PING 项并清空该通道的取样环与结论。
 * 返回本次状态变化的通道数（>=0）或负码。 */
int iv_camdet_set_channels(iv_camdet_t *d, const uint32_t *ip_be, uint64_t now_ms);

/* 切换检测方式（1/2/3）。非法值返回 IV_EINVAL 且不改动现状。
 * 切换会**丢弃**在飞的探测与当前方式的轮进度（结论保留，直到新方式给出新结论）。 */
int iv_camdet_set_mode(iv_camdet_t *d, int mode, uint64_t now_ms);

/* 注入"摄像机所在网口"的 carrier 状态（装配层从 S3.1 iv_netlink 转接）。
 * up=0 ⇒ 所有**已配 IP**的通道对外状态变为故障(2)（对齐现网
 * `com_report_get_camera_status()` 的"网线断开→2"）；up=1 恢复为探测结论。 */
int iv_camdet_set_link(iv_camdet_t *d, int up, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 驱动
 * ------------------------------------------------------------------------- */

/* 周期驱动（装配层 200 ms 一次）：
 *   ① PING：驱动内嵌引擎（发探/收包/超时）→ 采样新完成的包进 4 包环 →
 *      集满 4 个样本结算一轮并更新结论；
 *   ② 端口扫描 / RTSP：推进串行状态机（到期开新一轮）；
 *   ③ 重算对外状态并按需回调 on_change。
 * 返回本次**对外状态变化**的通道数（>=0）；IV_EINVAL/IV_ESTATE 见文件头。 */
int iv_camdet_tick(iv_camdet_t *d, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */

/* 通道对外状态（含"网线断开"覆盖）。ch 越界返回 IV_CAMDET_NO_IP。 */
iv_camdet_status_t iv_camdet_status(const iv_camdet_t *d, int ch);

/* 通道探测结论（**不含**"网线断开"覆盖）。ch 越界返回 IV_CAMDET_NO_IP。 */
iv_camdet_status_t iv_camdet_verdict(const iv_camdet_t *d, int ch);

/* 通道 IP（网络序）；0=未配置。 */
uint32_t iv_camdet_ip(const iv_camdet_t *d, int ch);

/* 导出 CNS 六路编码（out 至少 IV_CAMDET_CH_N 个 int）。d/out 为 NULL 返回 IV_EINVAL。 */
int iv_camdet_cns(const iv_camdet_t *d, int *out);

/* 当前检测方式；未 open 返回 0。 */
/* 是否已 open（装配层接线用）。 */
int iv_camdet_is_open(const iv_camdet_t *d);

int iv_camdet_mode(const iv_camdet_t *d);

/* 对外状态最近一次变化时刻（调用方时钟）；从未变化返回 0。 */
uint64_t iv_camdet_last_change_ms(const iv_camdet_t *d, int ch);

/* PING 取样环内样本数与累计结算轮数（诊断/单测）。 */
int      iv_camdet_samples(const iv_camdet_t *d, int ch);
uint64_t iv_camdet_rounds(const iv_camdet_t *d);

/* 枚举转名（诊断/日志用，永不返回 NULL）。 */
const char *iv_camdet_status_name(iv_camdet_status_t s);
const char *iv_camdet_mode_name(int mode);

/* 常见摄像机端口表（只读，IV_CAMDET_PORT_TABLE_N 项）。 */
const uint16_t *iv_camdet_port_table(void);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_CAMDET_H */
