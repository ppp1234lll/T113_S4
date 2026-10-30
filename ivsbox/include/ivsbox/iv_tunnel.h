/*
 * 受控远程访问通道（libivmodules，功能开发计划 M8-S8.1~S8.3）
 *
 * ============================ 它是什么 ============================
 * 架构 §9.4 的**设备侧**那一半：4G 私网 / 客户网段边界下，平台与运维人员无法主动
 * 拨进设备，只能由**设备主动外连**建立反向通道。
 *
 * **2026-10-30：整块按参考项目 BIRD 运维箱（bird_20260623）逐条对齐**，不再自创
 * "按服务名开多个代理"的模型 —— 现场要的是"GO 版能用，这套也要一样能用"。对齐后的
 * 事实（全部可在 GO 源码里逐条核对）：
 *
 *   | 项            | BIRD（GO）                                       | 本模块 |
 *   |---------------|--------------------------------------------------|--------|
 *   | 代理条数      | **1 条**（`factory_manager.go` writeFrpConfig）   | 同 |
 *   | 代理名        | `name = "<SN>"`                                   | 同 |
 *   | 转发目标      | `localIP = "127.0.0.1"` / **`localPort = 22`**    | 同 |
 *   | 代理类型      | `type = "stcp"`（端口不开在 frps 上）             | 同 |
 *   | 钥匙          | `GenerateSTCPKey(sn, token)`                      | 同 |
 *   | 配置文件      | `/etc/frp/frpc.toml`（0600）                      | 同 |
 *   | 开关标记      | `/tmp/frp.enabled`（`frp.sh start` 里 `touch`）    | 同 |
 *   | 进程守护      | `monitor.sh` 每 5 s：标记在、frpc 不在 ⇒ 拉起      | 同 |
 *   | 有效期        | **没有**（开机不自动开，靠 /tmp 易失）             | 默认 0＝不过期，平台可显式给 TTL（扩展，不影响 BIRD 语义） |
 *
 * **为什么只开 SSH（22）而不是每个服务一条代理**：BIRD 的访问端 `tools/visitor/
 * connect.bat` 是这么用的 ——
 *     1) 生成 visitor 配置（`serverName = <SN>`、`bindPort = 6000`、同一把钥匙）；
 *     2) `frpc -c temp_frpc.toml` 把设备 22 映射到本机 6000；
 *     3) `ssh -p 6000 -L 9090:<摄像机IP>:80 root@127.0.0.1 -N`，浏览器开
 *        `http://127.0.0.1:9090`。
 * 也就是说"看摄像机 / 取文件 / 传固件"**全部由 SSH 本地转发按需建立**，设备侧只
 * 需要一条到 22 的通道。这样做的好处是确定的：① 设备侧配置与设备上有什么服务无关，
 * 加一台摄像机不用改 frpc.toml、不用重开通道；② SSH 转发是裸 TCP，视频流不经过任何
 * HTTP 反代，不会碰到 504 / 缓冲截断 / 每条流一个槽位这些坑；③ 认证、加密、审计都
 * 复用 sshd 一套，frp 只管搬运。
 *
 * ============================ 本模块管什么、不管什么 ============================
 *   管：secretKey 派生（与 BIRD 同一个公式）；frpc.toml 渲染与原子落盘（0600，保护
 *       钥匙）；标记文件的置与清；**可选**有效期闸门；会话时长与拒绝计数。
 *   不管：frpc 进程启停（归 `scripts/frp.sh` 与 `scripts/monitor.sh`，与 BIRD 同构）、
 *       数据面与加密（归 frp）、按需端口转发（归访问端的 `ssh -L`）。
 *   **理由**：进程与连线属"运维体系"层，塞进模块层会让单测变成"起真进程"；这一层
 *   最该钉死的是"钥匙怎么派生、文件里到底写了什么、到期会不会自己关"。
 *
 * ============================ secretKey 派生（与 BIRD 字节级一致） ============================
 *   secretKey = 小写 hex( MD5( sn + token ) 的前 8 字节 )  → 16 个字符
 * 这就是 BIRD 的 `internal/utils/md5.go: GenerateSTCPKey()`：
 *
 *     hash := md5.Sum([]byte(sn + token))
 *     return hex.EncodeToString(hash[:8])
 *
 * 访问端 `connect.bat` 用 `certutil -hashfile <sn+token> MD5` 取前 16 位，同一个值。
 * **为什么用 MD5**：两侧必须算出同一把钥匙，公式由运维侧既有工具决定，换算法得同步
 * 改 BIRD 与运维手里的 bat。此处是**共享密钥派生**用途，不是完整性校验。
 *
 * ============================ 有效期闸门（本工程的扩展） ============================
 * BIRD **没有**有效期：`debug.mode` 一开就一直开着，只有重启（`/tmp` 是 tmpfs）或
 * 平台显式关才停。本模块默认与之一致（`ttl_s = 0` ⇒ 不过期、标记文件写成空文件，
 * 与 BIRD 的 `touch` 逐字等价）；平台可以显式给 `ttl`（≤ 86400），此时标记文件的
 * **内容就是到期时刻**（unix 秒，十进制一行），`scripts/frp.sh` / `scripts/monitor.sh`
 * 能**独立**判过期并收掉 frpc —— 两道闸门互为兜底，主控被 SIGKILL 也不会让通道
 * 永远开着。标记里没有数字（BIRD 手工 `touch` 的那种）按"不过期"处理。
 *
 * ============================ 内存 / 并发 / 依赖 ============================
 *   - **零 malloc**：句柄由调用方持有（约 1 KB）；
 *   - **单写者**：只在装配层（Reactor）单线程内调用，内部无锁；
 *   - **时间由调用方注入**（unix 秒），单测可钉时间轴；
 *   - 文件 I/O 用 libc（open/write/fsync/rename/chmod），路径由装配层注入（单测用临时目录）；
 *   - MD5 走 OpenSSL EVP（不自写），故落 `src/modules/tunnel/` 进 `libivmodules.a`。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK / IV_EINVAL / IV_ERANGE / IV_ESTATE / IV_ENOENT / IV_EPROTO / IV_EIO /
 *   IV_EFULL。细粒度原因看 `iv_tunnel_reason_t`。
 */
#ifndef IVSBOX_IV_TUNNEL_H
#define IVSBOX_IV_TUNNEL_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量
 * ------------------------------------------------------------------------- */
#define IV_TUNNEL_SN_MAX      32u   /* 设备序列号（含 NUL） */
#define IV_TUNNEL_TOKEN_MAX   64u   /* 平台下发的调试令牌（含 NUL），对齐 BIRD 的 64 字节 */
#define IV_TUNNEL_ADDR_MAX    128u  /* frps 地址（域名或 IPv4，含 NUL） */
#define IV_TUNNEL_SECRET_HEX  16u   /* secretKey：MD5 前 8 字节 → 16 位小写 hex */
#define IV_TUNNEL_SECRET_MAX  17u   /* 含 NUL */
#define IV_TUNNEL_NAME_MAX    48u   /* frp 代理项名 = `<sn>`（含 NUL） */
#define IV_TUNNEL_PATH_MAX    256u  /* 配置 / 标记文件路径（含 NUL） */
#define IV_TUNNEL_TXT_MAX     1024u /* 渲染出的 frpc.toml 上限（单条代理，远小于此） */

#define IV_TUNNEL_PORT_DEF 7000u    /* frps 默认端口（BIRD 同款） */
#define IV_TUNNEL_SSH_PORT 22u      /* 转发目标：本机 sshd（BIRD 硬编码 22） */
#define IV_TUNNEL_TTL_MAX  86400u   /* 显式有效期上限 24 小时；0＝不过期（BIRD 语义） */

/* 默认落点：与 BIRD **完全同名**（`frp.sh` / `monitor.sh` 里写死的两个路径）。
 * 装配层可用 iv_tunnel_set_paths() 覆盖（单测用临时目录）。 */
#define IV_TUNNEL_CONF_DEFAULT "/etc/frp/frpc.toml"
#define IV_TUNNEL_FLAG_DEFAULT "/tmp/frp.enabled"

/* ---------------------------------------------------------------------------
 * 拒绝原因（诊断 / 上报 / 单测断言用；数值稳定，可落库）
 * ------------------------------------------------------------------------- */
typedef enum {
    IV_TUNNEL_OK = 0,
    IV_TUNNEL_EBADARG = 1,  /* 参数非法：有效期越界 / 缓冲不足 / 参数为空 */
    IV_TUNNEL_ENOIDENT = 2, /* 缺身份：SN 或令牌为空 */
    IV_TUNNEL_ENOSERVER = 3,/* 缺 frps 地址或端口 */
    IV_TUNNEL_EUNMAPPED = 4,/* 预留（BIRD 模型下没有"服务名映射"这一步） */
    IV_TUNNEL_EHOST = 5,    /* 预留（同上：不再由本模块决定代理到哪里） */
    IV_TUNNEL_EIO = 6,      /* 配置文件 / 标记文件读写失败 */
    IV_TUNNEL_ESTATE = 7,   /* 状态不允许（重复开启 / 未开启就关闭） */
    IV_TUNNEL_REASON_N = 8
} iv_tunnel_reason_t;

const char *iv_tunnel_reason_name(iv_tunnel_reason_t r);

/* 停止原因名（会话留档 / 审计 / 单测断言用）。`r` 取 iv_tunnel_stop_reason_t。 */
const char *iv_tunnel_stop_reason_name(int r);

/* ---------------------------------------------------------------------------
 * 纯函数（无 I/O、无状态，单测直接钉字节）
 * ------------------------------------------------------------------------- */

/*
 * secretKey 派生：hex( MD5(sn + token)[0..8) )，写 16 个字符 + NUL。
 * 与 BIRD `utils.GenerateSTCPKey()` 同源，访问端 `connect.bat` 用 certutil 算同一个值。
 * 返回 IV_OK / IV_EINVAL（参数）/ IV_ERANGE（cap < IV_TUNNEL_SECRET_MAX）。
 */
int iv_tunnel_secret(const char *sn, const char *token, char *out, size_t cap);

/*
 * 渲染 frpc.toml 文本（**不落盘**，纯拼装，便于单测逐字钉内容）。
 * 产物与 BIRD `writeFrpConfig()` 的输出**逐字一致**（含空行位置）：
 *
 *     serverAddr = "<addr>"
 *     serverPort = <port>
 *     auth.token = "<token>"
 *
 *     [[proxies]]
 *     name = "<sn>"
 *     type = "stcp"
 *     secretKey = "<16 hex>"
 *     localIP = "127.0.0.1"
 *     localPort = 22
 *
 * `out` 缓冲不足返回 IV_ERANGE；缺身份 / 缺服务端 ⇒ 负码（原因见 `why`）。
 * 成功时 `*out_len` 为写入字节数（不含结尾 NUL）。
 */
int iv_tunnel_render(const char *sn, const char *token, const char *server_addr,
                     uint16_t server_port, char *out, size_t cap, size_t *out_len,
                     iv_tunnel_reason_t *why);

/* ---------------------------------------------------------------------------
 * 状态与留档
 * ------------------------------------------------------------------------- */
typedef enum {
    IV_TUNNEL_OFF = 0, /* 通道关（默认） */
    IV_TUNNEL_ON = 1   /* 通道开（配置已落盘 + 标记文件存在） */
} iv_tunnel_state_t;

typedef enum {
    IV_TUNNEL_STOP_NONE = 0,
    IV_TUNNEL_STOP_LOCAL = 1,   /* 本地 / 平台显式关闭 */
    IV_TUNNEL_STOP_EXPIRED = 2, /* 有效期到期自动关闭 */
    IV_TUNNEL_STOP_RECONF = 3   /* 身份或服务端变更，配置作废 */
} iv_tunnel_stop_reason_t;

/* 一次会话的留档（架构 §9.4：要"记录连接时长"）。 */
typedef struct {
    uint64_t open_s;
    uint64_t close_s;
    uint32_t ttl_s; /* 0 ＝ 不过期（BIRD 语义） */
    uint8_t  stop_reason;
} iv_tunnel_session_t;

typedef struct {
    uint64_t opens;    /* 成功开启次数 */
    uint64_t denies;   /* 被拒次数 */
    uint64_t expires;  /* 到期自动关闭次数 */
    uint64_t uptime_s; /* 累计开启时长（秒） */
    uint64_t deny_by[IV_TUNNEL_REASON_N];
} iv_tunnel_stat_t;

/* ---------------------------------------------------------------------------
 * 句柄（内部字段直接暴露，符合本工程零 malloc 风格；调用方只读）
 * ------------------------------------------------------------------------- */
typedef struct {
    /* 身份与服务端（secretKey 的两个输入 + frps 落点） */
    char     sn[IV_TUNNEL_SN_MAX];
    char     token[IV_TUNNEL_TOKEN_MAX];
    char     server_addr[IV_TUNNEL_ADDR_MAX];
    uint16_t server_port;

    /* 落点 */
    char conf_path[IV_TUNNEL_PATH_MAX];
    char flag_path[IV_TUNNEL_PATH_MAX];

    /* 运行态 */
    uint8_t  state;    /* iv_tunnel_state_t */
    uint64_t open_s;
    uint64_t expire_s; /* 0 ＝ 不设到期（BIRD 语义） */
    uint32_t ttl_s;
    uint16_t local_port; /* 固定 IV_TUNNEL_SSH_PORT，留字段便于状态回显 */

    iv_tunnel_session_t last;
    int                 have_last;

    iv_tunnel_stat_t    stat;
} iv_tunnel_t;

/* ---------------------------------------------------------------------------
 * 生命周期与配置
 * ------------------------------------------------------------------------- */

/* 清零 + 默认落点与端口。**未设置身份 / 服务端前，enable 一律被拒**。 */
void iv_tunnel_init(iv_tunnel_t *t);

/* 设置设备 SN 与平台令牌（secretKey 派生的两个输入）。空串 ⇒ IV_EINVAL（fail-closed）。 */
int iv_tunnel_set_identity(iv_tunnel_t *t, const char *sn, const char *token);

/* 设置 frps 地址与端口。addr 为空或超长 ⇒ IV_EINVAL / IV_ERANGE；port=0 ⇒ 取默认。 */
int iv_tunnel_set_server(iv_tunnel_t *t, const char *addr, uint16_t port);

/* 覆盖落点（配置文件与标记文件）。NULL 的那一项目前值保持不变。 */
int iv_tunnel_set_paths(iv_tunnel_t *t, const char *conf_path, const char *flag_path);

/* 身份或服务端变更时调用：关通道（STOP_RECONF）并作废配置，避免"新令牌配旧配置"。 */
int iv_tunnel_invalidate(iv_tunnel_t *t);

/* ---------------------------------------------------------------------------
 * 闸门
 * ------------------------------------------------------------------------- */

/*
 * 开启通道：校验 → 渲染 → 原子落盘（0600）→ 置标记文件。
 * `ttl_s` 为 0 ⇒ **不过期**（＝ BIRD 的行为）；> IV_TUNNEL_TTL_MAX ⇒ IV_ERANGE。
 * 已开启时重复调用 ⇒ IV_ESTATE（要改有效期，先 disable 再 enable，
 * 免得"悄悄换掉正在用的通道"）。
 * 成功返回 IV_OK；失败返回负码并填 *why（可为 NULL）。
 */
int iv_tunnel_enable(iv_tunnel_t *t, uint32_t ttl_s, uint64_t now_s,
                     iv_tunnel_reason_t *why);

/* 关闭通道：清标记文件 + 记会话留档。未开启 ⇒ IV_ESTATE。 */
int iv_tunnel_disable(iv_tunnel_t *t, uint64_t now_s, iv_tunnel_stop_reason_t reason);

/*
 * 认领"已经开着的通道"（装配层启动时调一次）。
 *
 * 为什么需要它：BIRD 里通道的存活**不依赖 Go 进程** —— 标记与 frpc 都归 `frp.sh` /
 * `monitor.sh` 管，重启 Go 服务时通道照旧在跑。本工程的 `state` 是内存态，进程一重启
 * 就归零；若不管，"通道明明开着、`tunnel.status` 却说 on=false、`tick()` 也不判过期"
 * 就会同时出现（监控与真相脱节比通道关掉更难查）。
 *
 * 读标记文件：不存在 ⇒ IV_ENOENT（保持 OFF）；空内容 ⇒ ON 且不过期（BIRD 的 `touch`）；
 * 内容是到期时刻且已过 ⇒ 删标记并返回 IV_ESTATE（守护脚本这一侧已经判过，这里兜底）。
 */
int iv_tunnel_restore(iv_tunnel_t *t, uint64_t now_s);

/* 周期驱动（装配层 1 s 级）：到期自动关闭。返回 1 表示本次关掉了。
 * `ttl_s == 0`（不过期）时恒返回 0。 */
int iv_tunnel_tick(iv_tunnel_t *t, uint64_t now_s);

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */
iv_tunnel_state_t iv_tunnel_state(const iv_tunnel_t *t);

/* 剩余有效秒数；未开启或不过期返回 0。 */
uint32_t iv_tunnel_remain_s(const iv_tunnel_t *t, uint64_t now_s);

/* 能力位（S8.4 协商用）：身份 / 服务端 / 落点 都就位才置 ENABLED 位。 */
#define IV_TUNNEL_CAP_SSH     (1u << 0) /* 通道转发的是本机 sshd（BIRD 同款） */
#define IV_TUNNEL_CAP_ENABLED (1u << 31)
uint32_t iv_tunnel_caps(const iv_tunnel_t *t);

void iv_tunnel_stat(const iv_tunnel_t *t, iv_tunnel_stat_t *out);

/* 最近一次会话留档（审计 / 诊断）。还没有任何会话返回 IV_ENOENT。 */
int iv_tunnel_last_session(const iv_tunnel_t *t, iv_tunnel_session_t *out);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_TUNNEL_H */
