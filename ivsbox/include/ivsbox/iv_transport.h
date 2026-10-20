/*
 * 平台通道 TCP 传输层（libivmodules，功能开发计划 M3-S3.5）
 *
 * ============================ 它是什么 ============================
 * 与平台上位机之间那条 **TCP 长连接**的传输层：负责"怎么发、怎么收、断了怎么办"，
 * **不管"发什么"** —— 业务组装归装配层（S3.6），协议编解码归 S2.4 `iv_proto`。
 * 本层是 S2.4 交出的 `on_tx` 回调与 `iv_proto_recv()` 的落点：
 *
 *   ── 发（两条路，共用一个线性暂存缓冲，先进先出）──
 *     1. **取件泵**：从待发持久队列（S3.4）按序号取最老一条 → 由装配层注入的
 *        `pull` 回调**组帧成线路字节** → 排入暂存 → 写 socket；**整件字节全部
 *        写出后**回调 `ack(cookie)`，装配层据此 `iv_queue_ack()`。这条路径负责
 *        "断线补传"，只在连接 UP 时推进（不 UP 不取件）。
 *     2. **即时发送**：`iv_transport_send()`（ACK / 心跳 / 查询响应等），直接排
 *        入暂存，随写机会发出；不 UP 时返回 `IV_ESTATE`（不积压过期帧）。
 *
 *   ── 收 ──
 *     连续读 socket，把字节**原样**交给 `on_rx`（装配层接 `iv_proto_recv()`），
 *     本层不解析内容、不假设帧边界 —— 拆帧/半包/粘包归 S2.4。跨 recv 边界的
 *     缓冲由上层负责，本层只保证"读到的字节按序、不丢、不重"。
 *
 *   ── 断线 ──
 *     建连失败 / 对端关闭（read 返回 0）/ 读写出错 ⇒ 判 DOWN，进入**退避重连**
 *     （backoff 起，每次失败翻倍、封顶 backoff_max），重连成功回调 on_state。
 *
 * ============================ 职责边界 ============================
 *   - **不做业务组装、不做组帧**：取件泵的组帧由装配层注入的 `pull` 完成
 *     （装配层用 `iv_proto_text_frame()` / `iv_proto_bin_build()` 造线路字节），
 *     本层只当字节搬运工。
 *   - **不选出口，只按调用方指定的网卡绑 socket**：双 WAN 切换（S3.3）由装配层在
 *     切换事务里调 `iv_transport_close()` 断旧连接，再用新出口重新 `open()`；
 *     出口由 `cfg.bind_ifname` 指定，本层**不选路由、不改系统默认路由** ——
 *     只用 `SO_BINDTODEVICE` 把这一条 socket 钉在指定网卡上（Web / SSH 不受影响）。
 *   - **不发 DNS/配置**：目标 host/port 由装配层从 `ivsbox.json` 读好后传入。
 *   - 零 malloc：`iv_transport_t` 由调用方持有，收发缓冲都在句柄内。
 *
 * ============================ 并发与生命周期 ============================
 *   - **单写者**：全部接口只在装配层（Reactor）单线程内调用，内部无锁；
 *   - **不引 pthread、不引 iv_reactor**：socket 全部 `O_NONBLOCK`，只把 fd 经
 *     `iv_transport_fd()` 交出；可读/可写/定时事件由调用方驱动
 *     `iv_transport_step()`（也支持调用方按固定节拍轮询 step，单测即如此）；
 *   - **时间由调用方传入**（`now_ms`），本层不自己取时（便于单测注入时间轴）；
 *   - `on_rx` 收到的是**内部读缓冲**，回调返回后失效，需留存须拷贝；
 *   - 调用时序：`iv_transport_init()` →（可多次）`open() → step()… → close()`；
 *     `open()` 后必须**周期性 step** 才有进展（建连、收发、重连都在 step 内）。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK      成功
 *   IV_EINVAL  参数非法（NULL / 未 init / host 为空）
 *   IV_ESTATE  状态不允许（重复 open / 未 open 就 step / 未 UP 就 send）
 *   IV_ERANGE  host 超长、或单件字节超过暂存上限（`IV_TRANSPORT_TX_MAX`）
 *   IV_EFULL   暂存已满（`iv_transport_send`）
 *   IV_EAGAIN  取件泵当前无可发（仅出现在 `pull` 回调的约定里）
 *   IV_EIO     系统调用失败（errno 尽量保留原值）
 *   IV_ENOTSUP 内置 IO 不支持该地址族（非 IPv4）
 */
#ifndef IVSBOX_IV_TRANSPORT_H
#define IVSBOX_IV_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量（编译期定死，风格同 iv_probe / iv_queue）
 * ------------------------------------------------------------------------- */

/* host 字符串上限（含结尾 NUL） */
#define IV_TRANSPORT_HOST_MAX 128u

/* 发送暂存上限。必须 ≥ 单件最大字节（队列单条 ≤ 2048，加 ## 帧头尾仍远小于此），
 * 否则取件泵的件会被拒。留足余量以容纳在途件之后的即时帧。 */
#define IV_TRANSPORT_TX_MAX 8192u

/* 单次 read 缓冲上限 */
#define IV_TRANSPORT_RX_MAX 2048u

/* 出口网卡名上限（含结尾 NUL）＝内核 IFNAMSIZ，`usb0` / `eth0` 远小于此 */
#define IV_TRANSPORT_IF_MAX 16u

/* 退避默认值（可被 cfg 覆盖）：首次 1 s，翻倍封顶 30 s */
#define IV_TRANSPORT_DEF_BACKOFF_MS     1000u
#define IV_TRANSPORT_DEF_BACKOFF_MAX_MS 30000u

/* 同一件最多连写几次才判链路死（对齐参考实现 `SEND_RETRY_MAX` = 3：失败重试 3 次，
 * 耗尽才复位 TCP／复位 4G 网络）。判死之后由持久队列在重连后整件重发。 */
#define IV_TRANSPORT_TX_TRIES 3u

/* ---------------------------------------------------------------------------
 * 状态
 * ------------------------------------------------------------------------- */
typedef enum {
    IV_TRANSPORT_CLOSED = 0, /* 未 open 或已 close */
    IV_TRANSPORT_CONNECTING, /* 已发起/正在建连（含失败后的下一次尝试前） */
    IV_TRANSPORT_UP,         /* 已连接，可收发 */
    IV_TRANSPORT_DOWN        /* 建连失败或断开，退避等待中 */
} iv_transport_state_t;

/* ---------------------------------------------------------------------------
 * 注入式 I/O（NULL = 用本文件内置的真实 socket 实现；单测注入假实现做确定性驱动）
 * ------------------------------------------------------------------------- */
typedef struct {
    /* 建 socket，返回 fd；负值 = 失败。 */
    int (*sock_open)(void *arg);
    /* 发起 connect：IV_OK=已连上；IV_EAGAIN=进行中；其它负=失败。 */
    int (*sock_connect)(void *arg, int fd, const char *host, uint16_t port);
    /* 写：>0=写出字节数；IV_EAGAIN=暂不可写；其它负=出错。 */
    int (*sock_write)(void *arg, int fd, const uint8_t *buf, size_t len);
    /* 读：>0=读到字节数；0=对端关闭(EOF)；IV_EAGAIN=暂无数据；其它负=出错。 */
    int (*sock_read)(void *arg, int fd, uint8_t *buf, size_t cap);
    /* 查询异步 connect 结果：IV_OK=已连上；IV_EAGAIN=仍在进行；其它负=失败。 */
    int (*sock_error)(void *arg, int fd);
    /* 关闭 fd。 */
    void (*sock_close)(void *arg, int fd);
    /*
     * 绑定出口网卡（可选；NULL = 本层不绑，走内核默认路由）。
     * 在 open 出 fd 之后、connect 之前调用一次。返回 IV_OK 才算绑上；
     * 失败时本层只计数（`iv_transport_bind_fail`）**并继续建连** ——
     * 绑不上就退化成原来的行为，不能因为网卡名写错就整条通道不可用。
     */
    int (*sock_bind)(void *arg, int fd, const char *ifname);
    void *arg;
} iv_transport_io_t;

/* ---------------------------------------------------------------------------
 * 取件泵（可选）：从待发持久队列取一条并组帧
 * ------------------------------------------------------------------------- */
typedef struct {
    /*
     * 取下一件：IV_OK 时回填 bytes、len、cookie 三个出参；IV_EAGAIN = 暂无可发；
     * 其它负 = 本条无法处理（本层跳过、不 ack，等下一条）。
     * 约定：*len 必须 ≤ IV_TRANSPORT_TX_MAX，否则本层 ack(cookie) 丢弃并计数
     * （pump_oversize），以免泵被一条畸形件永久堵死。
     */
    int (*pull)(void *arg, const uint8_t **bytes, size_t *len, uint64_t *cookie);
    /* 该件字节**全部写出后**回调一次（cookie 为 pull 给的标识，通常＝队列序号）。 */
    void (*ack)(void *arg, uint64_t cookie);
    void *arg;
} iv_transport_pump_t;

/* ---------------------------------------------------------------------------
 * 配置与句柄
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *host;                 /* 目标主机（IPv4 点分十进制，或主机名） */
    uint16_t    port;                 /* 目标端口（主机序） */
    const iv_transport_io_t   *io;    /* NULL = 内置真实 socket */
    const iv_transport_pump_t *pump;  /* NULL = 无取件泵（纯字节流） */
    void (*on_rx)(void *arg, const uint8_t *bytes, size_t len); /* 收到线路字节 */
    void *rx_arg;
    void (*on_state)(void *arg, iv_transport_state_t st);       /* 状态变化回调 */
    void *state_arg;
    uint32_t backoff_ms;              /* 0 = IV_TRANSPORT_DEF_BACKOFF_MS */
    uint32_t backoff_max_ms;          /* 0 = IV_TRANSPORT_DEF_BACKOFF_MAX_MS */
    /*
     * 出口网卡名（如 `usb0` / `eth0`；NULL 或空串 = 不绑，走内核默认路由）。
     * 只影响**本连接**：不碰系统默认路由，Web / SSH 等其它流量完全不受影响。
     * 这是"自动切换优先 4G"能真正落地的那一半 —— 选服务器的地址只决定连哪儿，
     * 从哪儿出要靠它（否则双 WAN 时内核按 metric 挑，报文会从 eth0 走）。
     */
    const char *bind_ifname;
} iv_transport_cfg_t;

typedef struct {
    /* 配置快照 */
    char     host[IV_TRANSPORT_HOST_MAX];
    uint16_t port;
    const iv_transport_io_t   *io;
    const iv_transport_pump_t *pump;
    void (*on_rx)(void *arg, const uint8_t *bytes, size_t len);
    void *rx_arg;
    void (*on_state)(void *arg, iv_transport_state_t st);
    void *state_arg;
    uint32_t backoff_ms;
    uint32_t backoff_max_ms;
    char     bind_ifname[IV_TRANSPORT_IF_MAX]; /* 空 = 不绑出口 */

    /* 运行态 */
    uint8_t  opened;
    uint8_t  state;         /* iv_transport_state_t */
    uint8_t  pump_active;   /* 有在途取件件（暂存头部 pump_len 字节属于它） */
    uint8_t  reserve;
    int      fd;
    uint64_t retry_at_ms;   /* 下一次尝试连接的时刻 */
    uint32_t backoff;       /* 当前退避间隔 */

    /* 发送暂存（线性 FIFO，有效字节在 tbuf[0..tx_len)） */
    uint16_t tx_len;
    uint16_t pump_len;      /* 暂存头部属于在途取件件的字节数（0 = 无） */
    uint64_t pump_cookie;
    /* 同一条消息"连发几次才判链路死"的计数（对齐参考实现 send.c:send_handle_fail：
     * 失败重试 `SEND_RETRY_MAX`=3 次、耗尽才 `set_tcp_connect_reset`／`network_reset`）。
     * 暂存不清，下个节拍原样重写；3 次都写不出去才 `link_down()`。 */
    uint8_t  tx_tries;

    /* 统计 */
    uint64_t bytes_tx;      /* 累计写出线路字节 */
    uint64_t bytes_rx;      /* 累计读入线路字节 */
    uint64_t connects;      /* 累计成功建连次数 */
    uint64_t drops;         /* 累计连接丢失/失败次数 */
    uint64_t pump_oversize; /* pull 返回超限被丢弃的件数 */
    uint64_t bind_fail;     /* 出口绑定失败次数（失败仍建连，走默认路由） */
    int      bind_errno;    /* 最近一次绑定失败的 errno（0 = 从未失败） */

    uint8_t  rbuf[IV_TRANSPORT_RX_MAX]; /* 单次 read 缓冲 */
    uint8_t  tbuf[IV_TRANSPORT_TX_MAX]; /* 发送暂存 */
} iv_transport_t;

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

/* 清零句柄（不碰系统资源）。使用前必须先 init（或 `= {0}`）。 */
void iv_transport_init(iv_transport_t *t);

/*
 * 启动：快照配置、进入 CONNECTING（首次 step 即发起连接）。
 * 未 close 再次 open 返回 IV_ESTATE；host 为空返回 IV_EINVAL；
 * host 超长返回 IV_ERANGE。open 不立即连网，进展全在 step() 内。
 */
int iv_transport_open(iv_transport_t *t, const iv_transport_cfg_t *c);

/*
 * 关闭：关掉活动 fd、回到 CLOSED（后续可再次 open）。未 open 返回 IV_ESTATE。
 * S3.3 出口切换时装配层主动调用它来断旧连接。
 */
int iv_transport_close(iv_transport_t *t);

/* ---------------------------------------------------------------------------
 * 驱动与收发
 * ------------------------------------------------------------------------- */

/*
 * 单步驱动：推进建连/退避重连 → 取件泵 → 写 → 读。
 * 未 open 返回 IV_ESTATE；其余情况返回 IV_OK（内部错误只改状态、回调上报，
 * 不向调用方抛错 —— 传输层的"错"就是"断了"，由 on_state 告知装配层）。
 * 建议被周期性调用（可读/可写事件时，或固定节拍如 100 ms）。
 */
int iv_transport_step(iv_transport_t *t, uint64_t now_ms);

/*
 * 即时发送一段线路字节（排入暂存，随写机会发出）。仅 UP 时接受：
 * 未 open → IV_EINVAL；未 UP → IV_ESTATE；暂存放不下 → IV_EFULL；
 * 单段 > IV_TRANSPORT_TX_MAX → IV_ERANGE。
 */
int iv_transport_send(iv_transport_t *t, const uint8_t *bytes, size_t len);

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */

iv_transport_state_t iv_transport_state(const iv_transport_t *t);
size_t   iv_transport_tx_pending(const iv_transport_t *t); /* 暂存中未写出字节数 */
int      iv_transport_fd(const iv_transport_t *t);         /* 供 Reactor 注册；-1=无 */
uint64_t iv_transport_bytes_tx(const iv_transport_t *t);
uint64_t iv_transport_bytes_rx(const iv_transport_t *t);
uint64_t iv_transport_connects(const iv_transport_t *t);   /* 成功建连次数 */
uint64_t iv_transport_drops(const iv_transport_t *t);      /* 连接丢失/失败次数 */
uint64_t iv_transport_pump_oversize(const iv_transport_t *t);
/* 出口绑定失败次数。持续增长说明网卡名配错或进程没有 CAP_NET_RAW ——
 * 此时连接仍会建立，只是走了内核默认路由，报文可能从另一条 WAN 出去。 */
uint64_t iv_transport_bind_fail(const iv_transport_t *t);
/* 本连接实际绑定的出口网卡名（空串 = 未绑定）。诊断用：日志里打这个值，
 * 才能分清"配置传了名字"与"传输层真的拿到并绑上了"。 */
const char *iv_transport_bind_if(const iv_transport_t *t);
/* 最近一次绑定失败的 errno（0 = 从未失败）。ENODEV＝网卡名不存在，
 * EPERM＝进程缺 CAP_NET_RAW，ENOTSUP＝注入的 io 没提供 sock_bind。 */
int iv_transport_bind_errno(const iv_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_TRANSPORT_H */
