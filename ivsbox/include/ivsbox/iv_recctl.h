/*
 * IVSBox 断网录像状态机（libivmodules，功能开发计划 M6-S6.3）
 *
 * ============================ 它是什么 ============================
 * S6.3 的触发条件收口成一个**纯逻辑**小状态机：**主网（有线 WAN）不可用**
 * 且保持 `hold_ms` → 逐路 `start(id, url)`；主网恢复持续 `debounce_ms` →
 * 逐路 `stop(id)`。它自己不发任何 I/O——动作经 `iv_recctl_ops_t` 回调交给
 * 装配层（生产＝iv_media 桥接，单测＝假实现）。
 *
 * ============================ 判据口径（2026-10-28 用户拍板，覆盖 10-14 旧口径） ============================
 * **只看主网（`IV_WAN_WIRED`）是否可用，4G 与平台链路一律不参与。**
 *
 *   - 判据来源＝`iv_netmgr_wan_is_up(&nm, IV_WAN_WIRED)` 的迟滞结论（连续
 *     `netmgr.fail_n` 次探测失败才判死）。不用裸 `iv_probe_state()`：单次超时
 *     会把"抖一下"判成断网，录像忽开忽关。
 *   - **旧口径是"出口全断（4G 与有线都断）＋ 平台 TCP 断"**。现场证伪了它：
 *     主网断了但 4G 顶着、平台走 4G 照常 ESTABLISHED ⇒ 双断永不成立 ⇒
 *     主网故障期间一段视频都不录，而这正是最需要留证据的时刻。
 *     用户 2026-10-28 明确：**"只有当主网断开时，就录像，和 4G 没有关系"**。
 *   - 有线接口压根没绑（`wan_ifindex[WIRED]==0`）时 `wan_is_up` 恒 0 ⇒ 视为
 *     "无主网" ⇒ 会录。这与"没主网就该录"一致；纯 4G 部署的箱子因此会**一直录**
 *     （这是用户口径的直接推论，登记在案，不另设例外）。
 *   - 触发与停止是同一条判据的正反两面：主网断 ⇒ 录，主网恢复（防抖后）⇒ 停。
 *
 * ============================ 启动失败必须能定位（2026-10-28） ============================
 * "start 被受理"不等于"真的在录"：桥接只回 HTTP 200（bird_media 只是置标志，
 * ffmpeg 在 worker 里异步拉流）。摄像机账密错、编码不匹配、路径写错，
 * 受理都会成功，然后 worker 里静静失败 —— 板子上只剩一个空目录。
 *
 * 因此本模块做**两级确认**，并把原因留成人能看懂的中文：
 *
 *   1) `ops->start()` 必须带回失败原因（`why`）：配置错（非 rtsp:// / 无主机 /
 *      非 IPv4）与网络不通（连接超时 / 拒连 / 无路由）在这一步就分开了，
 *      由桥接层做预检（见 `iv_media_record_start()`）。
 *   2) `ops->status()` 在 `IV_RECCTL_CONFIRM_MS` 后复查真实状态：受理了却没
 *      在录 ⇒ 记"已受理但未进入录像（拉流失败：账密/编码/路径）"。这一条
 *      是账密错误**唯一**能被发现的路径。
 *
 * 每路的最近一次失败原因存在 `ch_why[]`，由 `iv_recctl_fail_reason()` 读出
 * （ctl 的 `record.status` 与 Web 展示走它）。成功之后自动清空，不留陈旧结论。
 *
 * 失败重试走**退避**（`IV_RECCTL_RETRY_MS`），不是每拍重来：预检要建 TCP 连接，
 * 每拍对一个连不上的摄像机重试会把 Reactor 单线程拖住。
 *
 * **为什么单独成模块**：状态机要被单测用假时间轴逐拍验证（沿序列/保持/防抖/
 * 重触发/确认/退避），埋进 iv_agent 的 step 里就只能整机测。iv_agent 每拍喂一次输入即可。
 *
 * **URL 表的来源**：`camera.ch.N.url`（第 N 路拉流地址，值即 RTSP URL，
 * **含账密**）＋ `camera.ch.N.record`（该路是否录像）——**两者同时满足**
 * 该路才进表，由 app 层（`cfg_rec_sources()`）填 `n_url`/`url[]`。
 * **空串路被本模块直接跳过**（`url[i][0] == '\0'`）⇒ 「配几路录几路」，
 * 中间空洞不会对不存在的地址发 start。
 * 地址长度受 `IV_CFG_STR_MAX`（160 含 NUL）限制。
 * ⚠ `IV_RECCTL_CH_N`＝8 是**录像侧**路数；网络探测（`IV_CAMDET_CH_N`）仍是 6
 *   （平台上报帧 `CNS` 段固定 6 宽）⇒ 第 7/8 路只录不测。
 */
#ifndef IVSBOX_IV_RECCTL_H
#define IVSBOX_IV_RECCTL_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量：`CH_N`/`URL_MAX` 是**模块容量**；两个 `*_MS_DEFAULT` 只是"cfg 传 0 时"
 * 的回落值（生产路径由 `camera.rec.hold_ms` / `.debounce_ms` 填）。
 * ------------------------------------------------------------------------- */
#define IV_RECCTL_CH_N         8
#define IV_RECCTL_URL_MAX      256
#define IV_RECCTL_WHY_MAX      96   /* 单路失败原因文本上限（含 NUL） */
#define IV_RECCTL_HOLD_MS_DEFAULT     10000u  /* 主网断保持 10 s 才开录 */
#define IV_RECCTL_DEBOUNCE_MS_DEFAULT 60000u  /* 恢复保持 60 s 才停录 */
/* 启动后多久复查真实状态。ffmpeg 拉 RTSP + 出首帧通常在 1~2 s 内；取 3 s
 * 既躲开正常建流窗口，又不至于让"账密错"这种硬失败拖到下一轮排查。 */
#define IV_RECCTL_CONFIRM_MS   3000u
/* 全部路都起不来时的重试间隔。预检要连 TCP，不能每拍重来。 */
#define IV_RECCTL_RETRY_MS     30000u

/* ---------------------------------------------------------------------------
 * 配置 / 动作回调 / 句柄
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t hold_ms;     /* 0 → 默认 */
    uint32_t debounce_ms; /* 0 → 默认 */
    int      n_url;       /* 0..IV_RECCTL_CH_N；0 ⇒ 恒不动作 */
    char     url[IV_RECCTL_CH_N][IV_RECCTL_URL_MAX]; /* 录像源 RTSP 地址 */
} iv_recctl_cfg_t;

typedef struct {
    /*
     * 启动一路录像。返回 IV_OK 才算受理。
     * **失败时必须在 why 写入可读原因**（why 可为 NULL 表示调用方不关心）；
     * 约定是"配置错误：…" / "摄像机网络不可达：…" 这类能照着修的句子，
     * 不许写裸错误码。
     */
    int  (*start)(int id, const char *url, char *why, size_t whycap, void *arg);
    int  (*stop)(int id, void *arg);
    /*
     * 复查该路**真实**在录状态：1 在录 / 0 未在录 / <0 查询失败（回调不可用）。
     * 可空 —— 为空时跳过启动确认（只剩"受理即成功"的口径，诊断能力降级）。
     */
    int  (*status)(int id, void *arg);
    void *arg;
} iv_recctl_ops_t;

typedef struct {
    iv_recctl_cfg_t cfg;
    uint8_t         recording;     /* 0/1：本机在录 */
    uint64_t        down_since_ms; /* 0 = 主网尚未断 */
    uint64_t        up_since_ms;   /* 0 = 主网尚未恢复 */
    /* ---- 启动确认（见文件头"两级确认"）---- */
    uint8_t         pending;       /* 1 = 已发起 start，等 status 复查 */
    uint64_t        pending_ms;    /* 发起时刻 */
    /* ---- 失败重试退避 ---- */
    uint64_t        next_try_ms;   /* 0 或 now < 它 ⇒ 本拍不重试 */
    /* ---- 每路失败原因（成功即清） ---- */
    uint8_t         ch_failed[IV_RECCTL_CH_N];
    char            ch_why[IV_RECCTL_CH_N][IV_RECCTL_WHY_MAX];
    /* ---- S6.5 存储容量闸门（由 iv_recctl_gate() 每拍刷新）----
     * gate_block：没空间时不**开**新录像；gate_stop：空间告急/故障时**立即停**。
     * 二者都由装配层从 iv_storage 的决策里喂进来（见 iv_storage.h）。 */
    uint8_t         gate_block;
    uint8_t         gate_stop;
} iv_recctl_t;

/* ---------------------------------------------------------------------------
 * 生命周期与驱动
 * ------------------------------------------------------------------------- */
/* 填默认配置（hold/debounce 默认、n_url=0）。 */
void iv_recctl_cfg_default(iv_recctl_cfg_t *cfg);

/* 初始化（拷 cfg）。重复 init 幂等。 */
int iv_recctl_init(iv_recctl_t *r, const iv_recctl_cfg_t *cfg);

/*
 * 每拍喂一次（建议 ≤1 s 节拍；装配层在 probe/netmgr tick 里调）。
 *
 * main_up：**主网（有线 WAN）是否可用**（`iv_netmgr_wan_is_up(nm, IV_WAN_WIRED)`）。
 *          4G 与平台链路**不参与**判据（见文件头口径）。
 * 动作（start/stop/status）通过 ops 回调同步发生——ops 实现必须非阻塞或可接受的
 * 毫秒级（生产＝iv_media 同步回环 HTTP ＋ 一次短 TCP 预检）。
 *
 * 返回本拍发生的状态变化数（0/1；start 或 stop 各算一次）。
 */
int iv_recctl_step(iv_recctl_t *r, int main_up, uint64_t now_ms,
                   const iv_recctl_ops_t *ops);

/*
 * S6.5 存储容量闸门（装配层每拍在 step **之前**调用；判据来自 iv_storage）：
 *   allow_new=0 ⇒ 抑制**开**新录像（水位 ≥90% 或 SD 故障）——已录的不受影响；
 *   stop_rec=1  ⇒ 若在录则**立即**逐路 stop（水位 ≥95% 或 SD 故障），不等恢复防抖。
 * 返回本次因 stop_rec 停掉的路数（0 = 没有需要停的）。step() 里也有一份等价兜底，
 * 以防调用方漏调本函数。幂等：可每拍调用。
 */
int iv_recctl_gate(iv_recctl_t *r, int allow_new, int stop_rec,
                   const iv_recctl_ops_t *ops);

/* 手动停录（关停/复位时用）：在录则逐路 stop 并清状态。返回停掉的路数。 */
int iv_recctl_stop_all(iv_recctl_t *r, const iv_recctl_ops_t *ops);

/* 当前是否在录。 */
int iv_recctl_recording(const iv_recctl_t *r);

/*
 * 该路最近一次失败原因（诊断用，ctl/Web 展示）：
 *   返回 1 = 有原因（写入 buf，永不空串）；0 = 无（在录 / 未尝试过）；
 *   -1 = 入参非法。
 * 成功后自动清空，读到的永远是"当前"结论，不是历史。
 */
int iv_recctl_fail_reason(const iv_recctl_t *r, int id, char *buf, size_t cap);

/* 该路当前是否处于失败态（1/0）。 */
int iv_recctl_failed(const iv_recctl_t *r, int id);

/* 运行期换配置（2026-10-22：`camera.ch.N.url` / `.record` / `camera.rec.*` 热生效）。
 *
 * **没在录** ＝ 等价于 init；**正在录** ＝ 先用 `ops` 把这一轮逐路 stop 收口，
 * 再按新表重开 —— 不能只改 `cfg`：被删掉/改掉的那一路 recorder 会漏在后台
 * 继续写 SD 卡，而状态机已经不认识它了。
 * 因此"在录却没给 `ops`"按 `IV_EINVAL` 处理（不知道该叫谁停）。
 *
 * `now_ms` 是**失败退避的基准时间**（与 step 同一个时钟）。必须传真实值：
 * 传 0 时无法算退避，只能退化成"下一拍立刻重试"，一个连不上的摄像机
 * 会把 Reactor 单线程按每拍 500 ms 预检超时反复拖住（2026-10-28 现场踩到：
 * 热更新重开失败后 11 s 就重试，正是因为当时没有 now 可传）。
 *
 * 返回 `IV_OK` / `IV_EINVAL`。 */
int iv_recctl_set_cfg(iv_recctl_t *r, const iv_recctl_cfg_t *cfg,
                      const iv_recctl_ops_t *ops, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_RECCTL_H */
