/*
 * 受控远程访问通道（libivmodules，功能开发计划 M8-S8.1~S8.3）
 *
 * ============================ 它是什么 ============================
 * 架构 §9.4 的**设备侧**那一半：4G 私网 / 客户网段边界下，平台与运维人员无法主动
 * 拨进设备，只能由**设备主动外连**建立反向通道。本模块不自己写隧道，而是按
 * **BIRD 运维箱（bird_20260623）已跑通的同款方式**实现：
 *
 *   frpc（设备侧）── 外连 ──> frps（运维服务器）
 *        ↑ stcp 代理项：只有**持有同一个 secretKey 的访问者**（另一侧的 frpc
 *          visitor）才连得上来，端口不在公网上暴露
 *
 * 对应到参考项目（bird_20260623）：
 *   - 承载：`/etc/frp/frpc.toml` + `frpc` 进程（`bird/scripts/frp.sh`）；
 *   - 代理类型：**stcp**（`type = "stcp"` + `secretKey`），不用 `tcp` ——
 *     `tcp` 会把端口直接开在 frps 上，任何扫到端口的人都能连，那不叫"受控"；
 *   - 开关闸门：**标记文件**（BIRD 的 `/tmp/frp.enabled`）+ `monitor.sh` 只在标记
 *     存在时守护 frpc —— **标记文件是"业务上需不需要"的唯一真相**，进程在不在不算；
 *   - 钥匙：secretKey 由 **设备 SN + 平台下发令牌** 派生（BIRD 的
 *     `utils.GenerateSTCPKey(sn, token)`）。
 *
 * ============================ 本模块管什么、不管什么 ============================
 *   管：服务名白名单 → 本机代理入口的映射；secretKey 派生；frpc.toml 渲染与原子落盘
 *       （0600，保护 secretKey）；标记文件的置与清；**有效期闸门**（到期自动关闭）；
 *       会话时长与拒绝计数。
 *   不管：frpc 进程启停（归 `scripts/frp.sh` 与 `scripts/monitor.sh`）、数据面与
 *       加密（归 frp）、摄像机页面兼容（归架构 §9.3 / S8.5 的受控代理）、
 *       平台命令（归 S8.4 能力协商）。
 *   **理由**：进程与连线属"运维体系"层，塞进模块层会让单测变成"起真进程"；这一层
 *   最该钉死的是"哪些服务能被代理、钥匙怎么派生、文件怎么落盘、到期会不会自己关"。
 *
 * ============================ 服务名白名单（架构 §9.4「禁止任意 ip:port」） ============================
 * 只接受 `web` / `camera:<id>` / `media:<id>` / `ftp`，由**注入式 resolver** 映射成本机代理
 * 入口。frp 的 `localIP` 只可能是本机可达地址，因此本模块**硬性要求 resolver 给出的
 * 目标必须是环回地址（127.0.0.0/8）**：
 *   - `web`      → 本机 Web 服务端口（ivsbox-web）；
 *   - `camera:N` → 本机**摄像机受控代理**端口（S8.5：ivsbox-web 按设备档案把请求转发
 *                  到摄像机，摄像机凭据只留在设备侧、绝不下发给浏览器）；
 *   - `media:N`  → 本机媒体读取端口；
 *   - `ftp`      → 本机 vsftpd：控制口（resolver 给）+ 被动数据口（`ftp_data_port`）。
 *                  **一条服务名渲染两条 stcp 代理**（`<sn>-ftp` / `<sn>-ftpdata`）——
 *                  FTP 的数据连接是另一条 TCP，一条 frp 代理转不了两个端口。
 * 也就是说：**摄像机 IP 从不出现在通道配置里** —— 想访问摄像机只能过设备上的受控
 * 代理，这正是 §9.4 白名单想要的结果。非环回目标一律拒（`EHOST`）。
 *
 * ============================ secretKey 派生（与运维侧字节级一致） ============================
 *   secretKey = 小写 hex( MD5( sn + token + svc ) 的前 8 字节 )  → 16 个字符
 * 与 BIRD 的 `GenerateSTCPKey` 同源（BIRD 只有单服务，`svc` 为空串时公式完全等价）。
 * **为什么沿用 MD5**：访问方要拿同一把钥匙，两侧公式必须一致；换更强的算法需要运维
 * 侧同步改造（已登记为待确认项）。此处是**共享密钥派生**用途，不是完整性校验；令牌
 * 由平台下发、不入库（fail-closed：缺令牌就开不了通道）。
 * **每服务独立派生**：`svc` 进公式，所以"授权看 Web"不会顺带把摄像机 Web 也授权出去
 * （架构 §9.4「令牌必须绑定……服务」）。
 *
 * ============================ 有效期闸门 ============================
 * 通道默认关（BIRD 亦为"按需开"）。`iv_tunnel_enable()` 必须带有效期（默认 3600 s、
 * 上限 86400 s），到期由 `iv_tunnel_tick()` 自动关：**远程入口不能靠人记得去关**。
 * 关闭＝清标记文件（随后 `frp.sh` / `monitor.sh` 停掉 frpc）+ 记录会话时长；配置文件
 * 保留（0600），不因关闭而丢失"下次怎么连"的信息。
 *
 * **到期闸门不依赖主控存活**：`tick()` 跑在 `ivsboxd` 里，主控一旦不在，光靠它就会
 * 让通道"开着没人管"。故**标记文件的内容就是到期时刻**（unix 秒，十进制一行）——
 * `scripts/frp.sh` 与 `scripts/monitor.sh` 能**独立**判过期并收掉 frpc（见脚本内的
 * `flag_expired`）。两道闸门互为兜底：进程内 `tick()` 更及时，脚本侧在主控死后仍有效。
 * 标记里没有数字（例如运维手工 `touch`）时按"未过期"处理 —— 不擅自关掉别人手工开的
 * 通道，这一点写在脚本注释里。
 *
 * ============================ 内存 / 并发 / 依赖 ============================
 *   - **零 malloc**：句柄由调用方持有（约 2 KB）；
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
#define IV_TUNNEL_NAME_MAX    48u   /* frp 代理项名 `<sn>-<svc>`（含 NUL） */
#define IV_TUNNEL_SVC_MAX     24u   /* 服务名（含 NUL） */
#define IV_TUNNEL_PATH_MAX    256u  /* 配置 / 标记文件路径（含 NUL） */
#define IV_TUNNEL_PROXIES_MAX 8u    /* 单次开启的代理项上限 */
#define IV_TUNNEL_TXT_MAX     4096u /* 渲染出的 frpc.toml 上限 */

#define IV_TUNNEL_PORT_DEF 7000u    /* frps 默认端口（BIRD 同款） */
#define IV_TUNNEL_TTL_DEF  3600u    /* 默认有效期 1 小时 */
#define IV_TUNNEL_TTL_MAX  86400u   /* 有效期上限 24 小时 */

/* 默认落点（与 BIRD 同构；装配层可用 iv_tunnel_set_paths() 覆盖） */
#define IV_TUNNEL_CONF_DEFAULT "/etc/frp/frpc.toml"
#define IV_TUNNEL_FLAG_DEFAULT "/tmp/ivsbox-tunnel.enabled"

/* ---------------------------------------------------------------------------
 * 服务名（白名单的载体）
 * ------------------------------------------------------------------------- */
typedef enum {
    IV_TUNNEL_SVC_WEB = 0,    /* 本机 Web（`web`） */
    IV_TUNNEL_SVC_CAMERA = 1, /* 摄像机受控代理（`camera:<id>`） */
    IV_TUNNEL_SVC_MEDIA = 2,  /* 本机媒体读取（`media:<id>`） */
    IV_TUNNEL_SVC_FTP = 3     /* 文件下载（`ftp`）—— **一条服务名、两条代理** */
} iv_tunnel_svc_kind_t;

/* `ftp` 的被动模式数据端口默认值。必须与设备侧 vsftpd 的
 * `pasv_min_port` / `pasv_max_port` 同值：frp 一条代理只映射一个端口，数据端口
 * 只能**先钉死**再写进 frpc.toml 与访问端 visitor —— 端口随机就没法预置。 */
#define IV_TUNNEL_FTP_DATA_DEF 50000u

typedef struct {
    iv_tunnel_svc_kind_t kind;
    unsigned             index; /* camera:/media: 的下标；web 恒 0 */
} iv_tunnel_svc_t;

/* 摄像机隧道上限 6（与 `IV_DEV_MAX` 无关：探测档案 2026-10-27 起放到 10 台，
 * 但每台隧道要占一个 frpc 代理，产品只开 6 条）；媒体 8（录像侧口径）。 */
#define IV_TUNNEL_CAM_MAX   6u
#define IV_TUNNEL_MEDIA_MAX 8u

/*
 * 服务名 → 本机代理入口（装配层注入）。
 * 回填 `*ip_be`（网络序）与 `*port`（主机序）；**必须是环回地址**，否则本模块拒绝
 * （`EHOST`）。resolver 未注入 ⇒ 一律拒绝（fail-closed）。
 */
typedef struct {
    int (*resolve)(const iv_tunnel_svc_t *svc, uint32_t *ip_be, uint16_t *port, void *arg);
    void *arg;
    /* 仅 `ftp` 用：被动模式的数据端口。0 ⇒ 取 IV_TUNNEL_FTP_DATA_DEF。
     * 放在这里而不是再加一个 resolver 回调：FTP 的数据口是本机固定监听，
     * 与"服务名→入口"的映射无关，装配层从配置读一次即可。 */
    uint16_t ftp_data_port;
} iv_tunnel_resolver_t;

/* ---------------------------------------------------------------------------
 * 拒绝原因（诊断 / 上报 / 单测断言用；数值稳定，可落库）
 * ------------------------------------------------------------------------- */
typedef enum {
    IV_TUNNEL_OK = 0,
    IV_TUNNEL_EBADARG = 1,  /* 参数非法：服务名不在白名单 / 数量越界 / 有效期越界 */
    IV_TUNNEL_ENOIDENT = 2, /* 缺身份：SN 或令牌为空 */
    IV_TUNNEL_ENOSERVER = 3,/* 缺 frps 地址或端口 */
    IV_TUNNEL_EUNMAPPED = 4,/* resolver 未注入，或该服务名当前不可达 */
    IV_TUNNEL_EHOST = 5,    /* 映射出的目标不是环回地址（想代理到网内别的机器） */
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
 * 解析服务名。合法：`web`；`camera:N`（十进制、无前导零、< 6）；`media:N`（< 8）；
 * `ftp`（无下标，`ftp:21` 是非法形态）。
 * 非法（`ip:port`、域名、URL、`web:80`、`camera:`、`camera:06`、`camera:6`…）：
 * IV_EINVAL 形态不符 / IV_ERANGE 下标越界。
 */
int iv_tunnel_svc_parse(const char *s, size_t len, iv_tunnel_svc_t *out);

/* 反向渲染（审计 / 日志用）。cap 不足返回 IV_ERANGE，out 仍 NUL 终止。 */
int iv_tunnel_svc_format(const iv_tunnel_svc_t *svc, char *out, size_t cap);

/*
 * secretKey 派生：hex( MD5(sn + token + svc)[0..8) )，写 16 个字符 + NUL。
 * `svc` 为 NULL 或空串 ⇒ BIRD 的原始公式（单服务场景）。
 * 返回 IV_OK / IV_EINVAL（参数）/ IV_ERANGE（cap < IV_TUNNEL_SECRET_MAX）。
 */
int iv_tunnel_secret(const char *sn, const char *token, const char *svc,
                     char *out, size_t cap);

/*
 * 渲染 frpc.toml 文本（**不落盘**，纯拼装，便于单测逐字钉内容）。
 * `svcs`/`n` 为本次要开放的服务名；`out` 缓冲不足返回 IV_ERANGE；
 * 任一服务名非法 / 不可达 / 目标非环回 ⇒ 负码（原因见 `why`）。
 * 成功时 `*out_len` 为写入字节数（不含结尾 NUL）。
 */
int iv_tunnel_render(const char *sn, const char *token, const char *server_addr,
                     uint16_t server_port, const iv_tunnel_resolver_t *resolver,
                     const iv_tunnel_svc_t *svcs, unsigned n,
                     char *out, size_t cap, size_t *out_len,
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
    uint64_t               open_s;
    uint64_t               close_s;
    uint32_t               ttl_s;
    unsigned               n_svc;
    iv_tunnel_svc_t        svc[IV_TUNNEL_PROXIES_MAX];
    uint8_t                stop_reason;
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

    const iv_tunnel_resolver_t *resolver;

    /* 运行态 */
    uint8_t             state;      /* iv_tunnel_state_t */
    uint64_t            open_s;
    uint64_t            expire_s;
    uint32_t            ttl_s;
    unsigned            n_svc;
    iv_tunnel_svc_t     svc[IV_TUNNEL_PROXIES_MAX];

    iv_tunnel_session_t last;
    int                 have_last;

    iv_tunnel_stat_t    stat;
} iv_tunnel_t;

/* ---------------------------------------------------------------------------
 * 生命周期与配置
 * ------------------------------------------------------------------------- */

/* 清零 + 默认落点与端口。**未设置身份 / 服务端 / resolver 前，enable 一律被拒**。 */
void iv_tunnel_init(iv_tunnel_t *t);

/* 设置设备 SN 与平台令牌（secretKey 派生的两个输入）。空串 ⇒ IV_EINVAL（fail-closed）。 */
int iv_tunnel_set_identity(iv_tunnel_t *t, const char *sn, const char *token);

/* 设置 frps 地址与端口。addr 为空或超长 ⇒ IV_EINVAL / IV_ERANGE；port=0 ⇒ 取默认。 */
int iv_tunnel_set_server(iv_tunnel_t *t, const char *addr, uint16_t port);

/* 注入服务名→本机代理入口映射。NULL ⇒ 解除映射（此后一律拒绝）。 */
int iv_tunnel_set_resolver(iv_tunnel_t *t, const iv_tunnel_resolver_t *r);

/* 覆盖落点（配置文件与标记文件）。NULL 的那一项目前值保持不变。 */
int iv_tunnel_set_paths(iv_tunnel_t *t, const char *conf_path, const char *flag_path);

/* 身份或服务端变更时调用：关通道（STOP_RECONF）并作废配置，避免"新令牌配旧配置"。 */
int iv_tunnel_invalidate(iv_tunnel_t *t);

/* ---------------------------------------------------------------------------
 * 闸门
 * ------------------------------------------------------------------------- */

/*
 * 开启通道：校验 → 渲染 → 原子落盘（0600）→ 置标记文件。
 * `ttl_s` 为 0 取默认；> IV_TUNNEL_TTL_MAX ⇒ IV_ERANGE。
 * 已开启时重复调用 ⇒ IV_ESTATE（要改服务集合，先 disable 再 enable，
 * 免得"悄悄换掉正在用的通道"）。
 * 成功返回 IV_OK；失败返回负码并填 *why（可为 NULL）。
 */
int iv_tunnel_enable(iv_tunnel_t *t, const iv_tunnel_svc_t *svcs, unsigned n,
                     uint32_t ttl_s, uint64_t now_s, iv_tunnel_reason_t *why);

/* 关闭通道：清标记文件 + 记会话留档。未开启 ⇒ IV_ESTATE。 */
int iv_tunnel_disable(iv_tunnel_t *t, uint64_t now_s, iv_tunnel_stop_reason_t reason);

/* 周期驱动（装配层 1 s 级）：到期自动关闭。返回 1 表示本次关掉了。 */
int iv_tunnel_tick(iv_tunnel_t *t, uint64_t now_s);

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */
iv_tunnel_state_t iv_tunnel_state(const iv_tunnel_t *t);

/* 剩余有效秒数；未开启返回 0。 */
uint32_t iv_tunnel_remain_s(const iv_tunnel_t *t, uint64_t now_s);

/* 能力位（S8.4 协商用）：身份 / 服务端 / resolver / 落点 都就位才置 ENABLED 位。 */
#define IV_TUNNEL_CAP_WEB     (1u << 0)
#define IV_TUNNEL_CAP_CAMERA  (1u << 1)
#define IV_TUNNEL_CAP_MEDIA   (1u << 2)
#define IV_TUNNEL_CAP_FTP     (1u << 3)
#define IV_TUNNEL_CAP_ENABLED (1u << 31)
uint32_t iv_tunnel_caps(const iv_tunnel_t *t);

void iv_tunnel_stat(const iv_tunnel_t *t, iv_tunnel_stat_t *out);

/* 最近一次会话留档（审计 / 诊断）。还没有任何会话返回 IV_ENOENT。 */
int iv_tunnel_last_session(const iv_tunnel_t *t, iv_tunnel_session_t *out);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_TUNNEL_H */
