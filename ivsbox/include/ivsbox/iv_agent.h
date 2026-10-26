/*
 * M3 装配层（libivmodules，功能开发计划 M3 收口）
 *
 * ============================ 它是什么 ============================
 * 把 M3 的四条独立模块接成**一个可运行的整体**，并统一持有它们的句柄与
 * Reactor 接线。它本身**不含业务逻辑**（业务全在被接的模块里），只做三件事：
 *
 *   1) **装配**：按配置初始化并 open 四条链，注册 Reactor fd/定时器；
 *   2) **转接**：把一条链的输出喂给下一条链的输入（全是回调转接，无加工）；
 *   3) **编排**：按固定的周期驱动各自的 tick，并定义关闭顺序。
 *
 * 四条链与数据流：
 *
 *   ┌─ 网络侧（S3.1→S3.2→S3.3）──────────────────────────────────────┐
 *   │  iv_netlink（内核 rtnetlink 事件）                              │
 *   │     │ LINK_UP/DOWN                                            │
 *   │     ▼                                                          │
 *   │  iv_probe（LINK/ICMP/TCP 探活，绑接口）                        │
 *   │     │ 每 WAN 的 UP/DOWN/DEGRADED                               │
 *   │     ▼                                                          │
 *   │  iv_netmgr（双 WAN 迟滞 + 六态状态机 + 切换事务）              │
 *   │     │ 事务步 reconnect                                          │
 *   │     ▼                                                          │
 *   └──► iv_report_reconnect（断旧 TCP、用新出口重建）───────────────┘
 *
 *   ┌─ 控制板侧（S2.2→S2.3→S2.6 ＋ S3.4→S3.5 ＋ S2.4）───────────────┐
 *   │  全部封装在 iv_report（S3.6）内部，本层只调 open / reconnect /  │
 *   │  close，并共享同一个 Reactor。                                 │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * ============================ M4 追加的三段（2026-10-13） ============================
 *   ┌─ 设备管理（S4.1/S4.2/S4.4/S4.5）───────────────────────────────┐
 *   │  iv_camdet（S4.1 五态探测）── 1→2 沿 ──► iv_recovery（S4.4）    │
 *   │                                            └─ 0xD1 ──► iv_report_send_cmd
 *   │  iv_device（S4.2 多品牌发现）── 档案 ──► iv_agent_device_get()   │
 *   │  iv_netmgr 迟滞结论 ──► MN 两路 ── 1→2 沿 ──► iv_recovery（继电器 1）
 *   │  iv_status 0xC1 ──► on_door_open（S4.5，抓图本体属 M6）          │
 *   └────────────────────────────────────────────────────────────────┘
 *   ┌─ SNMP 采集（S4.3）─────────────────────────────────────────────┐
 *   │  iv_agent_snmp_start() ──► iv_taskpool（**worker 线程内阻塞收发**）│
 *   │      └─ 完成 ──► on_snmp_done（本层线程）──► 结果表              │
 *   └────────────────────────────────────────────────────────────────┘
 *   **MN 的生产者就在这里**：`env_feed()` 用 `iv_netmgr_wan_is_up()`（迟滞结论，
 *   连续 fail_n 次才判死）—— 不用裸 `iv_probe_state()`，单次超时不该切主网电。
 *   平台命令 0xE4（搜索摄像机）/ 0xE7（查 SNMP）也由本层注册路由。
 *   三段中的 SNMP 是**唯一需要慢任务池**的（阻塞式收发）；设备发现与 camdet 都是
 *   非阻塞相位机，只吃 200 ms tick。
 *
 * ============================ 为什么单独成模块 ============================
 * 装配逻辑若直接写进 `src/app/main.c`，因**app 层 .c 不进静态库**，单测链接
 * 不到符号，只能整机冒烟。落成 modules 层的独立模块后可单测、可反向证伪，
 * 也让 main.c 退回"纯骨架装配"（与前例 S3.6 落 modules 的理由一致）。
 *
 * ============================ 并发与生命周期 ============================
 *   - **单写者**：全部接口只在装配层（Reactor 单线程）调用，内部无锁；
 *   - 零 malloc：`iv_agent_t` 由调用方持有（约 35 KB，主要为 iv_report 内嵌的
 *     队列/传输缓冲）；**必须放静态区或堆**，不要放栈；
 *   - 时间一律由调用方以 `now_ms` 传入；reactor 模式内部用
 *     `iv_clock_monotonic_ms()`；
 *   - 关停顺序：本层定时器 → report → netmgr → probe → netlink（open 的逆序）。
 *
 * ============================ 已知取舍（登记，不掩盖） ============================
 *   - **probe 的活动 fd 不逐个挂 Reactor**：iv_probe 的 TCP fd 是每次 tick 按需
 *     创建/销毁的，逐个注册会让 Reactor 注册表频繁抖动。改为在 probe 定时器
 *     （`IV_AGENT_PROBE_TICK_MS`）里对该时刻的全部 fd 调一次 on_readable /
 *     on_writable —— 两者对"不认识的 fd"返回错误码而不误处理，循环调用安全。
 *   - **平台地址按活动出口选**：`srv_host[IV_WAN_N]` / `srv_port[IV_WAN_N]` 下标＝
 *     `iv_wan_t`，分别来自 `net.server.wireless.*` 与 `net.server.wired.*`。
 *     开链时按**初始活动出口**取（只有 `WIRED_ONLY` 模式从有线起步，其余从无线起步，
 *     与 netmgr 初始态 `WIRELESS_UP` 一致）；换出口时在 `reconnect` 钩子里按
 *     **netmgr 传入的 new_ifindex** 反推目标出口再取址，并交给
 *     `iv_report_set_server()` 生效。选址优先级链（2026-10-22 起四条）：
 *     **本口主 → 本口备 → 另一口主 → 另一口备**（备地址来自
 *     `net.server.<口>.host_bak` / `port_bak`，**只在主地址没配时顶上，不做失败切换**）；
 *     四条都空 ⇒ 不启平台链路（`iv_report_reconnect` 返回 `IV_ESTATE` 空转）。
 *   - **箱门事件（S4.5）**：本层把 `on_door_open` 挂到 `iv_report_cfg_t::on_door`，
 *     `iv_status` 解到 `0xC1`（箱门开启）时同步回调。**M4 阶段＝空实现 ＋ 日志 ＋ 计数**，
 *     抓图本体属 M6-S6.2（到时把 `on_door_open` 换成本地通道 `snapshot.take` 派发即可）；
 *     无鉴权 / 无审计 / 无去重表。
 *   - **devid（TID）无配置键来源**：按架构 §10.2「身份不进配置」，
 *     devid 由工厂身份分区（`/dev/by-name/private`，S5.5）读取 ⇒ **不设配置键**。
 *   - **mod（型号）/ sv（软件版本）/ tsn（芯片序列号）各有唯一来源**（2026-10-20 定）：
 *     mod 走配置键 `sys.device.model`（Web 可改，用户明确要求破例，见 iv_config.c）；
 *     sv 由本层填 `iv_version_sysver()`（`1.0.0.<构建日期>`，平台只认这个格式）；
 *     tsn 由装配层从 `iv_prov_tsn()` 取芯片唯一号，**不派生自 devid**。
 */
#ifndef IVSBOX_IV_AGENT_H
#define IVSBOX_IV_AGENT_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_camdet.h"
#include "ivsbox/iv_cellular.h"
#include "ivsbox/iv_device.h"
#include "ivsbox/iv_gps.h"
#include "ivsbox/iv_media.h"
#include "ivsbox/iv_netlink.h"
#include "ivsbox/iv_netmgr.h"
#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_recctl.h"
#include "ivsbox/iv_store.h"
#include "ivsbox/iv_storage.h"
#include "ivsbox/iv_recovery.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_snmp.h"
#include "ivsbox/iv_taskpool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WAN 接口名长度（含结尾 NUL） */
#define IV_AGENT_WAN_NAME_MAX 16u

/* ---------------------------------------------------------------------------
 * M4-S4.2 摄像机信息探测（设备发现）与 M4-S4.3 SNMP 采集的装配常量
 * ------------------------------------------------------------------------- */
#define IV_AGENT_SNMP_TARGET_N 2 /* [0]=摄像机（camera.ch.0.ip）[1]=交换机（switch.ip） */
#define IV_AGENT_SNMP_CAM      0
#define IV_AGENT_SNMP_SW       1

/* 单目标最多槽位＝交换机 12 槽（摄像机 9 槽是它的子集） */
#define IV_AGENT_SNMP_SLOT_MAX IV_SNMP_SW_SLOT_N

/* GPS 串口默认设备节点（UART1；板端设备树启用后才有这个节点）。
 * 波特率见 `IV_GPS_UART_BAUD`（9600，外部事实，不进配置）。 */
#define IV_AGENT_GPS_PATH_DEFAULT "/dev/ttySAC1"

/* 4G 模组 AT 口（只读状态查询，见 iv_cellular.h）。
 * **与出厂镜像的拨号脚本共用同一个口**：本层只发查询命令，不发任何会改模块
 * 状态或断网的命令。实测 EC200G 的 AT 口是 `/dev/ttyUSB0`（ttyUSB1=NMEA，
 * ttyUSB2 不回话，ttyUSB3 是数据口）。
 *
 * ⚠ 这个常量只是**首选**，不是"事实"：模组 `AT+CFUN=1,1` 会让 USB 重枚举，
 * ttyUSB 的编号会变（2026-10-23 板端踩过）。真正的口由 `iv_cellport` 探出来，
 * 结果记在 `iv_agent_t::cell_dev`。本常量仅用作"第一个要探的候选"。 */
#define IV_AGENT_CELL_PATH_DEFAULT "/dev/ttyUSB0"

/* 实际在用的 AT 口路径缓冲。`/dev/ttyUSB12` 共 14 字节，32 有 2 倍余量。 */
#define IV_AGENT_CELL_DEV_N 32
/* 自动重拨用的**拨号动作**：一段由 `/bin/sh -c` 执行的短序列，`$0` = AT 口路径、
 * `$1` = 无线网卡名。默认值就是本板手工验证过的那三步（见下），**不调出厂
 * `usb-EC200G-inserted.sh`** —— 那个脚本在本板会卡在 `timeout 1 cat /dev/ttyUSB0`
 * 上不返回（16:40/16:47 两次实测），而且它 `flock -n` 只在没被 udhcpc 继承锁时能跑。
 *
 * 三步：起口 → 让模组重新拨号（`AT+QNETDEVCTL=1,1,1`）→ 要地址。
 * ⚠ 这是**全工程唯一一处向共用 AT 口发非查询命令**的地方：只在判定"该重拨"时执行，
 * 且执行前会把本进程的 AT fd 关掉（`cell_link_drop`），避免两边抢同一个口。
 * 空串 = 关闭自动重拨。 */
#define IV_AGENT_CELL_DIAL_DEFAULT \
    "ip link set \"$1\" up; " \
    "killall udhcpc 2>/dev/null; " \
    "( sleep 1; printf 'AT+QNETDEVCTL=1,1,1\\r' > \"$0\" ) >/dev/null 2>&1 & " \
    "timeout 3 cat \"$0\" >/dev/null 2>&1; " \
    "udhcpc -i \"$1\" -n -q >/dev/null 2>&1"
/* 拨号子进程的硬超时：超过这个时间还没退就 SIGKILL。
 * 出厂脚本卡死那次暴露的问题不是"拨不上"，而是**守护进程一直等它** ——
 * AT 轮询跟着停，页面上的所有 4G 数据 freeze。 */
#define IV_AGENT_CELL_DIAL_TIMEOUT_MS ((uint64_t)45000u)
/* AT 轮询驱动节拍。必须远小于 `IV_CELL_CMD_TIMEOUT_MS`（800 ms），
 * 否则节拍本身会把"发一条、等一条"的节奏拖成龟速。 */
#define IV_AGENT_CELL_TICK_MS 100u

/* 判定"AT 口静默失效"的连续无回应轮数，以及两次重探之间的最小间隔。
 * 一轮 30 s，3 轮 = 90 s：短于这个数可能只是被拨号脚本抢了口（它要在同一个
 * 口上读写几秒），长于这个数现场就白等。重探本身要扫 6 个口（最坏 1.2 s），
 * 30 s 的间隔保证即使一直探不到也不会拖慢主循环。 */
#define IV_AGENT_CELL_NOREPLY_RESCAN 3u
#define IV_AGENT_CELL_RESCAN_GAP_MS  ((uint64_t)30000u)
/* 表列 GETNEXT 结果**留存行数**上限（装配层只留前 N 行，避免结果表过大；
 * 完整遍历上限由 iv_snmp_target_t::max_rows 管） */
#define IV_AGENT_SNMP_ROWS_MAX 4

/* 总线节拍：probe 驱动（发探/收结果/超时判定）与 netmgr 驱动（迟滞/回切）。 */
#define IV_AGENT_PROBE_TICK_MS  200u
/* **探测回包的排空节拍**（2026-10-26 加）。
 *
 * 为什么不能只靠上面那个 200 ms 的节拍：`iv_probe` 测的 RTT 是"发出时刻 → 读到
 * 回包的时刻"。回包只在调用方喂 fd 的时候才被读走 —— 如果只每 200 ms 读一次，
 * 那么**局域网里 0.5 ms 的往返也会被记成 200 ms**（回包躺在缓冲区里等到下一拍）。
 * 于是 `probe.timeout_ms` 这个"延时门限"根本无法判 100 ms 量级，配 100 就会把
 * 两条好链路全标成「延时」——正是 `iv_camdet` 在 R-02（2026-10-22）踩过的同一个坑
 * （见 `iv_camdet.h` 的 `IV_CAMDET_RTT_THR_MS_DEFAULT` 注释）。
 *
 * 本板的口径是"每 ping 一次拿到平均时延，和门限比"（用户 2026-10-26），所以
 * **测量分辨率必须明显细于门限下界 100 ms**：10 ms ⇒ 量化误差 ≤ 10%。
 * 代价是每 10 ms 两个 `recv()`（无数据立刻 EAGAIN），单核 1 GHz 上可忽略。
 *
 * 注意：`iv_camdet` 有自己的实例与节拍，**不受这个值影响**（它的 200 ms 分辨率
 * 已在 R-02 里按"严格大于"判据适配过）。 */
#define IV_AGENT_PROBE_DRAIN_MS 10u
/* 契约钉在编译期：采样分辨率必须明显细于门限下界（`probe.timeout_ms` 的下界是
 * 100 ms）。有人把这个值改大、或者把门限下界改小到覆盖不了，这里立刻编译失败
 * —— 否则"延时门限"会悄悄退化成"能不能在一个节拍内应答"的二值判断。 */
_Static_assert(IV_AGENT_PROBE_DRAIN_MS * 5u <= 100u,
               "RTT sampling must be well below the 100 ms threshold floor");
#define IV_AGENT_NETMGR_TICK_MS 1000u

/* 阈值下发（0xF1）待发 JSON 的缓冲。九项全发也就 `{"hv":12345,...}` 一百多字节，
 * 256 足够；超了 `iv_report_build_thresh()` 会回 `IV_ERANGE`，不会被静默截断。 */
#define IV_AGENT_THRESH_JSON_MAX 256u
/* 待发项被 `IV_EBUSY` 挡回后的重试间隔。刻意给 1 s 而不是每拍（200 ms）：
 * 挡路的通常是 30 s 一次、几百毫秒就结束的 0xE1 轮询，1 s 足够跨过去，
 * 又不会在采集板长时间不应答时把总线刷满。 */
#define IV_AGENT_THRESH_RETRY_MS 1000u

/* 探活目标路数（probe.target1 / target2） */
#define IV_AGENT_TARGET_N 2

/* ---------------------------------------------------------------------------
 * S4.3 SNMP 采集：目标配置与结果表
 *
 * OID **只来自配置**（`camera.snmp.oid.<槽位>` / `switch.snmp.oid.<槽位>`）。
 * 槽位名固定取 `iv_snmp_cam_slots[]` / `iv_snmp_sw_slots[]` 的前 n_oid 个
 * （顺序即配置默认值表顺序）⇒ 代码内**不内置任何厂家 OID 值**。
 * ------------------------------------------------------------------------- */
typedef struct {
    int      enable;     /* 0 = 不采该目标 */
    uint32_t ip_be;      /* 目标 IPv4（网络序）；0 = 未配（即使 enable 也不采） */
    uint16_t port;       /* 0 → 161 */
    char     community[IV_SNMP_COMMUNITY_MAX];
    uint32_t timeout_ms; /* 0 → 1500 */
    uint32_t retries;    /* 0 → 1 */
    uint32_t max_rows;   /* 表列 GETNEXT 上限；0 → 64 */
    int      n_oid;      /* 有效槽位数 0..IV_AGENT_SNMP_SLOT_MAX */
    char     oid[IV_AGENT_SNMP_SLOT_MAX][IV_SNMP_OID_STR_MAX];
} iv_agent_snmp_cfg_t;

/* 一条取值（比 iv_snmp_value_t 少一个 OID 文本字段——槽位自带 OID） */
typedef struct {
    uint8_t type; /* iv_snmp_value_t.type；0 = 无值 */
    int64_t num;
    char    str[IV_SNMP_STR_MAX];
} iv_agent_snmp_row_t;

typedef struct {
    char                oid[IV_SNMP_OID_STR_MAX]; /* 本次实际查的 OID（诊断/Web） */
    int                 n_rows;                   /* 0 = 无值 */
    int                 rc;                       /* 该槽位最后一次 iv_snmp 返回码 */
    iv_agent_snmp_row_t row[IV_AGENT_SNMP_ROWS_MAX];
} iv_agent_snmp_slot_t;

/* 采集作业（＝结果表本体）。**跨线程分工见 iv_agent.c 文件头**：
 * worker 只写本结构，reactor 线程只在 on_done 之后读。 */
typedef struct {
    iv_snmp_target_t     t;    /* 目标参数（worker 只读；iops 已填默认） */
    void                *owner;/* iv_agent_t*（装配层回填；worker 不碰） */
    int                  which;/* 目标下标 IV_AGENT_SNMP_CAM/_SW（worker 不碰） */
    int                  n_slot;
    iv_agent_snmp_slot_t slot[IV_AGENT_SNMP_SLOT_MAX];
} iv_agent_snmp_job_t;

/* ---------------------------------------------------------------------------
 * 配置（由 app 层从 iv_config 解析后填入；本层不依赖 iv_config）
 * ------------------------------------------------------------------------- */
typedef struct {
    /* ---- 控制板上报链（S3.6）---- */
    const char *serial_path; /* NULL → 默认 /dev/ttySAC5 */
    const char *queue_dir;   /* NULL → 默认 /mnt/UDISK/ivsbox/queue */
    /* 平台地址：下标＝iv_wan_t（IV_WAN_WIRELESS / IV_WAN_WIRED）；
     * 某口 NULL/"" 或 port=0 ⇒ 该口无地址（自动回退另一口）。 */
    const char *srv_host[IV_WAN_N];
    uint16_t    srv_port[IV_WAN_N];
    /* 各口的**备份**平台地址（2026-10-22 新增；对应 `net.server.<口>.host_bak` / `port_bak`）。
     * 选地址的完整优先级链在 `iv_agent_pick_host()`：
     *     本口主 → 本口备 → 另一口主 → 另一口备
     * ⚠ **不参与失败切换**：主地址连不上时不会自动改走备份（用户 2026-10-22
     *   拍板「不切换，只作为备份」）。它只在主地址"没配"（NULL/"" 或 port=0）
     *   时才顶上 —— 所以这条链是**静态选择**，每次重连/换出口重跑一遍即可，
     *   不需要额外状态、也没有"卡在备地址回不来"的风险。
     * 与主地址同口径：NULL/"" 或 port=0 ⇒ 该口无备份。
     * ⚠ 指针语义同 `mod`（见 main.c 的 `g_mod[]` 注释）：装配层把配置内部的
     *   定长缓冲指针直接放在这里，配置事件里原地改写内容、指针不变。 */
    const char *srv_host_bak[IV_WAN_N];
    uint16_t    srv_port_bak[IV_WAN_N];
    const char *mod;         /* 硬件型号串（E3 应答）；NULL → "" */
    const char *sv;          /* 软件版本串（E3 应答）；NULL → 由本层填 iv_version */
    const char *tsn;         /* 终端序列号（E3 应答）；NULL → "" */

    /* 平台 0xE1（查询设备当前参数设置）的 `data` 段：契约见
     * `iv_report_cfg_t::on_query_cfg`。本层只把它透传给 report。
     * NULL ⇒ 0xE1 回 0x74（"未实现"），不发半份配置。 */
    int (*on_query_cfg)(char *out, size_t cap, void *arg);
    void *on_query_cfg_arg;
    uint32_t    devid;       /* 设备标识（TID） */
    uint16_t    devtype;     /* 0 → 默认 0x0A00（`IV_PROTO_DEVTYPE_DEFAULT`） */
    uint32_t    report_ms;   /* 周期上报间隔；0 → 默认 300 s */
    uint32_t    heartbeat_ms;/* 心跳间隔；0 → 默认 30 s */
    uint32_t    poll_ms;     /* 0xE1 轮询间隔；0 → 默认 30 s */

    /* ---- 双 WAN 与探活（S3.1/S3.2/S3.3）---- */
    char        wan_name[IV_WAN_N][IV_AGENT_WAN_NAME_MAX]; /* 下标＝iv_wan_t；空串跳过 */
    uint32_t    probe_target_be[IV_AGENT_TARGET_N]; /* target1/2（**网络序**）；0=未配 */
    uint32_t    probe_interval_ms;  /* 0 → iv_probe 默认 5000 */
    /* 有线网络**延时门限**（毫秒），来自配置键 `probe.timeout_ms`
     * ＝平台 0xFE「有线网络超时时间(ms)」/ 0xE1 的 `pn`。落到
     * `iv_probe_cfg_t::degrade_rtt_ms`：每轮 ping 的平均时延超过它 ⇒ 判
     * "延时严重"（`IV_PROBE_DEGRADED` ⇒ 上报 `MN=4`），不超过 ⇒ 正常。
     * 0 → `iv_probe` 默认 800。
     *
     * ⚠ 它**不是**单次探测的等回包超时（那一项由间隔推导，见
     * `probe_io_timeout_of()`）。原先两件事共用一个名字，2026-10-26 用户明确
     * 口径后拆开：配置里那个数只负责"延时算不算大"。 */
    uint32_t    probe_delay_ms;
    uint32_t    probe_fail_n;       /* 0 → iv_probe 默认 3 */

    iv_netmgr_mode_t mode;          /* 0 → AUTO */
    uint32_t    nm_fail_n;          /* 0 → 默认 3 */
    uint32_t    nm_ok_n;            /* 0 → 默认 2 */
    uint32_t    nm_hold_s;          /* 0 → 默认 30 */
    uint32_t    nm_stable_s;        /* 0 → 默认 10 */

    /* ---- M4 摄像机网络状态检测（S4.1）---- */
    uint32_t    camera_ip_be[IV_CAMDET_CH_N]; /* 0=该路未配置 */
    uint32_t    camera_detect_mode;   /* 0 → 默认 PING(1)；对应现网 search_mode */
    uint32_t    camera_detect_rtt_ms; /* 0 → 默认 200 ms */

    /* ---- M4 自愈（S4.4）：故障沿 → 电源通道断电重启 ----
     * `netthr.reboot_max`：每通道最多动作次数，**0 ＝ 禁用自愈**
     *   （现网出厂 net_reload=0；本工程该键出厂默认 3，见修改记录）。
     * `netthr.reboot_interval_s`：动作后冷却秒数（现网 NETWAOK_RELOAD_TIME
     *   = 60）。0 → 由 iv_recovery 取默认（60 s）。 */
    uint32_t    netthr_reboot_max;
    uint32_t    netthr_reboot_interval_s;

    /* ---- M4 摄像机信息探测（S4.2，设备档案）----
     * `dev_enable` 为 0 时本层**完全不建**设备发现（不 open、不占 fd），
     * 主进程在 S10 装配里置 1（开机自动搜索）。 */
    int          dev_enable;
    char         dev_ifname[IV_AGENT_WAN_NAME_MAX]; /* SADP 网口；空串 → wan_name[IV_WAN_WIRED]。
                                          **刻意是数组而非 const char\***：本层会整拷 cfg，
                                          指向调用方局部数组的指针拷完即悬空。 */
    uint32_t     dev_local_ip_be; /* SADP Inquiry 的本机 IP；0 → 0.0.0.0 */
    uint8_t      dev_local_mac[6];/* SADP Inquiry 的本机 MAC；全 0 → 固定占位 */

    /* ---- M4 SNMP 采集（S4.3，**必须经慢任务池**）----
     * `pool` 为 NULL ⇒ 不启采集（iv_snmp 是阻塞式收发，留在 Reactor 线程会
     * 卡住整个事件循环，属架构 §4.2 明令禁止）。下标见 IV_AGENT_SNMP_*。 */
    iv_taskpool_t *pool;
    iv_agent_snmp_cfg_t snmp[IV_AGENT_SNMP_TARGET_N];

    /* ---- S9 延伸：control.db 落盘（2026-10-15）----
     * 可空 ⇒ 只写日志不落库（fail-open：档案/审计是辅助数据，建表失败
     * （磁盘满/只读）不得拖死主控）。挂上后：设备发现结果 upsert 进
     * `device`、SNMP 结果进 `snmp_latest`、出口切换进 `net_event`。 */
    iv_store_t *store;

    /* ---- M6 媒体（S6.1/S6.2/S6.3）----
     * `media_snap_dir`：门开/ctl 自动命名的抓拍落盘目录；空串 →
     *   IV_MEDIA_SNAP_DIR_DEFAULT（板端 SD 挂载点确认前按 bird_media 的
     *   /mnt/sdcard 根系走，登记为待板端事实）。
     * `rec_*`：断网录像（S6.3）。判据＝出口全断（迟滞结论）＋平台 TCP 断，
     *   保持 rec_hold_ms 才开录；恢复 rec_debounce_ms 才停录。
     *   **来源已接线（2026-10-14，S6.5 前置）**：`camera.ch.N.url`（RTSP 地址，
     *   含账密）＋ `camera.ch.N.record`（是否录像）**同时满足**该路才进表；
     *   时间参数来自 `camera.rec.hold_ms` / `camera.rec.debounce_ms`
     *   （0 ⇒ 用 recctl 默认 10 s / 60 s）。
     *   ⚠ **两个路数域**：本表 8 路，但只有**前 6 路**会被网络探测
     *   （平台上报帧 `CNS` 段固定 6 宽）⇒ 第 7/8 路**只录不测**。 */
    char     media_snap_dir[64];
    uint32_t rec_hold_ms;     /* 0 → IV_RECCTL_HOLD_MS_DEFAULT */
    uint32_t rec_debounce_ms; /* 0 → IV_RECCTL_DEBOUNCE_MS_DEFAULT */
    int      rec_n_url;       /* 0..IV_RECCTL_CH_N */
    char     rec_url[IV_RECCTL_CH_N][IV_RECCTL_URL_MAX];

    /* ---- M6 SD 卡策略（S6.5）----
     * `storage_enable`：1 = 装配每拍巡检（statfs＋有界扫描＋按需删最旧）。
     * `storage_root`：空串 ⇒ IVSBOX_SD_ROOT ⇒ 默认 /mnt/sdcard（与 media 抓拍
     *   目录同源）。**刻意不接配置键**（§18.5 拍板：容量与水位是运行时事实）。
     * 无独立开关时，未插 SD 会走"未挂载"故障路径（拒新＋停录＋告警），
     * 这是有意行为，不是 bug。 */
    int      storage_enable;
    char     storage_root[IV_STORAGE_PATH_MAX];

    /* ---- S2.5 GPS / NMEA（UART1，9600 8N1）----
     * `gps_path`：NULL → 默认 `IV_AGENT_GPS_PATH_DEFAULT`（`/dev/ttySAC1`）；
     *   空串 → **不打开串口**（解析器仍然初始化，调用方可用
     *   `iv_agent_gps_feed()` 自行灌字节，测试/回放用）。
     * ⚠ 波特率**不在这里**：它是 `IV_GPS_UART_BAUD`（9600）这条外部事实，
     *   装配层直接用常量，**绝不能**用 `iv_serial_cfg_default()` 的 115200
     *   —— 那是采集板链路的参数，配错的症状是"收不到任何数据"，看起来像没上电。
     * `gps_sync_time`：1 = 允许按 GPS 校时（`iv_gps_sync_time()`）；
     *   **默认 0（只解析、不碰系统时钟）**。校时会改整机时间、继而影响日志
     *   时间线与 OTA 判定，属"改行为"的动作，需显式打开。 */
    const char *gps_path;
    int         gps_sync_time;

    /* 4G 模组状态查询（只读）。`cell_path`：NULL → 默认 `/dev/ttyUSB0`；
     * **空串 = 不打开 AT 口**（页面显示"未启用"，而不是一堆未知值 ——
     * "没开"和"开了但读不到"是两件事，不能混成一个空表）。
     * `cell_period_ms`：0 → 用 `IV_CELL_PERIOD_MS`（30 s）。 */
    const char *cell_path;
    uint32_t    cell_period_ms;
    /* 自动重拨：`cell_dial_cmd` 为空串 = 关闭；NULL = 用 `IV_AGENT_CELL_DIAL_DEFAULT`。
     * 参数按出场脚本的固定形态给（vid pid 网卡名），不开放成配置细节。 */
    const char *cell_dial_cmd;
} iv_agent_cfg_t;

/* ---------------------------------------------------------------------------
 * 句柄
 * ------------------------------------------------------------------------- */
typedef struct {
    iv_agent_cfg_t cfg;

    iv_report_t  rp;  /* 控制板上报链（S3.6，内嵌 S2.x/S3.4/S3.5） */
    iv_netlink_t nl;  /* S3.1 */
    iv_probe_t   pb;  /* S3.2 */
    iv_netmgr_t  nm;  /* S3.3 */
    iv_camdet_t  cd;  /* S4.1 摄像机网络状态探测（内嵌第二个 iv_probe 实例） */
    iv_recovery_t rec; /* S4.4 自愈：消费 cd 的"正常→故障"沿，发 0xD1 断电重启 */
    iv_device_t  dev;  /* S4.2 多品牌设备发现（开机一次 ＋ 0xE4 手动触发） */
    iv_agent_snmp_job_t snmp_job[IV_AGENT_SNMP_TARGET_N]; /* S4.3 采集作业与结果 */

    /* M6 媒体（S6.1~S6.3）：media＝回环 HTTP 桥接（自带 worker 线程）；
     * recm＝断网录像状态机（纯逻辑，动作回调绑本实例的 media）。名字避开
     * 上面的 rec（iv_recovery）。 */
    iv_media_t   media;
    iv_recctl_t  recm;
    iv_storage_t stg;  /* S6.5 SD 卡策略（每拍 step；只读 statfs＋有界扫描，无 fd 常驻） */

    /* S2.5 GPS 链路。**解析器恒初始化**（`r == NULL` 或串口没开出来时同样可用），
     * 串口打不开只降级、不致命 —— GPS 是辅助信息，不能因为没插天线就拖死主控。 */
    iv_gps_t     gps;
    int          gps_fd;            /* -1 = 串口未打开 */
    iv_event_t  *gps_ev;            /* Reactor 注册句柄；r == NULL 时恒 NULL */
    uint64_t     gps_rx_bytes;      /* 累计收到的字节数（0 ⇒ 线没接 / 波特率错 / 没上电） */
    uint64_t     gps_last_parse_ms; /* 最近一次成功解析出语句的单调时刻；0 = 从未 */
    int          gps_open_rc;       /* 最近一次开串口的结果：0 = 已打开，负 = 错误码，IV_ENOTSUP = 配置为不开 */

    /* 4G 模组状态（只读）。与 GPS 同一套装配方式：串口 + Reactor 可读事件
     * ＋ 一个周期定时器驱动查询状态机。**打开失败只降级不致命** ——
     * 没插模块不该影响主控，页面显示"AT 口打不开"即可。 */
    iv_cell_t    cell;
    int          cell_fd;           /* -1 = AT 口未打开 */
    iv_event_t  *cell_ev;           /* Reactor 读事件；r == NULL 时恒 NULL */
    iv_timer_t  *cell_timer;        /* 查询驱动定时器；r == NULL 时恒 NULL */
    uint64_t     cell_rx_bytes;     /* 累计读到的字节（0 且 fd 已开 ⇒ 模块不回话） */
    int          cell_open_rc;      /* 最近一次开 AT 口的结果：0 = 已打开 */
    /* 自动重拨（遗留 ⑧：出厂 udev 脚本只在插入时拨一次，掉线不重拨）。
     * 判定在 iv_cellular（纯函数），这里只负责"让出 AT 口 → 起脚本 → 收尸 → 重开"。 */
    iv_cell_redial_t cell_redial;
    int          cell_dial_pid;     /* >0 = 拨号序列在跑；-1 = 空闲 */
    uint64_t     cell_dial_ms;      /* 本次拨号开始的时刻（判硬超时用） */
    uint32_t     cell_dials;        /* 累计发起次数（给日志/诊断） */
    uint64_t     cell_reopen_ms;    /* 上次尝试重开 AT 口的时刻；防每拍 open() */
    /* **实际在用的** AT 口（探口结果），不是配置值。
     * 为什么要单独记：`cfg.cell_path` 是"用户说要哪个"，而 ttyUSB 编号会随
     * 模组重新枚举漂移（`AT+CFUN=1,1` 就会），写死的路径可能指向 diag 口。
     * 探口一旦命中就记在这里，之后重开先探它（一次往返 ~20 ms），探不到才全扫。
     * 空串 = 还没探到过。`cell_spawn_dial()` 也用它当 `$0` —— 否则拨号脚本会
     * 去读那个错的路径，把好不容易探对的口又扔了。 */
    char         cell_dev[IV_AGENT_CELL_DEV_N];
    uint64_t     cell_rescan_ms;    /* 上次因"整轮无响应"而重探的时刻；防风暴 */

    /* netmgr 的事务执行点：switch_route 复用默认（netlink 改路由），reconnect
     * 接本层 → iv_report_reconnect。ops.arg 指向本结构，故不能是静态常量。 */
    iv_netmgr_txn_ops_t ops;

    iv_reactor_t *r;          /* NULL = 由调用方按节拍调 iv_agent_step() */
    iv_event_t   *nl_ev;
    iv_timer_t   *pb_timer;
    /* 回包排空定时器（`IV_AGENT_PROBE_DRAIN_MS`）：只管读回包，不发探、不判超时。
     * 与 `pb_timer` 分开是为了让"测量分辨率"和"发送/超时节拍"互不牵制。 */
    iv_timer_t   *pb_drain_timer;
    iv_timer_t   *nm_timer;

    int          wan_ifindex[IV_WAN_N];   /* 启动时按名字解析；0=未绑定 */
    int          wan_probe_idx[IV_WAN_N]; /* 绑该 WAN 的 probe 项下标；-1=无 */
    uint8_t      wan_link_up[IV_WAN_N];   /* 最近一次 netlink carrier 状态 */
    uint64_t     door_events;             /* S4.5 累计门开事件次数（M4 阶段可观测用） */

    uint8_t      mn_last[IV_RECOVERY_MASTER_N]; /* S4.4 主网每路上一次 MN 状态（1/2） */
    uint8_t      snmp_state[IV_AGENT_SNMP_TARGET_N]; /* 0 空闲 / 1 在飞 / 2 就绪 */
    uint8_t      snmp_stale[IV_AGENT_SNMP_TARGET_N]; /* 1 = 配置换了但作业正在飞，
                                                      * 等它落地后由 start 重装 */
    uint32_t     snmp_reqid[IV_AGENT_SNMP_TARGET_N]; /* request-id 递增种子 */
    uint64_t     snmp_runs;           /* 累计完成的采集轮数 */
    uint64_t     snmp_slot_noval;     /* 累计"该项无值"（无应答/异常值）槽位次数 */

    /* 阈值下发（`0xF1`）的待发项。`elec.*`/`sensor.*` 改完只是"想让它生效"，
     * 真正发帧会被 `IV_EBUSY`（同 cmd 在飞，通常是 30 s 一次的 0xE1 轮询）挡回，
     * 所以这里存一份**已拼好的 JSON**，每拍重试到 `IV_OK` 为止——与 S4.4 的
     * "保留队头、下一拍再试"同一套做法。存文本而不是 `iv_thresh_t`：重试时不必
     * 再读配置（配置可能又变了一次，那会走新的 apply 覆盖它）。 */
    char         th_json[IV_AGENT_THRESH_JSON_MAX];
    uint16_t     th_len;       /* 0 = 没有待发项 */
    uint32_t     th_drops;     /* 累计被更新的阈值覆盖掉的待发项次数（诊断） */
    uint16_t     th_tries;     /* 当前待发项已重试次数（日志降噪用） */
    uint64_t     th_next_ms;   /* 下次允许重试的时刻；0 = 立即 */

    uint64_t     now_ms;
    uint8_t      opened;
} iv_agent_t;


/* iv_agent_snmp_state() 的返回 */
#define IV_AGENT_SNMP_IDLE 0
#define IV_AGENT_SNMP_RUN  1
#define IV_AGENT_SNMP_DONE 2

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

/* 填默认配置：默认串口/队列路径、wan_name = usb0/eth0、mode = AUTO、
 * 探活默认、平台地址空（不启平台链路）。 */
void iv_agent_cfg_default(iv_agent_cfg_t *cfg);

/*
 * 平台地址选择（**纯函数**，2026-10-22 起是 4 条链）。
 * 入参 `wan` 是"当前活动出口"（`iv_wan_t`：IV_WAN_WIRELESS / IV_WAN_WIRED）。
 * 优先级：**本口主 → 本口备 → 另一口主 → 另一口备**，第一条"地址有效"的胜出；
 * 有效判据＝host 非 NULL / 非空串 **且** port != 0。
 *   - 返回值为 `cfg` 内部的字符串指针（不拷贝、不返回静态缓冲），`*port` 同步改写；
 *   - 四条都无效 ⇒ 返回 NULL 且 `*port = 0`（调用方据此不启平台链路）；
 *   - `port == NULL` 时不回写端口；
 *   - `cfg == NULL` ⇒ 返回 NULL。
 *
 * ⚠ **它不看任何运行期状态，也不做失败切换**（用户 2026-10-22 拍板
 *   「不切换，只作为备份」）：主地址"有效但连不上"时会话层报错重连，
 *   **不会**改走备地址 —— 备份只在主地址"没配"时顶上。这是刻意的：
 *   要真切换得先定"何时切、何时回切、门限多少"，属另一件事（登记在 docs）。
 *
 * 之所以导出而不是留 static：这是本层最容易写错的一处分支（4 条链 × 两个口），
 * 单测直接钉住比"起一整套 report 只为了看选中谁"便宜得多。
 */
const char *iv_agent_pick_host(const iv_agent_cfg_t *cfg, int wan, uint16_t *port);

/* 清零句柄（不碰系统资源）。使用前必须先 init（或 `= {0}`）。 */
void iv_agent_init(iv_agent_t *a);

/*
 * 启动：按 cfg 依次 open 上报链 / netlink / probe / netmgr，并在 r 非 NULL 时
 * 注册 netlink fd 与两个定时器。r 为 NULL 时需调用方按节拍调 iv_agent_step()。
 *   IV_OK ；IV_EINVAL（a/cfg 为 NULL）；IV_ESTATE（已 open）
 *   IV_EIO/… report 启动失败时的透传码
 * 任一子链失败都**不致命**：netlink 拿不到 fd、probe 开不出 ICMP、平台地址为空
 * 都按降级继续（各自有日志），只有 report 的 EINVAL/ESTATE 直接失败返回。
 *
 * ⚠ **两种驱动模式装配的东西完全一样**：`r != NULL` 只是多注册 netlink fd 与两个
 * 定时器；camdet（S4.1）、recovery（S4.4）、device（S4.2）、SNMP 作业（S4.3）与
 * 0xA5/0xE4/0xE7 路由在 `r == NULL` 时**同样装配**（它们全不依赖 Reactor，
 * 由 `iv_agent_step()` 推进）。这条纪律别破坏 —— 曾经把 camdet/recovery 的 init
 * 写在 `r != NULL` 里，结果"调用方按节拍推进"的模式下 `iv_recovery_t` 从未 init，
 * 主网/摄像机的故障沿静默失效。
 */
int iv_agent_open(iv_agent_t *a, const iv_agent_cfg_t *cfg, iv_reactor_t *r,
                  uint64_t now_ms);

/*
 * 单步驱动（无 reactor 时由调用方按节拍调用；reactor 模式下两个定时器内部
 * 走的就是同一批动作）：驱动 probe → 喂 netmgr → netmgr tick → report step。
 * 未 open 返回 IV_ESTATE；其余恒 IV_OK。
 */
int iv_agent_step(iv_agent_t *a, uint64_t now_ms);

/* 关闭：本层定时器 → report → netmgr → probe → netlink（open 的逆序）。 */
void iv_agent_close(iv_agent_t *a);

/* ---------------------------------------------------------------------------
 * 只读读数
 * ------------------------------------------------------------------------- */
int               iv_agent_is_open(const iv_agent_t *a);
iv_netmgr_state_t iv_agent_netmgr_state(const iv_agent_t *a);
iv_wan_t          iv_agent_active_wan(const iv_agent_t *a);
int               iv_agent_active_ifindex(const iv_agent_t *a);
/* 活动出口的**接口名**（配置 `net.wan.*.name` 里的值，如 "usb0"/"eth0"）。
 * Web 要显示"当前通信网口"就得拿这个名字 —— 只给 wired/wireless 现场没法对照
 * `ip addr`。未装配 / 该 WAN 没配名字 ⇒ 返回空串（不是 NULL，调用方不必判空）。 */
const char       *iv_agent_active_wan_name(const iv_agent_t *a);
/* 累计成功切换次数 / 累计事务失败次数（netmgr 读数；未装配给 0）。 */
uint64_t          iv_agent_netmgr_switches(const iv_agent_t *a);
uint64_t          iv_agent_netmgr_txn_fail(const iv_agent_t *a);

/* ---------------------------------------------------------------------------
 * 平台地址热重载（2026-10-19 补）
 *
 * ============================ 为什么需要它 ============================
 * `iv_report` 打开时把平台地址**拷进自己的副本**（`server_host/server_port`），
 * 之后只有 netmgr 换出口才经 `agent_reconnect` 重推一次。于是 Web 上改了
 * `net.server.*` 之后：配置落盘了、界面也显示新值，**但报文还发往旧地址**，
 * 一直到重启主控为止。现场表现就是"改了端口，平台那边一直等不到数据"。
 *
 * ============================ 调用契约 ============================
 * 装配层（`main.c` 的配置订阅回调）先把新地址写进 `a->cfg.srv_host[]` /
 * `srv_port[]`，再调本函数：
 *   - host 可以直接沿用旧指针：配置的字符串存在 `union{char s[N]}` 定长缓冲里，
 *     原地改写、指针一直有效；
 *   - **port 是值类型，必须重写**（agent 存的是拷贝）。
 *
 * 本函数按**当前活动出口**重取地址（本口没配则回退另一口），推给上报链、
 * 顺带把出口网卡绑定一起刷新（只换地址不换绑定，报文会从旧网卡出去），
 * 然后重连一次。
 *
 * 返回值＝`iv_report_reconnect()` 的结果；未 open 返回 `IV_ESTATE`。
 * 地址与原来一致时同样会重连一次 —— 代价是几秒重建，换来"装配层不必记住
 * 哪些键需要重启"，这比省那几秒重要。
 * ------------------------------------------------------------------------- */
int iv_agent_reload_server(iv_agent_t *a);

/* ---------------------------------------------------------------------------
 * 探活参数热生效（2026-10-21 补，对应 `probe.*` 五个键与平台 `0xF2`/`0xF8`）
 *
 * ============================ 为什么需要它 ============================
 * `probe.target1/target2/interval_ms/timeout_ms/max_fail` 原先只在
 * `iv_agent_open()` 里读一次，改完必须重启主控。这和 `sys.report.interval_s`、
 * `sys.transport.mode`、`net.server.*` 是同一类坑 —— **改配置要查"有没有人订阅"**。
 *
 * ============================ 两条路，别混 ============================
 *   - 目标 IP 没变（或只改节拍/超时/阈值）⇒ 只调参数，**不重建探测表**：
 *     重建会把滑窗与连续成功/失败计数清零，让 netmgr 的迟滞从零重来，
 *     改一次"5 s → 10 s"不该有这样的副作用。
 *   - 目标 IP 变了 ⇒ 必须重建：`iv_probe` 里"有目标"是 ICMP 项、"没目标"是被动
 *     LINK 项，**换层只能重来**；且新目标本来就是一整套新统计。
 *
 * 重建后**必须**重播链路现状（`agent_probe_seed_links()`）：探测表清空时
 * `link_ifindex/link_up` 一起没了，而 netlink 只报变化、不回放现状（见
 * `iv_netlink.h` 取舍 2）—— 不重播会让两条 WAN 都 UP 的板"看不见"任何可用 WAN。
 *
 * ============================ 参数 ============================
 * 传的是**配置原始值**（`0` 表示"取默认"）。归一化只走 `probe_cfg_of()` 一处，
 * open 与 reload 共用，避免"加了新键只改 open、重载后悄悄回默认"。
 *
 * ⚠ 2026-10-22 起 `fail_n` **只喂 `iv_probe`**（判目标 DOWN）。状态机的切换门限
 * （`iv_netmgr` 的 `cfg.fail_n`）改由 `netthr.loss_count` 提供，见
 * `iv_agent_reload_netthr()` —— 架构 §10.2 明写「切换门限由 `netthr.*` 承担」。
 * 在此之前两处同源，改 `probe.max_fail` 会把切换门限一起改掉。
 *
 * 未 open 返回 `IV_ESTATE`；`timeout_ms >= interval_ms` 等非法组合返回 `IV_EINVAL`
 * 且**不改任何东西**（半套生效比不生效更难查）。归一化后与现值完全一致时直接
 * `IV_OK`、连日志都不打 —— `PROBE` 分类任一键变化都会走到这里，没有这条早返回会
 * 让"改 target2"看起来像"探测表被重建过"。
 * ------------------------------------------------------------------------- */
int iv_agent_reload_probe(iv_agent_t *a, uint32_t target1_be, uint32_t target2_be,
                          uint32_t interval_ms, uint32_t timeout_ms, uint32_t fail_n,
                          uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 阈值下发（2026-10-22 补，对应 `elec.*` / `sensor.*` 九个键）
 *
 * ============================ 修的是什么 ============================
 * 这九个键此前**全工程没有任何消费者**：网页「电阈值与传感器」卡片能改能存，
 * 存完采集板一无所知 —— 界面上有、设备不认，属于"假功能"。参考 web 的
 * 「阈值设置」是核心功能，本机只是个配置编辑器。
 *
 * ============================ 为什么不在本函数里直接发帧 ============================
 * `iv_link` 同一 cmd 只许一笔在飞（`IV_EBUSY`），而 0xE1 轮询每 30 s 就占一次
 * 总线。改配置那一刻撞上在飞的查询是常态，直接发帧会有相当比例的失败。
 * 所以这里只**登记**待发载荷，真正发帧由 `thresh_drive()` 在每拍重试到成功
 * 为止 —— 与 S4.4 的「保留队头、下一拍再试」同一套。
 *
 * ============================ 同值早返回 ============================
 * 拼出来的 JSON 与待发项逐字节相同就直接返回 `IV_OK`，不重置重试计数。
 * 理由与 `iv_agent_reload_probe()` 一致：`on_cfg_committed` 是对整类配置的
 * 批量回调，改别的键不该顺带把待发项搅乱。
 *
 * 返回：`IV_OK` 已登记（或同值无需再发）；`IV_ESTATE` 九项全为 0（没什么可发，
 * **不是错误**，但也别记"已下发"）；`IV_ERANGE` 拼装超缓冲；`IV_EINVAL` 参数非法。
 * ------------------------------------------------------------------------- */
int iv_agent_apply_thresh(iv_agent_t *a, const iv_thresh_t *t, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 传输模式热生效（2026-10-21 补，对应平台下行 `0xF3` 与网页 `sys.transport.mode`）
 *
 * ============================ 为什么需要它 ============================
 * `sys.transport.mode` 原先只在 `agent_setup()` 里读一次、交给
 * `iv_agent_open()` 决定初始活动口 —— 之后改配置（网页或平台 `0xF3`）**必须
 * 重启主控**才生效。和当年 `sys.report.interval_s` / `net.server.*` 是同一类坑，
 * 也是本工程反复踩的那条：**改配置要查"有没有人订阅"**。
 *
 * ============================ 做了什么 ============================
 * 1. `iv_netmgr_set_mode()`：改 `cfg.mode` 并立刻重跑一次判定（不新造规则）；
 * 2. `a->cfg.mode = mode`：让后续 `iv_agent_open()`（如重开）沿用；
 * 3. `iv_agent_reload_server()`：活动出口真变了的话，**目标地址与出口绑定必须
 *    成对刷新**，否则报文继续从旧网卡出去（2026-10-19 板上实测过）。
 *
 * ============================ 代价 ============================
 * 第 3 步会拆掉现有平台 TCP 连接并重建（几秒）。模式没变时全程早返回，
 * 不会因为平台反复下发同一配置而抖。
 * ------------------------------------------------------------------------- */
int iv_agent_set_transport_mode(iv_agent_t *a, iv_netmgr_mode_t mode, uint64_t now_ms);

iv_report_t      *iv_agent_report(iv_agent_t *a); /* 读上报链统计/状态用 */
uint64_t          iv_agent_door_events(const iv_agent_t *a); /* S4.5 门开事件计数 */

/* S4.1 摄像机探测句柄只读出口（M5-S5.1 新增：Web 总览要读 CNS 六路与检测方式）。
 * 返回内部句柄，调用方只读；a 为 NULL 返回 NULL。 */
const iv_camdet_t *iv_agent_camdet(const iv_agent_t *a);

/* ---------------------------------------------------------------------------
 * 摄像机检测方式热生效 ＋ 立即重测一路（2026-10-22 补）
 *
 * ============================ 修的是什么 ============================
 * ① `camera.detect.mode` / `camera.detect.rtt_ms` 只在 `iv_agent_open()` 里读一次，
 *    网页「检测方式」改完**必须重启主控**才生效 —— 参考 web 那个下拉框是保存即生效
 *    的，本机等于摆设。这是"改配置不生效"清单上的同一条。
 * ② **`camera.ch.N.ip` 也一样**：改完摄像机 IP，camdet 还在测旧地址，直到重启。
 *    现场最典型的动作就是"换个 IP 再测一下"，不热生效等于整条流程断掉。
 * ③ `iv_camdet` 是轮询式（PING 20 s 一轮、扫描/RTSP 30 s 一轮），现场装完摄像机
 *    想马上知道"这一路通不通"只能干等一整轮。Web 摄像机页的「测」按钮就是它。
 *
 * ============================ 参数口径 ============================
 * `mode`：1/2/3（PING/端口扫描/RTSP）。**`0` 或负数一律跳过**，不当成"改回默认" ——
 *   与 `iv_agent_open()` 的口径一致（那里 `cfg->camera_detect_mode != 0` 才用），
 *   否则用户在 web 上把键清空会被读成"他选了 PING"。
 * `rtt_ms`：同上，`0` 跳过（0 是 init 期的"取默认"哨兵，不是合法阈值）。
 * `ip_be`：6 项网络序地址表（`0` = 该路未配置）；**NULL ⇒ 不动通道表**。
 *   与 `iv_camdet_set_channels()` 同口径：表没变时它是空操作，所以本函数可以在
 *   camera 分类任一键变化时无脑调用。
 * `ch`：**0 基下标 0..5**，与 `camera.ch.N.ip` 的 N 同口径（CNS 显示时再 +1）。
 *
 * 返回：reload 给 `IV_OK`（已应用）／`IV_EINVAL`／`IV_ESTATE`（agent 或 camdet 未
 * open）；probe 给 `IV_OK`／`IV_EINVAL`（ch 越界）／`IV_ESTATE`／`IV_ENOENT`
 * （该路没配 IP，前端据此提示"先配 IP"）。
 * ------------------------------------------------------------------------- */
int iv_agent_reload_camdet(iv_agent_t *a, int mode, uint32_t rtt_ms,
                          const uint32_t *ip_be, uint64_t now_ms);
int iv_agent_camdet_probe(iv_agent_t *a, int ch, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 网络阈值热生效 ＋ 运行期读数（2026-10-22 补，对应 `netthr.*` 四个键）
 *
 * ============================ 修的是什么 ============================
 * `netthr.*` 是"改配置不生效"清单上最后一块：
 *   - `netthr.reboot_max` / `netthr.reboot_interval_s`：只在 `agent_setup()` 里读
 *     一次，交给 `iv_agent_open()` 初始化 `iv_recovery` ⇒ 网页改完**必须重启主控**。
 *   - `netthr.loss_count`：**零消费方**。状态机的切换门限一直是 `probe.max_fail`
 *     在喂，而架构 §10.2 明写「切换门限由 `netthr.*` 承担」—— 界面上那个
 *     「丢包次数上限」改了多少次都不影响切换。
 *   - `netthr.timeout_ms`：同样零消费方，**本轮不硬接**（没有诚实的消费者：
 *     ICMP 超时已有 `probe.timeout_ms`、平台链路没有可配超时）。它在默认值表里
 *     保留为**保留键**，说明文字已改成"改后不生效"，别让界面骗人。
 *
 * ============================ 同值早返回是硬要求，不是优化 ============================
 * `iv_recovery_set_limits()` 内部走 `limits_apply()`，它会**把每通道的
 * `credit` 重置成 `reboot_max`**（这是它的规格：热更新限次就该重新发次数）。
 * 但 `on_cfg_committed()` 是对整类配置的批量回调 —— 用户只改 `loss_count`
 * 也会进来。没有同值早返回的话，"改个不相干的键"就等于把自愈次数偷偷补满，
 * 现场会看到一台该停手的设备反复断电重启。所以这里**先算归一化后的有效值、
 * 与现值逐项比对、全等就 `IV_OK` 返回且不碰任何模块**。
 *
 * ============================ 参数口径 ============================
 * `reboot_max`：`< 0` ⇒ **跳过**（0 是合法值＝禁用自愈，不能当哨兵）。
 * `cooldown_s`：`0` ⇒ 跳过（0 在 init 语境表示"取模块默认 60 s"，不是有效秒数）。
 * `loss_count`：`0` ⇒ 跳过（默认值表的下界是 1）。
 *
 * 返回 `IV_OK`（已应用或同值）/ `IV_EINVAL`（参数全被跳过）/ `IV_ESTATE`（未 open）。
 * 某一项失败不影响另一项，返回值取**第一个非 OK 的**，并各记一条日志。
 * ------------------------------------------------------------------------- */
int iv_agent_reload_netthr(iv_agent_t *a, int reboot_max, uint32_t cooldown_s,
                           uint32_t loss_count);

/* ============================ 录像源表热生效 ============================
 * 2026-10-22：`camera.ch.N.url` / `camera.ch.N.record` / `camera.rec.hold_ms` /
 * `camera.rec.debounce_ms` 原先只在 `iv_agent_open()` 读一次 ⇒ 改了必须重启主控。
 * 同一张 `camera.ch` 表里 `ip` 早在 11:20 就热了，相邻几列却要重启，现场最容易踩。
 *
 * ============================ 参数口径 ============================
 * `urls`：`n` 个 C 串，**空串或 `NULL` ＝ 该路不进表**（与 `cfg_rec_sources()`
 *   的"url 非空 且 record 为真"同义，判定在 app 层做完再传进来）。
 * `n`：`0..IV_RECCTL_CH_N`（表长，允许中间留洞）。
 * `hold_ms` / `debounce_ms`：`0` ⇒ **跳过**（`0` 在 recctl 语境是"取默认"，
 *   不是有效时长）。归一化后的有效值与现值全等就 `IV_OK` 早返回、一个模块都不碰 ——
 *   整类提交（改个别的不相干键）是常态，早返回是硬要求。
 *
 * **正在录时会先把这一轮收口再按新表重开**（见 `iv_recctl_set_cfg()`）：只改
 * `cfg` 会让被删掉的那一路 recorder 漏在后台继续写 SD 卡。
 *
 * 返回 `IV_OK` / `IV_EINVAL` / `IV_ESTATE`（未 open，或 media 桥接没起来 ——
 * 这种情形配置已落盘，下次 open 按新值建表）。
 * ------------------------------------------------------------------------- */
int iv_agent_reload_rec(iv_agent_t *a, const char *const *urls, int n,
                        uint32_t hold_ms, uint32_t debounce_ms);

/* ============================ SNMP 目标热生效 ============================
 * 2026-10-22：`camera.snmp.*` / `switch.snmp.*` 原先只在 `agent_setup()` 读一次 ⇒
 * 换交换机 community、补一个 OID 槽位都要重启主控。
 *
 * `sc`：`IV_AGENT_SNMP_TARGET_N` 项（cam/sw），**按数组顺序**对齐。
 * 判据是**归一化后的目标**（`port/timeout/retries/max_rows` 的 `0` 折成模块默认；
 * `n_oid` 截到槽位上限），归一化后逐字节相同就跳过该目标。
 *
 * **作业在飞时不改作业**（慢池里正读它），只置 `snmp_stale[t]`，由
 * `iv_agent_snmp_start()` 在空闲时落地。返回 `IV_OK` / `IV_EINVAL` / `IV_ESTATE`。
 * ------------------------------------------------------------------------- */
int iv_agent_reload_snmp(iv_agent_t *a, const iv_agent_snmp_cfg_t *sc);

/* `netthr.*` 的**运行期真值**（诊断/取证/Web 展示）。
 *
 * 读的是模块内部实际生效的值，**不是配置里的值** —— 这两者曾经长期不一致
 * （配置改了、模块还是旧的），那正是本组函数的修复对象。所以别改成读 config。
 *
 * 例：`netthr.status` ctl 方法就是它的 JSON 包装；集成用例也靠它断言热生效。 */
typedef struct {
    int      nm_fail_n;    /* iv_netmgr 在用的切换门限（`netthr.loss_count`） */
    int      probe_fail_n; /* iv_probe 判目标 DOWN 的门限（`probe.max_fail`） */
    /* iv_probe 在用的**延时门限**（毫秒，`probe.timeout_ms` ⇒ `degrade_rtt_ms`）：
     * ping 平均时延超过它 ⇒ DEGRADED ⇒ 上报 `MN=4`。
     * 加这个字段是为了让"0xFE 落盘了、引擎有没有真换门限"能取证 —— 只看配置
     * 只能证明存下来了（`0xFE` 那一课：回了 0x01 不等于生效）。 */
    int      probe_delay_ms;
    /* 每路 WAN 的**平均时延**（ms，下标＝`iv_wan_t`；-1 ＝该路没有探测项）。
     * 这是"延时门限"那一侧的被比较量：现场问"为什么显示延时"时，要能直接看到
     * 分子（实测平均）和分母（门限），否则只能靠猜。 */
    int      probe_rtt_ms[IV_WAN_N];
    int      reboot_max;   /* iv_recovery 限次；0 = 禁用自愈 */
    uint32_t cooldown_ms;  /* iv_recovery 冷却（毫秒；已归一化，永不为 0） */
    /* 每通道剩余动作次数（下标＝`iv_recovery_port` 的通道号，0 基） */
    uint8_t  credit[IV_RECOVERY_PORT_N];
} iv_agent_netthr_view_t;

/* a 或 out 为 NULL → IV_EINVAL；未 open → IV_ESTATE。 */
int iv_agent_netthr_view(const iv_agent_t *a, iv_agent_netthr_view_t *out);

/* M6 媒体出口：media＝iv_ctl_media_ops_t 绑定用（装配层经 iv_agent_media()
 * 拿实例指针）；recm＝app 层填/读录像 URL 表用（open 后直接改 recm.cfg 也行）。
 * 未 open 时 media.inited=0，绑给 ctl 的方法会按 IV_ENOTSUP 应答。 */
iv_media_t  *iv_agent_media(iv_agent_t *a);
iv_recctl_t *iv_agent_recctl(iv_agent_t *a);
iv_storage_t *iv_agent_storage(iv_agent_t *a);

/* S4.4 主网第 w 路（0/1）的 MN 状态：1=正常 / 2=断开（迟滞判定后；0=尚未判定）。
 * 就是上报数据段 `MN=` 字段的生产者（见 iv_report.h 的 env.mn[]）。 */
uint8_t           iv_agent_mn(const iv_agent_t *a, int w);

/* ---------------------------------------------------------------------------
 * S4.2 设备发现（摄像机信息探测）
 * ------------------------------------------------------------------------- */

/* 重开一轮发现（清空档案、从海康 SADP 相位起）。平台 0xE4 / Web「摄像机搜索」
 * 与开机一次都走这里。未装配（dev_enable=0）返回 IV_ESTATE。 */
int iv_agent_device_search(iv_agent_t *a, uint64_t now_ms);

/* 发现档案只读出口（Web/平台展示用）。
 * count：a 为 NULL → IV_EINVAL，否则返回条数（未装配恒 0）。
 * get：a/out 为 NULL → IV_EINVAL；**i 越界 → IV_ENOENT**（透传 iv_device_get，
 * 未装配时 n_items 恒 0 ⇒ 取任何下标都是 IV_ENOENT）。 */
int iv_agent_device_count(const iv_agent_t *a);
int iv_agent_device_get(const iv_agent_t *a, int i, iv_dev_desc_t *out);

/* 设备发现句柄（测试/诊断；未装配时返回未 open 的句柄，字段可读）。 */
iv_device_t *iv_agent_device(iv_agent_t *a);

/* ---------------------------------------------------------------------------
 * S4.3 SNMP 采集（慢任务池内执行）
 * ------------------------------------------------------------------------- */

/* 发起一轮采集。which = IV_AGENT_SNMP_CAM / IV_AGENT_SNMP_SW。
 * 前置：cfg.pool 非 NULL 且该目标 enable 且 ip_be 非 0（否则 IV_ESTATE）；
 * 上一轮未完成返回 IV_EBUSY。平台 0xE7 / Web 走这里。 */
int iv_agent_snmp_start(iv_agent_t *a, int which, uint64_t now_ms);

/* 采集状态：IV_AGENT_SNMP_IDLE / _RUN / _DONE；参数非法返回 IV_EINVAL。 */
int iv_agent_snmp_state(const iv_agent_t *a, int which);

/* 结果读取（仅在 _DONE 后有内容）。n 为槽位数；越界返回 IV_EINVAL。
 * slot_name 出参＝规范槽位名（永不 NULL），rows 出参拷贝最多 rowcap 行。 */
int iv_agent_snmp_get(const iv_agent_t *a, int which, int i,
                      const char **slot_name, const char **oid, int *n_rows,
                      iv_agent_snmp_row_t *rows, int rowcap);

/* ---------------------------------------------------------------------------
 * S2.5 GPS / NMEA
 * ------------------------------------------------------------------------- */

/* 最近的定位快照（指针指向本层内部，**只读、随下一批字节变化**）。
 * 从未收到任何语句时返回的是"全 NA ＋ valid=0"的快照（不是 NULL，也不用查空）。 */
const iv_gps_t *iv_agent_gps(const iv_agent_t *a);

/* 链路状态（给 Web / 诊断用）：`*path` 出参指向本层实际的设备节点（永不为 NULL），
 * `*fd` 出参为串口 fd（-1 = 没开），返回值为最近一次开串口的结果
 * （0 = 已打开；负 = 错误码；`IV_ENOTSUP` = 配置为空串、本层刻意不开）。 */
int iv_agent_gps_link(const iv_agent_t *a, const char **path, int *fd);

/* ---------------------------------------------------------------------------
 * 4G 模组状态（Quectel EC200G，只读 AT 查询）
 * ------------------------------------------------------------------------- */

/* 最近一次查询得到的快照（指针指向本层内部，**只读**）。
 * 从未完成过任何一轮时返回的是"全未知 ＋ valid=0"的快照（不是 NULL）。 */
const iv_cell_t *iv_agent_cell(const iv_agent_t *a);

/* 链路状态（给页面与诊断用）：`*path` 出参指向实际设备节点（永不为 NULL），
 * `*fd` 为 AT 口 fd（-1 = 没开），`*rx_bytes` 为累计收字节，
 * 返回最近一次开 AT 口的结果（0 = 已打开；负 = 错误码；IV_ENOTSUP = 配置为空串）。
 * **`*path` 是探口探出来的那个口，不是配置里写的那个**：两者不一致时以实际为准，
 * 否则页面上的 `link.path` 会指向一个根本没在用的设备（现场照它排查必错方向）。 */
int iv_agent_cell_link(const iv_agent_t *a, const char **path, int *fd,
                       uint64_t *rx_bytes);

/* 灌入一段字节（**给非 Reactor 驱动与回放用**：`r == NULL` 的测试、或调用方
 * 从别处拿到了 NMEA 字节流）。返回成功解析的语句条数。
 * Reactor 模式下串口的字节由本层自己读，调用方通常不需要碰这个口子。 */
int iv_agent_gps_feed(iv_agent_t *a, const char *buf, size_t len);

/* 累计收字节／最近一次解析时刻（单调毫秒，0 = 从未解析出语句）。
 * Web 用它算"数据 N 秒前"，现场用它区分"没数据"和"有数据但不定位"。 */
uint64_t iv_agent_gps_rx_bytes(const iv_agent_t *a);
uint64_t iv_agent_gps_last_parse_ms(const iv_agent_t *a);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_AGENT_H */
