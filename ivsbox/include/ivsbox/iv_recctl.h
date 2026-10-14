/*
 * IVSBox 断网录像状态机（libivmodules，功能开发计划 M6-S6.3）
 *
 * ============================ 它是什么 ============================
 * S6.3 的触发条件收口成一个**纯逻辑**小状态机：主网出口与平台链路**均不可用**
 * 且保持 `hold_ms` → 逐路 `start(id, url)`；恢复（任一出口可用 **或** 平台链路
 * 可用）持续 `debounce_ms` → 逐路 `stop(id)`。它自己不发任何 I/O——动作经
 * `iv_recctl_ops_t` 回调交给装配层（生产＝iv_media 桥接，单测＝假实现）。
 *
 * **判据口径（2026-10-14 实现定稿，登记）**：
 *   - 出口可用 ＝ `iv_netmgr_wan_is_up()` 的迟滞结论（无线 || 有线任一）；
 *     裸 `iv_probe_state()` 单次超时会误判，绝不用（S4.4 同口径）。
 *   - 平台可用 ＝ `iv_transport_state() == IV_TRANSPORT_UP`（平台 TCP 链路）。
 *   - 计划原文"主网＋平台双确认"落在"出口全断 ＋ 平台断"上：出口全断时平台
 *     TCP 必然也断，双条件取交集只是保守化，等价且不引入新判据。
 *   - hold/debounce **已接配置**（2026-10-14 S6.5 前置）：`camera.rec.hold_ms` /
 *     `camera.rec.debounce_ms`（默认 10000 / 60000，下界 1000 ms＝推进节拍）；
 *     cfg 传 0 ⇒ 回落到下面的 `IV_RECCTL_*_MS_DEFAULT`。
 *
 * **为什么单独成模块**：状态机要被单测用假时间轴逐拍验证（沿序列/保持/防抖/
 * 重触发），埋进 iv_agent 的 step 里就只能整机测。iv_agent 每拍喂一次输入即可。
 *
 * **URL 表的来源（2026-10-14 已接线）**：`camera.ch.N.url`（第 N 路拉流地址，
 * 值即 RTSP URL，**含账密**）＋ `camera.ch.N.record`（该路是否录像）——**两者
 * 同时满足**该路才进表，由 app 层（`cfg_rec_sources()`）填 `n_url`/`url[]`。
 * **空串路被本模块直接跳过**（`url[i][0] == '\0'`）⇒ 「配几路录几路」，
 * 中间空洞不会对不存在的地址发 start。
 * ⚠ 键**早已存在**于默认表（旧注释"无 RTSP 地址键"已过期，此处更正）；
 *   地址长度受 `IV_CFG_STR_MAX`（160 含 NUL）限制。
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
#define IV_RECCTL_HOLD_MS_DEFAULT     10000u  /* 双断保持 10 s 才开录 */
#define IV_RECCTL_DEBOUNCE_MS_DEFAULT 60000u  /* 恢复保持 60 s 才停录 */

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
    /* 逐路动作；返回 IV_OK 才算受理。arg 为 ops_arg。 */
    int  (*start)(int id, const char *url, void *arg);
    int  (*stop)(int id, void *arg);
    void *arg;
} iv_recctl_ops_t;

typedef struct {
    iv_recctl_cfg_t cfg;
    uint8_t         recording;     /* 0/1：本机在录 */
    uint64_t        down_since_ms; /* 0 = 尚未全断 */
    uint64_t        up_since_ms;   /* 0 = 尚未恢复 */
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
 * wan_up：任一 WAN 出口的迟滞结论；plat_up：平台 TCP 链路是否 UP。
 * 动作（start/stop）通过 ops 回调同步发生——ops 实现必须非阻塞或可接受的
 * 毫秒级（生产＝iv_media 同步回环 HTTP，符合该约束）。
 * 返回本拍发生的状态变化数（0/1；start 或 stop 各算一次）。
 */
int iv_recctl_step(iv_recctl_t *r, int wan_up, int plat_up, uint64_t now_ms,
                   const iv_recctl_ops_t *ops);

/* 手动停录（关停/复位时用）：在录则逐路 stop 并清状态。返回停掉的路数。 */
int iv_recctl_stop_all(iv_recctl_t *r, const iv_recctl_ops_t *ops);

/* 当前是否在录。 */
int iv_recctl_recording(const iv_recctl_t *r);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_RECCTL_H */
