/*
 * IVSBox 平台触发升级的**拉包**一半（平台下行 0xB3「更新系统」）
 *
 * ============================ 它是什么 ============================
 * `0xB3` 的语义是「平台按下升级按钮」；参考固件的做法（`General_Version/main/APP/Update/`）
 * 不是"平台把固件推下来"，而是**让设备自己去升级服务器取**：
 *
 *   1. `GET /<硬件名>/info.txt` → JSON `{"version":"<硬件名>-<软件版本>","url":"http://…"}`
 *   2. 版本与本机一致 ⇒ 什么都不做（参考实现**连应答都不发**，本工程改为回 0x01，
 *      见 `iv_agent` 侧注释：平台等一帧 ACK 等到超时，现场只会觉得"升级按钮没反应"）
 *   3. `HEAD <url>` 取 Content-Length；`GET <url>` ＋ `Range:` 分块取回
 *   4. 落盘 → **交给本地安装链路**（`iv_upd_request_write()`：只写一行请求文件，
 *      由 `scripts/monitor.sh` 拉起 `ivsbox-updater install`）——本模块**不装盘**
 *
 * 分工是硬的：本模块只回答"**包怎么从服务器搬下来**"；装盘/验签/双版本切换归
 * `iv_upd`，包合法与否归 `iv_ota`。三者谁也不替谁判断。
 *
 * ============================ 两种分块格式（为什么有两种） ============================
 * 参考固件的包是 STM32 的 `crc.bin`，按 **1026 B 一帧**（1024 B 数据 ＋ 2 B
 * big-endian CRC16/MODBUS 尾）打包，服务端要求文件长度是 1026 的整数倍
 * （`http_update_get_crc_bin_size()` 里 `len % UPDATE_CHUNK_SIZE` 直接判错）。
 * 这套帧格式是**它自己刷 Flash 的产物**，不是 HTTP 的约定。
 *
 * 本工程的升级包是 `IVSOTA01`（带清单与 Ed25519 签名的整体包），完整性由
 * `iv_ota` 的 SHA-256 ＋ 签名保证，**不需要**逐帧 CRC。但现场那台升级服务器
 * 到底会不会给 Linux 产品套 1026 帧，现在**无从得知**（发布侧还没上过 T113 的包）。
 * 所以按"能验证的事实"分派，不猜：
 *
 *   - `Content-Length % 1026 == 0` ⇒ **帧模式**（与参考逐字一致，逐帧验 CRC）
 *   - 否则                        ⇒ **流模式**（一次 GET，按 Content-Length 落盘）
 *   - 帧模式下**第一帧 CRC 就对不上** ⇒ 打 WARN、退回流模式重下一次（只试一次）。
 *     这条兜底针对的是"服务器直接给裸包、长度又恰好是 1026 的整数倍"这种真实可能
 *     ——没有它，设备会在一个纯格式问题上死循环，而现场只看到"升级失败"。
 *
 * 最终判据永远是**装盘时 `iv_ota` 的验签**：帧 CRC 只是传输层的廉价自检。
 *
 * ============================ 落点与"谁改盘" ============================
 * ```text
 * <data_root>/ota/download/<文件名>   本模块下载落点（先写 .part，成功后 rename）
 * <data_root>/ota/request             安装请求（一行：<包绝对路径> <restart 0|1>）
 * ```
 * 本模块**只写这两个文件**：不改 `current`/`previous` 链接、不碰 `pending.json`
 * （那是 `iv_upd` 的单写者约定）。下载走 `.part` ＋ `rename`，保证请求文件指向的
 * 永远是**完整**的包。
 *
 * ============================ 内存 / 线程 / 依赖 ============================
 *   - 句柄内嵌进 `iv_agent_t`（零 malloc），含约 16 KiB 的窗口缓冲
 *     ⇒ **不要放栈上**（与 `iv_logup_t` 同一条纪律）；
 *   - **阻塞 IO**：只能在慢任务池 worker 里跑（架构 §4.2），绝不上 Reactor 线程；
 *   - 出口网卡用 `SO_BINDTODEVICE` 绑定（双 WAN 的坑与 `iv_logup`／传输层同一个）；
 *   - 依赖 `iv_crc`（帧 CRC）、`iv_upd`（请求文件）；**不依赖 json-c**，
 *     info.txt 体量小、字段固定，直接在该层做严格扫描。
 *
 * ============================ 返回码 ============================
 *   IV_OK          下载完成且请求文件已落盘
 *   IV_UPDFETCH_SAME(1) 服务器版本与本机相同 —— **不是错误**
 *   IV_EINVAL      未 init / host 空 / 版本串空 / 落点目录不可写
 *   IV_ECONN       DNS / connect / send / recv 失败
 *   IV_EPROTO      HTTP 非 2xx、info.txt 解析失败、帧 CRC 连续不符
 *   IV_ERANGE      包比 `max_bytes` 还大（防"服务器给错文件"把 UDISK 写满）
 *   IV_EIO         落盘/rename 失败
 *   IV_ENOSPC      写盘时 ENOSPC
 */
#ifndef IVSBOX_IV_UPDFETCH_H
#define IVSBOX_IV_UPDFETCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 默认值 ---- */
/* 硬件名：info.txt 的路径段与版本号前缀都用它（参考 `HARD_NO_STR "FN-YJTY-PY-8"`）。
 * 与 `IV_UPD_DEV_MODEL_DEFAULT` 同口径（"IVSBOX-T113"），发布侧要按它建目录；
 * 装配层可用环境变量 `IVSBOX_OTA_HW` 覆盖（工装/现场临时对另一套命名时用）。 */
#define IV_UPDFETCH_HW_DEFAULT "IVSBOX-T113"
/* 参考 `UPDATE_CHUNK_SIZE (1024 + 2)`：1024 数据 ＋ 2 字节 CRC16 尾 */
#define IV_UPDFETCH_FRAME_SIZE 1026u
#define IV_UPDFETCH_FRAME_DATA 1024u
/* 一次 Range 取 16 帧（16416 B）：既不是"一帧一请求"（十几 MB 的包要上万次
 * 往返），也不至于让单次应答大到难以判错。帧模式的有效性不受窗口大小影响。 */
#define IV_UPDFETCH_WIN_FRAMES 16u
#define IV_UPDFETCH_WIN_BYTES (IV_UPDFETCH_WIN_FRAMES * IV_UPDFETCH_FRAME_SIZE)
/* 单次下载上限：T113 的应用包按 rootfs 1.5 GiB 双版本受控，64 MiB 已远超实际；
 * 超过就判 ERANGE（宁可拒绝，也不要把 UDISK 写满 —— 写满会连配置与日志一起废）。 */
#define IV_UPDFETCH_MAX_BYTES_DEFAULT (64u * 1024u * 1024u)
#define IV_UPDFETCH_NAME_DEFAULT "ivsbox.pkg"
#define IV_UPDFETCH_DOWNLOAD_SUBDIR "download"

/* 版本号相同：**不是错误**，调用方按"已是最新"记一行日志即可 */
#define IV_UPDFETCH_SAME 1

/* 分块模式（统计与日志用） */
#define IV_UPDFETCH_MODE_NONE 0   /* 还没定 */
#define IV_UPDFETCH_MODE_FRAME 1  /* 1026 B 帧 ＋ 每帧 CRC16 */
#define IV_UPDFETCH_MODE_STREAM 2 /* 整包流式 */

/* 缓冲尺寸 */
#define IV_UPDFETCH_HOST_MAX 64u
#define IV_UPDFETCH_BIND_MAX 16u
#define IV_UPDFETCH_HW_MAX 32u
#define IV_UPDFETCH_VER_MAX 64u
#define IV_UPDFETCH_ROOT_MAX 96u
#define IV_UPDFETCH_NAME_MAX 48u
#define IV_UPDFETCH_PATH_MAX 192u
#define IV_UPDFETCH_URL_MAX 256u
#define IV_UPDFETCH_HDR_MAX 1024u
#define IV_UPDFETCH_INFO_MAX 4096u /* info.txt 体：参考实现按 2 KiB 缓冲、8 KiB 上限 */

typedef struct {
    const char *host;     /* **必填**：升级服务器地址（`net.server.upgrade.host`） */
    uint16_t    port;     /* 0 → 8989（`net.server.upgrade.port` 出厂值） */
    const char *bind_dev; /* 出口网卡（usb0/eth0）；NULL/"" = 由内核按路由选 */
    const char *hw_name;  /* info.txt 路径段/版本前缀；NULL → IV_UPDFETCH_HW_DEFAULT */
    const char *ver_str;  /* **必填**：本机软件版本串（与 info.txt 的 version 比），
                           * 取 `iv_version_sysver()`（形如 "1.0.0.20261020"） */
    const char *data_root;/* NULL → /mnt/UDISK/ivsbox（与 iv_upd 同口径） */
    const char *dest_name;/* 落盘文件名；NULL → "ivsbox.pkg"。**只允许安全字符** */
    uint32_t    max_bytes;/* 0 → IV_UPDFETCH_MAX_BYTES_DEFAULT */
    uint32_t    connect_ms; /* 0 → 5000 */
    uint32_t    send_ms;    /* 0 → 10000 */
    uint32_t    recv_ms;    /* 0 → 20000（4G 上一个 16 KiB 窗口实测可能 >10 s） */
} iv_updfetch_cfg_t;

typedef struct {
    uint64_t runs;       /* 累计执行次数（含失败） */
    uint64_t ok;         /* 下载完成次数 */
    uint64_t same;       /* "已是最新"次数 */
    uint64_t fail;       /* 失败次数 */
    uint64_t bytes;      /* 累计落盘字节数 */
    uint64_t frames;     /* 累计通过 CRC 的帧数（流模式恒 0） */
    int      mode;       /* 最近一次用的模式（IV_UPDFETCH_MODE_*） */
    int      last_rc;    /* 最近一次 `iv_updfetch_run()` 返回码 */
    int      last_http;  /* 最近一次 HTTP 状态码（0 = 没连上/没应答） */
    uint32_t last_bytes; /* 最近一次落盘字节数 */
    char     info_version[IV_UPDFETCH_VER_MAX]; /* info.txt 里读到的版本串 */
    char     url[IV_UPDFETCH_URL_MAX];          /* info.txt 里读到的包 URL */
    char     file[IV_UPDFETCH_NAME_MAX];        /* 最近一次落盘文件名 */
} iv_updfetch_stats_t;

/* 句柄：内嵌进 `iv_agent_t`（约 17 KiB，**不要放栈上**） */
typedef struct {
    /* 配置（字符串落在下方定长缓冲里，init 后自洽） */
    char     host[IV_UPDFETCH_HOST_MAX];
    char     bind_dev[IV_UPDFETCH_BIND_MAX];
    char     hw[IV_UPDFETCH_HW_MAX];
    char     ver[IV_UPDFETCH_VER_MAX];
    char     root[IV_UPDFETCH_ROOT_MAX];
    char     name[IV_UPDFETCH_NAME_MAX];
    uint16_t port;
    uint32_t max_bytes;
    uint32_t connect_ms;
    uint32_t send_ms;
    uint32_t recv_ms;
    int      inited;

    /* 一次下载的会话状态（只在 run() 内用） */
    int      fd;              /* -1 = 未连 */
    int      running;         /* 1 = run() 在飞（防中途改地址） */
    int      status;          /* 最近一次 HTTP 状态码 */
    int      need_reconnect;  /* 1 = 下一次请求前重连（Connection: close / 断链） */
    int      mode;            /* 本次用的模式 */
    size_t   hdr_len;         /* 应答头已收长度 */
    size_t   body_off;        /* 应答头之后 body 在 hdr 里的起点（可能已带一段 body） */
    long     body_len;        /* 应答头的 Content-Length（-1 = 没有） */
    long     file_size;       /* 包总长（HEAD 得来） */
    char     conn_host[IV_UPDFETCH_HOST_MAX]; /* 当前连接的目标（换目标即重连） */
    uint16_t conn_port;
    char     url_host[IV_UPDFETCH_HOST_MAX];
    char     url_path[IV_UPDFETCH_PATH_MAX];
    uint16_t url_port;
    char     info_version[IV_UPDFETCH_VER_MAX];

    /* 缓冲 */
    char     hdr[IV_UPDFETCH_HDR_MAX];
    uint8_t  win[IV_UPDFETCH_WIN_BYTES]; /* 窗口 / info.txt 体 / 流模式搬运块 */

    iv_updfetch_stats_t st;
} iv_updfetch_t;

/* 填默认配置（根/端口/上限/超时全默认；host 与 ver_str 为空 ⇒ run 返回 IV_EINVAL） */
void iv_updfetch_cfg_default(iv_updfetch_cfg_t *cfg);

/*
 * 初始化（复制配置、清统计、fd = -1）。重复 init 返回 IV_ESTATE。
 * **不建线程、不连网**：连接在 `iv_updfetch_run()` 里现建。
 * host/ver_str 为空**不算失败**（返回 IV_OK 并打 WARN）：装配层可能先 init、
 * 等配置读到再 `iv_updfetch_set_target()` 补地址。
 */
int iv_updfetch_init(iv_updfetch_t *u, const iv_updfetch_cfg_t *cfg);

/*
 * 换地址/端口/出口网卡/版本串（已 init 且未在跑时随时可调）。
 *
 * 与 `iv_logup_set_target()` 同一个理由：地址来自配置、出口会随 netmgr 切换、
 * 版本串来自本机版本宏 —— 每次 `run()` 前由装配层按"这一刻"的事实刷新一遍。
 * 正在下载时调本接口**不生效**（IV_EBUSY）。
 */
int iv_updfetch_set_target(iv_updfetch_t *u, const char *host, uint16_t port,
                           const char *bind_dev, const char *ver_str);

/* 关句柄（关 socket、清配置）。不是必须调用，但重启前调用可立即释放 fd。 */
void iv_updfetch_close(iv_updfetch_t *u);

/*
 * 同步跑一次"取 info.txt → 比版本 → 下载 → 写安装请求"。
 * **只能由 worker 线程调用**（阻塞 IO，架构 §4.2）。
 *
 * 返回 IV_OK（已下载并写好请求）、`IV_UPDFETCH_SAME`（已是最新，什么都没改），
 * 或负的 IV_* 错误码（见文件头返回码表）。成败细节看 `iv_updfetch_stats()`。
 */
int iv_updfetch_run(iv_updfetch_t *u);

/* 取统计快照（逐字段整拷；可在任意线程读） */
void iv_updfetch_stats(const iv_updfetch_t *u, iv_updfetch_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_UPDFETCH_H */
