/*
 * 上报装配层（libivmodules，功能开发计划 M3-S3.6）
 *
 * ============================ 它是什么 ============================
 * 把 M2 的采集板链路与 M3 的队列/传输拼成**端到端可运行的一层**，补齐 S2.4
 * 交出的"上报/查询响应的业务数据生成（装配层职责）"：
 *
 *   采集板 ── ttySAC5 ──► iv_serial ──► iv_link ──► iv_status（状态镜像）
 *                                                     │
 *                        (周期/触发) ──► build_seg ──► iv_queue_push（落盘）
 *                                                     │
 *   平台 ◄── TCP ── iv_transport ◄── pump.pull/ack ───┘
 *        └─ on_rx ──► iv_proto_recv ──► route ──► 查询应答(## + JSON) ──► on_tx
 *        └─ 心跳/ACK ◄── iv_proto(ack/heartbeat) ──► on_tx ──► iv_transport_send
 *
 * ============================ 发送路由规则（重要） ============================
 *   - **周期上报 / 立即上报**（无平台请求）：组 `##` 上报帧 → **入 S3.4 持久队列**
 *     → 由传输层的取件泵按序号发出、整件写尽才 ack。断线期间自然积压、恢复后补传。
 *   - **查询应答 / ACK / 心跳**（平台在线才有意义）：经 `iv_proto` 的 `on_tx`
 *     **直发** `iv_transport_send()`（非 UP 时传输层返回 IV_ESTATE，不积压过期帧）。
 *   - `iv_queue` 里存的是**已组好的线路字节**（整帧），取件泵的 `pull` 纯透传；
 *     这样"入队什么、发出去就是什么"，不会在补传时因组包逻辑变化而被重新组帧。
 *
 * ============================ 数据段口径（架构 §18.1，2026-10-10 冻结） ============================
 * 依据《指令-通用版 20260623.xlsx》sheet「正常上报」的**字段表**（权威定义）
 * ＋ 参考实现 `General_Version/main/APP/User/src/com.c`：
 *
 *   数据段 = `QN=0;TID=<hex>;VER=11;DEVTYPE=0400;CP=&&<数据区>&&`
 *   数据区（**按指令表字段表顺序**；未取到值的字段不上传，表内注明"未用到的字段
 *   无需上传"）：
 *     DT(14 位 YYYYMMDDhhmmss) CNS(6 路，逗号分隔) MN(2 路，逗号分隔)
 *     V A H T DS P APOWER AKW RELAY(3) CHV(3) CHA(3) POWER(3) ELEC(3)
 *     LAT LNG CSQ [ERR]
 *
 *   ⚠ 三处与指令表**不一致**的处置（已在 §18.1 登记）：
 *     1) **`DS`**：字段表里**没有 DS 行**，只有示例串 `T=31.33;DS=1;P=0;` 有它
 *        （位置在 T 与 P 之间）。本层按示例串位置发出、**原值直传采集板的
 *        `DS`（1=关/2=开）**；平台侧的 DS 语义**指令表未定义**，故不做映射，
 *        待平台确认后再定（**不猜**）。
 *     2) **`DEVTYPE` vs `TYPE`**：字段表写 `DEVTYPE`，示例串却写 `TYPE=`。
 *        以字段表 + 参考实现（`sprintf(...,"DEVTYPE=%04x;",...)`）为准，用 `DEVTYPE`。
 *     3) 示例串本身不可信（`...DT=...;;` 双分号、缺 `DS`、把 `CSQ` 写成 `ur`、
 *        CRC 值 `e8` 用任何等价算法都复现不出）——**以字段表 + 参考实现为准**。
 *
 * ============================ 查询响应（## + JSON + ##） ============================
 * 与上报帧**不同壳**：`##` + JSON + `##`（**无 4 位长度、无尾 CRC**）。
 * JSON 内 `crc` 字段 = CRC8(ASCII of `ver+devtype+tid+cmd`)，其中 cmd 是**被应答
 * 的那条命令**。E3 应答即 `11`+`0400`+`101`+`E3` → 字符串 `110400101E3`
 * → 取其 ASCII 字节算 CRC8 = **0x06**（与参考实现
 * `sprintf(crc_buff,"%02x%04x%xE3",...)` 同构；`E1` 对应 `110400101E1`→0x08）。
 * 本步只落 **E3（查版本）** 与 **E2（立即上报）** 两条做通路径；其余查询命令
 * 由 `iv_proto` 的未命中默认分支回 ACK(0x01)——**各命令的完整 JSON schema 属 M4**。
 *
 * ============================ 并发与生命周期 ============================
 *   - 单写者：全部接口只在装配层（Reactor）单线程内调用，内部无锁；
 *   - 零 malloc：`iv_report_t` 由调用方持有（约 15 KB，含 iv_link/iv_queue/iv_transport）；
 *   - 时间一律由调用方以 `now_ms` 传入（单测给假时钟）；reactor 模式下内部用
 *     `iv_clock_monotonic_ms()` 取时（hal 层，modules 可依赖）；
 *   - reactor 可传 NULL：此时由调用方自行按节拍调用 `iv_report_step()` 与
 *     `iv_report_on_serial()`（单测/无事件循环场景）。
 *
 * ============================ 已知取舍（登记，不掩盖） ============================
 *   - 本层**只注册串口 fd 的 READ 事件 + 一个周期 tick 定时器**，**不注册传输层 fd**：
 *     传输层的建连/收发/取件全在 `iv_transport_step()` 内推进，tick 周期
 *     `IV_REPORT_TICK_MS`（200 ms）即收发节拍。代价是补传吞吐 ≈ 5 件/s；
 *     现场若嫌慢，后续把 `iv_transport_fd()` 注册进 Reactor 即可（登记为遗留）。
 *   - `CNS`（6 路摄像机网络状态）当前由调用方注入，本步**恒为 0**（摄像机探测属 M4）。
 */
#ifndef IVSBOX_IV_REPORT_H
#define IVSBOX_IV_REPORT_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_link.h"
#include "ivsbox/iv_proto.h"
#include "ivsbox/iv_queue.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_status.h"
#include "ivsbox/iv_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 默认参数
 * ------------------------------------------------------------------------- */

/* 采集板链路设备节点（架构 §18.2：板端实际是 /dev/ttySAC5 @115200 8N1） */
#define IV_REPORT_SERIAL_DEFAULT "/dev/ttySAC5"

/* 总线节拍：驱动 iv_link_tick / iv_transport_step / 各定时器计数 */
#define IV_REPORT_TICK_MS      200u
/* 平台心跳周期（指令表「更新和心跳包」：CMD=0xFF、内容=0x01） */
#define IV_REPORT_HEARTBEAT_MS (30u * 1000u)
/* 周期上报默认间隔（指令表「配置指令」0xF5 示例值 300 s） */
#define IV_REPORT_REPORT_MS    (300u * 1000u)
/* 采集板 0xE1 全量查询轮询周期（镜像刷新/自愈） */
#define IV_REPORT_POLL_MS      (30u * 1000u)

/* 上报数据段上限（= iv_proto 的 ## 帧数据段上限） */
#define IV_REPORT_SEG_MAX      IV_PROTO_TXT_DATA_MAX

/* 串口发送暂存（帧都很小；溢出即丢弃并计数——不覆盖未发完的字节） */
#define IV_REPORT_TX_MAX       512u

/* 摄像机路数（CNS）/ 主网路数（MN），固定宽度见 §18.1 */
#define IV_REPORT_CNS_N 6
#define IV_REPORT_MN_N  2

/* ---------------------------------------------------------------------------
 * 外部注入的"非采集板"事实（本层不自己探测，由装配层填）
 * ------------------------------------------------------------------------- */
typedef struct {
    int    cns[IV_REPORT_CNS_N]; /* 摄像机网络状态：0 不存在/1 正常/2 断开/4 延时 */
    int    mn[IV_REPORT_MN_N];   /* 主网状态：0 未指定IP/1 正常/2 断开/3 网线断开/4 延时 */
    int    csq;                  /* 4G 信号强度 0..31；**<0 = 未知 ⇒ 不上传该字段** */
    int    have_loc;             /* 1 = LAT/LNG 有效 */
    double lat;
    double lng;
    const char *err;             /* 故障码串（逗号分隔的 4 字节码）；NULL/"" ⇒ 不上传 */
} iv_report_env_t;

/* ---------------------------------------------------------------------------
 * 配置与句柄
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *serial_path;  /* NULL → IV_REPORT_SERIAL_DEFAULT */
    const char *server_host;  /* 平台地址；NULL 或空串 → **不启动平台链路** */
    uint16_t    server_port;
    const char *queue_dir;    /* NULL → IV_QUEUE_DIR_DEFAULT */
    uint32_t    devid;        /* 设备唯一标识（TID，以 %x 十六进制上报） */
    uint16_t    devtype;      /* 0 → IV_PROTO_DEVTYPE_DEFAULT(0x0400) */
    uint32_t    report_ms;    /* 0 → IV_REPORT_REPORT_MS */
    uint32_t    heartbeat_ms; /* 0 → IV_REPORT_HEARTBEAT_MS */
    uint32_t    poll_ms;      /* 0 → IV_REPORT_POLL_MS */
    const char *mod;          /* 硬件型号串（E3 应答 `mod`）；NULL → "" */
    const char *sv;           /* 软件版本串（E3 应答 `sv`）；NULL → iv_version_string() */
} iv_report_cfg_t;

typedef struct {
    iv_report_cfg_t cfg;

    iv_status_t    st;   /* 采集板状态镜像（S2.6） */
    iv_link_t      lk;   /* 采集板可靠层（S2.3） */
    iv_proto_t     pf;   /* 平台协议层（S2.4） */
    iv_queue_t     q;    /* 待发持久队列（S3.4） */
    iv_transport_t tr;   /* 平台 TCP 传输层（S3.5） */
    iv_transport_pump_t pump; /* 取件泵（传输层只存指针，故本体须在此持有） */

    iv_report_env_t env; /* 由装配层注入的非采集板事实 */

    iv_reactor_t *r;          /* NULL = 由调用方驱动 step() */
    int           serial_fd;  /* <0 = 未打开 */
    iv_event_t   *serial_ev;
    iv_timer_t   *tick_timer;
    uint64_t      now_ms;     /* 最近一次 step/on_serial 的时刻（回调内取时用） */

    uint8_t txbuf[IV_REPORT_TX_MAX]; /* 串口发送暂存（FIFO） */
    size_t  txlen;

    uint64_t next_tick;   /* 下一个总线节拍 */
    uint64_t next_hb;     /* 下一个心跳时刻 */
    uint64_t next_report; /* 下一次周期上报时刻 */
    uint64_t next_poll;   /* 下一次 0xE1 轮询时刻 */

    /* 统计 */
    uint64_t reports;      /* 累计入队的上报帧数 */
    uint64_t heartbeats;   /* 累计发出心跳数 */
    uint64_t query_resp;   /* 累计查询应答数 */
    uint64_t queue_full;   /* 入队被拒（IV_EFULL）次数 */
    uint64_t serial_rx;    /* 串口累计读入字节 */
    uint64_t serial_tx;    /* 串口累计写出字节 */
    uint64_t serial_err;   /* 串口读/写错误次数 */
    uint64_t tx_drop;      /* 串口发送暂存溢出丢弃次数 */
    uint64_t polls;        /* 累计 0xE1 轮询下发次数 */
    uint8_t  opened;
} iv_report_t;

/* 填默认配置（serial/queue 用默认路径、devtype/report/hb/poll 用默认值、devid=0） */
void iv_report_cfg_default(iv_report_cfg_t *cfg);

/* 清零句柄（不碰系统资源）。使用前必须先 init（或 `= {0}`）。 */
void iv_report_init(iv_report_t *rp);

/*
 * 启动：打开串口、开队列（恢复）、建协议层、连平台（若 host 非空）；r 非 NULL 时
 * 把串口 READ 事件与 tick 定时器注册进 Reactor。now_ms 为单调毫秒。
 *   IV_OK / IV_EINVAL（rp/cfg 为 NULL）/ IV_EIO（串口或队列不可用）
 * 串口打不开**不是致命错误**：记录后继续（平台链路仍可跑，镜像停在未刷新态）。
 */
int iv_report_open(iv_report_t *rp, const iv_report_cfg_t *cfg,
                   iv_reactor_t *r, uint64_t now_ms);

/*
 * 单步驱动（reactor 模式由 tick 定时器调用；无 reactor 时由调用方按节拍调用）：
 *   ① 抽干串口 → iv_link；② iv_link_tick；③ iv_transport_step；
 *   ④ 到期则：链路轮询(0xE1) / 心跳 / 周期上报。
 * 未 open 返回 IV_ESTATE；其余恒 IV_OK（内部错误只计数、不向调用方抛）。
 */
int iv_report_step(iv_report_t *rp, uint64_t now_ms);

/* 串口可读事件处理（把字节喂进 iv_link）。reactor 模式下由 READ 事件回调调用。 */
void iv_report_on_serial(iv_report_t *rp, uint64_t now_ms);

/* 立即触发一次上报入队（E2「立即上报设备状态」用；也供现场排障手动催报）。 */
int iv_report_trigger(iv_report_t *rp, uint64_t now_ms);

/* 关闭：从 Reactor 摘事件/定时器、关串口与传输、关队列。 */
void iv_report_close(iv_report_t *rp);

/* ---------------------------------------------------------------------------
 * 纯函数（零 I/O，可单测）
 * ------------------------------------------------------------------------- */

/*
 * 组装平台上报**数据段**（不含 ## 壳，不含结尾 '\0' —— 返回写入长度）。
 *   st   ：采集板镜像（NULL ⇒ 全部字段视为未刷新，只发 DT/CNS/MN）
 *   env  ：非采集板事实（NULL ⇒ CNS/MN 全 0、CSQ 未知、无定位、无故障码）
 *   dt14 ：14 位 `YYYYMMDDhhmmss`（必须非 NULL 且恰 14 字符）
 * 字段顺序与条件见文件头；缓冲不足返回 IV_ERANGE（不写半截）。
 */
int iv_report_build_seg(const iv_status_t *st, const iv_report_env_t *env,
                        uint32_t devid, const char *dt14,
                        char *out, size_t cap);

/* 查询响应 JSON 内的 `crc` 字段：CRC8(ASCII of "%02x%04x%x%02X"(ver,devtype,devid,cmd)) */
uint8_t iv_report_query_crc(uint16_t devtype, uint32_t devid, uint8_t cmd);

/*
 * 组装 E3（查设备软硬件版本）的 **JSON 响应体**（不含 ## 壳）：
 *   {"code":0,"qn":"<qn>","data":{"ver":"..","type":"..","tid":"..","cmd":"E3",
 *     "mod":"..","sv":"..","crc":".."}}
 * qn 为 NULL/"" 时写 "0"；mod/sv 为 NULL 时写 ""。返回写入长度或 IV_ERANGE。
 */
int iv_report_build_query_json(uint16_t devtype, uint32_t devid,
                               const char *qn, const char *mod, const char *sv,
                               char *out, size_t cap);

/* ---------------------------------------------------------------------------
 * 只读读数
 * ------------------------------------------------------------------------- */
int      iv_report_is_open(const iv_report_t *rp);
int      iv_report_serial_fd(const iv_report_t *rp);
uint64_t iv_report_reports(const iv_report_t *rp);
uint64_t iv_report_heartbeats(const iv_report_t *rp);
uint64_t iv_report_query_responses(const iv_report_t *rp);
size_t   iv_report_queue_count(const iv_report_t *rp);
uint64_t iv_report_queue_full(const iv_report_t *rp);

/* ---------------------------------------------------------------------------
 * 测试辅助（正常装配不使用）
 * ------------------------------------------------------------------------- */

/* 直接喂一帧平台下行字节（等价于传输层 on_rx 收到）。 */
void iv_report_feed_platform(iv_report_t *rp, const uint8_t *buf, size_t len);

/* 设置/更新注入的环境事实。 */
void iv_report_set_env(iv_report_t *rp, const iv_report_env_t *env);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_REPORT_H */
