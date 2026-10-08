/*
 * 采集板状态镜像（libivmodules，功能开发计划 M2-S2.6）
 *
 * ============================ 它是什么 ============================
 * 采集板全部 IO / 阈值 / 继电器状态在 `ivsboxd` 内存中的**权威镜像**：
 *   - 来源：与 `iv_link` 的三条回调对接
 *       - `on_done(IV_OK, cmd, json, len)` 收到 0xE1 查询应答 → **全量刷新**
 *       - `on_upstream(cmd, json, len)`   收到 0xC2 事件上报 → **按 TAG 增量**
 *       - `on_upstream(cmd, data, len)`   收到 0xC1 箱门（data=0x01）→ **门状态**
 *       - 主动发 0xE1（iv_link_send）的入口由装配层在 `on_change(1)` 时调用
 *   - 出口：
 *       - `iv_status_snapshot_json(buf, size)` → 序列化给本地通道 `system.snapshot` /
 *         平台周期上报 / Web 展示（调用方负责分配足够缓冲）
 *   - 单写者：所有入口仅在装配层（Reactor 单线程）调用，**零锁**；
 *     读出口快照由装配层自己在 Reactor 内调用，不在 ISR 读。
 *
 * **不读不写串口**：字节流由 `iv_link` 处理，本模块只见 `iv_link_cb` 回调。
 *
 * ============================ JSON 提取 ============================
 * 字段名按架构 §18.2（2026-10-08 单片机源码核对）的口径取，不做模糊匹配。
 * 提取走**自研极简解析**（strstr + atof/atoi），不引 json-c：
 *   ① 这只是 `iv_status` 内部用的一次性映射，规模小；
 *   ② `libivcore` 纪律是"仅 libc"；json-c 已在 §15.5 划归 modules 层用；
 *   ③ 仅接受 `"key":<value>` 紧邻语法（前面可能有空白／对象边界）；
 *      **不**处理嵌套对象、数组、字符串转义 —— 采集板协议里的 JSON 全部扁平
 *      且只含数字与固定键（已由单片机源码验证）。
 * 提取失败 → 该字段保持当前值（**绝不静默归零**——不刷新比填错更安全）。
 *
 * ============================ 未知 / 缺失字段 ============================
 * - 字段首次刷新前为 `iv_status_t::valid` 对应位 = 0
 * - 整体镜像"是否被全量刷新过"用 `iv_status_valid_t` 位图统一记录
 *
 * ============================ 已知局限（与协议一致） ============================
 * - 协议**无请求号**，重复帧（真实事件 vs 重传）由镜像的"内容比较"天然幂等；
 *   0xC2 上报里 door 事件 DS=1/2、姿态 P 等每帧一个 TAG，**不关心是哪个告警**，
 *   只按 TAG 覆盖对应字段。
 * - 0xFF/0xE2 不在协议内（用户 2026-10-08 确认无心跳包/周期上报；移植代码属对端
 *   待清事项，**本工程不处理**）。
 *
 * ============================ 返回码 ============================
 *   iv_status_* 全部为 void / 简单状态：零 I/O、零内存分配、不会失败。
 *   iv_status_snapshot_json 在缓冲不足时返回 IV_ERANGE。
 */
#ifndef IVSBOX_IV_STATUS_H
#define IVSBOX_IV_STATUS_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_link.h" /* iv_link_done_cb / iv_link_upstream_cb 签名 */
#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 镜像
 *
 * 字段顺序与命名严格对照架构 §18.2 0xE1 应答键表；缺省 0 由各类型天然承载
 * （= 0 在多数场景下能区分"未填"与"真零"——电压电流不会真零，温度/湿度允许
 *  0，按 valid 位图判定）。
 * ------------------------------------------------------------------------- */
typedef struct {
    /* 运行状态（0xE1） */
    float    V;             /* 输入电压 V */
    float    A;             /* 输入电流 A */
    float    H;             /* 湿度 % */
    float    T;             /* 温度 ℃ */
    int32_t  DS;            /* 门状态：1=关 / 2=开 */
    int32_t  P;             /* 姿态 attitude_acc */
    int32_t  SPD;           /* 防雷：1 / 2 */
    int32_t  PA;            /* 设备供电 */
    int32_t  PV;            /* 输入电压状态 */
    char     APOWER[16];    /* 总功率（字符串化小数，源端不送 JSON number） */
    char     AKW[16];       /* 总用电量（同上） */
    /* 阈值（0xE1 + 0xF1） */
    int32_t  hv;            /* 高压 */
    int32_t  lv;            /* 低压 */
    int32_t  ov;            /* 过流 */
    int32_t  tu;            /* 温度上限 */
    int32_t  tl;            /* 温度下限（可负） */
    int32_t  hu;            /* 湿度上限 */
    int32_t  hl;            /* 湿度下限 */
    int32_t  sl;            /* 姿态 */
    int32_t  ld;            /* 漏电流 */
    /* 继电器（0xE1） */
    int32_t  RELAY[3];      /* 1=开 / 2=关 */
    float    CHV[3];        /* 各继电器电压 */
    float    CHA[3];        /* 各继电器电流 */
    float    POWER[3];      /* 各继电器功率 */
    float    ELEC[3];       /* 各继电器用电量 */
} iv_status_data_t;

/* valid 位图：第 N 位为 1 表示对应字段已被刷新至少一次。
 * 用位图而非"哨兵值"判定"是否已填"——避免"真实零值"与"未填"混淆。 */
typedef struct {
    uint32_t V : 1;
    uint32_t A : 1;
    uint32_t H : 1;
    uint32_t T : 1;
    uint32_t DS : 1;
    uint32_t P : 1;
    uint32_t SPD : 1;
    uint32_t PA : 1;
    uint32_t PV : 1;
    /* APOWER / AKW 用 char[16] 首字节非零判定"已填"，不占位图 */
    uint32_t hv : 1;
    uint32_t lv : 1;
    uint32_t ov : 1;
    uint32_t tu : 1;
    uint32_t tl : 1;
    uint32_t hu : 1;
    uint32_t hl : 1;
    uint32_t sl : 1;
    uint32_t ld : 1;
    uint32_t RELAY : 1;
    uint32_t CHV : 1;
    uint32_t CHA : 1;
    uint32_t POWER : 1;
    uint32_t ELEC : 1;
} iv_status_valid_t;

typedef struct {
    iv_status_data_t  data;
    iv_status_valid_t valid;
} iv_status_t;

/* 单条事件（0xC2）的解析结果——本结构只用于在快照里说明"最近一次事件"
 * （便于排错；不是协议要求）。snapshot 时附带 TAG + 字符串值。 */
typedef struct {
    char     tag[8];        /* 例如 "T" / "DS" / "OV" / "P" 等 */
    char     value[32];     /* 数字以 ASCII 文本写入 */
} iv_status_event_t;

typedef struct {
    iv_status_t         base;
    iv_status_event_t   last_event; /* 0xC2 上报解出的最近一条；置空标记 */
} iv_status_snapshot_t;

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* 初始化（全字段置零 + valid 清零 + last_event 置空）。 */
void iv_status_init(iv_status_t *st);

/* 装配层调用：把三个回调直接转给 iv_link 即可。S2.6 入口全在这里。
 * 返回该填入的 `iv_link_upstream_cb` / `iv_link_done_cb`（上下行回调统一
 * 由此内部消化）。 */
iv_link_upstream_cb iv_status_on_upstream(void);
iv_link_done_cb     iv_status_on_done(void);

/* 内部：处理一条 0xE1 完整应答（供 iv_link on_done 回调直接转发） */
void iv_status_handle_query(iv_status_t *st,
                            const uint8_t *json, uint16_t len);

/*
 * 处理一条 0xC1/0xC2 上行帧（装配层从 iv_link 的 on_upstream 回调里调用）。
 * 0xC1 door → 门状态；0xC2 event → 按 TAG 增量；其它 cmd 一律 no-op。
 * 适配 iv_link_upstream_cb 4 参签名：arg 字段填 iv_status_t* 指针。
 */
void iv_status_handle_upstream(uint8_t cmd, const uint8_t *data,
                                uint16_t len, void *arg);

/*
 * 输出快照为可打印摘要（人读，调试用）：
 *   "V=220.0 A=0.5 T=25.0 H=60.0 DS=1 hv=280 ..."
 * 缓冲 0 字节视为长度探测（返回所需长度，不含 '\0'）。buf 太短返 IV_ERANGE。
 */
int iv_status_format(const iv_status_t *st, char *buf, size_t buf_size);

/* 序列化快照为单层 JSON（无嵌套，无空字段）：
 *   {"V":220.0,"T":25.0,...}
 * 供本地通道 / 平台 / Web 读。已 valid 字段全写；未 valid 字段省略。
 * 缓冲 0 字节为长度探测。buf 太短返 IV_ERANGE。
 */
int iv_status_snapshot_json(const iv_status_t *st, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_STATUS_H */
