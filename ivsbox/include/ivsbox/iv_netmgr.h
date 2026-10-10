/*
 * iv_netmgr —— 双 WAN 状态机（libivmodules，M3-S3.3）
 *
 * ============================ 定位与口径 ============================
 * 架构 §7.2：默认「**无线优先、无线失败切有线、无线稳定恢复后切回无线**」。
 * 组合态：WIRELESS_UP / WIRELESS_DEGRADED / SWITCH_TO_WIRED / WIRED_UP /
 *         SWITCH_TO_WIRELESS / BOTH_DOWN。
 *
 * 「无线」＝ 4G（板端网卡 `usb0`），「有线」＝ `eth0` —— 架构 §18.3 板端实测冻结。
 *
 * ============================ 边界（干什么 / 不干什么） ============================
 * 干：
 *   1) 吃**探活结论**（每条 WAN 的 ok/fail，来自 S3.2）做 fail_n/ok_n **迟滞判定**；
 *   2) 按 §7.2 跑**六态状态机**（含 hold_s 回切稳定期、双断 BOTH_DOWN）；
 *   3) 在切换时**编排 §7.2 七步事务**并逐步回调调用方。
 * 不干：
 *   - **不自己探活**（探活归 S3.2，本模块只收结论）；
 *   - **不自己收发网络 I/O**：七步事务的每一步都经 `iv_netmgr_txn_ops_t` 注入
 *     （风格同 iv_transport 的注入式 I/O）。生产可传 `iv_netmgr_ops_default()`：
 *     其中 `switch_route` 走 netlink（RTM_NEWROUTE/RTM_DELROUTE）真改默认出口，
 *     其余四项为 NULL，由装配层按需补。
 *   - 不管 4G 数据面本身「拨没拨起来」（§18.3 ⚠②：板端当前无自动拨号脚本）。
 *
 * 零 malloc、不引 pthread/Reactor；时间一律由调用方以 `now_ms` 传入（便于单测）。
 * 本模块不引用其它业务模块（只经公共头 + 回调交互）。
 */
#ifndef IVSBOX_IV_NETMGR_H
#define IVSBOX_IV_NETMGR_H

#include <stdint.h>

#include "ivsbox/iv_ret.h"

/* 网卡名长度（含结尾 NUL），与内核 IFNAMSIZ 同量级；头文件不引 <net/if.h> */
#define IV_NETMGR_NAME_MAX 16u

/* 两条 WAN 的固定序号（数组下标即此值） */
typedef enum {
    IV_WAN_WIRELESS = 0, /* 4G / usb0 */
    IV_WAN_WIRED    = 1, /* 有线 / eth0 */
    IV_WAN_N        = 2
} iv_wan_t;

/* 组合态（架构 §7.2） */
typedef enum {
    IV_NETMGR_WIRELESS_UP = 0,      /* 无线在用 */
    IV_NETMGR_WIRELESS_DEGRADED,    /* 无线连续失败，尚未确认切走 */
    IV_NETMGR_SWITCH_TO_WIRED,      /* 事务执行中：切有线 */
    IV_NETMGR_WIRED_UP,             /* 有线在用 */
    IV_NETMGR_SWITCH_TO_WIRELESS,   /* 事务执行中：切回无线 */
    IV_NETMGR_BOTH_DOWN             /* 两条都不可用：只告警、不动作 */
} iv_netmgr_state_t;

/* 传输模式（配置键 `sys.transport.mode`，架构 §10.3；"用哪个网口由它隐含"） */
typedef enum {
    IV_NETMGR_MODE_WIRED_ONLY    = 1, /* 只用有线，不切换 */
    IV_NETMGR_MODE_WIRELESS_ONLY = 2, /* 只用无线，不切换 */
    IV_NETMGR_MODE_BOTH          = 3, /* 双链路启用、不自动切换（活动口＝无线优先） */
    IV_NETMGR_MODE_AUTO          = 4  /* 自动选择：跑 §7.2 完整状态机 */
} iv_netmgr_mode_t;

/* 一条 WAN 的描述 */
typedef struct {
    int      ifindex;    /* 内核 ifindex；0 = 未绑定（该 WAN 视为不可用） */
    uint32_t gateway_be; /* 默认网关（**网络序**）；0 = 无网关（点对点/ECM 链路可用） */
    char     name[IV_NETMGR_NAME_MAX]; /* 仅诊断用（可空） */
} iv_netmgr_wan_t;

/* ---------------------------------------------------------------------------
 * 七步事务的可注入执行点（架构 §7.2）。任一指针为 NULL = 该步不执行。
 * 返回 IV_OK 视为成功；其它负码视为该次切换失败（见 iv_netmgr_tick 说明）。
 * ------------------------------------------------------------------------- */
typedef struct {
    /* 1. 冻结新的低优先级（媒体）发送任务 */
    int  (*freeze_low_prio)(void *arg);

    /* 2. 切换默认出口：from_ifindex -> to_ifindex/to_gateway_be */
    int  (*switch_route)(void *arg, int from_ifindex, int to_ifindex,
                         uint32_t to_gateway_be);

    /* 3. 关闭绑定旧接口的 TCP 连接及其远程流 */
    int  (*close_old_tcp)(void *arg, int old_ifindex);

    /* 4~6. 用新接口重新解析地址/建连、鉴权与会话恢复、按优先级补传 */
    int  (*reconnect)(void *arg, int new_ifindex);

    /* 7. 稳定期后恢复媒体上传与大文件下载 */
    int  (*resume_media)(void *arg, int ifindex);

    void *arg; /* 以上回调的公用上下文 */
} iv_netmgr_txn_ops_t;

/* 默认执行点：`switch_route` 用 netlink 真改默认路由；其余为 NULL。
 * （NULL 项＝该步不执行，装配层可自行组合。）永不返回 NULL。 */
const iv_netmgr_txn_ops_t *iv_netmgr_ops_default(void);

/* 配置 */
typedef struct {
    iv_netmgr_mode_t mode;        /* 0 视为 AUTO */
    iv_netmgr_wan_t  wan[IV_WAN_N];
    uint32_t fail_n;              /* 连续失败阈值：判该 WAN 不可用；0 视为默认 3 */
    uint32_t ok_n;                /* 连续成功阈值：判该 WAN 可用；  0 视为默认 2 */
    uint32_t hold_s;              /* 回切前无线需保持稳定的秒数；   0 视为默认 30 */
    uint32_t stable_s;            /* 切到新 WAN 后恢复媒体的稳定秒数；0 视为默认 10 */
    const iv_netmgr_txn_ops_t *ops; /* NULL -> iv_netmgr_ops_default() */
} iv_netmgr_cfg_t;

/* 状态（内部字段直接暴露，符合本工程零 malloc 风格；调用方只读） */
typedef struct {
    iv_netmgr_cfg_t   cfg;
    iv_netmgr_state_t state;
    uint8_t           active;              /* 当前活动 WAN（iv_wan_t） */
    uint8_t           wan_up[IV_WAN_N];    /* 迟滞判定后的可用性 */
    uint8_t           media_suspended;     /* 1 = 低优先级（媒体）发送已冻结 */
    uint8_t           media_resume_pending;/* 1 = 稳定期到点后待恢复媒体 */
    uint8_t           opened;
    uint8_t           reserve;
    uint32_t          fail_streak[IV_WAN_N];
    uint32_t          ok_streak[IV_WAN_N];
    uint64_t          wan_up_since[IV_WAN_N]; /* 该 WAN 变为可用的时刻（回切 hold_s 计时） */
    uint64_t          media_resume_at;        /* 计划恢复媒体的时刻 */
    uint64_t          retry_at;               /* 事务失败后的重试保护时刻 */
    uint64_t          switches;               /* 累计成功切换次数 */
    uint64_t          txn_fail;               /* 累计事务失败次数 */
} iv_netmgr_t;

/* 生命周期 */
void iv_netmgr_cfg_default(iv_netmgr_cfg_t *cfg);
void iv_netmgr_init(iv_netmgr_t *m);

/* 初始化并进入初始态（按 mode：AUTO/BOTH 起始于无线优先）。
 * 已 open 再调返回 IV_ESTATE；cfg==NULL 返回 IV_EINVAL。 */
int iv_netmgr_open(iv_netmgr_t *m, const iv_netmgr_cfg_t *cfg, uint64_t now_ms);

/* 喂入某条 WAN 的**一次探活结论**（来自 S3.2 的该 WAN 判定）。
 * ok!=0 记成功、否则记失败；内部按 fail_n/ok_n 迟滞更新该 WAN 可用性，
 * 并**立即跑一次状态推进**（可能触发切换事务）。返回值恒为 IV_OK（喂结论本身不失败）。 */
int iv_netmgr_note_probe(iv_netmgr_t *m, iv_wan_t wan, int ok, uint64_t now_ms);

/* 周期性驱动：处理 hold_s 回切、稳定期后恢复媒体、事务失败重试窗口。
 * 建议由 Reactor timer（1 s 级）调用。 */
int iv_netmgr_tick(iv_netmgr_t *m, uint64_t now_ms);

/* 只读读数 */
iv_netmgr_state_t iv_netmgr_state(const iv_netmgr_t *m);
iv_wan_t          iv_netmgr_active_wan(const iv_netmgr_t *m);
int               iv_netmgr_active_ifindex(const iv_netmgr_t *m);
int               iv_netmgr_media_suspended(const iv_netmgr_t *m);
int               iv_netmgr_wan_is_up(const iv_netmgr_t *m, iv_wan_t wan);
uint64_t          iv_netmgr_switches(const iv_netmgr_t *m);
uint64_t          iv_netmgr_txn_fail(const iv_netmgr_t *m);
const char       *iv_netmgr_state_name(iv_netmgr_state_t s);

#endif /* IVSBOX_IV_NETMGR_H */
