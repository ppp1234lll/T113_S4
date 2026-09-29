/*
 * IVSBox 配置模块（功能开发计划 M1-S8）
 *
 * 一句话职责：给 `ivsboxd` 当配置管家 —— 从 `<dir>/<名>.json` 读出参数供各模块
 * 按"点分键"取值、能被程序改回去、改坏了不会把服务搞崩、外部改了文件不重启就生效。
 *
 * 八个功能与本文件的落点（对应计划 §S8「本步要实现的八个功能」）：
 *   F1 读配置      iv_config_open()
 *   F2 按名取参数  iv_config_get_str() / _get_int() / _get_bool() / _origin()
 *   F3 改参数保存  iv_config_set_*() → iv_config_save()
 *   F4 缺就补默认  内置默认值表 + iv_config_fill_defaults()，open() 时自动补齐
 *   F5 抗断电写盘  iv_config_save() 内部：tmp → fsync → rename → fsync(目录)
 *   F6 热更新      iv_config_watch_fd() / iv_config_watch_poll()
 *   F7 事务通知    iv_config_subscribe()，全部校验通过才替换内存再逐个通知
 *   F8 密钥分离    iv_config_secure_read()
 *
 * 落点与分层：实现放 `src/modules/config/`、进 `libivmodules.a`；`libivcore` /
 * `libivhal` 里**不得出现 json-c 符号**，所以本头文件**不暴露任何 json-c 类型**，
 * 调用方不需要 include json-c，也看不到 `json_object`。
 *
 * 键的写法：点分路径映射到 JSON 嵌套对象，`net.wan.fail_n` 对应
 * `{"net":{"wan":{"fail_n":3}}}`。段数上限 IV_CFG_DEPTH_MAX，段名与键名受
 * IV_CFG_SEG_MAX 约束；键不存在返回 IV_ENOENT，类型不符返回 IV_EPROTO。
 *
 * 文件结构（架构 §10.2「每份配置包含 schema 版本、内容版本和校验值」）：
 *   {
 *     "_meta": {                                    // 元数据容器
 *       "schema":  1,                               // 缺省按 IV_CFG_SCHEMA_DEFAULT
 *       "version": 7,                               // 内容版本号：每次成功保存 +1，供
 *                                                   // M5 的 S5.4 做"基于哪个版本改的"
 *                                                   // 提交校验（版本不符即拒绝）
 *       "crc":     4660                             // 全部业务键按 key 排序后的
 *                                                   // "key=type:value\n" 指纹串的
 *                                                   // CRC-16/MODBUS，用于发现改坏/截断
 *     },
 *     "net":      { "wan": { "mode": "wireless-first" } },   // 业务键：顶层嵌套
 *     "platform": { "proto": { "heartbeat_s": 30 } }
 *   }
 * 业务键一律写在**顶层**（点分键去掉点就是它的嵌套层级，不再套一层 `data` 容器）；
 * 只有 `_` 前缀留给元数据（`_meta`），`set_*()` 也拒绝以 `_` 开头的键，两者不会撞名。
 *
 * `crc` 的判定口径（**刻意宽松**，理由见下）：
 *   - 文件里没有 `crc`、或文件根本不存在 ⇒ 正常加载，不算损坏；
 *   - 有 `crc` 但与数据区不符 ⇒ 记 `crc_mismatch` 并打一条告警，**仍然加载**。
 *   为什么不用"校验不过就拒绝"：F4/F6 的可用性口径是"只有值非法才拒绝"，而手工
 *   编辑（正是 F6 的典型场景）必然让 `crc` 过期；此时若拒绝加载，等于把"改配置"
 *   变成"停服"。所以 `crc` 只用来发现损坏与提醒，不作拒绝依据；下次 `save()` 会
 *   把 `crc` 与 `version` 一并刷新。
 *
 * 校验分四类（计划 §S8「实现时必读」旁的校验口径），本步落地情况：
 *   1. 类型：内置默认值表钉死每个已知键的类型，取值/写入两侧都查；另外**占着
 *      已知键父路径的标量键**（文件里把子树写成了标量，如 `"net": 5`）按类型
 *      冲突拒绝 —— 否则整棵 `net.*` 默认子树会被它挡在门外，热更新还会因为
 *      "没发现非法值"而整份接受，静默清空运行中的配置；
 *   2. 范围：表里带 min/max 的整数键做闭区间检查；
 *   3. 引用：本步**无适用项** —— 唯一要引用的 `secure/` 密钥走 F8 独立接口，
 *      不通过普通配置引用，故不虚构引用检查（等 M2 平台协议成形再补）；
 *   4. 跨字段：见 `iv_config_validate()` 的两条不变量（探活超时 < 探活间隔；
 *      双 WAN 主备网卡不得同名）。
 *   任一类失败 ⇒ `iv_config_save()` 返回 IV_EINVAL，**一个字段都不落盘**，
 *   并把出错的键名与原因写进调用方给的 `detail` 缓冲。
 *
 * 未知键：文件里有、默认值表里没有的键**原样保留**（向前兼容：新版本固件写的
 * 字段不该被旧版本抹掉），类型按 JSON 实际类型推断，不参与范围/跨字段校验。
 * **例外**见上面第 1 类：占着已知键父路径的标量键不属"新字段"，一律拒绝。
 *
 * 线程与调用约定：**非线程安全**。所有接口（含只读的 get）都只在**reactor 线程**
 * 内调用，与 `iv_reactor` 同一条纪律；跨线程共享请由调用方自己加锁。`watch_fd()`
 * 返回的 inotify fd 由本模块持有并负责关闭（随 close 一起），调用方只注册、不关闭。
 *
 * 磁盘落点：运行期目录默认由调用方给出（S10 装配时用 `/mnt/UDISK/ivsbox/config/`，
 * 见计划 §0）。`open()` 会尝试 `mkdir` 该目录（已存在不算错），因为 F4 要求
 * "文件不存在也照常启动"，而补默认后要有个地方落盘。
 */
#ifndef IVSBOX_IV_CONFIG_H
#define IVSBOX_IV_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 规模上限：全部静态容量，本模块自身**不做任何 malloc**（句柄走静态池）。
 * 解析用的 json-c 内部会分配，那部分是库行为，不受本约束。
 * ------------------------------------------------------------------------- */
#define IV_CFG_NAME_MAX   32u  /* 配置名（"platform"）最大长度，含结尾 NUL */
#define IV_CFG_SEG_MAX    32u  /* 单个键段最大长度，含结尾 NUL */
#define IV_CFG_DEPTH_MAX  4u   /* 点分键最大段数（net.wan.fail_n = 3 段） */
#define IV_CFG_KEY_MAX    96u  /* 扁平化后的完整点分键上限，含结尾 NUL */
#define IV_CFG_STR_MAX    160u /* 字符串值上限，含结尾 NUL */
#define IV_CFG_ITEMS_MAX  32u  /* 单份配置的最大键数 */
#define IV_CFG_PATH_MAX   256u /* 拼出的文件路径上限，含结尾 NUL */
#define IV_CFG_SUBS_MAX   8u   /* 订阅者上限 */
#define IV_CFG_HANDLES    4u   /* 同时打开的配置份数上限（静态池大小） */
#define IV_CFG_DETAIL_MAX 128u /* save() 出错说明的建议缓冲长度 */

/* schema 版本：文件里没写 schema 时按本值处理 */
#define IV_CFG_SCHEMA_DEFAULT 1u

/* ---------------------------------------------------------------------------
 * 类型与来源
 * ------------------------------------------------------------------------- */

typedef enum {
    IV_CFG_T_STR = 1,  /* 字符串 */
    IV_CFG_T_INT = 2,  /* 整数（64 位有符号） */
    IV_CFG_T_BOOL = 3  /* 布尔 */
} iv_cfg_type_t;

/* 这个值是从文件读来的、还是内置默认补的（F4 的"记录值来自文件还是默认"） */
typedef enum {
    IV_CFG_FROM_FILE = 0,
    IV_CFG_FROM_DEFAULT = 1
} iv_cfg_origin_t;

/* 诊断快照（只读） */
typedef struct {
    uint32_t schema;        /* 文件里的 schema；缺失或未加载为 IV_CFG_SCHEMA_DEFAULT */
    uint64_t version;       /* 当前内容版本号 */
    uint32_t items;         /* 内存里的键数 */
    uint32_t from_file;     /* 其中来自文件的键数 */
    uint32_t from_default;  /* 其中内置默认补出来的键数 */
    uint32_t invalid;       /* 加载时因"值非法"被拒绝并回退默认的键数（0 = 文件干净） */
    int loaded;             /* 1 = 本地文件解析成功并采用；0 = 用默认值启动 */
    int crc_mismatch;       /* 1 = 文件有 crc 但与数据区不符（已告警，仍加载） */
    int dirty;              /* 1 = 内存有未保存的改动 */
} iv_cfg_stat_t;

typedef struct iv_config iv_config_t;

/* ---------------------------------------------------------------------------
 * F1 读配置
 * ------------------------------------------------------------------------- */

/*
 * 打开一份配置。`dir` 为配置目录（可为 NULL ⇒ 用 IV_CFG_DIR_DEFAULT），
 * `name` 为配置名（如 "platform"），实际文件为 `<dir>/<name>.json`。
 *
 * 行为：目录按需创建（mkdir 0755，已存在不算错）；文件不存在、不可读、是空文件、
 * JSON 解析失败、缺字段、类型不符 ⇒ 都用**内置默认值**补齐并照常返回句柄（F4）。
 * 只有"键存在但值非法"（越界、跨字段冲突）才在 `iv_config_save()` 侧拒绝。
 * 解析失败时 `iv_cfg_stat_t.loaded` 为 0，并打一条告警，不阻断启动。
 *
 * 失败：参数非法 / 句柄池满 / 路径超长 / 目录建不出来 ⇒ 返回 NULL。
 */
iv_config_t *iv_config_open(const char *dir, const char *name);

/* 关闭并释放句柄：关掉 inotify fd、清订阅者、把寄存器归零。NULL 安全。 */
void iv_config_close(iv_config_t *c);

/* 运行期配置目录默认值（S10 装配前留一个明确落点；板端口径见计划 §0） */
#ifndef IV_CFG_DIR_DEFAULT
#define IV_CFG_DIR_DEFAULT "/mnt/UDISK/ivsbox/config"
#endif

/* ---------------------------------------------------------------------------
 * F2 按名取参数
 * 三类取值都按"点分键"查表；取到的是**值本身**，此后与文件、与 JSON 无关。
 * ------------------------------------------------------------------------- */

/* 键不存在 ⇒ IV_ENOENT；键在但不是字符串 ⇒ IV_EPROTO。*out 指向模块内部存储，
 * 生命周期到 close() 或下一次 save()/热重载为止，调用方不得修改或释放。 */
int iv_config_get_str(const iv_config_t *c, const char *key, const char **out);

/* 键不存在 ⇒ IV_ENOENT；不是整数 ⇒ IV_EPROTO。布尔键不隐式转整数。 */
int iv_config_get_int(const iv_config_t *c, const char *key, int64_t *out);

/* 键不存在 ⇒ IV_ENOENT；不是布尔 ⇒ IV_EPROTO。*out 取 0/1。 */
int iv_config_get_bool(const iv_config_t *c, const char *key, int *out);

/* 这个键的值是文件给的还是默认补的（F4 要能区分，便于 Web 标"未配置"） */
int iv_config_origin(const iv_config_t *c, const char *key, iv_cfg_origin_t *out);

/* ---------------------------------------------------------------------------
 * F3 改参数
 * 只改内存，不碰磁盘；落盘由 save() 一次性完成。未知键也允许写（向前兼容），
 * 但按"文件来的"记来源。类型由调用的函数决定，不做隐式转换。
 * ------------------------------------------------------------------------- */
int iv_config_set_str(iv_config_t *c, const char *key, const char *val);
int iv_config_set_int(iv_config_t *c, const char *key, int64_t val);
int iv_config_set_bool(iv_config_t *c, const char *key, int val);

/* ---------------------------------------------------------------------------
 * F4 缺就补默认
 * open() 已自动补过一轮；本函数用于"补完顺手写盘"或明确统计补齐项。
 * `*filled` 返回本次补进来的键数（可为 NULL），**与随后写盘是否成功无关**：
 * 补齐先落在内存（补进去的是内置默认值，本身就是合法值），紧接着调用 save()；
 * 若 save() 因校验或 IO 失败返回错误码，内存中已补齐的键**保留**，下次 save()
 * 会一并落盘。因此调用方看到非 IV_OK 时，只表示"还没落盘"，不表示"没补齐"。
 * ------------------------------------------------------------------------- */
int iv_config_fill_defaults(iv_config_t *c, int *filled, char *detail, size_t detail_cap);

/* ---------------------------------------------------------------------------
 * F3 + F5 保存
 * 步骤：整体校验（类型/范围/跨字段）→ 写 `<path>.tmp` → fsync 文件 →
 * rename 覆盖 → fsync 目录（保证改名本身落盘，见计划 §S8 坑 3）。
 * 校验失败 ⇒ IV_EINVAL，**一个字段都不落盘**，`detail` 里给出错的键名与原因；
 * 写盘失败 ⇒ IV_EIO / IV_ENOSPC，同样是"旧文件保持原样"。
 * 成功 ⇒ 内容版本号 +1，并刷新 crc；磁盘上要么是完整旧配置、要么是完整新配置（F5）。
 * `detail` 可为 NULL（cap 为 0）。保存成功后会把自身触发的 inotify 事件抽干，
 * 避免 F6 把自己的保存当成"外部改动"再重载一次。
 * ------------------------------------------------------------------------- */
int iv_config_save(iv_config_t *c, char *detail, size_t detail_cap);

/* ---------------------------------------------------------------------------
 * 内容版本（给 M5 的 S5.4 用）
 * ------------------------------------------------------------------------- */
uint64_t iv_config_version(const iv_config_t *c);

/* 诊断快照；参数为 NULL 返回 IV_EINVAL */
int iv_config_stat(const iv_config_t *c, iv_cfg_stat_t *out);

/* ---------------------------------------------------------------------------
 * F6 热更新
 * 监听的是**目录**而不是文件：原子写走 rename（新 inode 换掉旧 inode），
 * 对旧文件的 watch 会在第一次保存后立刻失效（见计划 §S8 坑 2）。
 * 事件掩码 = IN_CLOSE_WRITE | IN_MOVED_TO，再按文件名 `<name>.json` 过滤。
 * ------------------------------------------------------------------------- */

/* 返回该配置的 inotify fd（IN_NONBLOCK|IN_CLOEXEC），调用方把它注册进 Reactor。
 * fd 归本模块所有，调用方**只注册、不关闭**（close() 会一并关掉）。
 * **首次调用会创建 inotify 实例并给配置目录加 watch**（因此是非 const 参数）：
 * 建 watch 失败返回负的 IV_* 码（IV_EFAIL 建实例失败 / IV_ENOENT 目录不在 /
 * IV_ESTATE 已另行关闭）。重复调用返回同一个 fd。 */
int iv_config_watch_fd(iv_config_t *c);

/*
 * fd 可读时调用（在 reactor 回调里）。读干事件、按文件名过滤，命中则重载。
 * 重载是**事务式**的（F7）：先把整份文件解析+校验到暂存区，全部通过才整体替换
 * 内存并通知订阅者；任一项不通过 ⇒ **保留旧配置继续跑**，通知一个订阅者都不发，
 * 打告警（F6 的"坏配置不停服"）。
 *
 * **启动与运行期对"非法值"的处置刻意不同**（两条口径来自 F4 与 F6，别混）：
 *   - `iv_config_open()`（启动，此时没有"旧配置"可留）⇒ **逐键**回退默认、照常启动，
 *     非法键数记在 `iv_cfg_stat_t.invalid`；"缺字段"不算非法（那是 F4 的正常补齐）。
 *   - `iv_config_watch_poll()`（运行期，手里有一份好的旧配置）⇒ 只要出现**任何一个**
 *     非法值（类型/范围/枚举/跨字段）或解析失败，就**整份拒绝**、旧值一字不动。
 *     理由：半新半旧比全旧更危险，宁可让运行中的服务继续用已知good的配置。
 * `*reloads` 返回本次成功重载的次数（可为 NULL）。
 * 返回 IV_OK（即使没有命中事件也算成功）、IV_EAGAIN（无事件且 fd 未就绪）、
 * IV_ESTATE（未开启 watch）。
 */
int iv_config_watch_poll(iv_config_t *c, uint32_t *reloads);

/* ---------------------------------------------------------------------------
 * F7 事务通知
 * 配置生效后**在 reactor 线程内**逐个回调。契约：要么所有订阅者都收到、要么
 * 一个都不收到 —— 校验不过时一个都不调用，所以回调里看到的一定是新的合法配置。
 * 回调内**禁止**再调用本模块的写入接口（set_ / save / open）—— 会改到正在
 * 分发的状态；只读的 get_ 系列可以。
 * ------------------------------------------------------------------------- */
typedef void (*iv_cfg_notify_fn)(iv_config_t *c, const char *name, void *user);

int iv_config_subscribe(iv_config_t *c, iv_cfg_notify_fn fn, void *user);

/* ---------------------------------------------------------------------------
 * F8 密钥分离
 * 口令 / 令牌不进普通配置，放独立 `secure/` 目录、权限 0600；普通配置里只写
 * "去哪儿取"。本函数读一条密钥到调用方缓冲。
 *
 * 拒绝条件（任一命中即失败，且**日志里不得出现密钥内容**）：
 *   IV_EINVAL  参数非法（secure_dir/name/out 为空、cap 为 0）
 *   IV_ERANGE  cap 放不下（含结尾 NUL）
 *   IV_ENOENT  文件不存在（报明确错误，不静默给空串）
 *   IV_EAUTH   权限不是 0600（组/其他位任何一位打开就拒），或路径是符号链接
 *   IV_EIO     打开/读取失败；IV_ECORRUPT 文件为空
 * 读取用 O_RDONLY|O_NOFOLLOW，开完再 fstat 复核"是普通文件且 0600"，
 * 防 TOCTOU；读出后去掉行尾 CR/LF。
 * ------------------------------------------------------------------------- */
int iv_config_secure_read(const char *secure_dir, const char *name, char *out, size_t cap);

/* ---------------------------------------------------------------------------
 * 内置默认值表的只读查询（给 Web / 诊断用：显示"这个键的出厂默认是什么"）
 * 未知键返回 IV_ENOENT。`out_type` 可为 NULL。
 * ------------------------------------------------------------------------- */
int iv_config_default_get(const char *name, const char *key, iv_cfg_type_t *out_type,
                          char *out_text, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_CONFIG_H */
