/*
 * IVSBox 升级安装 / 双版本切换 / 回滚与启动包装（libivmodules，M7-S7.2 ＋ S7.3）
 *
 * ============================ 它是什么 ============================
 * 升级链路里"**改盘**"的那一半：`iv_ota` 只回答"这包该不该收"，本模块负责
 *   - **装**（S7.2）：验签 → 空间校核 → 解压暂存 → 准入检查 → 落到版本目录 →
 *     `rename` 原子切 `current` → 写 `pending.json`；
 *   - **回滚与启动包装**（S7.3）：`boot-begin / boot-ok / boot-fail / rollback`，
 *     用 `bootcount` 兜住"新版本起不来"的场景，切回 `previous` 并留下版本事件。
 * 架构依据：§11.2（应用级双版本：`current`/`previous` 符号链接 ＋ pending 状态）、
 * §11.3（**应用级回滚不用 U-Boot 槽位冒充**）、§16.2 场景 8（升级失败回滚）。
 *
 * ============================ 落点（`§0 程序与数据分家` 的具体化） ============================
 * ```text
 * <app_root>  = /opt/ivsbox            ← 程序，rootfs（1.5 GiB 可用，双版本体积须受控）
 *   releases/<ver>/bin,releases/<ver>/scripts   安装后的版本目录
 *   current  -> releases/<ver>        ← 原子切换点（tmp symlink + rename）
 *   previous -> releases/<ver>
 *   bin/ scripts/                     ← 首版/产线部署留下的**裸布局**（兼容，见"首次升级"）
 * <data_root> = /mnt/UDISK/ivsbox      ← 数据，eMMC UDISK
 *   ota/pending.json                  ← 待确认状态（tmp+fsync+rename 原子写）
 *   ota/events.log                    ← 版本事件（append+fsync）
 *   ota/staging/<ver>/                ← 解压暂存（先暂存后安装，安装失败不留半个版本目录）
 * ```
 * **与计划原文的一处口径收敛（登记在案）**：计划写「版本包仓库与暂存留 UDISK
 * （`/mnt/UDISK/ivsbox/releases/<ver>/`）」，本模块把**暂存放 `<data_root>/ota/staging/`**、
 * **版本目录落 rootfs 的 `<app_root>/releases/`**。理由：`S60mount_udisk` 挂载失败会
 * **`mkfs.ext4` 静默清空 UDISK**（计划 §0 已写明该风险），把"正在运行的程序本体"
 * 放在 UDISK 上等于把可运行性押在一个可能被格式化的分区上；而架构 §11.2 的
 * `releases/` 本来就写在 `/opt/ivsbox/` 下。UDISK 只承担仓库/暂存/状态。
 *
 * ============================ 首次升级（裸布局兼容） ============================
 * 现有板子是「`/opt/ivsbox/bin/ivsboxd` ＋ `/opt/ivsbox/scripts/`」的**裸布局**（没有
 * `releases/` 与 `current`）。首次安装会先做 **adopt**：把裸布局的 `bin/`＋`scripts/`
 * 复制成 `releases/<当前版本>/`，再建 `current -> releases/<当前版本>` —— 这样
 * "上一版本"是一个**真实存在、可回滚**的目录，而不是一个说不清的"原来那个"。
 * `<当前版本>` 取自 **`<app_root>/current` 指向的目录名**；没有 `current` 时取
 * 编译期 `IVSBOX_VERSION_*`（即 ivsboxd 自身的版本宏）。
 * **adopt 不删裸布局**：`<app_root>/bin/`（含 `ivsbox-updater`）刻意**不参与版本轮换**，
 * 它是"起不来的新版也得靠它救回来"的引导程序 —— 恢复路径不依赖被测对象。
 *
 * ============================ 升级的落盘顺序（掉电窗口逐个钉住） ============================
 * 1. 验签（**只读**，不改盘）
 * 2. adopt（首次；只增不减）
 * 3. 空间校核（rootfs 与 UDISK 各自都够）
 * 4. 暂存 `<data_root>/ota/staging/<ver>/`（边解边核 SHA-256）
 * 5. 版本目录 `<app_root>/releases/<ver>/`（暂存 → 版本目录的整树复制）
 * 6. 写 `pending.json`（state=pending，**含本次要用的 app_dir**）
 * 7. `previous` 符号链接切到旧版本
 * 8. `current` 符号链接切到新版本（原子）
 * 9. 记 `install` 事件
 * 这样排的理由：**`pending.json` 必须在 `current` 之前落盘** —— 若反序，在
 * "`current` 已切、`pending` 未写"的窗口里掉电，新版本会**无人确认也无人回滚**地跑起来；
 * 而按这个顺序，最坏情况是"`pending` 存在但 `current` 还是旧的"，`boot_begin()` 按
 * `pending` 里记的 `app_dir` 起新版，自检不过就正常回滚 —— 每个窗口都有兜底。
 *
 * ============================ 掉电注入（S7.4 复用） ============================
 * `cfg->fault_at` 命中某个阶段时，函数**立即返回 IV_UPD_EFAULT 且不做任何清理** ——
 * 这正是"升级到一半掉电"的现场：盘上可能停在任何中间态。故障注入脚本与单测靠它
 * 复现"升级中掉电 → 重启后旧版本可运行"，不必真拔电源。阶段名与落盘顺序一一对应：
 *   `verify`  验签之后、**任何落盘之前**
 *   `stage`   暂存目录已建、文件写到一半
 *   `install` 版本目录已就位，`pending` 未写、链接未切
 *   `switch`  `pending` 已写、`previous` 已切，**`current` 未切**
 *   `pending` `current` 已切、`pending` 仍是待确认（"装完就掉电，还没重启"）
 *
 * ============================ 内存 / 并发 / 依赖 ============================
 *   - **零 malloc**：状态结构由调用方持有；路径拼接用有界缓冲（`IV_UPD_PATH_MAX`）；
 *     只有读包用 16 KiB 栈上分块，绝不整包进内存；
 *   - 单写者：只允许一个进程改 `pending.json`/符号链接（`ivsbox-updater`，root，按需运行）；
 *   - 用 `iv_ota`（同层模块）＋ `iv_sha256`（core）；`pending.json` **手写**扁平 JSON，
 *     **不引 json-c**（分层纪律：json-c 只落 `src/modules/config/`）。写入的键值都限定
 *     在 ASCII 无 `"`/`\`/控制字符 的字符集内（写入前逐字符过滤，不合法即拒），
 *     读取端因此可以是一个只认本文件的小扫描器；
 *   - **本模块不打日志**（不用 `iv_log`）：一切诊断走返回码 ＋ `iv_upd_reason_t`
 *     ＋ `events.log`，方便单测与故障注入在任意临时根下跑。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_UPD_OK      → IV_OK          IV_UPD_ESIG    → IV_EAUTH
 *   IV_UPD_ENOSPC  → IV_ENOSPC      IV_UPD_EFAULT  → IV_EFAIL
 *   IV_UPD_ESTAGE  → IV_ECORRUPT    IV_UPD_EELF    → IV_EPROTO
 *   IV_UPD_EDEPS   → IV_ENOENT      IV_UPD_EPERM   → IV_ESTATE
 *   IV_UPD_ESWITCH → IV_EIO         IV_UPD_ESTATE  → IV_ESTATE
 *   IV_UPD_EIO     → IV_EIO
 * 传入指针非法（`pkg_path == NULL`）时返回 IV_EINVAL 且**不写** *why。`cfg == NULL`
 * 视作"全默认"，`why == NULL` 容错。
 */
#ifndef IVSBOX_IV_UPD_H
#define IVSBOX_IV_UPD_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ota.h"
#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 默认落点与常量
 * ------------------------------------------------------------------------- */

#define IV_UPD_APP_ROOT_DEFAULT "/opt/ivsbox"
#define IV_UPD_DATA_ROOT_DEFAULT "/mnt/UDISK/ivsbox"
#define IV_UPD_RELEASES_SUBDIR "releases"
#define IV_UPD_LINK_CURRENT "current"
#define IV_UPD_LINK_PREVIOUS "previous"
#define IV_UPD_OTA_SUBDIR "ota"
#define IV_UPD_PENDING_NAME "pending.json"
#define IV_UPD_EVENTS_NAME "events.log"
#define IV_UPD_STAGING_SUBDIR "staging"
/* "请装这个包"的请求文件（`<data_root>/ota/request`）：ivsboxd 只落盘，
 * 由 `scripts/monitor.sh` 消费后拉起 `ivsbox-updater` 执行安装（S7.2 装配接线）。 */
#define IV_UPD_REQUEST_NAME "request"

/* ivsboxd 自检通过后落下的就绪标记（S7.3：包装脚本拿它当"这次启动成不成"的判据）。
 * 放 `/tmp`（tmpfs）：**每次开机自然清空**，不需要额外的清理逻辑；每次拉起前由
 * 包装脚本再删一次，避免同一开机周期内上一实例的旧标记把新版"确认"掉。 */
#define IV_UPD_READY_MARK_DEFAULT "/tmp/ivsbox-ready"

/* 路径缓冲（最长的形态是 <app_root>/releases/<ver>.tmp）：256 足够。 */
#define IV_UPD_PATH_MAX 256u
/* 版本串缓冲（与 iv_ota 对齐）。 */
#define IV_UPD_VER_MAX IV_OTA_VER_STR_MAX
/* 状态名缓冲（"rolled-back" = 11）。 */
#define IV_UPD_STATE_MAX 24u
/* 原因名缓冲（调用方自填的 reason 串）。 */
#define IV_UPD_REASON_MAX 24u

/* bootcount 上限默认值：连跑 3 次都没确认就回滚（配合 monitor 的 5 s 轮询，
 * 意味着"新版起不来"最多 ~15 s 就会切回旧版，而不是在那儿反复崩）。 */
#define IV_UPD_MAX_BOOT_DEFAULT 3u
/* 空间余量：除新版本体积外再留 32 MiB（覆盖暂存 + 目录项 + ext4 预留）。 */
#define IV_UPD_FREE_MARGIN (32u * 1024u * 1024u)

/* 安装期的权限口径：清单里 `bin/` 前缀的文件＝程序 ⇒ 0755，其余 ⇒ 0644。
 * **包格式（S7.1）里没有 mode 字段**，也不打算加（那会作废已交付的格式）；
 * 所以模式只由路径约定决定，且本模块在 `chmod` 之后**回读 `stat` 确认**：
 * 只读挂载 / 不支持权限位的文件系统会让 chmod 静默失效，那种"以为设了其实没设"
 * 正是本工程反复踩到的一类 bug。回读不符 ⇒ `IV_UPD_EPERM`。 */
#define IV_UPD_PROG_PREFIX "bin/"
#define IV_UPD_MODE_PROG 0755u
#define IV_UPD_MODE_DATA 0644u

/* 本机产品型号/硬件版本的编译期默认值（可用 cfg 覆盖；型号必须与发布侧一致）。 */
#define IV_UPD_DEV_MODEL_DEFAULT "IVSBOX-T113"
#define IV_UPD_DEV_HW_DEFAULT 1u

/* ---- 本机 ELF 身份（编译期定死）：准入检查的对照物 ----
 * 对外暴露的理由：需要"造一个本机架构的 ELF"的场合（单测、产线工具）必须用同一套
 * 判据，否则测试造出来的 ELF 与实际校验的期望会各说各话。
 * 未知架构给 0 —— 任何真实 ELF 都匹配不上 ⇒ 准入检查一律拒（fail-closed），
 * 这比 `#error` 让包含本头文件的整个 TU 编不过要合适：头文件不该有权终止别人的编译。 */
#if defined(__x86_64__)
#  define IV_UPD_ELF_MACHINE 62u  /* EM_X86_64 */
#  define IV_UPD_ELF_CLASS 2u     /* ELFCLASS64 */
#elif defined(__i386__)
#  define IV_UPD_ELF_MACHINE 3u   /* EM_386 */
#  define IV_UPD_ELF_CLASS 1u
#elif defined(__aarch64__)
#  define IV_UPD_ELF_MACHINE 183u /* EM_AARCH64 */
#  define IV_UPD_ELF_CLASS 2u
#elif defined(__arm__)
#  define IV_UPD_ELF_MACHINE 40u  /* EM_ARM */
#  define IV_UPD_ELF_CLASS 1u
#else
#  define IV_UPD_ELF_MACHINE 0u
#  define IV_UPD_ELF_CLASS 0u
#endif

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#  define IV_UPD_ELF_DATA 2u      /* ELFDATA2MSB */
#else
#  define IV_UPD_ELF_DATA 1u      /* ELFDATA2LSB */
#endif

/* NEEDED 搜索目录默认候选（`:` 分隔）。板端是 `/lib` + `/usr/lib`（glibc 家族在
 * /lib、第三方库在 /usr/lib）；后面几个多架构目录只为让**宿主**上的验证也能用默认值
 * 跑，不影响板端语义。调用方给的 `lib_paths` 会与 `$LD_LIBRARY_PATH` 合并搜索。 */
#define IV_UPD_LIB_PATHS_DEFAULT                                              \
    "/lib:/usr/lib:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:"         \
    "/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu"

/* events.log 扫描上限：只看最近 1 MiB（事件是"每次升级一行"量级，正常永远够）。 */
#define IV_UPD_EVENTS_SCAN_MAX (1024u * 1024u)

/* ---------------------------------------------------------------------------
 * 配置（全部可注入，单测/故障注入靠它把根指到临时目录）
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *app_root;  /* NULL → /opt/ivsbox */
    const char *data_root; /* NULL → /mnt/UDISK/ivsbox */
    const char *lib_paths; /* NEEDED 搜索目录，':' 分隔；NULL → 默认候选（另含 $LD_LIBRARY_PATH） */

    const char *dev_model; /* 本机型号（与包内 model 比对）；NULL → 编译期默认 */
    uint32_t    dev_hw;    /* 本机硬件版本；0 → 编译期默认 */

    const uint8_t *pubkey; /* 发布公钥（32B）；NULL → iv_ota_builtin_pubkey()（当前未部署 ⇒ 拒一切包） */

    uint32_t    max_boot;  /* bootcount 上限；0 → 默认 3 */

    const char *fault_at;  /* 掉电注入阶段名（NULL = 关），见文件头"掉电注入" */
} iv_upd_cfg_t;

void iv_upd_cfg_default(iv_upd_cfg_t *cfg);

/* ---------------------------------------------------------------------------
 * 拒绝原因（诊断 / 上报 / 测试断言；数值稳定）
 * ------------------------------------------------------------------------- */

typedef enum {
    IV_UPD_OK = 0,
    IV_UPD_ESIG = 1,     /* 包被拒（签名/型号/版本/格式，细节看 iv_ota_reason_t） */
    IV_UPD_ENOSPC = 2,   /* 空间不足（rootfs 或 UDISK） */
    IV_UPD_EFAULT = 3,   /* 命中掉电注入点（S7.4 用），**有意不清理现场** */
    IV_UPD_ESTAGE = 4,   /* 解压暂存失败（写不出/写不全/摘要不符） */
    IV_UPD_EELF = 5,     /* 准入检查：不是可执行 ELF / 架构不符 / 类型不对 */
    IV_UPD_EDEPS = 6,    /* 准入检查：NEEDED 依赖在目标上找不到 */
    IV_UPD_EPERM = 7,    /* 安装期回读发现可执行位没生效（chmod 被静默吞掉） */
    IV_UPD_ESWITCH = 8,  /* 符号链接原子替换失败 */
    IV_UPD_ESTATE = 9,   /* 状态不允许（无 pending 却 confirm/rollback；previous 缺失） */
    IV_UPD_EIO = 10      /* 本地 IO 失败（目录创建/复制/删除/rename） */
} iv_upd_reason_t;

const char *iv_upd_reason_name(iv_upd_reason_t r);

/* 本地返回码（`iv_ret.h` 的负值，或 IV_OK）→ 名字。诊断/日志/命令行用，
 * 只为"看得懂退出原因"，不参与任何判断。未知值返回 "unknown"。 */
const char *iv_upd_ret_name(int rc);

/* ---------------------------------------------------------------------------
 * 状态查询（updater status ＋ `sys.overview` 的 ota 段共用）
 * ------------------------------------------------------------------------- */

typedef struct {
    int          pending;    /* pending.json 是否存在 */
    int          active;     /* 该 pending 是否仍"待确认"（state == pending） */
    char         state[IV_UPD_STATE_MAX]; /* pending / confirmed / rolled-back；无文件时空串 */
    iv_ota_ver_t version;    /* 待确认/最近一次升级的版本 */
    iv_ota_ver_t previous;   /* 它替换掉的版本 */
    uint32_t     bootcount;  /* 已尝试启动次数 */
    uint32_t     max_boot;
    uint32_t     rollbacks;  /* 历史回滚次数（events.log 里 rollback 行数，窗口见头文件） */
    char         last_event[IV_UPD_STATE_MAX];  /* 最近一条事件的类型；无事件时空串 */
    char         last_reason[IV_UPD_REASON_MAX];/* 最近一条事件的原因 */
    char         current_dir[IV_UPD_VER_MAX];   /* 解析出的 current 目标版本名（不含路径）；无则空串 */
} iv_upd_status_t;

/* 填状态。**永不失败、永不写盘**：读不到的东西按"没有"处理（只读查询，NULL 安全）。 */
void iv_upd_status(const iv_upd_cfg_t *cfg, iv_upd_status_t *out);

/* ---------------------------------------------------------------------------
 * S7.2 安装
 * ------------------------------------------------------------------------- */

/* 装一个升级包。成功返回 IV_OK；失败返回负码并把 *why 置为原因（why 可为 NULL）。
 * **失败不做回滚清理**：留下现场，由下次 `iv_upd_boot_begin()` 按 bootcount 决定。 */
int iv_upd_install(const char *pkg_path, const iv_upd_cfg_t *cfg, iv_upd_reason_t *why);

/* 首次升级前的裸布局收养（幂等）：把 `<app_root>/bin`＋`scripts` 复制成
 * `releases/<当前版本>/` 并建 `current`。已有 `current` 时直接返回 IV_OK。
 * 与升级流程分开暴露，是为了让"先收养、后升级"能在产线上单独做一次并复验。 */
int iv_upd_adopt(const iv_upd_cfg_t *cfg, iv_upd_reason_t *why);

/* ---------------------------------------------------------------------------
 * 装配接线：安装请求文件（本地通道 `ota.request` → monitor.sh 消费）
 * ---------------------------------------------------------------------------
 * 文件是**一行两段**：`<包绝对路径> <restart 0|1>\n`。这样 monitor 侧一句
 * `read -r pkg restart` 就够，不必在 shell 里写 JSON 解析器。
 * 写入侧的把关：路径必须**绝对**、**不含空白字符**、**位于 `<data_root>/ota/` 之下**、
 * 且**是已存在的普通文件**；任一不满足即拒（IV_EINVAL / IV_ENOENT / IV_ERANGE）。
 * 只有本模块与 `scripts/monitor.sh` 知道这个格式 —— 格式改了要同时改两处。 */

int iv_upd_request_write(const iv_upd_cfg_t *cfg, const char *pkg_path, int restart,
                         iv_upd_reason_t *why);

/* 读回请求（不存在返回 IV_ENOENT）。`restart_out` 可为 NULL。 */
int iv_upd_request_read(const iv_upd_cfg_t *cfg, char *pkg_out, size_t cap, int *restart_out);

/* ---------------------------------------------------------------------------
 * S7.3 启动包装与回滚
 * ------------------------------------------------------------------------- */

/* 启动包装的第一步：决定这一次该用哪个程序目录。
 *   - 无 pending（或已 confirmed/rolled-back）⇒ app_dir = current（无 current 时＝裸布局根）；
 *   - 有 pending：bootcount++ 落盘；未超上限 ⇒ app_dir = pending 里记的版本目录；
 *     超过上限（max_boot）或该目录已不存在 ⇒ **自动回滚**并用回滚后的 current，
 *     `*rolled_back` 置 1。
 * app_dir 里写的是**绝对路径**（`<app_root>/releases/<ver>` 或裸布局 `<app_root>`），
 * 供包装脚本直接 exec。`rolled_back`/`why` 可为 NULL。 */
int iv_upd_boot_begin(const iv_upd_cfg_t *cfg, char *app_dir, size_t cap, int *rolled_back,
                      iv_upd_reason_t *why);

/* 自检通过：把 pending 标成 confirmed 并清零 bootcount（幂等；无 pending 也算成功）。 */
int iv_upd_boot_ok(const iv_upd_cfg_t *cfg, iv_upd_reason_t *why);

/* 自检失败/超时：立刻回滚到 previous（等价 `iv_upd_rollback` ＋ reason=selftest-failed）。 */
int iv_upd_boot_fail(const iv_upd_cfg_t *cfg, iv_upd_reason_t *why);

/* 手动/自动回滚：current 与 previous 对调，pending 标记 rolled-back，记事件。
 * 无 pending、pending 已终态、或 previous 版本目录不存在 ⇒ IV_UPD_ESTATE
 * （**不猜**、不乱切：切到一个不存在的目录等于把设备弄成砖）。 */
int iv_upd_rollback(const iv_upd_cfg_t *cfg, const char *reason, iv_upd_reason_t *why);

/* ---------------------------------------------------------------------------
 * 纯函数（单测直接钉，不碰盘）
 * ------------------------------------------------------------------------- */

/* 空间判据：free_bytes 是否够放 pkg_payload_bytes ＋ 余量（余量见 IV_UPD_FREE_MARGIN）。 */
int iv_upd_space_ok(uint64_t free_bytes, uint64_t pkg_payload_bytes);

/* 准入检查（可单独调用/单测）：目标文件是不是**与本机同架构的可执行 ELF**，
 * 以及它的 NEEDED 依赖能否在 lib_paths 里找到。读文件、**不写不改**。
 * 返回 IV_OK；失败返回 `iv_ret.h` 的负码（**IV_EPROTO** = 不是可执行的 ELF /
 * **IV_ENOENT** = 依赖缺失 / **IV_EIO** = 读不了），并把 `*why` 置成
 * IV_UPD_EELF / IV_UPD_EDEPS / IV_UPD_EIO —— 与本模块其余接口一致：**返回码粗判、
 * reason 细判**，绝不靠返回码恰好等于某个枚举值来反推原因。`missing` 填第一个缺的库名
 * （可为 NULL；非 NULL 时保证 NUL 终止）。参数非法返回 IV_EINVAL。 */
int iv_upd_check_elf(const char *path, const char *lib_paths, char *missing, size_t miss_cap,
                     iv_upd_reason_t *why);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_UPD_H */
