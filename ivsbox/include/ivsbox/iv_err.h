/*
 * IVSBox 故障码字典（跨端共用码空间）
 *
 * 码空间与单片机工程 `General_Version/main`（`APP/User/inc/error.h`，对齐基准 mtime
 * 2026-07-11 10:51）**共用**：同一个 uint32 数值在两侧必须含义一致，**任何码值不得
 * 单方面改动**；新增或调整前须与单片机侧对齐后同步。
 *
 * 本头**只放跨端共用的 uint32 故障码**。本地 API 返回码是另一套负数命名空间
 * （见 `ivsbox/iv_ret.h`），严禁混入本头——混用会破坏"与单片机侧码值对齐"这条约束。
 *
 * 位段（本侧按 32 位字划分，用于拆解与打印；**其中只有类型段有对方依据**）：
 *   bit 31-28  类型 type     1=电量 ELEC  2=网络 NET  3=传感器 SENSOR（0 保留；4~15 待协商）
 *                            依据：单片机侧 `ERR_TYPE_*_BASE = 类型 << 28`。
 *                            取类型值时**以 BASE 为准**，勿参考该工程的 `ErrorType_e`
 *                            枚举——它写成 ELEC=0/NET=1/SENSOR=2，与该工程自己的 BASE
 *                            不自洽、且全工程未被使用；照它改成 0/1/2 会让全部码值错位。
 *   bit 27-20  类别 category 类型内的故障类别，如 NET 的 4=摄像机故障、10=摄像机丢包。
 *                            单片机侧只用到 ≤12，**8 位位宽是本侧约定，非对方声明**。
 *   bit 19-16  序号 index    同类别的实例号，如摄像机 1~6；无实例时为 0。
 *                            单片机侧只用到 1~6（`det.c` 用 BASE + (num<<16), num=0~5），
 *                            **4 位位宽是本侧约定**。上限 IV_ERR_IDX_MAX；由运行期实例号
 *                            构造码值前必须先自行校验，越界应报错，**不得静默进位**到
 *                            相邻字段（加法写法下 idx=15 会撞上类别段的下一个码）。
 *   bit 15-0   保留，固定为 0（两侧一致，单测已断言）。
 *
 * 示例：0x20410000 = type 2(NET) | category 4(摄像机故障)<<20 | index 1(1 号机)<<16
 *
 * 与单片机工程的两处命名差异（**不影响数值**）：
 *   1. 本文件所有码加 `IV_ERR_` 前缀，避免污染全局命名空间；
 *   2. 单片机工程把 CAMERA 误拼为 CAREMA，本文件按正确拼写 CAMERA。
 *
 * 尚未冻结的跨端约定（不是普通待办）：① 类型段 0x4~0xF 的归属；② 上报串格式
 * `ERR=<数量>,<XXXXXXXX>,...;`（单片机侧 `Error_Get_Codesbuf`）两侧必须一致。
 */
#ifndef IVSBOX_IV_ERR_H
#define IVSBOX_IV_ERR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 位段定义与组合 / 拆解
 * ------------------------------------------------------------------------- */
#define IV_ERR_OK ((uint32_t)0u) /* 无故障（单片机侧各类型 NORMAL 亦为 0） */

#define IV_ERR_TYPE_SHIFT 28
#define IV_ERR_CAT_SHIFT  20
#define IV_ERR_IDX_SHIFT  16

#define IV_ERR_TYPE_MASK ((uint32_t)0xF0000000u)
#define IV_ERR_CAT_MASK  ((uint32_t)0x0FF00000u)
#define IV_ERR_IDX_MASK  ((uint32_t)0x000F0000u)

/* 各字段上限：构造码值前必须自查，越界会静默进位到相邻字段 */
#define IV_ERR_TYPE_MAX ((uint32_t)0xFu)
#define IV_ERR_CAT_MAX  ((uint32_t)0xFFu)
#define IV_ERR_IDX_MAX  ((uint32_t)0x0Fu)

/* 编译期越界拦截（C11 _Static_assert，文件作用域与块作用域均可用） */
#define IV_ERR_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#define IV_ERR_ASSERT_FIELDS(type, cat, idx)                                  \
    IV_ERR_STATIC_ASSERT((uint32_t)(cat) <= IV_ERR_CAT_MAX,                   \
                         "IV_ERR: category overflows its 8-bit field");       \
    IV_ERR_STATIC_ASSERT((uint32_t)(idx) <= IV_ERR_IDX_MAX,                   \
                         "IV_ERR: index overflows its 4-bit field");          \
    IV_ERR_STATIC_ASSERT((uint32_t)(type) <= IV_ERR_TYPE_MAX,                 \
                         "IV_ERR: type overflows its 4-bit field")

/* 取位段（返回值均为右对齐的小整数） */
#define IV_ERR_TYPE_OF(code) \
    ((uint32_t)(((uint32_t)(code) & IV_ERR_TYPE_MASK) >> IV_ERR_TYPE_SHIFT))
#define IV_ERR_CAT_OF(code) \
    ((uint32_t)(((uint32_t)(code) & IV_ERR_CAT_MASK) >> IV_ERR_CAT_SHIFT))
#define IV_ERR_IDX_OF(code) \
    ((uint32_t)(((uint32_t)(code) & IV_ERR_IDX_MASK) >> IV_ERR_IDX_SHIFT))

/* 按 类型 / 类别 / 序号 组合码值 */
#define IV_ERR_MAKE(type, cat, idx)                                                        \
    ((((uint32_t)(type)) << IV_ERR_TYPE_SHIFT) | (((uint32_t)(cat)) << IV_ERR_CAT_SHIFT) | \
     (((uint32_t)(idx)) << IV_ERR_IDX_SHIFT))

/* 类型取值 */
#define IV_ERR_TYPE_ELEC   1u
#define IV_ERR_TYPE_NET    2u
#define IV_ERR_TYPE_SENSOR 3u

/* 类型基址（数值与单片机工程的 ERR_TYPE_*_BASE 一致） */
#define IV_ERR_TYPE_ELEC_BASE   IV_ERR_MAKE(IV_ERR_TYPE_ELEC, 0, 0)   /* 0x10000000 */
#define IV_ERR_TYPE_NET_BASE    IV_ERR_MAKE(IV_ERR_TYPE_NET, 0, 0)    /* 0x20000000 */
#define IV_ERR_TYPE_SENSOR_BASE IV_ERR_MAKE(IV_ERR_TYPE_SENSOR, 0, 0) /* 0x30000000 */

/* 各类型内的码值：IV_ERR_<类型>(类别, 序号)，等价于单片机工程的 BASE | (类别<<20) | (序号<<16) */
#define IV_ERR_ELEC(cat, idx)   (IV_ERR_TYPE_ELEC_BASE | IV_ERR_MAKE(0, (cat), (idx)))
#define IV_ERR_NET(cat, idx)    (IV_ERR_TYPE_NET_BASE | IV_ERR_MAKE(0, (cat), (idx)))
#define IV_ERR_SENSOR(cat, idx) (IV_ERR_TYPE_SENSOR_BASE | IV_ERR_MAKE(0, (cat), (idx)))

/* ---------------------------------------------------------------------------
 * 电量类 ELEC（type 1）
 * ------------------------------------------------------------------------- */
#define IV_ERR_ELEC_MAIN_AC      IV_ERR_ELEC(1, 0) /* 0x10100000 断电 */
#define IV_ERR_ELEC_ACDC_MODULE  IV_ERR_ELEC(2, 0) /* 0x10200000 ACDC 模块故障 */
#define IV_ERR_ELEC_AC_OVER_V    IV_ERR_ELEC(3, 0) /* 0x10300000 过压 */
#define IV_ERR_ELEC_AC_LOW_V     IV_ERR_ELEC(4, 0) /* 0x10400000 低压 */
#define IV_ERR_ELEC_AC_OVER_C    IV_ERR_ELEC(5, 0) /* 0x10500000 过流 */
#define IV_ERR_ELEC_AC_LEAKAGE   IV_ERR_ELEC(6, 0) /* 0x10600000 漏电 */
#define IV_ERR_ELEC_AC_MCB       IV_ERR_ELEC(7, 0) /* 0x10700000 空开故障 */
#define IV_ERR_ELEC_GROUND_FAULT IV_ERR_ELEC(8, 0) /* 0x10800000 地线缺失 */
#define IV_ERR_ELEC_AC_LN_FAULT  IV_ERR_ELEC(9, 0) /* 0x10900000 零火反接 */

/* ---------------------------------------------------------------------------
 * 网络类 NET（type 2）
 * ------------------------------------------------------------------------- */
#define IV_ERR_NET_LAN_PORT    IV_ERR_NET(1, 0) /* 0x20100000 LAN 端口故障 */
#define IV_ERR_NET_MAIN_FAULT  IV_ERR_NET(2, 0) /* 0x20200000 主网 1 故障 */
#define IV_ERR_NET_MAIN2_FAULT IV_ERR_NET(3, 0) /* 0x20300000 主网 2 故障 */

#define IV_ERR_NET_CAMERA1_FAULT IV_ERR_NET(4, 1) /* 0x20410000 摄像机 1 故障 */
#define IV_ERR_NET_CAMERA2_FAULT IV_ERR_NET(4, 2) /* 0x20420000 摄像机 2 故障 */
#define IV_ERR_NET_CAMERA3_FAULT IV_ERR_NET(4, 3) /* 0x20430000 摄像机 3 故障 */
#define IV_ERR_NET_CAMERA4_FAULT IV_ERR_NET(4, 4) /* 0x20440000 摄像机 4 故障 */
#define IV_ERR_NET_CAMERA5_FAULT IV_ERR_NET(4, 5) /* 0x20450000 摄像机 5 故障 */
#define IV_ERR_NET_CAMERA6_FAULT IV_ERR_NET(4, 6) /* 0x20460000 摄像机 6 故障 */

#define IV_ERR_NET_MAIN_DELAY  IV_ERR_NET(5, 0) /* 0x20500000 主网 1 延时告警 */
#define IV_ERR_NET_MAIN2_DELAY IV_ERR_NET(6, 0) /* 0x20600000 主网 2 延时告警 */

#define IV_ERR_NET_CAMERA1_DELAY IV_ERR_NET(7, 1) /* 0x20710000 摄像机 1 延时告警 */
#define IV_ERR_NET_CAMERA2_DELAY IV_ERR_NET(7, 2) /* 0x20720000 摄像机 2 延时告警 */
#define IV_ERR_NET_CAMERA3_DELAY IV_ERR_NET(7, 3) /* 0x20730000 摄像机 3 延时告警 */
#define IV_ERR_NET_CAMERA4_DELAY IV_ERR_NET(7, 4) /* 0x20740000 摄像机 4 延时告警 */
#define IV_ERR_NET_CAMERA5_DELAY IV_ERR_NET(7, 5) /* 0x20750000 摄像机 5 延时告警 */
#define IV_ERR_NET_CAMERA6_DELAY IV_ERR_NET(7, 6) /* 0x20760000 摄像机 6 延时告警 */

#define IV_ERR_NET_MAIN_LOSS  IV_ERR_NET(8, 0) /* 0x20800000 主网 1 丢包告警 */
#define IV_ERR_NET_MAIN2_LOSS IV_ERR_NET(9, 0) /* 0x20900000 主网 2 丢包告警 */

#define IV_ERR_NET_CAMERA1_LOSS IV_ERR_NET(10, 1) /* 0x20A10000 摄像机 1 丢包告警 */
#define IV_ERR_NET_CAMERA2_LOSS IV_ERR_NET(10, 2) /* 0x20A20000 摄像机 2 丢包告警 */
#define IV_ERR_NET_CAMERA3_LOSS IV_ERR_NET(10, 3) /* 0x20A30000 摄像机 3 丢包告警 */
#define IV_ERR_NET_CAMERA4_LOSS IV_ERR_NET(10, 4) /* 0x20A40000 摄像机 4 丢包告警 */
#define IV_ERR_NET_CAMERA5_LOSS IV_ERR_NET(10, 5) /* 0x20A50000 摄像机 5 丢包告警 */
#define IV_ERR_NET_CAMERA6_LOSS IV_ERR_NET(10, 6) /* 0x20A60000 摄像机 6 丢包告警 */

#define IV_ERR_NET_MAIN_IP_UNCONFIG  IV_ERR_NET(11, 0) /* 0x20B00000 主网 1 IP 未配置 */
#define IV_ERR_NET_MAIN2_IP_UNCONFIG IV_ERR_NET(12, 0) /* 0x20C00000 主网 2 IP 未配置 */

/* ---------------------------------------------------------------------------
 * 传感器类 SENSOR（type 3）
 * ------------------------------------------------------------------------- */
#define IV_ERR_SENSOR_TEMP_HIGH  IV_ERR_SENSOR(1, 0) /* 0x30100000 温度高 */
#define IV_ERR_SENSOR_TEMP_LOW   IV_ERR_SENSOR(2, 0) /* 0x30200000 温度低 */
#define IV_ERR_SENSOR_HUMI_HIGH  IV_ERR_SENSOR(3, 0) /* 0x30300000 湿度高 */
#define IV_ERR_SENSOR_BOX_TILT   IV_ERR_SENSOR(4, 0) /* 0x30400000 箱体倾斜 */
#define IV_ERR_SENSOR_DOOR_OPEN  IV_ERR_SENSOR(5, 0) /* 0x30500000 箱门打开 */
#define IV_ERR_SENSOR_WATER_LEAK IV_ERR_SENSOR(6, 0) /* 0x30600000 漏水 */
#define IV_ERR_SENSOR_SPD_FAULT  IV_ERR_SENSOR(7, 0) /* 0x30700000 防雷失效 */

/* ---------------------------------------------------------------------------
 * 接口
 * ------------------------------------------------------------------------- */

/*
 * 码值 → 稳定 ASCII 符号名（如 "NET_CAMERA1_FAULT"，即单片机工程符号名去掉类型前缀、
 * CAREMA 改为 CAMERA）。返回的是常量字符串，不写共享缓冲，任意线程可调用；
 * 0 → "OK"；未登记的码 → "UNKNOWN"（需要码值请直接按 uint32 打印）。
 * 板端控制台为 GBK，故本接口**只用 ASCII**；面向界面/上报的中文名另开接口，不要加在这里。
 */
const char *iv_strerror(uint32_t code);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_ERR_H */
