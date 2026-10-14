/*
 * IVSBox 升级包格式与验签器（libivmodules，功能开发计划 M7-S7.1）
 *
 * ============================ 它是什么 ============================
 * 定义**升级包的字节格式**，并提供**设备侧唯一入口的验签器**：读一个包，先查结构、
 * 再验签名、再查语义（型号 / 硬件版本 / 软件版本），最后按清单逐文件核对 SHA-256。
 * 本步**只验不改**：解压、安装、符号链接切换、回滚属 S7.2/S7.3 的 `ivsbox-updater`。
 *
 * 架构依据：
 *   - §11.1 通用安全要求：包内必须有 产品型号 / 硬件版本范围 / 软件版本 /
 *     最低可回滚版本 / 文件清单 / SHA-256 / 数字签名；设备内置只读公钥；
 *     **签名校验是强制步骤**；
 *   - §11.2 应用级双版本：`ivsboxd` 只协调（收命令 / 下载 / 上报进度），
 *     验签与安装归 `ivsbox-updater`；
 *   - §16.1/§16.2 场景 9：未签名或型号不匹配的升级包必须被拒绝。
 *
 * ============================ 为什么用 OpenSSL，而不是自写 Ed25519 ============================
 * 架构 §14「加密与签名：mbedTLS/OpenSSL + Ed25519/ECDSA，复用项目现有可信密码库」。
 * **实测**（2026-10-14，交叉工具链 `arm-linux-gnueabi-` 的 Tina sysroot）：
 *   - `/opt/EmbedSky/Tina/arm-buildroot-linux-gnueabi/sysroot/usr/lib/` 有
 *     `libcrypto.so.1.1`（OpenSSL **1.1.1a**，`OPENSSL_VERSION_NUMBER=0x1010101fL`）
 *     与 `libcrypto.a`，`usr/include/openssl/evp.h` 里 `EVP_PKEY_ED25519` 可用
 *     ⇒ **Ed25519 在板端可用，无需 vendor 第三方源码**；
 *   - 宿主（编译 VM）OpenSSL **3.0.2** + `libssl-dev` 已装，`-lcrypto` 可直接链。
 * 因此本项目**不再自写椭圆曲线**：密码学不是本工程该自研的部分（对比 `iv_sha256`
 * 是"纯计算、无密钥"才自写）。此决定登记在案：**升级包校验值＝SHA-256（自写，
 * core 层）+ 发布者认证＝Ed25519（OpenSSL，modules 层）**，两者不做互相替代。
 *
 * ============================ 包格式（全部小端） ============================
 * ```text
 * 偏移 0                     magic[8]   "IVSOTA01"
 * 偏移 8                     u32        manifest_len（清单字节数，<= IV_OTA_MANIFEST_MAX）
 * 偏移 12                    u32        payload_len （负载字节数，= 清单内各文件 size 之和）
 * 偏移 16                    manifest[manifest_len]  行式 ASCII 文本（见下）
 * 16 + manifest_len          payload[payload_len]    各文件内容按清单顺序首尾相接
 * 16 + manifest_len + payload_len   sig[64]          Ed25519，**覆盖它之前的全部字节**
 * ```
 * **总长必须严格等于 `16 + manifest_len + payload_len + 64`**：多一字节（夹带）或
 * 少一字节（截断）都判非法 —— 不做"宽容解析"，包是我们自己造的，模糊容忍只会
 * 让下游安装器与验签器对"包到哪儿结束"产生分歧。
 *
 * ============================ 清单文本（行式 ASCII，`\n` 结尾，末行也要有） ============================
 * ```text
 * pkgfmt=1
 * model=IVSBOX-T113
 * hwmin=1
 * hwmax=2
 * version=1.0.1
 * minrollback=1.0.0
 * files=2
 * file=bin/ivsboxd:1048576:0d5f...（64 位小写 hex sha256）
 * file=etc/ivsbox.json:312:9a01...（同上）
 * ```
 * 规则（**不满足即拒，没有任何"尽力而为"的分支**）：
 *   - 必需键：`pkgfmt` `model` `hwmin` `hwmax` `version` `minrollback` `files`；
 *     可重复键只有 `file`；**未知键判非法**（未来扩展靠 `pkgfmt` 升版，不靠忽略）；
 *   - `files=N` 必须与 `file=` 行数**精确相等**；
 *   - 每个 `file=` 三段以 `:` 分隔，路径里**不允许 `:`**（否则歧义）；
 *   - **路径围栏**：非空、<= `IV_OTA_PATH_MAX-1`、**不以 `/` 开头**（禁止绝对路径）、
 *     不含 `..` 路径分量、不含 `\`、不含控制字符（< 0x20 或 0x7F）、不以 `/` 结尾；
 *   - `size` 十进制、无前导 `+`、<= `UINT32_MAX`，其后合计必须等于 `payload_len`；
 *   - `sha256` 恰好 64 个小写 hex 字符（**大写不接受**，规范化只允许一种写法）；
 *   - 版本串 `M.m.p`：三段十进制、每段 <= 65535、段间单点、无前后缀空白。
 *
 * ============================ 验签顺序（固定，不做"先语义后签名"） ============================
 * 1. 结构：长度下限、magic、两段长度、总长精确匹配          → EBADFMT
 * 2. 清单：解析、必需键、路径围栏、逐项格式                  → EBADFMT / EPATH
 * 3. 负载长度 vs 清单合计                                   → EFILE
 * 4. 公钥：必须由调用方给出且非全零（全零＝未配置）           → ENOKEY
 * 5. 签名：Ed25519 覆盖头+清单+负载                          → ESIG
 * 6. 语义：型号 ∈ 本机型号                                   → EMODEL
 * 7. 语义：本机硬件版本 ∈ [hwmin, hwmax]                     → EHWVER
 * 8. 语义：包版本 > 本机版本 且 本机版本 >= minrollback       → EVER
 * 9. 完整性：**逐个文件**按清单 size/sha256 核对负载切片      → EFILE
 * **第 4~5 步之前不做任何语义判断**：清单内容在验签通过前一律当"攻击者可控数据"，
 * 只做长度与格式的边界检查（那两步必须在验签前完成，否则读不到要验的字节）。
 *
 * ============================ 与"最低可回滚版本"的口径 ============================
 * `minrollback` = **允许升级的版本下限**：本机当前版本 < `minrollback` ⇒ 拒绝
 * （防止从过老版本一步跳到新版本，必须先升到中间版本）。`version` 必须**严格大于**
 * 本机当前版本：同版本重装与降级都拒（降级走 S7.3 的回滚路径，不走升级包）。
 *
 * ============================ 内存 / 并发 / 依赖 ============================
 *   - **零 malloc**：`iv_ota_manifest_t` 由调用方持有（约 4.5 KB）；文件版用
 *     `open` + `fstat` + `mmap(MAP_PRIVATE)` 把包映射进来，**不整包读进堆**；
 *   - **无全局状态**：`EVP_*` 句柄在函数内建、函数内释放；同一进程多线程各调各的
 *     安全（OpenSSL 1.1.1+/3.x 自带线程安全）；
 *   - 本模块用第三方库（OpenSSL），故落 `src/modules/ota/` 进 `libivmodules.a`，
 *     **不进 core**（core 纪律：只允许 libc，见 §15.5 依赖方向）。
 *
 * ============================ 安全边界（说清楚，不夸大） ============================
 *   - **内置公钥当前是占位**：`iv_ota_builtin_pubkey()` 返回 0 表示"未配置量产公钥"，
 *     调用方拿不到密钥 ⇒ 走 ENOKEY **拒绝一切包**（fail-closed，绝不会"无密钥即免签"）。
 *     量产公钥 = 发布侧密钥工程产出，**只把公钥写进 `src/modules/ota/iv_ota_pubkey.c`**，
 *     私钥绝不入库（规则 3 第 6 条）；
 *   - 验签只回答"这包是不是发布者签的"，**不回答"发布者有没有签错内容"**；
 *     安装期的权限 / 架构 / 依赖检查是 S7.2 的事；
 *   - `mmap` 映射只读区：包在本地文件系统上由 `ivsbox-updater` 下载到临时目录后
 *     门禁调用，**并发截断该文件会 SIGBUS**（已登记，S7.2 落临时目录 + 独占命名以回避）。
 */
#ifndef IVSBOX_IV_OTA_H
#define IVSBOX_IV_OTA_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 格式常量
 * ------------------------------------------------------------------------- */

#define IV_OTA_MAGIC          "IVSOTA01"
#define IV_OTA_MAGIC_LEN      8u
#define IV_OTA_HEADER_LEN     16u /* magic(8) + manifest_len(4) + payload_len(4) */
#define IV_OTA_SIG_LEN        64u /* Ed25519 签名定长 */
#define IV_OTA_PUBKEY_LEN     32u
#define IV_OTA_SEED_LEN       32u /* 私钥种子（仅测试端签名用，生产侧不持有） */
#define IV_OTA_SHA256_LEN     32u
#define IV_OTA_SHA256_HEX_LEN 64u

/* 清单文本上限。32 个文件 × 约 180 字节 + 7 个单值键 ≈ 6 KB，取 8 KiB。 */
#define IV_OTA_MANIFEST_MAX 8192u
/* 单包文件数上限（应用级 A/B 的二进制 + 配置 + 脚本，32 足够）。 */
#define IV_OTA_FILES_MAX 32u
/* 清单内相对路径长度（含 NUL）。 */
#define IV_OTA_PATH_MAX 96u
/* 产品型号串长度（含 NUL）。 */
#define IV_OTA_MODEL_MAX 32u
/* 版本串缓冲区下限："65535.65535.65535" = 17 字符 + NUL。 */
#define IV_OTA_VER_STR_MAX 20u

/* ---------------------------------------------------------------------------
 * 版本号（M.m.p，与 iv_version 的语义版本同构，但只取三段数字）
 * ------------------------------------------------------------------------- */

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} iv_ota_ver_t;

/* 解析 "M.m.p"。成功返回 IV_OK，非法返回 IV_EINVAL（不改 *out）。 */
int iv_ota_ver_parse(const char *s, size_t len, iv_ota_ver_t *out);

/* 比较：a<b 返回负数，a==b 返回 0，a>b 返回正数。 */
int iv_ota_ver_cmp(iv_ota_ver_t a, iv_ota_ver_t b);

/* 渲染成 "M.m.p"。cap 不足时写出能装下的部分（永不越界，始终 NUL 终止）。 */
void iv_ota_ver_format(iv_ota_ver_t v, char *out, size_t cap);

/* ---------------------------------------------------------------------------
 * 清单（纯数据，由 iv_ota_manifest_parse() 或验签器填充）
 * ------------------------------------------------------------------------- */

typedef struct {
    char     path[IV_OTA_PATH_MAX];   /* 包内相对路径，已过围栏 */
    uint32_t size;                    /* 字节数 */
    uint8_t  sha256[IV_OTA_SHA256_LEN];
} iv_ota_file_t;

typedef struct {
    uint32_t      pkgfmt;             /* 包格式号，本版恒 1 */
    char          model[IV_OTA_MODEL_MAX];
    uint32_t      hw_min;
    uint32_t      hw_max;
    iv_ota_ver_t  version;            /* 包内软件版本 */
    iv_ota_ver_t  min_rollback;       /* 允许升级的版本下限 */
    uint32_t      n_files;
    iv_ota_file_t files[IV_OTA_FILES_MAX];
} iv_ota_manifest_t;

/* ---------------------------------------------------------------------------
 * 本机身份（验签的"对照物"，由调用方给，本模块不读配置/身份分区）
 * ------------------------------------------------------------------------- */

typedef struct {
    const char  *model;  /* 本机产品型号，如 "IVSBOX-T113"；NULL 视为非法 */
    uint32_t     hw_ver; /* 本机硬件版本 */
    iv_ota_ver_t cur;    /* 本机当前软件版本（来自 ivsbox.h 的 IVSBOX_VERSION_*） */
} iv_ota_self_t;

/* ---------------------------------------------------------------------------
 * 拒绝原因（诊断 / 上报 / 单测断言用；数值稳定，可落库）
 * ------------------------------------------------------------------------- */

typedef enum {
    IV_OTA_OK = 0,
    IV_OTA_EBADFMT = 1, /* 包结构或清单格式非法（magic/长度/未知键/字段格式） */
    IV_OTA_EPATH = 2,   /* 清单内路径越界（绝对路径、.. 分量、控制字符…） */
    IV_OTA_EFILE = 3,   /* 负载与清单不符（长度不等于合计，或某文件 SHA-256 不符） */
    IV_OTA_ENOKEY = 4,  /* 未提供公钥（或全零占位） */
    IV_OTA_ESIG = 5,    /* 无签名、签名长度不符或签名校验失败 */
    IV_OTA_EMODEL = 6,  /* 产品型号不匹配 */
    IV_OTA_EHWVER = 7,  /* 硬件版本不在 [hwmin, hwmax] */
    IV_OTA_EVER = 8,    /* 软件版本非法（不高于本机版本，或本机版本低于 minrollback） */
    IV_OTA_EIOFAIL = 9  /* 打不开 / 映射 / 内存不足等本地错误 */
} iv_ota_reason_t;

/* 原因名（纯 ASCII，日志与测试用，永不返回 NULL）。 */
const char *iv_ota_reason_name(iv_ota_reason_t r);

/* 原因 → 返回码的固定映射（调用方按返回码做粗判、按 reason 做细判）：
 *   OK       → IV_OK          EBADFMT → IV_EPROTO    EPATH   → IV_EPROTO
 *   EFILE    → IV_ECORRUPT    ENOKEY  → IV_EAUTH     ESIG    → IV_EAUTH
 *   EMODEL   → IV_EINVAL      EHWVER  → IV_ERANGE    EVER    → IV_ESTATE
 *   EIOFAIL  → IV_EIO / IV_EINVAL / IV_ENOMEM
 * 传入指针非法时返回 IV_EINVAL 且**不写** *why。`why` 可为 NULL。 */

/* ---------------------------------------------------------------------------
 * 纯函数：清单解析（无 I/O、无密钥，单测直接钉字节）
 * ------------------------------------------------------------------------- */

/* 解析清单文本。成功返回 IV_OK 并填满 *out（n_files 与 files[] 一致）；
 * 失败返回 **IV_EPROTO**（具体是 EBADFMT 还是 EPATH 看 *why）或 IV_EINVAL（参数非法）。
 * `why` 可为 NULL。 */
int iv_ota_manifest_parse(const char *buf, size_t len, iv_ota_manifest_t *out,
                          iv_ota_reason_t *why);

/* ---------------------------------------------------------------------------
 * 验签器
 * ------------------------------------------------------------------------- */

/* 内存版：buf 指向完整包的起始字节（含签名），len 为包总长。
 * 成功返回 IV_OK 并填 *out；失败返回负码（见 iv_ret.h）并把 *why 置为原因。 */
int iv_ota_verify_mem(const void *buf, size_t len, const iv_ota_self_t *self,
                      const uint8_t pubkey[IV_OTA_PUBKEY_LEN], iv_ota_manifest_t *out,
                      iv_ota_reason_t *why);

/* 文件版：内部 open + fstat + mmap(MAP_PRIVATE) 映射整个包，
 * **不做任何写操作、不创建文件**；返回码与 why 语义同 iv_ota_verify_mem()。 */
int iv_ota_verify_file(const char *path, const iv_ota_self_t *self,
                       const uint8_t pubkey[IV_OTA_PUBKEY_LEN], iv_ota_manifest_t *out,
                       iv_ota_reason_t *why);

/* 设备内置只读公钥（§11.1）。返回 IV_OK 表示已写入 out；
 * 返回 IV_ENOTSUP 表示**量产公钥尚未配置**（当前工程状态）——
 * 调用方此时必须按"无公钥"处理（拒绝一切包），不得跳过验签。 */
int iv_ota_builtin_pubkey(uint8_t out[IV_OTA_PUBKEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_OTA_H */
