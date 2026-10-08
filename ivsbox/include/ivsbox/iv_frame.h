/*
 * 采集板 UART 帧成帧 / 编码（libivmodules，功能开发计划 M2-S2.2）
 *
 * ============================ 它是什么 ============================
 * 把串口上来的**采集板字节流**切成完整帧（同步、半包、粘包、噪声重同步、
 * 长度上限、CRC 校验、帧尾校验、帧超时复位），并把要下发的 cmd+data 编成
 * 完整帧字节。**它不理解 data 的内容**：JSON 应答/事件载荷对它是透明字节，
 * 解析归上层（S2.3/S2.6）；这样本模块的规格只依赖架构 §18.2 的帧结构，
 * 不依赖尚未给出的 JSON schema。
 *
 * **它不负责读串口**：字节从哪来由调用方决定（`iv_serial_open()` 的 fd 加进
 * Reactor 后喂进来），单测用构造字节流驱动，不需要任何硬件。
 *
 * ============================ 帧规格（架构 §18.2，2026-10-08 冻结） ============================
 * 来源：用户提供《单片机通信 20260427.xlsx》。两方向同构，仅帧头不同：
 *
 *   | head(2B) | cmd(1B) | len(2B) | data(len B) | crc8(1B) | end(2B) |
 *
 *   - head：下行（主板→单片机）`0xF0F0`；上行（单片机→主板）`0x0F0F`（04-27 版
 *     把旧版上报类误写的 `0xF0F0` 改为 `0x0F0F`，上行帧头就此统一）。
 *   - crc8：CRC-8/SMBUS（poly 0x07 / init 0x00 / 不反射 / xorout 0x00），覆盖
 *     **cmd+len+data**，不含帧头帧尾 —— 与 iv_crc 的现网口径一致（S3）。
 *   - end：`0xFFFF`。
 *   - len ＝ **data 字节数**，大端（架构 §18.2"推断项"①②：文档未明说，按两处
 *     交叉证据落定，**首个真实联调轮须核验**；帧头/帧尾两字节相同，天然不受
 *     字节序影响）。
 *
 * ============================ 设计口径 ============================
 *   - **零 malloc**：解析器结构由调用方持有（栈/静态均可），data 缓冲定长内嵌。
 *   - **不引 pthread、不引 Reactor**：纯字节进出，任何线程模型下可用；
 *     单写者约定由调用方保证（与 Reactor 单线程模型一致）。
 *   - 帧视图 `iv_frame_t.data` **指向解析器内部缓冲**，只在回调内有效；
 *     需要留存必须拷贝。这一条防住"回调外悬垂指针"整类 bug。
 *   - 超长帧（len > IV_FRAME_DATA_MAX）与坏帧的长度域都不可信 ⇒ 一律**回到
 *     同步字搜索**，把残骸当噪声丢到下一个帧头为止，不做"精确跳过"。
 *   - 帧超时：距最近一个字节超过 gap_ms 判断帧中断（`iv_framer_tick()`），
 *     复位状态机并计数 —— 没有它，一个半截帧会卡住状态机，把后面的正常帧
 *     全部吃成"data 中段"。gap_ms=0 关闭该机制。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   iv_frame_build：
 *     IV_OK(>0)  成功，返回写入 out 的总字节数
 *     IV_EINVAL  参数非法（NULL 指针）
 *     IV_ERANGE  len > IV_FRAME_DATA_MAX
 *     IV_ENOSPC  out_size 放不下整帧
 */
#ifndef IVSBOX_IV_FRAME_H
#define IVSBOX_IV_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 帧常量（架构 §18.2）
 * ------------------------------------------------------------------------- */

/* 帧头（线上顺序＝高字节在前；两字节各自相同，字节序免疫） */
#define IV_FRAME_HEAD_DOWN 0xF0F0u /* 下行：主板 → 单片机 */
#define IV_FRAME_HEAD_UP   0x0F0Fu /* 上行：单片机 → 主板 */
#define IV_FRAME_TAIL      0xFFFFu

/* 命令字 */
#define IV_FRAME_CMD_QUERY  0xE1u /* 查询单片机参数（请求 data=0x00, len=1） */
#define IV_FRAME_CMD_THRESH 0xF1u /* 配置阈值（请求 data=9 参数×2B=18B） */
#define IV_FRAME_CMD_POWER  0xD1u /* 控制电源接口（请求 data=接口序号+控制状态） */
#define IV_FRAME_CMD_REBOOT 0xD2u /* 设备重启（请求 data=0x00, len=1） */
#define IV_FRAME_CMD_DOOR   0xC1u /* 箱门开启上报（data=0x01） */
#define IV_FRAME_CMD_EVENT  0xC2u /* 事件上报（data=JSON） */

/* data 上限。len 域物理上限 65535，但现网帧是短命令 + JSON 应答，1024 已
 * 足够（本地通道单包 64 KiB 的 1/64）；超上限整帧丢弃并计数 —— 上限把
 * "长度域被噪声打坏"从"暗中吞内存"变成"一次可统计的丢弃"。 */
#define IV_FRAME_DATA_MAX 1024u

/* 整帧固定开销：head(2)+cmd(1)+len(2)+crc8(1)+end(2) */
#define IV_FRAME_OVERHEAD 8u
#define IV_FRAME_MIN      IV_FRAME_OVERHEAD
#define IV_FRAME_MAX      (IV_FRAME_OVERHEAD + IV_FRAME_DATA_MAX)

/* ---------------------------------------------------------------------------
 * 解析出的帧视图（回调内有效，勿保存）
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t         cmd;  /* 命令字 */
    uint16_t        len;  /* data 长度（== 帧内 len 域，已通过 CRC） */
    const uint8_t *data; /* 指向解析器内部缓冲，回调返回后失效 */
} iv_frame_t;

/* 完整帧回调：在 iv_framer_feed() 内同步调用（调用方线程），不得再喂同一解析器 */
typedef void (*iv_frame_cb)(const iv_frame_t *fr, void *arg);

/* ---------------------------------------------------------------------------
 * 解析器
 * ------------------------------------------------------------------------- */
typedef struct iv_framer iv_framer_t;

/*
 * 初始化解析器（零 malloc 版本：framer 结构由调用方持有）。
 *   sync_head：期待的上行帧头，生产传 IV_FRAME_HEAD_UP（0x0F0F）
 *   gap_ms    ：帧超时阈值，0 = 关闭；>0 时须周期调用 iv_framer_tick()
 * 返回 IV_OK / IV_EINVAL。
 */
int iv_framer_init(iv_framer_t *fr, uint16_t sync_head, uint32_t gap_ms);

/* 复位到初始状态（清状态机与统计，不清缓冲内容 —— 内容由下次喂入覆盖） */
void iv_framer_reset(iv_framer_t *fr);

/*
 * 喂入字节流（可任意切块：半包/粘包/逐字节均可）。
 * 完整帧在 cb 内同步交付；buf 为 NULL 且 len 为 0 时只刷新超时基准，安全。
 * now_ms：单调毫秒（调用方给，通常 iv_clock_monotonic_ms()；单测给假时钟）。
 */
void iv_framer_feed(iv_framer_t *fr, const uint8_t *buf, size_t len,
                    uint32_t now_ms, iv_frame_cb cb, void *arg);

/*
 * 帧超时检查（gap_ms>0 时由调用方周期调用，如 Reactor 每秒定时器）：
 * 距最近一次收到字节 >= gap_ms 且状态机不在同步字搜索态 ⇒ 丢弃半截帧、
 * 复位并计数 stats->resets。
 */
void iv_framer_tick(iv_framer_t *fr, uint32_t now_ms);

/* 统计（只读视图，指向 framer 内部，framer 存活期间有效） */
typedef struct {
    uint32_t frames;   /* 交付的完整帧（CRC+帧尾均过） */
    uint32_t crc_err;  /* CRC 不符整帧丢弃 */
    uint32_t tail_err; /* CRC 过但帧尾非 0xFFFF，整帧丢弃 */
    uint32_t too_long; /* len > IV_FRAME_DATA_MAX 整帧丢弃 */
    uint32_t noise;    /* 同步字搜索态丢弃的噪声字节 */
    uint32_t resets;   /* 帧超时复位次数 */
} iv_frame_stats_t;

const iv_frame_stats_t *iv_framer_stats(const iv_framer_t *fr);

/* ---------------------------------------------------------------------------
 * 编码（下行帧；单测构造上行帧时传 IV_FRAME_HEAD_UP）
 * ------------------------------------------------------------------------- */
/*
 * 组帧：[head_hi][head_lo][cmd][len_hi][len_lo][data...][crc8][FF][FF]
 * 返回写入总字节数（>0）；失败返回 iv_ret.h 负码。
 */
int iv_frame_build(uint16_t head, uint8_t cmd, const void *data, uint16_t len,
                   uint8_t *out, size_t out_size);

/* 结构体定义对内公开（调用方可静态分配/内嵌），布局是实现细节勿手工改 */
struct iv_framer {
    uint16_t sync_head;
    uint32_t gap_ms;
    uint32_t last_ms;  /* 最近一次收到字节的时刻 */
    uint8_t  state;    /* 内部状态机 */
    uint8_t  cmd;
    uint16_t need;     /* data 总长 */
    uint16_t got;      /* data 已收 */
    uint8_t  crc;      /* CRC-8 累计寄存器 */
    uint8_t  buf[IV_FRAME_DATA_MAX];
    iv_frame_stats_t stats;
};

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_FRAME_H */
