/*
 * iv_config 单测（libivmodules，功能开发计划 M1-S8 + §S8.1）
 *
 * 覆盖计划 §S8 的七个功能 F1~F7，以及 §S8.1 的五项新增/变更能力：
 *   ① 单文件口径（`<dir>/ivsbox.json`）与八个一级分类；
 *   ② 单句柄约束（第二次 open 同名必须返回 NULL）；
 *   ③ JSON 数组（camera.ch，元素＝条目，下标越界/空洞判非法）；
 *   ④ double（elec.* / sensor.* 物理量阈值，类型严格不隐式转换）；
 *   ⑤ 中文注释渲染 + **渲染→再解析** 往返等值；
 *   ⑥ 热更新事务粒度＝**按一级分类子树**（坏分类单独回退，其余分类照常采用）。
 *
 * 每个功能都配了**能反向证伪**的断言 —— 把实现改回缺陷态后这条断言必须真的
 * 失败（S7 那轮的教训：只断言"成功路径能跑通"的用例，把实现改坏也照样绿）。
 *
 * 工作目录用 mkdtemp 造在 /tmp 下，退出前把文件与目录全部删掉（AGENTS.md 规则 6）。
 * 日志一律改走空 sink：本模块与 iv_log 都会写盘，不在单测里产生任何落盘副作用。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_config.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_ret.h"

static int  g_fail;
static char g_dir[160]; /* mkdtemp 出来的工作目录 */

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* g_dir + "/" + rel；手工拼装，避免 snprintf 的 -Wformat-truncation 告警 */
static void path_of(char *out, size_t cap, const char *rel)
{
    size_t dl = strlen(g_dir);
    size_t rl = strlen(rel);

    if (dl + 1u + rl + 1u > cap) {
        chk(0, "path_of: buffer too small");
        out[0] = '\0';
        return;
    }
    memcpy(out, g_dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, rel, rl + 1u);
}

/* 写文件并**显式 fchmod**：O_CREAT 的 mode 会被 umask 削，权限相关用例必须可控 */
static int write_file(const char *path, const char *text, unsigned mode)
{
    int    fd  = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)mode);
    size_t len = strlen(text);
    size_t off = 0u;

    if (fd < 0)
        return -1;
    if (fchmod(fd, (mode_t)mode) != 0) {
        (void)close(fd);
        return -1;
    }
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    (void)close(fd);
    return 0;
}

static long read_file(const char *path, char *buf, size_t cap)
{
    int    fd   = open(path, O_RDONLY | O_CLOEXEC);
    size_t used = 0u;

    if (fd < 0)
        return -1;
    for (;;) {
        ssize_t n;

        if (used >= cap - 1u)
            break;
        n = read(fd, buf + used, cap - 1u - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            (void)close(fd);
            return -1;
        }
        if (n == 0)
            break;
        used += (size_t)n;
    }
    (void)close(fd);
    buf[used] = '\0';
    return (long)used;
}

static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* F7 订阅者计数 */
struct notify_ctx {
    int  count;
    char last[IV_CFG_NAME_MAX];
};

static void on_notify(iv_config_t *c, const char *name, void *user)
{
    struct notify_ctx *n = (struct notify_ctx *)user;

    (void)c;
    n->count++;
    if (name != NULL) {
        size_t l = strlen(name);

        if (l >= sizeof(n->last))
            l = sizeof(n->last) - 1u;
        memcpy(n->last, name, l);
        n->last[l] = '\0';
    }
}

static void quiet_sink(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

/* 唯一的配置文件路径（单文件口径） */
static void cfg_path(char *out, size_t cap)
{
    path_of(out, cap, "ivsbox.json");
}

/* 把整份配置强制读成一个新句柄，用于"重新读盘核对"；调用方负责 close */
static iv_config_t *reopen_cfg(void)
{
    return iv_config_open(g_dir, IV_CFG_NAME);
}

/* 打开、跑一段、关掉；避免每个用例都写一遍 open/close 的样板 */
#define WITH_CFG(c) for (c = reopen_cfg(); c != NULL; c = NULL)

/* ===========================================================================
 * §S8.1 ① + F1/F4：文件根本不存在 ⇒ 用内置默认值照常启动
 * 新口径：单文件、八分类、128 容量上限。默认表共 5+11+5+49+3+5+4+5 = 87 项。
 * 反向证伪：若默认值表被清空 / 补齐逻辑不跑，items 与各默认值断言立刻失败。
 * ========================================================================= */
static void case_defaults_startup(void)
{
    char           pf[200];
    iv_config_t   *c;
    iv_cfg_stat_t  st;
    iv_cfg_origin_t org;
    const char    *s  = NULL;
    int64_t        iv = 0;
    int            bv = 0;
    double         dv = 0.0;

    cfg_path(pf, sizeof(pf));
    (void)unlink(pf); /* 确保文件不存在 */

    c = iv_config_open(g_dir, IV_CFG_NAME);
    chk(c != NULL, "c1: open without a file still returns a handle");
    if (c == NULL)
        return;

    chk(iv_config_stat(c, &st) == IV_OK, "c1: stat");
    chk(st.loaded == 0, "c1: loaded == 0 (started from built-in defaults)");
    /* 8 个一级分类的全部默认键：sys 5 + net 11 + probe 5 + camera 49 +
     * switch 3 + elec 5 + netthr 4 + sensor 5 = 87 */
    chk(st.items == 87u, "c1: all 87 built-in defaults materialised");
    chk(st.from_file == 0u, "c1: nothing came from the file");
    chk(st.from_default == 87u, "c1: everything came from defaults");
    chk(st.invalid == 0u, "c1: a missing file is NOT an invalid value");
    chk(st.version == 0u, "c1: version is 0 when there is no file");
    chk(st.dirty == 0, "c1: not dirty right after open");

    /* 每个分类都抽查一个键，证明八棵树都补出来了 */
    chk(iv_config_get_int(c, "sys.transport.mode", &iv) == IV_OK && iv == 4,
        "c1: sys.transport.mode default 4");
    chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && s[0] == '\0',
        "c1: net.ip default empty (DHCP)");
    chk(iv_config_get_int(c, "probe.interval_ms", &iv) == IV_OK && iv == 5000,
        "c1: probe.interval_ms default 5000");
    chk(iv_config_get_str(c, "camera.search", &s) == IV_OK && strcmp(s, "off") == 0,
        "c1: camera.search default off");
    chk(iv_config_get_bool(c, "camera.ch.0.record", &bv) == IV_OK && bv == 0,
        "c1: camera.ch.0.record default false");
    chk(iv_config_get_str(c, "switch.ip", &s) == IV_OK && s[0] == '\0',
        "c1: switch.ip default empty");
    chk(iv_config_get_dbl(c, "elec.volt_high", &dv) == IV_OK && dv == 0.0,
        "c1: elec.volt_high is a double (factory 0.0 = unset)");
    chk(iv_config_get_dbl(c, "sensor.temp_high", &dv) == IV_OK && dv == 0.0,
        "c1: sensor.temp_high is a double (factory 0.0 = unset)");
    chk(iv_config_get_int(c, "netthr.loss_count", &iv) == IV_OK && iv == 3,
        "c1: netthr.loss_count default 3");

    chk(iv_config_origin(c, "sys.transport.mode", &org) == IV_OK && org == IV_CFG_FROM_DEFAULT,
        "c1: origin reports DEFAULT");
    chk(iv_config_origin(c, "no.such.key", &org) == IV_ENOENT, "c1: origin of a missing key");

    /* 一级分类查询（§S8.1 的事务粒度就靠它） */
    chk(iv_config_class_of("sys.debug.mode") == IV_CFG_CLS_SYS, "c1: class_of sys");
    chk(iv_config_class_of("net.ip") == IV_CFG_CLS_NET, "c1: class_of net");
    chk(iv_config_class_of("probe.target1") == IV_CFG_CLS_PROBE, "c1: class_of probe");
    chk(iv_config_class_of("camera.ch.3.vendor") == IV_CFG_CLS_CAMERA, "c1: class_of camera");
    chk(iv_config_class_of("switch.model") == IV_CFG_CLS_SWITCH, "c1: class_of switch");
    chk(iv_config_class_of("elec.power") == IV_CFG_CLS_ELEC, "c1: class_of elec");
    chk(iv_config_class_of("netthr.reboot_max") == IV_CFG_CLS_NETTHR, "c1: class_of netthr");
    chk(iv_config_class_of("sensor.humi_low") == IV_CFG_CLS_SENSOR, "c1: class_of sensor");
    chk(iv_config_class_of("unknown.top") == -1, "c1: an unknown top level has no class");
    chk(iv_config_class_of(NULL) == -1, "c1: class_of(NULL)");

    /* 内置默认表的只读查询（给 Web / 诊断用） */
    {
        iv_cfg_type_t t   = IV_CFG_T_BOOL;
        char          txt[32];

        chk(iv_config_default_get(IV_CFG_NAME, "sys.transport.mode", &t, txt, sizeof(txt)) == IV_OK,
            "c1: default_get on a known key");
        chk(t == IV_CFG_T_INT && strcmp(txt, "4") == 0, "c1: default_get renders '4'");
        chk(iv_config_default_get(IV_CFG_NAME, "sensor.tilt", &t, txt, sizeof(txt)) == IV_OK &&
                t == IV_CFG_T_DBL,
            "c1: default_get reports the double type");
        chk(iv_config_default_get(IV_CFG_NAME, "sys.zzz", &t, txt, sizeof(txt)) == IV_ENOENT,
            "c1: default_get on an unknown key == ENOENT");
        chk(iv_config_default_get(NULL, "x", NULL, NULL, 0u) == IV_EINVAL,
            "c1: default_get NULL args");
    }

    iv_config_close(c);
}

/* ===========================================================================
 * F1 + F2 + §S8.1 ④：读一份真文件，四种类型都取得到；类型不符必须明确报错
 * 反向证伪：① 若 get_dbl 接受整数键（或反之），两条"类型严格"断言立刻失败；
 *           ② 若 item_find 改成"找不到返回第一个"，ENOENT 断言会失败。
 * ========================================================================= */
static void case_read_typed(void)
{
    static const char JSON[] =
        "{\n"
        "  \"_meta\": { \"schema\": 3, \"version\": 7 },\n"
        "  \"sys\": { \"transport\": { \"mode\": 2 }, \"debug\": { \"mode\": true } },\n"
        "  \"net\": { \"ip\": \"10.0.0.5\" },\n"
        "  \"elec\": { \"volt_high\": 60.5 },\n"
        "  \"feature\": { \"enabled\": true, \"name\": \"x y\" }\n"
        "}\n";
    char            pf[200];
    iv_config_t    *c;
    iv_cfg_stat_t   st;
    iv_cfg_origin_t org;
    const char     *s  = NULL;
    int64_t         iv = 0;
    int             bv = 0;
    double          dv = 0.0;

    cfg_path(pf, sizeof(pf));
    chk(write_file(pf, JSON, 0644u) == 0, "c2: seed ivsbox.json");

    c = reopen_cfg();
    chk(c != NULL, "c2: open");
    if (c == NULL)
        return;

    chk(iv_config_stat(c, &st) == IV_OK, "c2: stat");
    chk(st.loaded == 1, "c2: loaded == 1");
    chk(st.schema == 3u, "c2: schema read from _meta");
    chk(st.version == 7u, "c2: version read from _meta");
    /* 文件给了 4 个已知键 + 2 个未知键，其余全部按默认补齐 */
    chk(st.invalid == 0u, "c2: no invalid key");
    chk(st.from_file == 6u, "c2: six keys came from the file");
    chk(st.crc_mismatch == 0, "c2: no crc in the file => no mismatch warning");

    chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && strcmp(s, "10.0.0.5") == 0,
        "c2: string value");
    chk(iv_config_get_int(c, "sys.transport.mode", &iv) == IV_OK && iv == 2,
        "c2: integer value");
    chk(iv_config_get_bool(c, "sys.debug.mode", &bv) == IV_OK && bv == 1, "c2: boolean value");
    chk(iv_config_get_dbl(c, "elec.volt_high", &dv) == IV_OK && dv == 60.5,
        "c2: double value");
    chk(iv_config_get_str(c, "feature.name", &s) == IV_OK && strcmp(s, "x y") == 0,
        "c2: unknown key with a space is preserved verbatim");
    chk(iv_config_get_bool(c, "feature.enabled", &bv) == IV_OK && bv == 1,
        "c2: unknown boolean preserved");
    chk(iv_config_origin(c, "feature.name", &org) == IV_OK && org == IV_CFG_FROM_FILE,
        "c2: unknown key origin is FILE");

    /* 类型不符：四类 get 都不得做隐式转换 */
    chk(iv_config_get_int(c, "net.ip", &iv) == IV_EPROTO, "c2: get_int on a string == EPROTO");
    chk(iv_config_get_str(c, "sys.transport.mode", &s) == IV_EPROTO,
        "c2: get_str on an int == EPROTO");
    chk(iv_config_get_int(c, "sys.debug.mode", &iv) == IV_EPROTO,
        "c2: bool is NOT implicitly converted to int");
    chk(iv_config_get_bool(c, "sys.transport.mode", &bv) == IV_EPROTO,
        "c2: get_bool on an int == EPROTO");
    /* §S8.1 ④：double 与 int 严格隔离，两个方向都拒 */
    chk(iv_config_get_dbl(c, "sys.transport.mode", &dv) == IV_EPROTO,
        "c2: get_dbl on an INT key == EPROTO");
    chk(iv_config_get_int(c, "elec.volt_high", &iv) == IV_EPROTO,
        "c2: get_int on a DOUBLE key == EPROTO");
    chk(iv_config_get_str(c, "elec.volt_high", &s) == IV_EPROTO,
        "c2: get_str on a double == EPROTO");

    /* 键不存在 / 参数非法 */
    chk(iv_config_get_str(c, "net.zzz", &s) == IV_ENOENT, "c2: missing key == ENOENT");
    chk(iv_config_get_str(c, "net.ip", NULL) == IV_EINVAL, "c2: NULL out");
    chk(iv_config_get_str(c, NULL, &s) == IV_EINVAL, "c2: NULL key");
    chk(iv_config_get_str(NULL, "net.ip", &s) == IV_EINVAL, "c2: NULL handle");
    chk(iv_config_get_int(c, "sys.transport.mode", NULL) == IV_EINVAL, "c2: NULL int out");
    chk(iv_config_get_dbl(c, "elec.volt_high", NULL) == IV_EINVAL, "c2: NULL dbl out");
    chk(iv_config_get_bool(c, "sys.debug.mode", NULL) == IV_EINVAL, "c2: NULL bool out");

    chk(iv_config_origin(c, "net.ip", &org) == IV_OK && org == IV_CFG_FROM_FILE,
        "c2: origin reports FILE");

    iv_config_close(c);
}

/* ===========================================================================
 * §S8.1 ②：单句柄约束 —— 同一份 <dir>/<name> 第二次 open 必须返回 NULL
 * 反向证伪：去掉 open() 里"分槽前"的单句柄预检后，第二次 open 会拿到一个新句柄，
 *           这条断言立刻失败（而"改了一个模块另一个看不见"的故障就此复活）。
 * ========================================================================= */
static void case_single_handle(void)
{
    iv_config_t *c1;
    iv_config_t *c2;

    c1 = reopen_cfg();
    chk(c1 != NULL, "c3: first open");
    if (c1 == NULL)
        return;

    c2 = reopen_cfg();
    chk(c2 == NULL, "c3: a SECOND handle for the same file is refused");

    iv_config_close(c1);
    /* 关掉之后必须能再开 —— 否则"拒绝"变成了"永久占用" */
    c2 = reopen_cfg();
    chk(c2 != NULL, "c3: after close the slot is reusable");
    iv_config_close(c2);
}

/* ===========================================================================
 * F3 + F5：set 只改内存（此刻磁盘仍是旧值）；save 才落盘且版本号 +1
 * 反向证伪：① 若 set_* 顺手写盘，"save 前磁盘仍是旧值"这条会失败；
 *           ② 若 save 不改版本号 / 多加了 1，版本断言失败。
 * ========================================================================= */
static void case_set_memory_then_save(void)
{
    static const char SEED[] =
        "{\n  \"_meta\": { \"schema\": 1, \"version\": 11 },\n"
        "  \"sys\": { \"transport\": { \"mode\": 2 }, \"report\": { \"interval_s\": 30 } },\n"
        "  \"net\": { \"ip\": \"192.168.1.10\" }\n}\n";
    char          pf[200];
    char          tmpf[200];
    char          buf[2048];
    iv_config_t  *c;
    iv_cfg_stat_t st;
    int64_t       iv = 0;
    double        dv = 0.0;
    uint64_t      v0;

    cfg_path(pf, sizeof(pf));
    path_of(tmpf, sizeof(tmpf), "ivsbox.json.tmp");
    chk(write_file(pf, SEED, 0644u) == 0, "c4: seed ivsbox.json");

    c = reopen_cfg();
    chk(c != NULL, "c4: open");
    if (c == NULL)
        return;

    chk(iv_config_get_int(c, "sys.report.interval_s", &iv) == IV_OK && iv == 30,
        "c4: seeded interval_s == 30");
    v0 = iv_config_version(c);
    chk(v0 == 11u, "c4: version comes from the file");

    /* ---- 改内存（含 double） ---- */
    chk(iv_config_set_int(c, "sys.report.interval_s", 45) == IV_OK, "c4: set_int");
    chk(iv_config_set_str(c, "net.ip", "192.168.1.99") == IV_OK, "c4: set_str");
    chk(iv_config_set_bool(c, "sys.debug.mode", 1) == IV_OK, "c4: set_bool");
    chk(iv_config_set_dbl(c, "elec.volt_high", 58.5) == IV_OK, "c4: set_dbl");
    chk(iv_config_get_int(c, "sys.report.interval_s", &iv) == IV_OK && iv == 45,
        "c4: memory reflects the new int");
    chk(iv_config_get_dbl(c, "elec.volt_high", &dv) == IV_OK && dv == 58.5,
        "c4: memory reflects the new double");
    chk(iv_config_stat(c, &st) == IV_OK && st.dirty == 1, "c4: handle marked dirty");

    /* **关键**：此刻磁盘上还是旧值 —— set_* 不碰磁盘 */
    chk(read_file(pf, buf, sizeof(buf)) > 0, "c4: re-read the file");
    chk(contains(buf, "\"interval_s\": 30"), "c4: file still holds the OLD value before save");
    chk(contains(buf, "192.168.1.10"), "c4: and the OLD ip");
    chk(!exists(tmpf), "c4: no tmp file yet");

    /* ---- save 才落盘 ---- */
    chk(iv_config_save(c, NULL, 0u) == IV_OK, "c4: save");
    chk(iv_config_version(c) == v0 + 1u, "c4: version incremented by exactly 1");
    chk(iv_config_stat(c, &st) == IV_OK && st.dirty == 0, "c4: dirty cleared after save");
    chk(!exists(tmpf), "c4: tmp file consumed by the rename");

    iv_config_close(c);

    /* 另一句柄重新读盘，证明真的落上去了（而不是只改了内存） */
    c = reopen_cfg();
    chk(c != NULL, "c4: second handle");
    if (c != NULL) {
        const char *s = NULL;

        chk(iv_config_get_int(c, "sys.report.interval_s", &iv) == IV_OK && iv == 45,
            "c4: the new int survived a reload from disk");
        chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && strcmp(s, "192.168.1.99") == 0,
            "c4: the new string survived too");
        chk(iv_config_get_dbl(c, "elec.volt_high", &dv) == IV_OK && dv == 58.5,
            "c4: the new double survived (round-trip through the renderer)");
        chk(iv_config_version(c) == v0 + 1u, "c4: the bumped version is on disk");

        /* ---- set_* 的参数与键名校验 ---- */
        chk(iv_config_set_str(c, "_meta.x", "y") == IV_EINVAL, "c4: '_' prefix is reserved");
        chk(iv_config_set_int(c, "a..b", 1) == IV_EINVAL, "c4: empty segment rejected");
        chk(iv_config_set_int(c, ".a", 1) == IV_EINVAL, "c4: leading dot rejected");
        chk(iv_config_set_int(c, "a.", 1) == IV_EINVAL, "c4: trailing dot rejected");
        chk(iv_config_set_int(c, "a.b.c.d.e", 1) == IV_EINVAL, "c4: too many segments rejected");
        chk(iv_config_set_str(c, NULL, "y") == IV_EINVAL, "c4: NULL key");
        chk(iv_config_set_str(c, "ok.key", NULL) == IV_EINVAL, "c4: NULL value");
        chk(iv_config_set_bool(NULL, "ok.key", 1) == IV_EINVAL, "c4: NULL handle");
        chk(iv_config_set_int(c, "ok.key", 1) == IV_OK, "c4: a normal unknown key is settable");

        iv_config_close(c);
    }
}

/* ===========================================================================
 * F4：文件只给一个键，其余必须补齐；来源能区分"文件给的/默认补的"
 * 反向证伪：若 fill_missing_defaults 不跑，from_default 会是 0。
 * ========================================================================= */
static void case_fill_and_origin(void)
{
    static const char PARTIAL[] = "{\n  \"probe\": { \"max_fail\": 4 }\n}\n";
    char            pf[200];
    iv_config_t    *c;
    iv_cfg_stat_t   st;
    iv_cfg_origin_t org;
    int64_t         iv   = 0;
    int             fill = -1;

    cfg_path(pf, sizeof(pf));
    chk(write_file(pf, PARTIAL, 0644u) == 0, "c5: seed a partial ivsbox.json");

    c = reopen_cfg();
    chk(c != NULL, "c5: open");
    if (c == NULL)
        return;

    chk(iv_config_stat(c, &st) == IV_OK, "c5: stat");
    chk(st.items == 87u, "c5: the whole default table is materialised");
    chk(st.from_file == 1u, "c5: exactly one key came from the file");
    chk(st.from_default == 86u, "c5: eighty-six were filled from defaults");
    chk(st.invalid == 0u, "c5: a MISSING key is not an INVALID key");
    chk(st.loaded == 1, "c5: the file itself loaded fine");

    chk(iv_config_get_int(c, "probe.max_fail", &iv) == IV_OK && iv == 4,
        "c5: the file value wins over the default");
    chk(iv_config_origin(c, "probe.max_fail", &org) == IV_OK && org == IV_CFG_FROM_FILE,
        "c5: origin FILE for the key we supplied");
    chk(iv_config_origin(c, "probe.interval_ms", &org) == IV_OK && org == IV_CFG_FROM_DEFAULT,
        "c5: origin DEFAULT for the filled key");

    chk(iv_config_fill_defaults(c, &fill, NULL, 0u) == IV_OK, "c5: fill_defaults");
    chk(fill == 0, "c5: open() already filled everything, so there is nothing left");

    iv_config_close(c);
}

/* ===========================================================================
 * F4：文件把子树写成标量（`"net": 5`）必须判非法并剔除，不能让它把整棵
 *     net.* 默认子树挡在门外。
 * 反向证伪：去掉 item_check() 里的 is_default_parent 分支后，invalid 会退回 0，
 *           而 items 也不再是 87 —— 这正是 F6 静默清空运行中配置的那条路。
 * ========================================================================= */
static void case_parent_scalar_rejected(void)
{
    char          pf[200];
    iv_config_t  *c;
    iv_cfg_stat_t st;
    int64_t       iv = 0;
    const char   *s  = NULL;

    cfg_path(pf, sizeof(pf));
    chk(write_file(pf, "{ \"net\": 5 }\n", 0644u) == 0, "c6: seed {\"net\": 5}");

    c = reopen_cfg();
    chk(c != NULL, "c6: open");
    if (c == NULL)
        return;

    chk(iv_config_stat(c, &st) == IV_OK, "c6: stat");
    chk(st.invalid == 1u, "c6: the scalar that shadows a subtree is counted as invalid");
    chk(st.items == 87u, "c6: the whole net.* default subtree is still there");
    chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && s[0] == '\0',
        "c6: net.* is usable again (defaulted empty)");
    chk(iv_config_get_int(c, "net", &iv) == IV_ENOENT, "c6: the offending key itself is gone");

    iv_config_close(c);
}

/* ===========================================================================
 * F5：抗断电写盘的两条可测性质
 *   ① 崩在半截的 `.tmp` 既不会被当成正式文件读，也不会在下次 save 后残留；
 *   ② save 走的是 rename 换 inode，不是原地改写。
 * 反向证伪：若实现改成 O_TRUNC 直接写正式文件，旧 fd 会立刻看到新内容。
 * ========================================================================= */
static void case_atomic_replace(void)
{
    static const char SEED[] =
        "{\"_meta\":{\"version\":2},\"net\":{\"ip\":\"OLD\"}}\n";
    char         pf[200];
    char         tmpf[200];
    char         buf[2048];
    char         oldbuf[1024];
    iv_config_t *c;
    int          old_fd;
    ssize_t      n;

    cfg_path(pf, sizeof(pf));
    path_of(tmpf, sizeof(tmpf), "ivsbox.json.tmp");
    chk(write_file(pf, SEED, 0644u) == 0, "c7: seed ivsbox.json");
    /* 崩在写一半的现场：只剩一个半截 .tmp */
    chk(write_file(tmpf, "{ this is a half-written leftover", 0644u) == 0, "c7: seed a stale tmp");

    c = reopen_cfg();
    chk(c != NULL, "c7: open");
    if (c == NULL)
        return;
    {
        const char *s = NULL;

        chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && strcmp(s, "OLD") == 0,
            "c7: a half-written tmp is NOT used as the config");
    }

    old_fd = open(pf, O_RDONLY | O_CLOEXEC);
    chk(old_fd >= 0, "c7: hold the pre-save inode open");

    chk(iv_config_set_str(c, "net.ip", "NEW") == IV_OK, "c7: set ip=NEW");
    chk(iv_config_save(c, NULL, 0u) == IV_OK, "c7: save");

    chk(read_file(pf, buf, sizeof(buf)) > 0 && contains(buf, "NEW") && !contains(buf, "OLD"),
        "c7: the path now holds the new content");

    if (old_fd >= 0) {
        n = read(old_fd, oldbuf, sizeof(oldbuf) - 1u);
        if (n < 0)
            n = 0;
        oldbuf[n] = '\0';
        chk(contains(oldbuf, "OLD") && !contains(oldbuf, "NEW"),
            "c7: the pre-save inode still holds the OLD content (rename, not in-place write)");
        (void)close(old_fd);
    }
    chk(!exists(tmpf), "c7: the stale tmp is consumed and gone after a successful save");

    iv_config_close(c);
}

/* ===========================================================================
 * F3 + F5 + §S8.1 ④：任何校验不过 ⇒ 一个字段都不落盘，文件逐字节不变
 * 反向证伪：若 save 先写后校验（或边写边校验），文件会被改坏，逐字节比较失败。
 * ========================================================================= */
static void case_reject_writes_nothing(void)
{
    static const char SEED[] =
        "{\"_meta\":{\"version\":5},\"probe\":{\"interval_ms\":5000,\"timeout_ms\":2000},\n"
        " \"camera\":{\"search\":\"off\"}}\n";
    char         pf[200];
    char         tmpf[200];
    char         before[2048];
    char         after[2048];
    char         detail[IV_CFG_DETAIL_MAX];
    iv_config_t *c;
    int64_t      iv = 0;

    cfg_path(pf, sizeof(pf));
    path_of(tmpf, sizeof(tmpf), "ivsbox.json.tmp");
    chk(write_file(pf, SEED, 0644u) == 0, "c8: seed ivsbox.json");

    c = reopen_cfg();
    chk(c != NULL, "c8: open");
    if (c == NULL)
        return;
    chk(read_file(pf, before, sizeof(before)) > 0, "c8: snapshot the file");

    /* ---- 范围越界（整数） ---- */
    chk(iv_config_set_int(c, "probe.max_fail", 0) == IV_OK, "c8: set an out-of-range value");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL, "c8: save refuses it");
    chk(contains(detail, "probe.max_fail"), "c8: detail names the offending key");
    chk(read_file(pf, after, sizeof(after)) > 0 && strcmp(before, after) == 0,
        "c8: the rejected save left the file byte-identical");
    chk(!exists(tmpf), "c8: and left no tmp behind");

    /* ---- 范围越界（double，§S8.1 ④） ---- */
    chk(iv_config_set_int(c, "probe.max_fail", 3) == IV_OK, "c8: restore max_fail");
    chk(iv_config_set_dbl(c, "sensor.humi_low", 150.0) == IV_OK, "c8: set a double out of range");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL, "c8: save refuses an out-of-range double");
    chk(contains(detail, "sensor.humi_low"), "c8: detail names the double key");
    chk(iv_config_set_dbl(c, "sensor.humi_low", 10.0) == IV_OK, "c8: fix the double");

    /* ---- 枚举越界（字符串形态的范围） ---- */
    chk(iv_config_set_str(c, "camera.search", "bogus") == IV_OK, "c8: set a bad enum");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL, "c8: save refuses a bad enum");
    chk(contains(detail, "camera.search"), "c8: detail names the enum key");
    chk(iv_config_set_str(c, "camera.search", "onvif") == IV_OK, "c8: restore the enum");

    /* ---- 跨字段：探活超时 >= 间隔 ---- */
    chk(iv_config_set_int(c, "probe.interval_ms", 1000) == IV_OK, "c8: set interval=1000");
    chk(iv_config_set_int(c, "probe.timeout_ms", 1000) == IV_OK, "c8: set timeout=1000");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL,
        "c8: save refuses timeout_ms >= interval_ms");
    chk(contains(detail, "probe.timeout_ms"), "c8: detail names the cross-field key");

    /* ---- 修好就能存（证明拒绝是"这条不变量"，不是"保存坏了"） ---- */
    chk(iv_config_set_int(c, "probe.timeout_ms", 500) == IV_OK, "c8: fix timeout");
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_OK, "c8: save succeeds after the fix");

    /* ---- 跨字段：阈值上下限倒置（§S8.1 新增的 double 不变量） ---- */
    chk(iv_config_set_dbl(c, "elec.volt_high", 20.0) == IV_OK, "c8: volt_high=20");
    chk(iv_config_set_dbl(c, "elec.volt_low", 50.0) == IV_OK, "c8: volt_low=50");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL,
        "c8: save refuses an inverted threshold pair");
    chk(contains(detail, "elec.volt_high"), "c8: detail names the offending threshold");
    chk(iv_config_set_dbl(c, "elec.volt_low", 10.0) == IV_OK, "c8: un-invert the pair");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_OK, "c8: an ordered pair is accepted");

    /* ---- 反向：0.0 = 未配置，不得被判成"倒置" ---- */
    chk(iv_config_set_dbl(c, "sensor.temp_high", 0.0) == IV_OK, "c8: temp_high=0");
    chk(iv_config_set_dbl(c, "sensor.temp_low", 0.0) == IV_OK, "c8: temp_low=0");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_OK,
        "c8: two unset (0.0) thresholds are NOT an inversion");

    chk(iv_config_get_int(c, "probe.timeout_ms", &iv) == IV_OK && iv == 500,
        "c8: the accepted values are live");

    iv_config_close(c);
}

/* ===========================================================================
 * F3 + F5：一个键是另一个键的父路径（同一路径既当叶子又当对象）必须拒绝
 * 反向证伪：去掉 items_check() 里的 items_conflict() 后 save 会成功，断言失败。
 * ========================================================================= */
static void case_key_prefix_conflict(void)
{
    static const char SEED[] = "{\"custom\":{\"mode\":\"x\"}}\n";
    char         pf[200];
    char         detail[IV_CFG_DETAIL_MAX];
    iv_config_t *c;
    const char  *s = NULL;

    cfg_path(pf, sizeof(pf));
    chk(write_file(pf, SEED, 0644u) == 0, "c9: seed ivsbox.json");

    c = reopen_cfg();
    chk(c != NULL, "c9: open");
    if (c == NULL)
        return;

    chk(iv_config_get_str(c, "custom.mode", &s) == IV_OK && strcmp(s, "x") == 0,
        "c9: seeded leaf value");

    /* custom.mode 是叶子；再加 custom.mode.x 会要求同一路径同时是叶子与对象 */
    chk(iv_config_set_str(c, "custom.mode.x", "y") == IV_OK, "c9: set a child under a leaf");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL,
        "c9: save refuses the leaf/object conflict");
    chk(contains(detail, "conflict"), "c9: detail marks it as a prefix conflict");

    iv_config_close(c);
}

/* ===========================================================================
 * §S8.1 ③：JSON 数组（camera.ch）——元素＝条目、array_len、下标越界/空洞
 * 反向证伪：
 *   ① 去掉 item_check() 的下标越界判定 ⇒ "ch.9 refused" 会变成成功；
 *   ② 去掉空洞判定 ⇒ "gap refused" 会变成成功；
 *   ③ array_len 若按"条数"而非"最大下标+1"算，先写 ch.2 后 len 会错。
 * ========================================================================= */
static void case_array_support(void)
{
    static const char SEED[] =
        "{\"_meta\":{\"version\":3},\n"
        " \"camera\":{ \"search\":\"onvif\",\n"
        "   \"ch\":[ {\"vendor\":\"hk\",\"ip\":\"1.1.1.1\",\"record\":true},\n"
        "            {\"vendor\":\"dh\",\"ip\":\"2.2.2.2\"} ] } }\n";
    char         pf[200];
    char         detail[IV_CFG_DETAIL_MAX];
    iv_config_t *c;
    unsigned     n  = 99u;
    const char  *s  = NULL;
    int          bv = 0;

    cfg_path(pf, sizeof(pf));
    chk(write_file(pf, SEED, 0644u) == 0, "c10: seed a config with a 2-element array");

    c = reopen_cfg();
    chk(c != NULL, "c10: open");
    if (c == NULL)
        return;

    /* 数组元素按"一个元素＝一个条目"存：两根路的字段都要读得到 */
    chk(iv_config_get_str(c, "camera.ch.0.vendor", &s) == IV_OK && strcmp(s, "hk") == 0,
        "c10: element 0 vendor");
    chk(iv_config_get_bool(c, "camera.ch.0.record", &bv) == IV_OK && bv == 1,
        "c10: element 0 record true");
    chk(iv_config_get_str(c, "camera.ch.1.vendor", &s) == IV_OK && strcmp(s, "dh") == 0,
        "c10: element 1 vendor");
    /* 元素 1 没给 record ⇒ 按默认补齐为 false */
    chk(iv_config_get_bool(c, "camera.ch.1.record", &bv) == IV_OK && bv == 0,
        "c10: element 1 record defaulted to false");
    /* 元数 0 给了 ip，元数 1 没给 ⇒ 每元素独立补默认 */
    chk(iv_config_get_str(c, "camera.ch.1.ip", &s) == IV_OK && strcmp(s, "2.2.2.2") == 0,
        "c10: element 1 ip");

    /* array_len：按"最大下标 + 1"计。默认表给足 camera.ch.0..5 六个槽 ⇒ 恒为 6，
     * 与文件里写了几路无关（文件只决定"值"，不决定"槽位"）。 */
    chk(iv_config_array_len(c, "camera.ch", &n) == IV_OK && n == 6u,
        "c10: array length is 6 (the default table stamps slots 0..5)");
    chk(iv_config_array_len(c, "camera.zzz", &n) == IV_OK && n == 0u,
        "c10: an array nobody declared has length 0");
    chk(iv_config_array_len(c, "camera.ch", NULL) == IV_EINVAL, "c10: NULL n");
    chk(iv_config_array_len(c, "a..b", &n) == IV_EINVAL, "c10: a malformed arr_key");

    /* 下标越界，且**不构成空洞**：ch.6 的下标 0..5 全在（默认表铺满）⇒
     * 只有"越界判定"能拒它，空洞判定不会替它兜底。这条专门钉死越界规则本身。
     * 反向证伪：把 `idx >= IV_CFG_ARR_MAX` 改成恒假 ⇒ 本用例 save 会变 IV_OK。 */
    chk(iv_config_set_str(c, "camera.ch.6.vendor", "oob") == IV_OK,
        "c10: set element 6 (one past the 6-slot cap, no gap)");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL,
        "c10: save refuses an out-of-range index that has NO gap");
    chk(contains(detail, "camera.ch.6") && contains(detail, "out of range"),
        "c10: detail names it as out-of-range (not as a gap)");
    iv_config_close(c); /* ch.6 无法用 set 删除，换个干净句柄做下一条 */

    /* 另一个越界形态：ch.9（越界与空洞两条规则都该拒它）。
     * 注意不能在同一个句柄上连做 —— set_str("") 只是把值清空，**键仍在表里**，
     * 上一条遗留的 ch.6 会先被判出来，detail 就指不到 ch.9。 */
    c = reopen_cfg();
    chk(c != NULL, "c10: fresh handle for the ch.9 probe");
    if (c != NULL) {
        chk(iv_config_set_str(c, "camera.ch.9.vendor", "bad") == IV_OK,
            "c10: set an out-of-range element index");
        detail[0] = '\0';
        chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL,
            "c10: save refuses an out-of-range array index");
        chk(contains(detail, "camera.ch.9"), "c10: detail names the offending element");
        iv_config_close(c);
    }

    {
        /* 空洞用例：用一个**默认表里没有的**数组前缀（`extra.slot`），这样默认补齐
         * 不会替我们把缺的下标补上，空洞得以保留到校验阶段。
         * 文件写 slot.0 与 slot.2、故意不写 slot.1（`{}` 空元素）⇒
         *   - 加载必须**保留**这三条键（加载期不判空洞，否则 .2 会被悄悄丢掉）；
         *   - `save()` 必须判非法（保存期严格校验空洞）。
         * 反向证伪：
         *   ① 把 item_check 的 strict 守卫去掉 ⇒ "load kept slot.2" 失败；
         *   ② 去掉空洞判定 ⇒ "save refuses" 失败。 */
        static const char GAPPED[] =
            "{\"_meta\":{\"version\":5},\n"
            " \"extra\":{ \"slot\":[ {\"a\":1}, {}, {\"a\":3} ] } }\n";
        iv_config_t *cg;
        int64_t      gv = 0;

        chk(write_file(pf, GAPPED, 0644u) == 0, "c10: seed an unknown array with a hole at 1");
        cg = reopen_cfg();
        chk(cg != NULL, "c10: reopen the gapped file");
        if (cg != NULL) {
            chk(iv_config_array_len(cg, "extra.slot", &n) == IV_OK && n == 3u,
                "c10: the unknown array reports length 3");
            chk(iv_config_get_int(cg, "extra.slot.2.a", &gv) == IV_OK && gv == 3,
                "c10: load KEPT element 2 (no hole rule during load)");
            detail[0] = '\0';
            chk(iv_config_save(cg, detail, sizeof(detail)) == IV_EINVAL,
                "c10: save refuses an array with an interior gap");
            chk(contains(detail, "gap"), "c10: detail reports the array gap");
            iv_config_close(cg);
        }
    }

    /* 空数组与"根本没有数组"等价：一份完全不含 camera.ch 的文件 ⇒ 默认表补齐后
     * 槽位仍在（6），但"一个键都没从文件读到"这件事由 from_file 反映。 */
    {
        char         pf3[200];
        iv_config_t *c3;
        iv_cfg_stat_t st3;

        cfg_path(pf3, sizeof(pf3));
        chk(write_file(pf3, "{\"_meta\":{\"version\":4}}\n", 0644u) == 0, "c10: seed an empty config");
        c3 = reopen_cfg();
        chk(c3 != NULL, "c10: reopen");
        if (c3 != NULL) {
            chk(iv_config_stat(c3, &st3) == IV_OK && st3.from_file == 0u,
                "c10: nothing came from the file");
            chk(iv_config_array_len(c3, "camera.ch", &n) == IV_OK && n == 6u,
                "c10: slots are still stamped by the default table");
            iv_config_close(c3);
        }
    }
}

/* ===========================================================================
 * §S8.1 ⑤：中文注释渲染 + **渲染→再解析** 往返等值
 *   ① save() 出来的文件里必须有一级分类的中文说明注释；
 *   ② 复用同一套渲染器写出的文件，重新 open 必须等值（注释不会污染解析）；
 *   ③ double 渲染后必须仍是 double（不能因 "1.0" 写成 "1" 而漂成 int）。
 * 反向证伪：① 去掉 render_json 里的 `// ` 输出 ⇒ 注释断言失败；
 *           ② 去掉 rj_scalar 里 double 的强制小数点 ⇒ "still a double" 失败。
 * ========================================================================= */
static void case_comment_roundtrip(void)
{
    char          pf[200];
    char          buf[8192];
    iv_config_t  *c;
    iv_cfg_stat_t st;
    double        dv = 0.0;
    int           bv = 0;
    int64_t       iv = 0;
    const char   *s  = NULL;

    cfg_path(pf, sizeof(pf));
    (void)unlink(pf);

    /* 全新的配置：改几个值后 save，得到"渲染器亲手写出"的文件 */
    c = reopen_cfg();
    chk(c != NULL, "c11: open (defaults)");
    if (c == NULL)
        return;

    chk(iv_config_set_dbl(c, "elec.volt_high", 1.0) == IV_OK, "c11: set a whole-number double");
    chk(iv_config_set_dbl(c, "sensor.temp_low", -5.5) == IV_OK, "c11: set a fractional double");
    chk(iv_config_set_int(c, "sys.transport.mode", 1) == IV_OK, "c11: set an int");
    chk(iv_config_set_bool(c, "sys.debug.mode", 1) == IV_OK, "c11: set a bool");
    chk(iv_config_set_str(c, "net.ip", "172.16.0.9") == IV_OK, "c11: set a string");

    chk(iv_config_save(c, NULL, 0u) == IV_OK, "c11: save");
    iv_config_close(c);

    /* ① 中文注释确实写进了文件 */
    chk(read_file(pf, buf, sizeof(buf)) > 0, "c11: re-read the rendered file");
    chk(contains(buf, "// "), "c11: the rendered file carries line comments");
    chk(contains(buf, "基础配置"), "c11: the sys class carries its Chinese description");
    chk(contains(buf, "网络配置"), "c11: the net class carries its Chinese description");
    chk(contains(buf, "摄像机配置"), "c11: the camera class carries its Chinese description");
    chk(contains(buf, "电阈值"), "c11: the elec class carries its Chinese description");
    /* 分类内的叶子注释也来自默认值表 */
    chk(contains(buf, "传输模式"), "c11: a leaf carries its Chinese description");
    /* 数组必须渲染成 JSON 数组而不是对象 */
    chk(contains(buf, "\"ch\": ["), "c11: camera.ch renders as an array");

    /* ② 再解析回来必须等值（注释不会污染解析） */
    c = reopen_cfg();
    chk(c != NULL, "c11: reopen the rendered file");
    if (c == NULL)
        return;

    chk(iv_config_stat(c, &st) == IV_OK, "c11: stat after round-trip");
    chk(st.loaded == 1, "c11: the rendered file parses back cleanly");
    chk(st.invalid == 0u, "c11: no value was rejected on the round-trip");
    chk(st.items == 87u, "c11: all keys survived the round-trip");

    chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && strcmp(s, "172.16.0.9") == 0,
        "c11: the string survived");
    chk(iv_config_get_int(c, "sys.transport.mode", &iv) == IV_OK && iv == 1,
        "c11: the int survived");
    chk(iv_config_get_bool(c, "sys.debug.mode", &bv) == IV_OK && bv == 1, "c11: the bool survived");

    /* ③ double 必须还是 double：整数值也不能漂成 int */
    chk(iv_config_get_dbl(c, "elec.volt_high", &dv) == IV_OK && dv == 1.0,
        "c11: a whole-number double is still a DOUBLE after the round-trip");
    chk(iv_config_get_int(c, "elec.volt_high", &iv) == IV_EPROTO,
        "c11: ... and it did NOT drift into an int");
    chk(iv_config_get_dbl(c, "sensor.temp_low", &dv) == IV_OK && dv == -5.5,
        "c11: the fractional double survived exactly");

    /* 元素也走过往返：默认表给足 6 个槽 ⇒ 渲染成 6 元素数组、读回仍是 6 */
    {
        unsigned n = 0u;

        chk(iv_config_array_len(c, "camera.ch", &n) == IV_OK && n == 6u,
            "c11: all six array slots survive the array render");
    }

    iv_config_close(c);
}

/* ===========================================================================
 * F6：外部改文件 ⇒ 不重启即生效
 *   ① 没改动时不重载；目录里别的文件动了也不重载（按文件名过滤有效）；
 *   ② **第二次** rename 也必须生效 —— "watch 目录而不是 watch 文件"的证伪。
 * ========================================================================= */
static void case_hot_reload(void)
{
    static const char V1[] =
        "{\"_meta\":{\"version\":50},\"sys\":{\"report\":{\"interval_s\":120}}}\n";
    static const char V2[] =
        "{\"_meta\":{\"version\":51},\"sys\":{\"report\":{\"interval_s\":200}}}\n";
    char         pf[200];
    char         stage[200];
    char         other[200];
    iv_config_t *c;
    int64_t      iv = 0;
    uint32_t     rl = 0u;
    int          fd;

    cfg_path(pf, sizeof(pf));
    path_of(stage, sizeof(stage), "ivsbox.stage");
    path_of(other, sizeof(other), "unrelated.json");
    (void)unlink(pf);

    c = reopen_cfg();
    chk(c != NULL, "c12: open");
    if (c == NULL)
        return;

    fd = iv_config_watch_fd(c);
    chk(fd >= 0, "c12: watch fd created");
    chk(iv_config_watch_fd(c) == fd, "c12: a repeated call hands back the same fd");
    chk(iv_config_watch_fd(NULL) == IV_EINVAL, "c12: watch_fd(NULL) == EINVAL");

    {
        int rc = iv_config_watch_poll(c, &rl);

        chk(rc == IV_EAGAIN || (rc == IV_OK && rl == 0u), "c12: nothing changed => no reload");
    }

    chk(write_file(other, "{}\n", 0644u) == 0, "c12: write an unrelated file");
    rl = 99u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl == 0u,
        "c12: another file in the same directory does not trigger a reload");

    /* 外部原子写 #1 */
    chk(write_file(stage, V1, 0644u) == 0, "c12: stage the new content");
    chk(rename(stage, pf) == 0, "c12: atomic swap #1");
    rl = 0u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl >= 1u, "c12: rename triggers a reload");
    chk(iv_config_get_int(c, "sys.report.interval_s", &iv) == IV_OK && iv == 120,
        "c12: the new value is live");
    chk(iv_config_version(c) == 50u, "c12: the version follows the file");

    /* 外部原子写 #2 —— watch 目录的证伪点 */
    chk(write_file(stage, V2, 0644u) == 0, "c12: stage the second revision");
    chk(rename(stage, pf) == 0, "c12: atomic swap #2");
    rl = 0u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl >= 1u,
        "c12: the SECOND rename also reloads (proves the watch is on the directory)");
    chk(iv_config_get_int(c, "sys.report.interval_s", &iv) == IV_OK && iv == 200,
        "c12: the second value is live");

    iv_config_close(c);
}

/* ===========================================================================
 * §S8.1 ⑥ + F6/F7：热更新事务粒度＝**按一级分类子树**
 *   一份新文件里同时改好 net 与 probe，但 probe 里塞一个越界值 ⇒
 *   只拒 probe、net 的新值照常采用；订阅者**收到通知**（因为至少一个分类采用）。
 *   再来一份"整份语法坏"的文件 ⇒ 整份拒绝、旧值一字不动、**一个都不通知**。
 *
 * 反向证伪：
 *   ① 若把 poll 改回"文件级全有全无"（发现任一非法就整份拒绝），
 *      那么 (a) 里 net 的新值不会生效 —— "net was still adopted" 断言失败；
 *      而 (b) 里订阅者会数到 1 —— "NO subscriber" 断言失败。
 *   ② 若通知条件写成"总是通知"，(b) 会数到 1。
 * ========================================================================= */
static void case_per_class_transaction(void)
{
    /* (a) net 合法、probe 冲突（timeout_ms >= interval_ms ⇒ 跨字段冲突）。
     * 用**跨字段冲突**而不是"越界值"当坏触发点：越界值在解析期就被
     * item_reset_to_default 换成默认值，"回退旧分类"与"不回退"的结果
     * 碰巧一样，断言会失去区分力（这正是 M8 证伪曾漏过的盲点）。
     * 跨字段冲突同样在解析期被 cross_fix_by_default 复位，但**下面先把
     * probe.interval_ms 在运行期改成 9000**，于是：
     *   有回退 ⇒ 该分类整块回退旧值，interval_ms 仍是 9000；
     *   无回退 ⇒ 解析期复位生效，interval_ms 变成默认 5000。
     * 两者结果不同，断言才真正卡住"按分类回退"这条路径。 */
    static const char MIXED[] =
        "{\"_meta\":{\"version\":80},\n"
        " \"net\":{ \"ip\":\"10.9.9.9\", \"gateway\":\"10.9.9.1\" },\n"
        " \"probe\":{ \"interval_ms\":2000, \"timeout_ms\":9000 } }\n";
    /* (b) 半截文件：整份解析失败 */
    static const char BAD_SYNTAX[] = "{\"sys\":{\"report\":{\"interval_s\":12";
    /* (c) 再来一份全合法的 */
    static const char GOOD[] =
        "{\"_meta\":{\"version\":81},\n"
        " \"net\":{ \"ip\":\"10.8.8.8\" },\n"
        " \"probe\":{ \"interval_ms\":8000, \"timeout_ms\":3000 } }\n";
    struct notify_ctx n1;
    struct notify_ctx n2;
    char         pf[200];
    char         stage[200];
    iv_config_t *c;
    const char  *s  = NULL;
    int64_t      iv = 0;
    uint32_t     rl = 0u;

    cfg_path(pf, sizeof(pf));
    path_of(stage, sizeof(stage), "ivsbox.stage");
    (void)unlink(pf);

    c = reopen_cfg();
    chk(c != NULL, "c13: open");
    if (c == NULL)
        return;
    chk(iv_config_watch_fd(c) >= 0, "c13: watch");

    n1.count = 0;
    n2.count = 0;
    n1.last[0] = '\0';
    n2.last[0] = '\0';
    chk(iv_config_subscribe(c, on_notify, &n1) == IV_OK, "c13: subscribe #1");
    chk(iv_config_subscribe(c, on_notify, &n2) == IV_OK, "c13: subscribe #2");
    chk(iv_config_subscribe(c, NULL, NULL) == IV_EINVAL, "c13: NULL callback rejected");

    /* (a) 混合：probe 冲突、net 好。
     * 先把 probe.interval_ms 在运行期改成 9000（≠ 默认 5000）—— 这是"旧值"，
     * 按分类回退应把它保留为 9000，而不是变回解析期的默认 5000。 */
    chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && s[0] == '\0', "c13: net.ip starts empty");
    chk(iv_config_set_int(c, "probe.interval_ms", 9000) == IV_OK, "c13: seed probe.interval_ms=9000");
    chk(iv_config_save(c, NULL, 0u) == IV_OK, "c13: persist the seeded value");
    n1.count = 0;
    n2.count = 0; /* save() 自己也会通知，先把计数器清零，只看下面这次 reload */
    chk(write_file(stage, MIXED, 0644u) == 0, "c13: stage a mixed file");
    chk(rename(stage, pf) == 0, "c13: swap in the mixed file");
    rl = 99u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK, "c13: poll the mixed file");
    chk(rl >= 1u, "c13: at least one class was adopted (net)");
    /* net 的新值必须生效 —— 这是"按分类"的核心证伪点 */
    chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && strcmp(s, "10.9.9.9") == 0,
        "c13: net was still adopted despite probe being invalid");
    chk(iv_config_get_str(c, "net.gateway", &s) == IV_OK && strcmp(s, "10.9.9.1") == 0,
        "c13: net.gateway adopted too");
    /* probe 分类整块回退**旧值** 9000（不是默认 5000）—— 区分"真回退"与"没回退" */
    chk(iv_config_get_int(c, "probe.interval_ms", &iv) == IV_OK && iv == 9000,
        "c13: the invalid probe class rolled back to its OLD value, not the parse-time default");
    chk(n1.count == 1 && n2.count == 1, "c13: subscribers were notified (a class was adopted)");
    chk(strcmp(n1.last, IV_CFG_NAME) == 0, "c13: the callback receives the config name");

    /* (b) 整份语法坏 ⇒ 全拒、不通知、旧值不动 */
    n1.count = 0;
    n2.count = 0;
    chk(write_file(stage, BAD_SYNTAX, 0644u) == 0, "c13: stage a broken file");
    chk(rename(stage, pf) == 0, "c13: swap in the broken file");
    rl = 99u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl == 0u,
        "c13: a syntactically broken file adopts nothing");
    chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && strcmp(s, "10.9.9.9") == 0,
        "c13: the previous good value is kept");
    chk(n1.count == 0 && n2.count == 0,
        "c13: NO subscriber was notified when nothing was adopted");

    /* (c) 全合法 ⇒ 立刻生效，两个订阅者都收到 */
    chk(write_file(stage, GOOD, 0644u) == 0, "c13: stage a good file");
    chk(rename(stage, pf) == 0, "c13: swap in the good file");
    rl = 0u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl >= 1u, "c13: the good file reloads");
    chk(iv_config_get_str(c, "net.ip", &s) == IV_OK && strcmp(s, "10.8.8.8") == 0,
        "c13: the new good value is live");
    chk(iv_config_get_int(c, "probe.interval_ms", &iv) == IV_OK && iv == 8000,
        "c13: the previously-rejected class now accepts a good value");
    chk(n1.count == 1 && n2.count == 1, "c13: both subscribers were notified");

    /* (d) 程序自己 save() 也要通知 */
    n1.count = 0;
    n2.count = 0;
    chk(iv_config_set_str(c, "net.ip", "10.7.7.7") == IV_OK, "c13: set a new value");
    chk(iv_config_save(c, NULL, 0u) == IV_OK, "c13: save");
    chk(n1.count == 1 && n2.count == 1, "c13: save() notifies subscribers too");

    iv_config_close(c);
}

/* ===========================================================================
 * F6（真 Reactor）：热更新必须在**真的 iv_reactor_run() 回调**里走通一次
 * 反向证伪：若 watch fd 没被注册进 reactor，reloads 会保持 0，由 2s 兜底结束。
 * ========================================================================= */
struct reactor_ctx {
    iv_reactor_t *r;
    iv_config_t  *c;
    char          stage[200];
    char          target[200];
    const char   *text;
    int           reloads;
    int           events;
    int           failed;
};

static void on_cfg_event(int fd, uint32_t events, void *arg)
{
    struct reactor_ctx *x = (struct reactor_ctx *)arg;
    uint32_t            n = 0u;

    (void)fd;
    (void)events;
    x->events++;
    if (iv_config_watch_poll(x->c, &n) == IV_OK && n > 0u)
        x->reloads += (int)n;
    if (x->reloads > 0)
        iv_reactor_stop(x->r);
}

static void stage_and_swap(void *arg)
{
    struct reactor_ctx *x = (struct reactor_ctx *)arg;

    if (write_file(x->stage, x->text, 0644u) != 0 || rename(x->stage, x->target) != 0)
        x->failed = 1;
}

static void force_stop(void *arg)
{
    struct reactor_ctx *x = (struct reactor_ctx *)arg;

    iv_reactor_stop(x->r);
}

static void case_reactor_integration(void)
{
    static const char NEWCFG[] =
        "{\"_meta\":{\"version\":70},\"sys\":{\"transport\":{\"mode\":3}}}\n";
    struct reactor_ctx x;
    iv_reactor_t       *r;
    iv_config_t        *c;
    int64_t             iv = 0;
    int                 fd;

    path_of(x.stage, sizeof(x.stage), "ivsbox.stage");
    path_of(x.target, sizeof(x.target), "ivsbox.json");
    (void)unlink(x.target);

    c = reopen_cfg();
    chk(c != NULL, "c14: open");
    if (c == NULL)
        return;

    r = iv_reactor_create(8);
    chk(r != NULL, "c14: reactor create");
    if (r == NULL) {
        iv_config_close(c);
        return;
    }

    fd = iv_config_watch_fd(c);
    chk(fd >= 0, "c14: watch fd");
    chk(iv_reactor_add(r, fd, IV_EV_READ, on_cfg_event, &x) != NULL,
        "c14: register the watch fd in the real reactor");

    x.r       = r;
    x.c       = c;
    x.text    = NEWCFG;
    x.reloads = 0;
    x.events  = 0;
    x.failed  = 0;

    chk(iv_timer_add(r, 40u, stage_and_swap, &x) != NULL, "c14: schedule the external edit");
    chk(iv_timer_add(r, 2000u, force_stop, &x) != NULL, "c14: schedule the safety stop");

    chk(iv_reactor_run(r) == IV_OK, "c14: reactor run returned cleanly");
    chk(x.failed == 0, "c14: the external atomic write actually happened");
    chk(x.reloads >= 1, "c14: the reload happened inside the real reactor loop");
    chk(iv_config_get_int(c, "sys.transport.mode", &iv) == IV_OK && iv == 3,
        "c14: the config is live after the reactor pass");
    chk(iv_config_version(c) == 70u, "c14: version followed the file");

    iv_config_close(c);
    iv_reactor_destroy(r);
}

/* ===========================================================================
 * 句柄池、配置名校验、未开 watch 就 poll、NULL 安全
 * 本用例同时是**句柄泄漏探测器**：要求 4 个槽恰好空出 4 个。
 * 注意单句柄约束：同一 <dir>/<name> 只能开一个 ⇒ 这里用**不同 name** 填满池子。
 * ========================================================================= */
static void case_pool_and_misc(void)
{
    iv_config_t *h[4];
    char         nm[8];
    int          i;

    for (i = 0; i < 4; i++) {
        nm[0] = 'h';
        nm[1] = (char)('1' + i);
        nm[2] = '\0';
        h[i]  = iv_config_open(g_dir, nm);
        chk(h[i] != NULL, "c16: open until the handle pool is full");
    }
    chk(iv_config_open(g_dir, "h9") == NULL, "c16: the 5th handle is refused");
    /* 单句柄：不同 name 但同名重复也要拒 */
    chk(iv_config_open(g_dir, "h1") == NULL, "c16: a duplicate name is refused too");

    iv_config_close(h[0]);
    {
        iv_config_t *again = iv_config_open(g_dir, "h9");

        chk(again != NULL, "c16: a freed slot is reusable");
        iv_config_close(again);
    }
    for (i = 1; i < 4; i++)
        iv_config_close(h[i]);

    iv_config_close(NULL); /* 必须安全 */

    /* 配置名校验（防拼出目录外的路径） */
    chk(iv_config_open(g_dir, "") == NULL, "c16: an empty name is rejected");
    chk(iv_config_open(g_dir, "a/b") == NULL, "c16: '/' in the name is rejected");
    chk(iv_config_open(g_dir, "../esc") == NULL, "c16: '..' in the name is rejected");
    chk(iv_config_open(NULL, NULL) == NULL, "c16: NULL name is rejected");

    /* 没开 watch 就 poll */
    {
        iv_config_t *c  = reopen_cfg();
        uint32_t     rl = 0u;

        chk(c != NULL, "c16: open for the poll-state check");
        if (c != NULL) {
            chk(iv_config_watch_poll(c, &rl) == IV_ESTATE, "c16: poll before watch_fd == ESTATE");
            iv_config_close(c);
        }
    }
    chk(iv_config_watch_poll(NULL, NULL) == IV_EINVAL, "c16: poll with a NULL handle");
    chk(iv_config_version(NULL) == 0u, "c16: version of NULL is 0");
    chk(iv_config_stat(NULL, NULL) == IV_EINVAL, "c16: stat with NULL");
}

/* ===========================================================================
 * F4：配置目录不存在时按需创建
 * ========================================================================= */
static void case_dir_autocreate(void)
{
    char         sub[200];
    char         pf[240];
    struct stat  sb;
    iv_config_t *c;

    path_of(sub, sizeof(sub), "auto");
    (void)rmdir(sub);

    c = iv_config_open(sub, IV_CFG_NAME);
    chk(c != NULL, "c17: open into a non-existent directory still works");
    if (c != NULL) {
        chk(stat(sub, &sb) == 0 && S_ISDIR(sb.st_mode),
            "c17: the module created the config directory");
        iv_config_close(c);
    }

    path_of(pf, sizeof(pf), "auto/ivsbox.json");
    (void)unlink(pf);
    (void)rmdir(sub);
}

/* ---------------------------------------------------------------------------
 * 收尾：把本用例造出来的东西全部删掉（AGENTS.md 规则 6）
 * ------------------------------------------------------------------------- */
static void cleanup(void)
{
    static const char *const files[] = {
        "ivsbox.json",      "ivsbox.json.tmp", "ivsbox.stage",   "unrelated.json",
        "h1.json",          "h2.json",         "h3.json",        "h4.json",
        "h9.json",          "auto/ivsbox.json",
    };
    char   p[240];
    size_t i;

    for (i = 0u; i < sizeof(files) / sizeof(files[0]); i++) {
        path_of(p, sizeof(p), files[i]);
        (void)unlink(p);
    }
    path_of(p, sizeof(p), "auto");
    (void)rmdir(p);
    (void)rmdir(g_dir);
}

int main(void)
{
    char   tmpl[] = "/tmp/ivcfg_XXXXXX";
    char  *d;
    size_t dl;

    /* 本模块与 iv_log 都会落盘；单测里关掉落盘并改走空 sink，
     * 免得在宿主机或板端留下 /mnt/UDISK/log 之类的副作用。 */
    iv_log_set_root(NULL);
    iv_log_set_sink(quiet_sink, NULL);

    d = mkdtemp(tmpl);
    if (d == NULL) {
        fprintf(stderr, "FAIL: mkdtemp(): %s\n", strerror(errno));
        return 1;
    }
    dl = strlen(d);
    if (dl + 1u > sizeof(g_dir)) {
        fprintf(stderr, "FAIL: temp dir name too long\n");
        return 1;
    }
    memcpy(g_dir, d, dl + 1u);

    case_defaults_startup();
    case_read_typed();
    case_single_handle();
    case_set_memory_then_save();
    case_fill_and_origin();
    case_parent_scalar_rejected();
    case_atomic_replace();
    case_reject_writes_nothing();
    case_key_prefix_conflict();
    case_array_support();
    case_comment_roundtrip();
    case_hot_reload();
    case_per_class_transaction();
    case_reactor_integration();
    case_pool_and_misc();
    case_dir_autocreate();

    cleanup();

    if (g_fail != 0) {
        fprintf(stderr, "test_config failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_config passed (S8 F1-F7 + S8.1 single-file/8-class/array/double/"
           "comment-roundtrip/per-class transaction)\n");
    return 0;
}
