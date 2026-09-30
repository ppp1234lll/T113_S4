/*
 * IVSBox 配置模块（功能开发计划 M1-S8）
 *
 * 一句话职责：给 `ivsboxd` 当配置管家 —— 从 `<dir>/<名>.json` 读出参数供各模块
 * 按"点分键"取值、能被程序改回去、改坏了不会把服务搞崩、外部改了文件不重启就生效。
 *
 * 八个功能与本文件的落点（对应计划 §S8「本步要实现的八个功能」）：
 *   F1 读配置      iv_config_open()
 *   F2 按名取参数  iv_config_get_str() / _get_int() / _get_bool() / _get_dbl() / _origin()
 *   F3 改参数保存  iv_config_set_*() → iv_config_save()
 *   F4 缺就补默认  内置默认值表 + iv_config_fill_defaults()，open() 时自动补齐
 *   F5 抗断电写盘  iv_config_save() 内部：tmp → fsync → rename → fsync(目录)
 *   F6 热更新      iv_config_watch_fd() / iv_config_watch_poll()
 *   F7 事务通知    iv_config_subscribe()，按分类提交后只通知值实际变化的分类
 *
 * 落点与分层：实现放 `src/modules/config/`、进 `libivmodules.a`；`libivcore` /
 * `libivhal` 里**不得出现 json-c 符号**，所以本头文件**不暴露任何 json-c 类型**，
 * 调用方不需要 include json-c，也看不到 `json_object`。
 *
 * **单文件口径（2026-09-30 §S8.1 起）**：全机只有一份配置文件 `ivsbox.json`，
 * 顶层是**八个一级分类**（架构 §10.2）：`sys` / `net` / `probe` / `camera` /
 * `switch` / `elec` / `netthr` / `sensor`。`name` 参数固定传 `IV_CFG_NAME`；
 * **同一份配置只允许一个句柄**（第二次 `open` 同名返回 NULL）—— 单文件下多个
 * 句柄会各自持有一份内存副本，互相覆盖，是"改了一个模块、另一个模块看不见"
 * 这类最难查的故障源头。各模块改为**订阅通知**（F7）而不是各开一份。
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
 *     "sys":    { "transport": { "mode": 4 } },     // 业务键：顶层嵌套（一级分类）
 *     "camera": { "search": "off", "ch": [ ... ] }, // 数组型分类见下
 *     "elec":   { "volt_high": 60.0 }               // double 型分类
 *   }
 * 业务键一律写在**顶层**（点分键去掉点就是它的嵌套层级，不再套一层 `data` 容器）；
 * 只有 `_` 前缀留给元数据（`_meta`），`set_*()` 也拒绝以 `_` 开头的键，两者不会撞名。
 *
 * **JSON 数组**（§S8.1 新增能力）：`camera.ch` 是数组，元素字段的条目键形如
 * `camera.ch.0.vendor`（元素下标进点分键）。数组长度独立记录，默认值只补到文件中
 * 实际存在的元素；因此 0 / 1 / 6 路都能准确表达。元素个数上限 IV_CFG_ARR_MAX，
 * 下标越界或中间空洞判非法。空数组合法。
 *
 * **double**（§S8.1 新增能力）：`IV_CFG_T_DBL` 用于 `elec.*` / `sensor.*` 的
 * 物理量阈值（电压 / 电流 / 功率 / 倾斜度 / 温度 / 湿度）。范围校验用 double
 * （dmin/dmax），与整数键的 imin/imax 分开记。
 *
 * **中文注释**（§S8.1 新增能力）：`json-c` 的输出接口**产生不了注释**，所以
 * `save()` 渲染时由本模块在**每个一级分类**前自行插入一行 `// 中文说明`（文字与
 * 默认值表的 `desc` 字段同源）。`json-c` 非严格模式下能**解析**行注释与块注释
 * （实测 0.13.1 与 0.15 均可，`#` 不支持），因此"渲染 → 再解析回来"
 * 是等值的（有往返单测）。**注意**：任何一次 `save()` 都会按当前默认值表重新渲染
 * 注释，手工加的注释会被覆盖 —— 这是"注释与默认值表同源"的代价，换来的是注释
 * 永不过期。
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
 *   3. 引用：本步**无适用项** —— 密钥分离（`secure/` 口令/令牌）留待后续步骤
 *      单独设计，本轮配置模块不含该接口，故不虚构引用检查；
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
 *
 * 2026-09-30 §S8.1 容量抬升：`IV_CFG_ITEMS_MAX` 由 32 抬到 128。依据＝架构 §10.2
 * 「键数上限按 128 设计」：八个一级分类下，数组按元素计约 43 项、按字段平铺计约 85 项。
 * 抬升的连带代价是**栈**：`cfg_snapshot_t`、`render_json()` / `items_fingerprint()`
 * 里的 `cfg_item_t sorted[ITEMS_MAX]`、`watch_poll()` 的快照，各自从约 9 KB 涨到约
 * 34 KB。reactor 线程栈默认 8 MB，够用；但**不要**在递归或深调用链里再复制这些数组。
 * ------------------------------------------------------------------------- */
#define IV_CFG_NAME_MAX   32u  /* 配置名（"ivsbox"）最大长度，含结尾 NUL */
#define IV_CFG_SEG_MAX    32u  /* 单个键段最大长度，含结尾 NUL */
#define IV_CFG_DEPTH_MAX  4u   /* 点分键最大段数（camera.ch.0.vendor = 4 段） */
#define IV_CFG_KEY_MAX    96u  /* 扁平化后的完整点分键上限，含结尾 NUL */
#define IV_CFG_STR_MAX    160u /* 字符串值上限，含结尾 NUL */
#define IV_CFG_ITEMS_MAX  128u /* 单份配置的最大键数（§S8.1：32 → 128） */
#define IV_CFG_ARR_MAX    6u   /* 数组元素个数上限（camera.ch 最多 6 路） */
#define IV_CFG_PATH_MAX   256u /* 拼出的文件路径上限，含结尾 NUL */
#define IV_CFG_SUBS_MAX   8u   /* 订阅者上限 */
#define IV_CFG_HANDLES    4u   /* 同时打开的配置份数上限（静态池大小） */
#define IV_CFG_DETAIL_MAX 128u /* save() 出错说明的建议缓冲长度 */
#define IV_CFG_CLASS_N    8u   /* 一级分类个数（架构 §10.2） */

/* 唯一的配置名（§S8.1 单文件口径）。调用方传这个。 */
#define IV_CFG_NAME "ivsbox"

/* schema 版本：文件里没写 schema 时按本值处理 */
#define IV_CFG_SCHEMA_DEFAULT 1u

/* ---------------------------------------------------------------------------
 * 类型与来源
 * ------------------------------------------------------------------------- */

typedef enum {
    IV_CFG_T_STR = 1,  /* 字符串 */
    IV_CFG_T_INT = 2,  /* 整数（64 位有符号） */
    IV_CFG_T_BOOL = 3, /* 布尔 */
    IV_CFG_T_DBL = 4   /* double（§S8.1 新增；elec.* / sensor.* 的物理量阈值） */
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

/*
 * 一级分类的枚举与下标（架构 §10.2 的八个分类，顺序即渲染顺序）。
 * 事务粒度＝按一级分类子树（§S8.1），故分类是**运行期可查询**的第一类概念。
 */
typedef enum {
    IV_CFG_CLS_SYS = 0,    /* 基础配置   sys.*     */
    IV_CFG_CLS_NET = 1,    /* 网络配置   net.*     */
    IV_CFG_CLS_PROBE = 2,  /* 检测配置   probe.*   */
    IV_CFG_CLS_CAMERA = 3, /* 摄像机配置 camera.*  */
    IV_CFG_CLS_SWITCH = 4, /* 交换机配置 switch.*  */
    IV_CFG_CLS_ELEC = 5,   /* 电阈值     elec.*    */
    IV_CFG_CLS_NETTHR = 6, /* 网络阈值   netthr.*  */
    IV_CFG_CLS_SENSOR = 7  /* 传感器阈值 sensor.*  */
} iv_cfg_class_t;

/* 取键所属的一级分类；键不在任何已知分类下返回 -1（未知顶层键，原样保留）。 */
int iv_config_class_of(const char *key);

typedef struct iv_config iv_config_t;

/* ---------------------------------------------------------------------------
 * F1 读配置
 * ------------------------------------------------------------------------- */

/*
 * 打开一份配置。`dir` 为配置目录（可为 NULL ⇒ 用 IV_CFG_DIR_DEFAULT），
 * `name` 为配置名（**单文件口径下固定传 `IV_CFG_NAME`（="ivsbox"）**），
 * 实际文件为 `<dir>/<name>.json`。
 *
 * **单句柄约束（§S8.1）**：同一份 `<dir>/<name>` 只允许一个活跃句柄，第二次 `open`
 * 返回 NULL 并告警。理由：单文件下两个句柄各持一份内存副本，A 改了 B 看不见，
 * B 一 `save()` 又把 A 的改动覆盖掉 —— 这是最难定位的一类故障。各模块应当
 * **订阅**同一句柄的通知（`iv_config_subscribe`），而不是各开一份。
 *
 * 行为：目录按需创建（mkdir 0755，已存在不算错）；文件不存在、不可读、是空文件、
 * JSON 解析失败、缺字段、类型不符 ⇒ 都用**内置默认值**补齐并照常返回句柄（F4）。
 * 只有"键存在但值非法"（越界、跨字段冲突）才在 `iv_config_save()` 侧拒绝。
 * 解析失败时 `iv_cfg_stat_t.loaded` 为 0，并打一条告警，不阻断启动。
 *
 * 失败：参数非法 / 句柄池满 / **同名已打开** / 路径超长 / 目录建不出来 ⇒ 返回 NULL。
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

/* 键不存在 ⇒ IV_ENOENT；不是 double ⇒ IV_EPROTO。**整数键不隐式转 double**
 * （§S8.1：类型必须严格匹配，避免"整数阈值被当成小数写回"的两义）。
 * 反过来 `get_int` 也不接受 double 键 —— 要取整数值就用整型键。 */
int iv_config_get_dbl(const iv_config_t *c, const char *key, double *out);

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
int iv_config_set_dbl(iv_config_t *c, const char *key, double val);

/*
 * 数组元素个数（§S8.1）。`arr_key` 是数组本身的点分键（如 `camera.ch`）。
 * `camera.ch` 返回解析时记录的真实 JSON 数组长度；其他数组按内存里以
 * `arr_key.` 开头、且这一段是**纯十进制下标**的条目之最大下标 + 1 计算。
 * 数组元素连续；删掉中间元素会留空洞，空洞按下标存在性判定。
 * `*n` 返回 0..IV_CFG_ARR_MAX；数组不存在（一个元素都没写）返回 IV_OK 且 *n = 0
 * —— 空数组与"根本没有这个数组"在配置语义上等价，都表示"未配置任何一路"。
 * `arr_key` 不是合法键 ⇒ IV_EINVAL。
 */
int iv_config_array_len(const iv_config_t *c, const char *arr_key, unsigned *n);

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
 * rename 前写盘失败 ⇒ IV_EIO / IV_ENOSPC，旧文件保持原样；rename 已成功但目录
 * fsync 失败 ⇒ IV_EIO，内存保持 dirty，表示文件已替换但掉电持久性未确认。
 * 成功 ⇒ 内容版本号 +1，并刷新 crc；磁盘上要么是完整旧配置、要么是完整新配置（F5）。
 * `detail` 可为 NULL（cap 为 0）。保存后不盲目抽干 inotify 队列；自己的事件会在
 * F6 比较新旧分类时识别为“值未变化”，避免吞掉同时到达的外部更新。
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
 * 重载是**事务式**的（F7），且**粒度＝按一级分类子树**（§S8.1）：
 *   先把整份文件解析+校验到暂存区，然后**逐个一级分类**判定 ——
 *     - 该分类内所有键都合法（类型/范围/枚举/跨字段）⇒ 采用该分类的新值；
 *     - 该分类内出现任何非法值 ⇒ **只拒绝这一个分类**，该分类回退旧值，
 *       其余分类的新改动照常采用（架构 §10.2「某个分类里出现非法值，只拒绝该
 *       分类，其余分类的新改动照常生效」）。
 *   整个文件解析失败（语法坏、截断、顶层不是对象）⇒ **整份拒绝**、旧值一字不动，
 *   因为解析失败时连"哪些键属于哪个分类"都拿不到。
 *   通知（F7）**只发给受影响分类的订阅者**：至少有一个分类的值实际变化 ⇒ 通知；
 *   全部分类都被拒 ⇒ 一个订阅者都不发。
 *   任一路径被拒都打告警，并把被拒的分类名写进日志（F6 的"坏配置不停服"）。
 *
 * **启动与运行期对"非法值"的处置刻意不同**（两条口径来自 F4 与 F6，别混）：
 *   - `iv_config_open()`（启动，此时没有"旧配置"可留）⇒ **逐键**回退默认、照常启动，
 *     非法键数记在 `iv_cfg_stat_t.invalid`；"缺字段"不算非法（那是 F4 的正常补齐）。
 *   - `iv_config_watch_poll()`（运行期，手里有一份好的旧配置）⇒ 见上，按分类子树
 *     分别拒绝；`*reloads` 记本次**值实际发生变化的分类数**（0 = 无有效变化）。
 * `*reloads` 返回本次值实际发生变化的分类数（可为 NULL）。
 * 返回 IV_OK（即使没有命中事件也算成功）、IV_EAGAIN（无事件且 fd 未就绪）、
 * IV_ESTATE（未开启 watch）。
 */
int iv_config_watch_poll(iv_config_t *c, uint32_t *reloads);

/* ---------------------------------------------------------------------------
 * F7 事务通知
 * 配置生效后**在 reactor 线程内**逐个回调。订阅时登记关心的一级分类位图，只有
 * `class_mask` 与本次 `changed_mask` 相交的订阅者才收到通知；回调拿到的
 * `changed_mask` 是整次事务实际变化的分类集合。未知顶层扩展键不属于业务分类，
 * 不触发分类订阅。
 * `name` 参数传的是**配置名**（单文件下恒为 "ivsbox"）。
 * 回调内**禁止**再调用本模块的写入接口（set_ / save / open）—— 会改到正在
 * 分发的状态；只读的 get_ 系列可以。
 * ------------------------------------------------------------------------- */
#define IV_CFG_CLASS_MASK(cls) (1u << (unsigned)(cls))
#define IV_CFG_CLASS_MASK_ALL  ((1u << IV_CFG_CLASS_N) - 1u)

typedef void (*iv_cfg_notify_fn)(iv_config_t *c, const char *name, uint32_t changed_mask,
                                 void *user);

int iv_config_subscribe(iv_config_t *c, uint32_t class_mask, iv_cfg_notify_fn fn, void *user);

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
