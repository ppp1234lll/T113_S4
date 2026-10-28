/*
 * 故障码表与合并（libivmodules，2026-10-28）
 *
 * ============================ 它是什么 ============================
 * 平台上报帧里 `ERR=<数量>,<码>,<码>;` 这一段的**唯一生产者**。
 * 码值、类别分层与判定口径全部照抄参考实现「雅下市电 GD32F460 版」
 * （`General_Version/main/APP/User/inc/error.h` ＋ `APP/User/src/error.c`，
 * 2026-10-28 逐条核对，值不得改动 —— 平台按码解码）：
 *
 *   0x1XXXXXXX  电量类    断电 / ACDC 模块 / 过压 / 低压 / 过流 / 漏电 /
 *                         空开 / 地线缺失 / 零火反接
 *   0x2XXXXXXX  网络类    LAN 口 / 主网1 / 主网2 / 摄像机1..6
 *                         × 故障 / 延时 / 丢包 ＋ 主网 IP 未配置
 *   0x3XXXXXXX  传感器类  温度高 / 温度低 / 湿度高 / 倾斜 / 门开 / 漏水 / 防雷失效
 *
 * 第 20..23 位是「项」（同一类里的第几种），第 16..19 位是「序号」
 * （只有摄像机用到：1..6 逐路相差 `1 << 16`）。
 *
 * ============================ 分工（用户 2026-10-28 裁定） ============================
 *   电量 / 传感器：**采集板判断**。主板不重算 —— 分压、互感器、温湿度传感器、
 *     水浸、门磁、防雷都在采集板那一侧，主板拿到的只有它送来的 JSON。两边都算
 *     必然给出两套结论（而且主板的"阈值"只是转发下去的配置，不代表测量值）。
 *   网络：**主板判断**。LAN 口载波、主网探活、摄像机 PING 全在 T113 上，
 *     采集板根本看不见这些事实。
 *   ⇒ 本模块就是这两半的**合并点**：采集板来的码进 `board[]`，主板算出来的进
 *     `net[]`，输出时并集去重（同一码两边都有也只出现一次）。
 *
 * ============================ 采集板的 ERR 从哪儿来 ============================
 * `0xE1` 全量应答 JSON 里的 `ERR` 键 —— 也就是 `iv_status` 镜像的 `ERR` 字段：
 *
 *     {"V":220.5,"A":0.12,...,"ERR":"2,10100000,30100000"}
 *
 * 值是参考实现 `Error_Get_Codesbuf()` 的**原文**：`ERR=<数量>,<码>,<码>;`。
 * 因此解析必须容忍三个变体（采集板固件怎么拼都得认）：
 *   - 带 `ERR=` 前缀 / 不带；
 *   - 结尾带 `;` / 不带；
 *   - 数量字段与真码数不一致。
 *
 * 三条硬口径：
 *   ① **数量字段不采信**：以真正解析出的码为准，数量对不上记 `board_mismatch`
 *      后照码处理。否则一个手误的计数就能让平台少显示几条故障。
 *   ② **只采信 0x1 / 0x3 类**：网络码一律丢弃并计入 `board_rejected` —— 网络
 *      以主板结论为准（见上一条分工），采集板送来的网络码只会与主板打架。
 *   ③ **整体替换**：每解析到一次 `ERR` 就按新值重建 `board[]`（不是增量累加）
 *      ⇒ 采集板故障消失后，下一帧上报里就没有那一条，不需要额外的"清除"命令。
 *
 * ⚠ 采集板若还没在 0xE1 应答里加 `ERR` 键（2026-10-28 的现场固件就没有），
 * 本模块**什么都不出**，上报里只剩主板自己的网络码 —— 不猜、不编、不拿
 * "上次的值"冒充现在的值。
 *
 * ============================ 网络码怎么判（对齐参考实现 det.c / alarm.c） ============================
 * 输入全在装配层手上，本模块只做"状态码 → 故障码"的纯映射：
 *
 *   状态码（与 `iv_report_env_t::mn` / `iv_camdet_status_t` 同一套编码）
 *     0 未指定 IP / 1 正常 / 2 故障 / 3 网线断开 / 4 延时 / 5 丢包
 *
 *   主网1（有线 eth0）
 *     2 ⇒ `NET_MAIN_FAULT`      4 ⇒ `NET_MAIN_DELAY`
 *     3 ⇒ **不置主网码** —— 那是"网线被拔"，由 `NET_LAN_PORT` 表达（同一条物理
 *         事实报两个码只会让平台多出一条不相干的条目）。参考实现的主网状态来自
 *         ping，本来就没有"网线断开"这一态；这一态是本板 `mn_state_of()` 新增的。
 *     0/1/5 ⇒ 不置码（丢包只检测不上报，参考实现 2026-09 改口径后即如此）
 *
 *   主网2（4G / usb0，没有网线这一说）
 *     2 或 3 ⇒ `NET_MAIN2_FAULT`     4 ⇒ `NET_MAIN2_DELAY`
 *
 *   LAN 口（运维网口，同样是有线 eth0）
 *     载波 down ⇒ `NET_LAN_PORT`；**载波未知（未绑接口）时不置码** ——
 *     开机头几拍还没收到 netlink 事件就报"LAN 口故障"是纯误报。
 *
 *   摄像机 1..6（0 基下标 0..5）
 *     2 ⇒ `NET_CAREMA<n>_FAULT`      4 ⇒ `NET_CAREMA<n>_DELAY`
 *     5 ⇒ 不置码（同上：丢包不上报）；0/1 ⇒ 不置码
 *     ⚠ 参考实现的码名是 `CAREMA`（拼写如此），码值才是契约，名字照抄以便对照。
 *
 * ============================ 输出格式 ============================
 *   `n,C1,C2,...`：8 位**大写**十六进制、**升序去重**；空表输出 `0`。
 *   与 `iv_report_build_seg()` 的 `ERR=%s;` 直接对接（健康态 `ERR=0;`，与参考
 *   实现逐字一致）。
 *   排序只为"同一组故障每次拼出来都一样"（参考实现按发生顺序；平台按集合消费，
 *   顺序无意义，稳定输出更便于对日志和写用例）。
 *
 * ============================ 并发与生命周期 ============================
 *   - 单写者：只在装配层（Reactor 单线程）调用，内部无锁、零 malloc；
 *   - `iv_fault_t` 由调用方持有（约 0.3 KB）；
 *   - 全部接口对 NULL 入参安全返回 IV_EINVAL，绝不崩。
 */
#ifndef IVSBOX_IV_FAULT_H
#define IVSBOX_IV_FAULT_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量：类别掩码与码表（**值即契约**，改一个平台就解错）
 * ------------------------------------------------------------------------- */

#define IV_FAULT_TYPE_SHIFT 28u
#define IV_FAULT_TYPE_ELEC   0x10000000u /* 电量 */
#define IV_FAULT_TYPE_NET    0x20000000u /* 网络 */
#define IV_FAULT_TYPE_SENSOR 0x30000000u /* 传感器 */

/* 类别（高 4 位）——返回 1/2/3，其它值表示"不认识" */
#define IV_FAULT_TYPE_OF(code) (((code) >> IV_FAULT_TYPE_SHIFT) & 0xFu)

/* 同表上限：与参考实现 `ErrorFault_t::fault_index[32]` 一致 */
#define IV_FAULT_MAX 32

/* 上报串上限：board[] + net[] 最坏并集（各 32 条）× 9 字节 ＋ 数量 ＋ NUL */
#define IV_FAULT_STR_MAX (IV_FAULT_MAX * 2 * 9 + 8)

/* ---- 电量类（采集板判断） ---- */
#define IV_FAULT_ELEC_MAIN_AC      (IV_FAULT_TYPE_ELEC | (1u << 20)) /* 断电 */
#define IV_FAULT_ELEC_ACDC_MODULE  (IV_FAULT_TYPE_ELEC | (2u << 20)) /* ACDC 模块故障 */
#define IV_FAULT_ELEC_AC_OVER_V    (IV_FAULT_TYPE_ELEC | (3u << 20)) /* 过压 */
#define IV_FAULT_ELEC_AC_LOW_V     (IV_FAULT_TYPE_ELEC | (4u << 20)) /* 低压 */
#define IV_FAULT_ELEC_AC_OVER_C    (IV_FAULT_TYPE_ELEC | (5u << 20)) /* 过流 */
#define IV_FAULT_ELEC_AC_LEAKAGE   (IV_FAULT_TYPE_ELEC | (6u << 20)) /* 漏电 */
#define IV_FAULT_ELEC_AC_MCB       (IV_FAULT_TYPE_ELEC | (7u << 20)) /* 空开故障 */
#define IV_FAULT_ELEC_GROUND       (IV_FAULT_TYPE_ELEC | (8u << 20)) /* 地线缺失 */
#define IV_FAULT_ELEC_LN_FAULT     (IV_FAULT_TYPE_ELEC | (9u << 20)) /* 零火反接 */

/* ---- 网络类（主板判断） ---- */
#define IV_FAULT_NET_LAN_PORT      (IV_FAULT_TYPE_NET | (1u << 20)) /* LAN 口故障 */
#define IV_FAULT_NET_MAIN_FAULT    (IV_FAULT_TYPE_NET | (2u << 20)) /* 主网1 故障 */
#define IV_FAULT_NET_MAIN2_FAULT   (IV_FAULT_TYPE_NET | (3u << 20)) /* 主网2 故障 */
#define IV_FAULT_NET_MAIN_DELAY    (IV_FAULT_TYPE_NET | (5u << 20)) /* 主网1 延时告警 */
#define IV_FAULT_NET_MAIN2_DELAY   (IV_FAULT_TYPE_NET | (6u << 20)) /* 主网2 延时告警 */
#define IV_FAULT_NET_MAIN_LOSS     (IV_FAULT_TYPE_NET | (8u << 20)) /* 主网1 丢包（不置码，留表） */
#define IV_FAULT_NET_MAIN2_LOSS    (IV_FAULT_TYPE_NET | (9u << 20)) /* 主网2 丢包（同上） */
#define IV_FAULT_NET_MAIN_NOIP     (IV_FAULT_TYPE_NET | (11u << 20)) /* 主网1 IP 未配置（同上） */
#define IV_FAULT_NET_MAIN2_NOIP    (IV_FAULT_TYPE_NET | (12u << 20)) /* 主网2 IP 未配置（同上） */

/* 摄像机：`n` 为 1..6（与 `camera.ch.N` 的 N 同口径），逐路相差 `1 << 16` */
#define IV_FAULT_NET_CAM_FAULT(n) \
    (IV_FAULT_TYPE_NET | (4u << 20) | (((uint32_t)(n) & 0xFu) << 16))
#define IV_FAULT_NET_CAM_DELAY(n) \
    (IV_FAULT_TYPE_NET | (7u << 20) | (((uint32_t)(n) & 0xFu) << 16))
#define IV_FAULT_NET_CAM_LOSS(n) \
    (IV_FAULT_TYPE_NET | (10u << 20) | (((uint32_t)(n) & 0xFu) << 16))

/* ---- 传感器类（采集板判断） ---- */
#define IV_FAULT_SENSOR_TEMP_HIGH  (IV_FAULT_TYPE_SENSOR | (1u << 20)) /* 温度高 */
#define IV_FAULT_SENSOR_TEMP_LOW   (IV_FAULT_TYPE_SENSOR | (2u << 20)) /* 温度低 */
#define IV_FAULT_SENSOR_HUMI_HIGH  (IV_FAULT_TYPE_SENSOR | (3u << 20)) /* 湿度高 */
#define IV_FAULT_SENSOR_TILT       (IV_FAULT_TYPE_SENSOR | (4u << 20)) /* 倾斜 */
#define IV_FAULT_SENSOR_DOOR_OPEN  (IV_FAULT_TYPE_SENSOR | (5u << 20)) /* 门打开 */
#define IV_FAULT_SENSOR_WATER_LEAK (IV_FAULT_TYPE_SENSOR | (6u << 20)) /* 漏水 */
#define IV_FAULT_SENSOR_SPD_FAULT  (IV_FAULT_TYPE_SENSOR | (7u << 20)) /* 防雷失效 */

/* ---------------------------------------------------------------------------
 * 句柄
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t board[IV_FAULT_MAX]; /* 采集板送来的码（0x1/0x3 类） */
    uint8_t  nb;
    uint32_t net[IV_FAULT_MAX];   /* 主板自己判出来的码（0x2 类） */
    uint8_t  nn;

    /* 统计（诊断用：现场"平台没有故障码"时先看这几个数） */
    uint64_t board_rounds;   /* 解析采集板 ERR 的轮数 */
    uint64_t board_ms;       /* 最近一次解析的时刻（单调毫秒；0 = 从未） */
    uint64_t board_rejected; /* 采集板送来但被剔除的码数（网络类/非法格式） */
    uint64_t board_mismatch; /* 数量字段与实际码数不一致的次数 */
    uint64_t board_overflow; /* 码数超过 IV_FAULT_MAX 的轮数 */
} iv_fault_t;

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* 清零（等价于 `= {0}`；单独给出是为了让"整表复位"这个动作显式可读）。 */
void iv_fault_init(iv_fault_t *f);

/*
 * 采集一条采集板 `ERR` 值（`iv_status` 镜像的 `ERR` 字段），**整体替换**
 * `f->board[]`。解析规则见文件头（容忍 `ERR=` 前缀 / 尾分号 / 数量不符）。
 *
 *   err_field 为 NULL 或空串 ⇒ 表被清空（＝采集板报"无故障"），仍算一轮；
 *   `now_ms`  只用于统计（`board_ms`），可传 0。
 *
 * 返回：本次**采信**的码数（>= 0）；`f == NULL` 返回 IV_EINVAL。
 * 注：单条码格式非法的 token 被跳过并计入 `board_rejected`，不影响其它码。
 */
int iv_fault_apply_board(iv_fault_t *f, const char *err_field, uint64_t now_ms);

/*
 * 按主板掌握的网络事实**重算** `f->net[]`（整体替换，不是增量）。
 *
 *   mn_primary ：主网1（有线 eth0）状态码 0/1/2/3/4/5
 *   mn_backup  ：主网2（4G usb0）状态码，同上
 *   cns        ：六路摄像机状态码（0 基，0/1/2/4/5），NULL ⇒ 一路都不判
 *   cns_n      ：`cns` 有效长度（> IV_FAULT_MAX 按上限截断）
 *   lan_link   ：运维网口（eth0）载波：1 有 / 0 无 / **负数 = 未知**
 *                （未知时不置 `NET_LAN_PORT`，避免开机误报）
 *
 * 判定表见文件头。任何输入都不产生"猜"出来的码：状态码不在表内即不置码。
 * `f == NULL` 返回 IV_EINVAL，否则 IV_OK。
 */
void iv_fault_apply_net(iv_fault_t *f, int mn_primary, int mn_backup,
                        const int *cns, int cns_n, int lan_link);

/*
 * 组上报串：`n,C1,C2,...`（升序、去重、8 位大写十六进制）；空表 = `0`。
 * 不含 `ERR=` 前缀与结尾 `;`（由 `iv_report_build_seg()` 加）。
 *   IV_OK / IV_EINVAL / IV_ERANGE（cap 不够，缓冲里不写半截）
 */
int iv_fault_format(const iv_fault_t *f, char *out, size_t cap);

/* 合并后的条数（board ∪ net 去重后）。 */
int iv_fault_count(const iv_fault_t *f);

/* 合并后的表里是否有该码（含只看某一半的调用方自己挑表，不另设接口）。 */
int iv_fault_has(const iv_fault_t *f, uint32_t code);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_FAULT_H */
