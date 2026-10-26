/*
 * IVSBox 4G 模组状态（libivmodules）
 *
 * ============================ 它是什么 ============================
 * Quectel EC200G 的**只读状态查询器**：把一串 AT 查询命令翻译成一份快照，
 * 供 ctl 层与 Web 页面读取。
 *
 * 三条边界，刻意划得很硬：
 *   1. **不做 IO**：不 open 串口、不注册 reactor、不 read/write fd。
 *      字节由 `iv_agent` 喂进来（`iv_cell_feed`），命令由 `iv_agent` 取走发出
 *      （`iv_cell_next_cmd`）。这与 `iv_gps`（`iv_gps_feed`）完全同构 ——
 *      解析器能在 Host 上纯字符串单测，不需要真串口。
 *   2. **不管拨号**：拨号是出厂镜像的 udev 脚本（`/usr/bin/usb-EC200G-inserted.sh`，
 *      `ACTION=="add"`）的职责。本模块**不产生任何拨号动作**。
 *   3. **只发查询命令**：`AT+CSQ` / `AT+CPIN?` 这类不会改变模块状态的命令。
 *      绝不发 `AT+CFUN` / `AT+QCFG` / `AT+QNETDEVCTL` —— 我们与拨号脚本**共用同一个
 *      AT 口**（`/dev/ttyUSB0`），任何"写"都可能把现场正在用的链路弄断。
 *      只读最坏的结果是"查不到"，不会把能用的链路弄坏。
 *
 * ============================ 为什么需要这个模块（2026-10-19 板端实证） ============================
 * 板端当天出现过这个组合，任何单一指标都解释不了：
 *   - `usb0` = `NO-CARRIER`，报文已经切到 `eth0`，平台连接照常（`ESTABLISHED`）；
 *   - AT 侧：`AT+CPIN?` → `+CME ERROR: 10`、`+QSIMSTAT: 0,0`、`+CGATT: 0`、
 *     `+COPS: 0`、`+CREG: 0,2`、`+QENG` → `LIMSRV`；
 *   - 可 `AT+QCSQ` 有信号（`RSRP −85`）、`AT+CFUN?` = 1（模块满功能）、`ATI` 正常。
 * 结论是"**模块好的、有信号，但读不到 SIM 卡**"。而当时界面上只显示"有线在用"，
 * 没有任何地方告诉人 4G 为什么不可用。
 *
 * 所以本模块的核心不是"多报几个字段"，而是把 4G 可用性**拆成四层**分别给：
 *   SIM（有没有卡/要不要 PIN）→ 注册（有没有驻留网络）→ 附着（PS 域挂上没有）
 *   → PDP（有没有拿到 IP）。
 * 哪一层断，一眼能看出来。`usable` 是这四层的与，不是"能不能 ping 通"。
 *
 * ============================ 状态机（谁在什么时候发命令） ============================
 * 本模块内部是一个**轮询状态机**，不持有线程、不持有定时器：
 *   - `iv_cell_next_cmd()`：调用方（agent）在串口空闲时来取"下一条该发的命令"；
 *     返回 0 表示本轮结束。
 *   - `iv_cell_feed()`：调用方把读到的字节喂进来；本模块按行解析，
 *     收到 `OK` / `ERROR` / `+CME ERROR:` 即判定当前命令结束并解析。
 *   - `iv_cell_tick()`：调用方按固定节拍调用，本模块据此判超时、起新一轮。
 * 这样"什么时候发"完全由调用方的事件循环决定，本模块只回答"下一句问什么"。
 *
 * **超时是必须的**：AT 口上可能同时有别的会话（拨号脚本），我们的命令可能拿不到
 * 响应。没有超时就会卡在一条命令上，后面所有字段永远不更新 —— 现象是"页面一直
 * 显示第一次的值"，比报错更难查。所以每条命令都有 `IV_CELL_CMD_TIMEOUT_MS`。
 */
#ifndef IVSBOX_IV_CELLULAR_H
#define IVSBOX_IV_CELLULAR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 文本字段容量。ICCID 20 位 ＋ NUL、IMSI 15 位 ＋ NUL。 */
#define IV_CELL_TXT       48
#define IV_CELL_ICCID_N   24
#define IV_CELL_IMSI_N    20
#define IV_CELL_IP_N      24

/* AT 口参数。**按项目纪律不进 JSON 配置**（硬件确定不变，配错的表现是"不通"
 * 而不是"报错"，属现场最难查的一类，见 iv_serial.h 的口径）。
 * 2026-10-19 板端实测：EC200G 的 AT 口是 `/dev/ttyUSB0`，115200 8N1、无流控。 */
#define IV_CELL_AT_BAUD      ((unsigned)115200)
#define IV_CELL_PATH_DEFAULT "/dev/ttyUSB0"

/* 默认轮询周期与单条命令超时。周期取 30 s：4G 状态不会秒级变化，
 * 而每次查询要在共用 AT 口上发 11 条命令，太频繁会与拨号脚本抢口。 */
#define IV_CELL_PERIOD_MS       ((uint32_t)30000u)
#define IV_CELL_CMD_TIMEOUT_MS  ((uint32_t)800u)

/* 内部缓冲区：单条命令的完整响应（含多行）上限。
 * `AT+QENG="servingcell"` 一行就有 80+ 字节，`AT+CGCONTRDP` 多行带 DNS，
 * 512 字节对 EC200G 的实测响应有 4 倍余量。溢出时**截断并标记**，
 * 不静默丢——静默丢会表现成"某个字段偶尔为空"。 */
#define IV_CELL_LINE_MAX 192
#define IV_CELL_RESP_MAX 512

/* 未知数值统一用这个哨兵，不用 0：`0 dBm` 是合法信号，`0` 当"未知"会骗人。 */
#define IV_CELL_UNKNOWN ((int)-32768)

/* 命令编号。顺序即发送顺序：先固定信息（型号/固件/SIM），再动态状态。 */
typedef enum {
    IV_CELL_CMD_IDLE = 0,
    IV_CELL_CMD_ATI,
    IV_CELL_CMD_CGMR,
    IV_CELL_CMD_CPIN,
    IV_CELL_CMD_QSIMSTAT,
    IV_CELL_CMD_QCCID,
    IV_CELL_CMD_CIMI,
    IV_CELL_CMD_CSQ,
    IV_CELL_CMD_COPS,
    IV_CELL_CMD_CEREG,
    IV_CELL_CMD_CGATT,
    IV_CELL_CMD_QENG,
    IV_CELL_CMD_CGCONTRDP,
    IV_CELL_CMD_LAST
} iv_cell_cmd_id_t;

/* 快照。**全部字段可以原样给前端**：数值未知一律 IV_CELL_UNKNOWN，
 * 文本未知一律空串，前端不必再猜语义。 */
typedef struct {
    /* ---- 采集元信息：用来判断"这份快照是不是新鲜的" ---- */
    int      valid;              /* 1 = 至少完成过一轮 */
    uint64_t last_ok_ms;         /* 最近一次整轮完成的单调时刻；0 = 从未 */
    uint64_t last_try_ms;        /* 最近一次起轮的时刻 */
    uint32_t ok_rounds;          /* 累计完成轮数 */
    uint32_t fail_rounds;        /* 累计失败轮数（整轮一条都没成） */
    int      at_err;             /* 最近一次 AT 层错误码（IV_* 负值）；0 = 无 */
    uint32_t cmd_timeouts;       /* 累计单条命令超时次数（判断是否有人在抢 AT 口） */

    /* ---- 模组 ---- */
    char     model[IV_CELL_TXT];      /* ATI 第二行，如 "EC200G" */
    char     firmware[IV_CELL_TXT];   /* CGMR，如 "EC200GCNLER01A13M04" */

    /* ---- SIM ---- */
    int      sim_present;             /* 1 = 读到了卡；0 = 明确没有；−1 = 未知 */
    char     sim_state[IV_CELL_TXT];  /* READY / SIM PIN / NOT INSERTED / … */
    char     iccid[IV_CELL_ICCID_N];  /* 20 位，读不到为空串 */
    char     imsi[IV_CELL_IMSI_N];    /* 15 位，读不到为空串 */

    /* ---- 信号 ---- */
    int      csq;                /* +CSQ 原始 rssi 0..31；99 = 未知（规范值） */
    int      dbm;                /* 由 csq 换算的 dBm；csq 未知时 IV_CELL_UNKNOWN */
    int      level;              /* 0..5 格（前端画信号格用，换算在后端做一次） */
    int      rsrp;               /* dBm，来自 QENG；IV_CELL_UNKNOWN = 未知 */
    int      rsrq;               /* dB，同上 */
    int      sinr;               /* 模组原始值（不换算、不猜单位） */

    /* ---- 注册 / 驻留 ---- */
    int      cereg_stat;         /* +CEREG 的 stat（1/5 = 已注册） */
    char     reg_text[IV_CELL_TXT];   /* stat 的文本解释，后端给 */
    char     operator_name[IV_CELL_TXT]; /* +COPS? 的运营商名；空 = 未注册 */
    char     act[8];             /* LTE / WCDMA / …（来自 QENG） */
    int      mcc;
    int      mnc;
    int      tac;                /* 跟踪区码（LTE 的 TAC，不是 LAC） */
    uint32_t cell_id;            /* 小区 ID（E-UTRAN Cell Identity） */
    int      pci;                /* 物理小区 ID */
    int      band;               /* 频段号 */
    int      earfcn;             /* 频点 */

    /* ---- 数据连接（PDP） ---- */
    int      attached;           /* +CGATT：1 附着 / 0 未附着 / −1 未知 */
    int      pdp_active;         /* 1 = 报出活动上下文 / 0 = 明确没有 / -1 = 没读到（未知） */
    char     pdp_ip[IV_CELL_IP_N];
    char     apn[IV_CELL_TXT];
    char     dns1[IV_CELL_IP_N];
    char     dns2[IV_CELL_IP_N];

    /* ---- 判定（后端算一次，前端不重算） ---- */
    int      usable;             /* 1 = SIM 就绪 && 已注册 && 已附着 && 有 IP */
    char     reason[64];         /* usable=0 时给出第一处断点的中文说明 */
} iv_cell_snap_t;

/* 状态机句柄。**要嵌进 iv_agent**，故完整定义放在头文件里（同 iv_gps 的做法）。 */
typedef struct {
    iv_cell_snap_t snap;
    uint32_t       period_ms;
    uint64_t       next_round_ms;   /* 下一轮可以开始的时刻 */
    uint64_t       deadline_ms;     /* 当前命令的截止时刻 */
    iv_cell_cmd_id_t cur;           /* 正在等哪条命令的响应 */
    int            running;         /* 1 = 一轮进行中 */
    int            in_cmd;          /* 1 = 命令已发出、等响应 */
    char           line[IV_CELL_LINE_MAX];
    size_t         line_len;
    char           resp[IV_CELL_RESP_MAX];
    size_t         resp_len;
    uint32_t       resp_trunc;      /* 响应被截断的次数（诊断用） */

    /* ---- 链路质量诊断（不进度快照，只给装配层判"要不要换口"） ----
     * `no_reply_rounds`：**整轮一条命令都没回**（12 条全超时）的连续轮数。
     *   为什么不能拿 "csq 无效" 代替：卡没插 / 没信号时 `+CSQ: 99,99` 也是
     *   "CSQ 无效"，但 AT 明明是通的。两者混在一起会把"串口接错"误诊成
     *   "SIM 卡问题"（2026-10-23 现场就是这个误诊）。
     * 装配层据此在连续若干轮无响应后**重新探口** —— 见 `iv_agent.c` 的
     * `cell_redial_tick()`。 */
    uint32_t       no_reply_rounds;
    uint32_t       timeouts0;       /* 本轮开始时的 cmd_timeouts（算本轮超时数用） */
    uint32_t       fail_streak;     /* 连续失败轮数（恢复时打一条日志并清零） */
} iv_cell_t;

/* 初始化 / 复位。
 * `iv_cell_init` 只清状态机与快照，**不动 period_ms**（那是配置）。
 * `iv_cell_reset` 在串口重开时调：丢掉半条命令与残留响应，但**保留上一次快照**
 * ——链路抖一下不该让页面上的信号值瞬间归零。 */
void iv_cell_init(iv_cell_t *c, uint32_t period_ms);
void iv_cell_reset(iv_cell_t *c);

/* 取"下一条要发的命令"。
 *   out/cap : 输出缓冲（含行尾 `\r`，调用方直接写串口即可）
 *   now_ms  : 单调时刻
 * 返回 >0 = 写出的字节数；0 = 本轮结束（调用方等下一拍再来）；
 * 负值 = 参数非法。
 * **只有在一轮已经开始、且当前没有在等响应时才会返回命令** ——
 * 调用方可以每拍无脑调用，不必自己判"现在该不该发"。 */
int iv_cell_next_cmd(iv_cell_t *c, char *out, size_t cap, uint64_t now_ms);

/* 喂入串口读到的字节。按行解析，收到终结行即完成当前命令。 */
int iv_cell_feed(iv_cell_t *c, const char *buf, size_t len, uint64_t now_ms);

/* 节拍：判超时、起新一轮、算 usable。调用方每 100~500 ms 调一次即可。 */
void iv_cell_tick(iv_cell_t *c, uint64_t now_ms);

/* 拷一份快照（结构体赋值，不加锁：单写单读，撕裂只会导致"这一帧某个数旧了"，
 * 下一个周期就自愈 —— 为此引入互斥不划算，而且与 reactor 单线程模型不符）。 */
void iv_cell_snapshot(const iv_cell_t *c, iv_cell_snap_t *out);

/* ---- 自动重拨判定 -------------------------------------------------------
 * 为什么判定放这里：它是**准纯函数**（快照 ＋ 时刻 ＋ 内部计时 → 该不该拨），
 * 不碰串口、不建进程，因此可以拿真机快照做单测。真正起进程的动作在 `iv_agent`。
 * 唯一的副作用是维护"数据面已断多久"（`down_since_ms`），见下。
 *
 * 触发条件分两层：
 *   ① 事实层：卡就绪、已注册、已附着 —— 少任何一条都不拨，卡没插时拨号脚本
 *      只会白跑，还会把共用 AT 口占掉几十秒。
 *   ② 数据面层：**内核侧无线 WAN 没有 IPv4 地址**，或模组**明确否认**有活动上下文
 *      （`pdp_active == 0`）。`pdp_active == -1`（没读到）**不算否认** ——
 *      这条是本板的血债：ECM（`cdc_ether`）模式下 `AT+CGCONTRDP` 不报上下文，
 *      早先把"没读到"当成"没有"，于是每小时恒定空拨 5 次（撞 `MAX_TRIES`），
 *      每次 `AT+CFUN=1,1` 让模组 USB 重枚举、usb0 消失、平台 TCP 断链重连；
 *      而同期 `netstat` 里 usb0 一直 ESTABLISHED —— 数据面本来是好的。
 *
 * 为什么还要"连续不可用 `DOWN_HOLD_MS` 才动手"：开机阶段 usb0 还没拿到 DHCP
 * 地址，立刻拨会把出厂脚本那一次正常拨号踩掉（`AT+CFUN=1,1` 会重置模组），
 * 于是自己把自己拖进"永远拨不完"的圈。宽限期就是给正常拨号留的路。
 *
 * 为什么管这件事：出厂 udev 脚本只在 USB `add` 时拨一次，掉线不重拨（遗留 ⑧）。
 * 4G 是首选出口，挂了没人管就等于整条链路只能等人工。
 */
#define IV_CELL_REDIAL_GAP_MS    ((uint64_t)120000u) /* 两次尝试最小间隔 */
#define IV_CELL_REDIAL_WINDOW_MS ((uint64_t)3600000u)/* 限速窗口 */
#define IV_CELL_REDIAL_MAX_TRIES 5u                  /* 窗口内最多尝试次数 */
#define IV_CELL_REDIAL_DOWN_HOLD_MS ((uint64_t)120000u) /* 数据面连续不可用多久才动手 */

typedef struct {
    uint64_t last_try_ms;     /* 上次尝试的单调时刻；0 = 从未 */
    uint64_t window_start_ms; /* 当前限速窗口起点；0 = 未开始 */
    uint64_t down_since_ms;   /* 数据面开始不可用的时刻；0 = 当前正常 */
    unsigned tries;           /* 窗口内已尝试次数 */
} iv_cell_redial_t;

void iv_cell_redial_init(iv_cell_redial_t *r);

/* 返回 1 = 现在该拨，0 = 不该。
 * `wan_has_ipv4`：无线 WAN 接口**此刻有没有 IPv4 地址**（装配层 getifaddrs 得到，
 * 已含载波判断）。为什么要这一维：只看 PDP 会漏掉最常见的一种掉线 —— 模组认为
 * 上下文还在，但 usb0 已经掉了载波、地址被内核摘掉（`NO-CARRIER`），报文一个都发不出去。
 *
 * `r` **不是 const**：本函数就地维护 `down_since_ms`（恢复即清零、开始断则记起点）。
 * 仍是可单测的确定性逻辑 —— 同一串调用得出同一串结论，没有隐藏时钟、没有 I/O。 */
int iv_cell_redial_due(const iv_cell_snap_t *s, iv_cell_redial_t *r,
                       int wan_has_ipv4, uint64_t now_ms);

/* 记一次尝试（由调用方在真正发起后调用，窗口内计数 ＋1）。 */
void iv_cell_redial_mark(iv_cell_redial_t *r, uint64_t now_ms);

/* 命令编号 → 命令行文本（给测试与诊断用）；IDLE/越界返回空串。 */
const char *iv_cell_cmd_text(iv_cell_cmd_id_t id);

/* 信号格数换算：csq 0..31 → 0..5 格；99 或越界 → -1（未知）。 */
int iv_cell_level_from_csq(int csq);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_CELLULAR_H */
