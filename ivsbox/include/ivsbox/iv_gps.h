/*
 * GPS / NMEA 定位与时间（libivmodules，功能开发计划 M2-S2.5）
 *
 * ============================ 它是什么 ============================
 * 把串口上来的 **NMEA 0183 文本流**解析成两方面东西：
 *   1. **定位**：经纬度、海拔、速度、航向、定位质量、卫星数、HDOP；
 *   2. **时间**：UTC 日期时间，用于给无 RTC 的板子校时。
 * 并按"首次立即上报、此后位移阈值或最长周期上报"的策略，把**该不该上报**这个
 * 判断也收在模块里（调用方只需要问 `iv_gps_should_report()`）。
 *
 * **它不负责读串口**：字节从哪来由调用方决定（通常是 `iv_serial_open()` 拿到的
 * fd 加进 Reactor 后喂进来）。这样本模块可以用文件回放、管道、pty 任意驱动，
 * 单测完全不需要硬件。
 *
 * ============================ 解析库：vendored minmea ============================
 * 用上游 **`github.com/kosma/minmea`**（pin 到 commit `c43c9e7c5ed788122a9d8a5445679e1594f8a030`），
 * 原样放在 `third_party/minmea/`：
 *   - 单源文件 + 单头文件、**零动态内存**、核心**不用浮点**（换算函数才用），
 *     很适合资源受限目标（与本工程的纪律一致）；
 *   - 覆盖 GBS/GGA/GLL/GSA/GST/GSV/RMC/VTG/ZDA 九种语句；
 *   - 许可证 **WTFPL v2**（`COPYING`），上游在 `README.md` 里声明"若许可证不合意可函商改用
 *     其它任何许可证"，仓库另附 `LICENSE.MIT`。**本地登记见 `third_party/minmea/README.ivsbox.md`**。
 * 上游文件**一字未改**（可用 SHA256 复核），因此第三方源码单独用一套编译标志
 * （见 `Makefile` 的 `third_party/%` 规则：不施加本工程的 `-Werror`）。
 *
 * ============================ 硬件事实与当前可验证性（2026-09-30 板端实测） ============================
 *   - GPS 模块接在 **UART1**，引脚 **PD21 / PD22**（用户提供）。
 *   - **串口参数：9600 8N1、无流控**（2026-09-30 用户确认）。**注意它不是采集板的
 *     115200** —— 装配层打开这条链路时必须用 `IV_GPS_UART_BAUD`，不要用
 *     `iv_serial_cfg_default()` 填出来的值。
 *   - 但板端设备树里 **`/soc@3000000/uart@2500400`（UART1）是 `status = "disabled"`**，
 *     所以**没有 `/dev/ttySAC1` 节点**，串口上读不到任何 NMEA（已实测各串口无数据）。
 *     引脚组 `uart1_pins@0`（`function = "uart1"`）已挂在 uart1 的 `pinctrl-0` 上。
 *   - 启用它要**改设备树并重新烧录 boot**：`/dev/by-name/boot`（`mmcblk0p4`）是
 *     **raw 镜像**（挂载报 I/O error），不能靠"挂载后替换 dtb 文件"完成；板端也**没有**
 *     设备树 overlay 支持，**没有** `dtc/fdtput`。
 *   - ⇒ **本模块当前的板端验证方式是"灌样本"**（把录制/构造的 NMEA 喂进 `iv_gps_feed()`），
 *     真实硬件联调要等设备树启用。**这不是模块的缺陷，是部署侧的前置条件。**
 *
 * ============================ 关于配置项：本步刻意不新增分类 ============================
 * 上报阈值之类参数一律走 `iv_gps_cfg_t`（编译期默认值 ＋ 调用方可覆盖），
 * **不往 `ivsbox.json` 里加 `gps.*` 分类** —— 架构 §10.2 的 8 个一级分类是冻结口径，
 * 新增分类属于"对外可见的口径变更"，要先登记再加（见 TODO）。
 *
 * ============================ 时间同步的安全策略（重要） ============================
 * 校时**会改系统时间**，所以策略是"宁可不动，不可乱动"：
 *   1. **没有合法日期就不校时**：NMEA 的日期可能缺失或明显不对（模块冷启动、字段为空、
 *      解析出 1970/2000 之类）。日期早于 `min_year`（默认 2024）一律**拒绝**。
 *   2. **偏差分级**：`|Δ| < small_adj_ms`（默认 1 s）走 `adjtime()` **渐进调整**（不跳变，
 *      不影响单调时钟推进节奏）；`small_adj_ms ≤ |Δ| ≤ big_adj_ms`（默认 1 h）直接 `settimeofday()`；
 *      **`|Δ| > big_adj_ms` 只记审计、不调** —— 一次离谱的跳变会把全机日志时间线打乱，
 *      而"GPS 时间离谱"本身是更有价值的故障信号。
 *   3. **节流**：两次校时间隔不小于 `sync_interval_ms`（默认 60 s），避免每个定位周期都调时钟。
 *   4. **可注入**：`iv_gps_cfg_t.timeops` 允许替换 `adjtime` / `settimeofday`，
 *      单测据此断言"走哪条分支"，**绝不真的去改测试机的时间**。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK       成功
 *   IV_EINVAL   参数非法（NULL / 未初始化）
 *   IV_EAGAIN   当前没有"值得上报"的变化 / 距上次校时太近
 *   IV_ERANGE   偏差超出 `big_adj_ms`，**只审计未调整**
 *   IV_ESTATE   没有可用日期或时间，无法校时
 */
#ifndef IVSBOX_IV_GPS_H
#define IVSBOX_IV_GPS_H

#include <math.h> /* NAN：未知量的表示 */
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h> /* struct timeval：adjtime/settimeofday 的签名要用 */

#ifdef __cplusplus
extern "C" {
#endif

/* GPS 链路的波特率 —— **已确认的外部事实**（2026-09-30 用户确认）。
 * **不进 `ivsbox.json`**（用户 2026-09-30 决定：硬件确定不变），以编译期常量存在，
 * 理由见 `iv_serial.h` 的"平台事实"段与架构 §10.2。
 *
 * 本模块**不打开串口**（字节由调用方喂进来），这个常量是给**装配层**用的：
 * 打开 GPS 那一路串口时用它设 `iv_serial_cfg_t.baud`。
 * ⚠ **不要用 `iv_serial_cfg_default()` 的 115200** —— 那是**采集板**链路的参数。
 *   波特率错配的症状不是报错，而是**收不到任何数据**（或满屏乱码），
 *   在链路层看起来像"模块没上电"，极易误判成接线/电气问题。*/
#define IV_GPS_UART_BAUD ((unsigned)9600)

/* 单条 NMEA 语句的最大长度。上游默认 80，但 NMEA 0183 允许到 82 字节，
 * 且个别模块会把多条 GSV 拼得很长；给 128 留余量。**超出即整条丢弃**（见 iv_gps_feed）。*/
#define IV_GPS_LINE_MAX 128

/* 判定"定位无效"的哨兵值：与"数值为 0"区分开。*/
#define IV_GPS_NA (NAN)

/* ---------------------------------------------------------------------------
 * 定位快照
 *
 * 未知量一律填 `IV_GPS_NA`（而不是 0）—— 纬度 0 / 经度 0 是几内亚湾里的一个**真实位置**，
 * 用 0 表示"未知"会让"模块没定位"被误当成"设备在赤道"。
 * ------------------------------------------------------------------------- */
typedef struct {
    double latitude;     /* 十进制度，北正南负 */
    double longitude;    /* 十进制度，东正西负 */
    double altitude_m;   /* 海拔（米，GGA 的椭球/平均海平面高） */
    double speed_kph;    /* 地面速度（千米/小时） */
    double course_deg;   /* 航向（度，正北为 0） */
    double hdop;         /* 水平精度因子，越小越好 */
    int    fix_quality;  /* GGA 的 fix quality：0=无效 1=GPS 2=差分 … */
    int    satellites;   /* 参与定位的卫星数（GGA） */
    int    valid;        /* 1 = 有可用定位（RMC 的 A 或 GGA quality > 0）*/

    /* UTC 时间（来自 RMC / ZDA / GGA 的合并结果） */
    int  have_time; /* 1 = 下面的年月日时分秒都可信 */
    int  year;      /* 四位年 */
    int  month;     /* 1~12 */
    int  day;       /* 1~31 */
    int  hour;      /* 0~23 */
    int  minute;    /* 0~59 */
    int  second;    /* 0~59（NMEA 无闰秒字段）*/
    long usec;      /* 微秒部分（GGA/RMC 的小数秒）*/
} iv_gps_fix_t;

/* 统计计数：现场排障用（"到底有没有数据、是校验错还是没定位"）。*/
typedef struct {
    uint64_t sentences_ok;    /* 校验通过并成功解析的语句数 */
    uint64_t checksum_bad;    /* 校验和不符 */
    uint64_t malformed;       /* 结构不合法（长度、字符、缺字段）*/
    uint64_t too_long;        /* 超长被丢弃的语句数 */
    uint64_t noise_bytes;     /* 语句之外的噪声字节数 */
    uint64_t fix_updates;     /* 有效定位更新次数 */
    uint64_t time_sync_ok;    /* 实际执行的校时次数（adjtime + settimeofday）*/
    uint64_t time_sync_rejected; /* 因日期不合法 / 偏差过大被拒的次数 */
} iv_gps_stats_t;

/* 系统时钟操作（可注入，便于单测）。NULL 字段 ⇒ 用真实系统调用。*/
typedef struct {
    int (*adjtime_fn)(const struct timeval *delta, struct timeval *olddelta);
    int (*settimeofday_fn)(const struct timeval *tv, const struct timezone *tz);
} iv_gps_timeops_t;

typedef struct {
    /* 上报策略 */
    unsigned report_distance_m;  /* 位移超过它就要上报（默认 50）*/
    unsigned report_interval_ms; /* 距上次上报超过它就要上报（默认 60000）*/
    int      report_on_fix_change; /* 定位有效性变化时立即上报（默认 1）*/

    /* 校时策略 */
    int      sync_system_time; /* 1 = 允许校时（默认 1）；0 = 只解析不碰时钟 */
    int      min_year;         /* 日期下限，早于它一律拒绝（默认 2024）*/
    long     small_adj_ms;     /* 小于它 → adjtime 渐进（默认 1000）*/
    long     big_adj_ms;       /* 大于它 → 只审计不调（默认 3600000）*/
    unsigned sync_interval_ms; /* 两次校时最小间隔（默认 60000）*/

    const iv_gps_timeops_t *timeops; /* NULL ⇒ 真实系统调用 */
} iv_gps_cfg_t;

/* 模块状态。**结构体公开**是为了让调用方自己持有内存（本模块零 malloc，
 * 与 `iv_reactor` 的"句柄语义"同一纪律）；下面的 `internal_` 字段是私有的，
 * 调用方只读 `fix` 与 `stats` 之外的任何字段都不算契约的一部分。*/
typedef struct {
    iv_gps_cfg_t cfg;

    /* -- internal_：行装配状态机 -- */
    char     internal_line[IV_GPS_LINE_MAX];
    size_t   internal_len;
    int      internal_in_sentence; /* 1 = 已经看到 '$' 且尚未行尾 */
    int      internal_overflow;    /* 1 = 本行已超长，丢弃到行尾 */

    /* -- internal_：日期与时刻分开到达，需合并 -- */
    int internal_have_date;
    int internal_year;
    int internal_month;
    int internal_day;
    int internal_have_utc;
    int internal_hour;
    int internal_minute;
    int internal_second;
    long internal_usec;

    uint64_t internal_last_sync_ms;
    int      internal_synced_once;

    /* -- internal_：上报判据用的上次快照 -- */
    iv_gps_fix_t internal_reported;
    uint64_t     internal_reported_ms;
    int          internal_have_reported;

    /* -- 对外可见 -- */
    iv_gps_fix_t    fix;   /* 最近一次合并出来的定位 */
    iv_gps_stats_t  stats;
} iv_gps_t;

/* 填默认配置：50 m / 60 s、允许校时、日期下限 2024、1 s 与 1 h 阈值、60 s 节流。
 * cfg 为 NULL 时直接返回（便于"可选参数"式调用）。*/
void iv_gps_cfg_default(iv_gps_cfg_t *cfg);

/* 初始化（可重复调用以复位）。`cfg` 为 NULL 用默认值。
 * 本函数**不做任何系统调用**、**不分配内存**，因此可以放在栈上随时重建。*/
void iv_gps_init(iv_gps_t *gps, const iv_gps_cfg_t *cfg);

/* 喂入一段字节流（来自串口/文件/pty）。
 *   buf/len : NULL 或 0 ⇒ IV_EINVAL
 * 返回本批字节中**成功解析的语句条数**（>= 0），或 IV_EINVAL。
 *
 * 内部是一个字符级状态机，不要求调用方按行切分（串口本来也不保证边界）：
 *   - 语句外的字节按噪声丢弃并计数；
 *   - `'$'` 开始收集，`'\n'` 结束（`'\r'` 忽略）；
 *   - 一行超过 `IV_GPS_LINE_MAX` 仍未见行尾 ⇒ **整行丢弃**（计数），
 *     并等下一个 `'$'` 重新开始 —— 绝不能"截断后继续解析"，那会把半条语句
 *     当成完整语句，产出的坐标是"看起来合理但错误"的。*/
int iv_gps_feed(iv_gps_t *gps, const char *buf, size_t len);

/* 取最近一次的定位快照（拷贝）。没有数据时返回**全 NA 且 `valid = 0`** 的快照。*/
int iv_gps_fix(const iv_gps_t *gps, iv_gps_fix_t *out);

/* 该不该上报？依据三条（任一满足即 1）：
 *   ① 从未上报过且有有效定位（首次立即上报）；
 *   ② 定位有效性发生变化（有效↔无效，`report_on_fix_change` 打开时）；
 *   ③ 位移 > `report_distance_m` 或距上次上报 > `report_interval_ms`。
 * `now_ms` 由调用方给（用 `iv_clock` 的单调毫秒），本模块不读时钟。*/
int iv_gps_should_report(const iv_gps_t *gps, uint64_t now_ms);

/* 标记"本次已上报"，把当前快照记为基线。上报动作完成后必须调用。*/
void iv_gps_mark_reported(iv_gps_t *gps, uint64_t now_ms);

/* 按策略校时（策略见文件头）。`now_ms` 为单调毫秒（节流用）。
 *   IV_OK      已执行调整（adjtime 或 settimeofday）
 *   IV_EAGAIN  距上次校时太近 / 没有新的时间信息
 *   IV_ESTATE  没有可用日期或时刻
 *   IV_ERANGE  偏差超过 `big_adj_ms` ⇒ **只记审计，未调整**
 *   IV_EINVAL  参数非法
 *   IV_EIO     底层系统调用失败（errno 保留）
 * 注：`sync_system_time = 0` 时不做任何调整，直接返回 IV_EAGAIN。*/
int iv_gps_sync_time(iv_gps_t *gps, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_GPS_H */
