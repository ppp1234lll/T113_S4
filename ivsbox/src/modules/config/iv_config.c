/*
 * IVSBox 配置模块实现（功能开发计划 M1-S8）
 *
 * 为什么 json-c 只出现在这个文件：
 *   计划 §S8「分层」要求"解析与存储落 `src/modules/config/`；`libivcore` /
 *   `libivhal` 里不得出现 json-c 符号"。本文件属模块层（libivmodules），是
 *   **唯一** include <json-c/json.h> 的地方；对外头文件不暴露任何 json-c 类型。
 *   用到的 json-c API 已按板端 sysroot 头（0.13.1）逐条核对声明，不使用
 *   0.15 才新增的接口（计划 §S8「实现时必读的三个坑」第 1 条）。
 *
 * 为什么不用 malloc 存自己的状态：
 *   句柄走静态池（IV_CFG_HANDLES 个槽）、条目走定长数组。配置份数与键数都是
 *   编译期数得清的（架构 §10.2 共 7 份，本步只落 2 份），动态分配只多出
 *   "分配失败路径"和碎片。json-c 解析时库内部会分配，那部分不受本约束。
 *
 * 抗断电写盘（F5）：写 `<path>.tmp` → fsync 文件 → rename → fsync 目录。
 *   只 fsync 文件不够：rename 这个目录项变更本身也要落盘，否则断电后可能仍是
 *   旧文件（计划 §S8 坑 3）。
 *
 * 单线程契约（与 iv_reactor 同一条纪律）：全部接口只在 reactor 线程内调用，
 * 因此下面两块解析/渲染缓冲直接做成文件级 static，不做重入保护 —— 同头文件的
 * "线程与调用约定"。
 */
#include "ivsbox/iv_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <json-c/json.h> /* 仅模块层允许依赖 json-c（core/hal 不允许） */

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

#define CFG_MOD "cfg"

/* 元数据键名：业务键不得以 '_' 开头（该前缀保留给元数据） */
#define CFG_META_KEY "_meta"

/* 读入文件大小上限：配置是小文件，超限即异常（防误读大文件吃内存） */
#define CFG_FILE_MAX (64u * 1024u)

/* 交叉字段"回退默认值再试"的轮数上限，防死循环 */
#define CFG_CROSSFIX_MAX 4u

/* 单线程契约下的共用缓冲（见文件头） */
static char s_read_buf[CFG_FILE_MAX];
static char s_render_buf[CFG_FILE_MAX];

/* 解析期间的"非法值落在哪个分类"位图（§S8.1 按分类子树事务用）。
 * 与上面两个缓冲同理：单线程契约下不做重入保护，parse_into 进出时清零。 */
static unsigned s_cls_bad;

static void cls_mark_bad(const char *key)
{
    int cls = iv_config_class_of(key);

    if (cls >= 0)
        s_cls_bad |= (1u << (unsigned)cls);
}
/* ---------------------------------------------------------------------------
 * 一级分类（架构 §10.2 的八个顶层键，顺序即渲染顺序）
 * ------------------------------------------------------------------------- */
typedef struct {
    const char   *key;  /* 顶层键名（＝点分键第一段） */
    iv_cfg_class_t cls; /* 枚举下标 */
    const char   *desc; /* 渲染时写在分类前的 `//` 中文说明（与文件内注释同源） */
} cfg_class_t;

static const cfg_class_t s_classes[IV_CFG_CLASS_N] = {
    { "sys", IV_CFG_CLS_SYS, "基础配置：调试模式 / 定时重启 / 定时上报 / 传输模式 / 采集周期" },
    { "net", IV_CFG_CLS_NET, "网络配置：本机 IP / 掩码 / 网关；四个服务器的 host + port" },
    { "probe", IV_CFG_CLS_PROBE, "检测配置：主网检测 IP1 / IP2 与探测周期" },
    { "camera", IV_CFG_CLS_CAMERA, "摄像机配置：搜索模式；ch[] 数组最多 6 路，每路 8 个字段" },
    { "switch", IV_CFG_CLS_SWITCH, "交换机配置：固定 1 台（IP / 品牌 / 型号）" },
    { "elec", IV_CFG_CLS_ELEC, "电阈值：高压 / 低压 / 过流 / 漏电流 / 功率（小数）" },
    { "netthr", IV_CFG_CLS_NETTHR, "网络阈值：超时时间 / 丢包次数 / 重启次数 / 重启时间" },
    { "sensor", IV_CFG_CLS_SENSOR, "传感器阈值：倾斜度 / 温度上下限 / 湿度上下限（小数）" },
};

/* ---------------------------------------------------------------------------
 * 内置默认值表（F4）
 *
 * **默认值来源（§S8.1 第 7 条）**：`net` / `probe` 部分沿用 S8 现有默认值里仍然
 * 有效的部分；其余分类的取值**以架构 §10.2 的参数表为准**，量程不自行编造 ——
 * 凡是架构未给出**具体数值**的项（各种服务器地址、相机凭据、交换机 IP 等），
 * 一律以 **0 / 空串** 表示"未配置"，而不是编一个看起来合理的假值。真正需要
 * 冻结数值的量程（阈值上下限、周期范围）已按架构的可接受范围给区间。
 *
 * 字段含义：
 *   `cls`     所属一级分类（决定渲染顺序与事务粒度）
 *   `desc`    中文说明，渲染时作为该分类内该键上方的 `//` 注释（同源，故不会过期）
 *   `sdef`    字符串默认值；布尔用 "true"/"false"
 *   `enum_csv`非空则对字符串键做枚举校验
 *   `idef/imin/imax` 整数默认值与闭区间（imin > imax 表示不查范围）
 *   `ddef/dmin/dmax` double 默认值与闭区间（dmin > dmax 表示不查范围）
 *
 * ⚠ 物理量阈值的**具体出厂数值**（高压多少 V、过流多少 A）属产品参数，架构 §10.2
 *   只给了量纲与类型、未给数值 ⇒ 本表给 **0.0（未配置）**，避免编造。等产品参数
 *   冻结后在 `ivsbox/docs/` 登记并替换此处的 0.0（**只改这一张表**）。
 * ------------------------------------------------------------------------- */
typedef struct {
    iv_cfg_class_t cls;
    const char   *key;      /* 点分键 */
    const char   *desc;     /* 中文说明（渲染成文件内注释） */
    iv_cfg_type_t type;
    const char   *sdef;
    const char   *enum_csv;
    int64_t       idef;
    int64_t       imin;
    int64_t       imax;
    double        ddef;
    double        dmin;
    double        dmax;
} cfg_default_t;

/* 简写宏：让下面这张表能逐行读（字段多，全写出来会看不清） */
#define D_STR(cls, key, desc, sdef, en) \
    { cls, key, desc, IV_CFG_T_STR, sdef, en, 0, 0, 0, 0.0, 0.0, 0.0 }
#define D_INT(cls, key, desc, idef, imin, imax) \
    { cls, key, desc, IV_CFG_T_INT, "", NULL, idef, imin, imax, 0.0, 0.0, 0.0 }
#define D_BOOL(cls, key, desc, bdef) \
    { cls, key, desc, IV_CFG_T_BOOL, bdef, NULL, 0, 0, 0, 0.0, 0.0, 0.0 }
#define D_DBL(cls, key, desc, ddef, dmin, dmax) \
    { cls, key, desc, IV_CFG_T_DBL, "", NULL, 0, 0, 0, ddef, dmin, dmax }

static const cfg_default_t s_defaults[] = {
    /* ---- sys. 基础配置（架构 §10.2） ---- */
    D_BOOL(IV_CFG_CLS_SYS, "sys.debug.mode", "调试模式（开＝保留调试口与详细日志）", "false"),
    D_INT(IV_CFG_CLS_SYS, "sys.reboot.hour", "定时重启时间（每天几点，0-23；-1＝不重启）", -1, -1, 23),
    D_INT(IV_CFG_CLS_SYS, "sys.report.interval_s", "定时上报间隔（秒）", 60, 5, 3600),
    D_INT(IV_CFG_CLS_SYS, "sys.transport.mode",
          "传输模式：1=有线 2=无线 3=有线+无线 4=自动选择", 4, 1, 4),
    D_INT(IV_CFG_CLS_SYS, "sys.sample.period_ms", "采集周期（毫秒）", 1000, 100, 60000),

    /* ---- net. 网络配置（架构 §10.2；原 net.wan.* 六项已删，网口由 sys.transport.mode 隐含） ---- */
    D_STR(IV_CFG_CLS_NET, "net.ip", "本机 IP 地址（空＝由 DHCP 分配）", "", NULL),
    D_STR(IV_CFG_CLS_NET, "net.mask", "子网掩码（空＝由 DHCP 分配）", "", NULL),
    D_STR(IV_CFG_CLS_NET, "net.gateway", "网关 IP（空＝由 DHCP 分配）", "", NULL),
    D_STR(IV_CFG_CLS_NET, "net.server.wired.host", "有线平台服务器地址", "", NULL),
    D_INT(IV_CFG_CLS_NET, "net.server.wired.port", "有线平台服务器端口", 0, 0, 65535),
    D_STR(IV_CFG_CLS_NET, "net.server.wireless.host", "无线平台服务器地址", "", NULL),
    D_INT(IV_CFG_CLS_NET, "net.server.wireless.port", "无线平台服务器端口", 0, 0, 65535),
    D_STR(IV_CFG_CLS_NET, "net.server.upgrade.host", "升级服务器地址", "", NULL),
    D_INT(IV_CFG_CLS_NET, "net.server.upgrade.port", "升级服务器端口", 0, 0, 65535),
    D_STR(IV_CFG_CLS_NET, "net.server.log.host", "日志上报服务器地址", "", NULL),
    D_INT(IV_CFG_CLS_NET, "net.server.log.port", "日志上报服务器端口", 0, 0, 65535),

    /* ---- probe. 检测配置（架构 §10.2；原 net.probe.* 三项迁到本分类） ---- */
    D_STR(IV_CFG_CLS_PROBE, "probe.target1", "主网检测 IP 1", "", NULL),
    D_STR(IV_CFG_CLS_PROBE, "probe.target2", "主网检测 IP 2", "", NULL),
    D_INT(IV_CFG_CLS_PROBE, "probe.interval_ms", "探测周期（毫秒）", 5000, 500, 600000),
    D_INT(IV_CFG_CLS_PROBE, "probe.timeout_ms", "单次探测超时（毫秒，必须 < 探测周期）", 2000, 100, 60000),
    D_INT(IV_CFG_CLS_PROBE, "probe.max_fail", "连续失败次数上限", 3, 1, 100),

    /* ---- camera. 摄像机配置（架构 §10.2；ch[] 是真数组，元素＝1 条目） ---- */
    D_STR(IV_CFG_CLS_CAMERA, "camera.search", "搜索模式（off/onvif/scan）", "off", "off,onvif,scan"),
    /* 6 路 × 8 字段——逐路铺出（数组按元素存，这里给足 0..5 的默认槽）。
     * ⚠ 密码字段**存 SM4 密文**（架构 §10.2），出厂默认空串＝未配置。*/
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.0.vendor", "第 1 路厂商", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.0.ip", "第 1 路 IP", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.0.mac", "第 1 路 MAC", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.0.user", "第 1 路用户名", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.0.pass", "第 1 路口令（SM4 密文）", "", NULL),
    D_BOOL(IV_CFG_CLS_CAMERA, "camera.ch.0.record", "第 1 路是否录像", "false"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.0.stream", "第 1 路码流类型（main/sub）", "", "main,sub,"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.0.url", "第 1 路拉流地址", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.1.vendor", "第 2 路厂商", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.1.ip", "第 2 路 IP", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.1.mac", "第 2 路 MAC", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.1.user", "第 2 路用户名", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.1.pass", "第 2 路口令（SM4 密文）", "", NULL),
    D_BOOL(IV_CFG_CLS_CAMERA, "camera.ch.1.record", "第 2 路是否录像", "false"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.1.stream", "第 2 路码流类型（main/sub）", "", "main,sub,"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.1.url", "第 2 路拉流地址", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.2.vendor", "第 3 路厂商", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.2.ip", "第 3 路 IP", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.2.mac", "第 3 路 MAC", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.2.user", "第 3 路用户名", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.2.pass", "第 3 路口令（SM4 密文）", "", NULL),
    D_BOOL(IV_CFG_CLS_CAMERA, "camera.ch.2.record", "第 3 路是否录像", "false"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.2.stream", "第 3 路码流类型（main/sub）", "", "main,sub,"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.2.url", "第 3 路拉流地址", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.3.vendor", "第 4 路厂商", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.3.ip", "第 4 路 IP", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.3.mac", "第 4 路 MAC", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.3.user", "第 4 路用户名", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.3.pass", "第 4 路口令（SM4 密文）", "", NULL),
    D_BOOL(IV_CFG_CLS_CAMERA, "camera.ch.3.record", "第 4 路是否录像", "false"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.3.stream", "第 4 路码流类型（main/sub）", "", "main,sub,"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.3.url", "第 4 路拉流地址", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.4.vendor", "第 5 路厂商", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.4.ip", "第 5 路 IP", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.4.mac", "第 5 路 MAC", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.4.user", "第 5 路用户名", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.4.pass", "第 5 路口令（SM4 密文）", "", NULL),
    D_BOOL(IV_CFG_CLS_CAMERA, "camera.ch.4.record", "第 5 路是否录像", "false"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.4.stream", "第 5 路码流类型（main/sub）", "", "main,sub,"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.4.url", "第 5 路拉流地址", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.5.vendor", "第 6 路厂商", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.5.ip", "第 6 路 IP", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.5.mac", "第 6 路 MAC", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.5.user", "第 6 路用户名", "", NULL),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.5.pass", "第 6 路口令（SM4 密文）", "", NULL),
    D_BOOL(IV_CFG_CLS_CAMERA, "camera.ch.5.record", "第 6 路是否录像", "false"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.5.stream", "第 6 路码流类型（main/sub）", "", "main,sub,"),
    D_STR(IV_CFG_CLS_CAMERA, "camera.ch.5.url", "第 6 路拉流地址", "", NULL),

    /* ---- switch. 交换机配置（架构 §10.2：固定 1 台） ---- */
    D_STR(IV_CFG_CLS_SWITCH, "switch.ip", "交换机 IP", "", NULL),
    D_STR(IV_CFG_CLS_SWITCH, "switch.vendor", "交换机品牌", "", NULL),
    D_STR(IV_CFG_CLS_SWITCH, "switch.model", "交换机型号", "", NULL),

    /* ---- elec. 电阈值（小数；数值待产品参数冻结，暂给 0.0＝未配置） ---- */
    D_DBL(IV_CFG_CLS_ELEC, "elec.volt_high", "电压上限（V）", 0.0, 0.0, 1.0e6),
    D_DBL(IV_CFG_CLS_ELEC, "elec.volt_low", "电压下限（V）", 0.0, 0.0, 1.0e6),
    D_DBL(IV_CFG_CLS_ELEC, "elec.over_current", "过流阈值（A）", 0.0, 0.0, 1.0e4),
    D_DBL(IV_CFG_CLS_ELEC, "elec.leak_current", "漏电流阈值（mA）", 0.0, 0.0, 1.0e4),
    D_DBL(IV_CFG_CLS_ELEC, "elec.power", "功率阈值（W）", 0.0, 0.0, 1.0e5),

    /* ---- netthr. 网络阈值（架构 §10.2；丢包＝**次数**；承担原 wan.* 的切换门限） ---- */
    D_INT(IV_CFG_CLS_NETTHR, "netthr.timeout_ms", "网络超时时间（毫秒）", 2000, 100, 60000),
    D_INT(IV_CFG_CLS_NETTHR, "netthr.loss_count", "丢包次数上限", 3, 1, 100),
    D_INT(IV_CFG_CLS_NETTHR, "netthr.reboot_max", "自愈重启次数上限", 3, 1, 100),
    D_INT(IV_CFG_CLS_NETTHR, "netthr.reboot_interval_s", "重启时间（秒，两次重启之间的最短间隔）",
          60, 1, 3600),

    /* ---- sensor. 传感器阈值（小数；数值待产品参数冻结，暂给 0.0＝未配置） ---- */
    D_DBL(IV_CFG_CLS_SENSOR, "sensor.tilt", "倾斜度（度）", 0.0, 0.0, 90.0),
    D_DBL(IV_CFG_CLS_SENSOR, "sensor.temp_high", "温度上限（℃）", 0.0, -100.0, 200.0),
    D_DBL(IV_CFG_CLS_SENSOR, "sensor.temp_low", "温度下限（℃）", 0.0, -100.0, 200.0),
    D_DBL(IV_CFG_CLS_SENSOR, "sensor.humi_high", "湿度上限（%）", 0.0, 0.0, 100.0),
    D_DBL(IV_CFG_CLS_SENSOR, "sensor.humi_low", "湿度下限（%）", 0.0, 0.0, 100.0),
};
#undef D_STR
#undef D_INT
#undef D_BOOL
#undef D_DBL

#define CFG_DEFAULT_N (sizeof(s_defaults) / sizeof(s_defaults[0]))

/* ---------------------------------------------------------------------------
 * 内存条目与句柄
 * ------------------------------------------------------------------------- */

typedef struct {
    char          key[IV_CFG_KEY_MAX];
    iv_cfg_type_t type;
    uint8_t       origin; /* iv_cfg_origin_t */
    union {
        char    s[IV_CFG_STR_MAX];
        int64_t i;
        int     b;
        double  d; /* §S8.1：elec.* / sensor.* 的物理量阈值 */
    } v;
} cfg_item_t;

struct iv_config {
    int  used;
    char dir[IV_CFG_PATH_MAX];
    char name[IV_CFG_NAME_MAX];
    char path[IV_CFG_PATH_MAX];
    char tmppath[IV_CFG_PATH_MAX];

    uint32_t schema;
    uint64_t version;
    int      loaded;
    int      crc_mismatch;
    int      invalid;
    int      dirty;
    /* §S8.1：本次加载中"哪个一级分类出现过非法值"的位图（bit = iv_cfg_class_t）。
     * 事务粒度＝按分类子树，重载时用它决定"哪些分类整块拒绝、哪些照常采用"。 */
    unsigned cls_bad;

    unsigned   n_items;
    cfg_item_t items[IV_CFG_ITEMS_MAX];

    int ifd; /* inotify fd，-1 = 未创建 */
    int wd;

    iv_cfg_notify_fn subs_fn[IV_CFG_SUBS_MAX];
    void            *subs_user[IV_CFG_SUBS_MAX];
    unsigned         n_subs;
};

/* 事务式重载用的状态快照（整份内存状态） */
typedef struct {
    cfg_item_t items[IV_CFG_ITEMS_MAX];
    unsigned   n_items;
    uint32_t   schema;
    uint64_t   version;
    int        loaded;
    int        crc_mismatch;
    int        invalid;
    int        dirty;
    unsigned   cls_bad;
} cfg_snapshot_t;

static struct iv_config s_pool[IV_CFG_HANDLES];

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

/* 有界字符串拷贝（不用 snprintf，免得 -Wformat-truncation 在 -Werror 下断编译） */
static void str_copy(char *out, size_t cap, const char *src)
{
    size_t n;

    if (cap == 0u)
        return;
    n = strlen(src);
    if (n > cap - 1u)
        n = cap - 1u;
    memcpy(out, src, n);
    out[n] = '\0';
}

/* dir + "/" + name；放不下返回 -1（不截断，避免拼出别的路径） */
static int path_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);

    if (dl + 1u + nl + 1u > cap)
        return -1;
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, name, nl);
    out[dl + 1u + nl] = '\0';
    return 0;
}

/* §S8.1：单文件下默认表不再按配置名过滤（只有一份配置），键全局唯一。 */
static const cfg_default_t *default_find(const char *key)
{
    size_t i;

    for (i = 0; i < CFG_DEFAULT_N; i++) {
        if (strcmp(s_defaults[i].key, key) == 0)
            return &s_defaults[i];
    }
    return NULL;
}

/* 取键所属的一级分类；不在任何已知分类下返回 -1（未知顶层键）。 */
int iv_config_class_of(const char *key)
{
    size_t i;
    size_t seg;

    if (key == NULL || key[0] == '\0')
        return -1;
    seg = strcspn(key, "."); /* 第一段长度 */
    for (i = 0; i < IV_CFG_CLASS_N; i++) {
        if (strlen(s_classes[i].key) == seg && strncmp(key, s_classes[i].key, seg) == 0)
            return (int)s_classes[i].cls;
    }
    return -1;
}

/*
 * 字符串是否落在逗号分隔的枚举集合里（enum_csv 为空视为不限）。
 *
 * ⚠ 必须支持**空候选**：`"main,sub,"` 的末尾逗号表示"允许空串"（camera.ch.N.stream
 * 未配置时就是 ""）。所以循环不能写成 `for (p = csv; *p != '\0'; p = q + 1)` ——
 * 那样读到最后一段空候选时 *p 已是 '\0'，循环直接退出，空串永远判不进集合。
 * 改成"处理完当前段后，仅当 q != NULL 才继续"，且允许 p 指向结尾的 '\0'。
 */
static int enum_ok(const cfg_default_t *d, const char *val)
{
    const char *p, *q;
    size_t      n;

    if (d == NULL || d->enum_csv == NULL || d->enum_csv[0] == '\0')
        return 1;
    for (p = d->enum_csv;; p = q + 1) {
        q = strchr(p, ',');
        n = (q != NULL) ? (size_t)(q - p) : strlen(p);
        if (strlen(val) == n && strncmp(p, val, n) == 0)
            return 1;
        if (q == NULL)
            break;
    }
    return 0;
}

/* 配置名合法性：非空、不超长、不含 '/' 与 ".."（防拼出目录外的路径） */
static int name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    if (strlen(name) >= (size_t)IV_CFG_NAME_MAX)
        return 0;
    if (strchr(name, '/') != NULL || strstr(name, "..") != NULL)
        return 0;
    return 1;
}

/* 点分键合法性：段数/段长/总长在限内，不允许空段（"a..b"）与首尾点 */
static int key_ok(const char *key)
{
    const char *p = key;
    size_t      depth = 1u;
    size_t      seglen = 0u;

    if (key == NULL || key[0] == '\0' || key[0] == '.')
        return 0;
    if (strlen(key) >= (size_t)IV_CFG_KEY_MAX)
        return 0;
    for (; *p != '\0'; p++) {
        if (*p == '.') {
            if (seglen == 0u || seglen >= (size_t)IV_CFG_SEG_MAX)
                return 0;
            seglen = 0u;
            if (++depth > (size_t)IV_CFG_DEPTH_MAX)
                return 0;
        } else if (++seglen >= (size_t)IV_CFG_SEG_MAX) {
            return 0;
        }
    }
    return seglen > 0u;
}

static int item_cmp(const void *a, const void *b)
{
    return strcmp(((const cfg_item_t *)a)->key, ((const cfg_item_t *)b)->key);
}

static cfg_item_t *item_find(const iv_config_t *c, const char *key)
{
    unsigned i;

    for (i = 0; i < c->n_items; i++) {
        if (strcmp(c->items[i].key, key) == 0)
            return (cfg_item_t *)&c->items[i];
    }
    return NULL;
}

/* 取/建条目；表满返回 NULL。只建空壳，type/值由调用方填。 */
static cfg_item_t *item_touch(iv_config_t *c, const char *key)
{
    cfg_item_t *it = item_find(c, key);

    if (it != NULL)
        return it;
    if (c->n_items >= (unsigned)IV_CFG_ITEMS_MAX)
        return NULL;
    it = &c->items[c->n_items];
    c->n_items++;
    memset(it, 0, sizeof(*it));
    memcpy(it->key, key, strlen(key) + 1u);
    return it;
}

static void item_drop(iv_config_t *c, const char *key)
{
    unsigned i;

    for (i = 0; i < c->n_items; i++) {
        if (strcmp(c->items[i].key, key) == 0) {
            memmove(&c->items[i], &c->items[i + 1u],
                    (c->n_items - i - 1u) * sizeof(cfg_item_t));
            c->n_items--;
            return;
        }
    }
}

/* 把某键写回**内置默认值**；该键不在默认表里则删除条目 */
static void item_reset_to_default(iv_config_t *c, const char *key)
{
    const cfg_default_t *d = default_find(key);
    cfg_item_t          *it;

    if (d == NULL) {
        item_drop(c, key);
        return;
    }
    it = item_touch(c, key);
    if (it == NULL)
        return;
    it->type   = d->type;
    it->origin = (uint8_t)IV_CFG_FROM_DEFAULT;
    if (d->type == IV_CFG_T_STR)
        str_copy(it->v.s, sizeof(it->v.s), d->sdef);
    else if (d->type == IV_CFG_T_INT)
        it->v.i = d->idef;
    else if (d->type == IV_CFG_T_DBL)
        it->v.d = d->ddef;
    else
        it->v.b = (strcmp(d->sdef, "true") == 0) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * 数组支持（§S8.1）
 *
 * 模型：**一个数组元素＝一个条目**，元素下标进点分键（`camera.ch.0.vendor`）。
 * 为什么不把整个数组塞进一个条目：容量口径与普通键统一（架构 §10.2「数组按元素
 * 计」），且 range/enum 校验天然对每个元素的每个字段生效，不必为数组写一套。
 *
 * 代价：需要两道额外检查 ——
 *   ① 下标必须 < IV_CFG_ARR_MAX，否则一份写着 `camera.ch.99.x` 的文件会白白吃容量；
 *   ② **同一个数组内下标不得空洞**（写 `[0, 2]` 而不写 `1`）：空洞在 JSON 里
 *      渲染出来是 `[.., ..]` 还是丢掉一个位置会有歧义，宁可直接判非法。
 * 判"这段是不是数组下标"＝全十进制数字且非空。
 * ------------------------------------------------------------------------- */

/* 判断**单个键段**是否为纯十进制下标；是则回填 *idx。 */
static int seg_is_index(const char *seg, size_t len, unsigned *idx)
{
    unsigned v = 0u;
    size_t   i;

    if (len == 0u)
        return 0;
    for (i = 0; i < len; i++) {
        if (seg[i] < '0' || seg[i] > '9')
            return 0;
        v = v * 10u + (unsigned)(seg[i] - '0');
        if (v > 999u) /* 明显越界，早点退出，避免溢出 */
            return 0;
    }
    *idx = v;
    return 1;
}

/* 在点分键里找到第一个"纯数字段"的位置（返回该段起始下标），没有返回 0。
 * 用于识别"这个键是某个数组的元素"。必须在**段边界**上比对，否则 `x1.y` 会被误判。 */
static const char *array_index_seg(const char *key)
{
    const char *p = key;

    if (key == NULL || key[0] == '\0')
        return NULL;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t      len = (dot != NULL) ? (size_t)(dot - p) : strlen(p);
        unsigned    idx;

        if (seg_is_index(p, len, &idx))
            return p;
        if (dot == NULL)
            return NULL;
        p = dot + 1;
    }
}

/* 取键的数组前缀（全部点分键去掉第一个数字段及其后缀），写入 buf。
 * 例：`camera.ch.2.url` ⇒ `camera.ch`。无数字段返回 -1。 */
static int array_prefix_of(const char *key, char *buf, size_t cap)
{
    const char *seg = array_index_seg(key);
    size_t      n;

    if (seg == NULL)
        return -1;
    n = (size_t)(seg - key); /* 含结尾的点 */
    if (n == 0u || n + 1u > cap)
        return -1;
    memcpy(buf, key, n - 1u); /* 去掉那个点 */
    buf[n - 1u] = '\0';
    return 0;
}

/* key 是否占着某个已知默认键的**祖先路径**（如 "net" 之于 "net.wan.mode"）。
 * 默认表只列叶子，所以这种键一定是"把子树写成了标量"的形态冲突。 */
static int is_default_parent(const char *key)
{
    size_t i, kl = strlen(key);

    for (i = 0; i < CFG_DEFAULT_N; i++) {
        const cfg_default_t *d = &s_defaults[i];

        if (kl < strlen(d->key) && strncmp(d->key, key, kl) == 0 && d->key[kl] == '.')
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 值合法性：已知键查类型/范围/枚举；未知键只认类型（向前兼容，不做业务校验）。
 * 三条**必须拒绝**的形态（任一条放过都会造成静默数据损坏）：
 *   ① 占着已知键父路径的标量键（`"net": 5`）—— 否则整棵 `net.*` 默认子树补不进来，
 *      热更新还会因为"没发现非法值"而整份接受，把运行中的配置悄悄清空（F6/F7）；
 *   ② 数组下标越界（`camera.ch.9.*`）—— 白白吃容量，且渲染回来会丢；
 *   ③ 数组下标空洞（写了 0 与 2、没写 1）—— 渲染成 `[...]` 时位置有歧义。
 * 判非法后该键回退默认：默认表里没有它 ⇒ 条目被删除，随后默认子树/默认槽正常补齐。
 *
 * `strict` 控制**空洞判定**是否生效：只有保存/整表校验（items_check）传 1。
 * 加载路径（flatten_leaf → item_check）传 0，因为加载是"边解析边加条目"，判空洞
 * 时后面的下标可能还没进来，必然误伤（详见函数内注释）。
 * ------------------------------------------------------------------------- */
static int item_check(const iv_config_t *c, const cfg_item_t *it, int strict, const char **why)
{
    const cfg_default_t *d = default_find(it->key);
    const char          *aseg;

    /* ② / ③：数组下标检查（对已知与未知键都做） */
    aseg = array_index_seg(it->key);
    if (aseg != NULL) {
        unsigned idx = 0u;
        const char *dot = strchr(aseg, '.');
        size_t      len = (dot != NULL) ? (size_t)(dot - aseg) : strlen(aseg);

        (void)seg_is_index(aseg, len, &idx);
        if (idx >= (unsigned)IV_CFG_ARR_MAX) {
            *why = "array index out of range";
            return 0;
        }

        /* 空洞检查：同前缀的每个前置下标都必须有至少一个条目存在。
         * **只在 `strict`（保存/整表校验）时判**：加载时条目是**边解析边加**的，
         * 此刻后面的下标还没进来，判空洞必然误伤（曾经的 `c->n_items > 0u` 守卫
         * 形同虚设 —— 加载期它恒真，于是 `[{"a":1},{},{"a":3}]` 里的 `.2.a`
         * 会被判成空洞而**悄悄丢掉**，热更新时表现为"改了几路相机、只生效第一路"）。 */
        if (strict) {
            char   pre[IV_CFG_KEY_MAX + 8u];
            unsigned k;

            if (array_prefix_of(it->key, pre, sizeof(pre)) == 0) {
                for (k = 0u; k < idx; k++) {
                    char t[IV_CFG_KEY_MAX + 16u];
                    size_t need = strlen(pre) + 1u + 2u; /* pre + '.' + 'k' + NUL */

                    if (k > 9u)
                        need = strlen(pre) + 1u + 5u;
                    if (need > sizeof(t))
                        break;
                    {
                        size_t pl = strlen(pre);

                        memcpy(t, pre, pl);
                        t[pl] = '.';
                        t[pl + 1u] = (char)('0' + (char)k);
                        t[pl + 2u] = '\0';
                    }
                    {
                        unsigned j;
                        int      found = 0;

                        for (j = 0u; j < c->n_items; j++) {
                            size_t cl = strlen(c->items[j].key);
                            size_t tl = strlen(t);

                            if (cl > tl && strncmp(c->items[j].key, t, tl) == 0 &&
                                c->items[j].key[tl] == '.') {
                                found = 1;
                                break;
                            }
                        }
                        if (!found) {
                            *why = "array index gap";
                            return 0;
                        }
                    }
                }
            }
        }
    }

    if (d == NULL) {
        if (is_default_parent(it->key)) {
            *why = "scalar where an object is expected";
            return 0;
        }
        return 1;
    }
    if (d->type != it->type) {
        *why = "type mismatch";
        return 0;
    }
    if (d->type == IV_CFG_T_INT && d->imin <= d->imax && (it->v.i < d->imin || it->v.i > d->imax)) {
        *why = "out of range";
        return 0;
    }
    if (d->type == IV_CFG_T_DBL && d->dmin <= d->dmax && (it->v.d < d->dmin || it->v.d > d->dmax)) {
        *why = "out of range";
        return 0;
    }
    if (d->type == IV_CFG_T_STR && !enum_ok(d, it->v.s)) {
        *why = "not in enum";
        return 0;
    }
    return 1;
}

/*
 * 前缀冲突：一个键是另一个键的父路径（"net" 与 "net.wan.fail_n"）时，写回 JSON
 * 只能二选一 —— 不查就会静默丢掉其中一个。发生条件是"文件里有未知的浅层标量键、
 * 而默认表又要往它下面补字段"，所以必须显式拒绝而不是让它悄悄消失。
 */
static int items_conflict(const iv_config_t *c, const char **k1, const char **k2)
{
    unsigned i, j;

    for (i = 0; i < c->n_items; i++) {
        size_t li = strlen(c->items[i].key);

        for (j = 0; j < c->n_items; j++) {
            if (i == j)
                continue;
            if (strncmp(c->items[i].key, c->items[j].key, li) == 0 &&
                c->items[j].key[li] == '.') {
                *k1 = c->items[i].key;
                *k2 = c->items[j].key;
                return 1;
            }
        }
    }
    return 0;
}

/*
 * 跨字段校验（校验四类里的第 4 类）。本步两条，都是真实业务不变量：
 *   1. 探测超时必须小于探测周期，否则本轮探测还没结束下一轮就开跑；
 *   2. 阈值上下限不得倒置（电：高压 ≥ 低压；传感器：温度/湿度上限 ≥ 下限）——
 *      倒置的阈值会让"越限告警"恒真或恒假，是最容易静默生效的配置错误。
 * 失败时回带参与冲突的两个键，供加载路径回退默认、保存路径直接拒绝。
 * **注意**：跨字段不变量天然是**同一分类内**的（第 1 条同属 probe、第 2 条同属
 * elec/sensor），因此不会与"按分类子树事务"冲突 —— 一个分类被拒不影响另一个。
 */
static int cross_check(const iv_config_t *c, const char **k1, const char **k2, const char **why)
{
    static const struct {
        const char *hi;
        const char *lo;
    } pairs[] = {
        { "elec.volt_high", "elec.volt_low" },
        { "sensor.temp_high", "sensor.temp_low" },
        { "sensor.humi_high", "sensor.humi_low" },
    };
    const cfg_item_t *ival, *tval;
    size_t            i;

    *k1 = NULL;
    *k2 = NULL;

    ival = item_find(c, "probe.interval_ms");
    tval = item_find(c, "probe.timeout_ms");
    if (ival != NULL && tval != NULL && ival->type == IV_CFG_T_INT && tval->type == IV_CFG_T_INT &&
        tval->v.i >= ival->v.i) {
        *k1  = "probe.timeout_ms";
        *k2  = "probe.interval_ms";
        *why = "probe timeout_ms must be < interval_ms";
        return 0;
    }

    for (i = 0u; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        const cfg_item_t *hi = item_find(c, pairs[i].hi);
        const cfg_item_t *lo = item_find(c, pairs[i].lo);

        /* 只在两个值都**已配置**（非 0.0）时校验：0.0 = 未配置，不参与比较。
         * 否则出厂默认（全 0.0）会因"上限 0 < 下限 0"被误判成冲突。 */
        if (hi != NULL && lo != NULL && hi->type == IV_CFG_T_DBL && lo->type == IV_CFG_T_DBL &&
            hi->v.d != 0.0 && lo->v.d != 0.0 && hi->v.d < lo->v.d) {
            *k1  = pairs[i].hi;
            *k2  = pairs[i].lo;
            *why = "upper threshold must be >= lower threshold";
            return 0;
        }
    }

    return 1;
}

/* 全表校验（保存路径用，严格）：类型/范围/枚举 + 前缀冲突 + 跨字段 */
static int items_check(const iv_config_t *c, const char **bad, const char **why)
{
    unsigned i;

    for (i = 0; i < c->n_items; i++) {
        if (!item_check(c, &c->items[i], 1, why)) {
            *bad = c->items[i].key;
            return 0;
        }
    }
    if (items_conflict(c, bad, why)) {
        *why = "key prefix conflict";
        return 0;
    }
    return 1;
}

/* 条目指纹串（按 key 排序 ⇒ 与 JSON 排版无关）：key=type:value\n
 * double 用 %.17g：截断位数不同会让"内容没变但 crc 变了"，必须给足有效位。 */
static size_t items_fingerprint(const iv_config_t *c, char *buf, size_t cap)
{
    cfg_item_t sorted[IV_CFG_ITEMS_MAX];
    size_t     used = 0u;
    unsigned   i;

    memcpy(sorted, c->items, c->n_items * sizeof(cfg_item_t));
    qsort(sorted, c->n_items, sizeof(cfg_item_t), item_cmp);

    for (i = 0; i < c->n_items; i++) {
        char   line[IV_CFG_KEY_MAX + IV_CFG_STR_MAX + 8u];
        size_t n;

        if (sorted[i].type == IV_CFG_T_STR)
            snprintf(line, sizeof(line), "%s=s:%s\n", sorted[i].key, sorted[i].v.s);
        else if (sorted[i].type == IV_CFG_T_INT)
            snprintf(line, sizeof(line), "%s=i:%lld\n", sorted[i].key,
                     (long long)sorted[i].v.i);
        else if (sorted[i].type == IV_CFG_T_DBL)
            snprintf(line, sizeof(line), "%s=d:%.17g\n", sorted[i].key, sorted[i].v.d);
        else
            snprintf(line, sizeof(line), "%s=b:%d\n", sorted[i].key, sorted[i].v.b);

        n = strlen(line);
        if (n > cap - used)
            break; /* 截断：指纹退化但不会越界（键数受 ITEMS_MAX 约束，正常走不到） */
        memcpy(buf + used, line, n);
        used += n;
    }
    return used;
}

/* 指纹缓冲是**文件级 static 而非栈上**：ITEMS_MAX=128 时若放栈上，
 * 单个函数的栈帧就会有 128 × (96+160+8) ≈ 33 KB，而本函数会被 render/save
 * 等路径层层调用，叠加起来容易在板端 8 MB 线程栈上冒风险。少用一个栈变量。 */
static char s_fp_buf[IV_CFG_ITEMS_MAX * (IV_CFG_KEY_MAX + IV_CFG_STR_MAX + 8u)];

static uint16_t items_crc(const iv_config_t *c)
{
    size_t n = items_fingerprint(c, s_fp_buf, sizeof(s_fp_buf));

    return iv_crc16_modbus(s_fp_buf, n, IV_CRC16_MODBUS_SEED_INIT);
}

/* ---------------------------------------------------------------------------
 * F1：解析与加载
 * ------------------------------------------------------------------------- */

/* 把一个 JSON 叶子写进条目表；类型不支持（null）则丢弃并告警。
 * §S8.1 新增：`json_type_double` 与 `json_type_array` 不再被 `default:` 丢弃。
 * 数组按"元素＝条目"展平，元素内的字段拼进键（`camera.ch.0.vendor`）。 */
static void flatten_leaf(iv_config_t *c, const char *key, json_object *val, int *invalid)
{
    cfg_item_t *it;
    const char *why = NULL;

    it = item_touch(c, key);
    if (it == NULL) {
        IV_LOG_W(CFG_MOD, "%s: too many keys, '%s' ignored", c->name, key);
        return;
    }

    switch (json_object_get_type(val)) {
    case json_type_string: {
        const char *s = json_object_get_string(val);

        if (s == NULL)
            s = "";
        if (strlen(s) >= (size_t)IV_CFG_STR_MAX) {
            IV_LOG_W(CFG_MOD, "%s: '%s' string too long, rejected", c->name, key);
            (*invalid)++;
            cls_mark_bad(key);
            item_reset_to_default(c, key);
            return;
        }
        it->type   = IV_CFG_T_STR;
        it->origin = (uint8_t)IV_CFG_FROM_FILE;
        str_copy(it->v.s, sizeof(it->v.s), s);
        break;
    }
    case json_type_int:
        it->type   = IV_CFG_T_INT;
        it->origin = (uint8_t)IV_CFG_FROM_FILE;
        it->v.i    = (int64_t)json_object_get_int64(val);
        break;
    case json_type_boolean:
        it->type   = IV_CFG_T_BOOL;
        it->origin = (uint8_t)IV_CFG_FROM_FILE;
        it->v.b    = json_object_get_boolean(val) ? 1 : 0;
        break;
    case json_type_double:
        it->type   = IV_CFG_T_DBL;
        it->origin = (uint8_t)IV_CFG_FROM_FILE;
        it->v.d    = json_object_get_double(val);
        break;
    default:
        IV_LOG_W(CFG_MOD, "%s: '%s' has unsupported json type, rejected", c->name, key);
        (*invalid)++;
        cls_mark_bad(key);
        item_drop(c, key);
        return;
    }

    /* 文件里的值本身非法 ⇒ 该键回退默认并计数。注意"缺字段"不在此列：那是 F4
     * 的正常补齐，由 fill_missing_defaults 处理，不计入 invalid。
     * 这里传 strict=0：加载期不判数组空洞（条目还没加全，见 item_check 注释）。 */
    if (!item_check(c, it, 0, &why)) {
        IV_LOG_W(CFG_MOD, "%s: '%s' invalid (%s), fallback to default", c->name, key, why);
        (*invalid)++;
        cls_mark_bad(key);
        item_reset_to_default(c, key);
    }
}

/* 展平一个数组：元素索引拼进键（`camera.ch` + `.0` + `.vendor`）。
 * 元素本身是对象时递归展平；元素是标量时直接落成 `<prefix>.<i>`（也允许）。 */
static void flatten(iv_config_t *c, const char *prefix, json_object *obj, unsigned depth,
                    int *invalid);

static void flatten_array(iv_config_t *c, const char *prefix, json_object *arr, unsigned depth,
                          int *invalid)
{
    size_t n = json_object_array_length(arr);

    if (n > (size_t)IV_CFG_ARR_MAX) {
        /* 超长数组：只取前 IV_CFG_ARR_MAX 个元素，多出来的按非法计数（不静默全收） */
        IV_LOG_W(CFG_MOD, "%s: array '%s' has %u elements (max %u), extras rejected", c->name,
                 prefix, (unsigned)n, (unsigned)IV_CFG_ARR_MAX);
        (*invalid) += (int)(n - (size_t)IV_CFG_ARR_MAX);
        cls_mark_bad(prefix);
        n = (size_t)IV_CFG_ARR_MAX;
    }

    for (size_t i = 0u; i < n; i++) {
        json_object *el = json_object_array_get_idx(arr, (int)i);
        char         full[IV_CFG_KEY_MAX];
        size_t       pl = strlen(prefix);
        char         idxbuf[8];
        size_t       il;

        if (el == NULL)
            continue;
        /* 下标渲染成十进制（0..ARR_MAX-1，最多两位） */
        {
            unsigned v = (unsigned)i;
            char     tmp[8];
            int      k = 0;

            if (v == 0u) {
                tmp[k++] = '0';
            } else {
                while (v > 0u) {
                    tmp[k++] = (char)('0' + (char)(v % 10u));
                    v /= 10u;
                }
            }
            for (int j = 0; j < k; j++)
                idxbuf[j] = tmp[k - 1 - j];
            idxbuf[k] = '\0';
            il        = (size_t)k;
        }
        if (pl + 1u + il + 1u > sizeof(full)) {
            IV_LOG_W(CFG_MOD, "%s: array key '%s.%s' too long, ignored", c->name, prefix, idxbuf);
            continue;
        }
        memcpy(full, prefix, pl);
        full[pl] = '.';
        memcpy(full + pl + 1u, idxbuf, il + 1u);

        if (json_object_get_type(el) == json_type_object)
            flatten(c, full, el, depth + 1u, invalid);
        else
            flatten_leaf(c, full, el, invalid);
    }
}

/* 递归展平 JSON 对象树；只处理 object，叶子交给 flatten_leaf */
static void flatten(iv_config_t *c, const char *prefix, json_object *obj, unsigned depth,
                    int *invalid)
{
    json_object_object_foreach(obj, key, val)
    {
        char       full[IV_CFG_KEY_MAX];
        size_t     need;
        const char *p = key;

        if (depth > (unsigned)IV_CFG_DEPTH_MAX) {
            IV_LOG_W(CFG_MOD, "%s: '%s' nested too deep, ignored", c->name, key);
            continue;
        }
        if (p[0] == '_' && prefix[0] == '\0')
            continue; /* 顶层元数据键（_meta）不算业务键 */

        if (prefix[0] == '\0') {
            need = strlen(p) + 1u;
            if (need > (size_t)IV_CFG_KEY_MAX) {
                IV_LOG_W(CFG_MOD, "%s: key '%s' too long, ignored", c->name, p);
                continue;
            }
            str_copy(full, sizeof(full), p);
        } else {
            need = strlen(prefix) + 1u + strlen(p) + 1u;
            if (need > (size_t)IV_CFG_KEY_MAX) {
                IV_LOG_W(CFG_MOD, "%s: key '%s.%s' too long, ignored", c->name, prefix, p);
                continue;
            }
            str_copy(full, sizeof(full), prefix);
            full[strlen(prefix)] = '.';
            str_copy(full + strlen(prefix) + 1u, sizeof(full) - strlen(prefix) - 1u, p);
        }

        if (json_object_get_type(val) == json_type_object)
            flatten(c, full, val, depth + 1u, invalid);
        else if (json_object_get_type(val) == json_type_array)
            flatten_array(c, full, val, depth + 1u, invalid);
        else
            flatten_leaf(c, full, val, invalid);
    }
}

/* 读整个文件到 buf（NUL 结尾）。>=0 为长度，<0 为负的 IV_* 码。 */
static long file_read_all(const char *path, char *buf, size_t cap)
{
    int    fd;
    size_t used = 0u;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        /* 符号链接一律不跟（配置路径被换成链接时宁可退回默认值并告警） */
        if (errno == ELOOP)
            return IV_EAUTH;
        return (errno == ENOENT) ? IV_ENOENT : IV_EIO;
    }

    for (;;) {
        ssize_t n = read(fd, buf + used, cap - 1u - used);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return IV_EIO;
        }
        if (n == 0)
            break;
        used += (size_t)n;
        if (used >= cap - 1u)
            break; /* 超限：交给解析失败处理，不吃更多内存 */
    }
    (void)close(fd);
    buf[used] = '\0';
    return (long)used;
}

/* 从 JSON 文本构建条目表（会先清空）。返回 IV_OK 或负码。 */
static int parse_into(iv_config_t *c, const char *text, unsigned len, uint32_t *schema,
                      uint64_t *version, uint16_t *file_crc, int *has_crc, int *invalid)
{
    struct json_tokener   *tok;
    struct json_object    *root;
    struct json_object    *meta;
    enum json_tokener_error jerr;
    int                    bad = 0;

    *schema   = IV_CFG_SCHEMA_DEFAULT;
    *version  = 0u;
    *file_crc = 0u;
    *has_crc  = 0;

    c->n_items = 0u;
    s_cls_bad  = 0u; /* §S8.1：每次解析都从零开始记"哪个分类有非法值" */

    tok = json_tokener_new();
    if (tok == NULL)
        return IV_ENOMEM;
    root = json_tokener_parse_ex(tok, text, (int)len);
    jerr = json_tokener_get_error(tok);
    json_tokener_free(tok);

    if (jerr != json_tokener_success || root == NULL) {
        if (root != NULL)
            json_object_put(root);
        return IV_ECORRUPT;
    }
    if (json_object_get_type(root) != json_type_object) {
        json_object_put(root);
        return IV_ECORRUPT;
    }

    if (json_object_object_get_ex(root, CFG_META_KEY, &meta) &&
        json_object_get_type(meta) == json_type_object) {
        json_object *v;

        if (json_object_object_get_ex(meta, "schema", &v) &&
            json_object_get_type(v) == json_type_int)
            *schema = (uint32_t)json_object_get_int64(v);
        if (json_object_object_get_ex(meta, "version", &v) &&
            json_object_get_type(v) == json_type_int)
            *version = (uint64_t)json_object_get_int64(v);
        if (json_object_object_get_ex(meta, "crc", &v) &&
            json_object_get_type(v) == json_type_int) {
            *file_crc = (uint16_t)(json_object_get_int64(v) & 0xFFFF);
            *has_crc  = 1;
        }
    }

    flatten(c, "", root, 1u, &bad);
    json_object_put(root);
    *invalid = bad;
    return IV_OK;
}

/* 补齐默认表里、但内存中没有的键（与前缀冲突的跳过并告警），返回补了几条 */
static int fill_missing_defaults(iv_config_t *c)
{
    size_t i;
    int    filled = 0;

    for (i = 0; i < CFG_DEFAULT_N; i++) {
        const cfg_default_t *d = &s_defaults[i];
        unsigned             j;
        size_t               dl;
        int                  conflict = 0;

        if (item_find(c, d->key) != NULL)
            continue;

        /* 已有条目占着它的父路径 ⇒ 补了就会在前缀冲突里丢掉一边，跳过。
         * item_check() 已把这种键判非法并从表里删掉（见 is_default_parent），
         * 所以正常路径走不到这里；保留作防御，免得将来校验放宽后静默丢键。 */
        dl = strlen(d->key);
        for (j = 0; j < c->n_items; j++) {
            size_t lj = strlen(c->items[j].key);

            if (lj < dl && strncmp(d->key, c->items[j].key, lj) == 0 && d->key[lj] == '.') {
                conflict = 1;
                break;
            }
        }
        if (conflict) {
            IV_LOG_W(CFG_MOD, "%s: skip default '%s' (conflicts with existing key)", c->name,
                     d->key);
            continue;
        }
        item_reset_to_default(c, d->key);
        filled++;
    }
    return filled;
}

/* 交叉字段冲突时把参与的两个键回退默认（加载路径用）。返回 1 = 已无冲突。 */
static int cross_fix_by_default(iv_config_t *c)
{
    const char *k1, *k2, *why;
    unsigned    round;

    for (round = 0u; round < CFG_CROSSFIX_MAX; round++) {
        if (cross_check(c, &k1, &k2, &why))
            return 1;
        IV_LOG_W(CFG_MOD, "%s: cross-field conflict (%s), revert '%s'/'%s' to default", c->name,
                 why, k1, k2);
        c->invalid++;
        cls_mark_bad(k1); /* 跨字段冲突必在同一分类内 ⇒ 标记该分类 */
        item_reset_to_default(c, k1);
        item_reset_to_default(c, k2);
    }
    return cross_check(c, &k1, &k2, &why);
}

static void snap_take(const iv_config_t *c, cfg_snapshot_t *s)
{
    memcpy(s->items, c->items, sizeof(s->items));
    s->n_items      = c->n_items;
    s->schema       = c->schema;
    s->version      = c->version;
    s->loaded       = c->loaded;
    s->crc_mismatch = c->crc_mismatch;
    s->invalid      = c->invalid;
    s->dirty        = c->dirty;
    s->cls_bad      = c->cls_bad;
}

static void snap_restore(iv_config_t *c, const cfg_snapshot_t *s)
{
    memcpy(c->items, s->items, sizeof(c->items));
    c->n_items      = s->n_items;
    c->schema       = s->schema;
    c->version      = s->version;
    c->loaded       = s->loaded;
    c->crc_mismatch = s->crc_mismatch;
    c->invalid      = s->invalid;
    c->dirty        = s->dirty;
    c->cls_bad      = s->cls_bad;
}

/*
 * 从磁盘加载并采用（open 与热重载共用）。
 * 返回 IV_OK / IV_ENOENT / IV_ECORRUPT / IV_EAUTH / IV_EIO / IV_ENOMEM。
 * 成功时 `c->invalid` 被置为**本次加载中因值非法而回退默认的键数**（调用方据此
 * 决定要不要整份拒绝：启动路径接受、重载路径拒绝）。
 */
static int load_from_disk(iv_config_t *c)
{
    long     n;
    uint32_t schema;
    uint64_t version;
    uint16_t fcr;
    int      has_crc = 0;
    int      invalid = 0;
    int      rc;

    n = file_read_all(c->path, s_read_buf, sizeof(s_read_buf));
    if (n < 0)
        return (int)n;
    if (n == 0)
        return IV_ECORRUPT; /* 空文件：按 F4 走默认值 */

    rc = parse_into(c, s_read_buf, (unsigned)n, &schema, &version, &fcr, &has_crc, &invalid);
    if (rc != IV_OK)
        return rc;

    c->schema       = schema;
    c->version      = version;
    c->crc_mismatch = (has_crc && fcr != items_crc(c)) ? 1 : 0;
    if (c->crc_mismatch)
        IV_LOG_W(CFG_MOD, "%s: file crc mismatch (file=0x%04X), accepted with warning", c->name,
                 (unsigned)fcr);

    (void)fill_missing_defaults(c); /* 缺字段是正常补齐，不计入 invalid */
    if (!cross_fix_by_default(c))
        invalid++;
    c->invalid   = invalid;
    c->cls_bad   = s_cls_bad; /* 解析期累积的"哪个分类有非法值" */
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * F5：原子写
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * 渲染器：把内存条目渲染成**带中文注释的 JSON 文本**
 *
 * 为什么不走 json-c 的 `json_object_to_json_string_ext()`：json-c **产生不了注释**，
 * 而架构 §10.2 明确要求文件内有中文注释。若用"json-c 渲染主体 + 事后按位置插注释"，
 * 每次保存都要把两段文本缝合，缝错就是坏文件。这里改为**完全自持的渲染器**：
 *   顺序：`_meta` → 八个一级分类（固定顺序，只输出有成员的那些）→ 分类内按键字典序；
 *   注释：分类前一行 `// <分类说明>`；**每个已知叶子键**前一行 `// <键说明>`；
 *         文字取自默认值表的 `desc`（同源 ⇒ 注释不会与默认值脱节）；
 *   嵌套：点分键按需开对象（`"net": { "server": { "wired": { ... } } }`）；
 *   数组：同一数组前缀（如 `camera.ch`）的条目聚成 `[ {...}, {...} ]`；
 *   双精度：`%.17g` 且保证含 `.`/`e`，否则 `1.0` 会读回 int、下次保存类型就漂；
 *   逗号：每层自己数成员，"还有下一个成员才加逗号" —— 不靠事后剪裁。
 *
 * 产物必须能被本模块自己的 `parse_into()` 读回且逐键等值（有往返单测钉死）。
 * ------------------------------------------------------------------------- */

/* 渲染缓冲写入器：越界置 err（调用方转成 IV_ERANGE） */
typedef struct {
    size_t off;
    int    err;
} rj_t;

static void rj_put(rj_t *w, const char *p, size_t n)
{
    if (w->err)
        return;
    if (n > (size_t)CFG_FILE_MAX - 2u - w->off) {
        w->err = 1;
        return;
    }
    memcpy(s_render_buf + w->off, p, n);
    w->off += n;
}

static void rj_str(rj_t *w, const char *s)
{
    rj_put(w, s, strlen(s));
}

/* 缩进：写 n 个空格 */
static void rj_indent(rj_t *w, unsigned n)
{
    static const char sp[] = "                                "; /* 32 空格 */

    while (n > 0u) {
        unsigned k = (n > sizeof(sp) - 1u) ? (unsigned)(sizeof(sp) - 1u) : n;

        rj_put(w, sp, k);
        n -= k;
    }
}

/* 渲染一个标量值 */
static void rj_scalar(rj_t *w, const cfg_item_t *it)
{
    char buf[64];

    switch (it->type) {
    case IV_CFG_T_STR: {
        const char *p = it->v.s;

        rj_put(w, "\"", 1u);
        for (; *p != '\0'; p++) {
            switch (*p) {
            case '"':
                rj_put(w, "\\\"", 2u);
                break;
            case '\\':
                rj_put(w, "\\\\", 2u);
                break;
            case '\n':
                rj_put(w, "\\n", 2u);
                break;
            case '\r':
                rj_put(w, "\\r", 2u);
                break;
            case '\t':
                rj_put(w, "\\t", 2u);
                break;
            default:
                if ((unsigned char)*p < 0x20u) {
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)*p);
                    rj_str(w, buf);
                } else {
                    rj_put(w, p, 1u);
                }
            }
        }
        rj_put(w, "\"", 1u);
        break;
    }
    case IV_CFG_T_INT:
        snprintf(buf, sizeof(buf), "%lld", (long long)it->v.i);
        rj_str(w, buf);
        break;
    case IV_CFG_T_DBL: {
        /* %.17g 无损；但整数型 double（1.0）会印成 "1" ⇒ 补 ".0"，
         * 否则解析回来成了 json_type_int、类型当场漂移。 */
        snprintf(buf, sizeof(buf), "%.17g", it->v.d);
        rj_str(w, buf);
        if (strchr(buf, '.') == NULL && strchr(buf, 'e') == NULL && strchr(buf, 'E') == NULL)
            rj_str(w, ".0");
        break;
    }
    case IV_CFG_T_BOOL:
    default:
        rj_str(w, it->v.b ? "true" : "false");
        break;
    }
}

/* 在 sorted[lo,hi) 内，前缀为 prefix（""=顶层）时，取"下一段名"为 seg 的成员区间 [i,j)。
 * 返回 0 = 找到（*i,*j 已填），-1 = lo 项不属于本前缀。 */
static int rj_group(const cfg_item_t *sorted, unsigned lo, unsigned hi, const char *prefix,
                    const char *seg, unsigned *i_out, unsigned *j_out)
{
    size_t   pl = (prefix[0] != '\0') ? strlen(prefix) : 0u;
    unsigned i, j;

    for (i = lo; i < hi; i++) {
        const char *k = sorted[i].key;

        if (pl != 0u) {
            if (strncmp(k, prefix, pl) != 0 || k[pl] != '.')
                break;
            k += pl + 1u;
        }
        if (strncmp(k, seg, strlen(seg)) == 0 &&
            (k[strlen(seg)] == '.' || k[strlen(seg)] == '\0'))
            break;
    }
    if (i >= hi)
        return -1;

    for (j = i; j < hi; j++) {
        const char *k = sorted[j].key;
        size_t      sl = strlen(seg);

        if (pl != 0u) {
            if (strncmp(k, prefix, pl) != 0 || k[pl] != '.')
                break;
            k += pl + 1u;
        }
        if (strncmp(k, seg, sl) != 0 || (k[sl] != '.' && k[sl] != '\0'))
            break;
    }
    *i_out = i;
    *j_out = j;
    return 0;
}

/* 渲染一个对象：先列本层的子对象/数组（按键序），再列叶子。
 * 注意 render 的输出顺序要与 parse 无关（parse 按键查表，不看顺序）。 */
static void rj_object(rj_t *w, const cfg_item_t *sorted, unsigned lo, unsigned hi,
                      const char *prefix, unsigned indent);

/* 渲染一个数组：前缀 arrpre（如 "camera.ch"），成员区间 [lo,hi) 全是 arrpre.<idx>.* */
static void rj_array(rj_t *w, const cfg_item_t *sorted, unsigned lo, unsigned hi,
                     const char *arrpre, unsigned indent)
{
    size_t   pl  = strlen(arrpre);
    unsigned i   = lo;
    int      first = 1;

    rj_put(w, "[", 1u);
    while (i < hi) {
        /* 取本元素下标 */
        const char *np = sorted[i].key + pl + 1u;
        const char *nd = strchr(np, '.');
        size_t      nl = (nd != NULL) ? (size_t)(nd - np) : strlen(np);
        char        epre[IV_CFG_KEY_MAX];
        unsigned    j;

        memcpy(epre, sorted[i].key, (size_t)(np - sorted[i].key) + nl);
        epre[(size_t)(np - sorted[i].key) + nl] = '\0';

        /* 本元素成员区间 [i, j) */
        for (j = i; j < hi; j++) {
            size_t el = strlen(epre);

            if (strncmp(sorted[j].key, epre, el) != 0 || sorted[j].key[el] != '.')
                break;
        }

        if (!first)
            rj_put(w, ",", 1u);
        first = 0;
        rj_put(w, "\n", 1u);
        rj_indent(w, indent);
        rj_put(w, "{", 1u);
        rj_object(w, sorted, i, j, epre, indent + 2u);
        rj_put(w, "\n", 1u);
        rj_indent(w, indent);
        rj_put(w, "}", 1u);

        i = j;
    }
    if (!first) {
        rj_put(w, "\n", 1u);
        rj_indent(w, indent > 2u ? indent - 2u : 0u);
    }
    rj_put(w, "]", 1u);
}

/* 渲染对象的前缀已写出（含 "{"），这里写成员；结束时**不写**收尾 "}"（调用方写）。 */
static void rj_object(rj_t *w, const cfg_item_t *sorted, unsigned lo, unsigned hi,
                      const char *prefix, unsigned indent)
{
    size_t   pl = (prefix[0] != '\0') ? strlen(prefix) : 0u;
    unsigned i  = lo;
    int      first = 1;

    while (i < hi) {
        const char *k = sorted[i].key + pl + (pl != 0u ? 1u : 0u);
        const char *dot = strchr(k, '.');

        if (dot != NULL) {
            /* 子对象 / 数组：取 seg，找它的整个区间 */
            char     seg[IV_CFG_SEG_MAX];
            size_t   seglen = (size_t)(dot - k);
            unsigned gi, gj;
            const char *nextp;
            size_t      nlen;
            unsigned    idx;

            if (seglen >= sizeof(seg))
                break;
            memcpy(seg, k, seglen);
            seg[seglen] = '\0';

            if (rj_group(sorted, i, hi, prefix, seg, &gi, &gj) != 0)
                break;

            /* 判断 seg 下面是"数组"还是"对象"：看 gi 项的下一段是否纯数字 */
            nextp = sorted[gi].key + pl + (pl != 0u ? 1u : 0u) + seglen + 1u;
            {
                const char *nd = strchr(nextp, '.');

                nlen = (nd != NULL) ? (size_t)(nd - nextp) : strlen(nextp);
            }

            if (!first)
                rj_put(w, ",", 1u);
            first = 0;
            rj_put(w, "\n", 1u);
            rj_indent(w, indent);
            rj_put(w, "\"", 1u);
            rj_str(w, seg);
            rj_put(w, "\": ", 3u);

            if (seg_is_index(nextp, nlen, &idx)) {
                char ap[IV_CFG_KEY_MAX];

                /* 数组前缀 = prefix + "." + seg（或顶层就是 seg） */
                if (pl != 0u) {
                    size_t need = pl + 1u + seglen;

                    if (need >= sizeof(ap))
                        break;
                    memcpy(ap, prefix, pl);
                    ap[pl] = '.';
                    memcpy(ap + pl + 1u, seg, seglen + 1u);
                } else {
                    memcpy(ap, seg, seglen + 1u);
                }
                rj_array(w, sorted, gi, gj, ap, indent + 2u);
            } else {
                char cp[IV_CFG_KEY_MAX];

                if (pl != 0u) {
                    size_t need = pl + 1u + seglen;

                    if (need >= sizeof(cp))
                        break;
                    memcpy(cp, prefix, pl);
                    cp[pl] = '.';
                    memcpy(cp + pl + 1u, seg, seglen + 1u);
                } else {
                    memcpy(cp, seg, seglen + 1u);
                }
                rj_put(w, "{", 1u);
                rj_object(w, sorted, gi, gj, cp, indent + 2u);
                rj_put(w, "\n", 1u);
                rj_indent(w, indent);
                rj_put(w, "}", 1u);
            }
            i = gj;
            continue;
        }

        /* 叶子 */
        {
            const cfg_default_t *d = default_find(sorted[i].key);
            char                 kbuf[IV_CFG_KEY_MAX];

            if (!first)
                rj_put(w, ",", 1u);
            first = 0;

            if (d != NULL) {
                rj_put(w, "\n", 1u);
                rj_indent(w, indent);
                rj_put(w, "// ", 3u);
                rj_str(w, d->desc);
            }
            rj_put(w, "\n", 1u);
            rj_indent(w, indent);
            str_copy(kbuf, sizeof(kbuf), k);
            rj_put(w, "\"", 1u);
            rj_str(w, kbuf);
            rj_put(w, "\": ", 3u);
            rj_scalar(w, &sorted[i]);
        }
        i++;
    }
}

static long render_json(const iv_config_t *c, uint64_t version)
{
    cfg_item_t sorted[IV_CFG_ITEMS_MAX];
    rj_t       w;
    char       head[256];
    unsigned   cls;
    int        n;

    if (c->n_items > (unsigned)IV_CFG_ITEMS_MAX)
        return IV_ERANGE;

    memcpy(sorted, c->items, c->n_items * sizeof(cfg_item_t));
    qsort(sorted, c->n_items, sizeof(cfg_item_t), item_cmp);

    w.off = 0u;
    w.err = 0;

    rj_str(&w, "{\n");
    /* `_meta` 恒在最前 ⇒ 后面每写一个分类都要**先补一个逗号**。曾经的写法是
     * "第一个分类不加逗号"，结果 `_meta` 与第一个分类之间缺逗号 ⇒ 整份文件
     * 非法 JSON（json-c 报 "object value separator ',' expected"）。有往返单测钉死。 */
    n = snprintf(head, sizeof(head),
                 "  \"_meta\": { \"schema\": %u, \"version\": %llu, \"crc\": %u }",
                 (unsigned)c->schema, (unsigned long long)version, (unsigned)items_crc(c));
    if (n > 0)
        rj_put(&w, head, (size_t)n);

    for (cls = 0u; cls < IV_CFG_CLASS_N; cls++) {
        const char *ck = s_classes[cls].key;
        size_t      cl = strlen(ck);
        unsigned    lo = c->n_items, hi = 0u;
        unsigned    i;

        /* sorted 按键字典序 ⇒ 同一顶层前缀的成员必连续 */
        for (i = 0u; i < c->n_items; i++) {
            if (strncmp(sorted[i].key, ck, cl) == 0 && sorted[i].key[cl] == '.') {
                if (lo == c->n_items)
                    lo = i;
                hi = i + 1u;
            }
        }
        if (lo >= hi)
            continue;

        /* 分类之间、以及 `_meta` 与第一个分类之间都靠这个逗号分隔 */
        rj_put(&w, ",", 1u);

        rj_str(&w, "\n  // ");
        rj_str(&w, s_classes[cls].desc);
        rj_put(&w, "\n  \"", 4u);
        rj_str(&w, ck);
        rj_put(&w, "\": {", 4u);
        rj_object(&w, sorted, lo, hi, ck, 4u);
        rj_put(&w, "\n  }", 4u);
    }

    /* 任何一级分类都没成员时也要保证 `_meta` 后有合法收尾（逗号已按需处理） */
    rj_put(&w, "\n}\n", 3u);
    if (w.err)
        return IV_ERANGE;

    s_render_buf[w.off] = '\0';
    return (long)w.off;
}

static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    if (fsync(fd) != 0) {
        (void)close(fd);
        return -1;
    }
    (void)close(fd);
    return 0;
}

/* 抽干进程自己产生的 inotify 事件（保存后调用，避免 F6 把自己的写当外部改动） */
static void watch_drain(iv_config_t *c)
{
    char buf[512];

    if (c->ifd < 0)
        return;
    while (read(c->ifd, buf, sizeof(buf)) > 0)
        ;
}

/* ---------------------------------------------------------------------------
 * 公开 API：F1
 * ------------------------------------------------------------------------- */

iv_config_t *iv_config_open(const char *dir, const char *name)
{
    const char  *d = (dir != NULL && dir[0] != '\0') ? dir : IV_CFG_DIR_DEFAULT;
    iv_config_t *c = NULL;
    unsigned     i;
    int          rc;

    if (!name_ok(name))
        return NULL;

    /* §S8.1 单句柄约束：同一份 <dir>/<name> 只允许一个活跃句柄。
     * 单文件下两个句柄各持一份内存副本 ⇒ A 改了 B 看不见、B 一 save 又把 A 的
     * 改动覆盖掉。这一步必须在**分槽之前**做，否则会白白占掉一个槽。 */
    for (i = 0; i < (unsigned)IV_CFG_HANDLES; i++) {
        if (s_pool[i].used && strcmp(s_pool[i].name, name) == 0 &&
            strcmp(s_pool[i].dir, d) == 0) {
            IV_LOG_E(CFG_MOD, "'%s' under '%s' is already open (single-handle rule)", name, d);
            return NULL;
        }
    }

    for (i = 0; i < (unsigned)IV_CFG_HANDLES; i++) {
        if (!s_pool[i].used) {
            c = &s_pool[i];
            break;
        }
    }
    if (c == NULL) {
        IV_LOG_E(CFG_MOD, "no free handle for '%s'", name);
        return NULL;
    }

    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->ifd  = -1;
    c->wd   = -1;
    c->schema = IV_CFG_SCHEMA_DEFAULT;

    if (strlen(d) + 1u > (size_t)IV_CFG_PATH_MAX || path_join(c->dir, sizeof(c->dir), d, "") != 0) {
        memset(c, 0, sizeof(*c));
        c->ifd = -1;
        return NULL;
    }
    /* 去掉 path_join(dir,"") 留下的尾斜杠（根目录 "/" 本身除外） */
    {
        size_t dl = strlen(c->dir);

        if (dl > 1u && c->dir[dl - 1u] == '/')
            c->dir[dl - 1u] = '\0';
    }
    str_copy(c->name, sizeof(c->name), name);

    {
        char   file[IV_CFG_NAME_MAX + 8u]; /* "<name>.json" */
        size_t nl = strlen(name);

        /* 手工拼装而非 snprintf：-Wformat-truncation 在 -Werror 下会因"目标可能
         * 放不下"直接断编译（本文件统一走这个口径）。 */
        memcpy(file, name, nl);
        memcpy(file + nl, ".json", 6u); /* 含结尾 NUL */

        if (path_join(c->path, sizeof(c->path), c->dir, file) != 0) {
            memset(c, 0, sizeof(*c));
            c->ifd = -1;
            return NULL;
        }
        /* 临时文件与目标同目录：rename 必须落在同一文件系统内才是原子的
         * （跨文件系统 rename 直接 EXDEV 失败）。 */
        if (strlen(c->path) + 4u + 1u > (size_t)IV_CFG_PATH_MAX) {
            memset(c, 0, sizeof(*c));
            c->ifd = -1;
            return NULL;
        }
        {
            size_t pl = strlen(c->path);

            memcpy(c->tmppath, c->path, pl);
            memcpy(c->tmppath + pl, ".tmp", 5u); /* 含结尾 NUL */
        }
    }

    /* 目录按需创建：F4 要求"文件不存在也照常启动"，补完默认要有地方落盘 */
    if (mkdir(c->dir, 0755) != 0 && errno != EEXIST)
        IV_LOG_W(CFG_MOD, "mkdir '%s' failed: %s (defaults stay in memory)", c->dir,
                 strerror(errno));

    rc = load_from_disk(c);
    if (rc == IV_OK) {
        c->loaded = 1;
        IV_LOG_I(CFG_MOD, "%s: loaded %u keys (invalid=%d), version=%llu", c->name, c->n_items,
                 c->invalid, (unsigned long long)c->version);
    } else {
        /* 文件不在 / 空 / 坏 / 无权限：一律用内置默认值照常启动 */
        c->loaded       = 0;
        c->crc_mismatch = 0;
        c->invalid      = 0;
        c->version      = 0u;
        c->n_items      = 0u;
        (void)fill_missing_defaults(c);
        (void)cross_fix_by_default(c);
        IV_LOG_W(CFG_MOD, "%s: '%s' unusable (rc=%d), start with %u default keys", c->name,
                 c->path, rc, c->n_items);
    }
    return c;
}

void iv_config_close(iv_config_t *c)
{
    if (c == NULL || !c->used)
        return;
    if (c->ifd >= 0) {
        if (c->wd >= 0)
            (void)inotify_rm_watch(c->ifd, c->wd);
        (void)close(c->ifd);
    }
    memset(c, 0, sizeof(*c));
    c->ifd = -1;
}

/* ---------------------------------------------------------------------------
 * 公开 API：F2
 * ------------------------------------------------------------------------- */

int iv_config_get_str(const iv_config_t *c, const char *key, const char **out)
{
    const cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || out == NULL)
        return IV_EINVAL;
    it = item_find(c, key);
    if (it == NULL)
        return IV_ENOENT;
    if (it->type != IV_CFG_T_STR)
        return IV_EPROTO;
    *out = it->v.s;
    return IV_OK;
}

int iv_config_get_int(const iv_config_t *c, const char *key, int64_t *out)
{
    const cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || out == NULL)
        return IV_EINVAL;
    it = item_find(c, key);
    if (it == NULL)
        return IV_ENOENT;
    if (it->type != IV_CFG_T_INT)
        return IV_EPROTO;
    *out = it->v.i;
    return IV_OK;
}

int iv_config_get_bool(const iv_config_t *c, const char *key, int *out)
{
    const cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || out == NULL)
        return IV_EINVAL;
    it = item_find(c, key);
    if (it == NULL)
        return IV_ENOENT;
    if (it->type != IV_CFG_T_BOOL)
        return IV_EPROTO;
    *out = it->v.b;
    return IV_OK;
}

int iv_config_get_dbl(const iv_config_t *c, const char *key, double *out)
{
    const cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || out == NULL)
        return IV_EINVAL;
    it = item_find(c, key);
    if (it == NULL)
        return IV_ENOENT;
    if (it->type != IV_CFG_T_DBL)
        return IV_EPROTO; /* 整数键不隐式转 double，见头文件契约 */
    *out = it->v.d;
    return IV_OK;
}

int iv_config_origin(const iv_config_t *c, const char *key, iv_cfg_origin_t *out)
{
    const cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || out == NULL)
        return IV_EINVAL;
    it = item_find(c, key);
    if (it == NULL)
        return IV_ENOENT;
    *out = (iv_cfg_origin_t)it->origin;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 公开 API：F3
 * ------------------------------------------------------------------------- */

int iv_config_set_str(iv_config_t *c, const char *key, const char *val)
{
    cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || val == NULL || !key_ok(key) || key[0] == '_')
        return IV_EINVAL;
    if (strlen(val) >= (size_t)IV_CFG_STR_MAX)
        return IV_ERANGE;
    it = item_touch(c, key);
    if (it == NULL)
        return IV_EFULL;
    it->type   = IV_CFG_T_STR;
    it->origin = (uint8_t)IV_CFG_FROM_FILE;
    str_copy(it->v.s, sizeof(it->v.s), val);
    c->dirty = 1;
    return IV_OK;
}

int iv_config_set_int(iv_config_t *c, const char *key, int64_t val)
{
    cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || !key_ok(key) || key[0] == '_')
        return IV_EINVAL;
    it = item_touch(c, key);
    if (it == NULL)
        return IV_EFULL;
    it->type   = IV_CFG_T_INT;
    it->origin = (uint8_t)IV_CFG_FROM_FILE;
    it->v.i    = val;
    c->dirty   = 1;
    return IV_OK;
}

int iv_config_set_bool(iv_config_t *c, const char *key, int val)
{
    cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || !key_ok(key) || key[0] == '_')
        return IV_EINVAL;
    it = item_touch(c, key);
    if (it == NULL)
        return IV_EFULL;
    it->type   = IV_CFG_T_BOOL;
    it->origin = (uint8_t)IV_CFG_FROM_FILE;
    it->v.b    = val ? 1 : 0;
    c->dirty   = 1;
    return IV_OK;
}

int iv_config_set_dbl(iv_config_t *c, const char *key, double val)
{
    cfg_item_t *it;

    if (c == NULL || !c->used || key == NULL || !key_ok(key) || key[0] == '_')
        return IV_EINVAL;
    if (val != val) /* NaN：JSON 里没有 NaN 字面量，写出去会产不出合法文件 */
        return IV_EINVAL;
    it = item_touch(c, key);
    if (it == NULL)
        return IV_EFULL;
    it->type   = IV_CFG_T_DBL;
    it->origin = (uint8_t)IV_CFG_FROM_FILE;
    it->v.d    = val;
    c->dirty   = 1;
    return IV_OK;
}

int iv_config_array_len(const iv_config_t *c, const char *arr_key, unsigned *n)
{
    unsigned i;
    unsigned max = 0u;
    size_t   al;

    if (c == NULL || !c->used || arr_key == NULL || n == NULL || !key_ok(arr_key))
        return IV_EINVAL;
    al = strlen(arr_key);

    for (i = 0u; i < c->n_items; i++) {
        const char *k = c->items[i].key;
        const char *np;
        const char *nd;
        size_t      nl;
        unsigned    idx = 0u;

        /* 必须是 arr_key + "." + <纯数字下标> + ... 的形态 */
        if (strncmp(k, arr_key, al) != 0 || k[al] != '.')
            continue;
        np = k + al + 1u;
        nd = strchr(np, '.');
        nl = (nd != NULL) ? (size_t)(nd - np) : strlen(np);
        if (!seg_is_index(np, nl, &idx))
            continue;
        if (idx + 1u > max)
            max = idx + 1u;
    }
    if (max > (unsigned)IV_CFG_ARR_MAX)
        max = (unsigned)IV_CFG_ARR_MAX;
    *n = max;
    return IV_OK;
}

uint64_t iv_config_version(const iv_config_t *c)
{
    return (c != NULL && c->used) ? c->version : 0u;
}

int iv_config_stat(const iv_config_t *c, iv_cfg_stat_t *out)
{
    unsigned i;

    if (c == NULL || !c->used || out == NULL)
        return IV_EINVAL;
    memset(out, 0, sizeof(*out));
    out->schema       = c->schema;
    out->version      = c->version;
    out->items        = c->n_items;
    out->loaded       = c->loaded;
    out->crc_mismatch = c->crc_mismatch;
    out->dirty        = c->dirty;
    out->invalid      = (uint32_t)c->invalid;
    for (i = 0; i < c->n_items; i++) {
        if (c->items[i].origin == (uint8_t)IV_CFG_FROM_FILE)
            out->from_file++;
        else
            out->from_default++;
    }
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 公开 API：F3 + F5 保存
 * ------------------------------------------------------------------------- */

int iv_config_save(iv_config_t *c, char *detail, size_t detail_cap)
{
    const char *bad = NULL;
    const char *why = NULL;
    const char *k1  = NULL;
    const char *k2  = NULL;
    long        n;
    uint64_t    nver;
    int         fd;

    if (c == NULL || !c->used)
        return IV_EINVAL;
    if (detail != NULL && detail_cap > 0u)
        detail[0] = '\0';

    /* 1) 整体校验：任一条不过，一个字段都不落盘 */
    if (!items_check(c, &bad, &why)) {
        if (detail != NULL && detail_cap > 0u)
            snprintf(detail, detail_cap, "%s: %s", bad, why);
        IV_LOG_E(CFG_MOD, "%s: save refused, '%s': %s", c->name, bad, why);
        return IV_EINVAL;
    }
    if (!cross_check(c, &k1, &k2, &why)) {
        if (detail != NULL && detail_cap > 0u)
            snprintf(detail, detail_cap, "cross-field %s ('%s' vs '%s')", why, k1, k2);
        IV_LOG_E(CFG_MOD, "%s: save refused, cross-field: %s", c->name, why);
        return IV_EINVAL;
    }

    /* 2) 渲染（版本号先算新值，写进文件的就是新版本） */
    nver = c->version + 1u;
    n    = render_json(c, nver);
    if (n < 0)
        return (int)n;

    /* 3) tmp → fsync → rename → fsync(目录) */
    fd = open(c->tmppath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        IV_LOG_E(CFG_MOD, "%s: open tmp failed: %s", c->name, strerror(errno));
        return IV_EIO;
    }
    (void)fchmod(fd, 0644); /* 不受 umask 影响：配置非机密，固定 0644 */
    {
        size_t off = 0u;
        int    rc  = IV_OK;

        while (off < (size_t)n) {
            ssize_t w = write(fd, s_render_buf + off, (size_t)n - off);

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = (errno == ENOSPC) ? IV_ENOSPC : IV_EIO;
                break;
            }
            off += (size_t)w;
        }
        if (rc == IV_OK && fsync(fd) != 0)
            rc = IV_EIO;
        (void)close(fd);
        if (rc != IV_OK) {
            (void)unlink(c->tmppath);
            IV_LOG_E(CFG_MOD, "%s: write tmp failed", c->name);
            return rc;
        }
    }
    if (rename(c->tmppath, c->path) != 0) {
        IV_LOG_E(CFG_MOD, "%s: rename failed: %s", c->name, strerror(errno));
        (void)unlink(c->tmppath);
        return IV_EIO;
    }
    if (fsync_dir(c->dir) != 0)
        IV_LOG_W(CFG_MOD, "%s: fsync dir failed: %s (rename may not be durable)", c->name,
                 strerror(errno));

    c->version = nver;
    c->loaded  = 1;
    c->dirty   = 0;
    c->crc_mismatch = 0;
    watch_drain(c); /* 自己的写不该被 F6 当成外部改动 */
    IV_LOG_I(CFG_MOD, "%s: saved %u keys, version=%llu", c->name, c->n_items,
             (unsigned long long)c->version);

    /* F7：程序主动保存同样要通知依赖方（此时配置一定合法） */
    {
        unsigned i;

        for (i = 0; i < c->n_subs; i++) {
            if (c->subs_fn[i] != NULL)
                c->subs_fn[i](c, c->name, c->subs_user[i]);
        }
    }
    return IV_OK;
}

int iv_config_fill_defaults(iv_config_t *c, int *filled, char *detail, size_t detail_cap)
{
    int n;

    if (c == NULL || !c->used)
        return IV_EINVAL;
    n = fill_missing_defaults(c);
    if (filled != NULL)
        *filled = n;
    if (n > 0)
        c->dirty = 1;
    return iv_config_save(c, detail, detail_cap);
}

/* ---------------------------------------------------------------------------
 * 公开 API：F6 热更新
 * ------------------------------------------------------------------------- */

int iv_config_watch_fd(iv_config_t *c)
{
    int fd, wd, saved;

    if (c == NULL || !c->used)
        return IV_EINVAL;
    if (c->ifd >= 0)
        return c->ifd;

    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        IV_LOG_E(CFG_MOD, "%s: inotify_init1 failed: %s", c->name, strerror(errno));
        return IV_EFAIL;
    }
    /* 必须 watch 目录：原子写走 rename，对旧文件的 watch 第一次保存后就失效
     * （计划 §S8 坑 2）。IN_CLOSE_WRITE 管"原地改写"，IN_MOVED_TO 管 rename。 */
    wd = inotify_add_watch(fd, c->dir, IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) {
        saved = errno;
        (void)close(fd);
        IV_LOG_E(CFG_MOD, "%s: inotify_add_watch '%s' failed: %s", c->name, c->dir,
                 strerror(saved));
        return (saved == ENOENT) ? IV_ENOENT : IV_EFAIL;
    }
    c->ifd = fd;
    c->wd  = wd;
    watch_drain(c); /* 建 watch 后的历史事件不算改动 */
    IV_LOG_I(CFG_MOD, "%s: watching '%s'", c->name, c->dir);
    return c->ifd;
}

int iv_config_watch_poll(iv_config_t *c, uint32_t *reloads)
{
    char  buf[sizeof(struct inotify_event) + IV_CFG_NAME_MAX + 8u];
    char  target[IV_CFG_NAME_MAX + 8u];
    int   hit = 0;
    int   saw = 0;

    if (c == NULL || !c->used)
        return IV_EINVAL;
    if (c->ifd < 0)
        return IV_ESTATE;

    snprintf(target, sizeof(target), "%s.json", c->name);

    for (;;) {
        ssize_t n = read(c->ifd, buf, sizeof(buf));
        char   *p;
        ssize_t left;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break; /* EAGAIN：本轮读干 */
        }
        if (n == 0)
            break;
        saw = 1;

        for (p = buf, left = n; left >= (ssize_t)sizeof(struct inotify_event);) {
            const struct inotify_event *ev = (const struct inotify_event *)p;

            if (ev->len > 0u && strcmp(ev->name, target) == 0)
                hit = 1;
            p += sizeof(struct inotify_event) + ev->len;
            left -= (ssize_t)(sizeof(struct inotify_event) + ev->len);
        }
    }

    if (!saw)
        return IV_EAGAIN;
    if (!hit) {
        if (reloads != NULL)
            *reloads = 0u;
        return IV_OK; /* 目录里有别的文件动了：与本配置无关 */
    }

    /* 事务式重载，**粒度＝按一级分类子树**（§S8.1 / 架构 §10.2）。
     *
     * 步骤：
     *   1) 整份解析到内存（load_from_disk 会把"哪个分类出现非法值"记进 c->cls_bad）；
     *   2) 解析失败（语法坏 / 截断 / 顶层非对象）⇒ **整份拒绝**，旧值一字不动 ——
     *      此时连"哪些键属于哪个分类"都拿不到，只能全拒；
     *   3) 解析成功 ⇒ 对**每个一级分类**分别决定：
     *        - 该分类没被标 bad ⇒ 采用新值；
     *        - 该分类被标 bad   ⇒ 从旧快照把该分类的条目整块搬回来（回退旧值）。
     *      注意：这里**不把"缺键"当非法** —— 外部手工编辑删掉几个键是正常操作，
     *      fill_missing_defaults 已按默认值补齐，属采用。
     *   4) 只要**至少一个分类采用了新值**就通知订阅者（F7）；全被拒则一个都不通知。
     *      版本号沿用文件里的值（若文件没写 version，则沿用旧版本号，见下）。 */
    {
        cfg_snapshot_t snap;
        int            rc;
        unsigned       adopted = 0u;

        snap_take(c, &snap);
        rc = load_from_disk(c);
        if (rc != IV_OK) {
            snap_restore(c, &snap);
            IV_LOG_W(CFG_MOD, "%s: reload rejected (rc=%d, unparsable), keep previous", c->name,
                     rc);
            if (reloads != NULL)
                *reloads = 0u;
            return IV_OK;
        }

        /* 逐分类：被标 bad 的分类整块回退旧值 */
        if (c->cls_bad != 0u) {
            unsigned cls;

            for (cls = 0u; cls < IV_CFG_CLASS_N; cls++) {
                const char *ck = s_classes[cls].key;
                size_t      cl = strlen(ck);
                unsigned    i;

                if ((c->cls_bad & (1u << cls)) == 0u)
                    continue;

                /* ① 先删掉新表里属于本分类的条目 */
                for (i = 0u; i < c->n_items;) {
                    if (strncmp(c->items[i].key, ck, cl) == 0 && c->items[i].key[cl] == '.') {
                        memmove(&c->items[i], &c->items[i + 1u],
                                (c->n_items - i - 1u) * sizeof(cfg_item_t));
                        c->n_items--;
                    } else {
                        i++;
                    }
                }
                /* ② 再把旧快照里本分类的条目搬回来 */
                for (i = 0u; i < snap.n_items; i++) {
                    if (strncmp(snap.items[i].key, ck, cl) == 0 && snap.items[i].key[cl] == '.') {
                        cfg_item_t *dst = item_touch(c, snap.items[i].key);

                        if (dst != NULL)
                            *dst = snap.items[i];
                    }
                }
                IV_LOG_W(CFG_MOD, "%s: reload: class '%s' rejected (invalid value), kept old",
                         c->name, ck);
            }
        }

        /* 统计采用新值的分类数（用于 reloads 返回与是否需要通知） */
        {
            unsigned cls;

            for (cls = 0u; cls < IV_CFG_CLASS_N; cls++) {
                if ((c->cls_bad & (1u << cls)) == 0u) {
                    const char *ck = s_classes[cls].key;
                    size_t      cl = strlen(ck);
                    unsigned    i;

                    for (i = 0u; i < c->n_items; i++) {
                        if (strncmp(c->items[i].key, ck, cl) == 0 && c->items[i].key[cl] == '.') {
                            adopted++;
                            break;
                        }
                    }
                }
            }
        }

        if (adopted == 0u) {
            /* 一个分类都没采用（例如文件里全是坏分类）⇒ 不算重载、不通知 */
            IV_LOG_W(CFG_MOD, "%s: reload adopted nothing, subscribers not notified", c->name);
            if (reloads != NULL)
                *reloads = 0u;
            return IV_OK;
        }

        c->loaded       = 1;
        c->crc_mismatch = 0; /* 重载后重新渲染会刷新 crc，不再报旧文件的 crc 不符 */
        IV_LOG_I(CFG_MOD, "%s: reloaded %u keys (%u class(es) adopted), version=%llu", c->name,
                 c->n_items, adopted, (unsigned long long)c->version);
        {
            unsigned i;

            for (i = 0; i < c->n_subs; i++) {
                if (c->subs_fn[i] != NULL)
                    c->subs_fn[i](c, c->name, c->subs_user[i]);
            }
        }
        if (reloads != NULL)
            *reloads = adopted;
        return IV_OK;
    }
}

/* ---------------------------------------------------------------------------
 * 公开 API：F7 订阅
 * ------------------------------------------------------------------------- */

int iv_config_subscribe(iv_config_t *c, iv_cfg_notify_fn fn, void *user)
{
    if (c == NULL || !c->used || fn == NULL)
        return IV_EINVAL;
    if (c->n_subs >= (unsigned)IV_CFG_SUBS_MAX)
        return IV_EFULL;
    c->subs_fn[c->n_subs]   = fn;
    c->subs_user[c->n_subs] = user;
    c->n_subs++;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 公开 API：默认值查询
 * ------------------------------------------------------------------------- */

int iv_config_default_get(const char *name, const char *key, iv_cfg_type_t *out_type,
                          char *out_text, size_t cap)
{
    const cfg_default_t *d;

    (void)name; /* 单文件口径：只有一份配置，name 参数保留只为接口兼容 */
    if (name == NULL || key == NULL)
        return IV_EINVAL;
    d = default_find(key);
    if (d == NULL)
        return IV_ENOENT;
    if (out_type != NULL)
        *out_type = d->type;
    if (out_text != NULL && cap > 0u) {
        if (d->type == IV_CFG_T_STR)
            str_copy(out_text, cap, d->sdef);
        else if (d->type == IV_CFG_T_INT)
            snprintf(out_text, cap, "%lld", (long long)d->idef);
        else if (d->type == IV_CFG_T_DBL)
            snprintf(out_text, cap, "%.17g", d->ddef);
        else
            snprintf(out_text, cap, "%d", (strcmp(d->sdef, "true") == 0) ? 1 : 0);
    }
    return IV_OK;
}
