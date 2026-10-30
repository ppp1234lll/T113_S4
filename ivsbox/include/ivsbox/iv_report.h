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
 * ============================ 数据段口径（架构 §18.1，2026-10-20 复核） ============================
 * 依据《指令-通用版 20260623.xlsx》sheet「正常上报」的**字段表**（权威定义）
 * ＋ 参考实现 `General_Version/main/APP/User/src/com.c` 的
 * `com_report_normally_function()`（现场已量产、平台侧一直正常的那份）：
 *
 *   数据段 = `QN=0;TID=<hex>;VER=11;DEVTYPE=0a00;CP=&&<数据区>&&`
 *   数据区（**逐字段对齐 com.c 的拼接顺序**；未取到值的字段不上传，表内注明
 *   "未用到的字段无需上传"）：
 *     DT(14 位 YYYYMMDDhhmmss) CNS(6 路) MN(2 路) V A APOWER AKW H T P DS
 *     RELAY(3) CHV(3) CHA(3) POWER(3) ELEC(3) CSQ [LAT LNG] [ERR]
 *
 *   ⚠ 与指令表 / 参考实现的差异（如实登记，不掩盖）：
 *     1) **字段顺序**：2026-10-20 按 com.c 重排。原先发的是
 *        `H;T;DS;P;APOWER;AKW`，参考实现是 `APOWER;AKW;H;T;P;DS` —— `DS` 与 `P`
 *        互换、`APOWER`/`AKW` 落到 `RELAY` 之前，平台若按位取字段会整体错位。
 *     2) **`DS`**：指令表字段表里**没有 DS 行**（示例串 `H=50.01;T=31.33;P=0;APOWER=0.00;`
 *        里也没有），但参考实现恒发 `DS=<箱门状态>` 且在 `P` **之后** ——
 *        以参考实现为准，**原值直传采集板的 `DS`**；平台侧 DS 语义指令表未定义，
 *        故不做映射（注意 `DS` 1=关/2=开 与 `RELAY` 1=开/2=关 相反）。
 *     3) **`CSQ` 与 `MIU`**：`CSQ` 参考实现恒发（取不到就发 0）。本层只在 4G
 *        模组取到 0..31 时上传 —— 取不到就不发，**不编 0 冒充"无信号"**
 *        （与"未知量给 null 不给 0"同一口径）。`MIU` **正常上报段里根本没有**
 *        （参考实现只在 0xE1 配置应答里给 `"miu"` 阈值键），本层不发；
 *        2026-10-30 复核后删除了曾经的占位 `MIU=;`。
 *     4) **`DEVTYPE` vs `TYPE`**：字段表写 `DEVTYPE`，示例串却写 `TYPE=`。
 *        以字段表 + 参考实现为准，用 `DEVTYPE`。
 *     5) 示例串本身不可信（`...DT=...;;` 双分号、缺 `DS`、把 `CSQ` 写成 `ur`、
 *        CRC 值 `e8` 用任何等价算法都复现不出）——以字段表 + 参考实现为准。
 *
 * ============================ 定时节拍（2026-10-20 对齐参考实现） ============================
 * 参考实现的节拍是 **1 ms 硬件计数器累加到设定值就发**（`app_com_time_function()`
 * 里 `report_time++` 到点清零），唤醒延迟不进入下一拍。本层照此把三个定时器
 * 全部改成 `next += period` 的自由运行整倍数节拍；原先的 `next = now + period`
 * 会把每次唤醒延迟累加进去（300 s 上报 40 min 慢 1 s、一天 30 s 以上，平台上
 * 表现为"时间不固定"）。
 * 另：`sys.report.interval_s` 现在**改完立即生效**（`iv_report_set_report_period()`），
 * 不必重启 —— 在这之前它只在 `iv_report_open()` 读一次，改了等于没改。
 *
 * ============================ 查询响应（## + JSON + ##） ============================
 * 与上报帧**不同壳**：`##` + JSON + `##`（**无 4 位长度、无尾 CRC**）。
 * JSON 内 `crc` 字段 = CRC8(ASCII of `ver+devtype+tid+cmd`)，其中 cmd 是**被应答
 * 的那条命令**。E3 应答即 `11`+`0a00`+`101`+`E3` → 字符串 `110a00101E3`
 * → 取其 ASCII 字节算 CRC8 = **0xb2**（与参考实现
 * `sprintf(crc_buff,"%02x%04x%xE3",...)` 同构；`E1` 对应 `110a00101E1`→0xbc）。
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
 *   - **箱门事件钩子（S4.5）**：`iv_report_cfg_t::on_door` 由装配层挂；本层在
 *     `iv_status` 把 `0xC1` 解析成门开之后同步触发。⚠ `iv_status_init()` 会清零钩子，
 *     故本层在**每一处** `iv_status_init(&rp->st)` 之后都用本层自持的 cfg 副本
 *     重新挂（含 `serial_drop()` 的串口重连路径）；配置里没挂则保持 NULL。
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
#define IV_REPORT_SERIAL_RETRY_MS 1000u
/* "催报"专用的 0xE1 重试间隔：`iv_report_request_poll()` 拨出来的那一拍若撞上
 * `IV_EBUSY`（同 cmd 在飞）且有一帧补报在等镜像，就按这个间隔重试，
 * 而不是退回 `poll_ms`（30 s）——否则事件上报会"有时立刻、有时半分钟"。 */
#define IV_REPORT_POLL_RETRY_MS   200u

/* 上报数据段上限（= iv_proto 的 ## 帧数据段上限） */
#define IV_REPORT_SEG_MAX      IV_PROTO_TXT_DATA_MAX

/* 串口发送暂存（帧都很小；溢出即丢弃并计数——不覆盖未发完的字节） */
#define IV_REPORT_TX_MAX       512u

/* 摄像机路数（CNS）/ 主网路数（MN），固定宽度见 §18.1 */
#define IV_REPORT_CNS_N 6
#define IV_REPORT_MN_N  2

/* E3 应答里 `mod`（硬件型号）的截断长度：型号串来自配置键 `sys.device.model`
 * （`IV_CFG_STR_MAX` 允许 159 字符），而整个 JSON 要能装进一个 `##` 帧且留余量。
 * 参考实现的型号串是编译期常量（"FN-YJTY-PY-8"，12 字符），31 已经够宽。 */
#define IV_REPORT_MOD_MAX 31

/* 平台 0xE1（查询设备当前参数设置）`data` 段的上限。
 * 参考实现的 E1 应答有 30 余个键、约 420 B；整个 JSON 要装进一个 `##` 帧
 * （数据段上限 1024）⇒ 外壳约 150 B ＋ 本段，768 留足余量。 */
#define IV_REPORT_CFG_PAIRS_MAX 768u

/* ---------------------------------------------------------------------------
 * 外部注入的"非采集板"事实（本层不自己探测，由装配层填）
 * ------------------------------------------------------------------------- */
typedef struct {
    int    cns[IV_REPORT_CNS_N]; /* 摄像机网络状态：0 不存在无IP/1 正常/2 故障/4 延时/5 丢包 */
    /* 主网状态 —— **主网1 / 主网2 = 两个「主网检测 IP」（`probe.target1/2`）的结论**，
     * 编码与参考固件 `com.c:com_report_get_main_network_status()` 逐字一致：
     *   0 无IP（该项没配）   1 正常   2 故障（一轮 4 包全无响应）
     *   3 网线断开（**只有主网1**：运维网口载波 down ⇒ 直接 3，不看 ping 结论）
     *   4 延时（全响应但平均 RTT 超 `probe.timeout_ms`）  5 丢包（只回了一部分）
     * **不是** 4G / 有线两条上行链路 —— 那是本机的 `IV_WAN_*` 概念，只用于切链路、
     * 界面显示与自愈故障沿（见 `iv_agent.c:env_feed()`）。 */
    int    mn[IV_REPORT_MN_N];
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
    const char *server_host;  /* **首建**连接的平台地址；NULL 或空串 → 不启动平台链路 */
    uint16_t    server_port;  /* 运行期改址走 iv_report_set_server()（按活动出口选） */
    const char *bind_ifname;  /* **首建**绑定的出口网卡；NULL/空 → 走内核默认路由。
                               * 运行期改口走 iv_report_set_bind_if()（按活动出口选）。 */
    const char *queue_dir;    /* NULL → IV_QUEUE_DIR_DEFAULT */
    uint32_t    devid;        /* 设备唯一标识（TID，以 %x 十六进制上报） */
    uint16_t    devtype;      /* 0 → IV_PROTO_DEVTYPE_DEFAULT(0x0A00) */
    uint32_t    report_ms;    /* 0 → IV_REPORT_REPORT_MS */
    uint32_t    heartbeat_ms; /* 0 → IV_REPORT_HEARTBEAT_MS */
    uint32_t    poll_ms;      /* 0 → IV_REPORT_POLL_MS */
    const char *mod;          /* 硬件型号串（E3 应答 `mod`）；NULL → "" */
    const char *sv;           /* 软件版本串（E3 应答 `sysver`）；NULL → ""（本层不兜底，
                               * 由装配层填 iv_version_sysver()，见 iv_version.h 的分工） */
    const char *tsn;          /* 终端序列号（E3 应答 `tsn`，12 位 HEX）；NULL → "" */

    /* 平台 0xE1（查询设备当前参数设置）的 `data` 段：由**装配层**拼 ——
     * 配置键、4G 快照、eth0 实取地址都只在装配层手上，本层拿不到。
     * 契约：写出 `"k":"v","k2":"v2"`（**不带尾逗号**），返回写入字节数；<0 ⇒ E1 回 0x74。
     * NULL ⇒ 0xE1 一律回 0x74（宁缺勿编：宁可让平台看到"未实现"，
     * 也不回一份假配置让它以为查询成功了）。 */
    int (*on_query_cfg)(char *out, size_t cap, void *arg);
    void             *on_query_cfg_arg;

    /* 箱门开启事件（S4.5）：`0xC1` 到达时回调（M4 阶段装配层只记日志；
     * 抓图本体属 M6-S6.2）。NULL = 不通知。详见文件头"已知取舍"。 */
    iv_status_door_cb on_door;
    void             *on_door_arg;
} iv_report_cfg_t;

typedef struct {
    iv_report_cfg_t cfg;

    /* 平台目标地址（**本层自持副本**）：`iv_report_open()` 从 cfg 拷入，
     * 运行期由 `iv_report_set_server()` 改写（装配层按当前活动出口选地址），
     * `iv_report_reconnect()` 一律以本副本重连。
     * 与 `cfg.server_host` 的分工：那里是"配置注入值"（原样留存用于诊断），
     * 这里是"下一次建连实际使用的地址"。 */
    char           srv_host[IV_TRANSPORT_HOST_MAX];
    uint16_t       srv_port;
    /* 出口网卡（本层自持副本）：空 = 不绑、走内核默认路由。
     * 与 `srv_host` 配对使用 —— 地址决定"连哪儿"，这里决定"从哪儿出"。
     * 由 `iv_report_set_bind_if()` 改写，`open` / `reconnect` 时交给传输层。 */
    char           bind_if[IV_TRANSPORT_IF_MAX];

    iv_status_t    st;   /* 采集板状态镜像（S2.6） */
    iv_link_t      lk;   /* 采集板可靠层（S2.3） */
    iv_proto_t     pf;   /* 平台协议层（S2.4） */
    iv_queue_t     q;    /* 待发持久队列（S3.4） */
    iv_transport_t tr;   /* 平台 TCP 传输层（S3.5） */
    iv_transport_pump_t pump; /* 取件泵（传输层只存指针，故本体须在此持有） */

    iv_report_env_t env; /* 由装配层注入的非采集板事实 */

    /* 「组帧前按**当前**事实重算 env」的回调（装配层在 open 后注册，
     * 见 `iv_report_set_env_refresh()`）。为什么需要它 ——
     *
     * `env` 只由装配层定时注入（`env_feed()`，200 ms/1 s 各一处），而**事件触发的
     * 上报是同步发生的**：采集板 `0xC2` 一到，本层当场催 `0xE1` → 应答落地 →
     * 刷新镜像 → 立刻组帧。这一整串都在**同一次** `iv_report_step()` 里跑完，
     * 装配层的下一次 `env_feed()` 还没轮到 ⇒ 那一帧带的是**事件之前**的 `ERR`/`MN`/`CNS`。
     *
     * 后果（2026-10-30 现场实证）：触发漏水 → 采集板 `ERR` 里出现 `0x30600000`
     * → 立马上报的那一帧 `ERR=` 里**没有**它；它要等下一次上报才出现，而事件上报
     * 恰恰是"平台看告警的那一帧"。用户看到的就是"触发了漏水，设备没上报"。
     *
     * 有了这个回调，组帧一律以"**发送瞬间**的事实"为准，与谁先谁后无关。 */
    void (*env_refresh)(void *arg);
    void         *env_refresh_arg;
    uint8_t       env_refreshing; /* 防重入（回调内部若又触发上报，不再递归刷新） */

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
    uint64_t next_serial_retry; /* 串口不可用时下一次重连时刻 */

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
    /* 最近一次从串口**真的读到字节**的单调时刻（0 = 从未收到）。
     * Web 用它算"数据年龄"，用来区分"采集板没接/没上电"与"接了但一帧都不对"
     * —— 只看帧计数时这两种情况都是 0，现场没法判。 */
    uint64_t last_rx_ms;
    uint8_t  opened;
    /* "下一次收到采集板 0xE1 应答后立刻补一帧上报"标志。
     * 设它有两个来源，语义相同（都是"现在就要一帧带新状态的正常上报"）：
     *   ① **动作后**（2026-10-27 继电器实时上报；`iv_report_request_report_after_poll()`）：
     *      平台/网页点了继电器（`0xD1`）。为什么不在动作后立刻发：`0xD1` 的应答
     *      **不进**状态镜像，刚发完命令时镜像里还是旧继电器状态，这时候上报等于把
     *      旧状态又发一遍 —— 平台界面看着"点了没反应"。正解＝动作 → 催 0xE1 →
     *      收到新镜像 → 立刻上报。
     *   ② **采集板主动上报后**（2026-10-28；`0xC1` 箱门 / `0xC2` 事件，本层
     *      `on_link_upstream()` 里自动置位）：事件帧只带一个 TAG，是增量，
     *      必须先用 0xE1 取全量，再用全量镜像组一帧正常上报，平台才看得到
     *      "告警发生的**那一刻**设备是什么样"。
     * 注意它只是"保底"：真正的判据还有 `on_link_done()` 里的**继电器变化检测**
     * （`RELAY1..3` 与上一轮不同就报），因为状态也可能是采集板自己改的
     * （实测「通道重启」＝断电后采集板自己又上电）。两者同时成立只报一帧。 */
    uint8_t  want_report;
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

/* 打一个标记：**下一次**收到采集板 0xE1 应答（镜像刷新完）之后，立刻补一帧上报。
 *
 * 用途＝"状态一变要立刻告诉平台"，两个来源：
 *   - **控制动作后**（本函数由装配层调）：继电器在采集板侧，`0xD1` 的应答不进
 *     镜像（见 `iv_report.h` 头部口径），所以动作完成后必须催一次 0xE1 才能拿到
 *     新状态；
 *   - **采集板主动上报后**（本层**自动**置位，装配层无需调用）：`0xC1` 箱门开启 /
 *     `0xC2` 事件到达时，`on_link_upstream()` 会自动 `request_poll()` ＋ 打这个
 *     标记 —— 事件帧只有一个 TAG，必须补全量才算一份可用状态。
 * 拿到新状态后**自动**入一帧上报，平台不用等下一个周期（默认 300 s）。
 * 只能置位、不能叠加：连点几次也只补一帧（多补没有意义，上报里是全量状态）。
 * 采集板不回 0xE1 时标记保持置位，等下一次轮询成功再补报 —— 不会丢。
 * 专门催出来的那一拍若撞上 `IV_EBUSY`，本层按 `IV_REPORT_POLL_RETRY_MS` 快速
 * 重试，不会退回 30 s 的轮询周期。 */
void iv_report_request_report_after_poll(iv_report_t *rp);

/*
 * 重连平台传输（S3.3 双 WAN 切换事务里 close_old_tcp + reconnect 两步的合体）：
 * 关掉绑定旧出口的 TCP，再用**原 host/port** 重新发起连接。只动传输层，
 * **不碰串口 / 队列 / 协议层** —— 队列里未确认的数据全部保留，连接重建后
 * 由取件泵按序号补传（架构 §7.2「关闭绑定旧接口的 TCP，再用新出口重建」）。
 * 一律使用本层自持的**当前目标地址**（`srv_host`/`srv_port`）—— 装配层可在调用前
 * 先用 `iv_report_set_server()` 换成"当前活动出口"对应的那条地址（`net.server.
 * wired.*` / `.wireless.*`），即"**按活动出口选平台地址**"。
 *   IV_OK ；IV_EINVAL（rp 为 NULL）；IV_ESTATE（未 open 或目标地址未配置/已清空）
 *
 * ⚠ **目标地址为空时会把在用连接关掉**（2026-10-22 起，R-01）：语义是
 * "重连到一个空地址 ＝ 断开"。装配层清空 `net.server.*.host` 走的就是这条路径，
 * 若这里不断，旧 TCP 会一直 ESTAB、上报继续发往旧平台（板端实测过）。
 * 关的只是传输层，串口/队列/协议层与正常重连保持一致。
 */
int iv_report_reconnect(iv_report_t *rp, uint64_t now_ms);

/*
 * 下发任意采集板命令（下行帧）：`0xD1` 电源控制 / `0xD2` 重启等**控制类**
 * 命令走这里。本层自持 `iv_link`，装配层不得另行持有第二份 —— 否则同 cmd
 * 在飞唯一（`IV_EBUSY`）的约束会被绕过。
 *
 *   调用方**必须在慢任务池之外、且在调用后自行按需 resolve**：
 *   0xD1/0xD2 属控制命令（`iv_link` 只发一次、不重传），`iv_link_send` 返回
 *   `IV_OK` 只表示**已交发送队列入队**，不代表对端已执行（协议无事务号，见
 *   iv_link.h 与 §S2.3）。`on_done` 本层不登记（控制命令的应答语义归应用层）。
 *
 *   返回 `iv_link_send` 的返回码：
 *     IV_OK      已入队（含"同 cmd 上一笔还在飞"以外的正常情形）
 *     IV_EBUSY   **同 cmd 在飞**：调用方应保留待发项、稍后重试（S4.4 的待发
 *                FIFO 就是这么串行化整排 7 条 0xD1 的）
 *     IV_EINVAL  参数非法（rp/data 为 NULL 而 len 非 0）
 *     IV_ERANGE  len > IV_FRAME_DATA_MAX
 *     IV_EFULL   在飞表满
 *     IV_ESTATE  未 open（本层不检查，交给 iv_link 的入队路径）
 */
int iv_report_send_cmd(iv_report_t *rp, uint8_t cmd, const void *data,
                       uint16_t len, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 阈值下发（`0xF1`）：把 `elec.*` / `sensor.*` 九个配置键送到采集板
 * ---------------------------------------------------------------------------
 * ============================ 载荷是 JSON，不是 18 字节 ============================
 * Excel《配置、控制》把 `0xF1` 的请求写成"9 参数 × 2B = 18B"二进制，但现网
 * 单片机源码是**按 JSON 解析**的（键 `hv/lv/ov/ld/sl/tu/tl/hu/hl`）。用户
 * 2026-10-08 已裁定：**取 JSON，Excel 该行作废**（见 `docs/修改记录.md`
 * 该日条目的「协议冲突」段）。照二进制发出去，单片机收到的是 18 个不可解析
 * 的字节 —— 而且帧长/CRC 全都自洽，现场只会表现为"阈值怎么改都不生效"。
 *
 * ============================ 值 0 ＝ 该项不设置 ============================
 * 九个键的配置默认值都是 `0.0`，本模块**没有**"未设置"位可用。因此约定：
 * **值为 0 的项不下发该键**（JSON 里根本不出现），采集板保持自己的现值。
 * 反过来"把采集板某一项真的设成 0"就无法表达 —— 这是已知限制，登记在
 * `docs/代码说明.adoc`，不是遗漏。
 *
 * 为什么不"用镜像补齐九项再整体下发"：镜像里的阈值同样来自采集板，补齐是
 * 空操作；而一旦某项既没配置、镜像也没读到（首轮 0xE1 之前），补齐就得**猜**
 * 一个 0 下发 —— 采集板会把"任何电压都超限"，比不发更糟。
 * ------------------------------------------------------------------------- */

/* 阈值九项（**顺序不重要**：下发用 JSON 键名；顺序沿用 0xE1 镜像的字段序，
 * 只为读代码时两处能对上）。单位与量纲同 0xE1 应答：V/A/mA/℃/%/度。 */
typedef struct {
    int32_t hv; /* 高压        elec.volt_high   */
    int32_t lv; /* 低压        elec.volt_low    */
    int32_t ov; /* 过流        elec.over_current*/
    int32_t ld; /* 漏电流      elec.leak_current*/
    int32_t sl; /* 倾斜        sensor.tilt      */
    int32_t tu; /* 温度上限    sensor.temp_high */
    int32_t tl; /* 温度下限（可负） sensor.temp_low */
    int32_t hu; /* 湿度上限    sensor.humi_high */
    int32_t hl; /* 湿度下限    sensor.humi_low  */
} iv_thresh_t;

/*
 * 纯函数：把 `t` 里**非 0** 的项拼成 `0xF1` 的 JSON 载荷（便于单测，不碰链路）。
 * 键序固定为 `hv lv ov ld sl tu tl hu hl`（与单片机源码的解析顺序一致）。
 *   IV_OK      成功，`*out_len` 为长度（不含 NUL；缓冲里同时留了 NUL 便于排查）
 *   IV_EINVAL  `t`/`out`/`out_len` 为 NULL
 *   IV_ERANGE  `cap` 不够
 *   IV_ESTATE  **九项全为 0**（没有可下发的内容——"发了等于没发"，回错误让
 *              调用方别记一条"已下发"的日志）
 */
int iv_report_build_thresh(const iv_thresh_t *t, char *out, size_t cap, size_t *out_len);

/*
 * 把 `iv_report_build_thresh()` 拼好的载荷作为 `0xF1` 下发。
 * `0xF1` 是**幂等**的（同样的值写两遍结果一样），因此**允许 `iv_link` 重传**
 * —— 与 `0xD1`/`0xD2` 那种"只能发一次"的控制命令不同，不必进免重传名单。
 * 返回码同 `iv_report_send_cmd()`（`IV_EBUSY` 表示同 cmd 在飞，调用方稍后重试）。
 */
int iv_report_send_thresh(iv_report_t *rp, const char *json, size_t len, uint64_t now_ms);

/*
 * 请求**立刻**下发一次 `0xE1` 全量查询（不等下一个 `poll_ms` 周期）。
 *
 * 用途：控制类动作（`0xD1` 电源/继电器）的应答**不进状态镜像**（口径见
 * `iv_status.h`：0xF1/0xD1/0xD2 应答属应用层），所以发完动作后镜像仍是旧值，
 * 要等下一个轮询周期才能看到"到底生效没有"。人工点一下按钮却要等 30 s 才知道
 * 结果，在"这是个会断电的开关"的场合不能接受 ⇒ 动作成功后主动催一次查询。
 *
 * 实现上只把 `next_poll` 拨到当前时刻，**不直接发帧**：真正的下发仍由
 * `iv_report_step()` 走在既有节拍上（不新增 fd/定时器/线程，也不打断在飞事务）。
 * `rp == NULL` 或未 open 时**静默忽略**（这是"催一下"，不是业务动作）。
 */
void iv_report_request_poll(iv_report_t *rp);

/*
 * 设置**下一次建连**使用的平台地址（**不主动断连**：在用连接原样保留，
 * 由 `iv_report_reconnect()` 关闭并按新址重开）。`host` 为空 / `port` 为 0
 * ⇒ 清空目标地址（此后 `iv_report_reconnect()` 返回 IV_ESTATE **并关掉在用
 * 连接**，见该函数说明）。
 * 改写的是本层自持副本，**不动** `cfg.server_host`。
 *   IV_OK ；IV_EINVAL（rp 为 NULL 或未 open）；IV_ERANGE（host 超长）
 */
int iv_report_set_server(iv_report_t *rp, const char *host, uint16_t port);

/*
 * 设置**下一次建连**绑定的出口网卡（同样不主动断连，配 `iv_report_reconnect()`
 * 一起用）。`ifname` 为空串或 NULL ⇒ 解除绑定、走内核默认路由。
 * 这解决的是"地址选了 4G 的服务器、报文却从 eth0 出"这类问题：
 * 传输层用 `SO_BINDTODEVICE` 把这条 socket 钉死在指定网卡上，**不改系统路由**，
 * Web / SSH 等其它流量不受影响。
 * 绑定失败不阻断建连（传输层只计数），故本函数只校验长度。
 *   IV_OK；IV_EINVAL（rp 为 NULL）；IV_ERANGE（ifname 超长）
 */
int iv_report_set_bind_if(iv_report_t *rp, const char *ifname);

/*
 * 设置**周期上报间隔**（`sys.report.interval_s` 的落点），**改完立即生效**。
 *   - 只改周期：不动在用连接、不动队列、不重置任何统计；
 *   - 新周期更短时，当前这一拍已排好的截止时刻就地前移（不必等旧的那拍到点）；
 *   - `report_ms == 0` ⇒ 回落默认 `IV_REPORT_REPORT_MS`（300 s）；
 *   - 节拍是**自由运行的整倍数**（对齐参考实现的硬件计数器），唤醒延迟不累加。
 * 背景：本参数原先只在 `iv_report_open()` 从配置取一次，网页上改了等于没改，
 * 必须重启进程才生效 —— 现场"设了 5 分钟、平台还是十分钟一条"就是这么来的。
 *   IV_OK；IV_EINVAL（rp 为 NULL 或未 open）
 */
int iv_report_set_report_period(iv_report_t *rp, uint32_t report_ms);

/*
 * 设置**采集周期**（`sys.sample.period_ms` 的落点，＝向采集板发 0xE1 的间隔），
 * **改完立即生效**。
 *   - 只改节拍：不动在用连接、不动队列、不重置任何统计；
 *   - 新周期更短时，已排好的下一次轮询就地前移（不必等旧的那拍到点）；
 *   - `poll_ms == 0` ⇒ 回落默认 `IV_REPORT_POLL_MS`（30 s）；
 *   - 与上报节拍同口径：**自由运行的整倍数**，唤醒延迟不累加。
 * 背景：本参数在 2026-10-21 之前是**死键** —— 全仓没有消费方，节拍被编译期的
 * `IV_REPORT_POLL_MS` 写死（板端实测 30.9 s 一轮而配置写 1000 ms），网页上把它
 * 从 30 s 改成 1 s 一点效果都没有。现在配置即真值。
 *   IV_OK；IV_EINVAL（rp 为 NULL 或未 open）
 */
int iv_report_set_poll_period(iv_report_t *rp, uint32_t poll_ms);

/**
 * 解析平台下行 `0xF5`「配置定时上报间隔」的 DATA 段（纯函数，无副作用）。
 *
 * 口径逐条对齐参考实现 `com.c:com_set_next_report_time()`（2026-10-20 核对）：
 *   `DATA = sel(1 字节) + 秒数(大端 u16)`，共 **3 字节**
 *   - `秒 == 0`：参考实现**不采纳**，回 `0x74`（`IV_PROTO_ERR_EXEC`）；
 *   - `sel != 1`：参考实现 `app_set_next_report_time_other()` 只在 `sel == 1`
 *     时写入，其余选择子**照样回 `0x01`**（告诉平台"已处理"）；
 *   - 其余：写入并回 `0x01`，参考实现另外**落盘**。
 *
 * 本函数只做解析，采纳/落盘/回什么码由装配层决定（见 `src/app/main.c`）。
 *
 * @param data  DATA 段起址（可为 NULL，此时必返回 IV_EINVAL）
 * @param len   DATA 段长度；< 3 ⇒ IV_EINVAL
 * @param sel   [out] 选择子（不可为 NULL）
 * @param sec   [out] 秒数（不可为 NULL）
 * @return IV_OK / IV_EINVAL
 */
int iv_report_parse_cfg_intv(const uint8_t *data, uint16_t len,
                             uint8_t *sel, uint16_t *sec);

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
                        uint16_t devtype, uint32_t devid, const char *dt14,
                        char *out, size_t cap);

/* 查询响应 JSON 内的 `crc` 字段：CRC8(ASCII of "%02x%04x%x%02X"(ver,devtype,devid,cmd)) */
uint8_t iv_report_query_crc(uint16_t devtype, uint32_t devid, uint8_t cmd);

/*
 * 组装 E3（查设备软硬件版本）的 **JSON 响应体**（不含 ## 壳）：
 *   {"code":0,"qn":"<qn>","data":{"ver":"11","devtype":"0a00","type":"0a00",
 *     "tid":"b2cc1","cmd":"E3","mod":"FN_RZTY_PY","sysver":"1.0.0.20261020",
 *     "sv":"1.0.0.20261020","tsn":"934072007C00","crc":"16"}}
 *
 * 字段名对齐参考实现 `com.c:com_version_information()`：`devtype` / `sysver` / `tsn`。
 * 同时保留旧名 `type` / `sv` 作**别名**（值完全相同）——协议 Excel 的字段表写的是
 * `devtype`+`sysver`，同一张表的示例串写的却是 `type`+`sv`，自相矛盾；平台按哪份
 * 实现都读得到，多两个键对 JSON 消费方无害。
 *
 * `qn` 为 NULL/"" 时写 "0"；`mod`/`sv`/`tsn` 为 NULL 时写 ""；`mod` 超长按 31 字符
 * 截断（E3 应答缓冲只有几百字节，型号串是配置项、长度可控）。
 * 返回写入长度或 IV_ERANGE。
 */
int iv_report_build_query_json(uint16_t devtype, uint32_t devid,
                               const char *qn, const char *mod, const char *sv,
                               const char *tsn, char *out, size_t cap);

/* 平台 0xE1（查询设备当前参数设置）应答的 JSON 外壳。
 *
 * 形如（**照抄** `com.c:com_query_configuration_function()` 的层级）：
 *
 *   ##{"code":0,"data":{"ver":"11","devtype":"0a00","qn":"<qn>","tid":"b2cc1",
 *        "cmd":"E1",<pairs>,"crc":"xx"}}##
 *
 * ⚠ 两处与 E3 **刻意不同**，别"统一"：
 *   ① E1 的 `qn` 在 `data` **里面**，E3 的在顶层；
 *   ② E1 没有 `type`/`sv` 别名（参考实现 E1 只发主名）。
 *
 * `pairs` 由调用方提供（装配层手上的配置/4G/网卡事实），写 `"k":"v",...` 不带尾逗号；
 * `pairs == NULL` 或返回负值 ⇒ 返回 IV_EROFS 以外的负值，由 handler 回 0x74。
 * 返回写入长度或负的错误码。
 */
int iv_report_build_cfg_json(uint16_t devtype, uint32_t devid, const char *qn,
                             int (*pairs)(char *out, size_t cap, void *arg),
                             void *pairs_arg, char *out, size_t cap);

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

/*
 * 注册「组帧前重算 env」的回调（装配层给：内部就是再跑一遍 `env_feed()`）。
 *
 * **必须注册**，否则事件触发的上报会带上一拍的旧 `ERR`/`MN`/`CNS`：
 * 采集板 `0xC2` 事件的链路是"催 `0xE1` → 镜像落地 → **同一次 step 内**立刻组帧"，
 * 装配层的定时 `env_feed()` 永远晚一步。详见 `iv_report_t.env_refresh` 的注释与
 * 2026-10-30 的漏水现场（触发漏水，事件那一帧里没有 `0x30600000`）。
 *
 * 回调内只能做纯计算 + `iv_report_set_env()`，**不得**回调本层的
 * `iv_report_trigger()`（会重入组帧；本层有 `env_refreshing` 兜底，但别依赖它）。
 * fn 为 NULL = 摘掉回调（回到"只认定时注入"的老行为，仅测试用）。
 */
void iv_report_set_env_refresh(iv_report_t *rp, void (*fn)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_REPORT_H */
