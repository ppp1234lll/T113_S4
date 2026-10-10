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
 *   - **平台地址为空则不启平台链路**：`server_host` 空时 iv_report 内部不开传输，
 *     `iv_agent_netmgr` 的换出口重连钩子此时是空转（`iv_report_reconnect`
 *     返回 IV_ESTATE）。现场填入 `net.server.*.host/port` 后自然生效。
 *   - **devid（TID）/ mod（型号）无配置键来源**：本步分别取配置注入值与 NULL，
 *     待工厂身份读取（private 分区）落地后接入。
 */
#ifndef IVSBOX_IV_AGENT_H
#define IVSBOX_IV_AGENT_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_netlink.h"
#include "ivsbox/iv_netmgr.h"
#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WAN 接口名长度（含结尾 NUL） */
#define IV_AGENT_WAN_NAME_MAX 16u

/* 总线节拍：probe 驱动（发探/收结果/超时判定）与 netmgr 驱动（迟滞/回切）。 */
#define IV_AGENT_PROBE_TICK_MS  200u
#define IV_AGENT_NETMGR_TICK_MS 1000u

/* 探活目标路数（probe.target1 / target2） */
#define IV_AGENT_TARGET_N 2

/* ---------------------------------------------------------------------------
 * 配置（由 app 层从 iv_config 解析后填入；本层不依赖 iv_config）
 * ------------------------------------------------------------------------- */
typedef struct {
    /* ---- 控制板上报链（S3.6）---- */
    const char *serial_path; /* NULL → 默认 /dev/ttySAC5 */
    const char *queue_dir;   /* NULL → 默认 /mnt/UDISK/ivsbox/queue */
    const char *server_host; /* 平台地址；NULL/"" → 平台链路不启动 */
    uint16_t    server_port;
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

    uint64_t     now_ms;
    uint8_t      opened;
} iv_agent_t;

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

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_AGENT_H */
