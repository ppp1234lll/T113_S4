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
 *     `iv_report_set_server()` 生效。**本口没配则回退另一口**；两口都空 ⇒
 *     不启平台链路（`iv_report_reconnect` 返回 `IV_ESTATE` 空转）。
 *   - **箱门事件（S4.5）**：本层把 `on_door_open` 挂到 `iv_report_cfg_t::on_door`，
 *     `iv_status` 解到 `0xC1`（箱门开启）时同步回调。**M4 阶段＝空实现 ＋ 日志 ＋ 计数**，
 *     抓图本体属 M6-S6.2（到时把 `on_door_open` 换成本地通道 `snapshot.take` 派发即可）；
 *     无鉴权 / 无审计 / 无去重表。
 *   - **devid（TID）/ mod（型号）无配置键来源**：按架构 §10.2「身份不进配置」，
 *     devid 应由工厂身份分区（`/dev/by-name/private`，S5.5）读取 ⇒ **不设配置键**；
 *     mod（硬件型号串）与 `sv` 同类，属**编译期事实**，待用户给型号串后定为默认值。
 *     当前二者分别取 0 与 NULL（→ 空串）。
 */
#ifndef IVSBOX_IV_AGENT_H
#define IVSBOX_IV_AGENT_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_camdet.h"
#include "ivsbox/iv_device.h"
#include "ivsbox/iv_netlink.h"
#include "ivsbox/iv_netmgr.h"
#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_reactor.h"
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
/* 表列 GETNEXT 结果**留存行数**上限（装配层只留前 N 行，避免结果表过大；
 * 完整遍历上限由 iv_snmp_target_t::max_rows 管） */
#define IV_AGENT_SNMP_ROWS_MAX 4

/* 总线节拍：probe 驱动（发探/收结果/超时判定）与 netmgr 驱动（迟滞/回切）。 */
#define IV_AGENT_PROBE_TICK_MS  200u
#define IV_AGENT_NETMGR_TICK_MS 1000u

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
    const char *mod;         /* 硬件型号串（E3 应答）；NULL → "" */
    const char *sv;          /* 软件版本串（E3 应答）；NULL → 由本层填 iv_version */
    uint32_t    devid;       /* 设备标识（TID） */
    uint16_t    devtype;     /* 0 → 默认 0x0400 */
    uint32_t    report_ms;   /* 周期上报间隔；0 → 默认 300 s */
    uint32_t    heartbeat_ms;/* 心跳间隔；0 → 默认 30 s */
    uint32_t    poll_ms;     /* 0xE1 轮询间隔；0 → 默认 30 s */

    /* ---- 双 WAN 与探活（S3.1/S3.2/S3.3）---- */
    char        wan_name[IV_WAN_N][IV_AGENT_WAN_NAME_MAX]; /* 下标＝iv_wan_t；空串跳过 */
    uint32_t    probe_target_be[IV_AGENT_TARGET_N]; /* target1/2（**网络序**）；0=未配 */
    uint32_t    probe_interval_ms;  /* 0 → iv_probe 默认 5000 */
    uint32_t    probe_timeout_ms;   /* 0 → iv_probe 默认 1000 */
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

    /* netmgr 的事务执行点：switch_route 复用默认（netlink 改路由），reconnect
     * 接本层 → iv_report_reconnect。ops.arg 指向本结构，故不能是静态常量。 */
    iv_netmgr_txn_ops_t ops;

    iv_reactor_t *r;          /* NULL = 由调用方按节拍调 iv_agent_step() */
    iv_event_t   *nl_ev;
    iv_timer_t   *pb_timer;
    iv_timer_t   *nm_timer;

    int          wan_ifindex[IV_WAN_N];   /* 启动时按名字解析；0=未绑定 */
    int          wan_probe_idx[IV_WAN_N]; /* 绑该 WAN 的 probe 项下标；-1=无 */
    uint8_t      wan_link_up[IV_WAN_N];   /* 最近一次 netlink carrier 状态 */
    uint64_t     door_events;             /* S4.5 累计门开事件次数（M4 阶段可观测用） */

    uint8_t      mn_last[IV_RECOVERY_MASTER_N]; /* S4.4 主网每路上一次 MN 状态（1/2） */
    uint8_t      snmp_state[IV_AGENT_SNMP_TARGET_N]; /* 0 空闲 / 1 在飞 / 2 就绪 */
    uint32_t     snmp_reqid[IV_AGENT_SNMP_TARGET_N]; /* request-id 递增种子 */
    uint64_t     snmp_runs;           /* 累计完成的采集轮数 */
    uint64_t     snmp_slot_noval;     /* 累计"该项无值"（无应答/异常值）槽位次数 */

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
iv_report_t      *iv_agent_report(iv_agent_t *a); /* 读上报链统计/状态用 */
uint64_t          iv_agent_door_events(const iv_agent_t *a); /* S4.5 门开事件计数 */

/* S4.1 摄像机探测句柄只读出口（M5-S5.1 新增：Web 总览要读 CNS 六路与检测方式）。
 * 返回内部句柄，调用方只读；a 为 NULL 返回 NULL。 */
const iv_camdet_t *iv_agent_camdet(const iv_agent_t *a);

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

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_AGENT_H */
