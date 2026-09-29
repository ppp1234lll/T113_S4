/*
 * iv_config 单测（libivmodules，功能开发计划 M1-S8）
 *
 * 覆盖计划 §S8 的八个功能 F1~F8，每个功能都配了**能反向证伪**的断言 ——
 * 也就是说，把实现改回缺陷态后这条断言必须真的失败（S7 那轮的教训：
 * 只断言"成功路径能跑通"的用例，把实现改坏也照样绿）。
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
static char g_sec[200]; /* 其下的 secure/ 子目录（F8 用） */

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

/* 写文件并**显式 fchmod**：O_CREAT 的 mode 会被 umask 削，F8 的权限用例必须可控 */
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

/* --- 样本 --- */

/* 9 个 network 键全给，version=11：验证"文件值优先"与"版本号来自文件" */
static const char NETWORK_JSON[] =
    "{\n"
    "  \"_meta\": { \"schema\": 1, \"version\": 11 },\n"
    "  \"net\": {\n"
    "    \"wan\": {\n"
    "      \"mode\": \"wireless-first\",\n"
    "      \"primary_if\": \"wlan0\",\n"
    "      \"backup_if\": \"eth0\",\n"
    "      \"fail_n\": 3,\n"
    "      \"ok_n\": 5,\n"
    "      \"hold_s\": 30\n"
    "    },\n"
    "    \"probe\": { \"interval_ms\": 5000, \"timeout_ms\": 2000, \"max_fail\": 3 }\n"
    "  }\n"
    "}\n";

/* 5 个 platform 键全给的样本（host 用 OLD，供 F5 的改名验证） */
static const char PLATFORM_JSON[] =
    "{\n"
    "  \"_meta\": { \"schema\": 1, \"version\": 2 },\n"
    "  \"platform\": { \"proto\": { \"host\": \"OLD\", \"port\": 1, \"device_id\": \"d\",\n"
    "                             \"heartbeat_s\": 30, \"reconnect_max_s\": 60 } }\n"
    "}\n";

/* ===========================================================================
 * F1 + F4：文件根本不存在 ⇒ 用内置默认值照常启动，不许失败
 * 反向证伪：若默认值表被清空 / 补齐逻辑不跑，items 与各默认值断言立刻失败。
 * ========================================================================= */
static void case_defaults_startup(void)
{
    char            pf[200];
    iv_config_t    *c;
    iv_cfg_stat_t   st;
    iv_cfg_origin_t org;
    const char     *s  = NULL;
    int64_t         iv = 0;
    iv_cfg_type_t   t  = IV_CFG_T_BOOL;
    char            txt[32];

    path_of(pf, sizeof(pf), "platform.json");
    (void)unlink(pf); /* 确保文件不存在 */

    c = iv_config_open(g_dir, "platform");
    chk(c != NULL, "c1: open without a file still returns a handle");
    if (c == NULL)
        return;

    chk(iv_config_stat(c, &st) == IV_OK, "c1: stat");
    chk(st.loaded == 0, "c1: loaded == 0 (started from built-in defaults)");
    chk(st.items == 5u, "c1: all 5 platform defaults materialised");
    chk(st.from_file == 0u, "c1: nothing came from the file");
    chk(st.from_default == 5u, "c1: everything came from defaults");
    chk(st.invalid == 0u, "c1: a missing file is NOT an invalid value");
    chk(st.version == 0u, "c1: version is 0 when there is no file");
    chk(st.dirty == 0, "c1: not dirty right after open");

    chk(iv_config_get_int(c, "platform.proto.heartbeat_s", &iv) == IV_OK && iv == 30,
        "c1: heartbeat_s default 30");
    chk(iv_config_get_int(c, "platform.proto.reconnect_max_s", &iv) == IV_OK && iv == 60,
        "c1: reconnect_max_s default 60");
    chk(iv_config_get_int(c, "platform.proto.port", &iv) == IV_OK && iv == 0,
        "c1: port default 0 (unconfigured)");
    chk(iv_config_get_str(c, "platform.proto.host", &s) == IV_OK && s[0] == '\0',
        "c1: host default empty (unconfigured)");

    chk(iv_config_origin(c, "platform.proto.heartbeat_s", &org) == IV_OK &&
            org == IV_CFG_FROM_DEFAULT,
        "c1: origin reports DEFAULT");
    chk(iv_config_origin(c, "no.such.key", &org) == IV_ENOENT, "c1: origin of a missing key");

    /* 内置默认表的只读查询（给 Web / 诊断用） */
    chk(iv_config_default_get("platform", "platform.proto.heartbeat_s", &t, txt, sizeof(txt)) ==
            IV_OK,
        "c1: default_get on a known key");
    chk(t == IV_CFG_T_INT && strcmp(txt, "30") == 0, "c1: default_get renders '30'");
    chk(iv_config_default_get("platform", "platform.proto.zzz", &t, txt, sizeof(txt)) == IV_ENOENT,
        "c1: default_get on an unknown key == ENOENT");
    chk(iv_config_default_get(NULL, "x", NULL, NULL, 0u) == IV_EINVAL, "c1: default_get NULL args");

    iv_config_close(c);
}

/* ===========================================================================
 * F1 + F2：读一份真文件，三种类型都取得到；类型不符/键不存在必须明确报错
 * 反向证伪：① 若 get_* 不查类型而做隐式转换，get_int(bool键) 会返回 IV_OK；
 *           ② 若 item_find 改成"找不到就返回第一个"，ENOENT 断言会失败。
 * ========================================================================= */
static void case_read_typed(void)
{
    static const char JSON[] =
        "{\n"
        "  \"_meta\": { \"schema\": 3, \"version\": 7 },\n"
        "  \"platform\": { \"proto\": { \"host\": \"10.0.0.5\", \"port\": 9000,\n"
        "                              \"device_id\": \"dev-abc\", \"heartbeat_s\": 15,\n"
        "                              \"reconnect_max_s\": 120 } },\n"
        "  \"feature\": { \"enabled\": true, \"name\": \"x y\" }\n"
        "}\n";
    char            pf[200];
    iv_config_t    *c;
    iv_cfg_stat_t   st;
    iv_cfg_origin_t org;
    const char     *s  = NULL;
    int64_t         iv = 0;
    int             bv = 0;

    path_of(pf, sizeof(pf), "platform.json");
    chk(write_file(pf, JSON, 0644u) == 0, "c2: seed platform.json");

    c = iv_config_open(g_dir, "platform");
    chk(c != NULL, "c2: open");
    if (c == NULL)
        return;

    chk(iv_config_stat(c, &st) == IV_OK, "c2: stat");
    chk(st.loaded == 1, "c2: loaded == 1");
    chk(st.schema == 3u, "c2: schema read from _meta");
    chk(st.version == 7u, "c2: version read from _meta");
    chk(st.items == 7u, "c2: 5 platform keys + 2 unknown-but-preserved keys");
    chk(st.from_file == 7u, "c2: every key came from the file");
    chk(st.from_default == 0u, "c2: nothing needed a default");
    chk(st.invalid == 0u, "c2: no invalid key");
    chk(st.crc_mismatch == 0, "c2: no crc in the file => no mismatch warning");

    chk(iv_config_get_str(c, "platform.proto.host", &s) == IV_OK && strcmp(s, "10.0.0.5") == 0,
        "c2: string value");
    chk(iv_config_get_int(c, "platform.proto.port", &iv) == IV_OK && iv == 9000,
        "c2: integer value");
    chk(iv_config_get_str(c, "feature.name", &s) == IV_OK && strcmp(s, "x y") == 0,
        "c2: unknown key with a space is preserved verbatim");
    chk(iv_config_get_bool(c, "feature.enabled", &bv) == IV_OK && bv == 1,
        "c2: boolean value");

    /* 类型不符：三类 get 都不得做隐式转换 */
    chk(iv_config_get_int(c, "platform.proto.host", &iv) == IV_EPROTO,
        "c2: get_int on a string == EPROTO");
    chk(iv_config_get_str(c, "platform.proto.port", &s) == IV_EPROTO,
        "c2: get_str on an int == EPROTO");
    chk(iv_config_get_int(c, "feature.enabled", &iv) == IV_EPROTO,
        "c2: bool is NOT implicitly converted to int");
    chk(iv_config_get_bool(c, "platform.proto.heartbeat_s", &bv) == IV_EPROTO,
        "c2: get_bool on an int == EPROTO");

    /* 键不存在 / 参数非法 */
    chk(iv_config_get_str(c, "platform.proto.zzz", &s) == IV_ENOENT, "c2: missing key == ENOENT");
    chk(iv_config_get_str(c, "platform.proto.host", NULL) == IV_EINVAL, "c2: NULL out");
    chk(iv_config_get_str(c, NULL, &s) == IV_EINVAL, "c2: NULL key");
    chk(iv_config_get_str(NULL, "platform.proto.host", &s) == IV_EINVAL, "c2: NULL handle");
    chk(iv_config_get_int(c, "platform.proto.port", NULL) == IV_EINVAL, "c2: NULL int out");
    chk(iv_config_get_bool(c, "feature.enabled", NULL) == IV_EINVAL, "c2: NULL bool out");

    chk(iv_config_origin(c, "platform.proto.host", &org) == IV_OK && org == IV_CFG_FROM_FILE,
        "c2: origin reports FILE");

    iv_config_close(c);
}

/* ===========================================================================
 * F3 + F5：set 只改内存（此刻磁盘仍是旧值）；save 才落盘且版本号 +1
 * 反向证伪：① 若 set_* 顺手写盘，"save 前磁盘仍是旧值"这条会失败；
 *           ② 若 save 不改版本号 / 多加了 1，版本断言失败；
 *           ③ 若 save 后没清 dirty，dirty 断言失败。
 * ========================================================================= */
static void case_set_memory_then_save(void)
{
    char           nf[200];
    char           buf[1024];
    char           tmpf[200];
    iv_config_t   *c;
    iv_config_t   *c2;
    iv_cfg_stat_t  st;
    int64_t        iv = 0;
    uint64_t       v0;

    path_of(nf, sizeof(nf), "network.json");
    path_of(tmpf, sizeof(tmpf), "network.json.tmp");
    chk(write_file(nf, NETWORK_JSON, 0644u) == 0, "c3: seed network.json");

    c = iv_config_open(g_dir, "network");
    chk(c != NULL, "c3: open");
    if (c == NULL)
        return;

    chk(iv_config_get_int(c, "net.wan.fail_n", &iv) == IV_OK && iv == 3,
        "c3: seeded fail_n == 3");
    v0 = iv_config_version(c);
    chk(v0 == 11u, "c3: version comes from the file");

    /* ---- 改内存 ---- */
    chk(iv_config_set_int(c, "net.wan.fail_n", 9) == IV_OK, "c3: set_int");
    chk(iv_config_set_str(c, "net.wan.mode", "wired-first") == IV_OK, "c3: set_str");
    chk(iv_config_set_bool(c, "feature.on", 1) == IV_OK, "c3: set_bool on a new key");
    chk(iv_config_get_int(c, "net.wan.fail_n", &iv) == IV_OK && iv == 9,
        "c3: memory reflects the new value");
    chk(iv_config_stat(c, &st) == IV_OK && st.dirty == 1, "c3: handle marked dirty");

    /* **关键**：此刻磁盘上还是旧值 —— set_* 不碰磁盘 */
    chk(read_file(nf, buf, sizeof(buf)) > 0, "c3: re-read the file");
    chk(contains(buf, "\"fail_n\": 3"), "c3: file still holds the OLD value before save");
    chk(contains(buf, "\"wireless-first\""), "c3: and the OLD mode");
    chk(!exists(tmpf), "c3: no tmp file yet");

    /* ---- save 才落盘 ---- */
    chk(iv_config_save(c, NULL, 0u) == IV_OK, "c3: save");
    chk(iv_config_version(c) == v0 + 1u, "c3: version incremented by exactly 1");
    chk(iv_config_stat(c, &st) == IV_OK && st.dirty == 0, "c3: dirty cleared after save");
    chk(!exists(tmpf), "c3: tmp file consumed by the rename");

    /* 另一句柄重新读盘，证明真的落上去了（而不是只改了内存） */
    c2 = iv_config_open(g_dir, "network");
    chk(c2 != NULL, "c3: second handle");
    if (c2 != NULL) {
        const char *s = NULL;

        chk(iv_config_get_int(c2, "net.wan.fail_n", &iv) == IV_OK && iv == 9,
            "c3: the new value survived a reload from disk");
        chk(iv_config_get_str(c2, "net.wan.mode", &s) == IV_OK && strcmp(s, "wired-first") == 0,
            "c3: the new string survived too");
        chk(iv_config_version(c2) == v0 + 1u, "c3: the bumped version is on disk");
        iv_config_close(c2);
    }

    /* ---- set_* 的参数与键名校验 ---- */
    chk(iv_config_set_str(c, "_meta.x", "y") == IV_EINVAL, "c3: '_' prefix is reserved");
    chk(iv_config_set_int(c, "a..b", 1) == IV_EINVAL, "c3: empty segment rejected");
    chk(iv_config_set_int(c, ".a", 1) == IV_EINVAL, "c3: leading dot rejected");
    chk(iv_config_set_int(c, "a.", 1) == IV_EINVAL, "c3: trailing dot rejected");
    chk(iv_config_set_int(c, "a.b.c.d.e", 1) == IV_EINVAL, "c3: too many segments rejected");
    chk(iv_config_set_str(c, NULL, "y") == IV_EINVAL, "c3: NULL key");
    chk(iv_config_set_str(c, "ok.key", NULL) == IV_EINVAL, "c3: NULL value");
    chk(iv_config_set_bool(NULL, "ok.key", 1) == IV_EINVAL, "c3: NULL handle");
    chk(iv_config_set_int(c, "ok.key", 1) == IV_OK, "c3: a normal unknown key is settable");

    iv_config_close(c);
}

/* ===========================================================================
 * F4 + F2：文件只给一个键，其余 8 个必须补齐；来源能区分"文件给的/默认补的"
 * 反向证伪：若 fill_missing_defaults 不跑，items 只有 1、from_default 为 0。
 * ========================================================================= */
static void case_fill_and_origin(void)
{
    static const char PARTIAL[] = "{\n  \"net\": { \"wan\": { \"fail_n\": 4 } }\n}\n";
    char            nf[200];
    iv_config_t    *c;
    iv_cfg_stat_t   st;
    iv_cfg_origin_t org;
    const char     *s  = NULL;
    int64_t         iv = 0;
    int             filled = -1;

    path_of(nf, sizeof(nf), "network.json");
    chk(write_file(nf, PARTIAL, 0644u) == 0, "c4: seed a partial network.json");

    c = iv_config_open(g_dir, "network");
    chk(c != NULL, "c4: open");
    if (c == NULL)
        return;

    chk(iv_config_stat(c, &st) == IV_OK, "c4: stat");
    chk(st.items == 9u, "c4: all 9 network keys are present");
    chk(st.from_file == 1u, "c4: exactly one key came from the file");
    chk(st.from_default == 8u, "c4: eight were filled from defaults");
    chk(st.invalid == 0u, "c4: a MISSING key is not an INVALID key");
    chk(st.loaded == 1, "c4: the file itself loaded fine");

    chk(iv_config_get_int(c, "net.wan.fail_n", &iv) == IV_OK && iv == 4,
        "c4: the file value wins over the default");
    chk(iv_config_get_str(c, "net.wan.mode", &s) == IV_OK && strcmp(s, "wireless-first") == 0,
        "c4: filled default mode");
    chk(iv_config_get_int(c, "net.probe.interval_ms", &iv) == IV_OK && iv == 5000,
        "c4: filled default interval_ms");
    chk(iv_config_get_int(c, "net.probe.timeout_ms", &iv) == IV_OK && iv == 2000,
        "c4: filled default timeout_ms");

    chk(iv_config_origin(c, "net.wan.fail_n", &org) == IV_OK && org == IV_CFG_FROM_FILE,
        "c4: origin FILE for the key we supplied");
    chk(iv_config_origin(c, "net.wan.mode", &org) == IV_OK && org == IV_CFG_FROM_DEFAULT,
        "c4: origin DEFAULT for the filled key");

    chk(iv_config_fill_defaults(c, &filled, NULL, 0u) == IV_OK, "c4: fill_defaults");
    chk(filled == 0, "c4: open() already filled everything, so there is nothing left");

    iv_config_close(c);
}

/* ===========================================================================
 * F4：文件把子树写成标量（`"net": 5`）必须判非法并剔除，不能让它把整棵
 *     net.* 默认子树挡在门外。
 * 反向证伪：去掉 item_check() 里的 is_default_parent 分支后，items 会退回 1、
 *           invalid 会退回 0，而这几条断言会立刻失败 —— 这正是 F6 静默清空
 *           运行中配置的那条路。
 * ========================================================================= */
static void case_parent_scalar_rejected(void)
{
    char          nf[200];
    iv_config_t  *c;
    iv_cfg_stat_t st;
    int64_t       iv = 0;

    path_of(nf, sizeof(nf), "network.json");
    chk(write_file(nf, "{ \"net\": 5 }\n", 0644u) == 0, "c5: seed {\"net\": 5}");

    c = iv_config_open(g_dir, "network");
    chk(c != NULL, "c5: open");
    if (c == NULL)
        return;

    chk(iv_config_stat(c, &st) == IV_OK, "c5: stat");
    chk(st.invalid == 1u, "c5: the scalar that shadows a subtree is counted as invalid");
    chk(st.items == 9u, "c5: the whole net.* default subtree is still there");
    chk(iv_config_get_int(c, "net.wan.fail_n", &iv) == IV_OK && iv == 3,
        "c5: defaults are usable again");
    chk(iv_config_get_int(c, "net", &iv) == IV_ENOENT, "c5: the offending key itself is gone");

    iv_config_close(c);
}

/* ===========================================================================
 * F5：抗断电写盘的两条可测性质
 *   ① 崩在半截的 `.tmp` 既不会被当成正式文件读，也不会在下次 save 后残留；
 *   ② save 走的是 rename 换 inode，不是原地改写 ——
 *      判据：save 之前就打开的 fd（指向旧 inode）在 save 之后必须仍读到旧内容。
 *      若实现改成 O_TRUNC 直接写正式文件，这个 fd 会立刻看到新内容，断言即失败。
 * ========================================================================= */
static void case_atomic_replace(void)
{
    char        pf[200];
    char        tmpf[200];
    char        buf[2048];
    char        oldbuf[1024];
    iv_config_t *c;
    int         old_fd;
    ssize_t     n;

    path_of(pf, sizeof(pf), "platform.json");
    path_of(tmpf, sizeof(tmpf), "platform.json.tmp");
    chk(write_file(pf, PLATFORM_JSON, 0644u) == 0, "c6: seed platform.json");

    /* 崩在写一半的现场：只剩一个半截 .tmp */
    chk(write_file(tmpf, "{ this is a half-written leftover", 0644u) == 0, "c6: seed a stale tmp");

    c = iv_config_open(g_dir, "platform");
    chk(c != NULL, "c6: open");
    if (c == NULL)
        return;
    {
        const char *s = NULL;

        chk(iv_config_get_str(c, "platform.proto.host", &s) == IV_OK && strcmp(s, "OLD") == 0,
            "c6: a half-written tmp is NOT used as the config");
    }

    old_fd = open(pf, O_RDONLY | O_CLOEXEC);
    chk(old_fd >= 0, "c6: hold the pre-save inode open");

    chk(iv_config_set_str(c, "platform.proto.host", "NEW") == IV_OK, "c6: set host=NEW");
    chk(iv_config_save(c, NULL, 0u) == IV_OK, "c6: save");

    /* 路径上读到新内容 */
    chk(read_file(pf, buf, sizeof(buf)) > 0 && contains(buf, "NEW") && !contains(buf, "OLD"),
        "c6: the path now holds the new content");

    /* 旧 inode 上仍是旧内容 ⇒ 走的是 rename 换 inode */
    if (old_fd >= 0) {
        n = read(old_fd, oldbuf, sizeof(oldbuf) - 1u);
        if (n < 0)
            n = 0;
        oldbuf[n] = '\0';
        chk(contains(oldbuf, "OLD") && !contains(oldbuf, "NEW"),
            "c6: the pre-save inode still holds the OLD content (rename, not in-place write)");
        (void)close(old_fd);
    }
    chk(!exists(tmpf), "c6: the stale tmp is consumed and gone after a successful save");

    iv_config_close(c);
}

/* ===========================================================================
 * F3 + F5：任何校验不过 ⇒ 一个字段都不落盘，文件逐字节不变
 * 反向证伪：若 save 先写后校验（或边写边校验），文件会被改坏，逐字节比较失败。
 * ========================================================================= */
static void case_reject_writes_nothing(void)
{
    char         nf[200];
    char         tmpf[200];
    char         before[2048];
    char         after[2048];
    char         detail[IV_CFG_DETAIL_MAX];
    iv_config_t *c;
    int64_t      iv = 0;

    path_of(nf, sizeof(nf), "network.json");
    path_of(tmpf, sizeof(tmpf), "network.json.tmp");
    chk(write_file(nf, NETWORK_JSON, 0644u) == 0, "c7: seed network.json");

    c = iv_config_open(g_dir, "network");
    chk(c != NULL, "c7: open");
    if (c == NULL)
        return;
    chk(read_file(nf, before, sizeof(before)) > 0, "c7: snapshot the file");

    /* ---- 范围越界（整数） ---- */
    chk(iv_config_set_int(c, "net.wan.fail_n", 0) == IV_OK, "c7: set an out-of-range value");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL, "c7: save refuses it");
    chk(detail[0] != '\0', "c7: detail explains which key failed");
    chk(contains(detail, "net.wan.fail_n"), "c7: detail names the offending key");
    chk(read_file(nf, after, sizeof(after)) > 0 && strcmp(before, after) == 0,
        "c7: the rejected save left the file byte-identical");
    chk(!exists(tmpf), "c7: and left no tmp behind");

    /* ---- 枚举越界（字符串形态的范围） ---- */
    chk(iv_config_set_int(c, "net.wan.fail_n", 3) == IV_OK, "c7: restore fail_n");
    chk(iv_config_set_str(c, "net.wan.mode", "bogus") == IV_OK, "c7: set a bad enum");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL, "c7: save refuses a bad enum");
    chk(contains(detail, "net.wan.mode"), "c7: detail names the enum key");

    /* ---- 跨字段：探活超时 >= 间隔 ---- */
    chk(iv_config_set_str(c, "net.wan.mode", "wireless-first") == IV_OK, "c7: restore mode");
    chk(iv_config_set_int(c, "net.probe.interval_ms", 1000) == IV_OK, "c7: set interval=1000");
    chk(iv_config_set_int(c, "net.probe.timeout_ms", 1000) == IV_OK, "c7: set timeout=1000");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL,
        "c7: save refuses timeout_ms >= interval_ms");
    chk(contains(detail, "cross-field"), "c7: detail marks it as a cross-field conflict");

    /* ---- 修好就能存（证明拒绝是"这条不变量"，不是"保存坏了"） ---- */
    chk(iv_config_set_int(c, "net.probe.timeout_ms", 500) == IV_OK, "c7: fix timeout");
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_OK, "c7: save succeeds after the fix");

    /* ---- 跨字段：双 WAN 主备不得同名 ---- */
    chk(iv_config_set_str(c, "net.wan.primary_if", "eth0") == IV_OK, "c7: primary_if=eth0");
    chk(iv_config_set_str(c, "net.wan.backup_if", "eth0") == IV_OK, "c7: backup_if=eth0");
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL,
        "c7: save refuses identical wan interfaces");
    chk(iv_config_set_str(c, "net.wan.backup_if", "wlan1") == IV_OK, "c7: backup_if=wlan1");
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_OK, "c7: distinct interfaces accepted");

    chk(iv_config_get_int(c, "net.probe.timeout_ms", &iv) == IV_OK && iv == 500,
        "c7: the accepted values are live");

    iv_config_close(c);
}

/* ===========================================================================
 * F3 + F5：一个键是另一个键的父路径（同一路径既要当叶子又要当对象）必须拒绝
 * 这里用**独立的一份配置**（名字 "net"，不在内置默认表里），免得把别的用例
 * 用的句柄弄成永久不可保存态。
 * 反向证伪：去掉 items_check() 里的 items_conflict() 后 save 会成功，断言失败。
 * ========================================================================= */
static void case_key_prefix_conflict(void)
{
    static const char SEED[] = "{\"net\":{\"wan\":{\"mode\":\"wireless-first\"}}}\n";
    char         nf[200];
    char         detail[IV_CFG_DETAIL_MAX];
    iv_config_t *c;
    const char  *s = NULL;

    path_of(nf, sizeof(nf), "net.json");
    chk(write_file(nf, SEED, 0644u) == 0, "c8: seed net.json");

    c = iv_config_open(g_dir, "net");
    chk(c != NULL, "c8: open");
    if (c == NULL)
        return;

    chk(iv_config_get_str(c, "net.wan.mode", &s) == IV_OK && strcmp(s, "wireless-first") == 0,
        "c8: seeded leaf value");

    /* net.wan.mode 是叶子；再加 net.wan.mode.x 会要求同一路径同时是叶子与对象 */
    chk(iv_config_set_str(c, "net.wan.mode.x", "y") == IV_OK,
        "c8: set a child under an existing leaf");
    detail[0] = '\0';
    chk(iv_config_save(c, detail, sizeof(detail)) == IV_EINVAL,
        "c8: save refuses the leaf/object conflict");
    chk(contains(detail, "prefix"), "c8: detail marks it as a prefix conflict");

    iv_config_close(c);
}

/* ===========================================================================
 * F6：外部改文件 ⇒ 不重启即生效
 *   ① 没改动时不重载；目录里别的文件动了也不重载（按文件名过滤有效）；
 *   ② 外部用"临时名 + rename"的原子写法 ⇒ 重载且新值生效；
 *   ③ **第二次** rename 也必须生效 —— 这是"watch 目录而不是 watch 文件"的证伪：
 *      若实现 watch 的是旧文件 inode，第一次 rename 之后 watch 就失效了，
 *      第二次改文件将永远收不到事件（计划 §S8 坑 2）。
 * ========================================================================= */
static void case_hot_reload(void)
{
    static const char V1[] =
        "{\n  \"_meta\": { \"version\": 50 },\n"
        "  \"net\": { \"wan\": { \"mode\": \"wired-first\", \"fail_n\": 8,\n"
        "                       \"primary_if\": \"wlan0\", \"backup_if\": \"eth0\" } }\n}\n";
    static const char V2[] =
        "{\n  \"_meta\": { \"version\": 51 },\n"
        "  \"net\": { \"wan\": { \"mode\": \"wireless-first\", \"fail_n\": 6,\n"
        "                       \"primary_if\": \"wlan0\", \"backup_if\": \"eth0\" } }\n}\n";
    char         nf[200];
    char         stage[200];
    char         other[200];
    iv_config_t *c;
    const char  *s  = NULL;
    uint32_t     rl = 0u;
    int          fd;

    path_of(nf, sizeof(nf), "network.json");
    path_of(stage, sizeof(stage), "network.stage");
    path_of(other, sizeof(other), "unrelated.json");
    chk(write_file(nf, NETWORK_JSON, 0644u) == 0, "c9: seed network.json");

    c = iv_config_open(g_dir, "network");
    chk(c != NULL, "c9: open");
    if (c == NULL)
        return;

    fd = iv_config_watch_fd(c);
    chk(fd >= 0, "c9: watch fd created");
    chk(iv_config_watch_fd(c) == fd, "c9: a repeated call hands back the same fd");
    chk(iv_config_watch_fd(NULL) == IV_EINVAL, "c9: watch_fd(NULL) == EINVAL");

    /* 没有任何改动 */
    rl = 99u;
    {
        int rc = iv_config_watch_poll(c, &rl);

        chk(rc == IV_EAGAIN || (rc == IV_OK && rl == 0u), "c9: nothing changed => no reload");
    }

    /* 目录里与本配置无关的文件被写：不得触发重载 */
    chk(write_file(other, "{}\n", 0644u) == 0, "c9: write an unrelated file");
    rl = 99u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl == 0u,
        "c9: another file in the same directory does not trigger a reload");

    /* 外部原子写 #1 */
    chk(write_file(stage, V1, 0644u) == 0, "c9: stage the new content");
    chk(rename(stage, nf) == 0, "c9: atomic swap #1");
    rl = 0u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl == 1u, "c9: rename triggers a reload");
    chk(iv_config_get_str(c, "net.wan.mode", &s) == IV_OK && strcmp(s, "wired-first") == 0,
        "c9: the new value is live");
    chk(iv_config_version(c) == 50u, "c9: the version follows the file");

    /* 外部原子写 #2 —— watch 目录的证伪点 */
    chk(write_file(stage, V2, 0644u) == 0, "c9: stage the second revision");
    chk(rename(stage, nf) == 0, "c9: atomic swap #2");
    rl = 0u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl == 1u,
        "c9: the SECOND rename also reloads (proves the watch is on the directory)");
    chk(iv_config_get_str(c, "net.wan.mode", &s) == IV_OK && strcmp(s, "wireless-first") == 0,
        "c9: the second value is live");

    iv_config_close(c);
}

/* ===========================================================================
 * F6 + F7：坏配置不停服 + 事务通知"要么全发、要么一个都不发"
 *   ① 越界值 / 半截文件 / 子树被写成标量 —— 三种坏法都必须整份拒绝、旧值一字不动；
 *   ② 被拒绝时**一个订阅者都不许收到通知**（否则下游会拿着半新半旧的配置跑）；
 *   ③ 随后一份合法文件必须立刻生效，且两个订阅者都收到；
 *   ④ 程序自己 save() 同样通知。
 * 反向证伪：去掉 watch_poll 里的 `|| c->invalid > 0` 后，(a) 会被当成成功重载，
 *           "旧值被保留""订阅者没被通知"两条断言立刻失败。
 * ========================================================================= */
static void case_bad_reload_rejected(void)
{
    static const char BAD_RANGE[] =
        "{\"_meta\":{\"version\":60},\"net\":{\"wan\":{\"fail_n\":999}}}\n";
    static const char BAD_TRUNC[] = "{\"net\":{\"wan\":{\"fail_n\":11";
    static const char BAD_SCALAR[] = "{\"_meta\":{\"version\":62},\"net\": 5}\n";
    static const char GOOD[] =
        "{\"_meta\":{\"version\":61},\"net\":{\"wan\":{\"fail_n\":7,\"mode\":\"wireless-first\",\n"
        "  \"primary_if\":\"wlan0\",\"backup_if\":\"eth0\"}}}\n";
    struct notify_ctx n1;
    struct notify_ctx n2;
    char         nf[200];
    char         stage[200];
    iv_config_t *c;
    const char  *s  = NULL;
    int64_t      iv = 0;
    uint32_t     rl = 0u;

    path_of(nf, sizeof(nf), "network.json");
    path_of(stage, sizeof(stage), "network.stage");
    chk(write_file(nf, NETWORK_JSON, 0644u) == 0, "c10: seed network.json");

    c = iv_config_open(g_dir, "network");
    chk(c != NULL, "c10: open");
    if (c == NULL)
        return;
    chk(iv_config_watch_fd(c) >= 0, "c10: watch");

    n1.count = 0;
    n2.count = 0;
    n1.last[0] = '\0';
    n2.last[0] = '\0';
    chk(iv_config_subscribe(c, on_notify, &n1) == IV_OK, "c10: subscribe #1");
    chk(iv_config_subscribe(c, on_notify, &n2) == IV_OK, "c10: subscribe #2");
    chk(iv_config_subscribe(c, NULL, NULL) == IV_EINVAL, "c10: NULL callback rejected");

    /* (a) 值越界 */
    chk(write_file(stage, BAD_RANGE, 0644u) == 0, "c10: stage an out-of-range file");
    chk(rename(stage, nf) == 0, "c10: swap in the out-of-range file");
    rl = 99u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl == 0u, "c10: an out-of-range file is rejected");
    chk(iv_config_get_int(c, "net.wan.fail_n", &iv) == IV_OK && iv == 3,
        "c10: the previous good value is kept");
    chk(iv_config_version(c) == 11u, "c10: the version is unchanged after a rejection");
    chk(n1.count == 0 && n2.count == 0, "c10: NO subscriber was notified");

    /* (b) 半截文件（写坏了） */
    chk(write_file(stage, BAD_TRUNC, 0644u) == 0, "c10: stage a truncated file");
    chk(rename(stage, nf) == 0, "c10: swap in the truncated file");
    rl = 99u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl == 0u, "c10: a truncated file is rejected");
    chk(iv_config_get_int(c, "net.wan.fail_n", &iv) == IV_OK && iv == 3,
        "c10: values survive a truncated file");
    chk(n1.count == 0 && n2.count == 0, "c10: still no notification");

    /* (c) 子树被写成标量 —— 若没有 is_default_parent 这道校验，
     *     这份文件会被当成"完全合法"而整份接受，把 net.* 全部清空 */
    chk(write_file(stage, BAD_SCALAR, 0644u) == 0, "c10: stage {\"net\": 5}");
    chk(rename(stage, nf) == 0, "c10: swap in the scalar file");
    rl = 99u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl == 0u, "c10: the scalar file is rejected");
    chk(iv_config_get_int(c, "net.wan.fail_n", &iv) == IV_OK && iv == 3,
        "c10: the whole subtree survives (this is the F6 silent-wipe regression)");
    chk(iv_config_get_str(c, "net.wan.mode", &s) == IV_OK && strcmp(s, "wireless-first") == 0,
        "c10: mode still readable");
    chk(n1.count == 0 && n2.count == 0, "c10: still no notification");

    /* (d) 一份合法文件 ⇒ 立刻生效，且两个订阅者都收到 */
    chk(write_file(stage, GOOD, 0644u) == 0, "c10: stage a good file");
    chk(rename(stage, nf) == 0, "c10: swap in the good file");
    rl = 0u;
    chk(iv_config_watch_poll(c, &rl) == IV_OK && rl == 1u,
        "c10: a good file reloads right after a rejected one");
    chk(iv_config_get_int(c, "net.wan.fail_n", &iv) == IV_OK && iv == 7,
        "c10: the new good value is live");
    chk(iv_config_version(c) == 61u, "c10: the new version is live");
    chk(n1.count == 1 && n2.count == 1, "c10: BOTH subscribers were notified (all-or-nothing)");
    chk(strcmp(n1.last, "network") == 0, "c10: the callback receives the config name");

    /* (e) 程序自己 save() 也要通知 */
    chk(iv_config_set_int(c, "net.wan.fail_n", 11) == IV_OK, "c10: set a new value");
    chk(iv_config_save(c, NULL, 0u) == IV_OK, "c10: save");
    chk(n1.count == 2 && n2.count == 2, "c10: save() notifies subscribers too");

    iv_config_close(c);
}

/* ===========================================================================
 * F6（真 Reactor）：按计划要求，热更新必须在**真的 iv_reactor_run() 回调**里
 * 走通一次完整回路 —— 只是手工调 watch_poll() 不算证明了"能挂进主循环"。
 * 回路：定时器回调里做一次外部原子写 ⇒ inotify fd 变可读 ⇒ reactor 分发 ⇒
 *       回调里 watch_poll() 重载 ⇒ 检测到重载即 stop。
 * 反向证伪：若 watch fd 没被注册进 reactor（或注册错事件），reloads 会保持 0，
 *           由 2s 兜底定时器结束循环，然后断言失败。
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

/* 模拟"外部程序"改配置：写临时名再 rename（编辑器 / Web 下发的原子写法） */
static void stage_and_swap(void *arg)
{
    struct reactor_ctx *x = (struct reactor_ctx *)arg;

    if (write_file(x->stage, x->text, 0644u) != 0 || rename(x->stage, x->target) != 0)
        x->failed = 1;
}

/* 兜底：万一事件没来，也要让 run() 退出，免得单测挂死 */
static void force_stop(void *arg)
{
    struct reactor_ctx *x = (struct reactor_ctx *)arg;

    iv_reactor_stop(x->r);
}

static void case_reactor_integration(void)
{
    static const char NEWCFG[] =
        "{\"_meta\":{\"version\":70},\"net\":{\"wan\":{\"mode\":\"wired-first\",\"fail_n\":4,\n"
        "  \"primary_if\":\"wlan0\",\"backup_if\":\"eth0\"}}}\n";
    struct reactor_ctx x;
    iv_reactor_t       *r;
    iv_config_t        *c;
    const char         *s = NULL;
    int                 fd;

    path_of(x.stage, sizeof(x.stage), "network.stage");
    path_of(x.target, sizeof(x.target), "network.json");
    chk(write_file(x.target, NETWORK_JSON, 0644u) == 0, "c11: seed network.json");

    c = iv_config_open(g_dir, "network");
    chk(c != NULL, "c11: open");
    if (c == NULL)
        return;

    r = iv_reactor_create(8);
    chk(r != NULL, "c11: reactor create");
    if (r == NULL) {
        iv_config_close(c);
        return;
    }

    fd = iv_config_watch_fd(c);
    chk(fd >= 0, "c11: watch fd");
    chk(iv_reactor_add(r, fd, IV_EV_READ, on_cfg_event, &x) != NULL,
        "c11: register the watch fd in the real reactor");

    x.r       = r;
    x.c       = c;
    x.text    = NEWCFG;
    x.reloads = 0;
    x.events  = 0;
    x.failed  = 0;

    chk(iv_timer_add(r, 40u, stage_and_swap, &x) != NULL, "c11: schedule the external edit");
    chk(iv_timer_add(r, 2000u, force_stop, &x) != NULL, "c11: schedule the safety stop");

    chk(iv_reactor_run(r) == IV_OK, "c11: reactor run returned cleanly");
    chk(x.failed == 0, "c11: the external atomic write actually happened");
    chk(x.reloads >= 1, "c11: the reload happened inside the real reactor loop");
    chk(iv_config_get_str(c, "net.wan.mode", &s) == IV_OK && strcmp(s, "wired-first") == 0,
        "c11: the config is live after the reactor pass");
    chk(iv_config_version(c) == 70u, "c11: version followed the file");

    iv_config_close(c);
    iv_reactor_destroy(r);
}

/* ===========================================================================
 * F8：密钥分离 —— 权限、符号链接、空文件、缓冲不足、参数非法
 * 反向证伪：① 去掉 fchmod 让权限随 umask 漂移，0644/0640/0601 三条会失败；
 *           ② 把 lstat 换成 stat，symlink 那条会失败；
 *           ③ 去掉"读满后再探一字节"，缓冲不足那条会退化成"截断后返回 OK"。
 * ========================================================================= */
static void case_secure_read(void)
{
    char p[240];
    char buf[64];

    chk(mkdir(g_sec, 0700) == 0 || errno == EEXIST, "c12: mkdir secure/");

    path_of(p, sizeof(p), "secure/token");
    chk(write_file(p, "s3cret-token\n", 0600u) == 0, "c12: write a 0600 token");
    chk(iv_config_secure_read(g_sec, "token", buf, sizeof(buf)) == IV_OK, "c12: read a 0600 file");
    chk(strcmp(buf, "s3cret-token") == 0, "c12: the trailing newline is stripped");

    path_of(p, sizeof(p), "secure/crlf");
    chk(write_file(p, "abc\r\n", 0600u) == 0, "c12: write a CRLF file");
    chk(iv_config_secure_read(g_sec, "crlf", buf, sizeof(buf)) == IV_OK && strcmp(buf, "abc") == 0,
        "c12: CR and LF are both stripped");

    /* 权限：组/其他位只要开了一位就拒 */
    path_of(p, sizeof(p), "secure/open644");
    chk(write_file(p, "x", 0644u) == 0, "c12: write a 0644 file");
    chk(iv_config_secure_read(g_sec, "open644", buf, sizeof(buf)) == IV_EAUTH,
        "c12: 0644 is rejected");

    path_of(p, sizeof(p), "secure/g640");
    chk(write_file(p, "x", 0640u) == 0, "c12: write a 0640 file");
    chk(iv_config_secure_read(g_sec, "g640", buf, sizeof(buf)) == IV_EAUTH, "c12: 0640 is rejected");

    path_of(p, sizeof(p), "secure/o601");
    chk(write_file(p, "x", 0601u) == 0, "c12: write a 0601 file");
    chk(iv_config_secure_read(g_sec, "o601", buf, sizeof(buf)) == IV_EAUTH, "c12: 0601 is rejected");

    path_of(p, sizeof(p), "secure/open600");
    chk(write_file(p, "x", 0600u) == 0, "c12: write a 0600 file");
    chk(iv_config_secure_read(g_sec, "open600", buf, sizeof(buf)) == IV_OK,
        "c12: 0600 is accepted (so the check really is the mode)");

    /* 符号链接：即使指向一个 0600 文件也不跟 */
    path_of(p, sizeof(p), "secure/link");
    if (symlink("token", p) == 0) {
        chk(iv_config_secure_read(g_sec, "link", buf, sizeof(buf)) == IV_EAUTH,
            "c12: a symlink is refused even when it points at a 0600 file");
        (void)unlink(p);
    } else {
        chk(0, "c12: symlink() failed");
    }

    /* 不存在 / 空文件 */
    chk(iv_config_secure_read(g_sec, "nope", buf, sizeof(buf)) == IV_ENOENT,
        "c12: a missing key file reports ENOENT (never a silent empty string)");

    path_of(p, sizeof(p), "secure/empty");
    chk(write_file(p, "", 0600u) == 0, "c12: write an empty 0600 file");
    chk(iv_config_secure_read(g_sec, "empty", buf, sizeof(buf)) == IV_ECORRUPT,
        "c12: an empty key file is corrupt, not an empty secret");

    /* 缓冲不足：必须 ERANGE，绝不静默截断 */
    path_of(p, sizeof(p), "secure/big");
    chk(write_file(p, "0123456789ABCDEF", 0600u) == 0, "c12: write a 16-byte secret");
    chk(iv_config_secure_read(g_sec, "big", buf, 8u) == IV_ERANGE,
        "c12: an undersized buffer is refused, never truncated");
    chk(iv_config_secure_read(g_sec, "big", buf, 16u) == IV_ERANGE,
        "c12: one byte short is still refused");
    chk(iv_config_secure_read(g_sec, "big", buf, 17u) == IV_OK &&
            strcmp(buf, "0123456789ABCDEF") == 0,
        "c12: an exactly sized buffer works");

    /* 参数非法 */
    chk(iv_config_secure_read(NULL, "token", buf, sizeof(buf)) == IV_EINVAL, "c12: NULL dir");
    chk(iv_config_secure_read("", "token", buf, sizeof(buf)) == IV_EINVAL, "c12: empty dir");
    chk(iv_config_secure_read(g_sec, NULL, buf, sizeof(buf)) == IV_EINVAL, "c12: NULL name");
    chk(iv_config_secure_read(g_sec, "", buf, sizeof(buf)) == IV_EINVAL, "c12: empty name");
    chk(iv_config_secure_read(g_sec, "token", NULL, 8u) == IV_EINVAL, "c12: NULL out");
    chk(iv_config_secure_read(g_sec, "token", buf, 0u) == IV_EINVAL, "c12: zero capacity");
    chk(iv_config_secure_read(g_sec, "a/b", buf, sizeof(buf)) == IV_EINVAL, "c12: '/' in name");
    chk(iv_config_secure_read(g_sec, "..", buf, sizeof(buf)) == IV_EINVAL, "c12: '..' in name");
}

/* ===========================================================================
 * 句柄池、配置名校验、未开 watch 就 poll、NULL 安全
 * 本用例同时是**句柄泄漏探测器**：它要求 4 个槽里恰好空出 4 个，
 * 任何一个前面的用例忘了 close，这里就会开不满。
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
        chk(h[i] != NULL, "c13: open until the handle pool is full");
    }
    chk(iv_config_open(g_dir, "h9") == NULL, "c13: the 5th handle is refused");

    /* 释放一个即可再用 */
    iv_config_close(h[0]);
    {
        iv_config_t *again = iv_config_open(g_dir, "h9");

        chk(again != NULL, "c13: a freed slot is reusable");
        iv_config_close(again);
    }
    for (i = 1; i < 4; i++)
        iv_config_close(h[i]);

    iv_config_close(NULL); /* 必须安全 */

    /* 配置名校验（防拼出目录外的路径） */
    chk(iv_config_open(g_dir, "") == NULL, "c13: an empty name is rejected");
    chk(iv_config_open(g_dir, "a/b") == NULL, "c13: '/' in the name is rejected");
    chk(iv_config_open(g_dir, "../esc") == NULL, "c13: '..' in the name is rejected");
    chk(iv_config_open(NULL, NULL) == NULL, "c13: NULL name is rejected");

    /* 没开 watch 就 poll */
    {
        iv_config_t *c  = iv_config_open(g_dir, "platform");
        uint32_t     rl = 0u;

        chk(c != NULL, "c13: open for the poll-state check");
        if (c != NULL) {
            chk(iv_config_watch_poll(c, &rl) == IV_ESTATE, "c13: poll before watch_fd == ESTATE");
            iv_config_close(c);
        }
    }
    chk(iv_config_watch_poll(NULL, NULL) == IV_EINVAL, "c13: poll with a NULL handle");
    chk(iv_config_version(NULL) == 0u, "c13: version of NULL is 0");
    chk(iv_config_stat(NULL, NULL) == IV_EINVAL, "c13: stat with NULL");
}

/* ===========================================================================
 * F4：配置目录不存在时按需创建 —— 否则"文件不存在也照常启动"补完默认后
 *     没有一个地方能落盘（S10 首次上板就是这个场景）。
 * ========================================================================= */
static void case_dir_autocreate(void)
{
    char          sub[200];
    char          pf[240];
    struct stat   sb;
    iv_config_t  *c;

    path_of(sub, sizeof(sub), "auto");
    (void)rmdir(sub); /* 确保一开始不存在 */

    c = iv_config_open(sub, "platform");
    chk(c != NULL, "c14: open into a non-existent directory still works");
    if (c != NULL) {
        chk(stat(sub, &sb) == 0 && S_ISDIR(sb.st_mode),
            "c14: the module created the config directory");
        iv_config_close(c);
    }

    path_of(pf, sizeof(pf), "auto/platform.json");
    (void)unlink(pf);
    (void)rmdir(sub);
}

/* ---------------------------------------------------------------------------
 * 收尾：把本用例造出来的东西全部删掉（AGENTS.md 规则 6）
 * ------------------------------------------------------------------------- */
static void cleanup(void)
{
    static const char *const files[] = {
        "platform.json",  "platform.json.tmp", "network.json",    "network.json.tmp",
        "network.stage",  "unrelated.json",    "net.json",        "net.json.tmp",
        "h1.json",        "h2.json",           "h3.json",         "h4.json",
        "h9.json",        "secure/token",      "secure/crlf",     "secure/open644",
        "secure/open600", "secure/g640",       "secure/o601",     "secure/link",
        "secure/empty",   "secure/big",        "auto/platform.json",
    };
    char   p[240];
    size_t i;

    for (i = 0u; i < sizeof(files) / sizeof(files[0]); i++) {
        path_of(p, sizeof(p), files[i]);
        (void)unlink(p);
    }
    path_of(p, sizeof(p), "secure");
    (void)rmdir(p);
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
    if (dl + 1u > sizeof(g_dir) || dl + 8u > sizeof(g_sec)) {
        fprintf(stderr, "FAIL: temp dir name too long\n");
        return 1;
    }
    memcpy(g_dir, d, dl + 1u);
    memcpy(g_sec, d, dl);
    memcpy(g_sec + dl, "/secure", 8u); /* 7 字符 + NUL */

    case_defaults_startup();
    case_read_typed();
    case_set_memory_then_save();
    case_fill_and_origin();
    case_parent_scalar_rejected();
    case_atomic_replace();
    case_reject_writes_nothing();
    case_key_prefix_conflict();
    case_hot_reload();
    case_bad_reload_rejected();
    case_reactor_integration();
    case_secure_read();
    case_pool_and_misc();
    case_dir_autocreate();

    cleanup();

    if (g_fail != 0) {
        fprintf(stderr, "test_config failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_config passed (F1 defaults, F2 typed read, F3 set/save, F4 fill, "
           "F5 atomic replace + reject, F6 hot reload + reactor, F7 notify, "
           "F8 secure read, pool)\n");
    return 0;
}











