/*
 * IVSBox 平台触发升级的**拉包**一半（平台下行 0xB3「更新系统」）
 *
 * ============================ 它是什么 ============================
 * `0xB3` 的语义是「平台按下升级按钮」；参考固件的做法（`General_Version/main/APP/Update/`）
 * 不是"平台把固件推下来"，而是**让设备自己去升级服务器取**：
 *
 *   1. `GET /<硬件名>/info.txt` → JSON `{"version":"<硬件名>-<软件版本>","url":"http://…"}`
 *      **没有 info.txt（404）⇒ 退回读目录列表**（autoindex 里挑文件名版本最大的
 *      `*.pkg`，见"目录列表模式"）—— 发布侧只放一个包时走这条；
 *   2. 版本与本机一致 ⇒ 什么都不做（参考实现**连应答都不发**，本工程由 `iv_agent`
 *      回一帧"已是最新"的结果帧，见那边的注释）
 *   3. `GET <url>` 整包流式下载（**纯 HTTP，不套任何私有分帧格式**）
 *   4. 落盘 → **交给本地安装链路**（`iv_upd_request_write()`：只写一行请求文件，
 *      由 `scripts/monitor.sh` 拉起 `ivsbox-updater install`）——本模块**不装盘**
 *
 * 分工是硬的：本模块只回答"**包怎么从服务器搬下来**"；装盘/验签/双版本切换/回滚
 * 归 `iv_upd`，包合法与否归 `iv_ota`。三者谁也不替谁判断。
 *
 * ============================ 为什么不再有 1026 帧模式 ============================
 * 第一版曾照参考固件实现"1026 B 一帧（1024 数据 ＋ 2 B CRC16 尾）、16 帧一窗、
 * 首帧 CRC 不符退回整包"的两套路径。**2026-11-02 用户口径：升级不按单片机那套，
 * 走常规 Linux** —— 于是整条帧路径被删掉：
 *   - 参考的 1026 帧是它自己刷 SPI Flash 的产物，与 HTTP 无关；
 *   - 本工程的包是 `IVSOTA01`（清单 ＋ SHA-256 ＋ Ed25519），完整性由 `iv_ota`
 *     在装盘时 fail-closed 校验，传输层再做一遍 CRC 只是**多一处会误判的判据**；
 *   - 两套路径意味着两倍的降级逻辑与两倍的用例，而现场只会看到"升级失败"。
 * 现在只有一条路径：`GET` 到底，边收边写，长度以 `Content-Length` 为准。
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
 *   - 句柄内嵌进 `iv_agent_t`（零 malloc），含 8 KiB 搬运缓冲
 *     ⇒ **不要放栈上**（与 `iv_logup_t` 同一条纪律）；
 *   - **阻塞 IO**：只能在慢任务池 worker 里跑（架构 §4.2），绝不上 Reactor 线程；
 *   - 出口网卡用 `SO_BINDTODEVICE` 绑定（双 WAN 的坑与 `iv_logup`／传输层同一个）；
 *   - 依赖 `iv_upd`（请求文件）；**不依赖 json-c**，info.txt 体量小、字段固定，
 *     直接在该层做严格扫描；也**不再依赖 `iv_crc`**。
 *
 * ============================ 返回码 ============================
 *   IV_OK          下载完成且请求文件已落盘
 *   IV_UPDFETCH_SAME(1) 服务器版本与本机相同 —— **不是错误**
 *   IV_EINVAL      未 init / host 空 / 版本串空 / 落点目录不可写
 *   IV_ECONN       DNS / connect / send / recv 失败（含服务器提前关连接）
 *   IV_EPROTO      HTTP 非 200、info.txt 解析失败
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
/* 硬件名：`/<硬件名>/` 的路径段，也是 info.txt 里 version 的前缀
 * （参考 `HARD_NO_STR "FN-YJTY-PY-8"`）。
 * **2026-11-02：发布侧给本产品的目录是 `FN_RZTY_PY`**（`/FN_RZTY_PY/` 里直接放包，
 * 没有 info.txt，见下面"目录列表模式"）。它与型号 `IV_UPD_DEV_MODEL_DEFAULT`
 * （"IVSBOX-T113"）**不是一回事**：型号不符 ⇒ `model-mismatch` 拒包；
 * 硬件名不符 ⇒ 只是 404 / 取不到列表。两者写错的表现完全不同，别混。
 * 现场/工装要临时对另一套命名，用环境变量 `IVSBOX_OTA_HW` 覆盖（`init` 时读一次）。 */
#define IV_UPDFETCH_HW_DEFAULT "FN_RZTY_PY"
/* 单次下载上限：T113 的应用包按 rootfs 1.5 GiB 双版本受控，64 MiB 已远超实际；
 * 超过就判 ERANGE（宁可拒绝，也不要把 UDISK 写满 —— 写满会连配置与日志一起废）。
 * 边下边计数，**不等服务器报长度**：先落半截再判超限也算拒绝。 */
#define IV_UPDFETCH_MAX_BYTES_DEFAULT (64u * 1024u * 1024u)
#define IV_UPDFETCH_NAME_DEFAULT "ivsbox.pkg"
#define IV_UPDFETCH_DOWNLOAD_SUBDIR "download"
/* 目录列表模式认的包后缀（大小写不敏感）。只在没有 info.txt 时用得上。 */
#define IV_UPDFETCH_PKG_SUFFIX ".pkg"

/* 版本号相同：**不是错误**，调用方按"已是最新"处理 */
#define IV_UPDFETCH_SAME 1

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
/* 一次 recv/fwrite 的搬运块，同时也是 info.txt 的读取上限（参考实现按 2 KiB 缓冲）。
 * 4 KiB 在 4G 上是一个折中：太小则 syscall 次数多，太大则单次 recv 超时窗口内
 * 必须凑满，反而更容易误判超时。 */
#define IV_UPDFETCH_BUF 4096u

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
    uint32_t    recv_ms;    /* 0 → 20000（4G 上 4 KiB 一块实测可能 >10 s） */
} iv_updfetch_cfg_t;

typedef struct {
    uint64_t runs;       /* 累计执行次数（含失败） */
    uint64_t ok;         /* 下载完成次数 */
    uint64_t same;       /* "已是最新"次数 */
    uint64_t fail;       /* 失败次数 */
    uint64_t listing;    /* 走"目录列表"而非 info.txt 的次数（退化路径用量） */
    uint64_t bytes;      /* 累计落盘字节数 */
    int      last_rc;    /* 最近一次 `iv_updfetch_run()` 返回码 */
    int      last_http;  /* 最近一次 HTTP 状态码（0 = 没连上/没应答） */
    uint32_t last_bytes; /* 最近一次落盘字节数 */
    char     info_version[IV_UPDFETCH_VER_MAX]; /* info.txt／列表里读到的版本串 */
    char     url[IV_UPDFETCH_URL_MAX];          /* info.txt 里读到的包 URL */
    char     file[IV_UPDFETCH_NAME_MAX];        /* 最近一次落盘文件名 */
} iv_updfetch_stats_t;

/* 句柄：内嵌进 `iv_agent_t`（约 5 KiB，**不要放栈上**） */
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
    size_t   hdr_len;         /* 应答头已收长度 */
    size_t   body_off;        /* 应答头之后 body 在 hdr 里的起点（可能已带一段 body） */
    long     body_len;        /* 应答头的 Content-Length（-1 = 没有） */
    long     file_size;       /* 包总长（GET 应答的 Content-Length，-1 = 未报） */
    char     conn_host[IV_UPDFETCH_HOST_MAX]; /* 当前连接的目标（换目标即重连） */
    uint16_t conn_port;
    char     url_host[IV_UPDFETCH_HOST_MAX];
    char     url_path[IV_UPDFETCH_PATH_MAX];
    uint16_t url_port;
    char     info_version[IV_UPDFETCH_VER_MAX];

    /* 缓冲 */
    char     hdr[IV_UPDFETCH_HDR_MAX];
    uint8_t  buf[IV_UPDFETCH_BUF]; /* info.txt 体 / 下载搬运块 */

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
