/*
 * 自愈：故障沿 → 电源通道断电重启（libivmodules，功能开发计划 M4-S4.4）
 *
 * ============================ 它是什么 ============================
 * 把"网络状态从**正常**变**故障**的**单沿**"翻译成"**采集板电源通道断电 5 s
 * 再上电**"这一条动作。**只有这一点功能**：
 *   - **没有**规则引擎 / 上游关联 / 熔断状态机 / 每小时每天限频（架构 §7.6
 *     那套"四道保护"是旧演绎，现网没有，2026-10-13 作废）；
 *   - **不探测**：状态由调用方从 S4.1 `iv_camdet` 的 `on_change` 沿回调灌进来；
 *   - **不落盘**：限次/冷却计数**只在内存**（照抄现网，参考项目也是 RAM 态）。
 *
 * ============================ 触发（硬口径） ============================
 *   `old == 1（正常） && new == 2（故障）` ⇒ 动作。**其余一律不动作**：
 *     0→2（开机就是坏的，避免上电风暴）、4→2 / 5→2（延时/丢包变故障）、
 *     1→4 / 1→5（劣化）、2→1 / 2→4（恢复）全部**不触发**。
 *   —— 与参考项目 `det.c: det_main_network_and_camera_network()` 逐字同口径。
 *
 * ============================ 动作（硬口径） ============================
 *   主网（MN 两路**任一**）1→2 ⇒ 重启**继电器 1**（port 0）
 *   任一摄像机（CNS 六路任一）1→2 ⇒ 重启**继电器 2~8 全部**（port 1~7，整排
 *     重启、不定位单台；依据《智能箱通用功能统计.xlsx》"功能说明"第 13 条
 *     "设备自动重启｜网络故障后，自动重启**所有输出端口**"）
 *   动作内容：`0xD1`（`IV_FRAME_CMD_POWER`）下发两次 —— 先
 *     `{"port":<0~7>,"status":0}`（断电），**5 s** 后
 *     `{"port":<0~7>,"status":1}`（上电）。
 *     JSON 载荷格式逐字对齐架构 §18.2 的冻结口径。
 *
 * ⚠ **与现网代码的一处已知差异（登记，不掩盖）**：`General_Version - 无SADP`
 * 的 `det.c` 当前只对 `RELAY_2` / `RELAY_3` 两个通道 `app_set_net_reload_num()`，
 * 并未遍历继电器 2~8。本模块按**计划 ＋ 需求文档**（"所有输出端口"）做成
 * 整排重启；差异与"输出路数可配置"（需求文档第 4 条备注）一起登记为待定项。
 *
 * ============================ 为什么有"待发队列"（重要，不是过度设计） ============================
 * 一次摄像机故障要发 **7 条** `0xD1`（整排），但 `iv_link` 的配对规则是
 * **「同 cmd 在飞唯一」（第二笔 `IV_EBUSY`，见 iv_link.h）** —— 连发 7 条
 * 只会有第 1 条被接受、后 6 条全被拒。而"7 个通道同时断电"这件事**必须**
 * 发出去，不能丢。
 * 于是本模块自持一个**待发 FIFO**（`pend[]`，16 槽 ＝ 8 通道 ×(OFF+ON) 上限），
 * 由 `iv_recovery_tick()` **每拍最多下发 1 条**：下发成功即出队；收到
 * `IV_EBUSY` 说明上一笔还在飞，**保留队头、下一拍再试**（这就是串行化的全部
 * 机制，没有"在飞完成回调"那种耦合）。装配层 200 ms 一拍 ⇒ 整排 7 条的
 * 下发在毫秒级完成（采集板应答即释放槽位），最坏（链路无应答）也按
 * `iv_link` 的 2 s 超时逐条推进。
 *   ⚠ 代价（登记）：**"断电 5 s"因此是近似值** —— 实际是
 *   "OFF 发出 ＋ 5 s ＋ 队内等待"。整排场景下各通道错开约一拍，仍在同一秒级。
 *
 * ============================ 时序与限次（硬口径） ============================
 *   - **冷却**：每次上电后 `cooldown_ms`（默认 60 s ＝ 现网 `NETWAOK_RELOAD_TIME`）
 *     内不再对该通道动作；
 *   - **限次**：每通道最多动作 `reboot_max` 次（＝现网 `net_reload`），
 *     **耗尽后不再动作、只保持告警**；`reboot_max == 0` ⇒ **禁用自愈**；
 *   - 参考项目的 `app_sys_net_relay_reload_num_times()` 在冷却结束后会**自动
 *     重新武装**同一通道（一次故障沿连做 N 轮，不管网络是否已恢复），且
 *     `net_reload_num[]` **全程无复位点** ⇒ 一轮用尽后该通道永久锁死到下次重启。
 *     本模块**不照抄这两点**：动作**只由新的故障沿驱动**（网络已恢复就不会白
 *     白断电摄像头），限次是"剩余次数"、`iv_recovery_set_limits()` 可重置。
 *
 * ============================ 与"电路"的边界 ============================
 *   继电器在**采集板侧**：本模块只产出 `0xD1` 载荷，经 `tx` 回调交出去
 *   （装配层接 `iv_report_send_cmd()` → `iv_link` → 串口）。因此本模块
 *   **不引 iv_link、不引 iv_report、不开任何 fd**，单测可以完全无硬件驱动。
 *
 * ============================ 并发与生命周期 ============================
 *   - 单写者：全部接口只在装配层（Reactor 单线程）调用，内部无锁；
 *   - 零 malloc：`iv_recovery_t` 由调用方持有（约 400 B）；
 *   - 时间一律由调用方以 `now_ms` 传入（单测可注入时间轴）。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK      成功
 *   IV_EINVAL  参数非法（NULL / 通道号越界 / 状态不在 0~2 / cap 不足）
 *   IV_ESTATE  未 init（opened 位为 0）就调用
 *   IV_ENOSPC  载荷缓冲放不下（build_payload）
 */
#ifndef IVSBOX_IV_RECOVERY_H
#define IVSBOX_IV_RECOVERY_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量：电源通道与触发源
 * ------------------------------------------------------------------------- */

/* 电源输出通道（继电器）总数。现网 `RELAY_DEV`：RELAY_1..RELAY_8 ⇒ 8 路。 */
#define IV_RECOVERY_PORT_N 8u

/* 待发 FIFO 槽位：8 通道 × (OFF ＋ ON) 是理论上限（同一通道不会同时排两条）。 */
#define IV_RECOVERY_PEND_MAX 16u

/* 通道分工：继电器 1（下标 0）给主网；继电器 2~8（下标 1~7）给摄像机供电。 */
#define IV_RECOVERY_PORT_MASTER 0u
#define IV_RECOVERY_PORT_CAM_LO 1u
#define IV_RECOVERY_PORT_CAM_HI 7u

/* 主网路数（上报字段 MN 两路）/ 摄像机路数（上报字段 CNS 六路）。 */
#define IV_RECOVERY_MASTER_N 2u
#define IV_RECOVERY_CAM_N    6u

/* 断电时长：现网 `app_sys_net_operate_relay()` 判据 `(now - relay_time) > 5000`。 */
#define IV_RECOVERY_OFF_MS_DEFAULT 5000u

/* 冷却时长：现网 `NETWAOK_RELOAD_TIME = 60`（秒）。 */
#define IV_RECOVERY_COOLDOWN_MS_DEFAULT 60000u

/* `0xD1` 载荷缓冲上限：最长 `{"port":7,"status":2}` ＝ 21 B，给到 48 留余量。 */
#define IV_RECOVERY_PAYLOAD_MAX 48u

/* 五态编码（与 S4.1 `iv_camdet_status_t` / 现网 `NET_STATUS_E` **同值**）。 */
#define IV_RECOVERY_NO_IP  0 /* 未配置该路 */
#define IV_RECOVERY_NORMAL 1 /* 网络正常 */
#define IV_RECOVERY_FAULT  2 /* 网络故障 */
#define IV_RECOVERY_DELAY  4 /* 延时 */
#define IV_RECOVERY_LOSS   5 /* 丢包 */
/* （**没有 3**：现网里 3 被主网报文的"网线断开"占用，摄像机侧用不到。） */

/* ---------------------------------------------------------------------------
 * `0xD1` 控制状态（架构 §18.2：`status` 0 关 / 1 开 / 2 重启）
 * 本模块**只发 OFF ＋ 5 s 后 ON**；`RESTART`（采集板侧 15 s 断电）留给"系统
 * 操作"里的人工单通道重启，本步不用、仅登记以免与 0/1 混淆。
 * ------------------------------------------------------------------------- */
typedef enum {
    IV_RECOVERY_PWR_OFF     = 0,
    IV_RECOVERY_PWR_ON      = 1,
    IV_RECOVERY_PWR_RESTART = 2
} iv_recovery_pwr_t;

/* ---------------------------------------------------------------------------
 * 回调
 * ------------------------------------------------------------------------- */

/* 交出 `0xD1` 载荷字节（data 在回调返回后失效；回调须自行复制或用完即发）。
 * 返回 IV_OK 表示已受理；负码表示未受理（**不消费限次、不入断电态**，等下次沿）。
 * 可 NULL —— 此时本模块只记账、不下发（单测/干跑）。 */
typedef int (*iv_recovery_tx_cb)(const void *data, uint16_t len, void *arg);

/* 每次**有效动作**（含 OFF 与 ON）回调一次，供装配层打日志/上报。
 * port 为通道号（0~7）、status 为 `0xD1` 的 0/1。可 NULL。 */
typedef void (*iv_recovery_act_cb)(int port, int status, void *arg);

/* ---------------------------------------------------------------------------
 * 配置
 * ------------------------------------------------------------------------- */
typedef struct {
    int      reboot_max;  /* 每通道最多动作次数；**0 ＝ 禁用自愈**（现网 net_reload=0） */
    uint32_t cooldown_ms; /* 动作后冷却；0 → IV_RECOVERY_COOLDOWN_MS_DEFAULT */
    uint32_t off_ms;      /* 断电时长；0 → IV_RECOVERY_OFF_MS_DEFAULT */

    iv_recovery_tx_cb  tx;        /* 下发通道；NULL → 只记账 */
    void              *tx_arg;
    iv_recovery_act_cb on_action; /* 诊断回调；NULL → 不回调 */
    void              *cb_arg;
} iv_recovery_cfg_t;

/* ---------------------------------------------------------------------------
 * 句柄（内部字段直接暴露，符合本工程零 malloc 风格；调用方只读）
 * ------------------------------------------------------------------------- */

/* 通道相位。**数值是内部约定**，用 iv_recovery_port_phase() 读。 */
#define IV_RECOVERY_ST_IDLE    0u /* 空闲（可被新的故障沿触发） */
#define IV_RECOVERY_ST_Q_OFF   1u /* OFF 已排队/待发 */
#define IV_RECOVERY_ST_OFF     2u /* OFF 已发出，断电中（等 off_ms） */
#define IV_RECOVERY_ST_Q_ON    3u /* ON 已排队/待发 */

typedef struct {
    iv_recovery_cfg_t cfg;

    /* 每通道状态机：**完全独立**（一个摄像机通道的冷却不影响其它通道） */
    struct {
        uint8_t  phase;        /* IV_RECOVERY_ST_* */
        uint8_t  credit;       /* 剩余动作次数（初值＝reboot_max） */
        uint64_t off_until_ms; /* phase=OFF 时：上电时刻 */
        uint64_t cd_until_ms;  /* 冷却截止（上电后设） */
    } ch[IV_RECOVERY_PORT_N];

    /* 待发 FIFO（见文件头"为什么有'待发队列'"） */
    struct {
        uint8_t port;
        uint8_t status;
    } pend[IV_RECOVERY_PEND_MAX];
    uint8_t pend_n;

    uint64_t actions;      /* 累计**成功下发**的报文条数（OFF ＋ ON） */
    uint64_t cycles;       /* 累计完成的"断电—上电"轮数 */
    uint64_t blocked_cool; /* 因冷却期未过被挡下的次数 */
    uint64_t blocked_lim;  /* 因限次耗尽被挡下的次数 */
    uint64_t disabled;     /* 因 reboot_max==0（禁用自愈）被挡下的次数 */
    uint64_t tx_busy;      /* 因同 cmd 在飞（IV_EBUSY）让出、留待下拍的次数 */
    uint64_t tx_err;       /* 其它下发失败（负码非 EBUSY）次数 */
    uint64_t pend_full;    /* 待发队列满而推迟入队的次数 */
    uint8_t  opened;
} iv_recovery_t;

/* ---------------------------------------------------------------------------
 * 生命周期与运行期设置
 * ------------------------------------------------------------------------- */

/* 填默认：reboot_max=0（**默认禁用**，与现网出厂值 net_reload=0 同口径）、
 * cooldown=60 s、off=5 s、回调全 NULL。 */
void iv_recovery_cfg_default(iv_recovery_cfg_t *cfg);

/* 初始化：清零 ＋ 拷配置 ＋ `credit = reboot_max`。cfg 为 NULL 时取默认。 */
void iv_recovery_init(iv_recovery_t *r, const iv_recovery_cfg_t *cfg);

/* 热更新限次与冷却并**重置限次**（credit = reboot_max；冷却/断电中的通道不动）。
 * 平台/Web 改 `netthr.reboot_max` / `netthr.reboot_interval_s` 后调用。
 * reboot_max<0 → 0；cooldown_ms==0 → 默认。返回 IV_OK / IV_EINVAL。 */
int iv_recovery_set_limits(iv_recovery_t *r, int reboot_max, uint32_t cooldown_ms);

/* 清全部通道状态（限次回满、冷却/断电中清空）。返回 IV_OK / IV_EINVAL。 */
int iv_recovery_reset(iv_recovery_t *r);

/* 停用（幂等）：幂等清 opened。 */
void iv_recovery_close(iv_recovery_t *r);

/* ---------------------------------------------------------------------------
 * 载荷构造（单独导出，便于单测做**字节级**黄金校验）
 * ------------------------------------------------------------------------- */

/* 构造 `0xD1` 载荷：`{"port":<0~7>,"status":<0|1|2>}`（架构 §18.2 冻结格式）。
 * 返回载荷长度（不含 NUL；最长 21）；port/status 非法 → IV_EINVAL；
 * cap 放不下 → IV_ENOSPC。out/ cap 非法 → IV_EINVAL。 */
int iv_recovery_build_payload(int port, int status, char *out, size_t cap);

/* ---------------------------------------------------------------------------
 * 状态沿入口（装配层直连 S4.1 `iv_camdet_cfg_t::on_change` 之类）
 * idx 越界 / r 为 NULL / 未 init → 静默忽略（回调里不便返错）。
 * 只有 `old==1 && new==2` 才可能动作，详见文件头"触发"。
 * ------------------------------------------------------------------------- */

/* 主网第 idx 路（0/1）的状态沿 ⇒ 触发通道 0（继电器 1）。 */
void iv_recovery_on_master_change(iv_recovery_t *r, int idx, int old_st,
                                  int new_st, uint64_t now_ms);

/* 摄像机第 idx 路（0~5）的状态沿 ⇒ 触发通道 1~7（继电器 2~8 **全部**）。 */
void iv_recovery_on_camera_change(iv_recovery_t *r, int idx, int old_st,
                                  int new_st, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 驱动
 * ------------------------------------------------------------------------- */

/* 周期驱动（装配层 200 ms 一次）：
 *   ① 断电到点（phase=OFF 且 now>=off_until）⇒ 该通道的 ON 入待发队；
 *   ② **最多下发 1 条**待发报文（`IV_EBUSY` ⇒ 留队头、下一拍再试）。
 * 返回本次**成功下发的报文条数**（0 或 1）；IV_EINVAL/IV_ESTATE 见文件头。 */
int iv_recovery_tick(iv_recovery_t *r, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 查询（诊断/单测）
 * ------------------------------------------------------------------------- */

/* 通道相位（IV_RECOVERY_ST_*）；port 越界 → IV_EINVAL。 */
int iv_recovery_port_phase(const iv_recovery_t *r, int port);

/* 通道剩余动作次数；port 越界 → IV_EINVAL。 */
int iv_recovery_port_credit(const iv_recovery_t *r, int port);

/* 待发队列长度（0~IV_RECOVERY_PEND_MAX）；r 为 NULL → IV_EINVAL。 */
int iv_recovery_pending(const iv_recovery_t *r);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_RECOVERY_H */
