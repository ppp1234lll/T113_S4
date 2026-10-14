/*
 * iv_upd —— 升级安装 / 双版本切换 / 回滚与启动包装（libivmodules，功能开发计划 M7-S7.2 ＋ S7.3）
 *
 * 契约与口径（落点、落盘顺序、掉电窗口、返回码映射）全在 `include/ivsbox/iv_upd.h`
 * 的文件头，本文件只写实现。几处实现层面的取舍单独记在这里：
 *
 * 1) **包字节格式在 iv_ota 里已定稿，为什么这里还要读一遍头？**
 *    验签器（S7.1）只回答"该不该收"，不把"负载从第几字节开始"吐出来。本模块要知道
 *    负载偏移才能逐文件解出来，于是在这里重新解析那 16 字节头（magic ＋ 两个长度）。
 *    **校验的权威仍是 `iv_ota_verify_file()`**，本文件里的头部解析只做"够不够格往下走"
 *    的边界检查，不承担任何安全判断 —— 重复读一遍是有意的：它保住了 S7.1 已交付、
 *    已验证的接口不动，代价是十几行无副作用的解析。
 *
 * 2) **暂存时重新算一遍 SHA-256，不是多余的**。
 *    `iv_ota_verify_file()` 用 `mmap` 核完摘要就把映射撤了，之后本模块再 `open` 一次
 *    来解包 —— 两次打开之间文件是可以被换掉的（TOCTOU）。所以解包时**边读边算**，
 *    与清单里的摘要逐个比对：核对的是"我正在写进盘的那几个字节"，而不是"刚才那一眼"。
 *    解包一律用 `pread`、不用 `mmap`：映射着的包被人并发截断会 SIGBUS（S7.1 头文件
 *    已登记该风险），`pread` 顶多返回短读，是可处理的错误。
 *
 * 3) **`chmod` 失败不报错，但要"回读"**。
 *    暂存期的 `fchmod` 只当尽力而为；安装完成后对 `bin/` 下的每个文件 `stat` 回读，
 *    `(mode & 0111) == 0` 才判 `IV_UPD_EPERM`。理由是"只读挂载 / 不支持权限位的
 *    文件系统会让 chmod 静默失效"，而这类"以为设了其实没设"是本工程反复踩到的
 *    一类 bug；判据放在安装后的真实目录上，才是真正会决定"起不起得来"的那份文件。
 *
 * 4) **不做任何清理式回滚**。
 *    安装失败就地返回、盘上留现场：半个暂存目录、多余的新版本目录、切了一半的链接。
 *    这些都由下一次 `iv_upd_boot_begin()` 按 `pending.json` ＋ `bootcount` 统一裁决，
 *    而不是在错误路径上现编一套"撤销逻辑"—— 错误路径上的清理代码是最难验证的部分，
 *    而它出错的方式恰恰是把设备弄成砖。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_ota.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_sha256.h"
#include "ivsbox/iv_upd.h"
#include "ivsbox/ivsbox.h" /* IVSBOX_VERSION_MAJOR/MINOR/PATCH */

/* 事件行上限：`<epoch> <event> <ver> <prev> <reason>\n`，256 绰绰有余。 */
#define IV_UPD_EVENT_MAX 256u
/* 单次 read/write 的分块大小（栈上）。 */
#define IV_UPD_CHUNK 16384u

/* ---------------------------------------------------------------------------
 * 编译期：本机 ELF 身份（准入检查的对照物）
 * ------------------------------------------------------------------------- */

#define IV_ELF_ET_EXEC 2u
#define IV_ELF_ET_DYN 3u
#define IV_ELF_PT_LOAD 1u
#define IV_ELF_PT_DYNAMIC 2u
#define IV_ELF_DT_NULL 0u
#define IV_ELF_DT_NEEDED 1u
#define IV_ELF_DT_STRTAB 5u
#define IV_ELF_DT_STRSZ 10u

/* 本机 ELF 身份（对照物）由 iv_upd.h 定义：单测要用同一套判据去"造一个本机 ELF"，
 * 所以它是接口的一部分，不藏在 .c 里。这里只起别名，让下面的代码短一点。 */
#define IV_ELF_MACHINE IV_UPD_ELF_MACHINE
#define IV_ELF_CLASS IV_UPD_ELF_CLASS
#define IV_ELF_DATA IV_UPD_ELF_DATA

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

static void set_why(iv_upd_reason_t *why, iv_upd_reason_t v)
{
    if (why != NULL)
        *why = v;
}

static int fail(iv_upd_reason_t *why, iv_upd_reason_t r)
{
    set_why(why, r);
    switch (r) {
    case IV_UPD_OK:      return IV_OK;
    case IV_UPD_ESIG:    return IV_EAUTH;
    case IV_UPD_ENOSPC:  return IV_ENOSPC;
    case IV_UPD_EFAULT:  return IV_EFAIL;
    case IV_UPD_ESTAGE:  return IV_ECORRUPT;
    case IV_UPD_EELF:    return IV_EPROTO;
    case IV_UPD_EDEPS:   return IV_ENOENT;
    case IV_UPD_EPERM:   return IV_ESTATE;
    case IV_UPD_ESWITCH: return IV_EIO;
    case IV_UPD_ESTATE:  return IV_ESTATE;
    case IV_UPD_EIO:     return IV_EIO;
    default:             return IV_EFAIL;
    }
}

/* 有界字符串拼接（本工程纪律：不使用 snprintf 往定长缓冲里写可变长串，
 * 那会在 -O2 下触发 -Wformat-truncation 而与 -Werror 冲突）。 */
typedef struct {
    char  *b;
    size_t cap;
    size_t n;
    int    ovf;
} sb_t;

static void sb_init(sb_t *s, char *b, size_t cap)
{
    s->b = b;
    s->cap = cap;
    s->n = 0u;
    s->ovf = 0;
    if (cap > 0u)
        b[0] = '\0';
}

static void sb_ch(sb_t *s, char c)
{
    if (s->n + 1u >= s->cap) {
        s->ovf = 1;
        return;
    }
    s->b[s->n++] = c;
    s->b[s->n] = '\0';
}

static void sb_write(sb_t *s, const char *p, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++)
        sb_ch(s, p[i]);
}

static void sb_str(sb_t *s, const char *p)
{
    if (p != NULL)
        sb_write(s, p, strlen(p));
}

static void sb_putu(sb_t *s, uint64_t v)
{
    char tmp[24];
    size_t k = 0u;
    size_t i;

    if (v == 0u) {
        sb_ch(s, '0');
        return;
    }
    while (v > 0u && k < sizeof tmp) {
        tmp[k++] = (char)('0' + (char)(v % 10u));
        v /= 10u;
    }
    for (i = k; i > 0u; i--)
        sb_ch(s, tmp[i - 1u]);
}

/* 事件字段过滤：只放行可见、无空格的 ASCII（空格是字段分隔符，`"`/`\` 是 JSON 保留字）。
 * 空串写成 `-`（否则连续两个空格会让读取端把字段数数错）。 */
static void sb_field(sb_t *s, const char *p)
{
    size_t i;
    size_t start = s->n;

    if (p != NULL) {
        for (i = 0u; p[i] != '\0'; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c <= 0x20u || c >= 0x7fu || c == (unsigned char)'"' || c == (unsigned char)'\\')
                sb_ch(s, '_');
            else
                sb_ch(s, (char)c);
        }
    }
    if (s->n == start)
        sb_ch(s, '-');
}

/* 路径拼接：a/b/c（任一段可为 NULL）。过长返回 IV_ERANGE（不截断、不部分写入）。 */
static int path3(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    sb_t s;

    sb_init(&s, out, cap);
    if (a != NULL) {
        sb_str(&s, a);
        if (b != NULL && s.n > 0u && s.b[s.n - 1u] != '/')
            sb_ch(&s, '/');
    }
    if (b != NULL) {
        sb_str(&s, b);
        if (c != NULL && s.n > 0u && s.b[s.n - 1u] != '/')
            sb_ch(&s, '/');
    }
    if (c != NULL)
        sb_str(&s, c);
    return s.ovf ? IV_ERANGE : IV_OK;
}

/* out = base ＋ suffix（用于 `.tmp` 这类同名临时文件）。 */
static int path_suffix(char *out, size_t cap, const char *base, const char *suffix)
{
    sb_t s;

    sb_init(&s, out, cap);
    sb_str(&s, base);
    sb_str(&s, suffix);
    return s.ovf ? IV_ERANGE : IV_OK;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0u;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (w == 0)
            return -1;
        done += (size_t)w;
    }
    return 0;
}

/* 读**最多** n 字节；`*got` 为实际读到的字节数（EOF 会短读，不算错误）。 */
static int read_at(int fd, void *buf, size_t n, off_t off, size_t *got)
{
    uint8_t *p = (uint8_t *)buf;
    size_t done = 0u;

    while (done < n) {
        ssize_t r = pread(fd, p + done, n - done, off + (off_t)done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        done += (size_t)r;
    }
    if (got != NULL)
        *got = done;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 目录 / 文件
 * ------------------------------------------------------------------------- */

static int mkdir_p(const char *path)
{
    char   tmp[IV_UPD_PATH_MAX];
    size_t n = strlen(path);
    size_t i;

    if (n == 0u || n >= sizeof tmp)
        return IV_ERANGE;
    memcpy(tmp, path, n + 1u);
    while (n > 1u && tmp[n - 1u] == '/')
        tmp[--n] = '\0';

    for (i = 1u; tmp[i] != '\0'; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return IV_EIO;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return IV_EIO;
    return IV_OK;
}

static int parent_of(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    size_t      n;

    if (slash == NULL) {
        if (cap < 2u)
            return IV_ERANGE;
        out[0] = '.';
        out[1] = '\0';
        return IV_OK;
    }
    n = (size_t)(slash - path);
    if (n == 0u)
        n = 1u; /* "/x" → "/" */
    if (n + 1u > cap)
        return IV_ERANGE;
    memcpy(out, path, n);
    out[n] = '\0';
    return IV_OK;
}

static int is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* 目录项落盘：rename/创建之后 fsync 父目录，否则挂掉后目录项可能还在缓存里。 */
static void sync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY);

    if (fd >= 0) {
        (void)fsync(fd);
        (void)close(fd);
    }
}

static void sync_parent_of(const char *path)
{
    char par[IV_UPD_PATH_MAX];

    if (parent_of(path, par, sizeof par) == IV_OK)
        sync_dir(par);
}

static int rm_rf(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return (errno == ENOENT) ? IV_OK : IV_EIO;

    if (S_ISDIR(st.st_mode)) {
        DIR           *d = opendir(path);
        struct dirent *e;
        int            rc = IV_OK;

        if (d == NULL)
            return IV_EIO;
        while ((e = readdir(d)) != NULL) {
            char child[IV_UPD_PATH_MAX];

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            if (path3(child, sizeof child, path, e->d_name, NULL) != IV_OK) {
                rc = IV_ERANGE;
                break;
            }
            rc = rm_rf(child);
            if (rc != IV_OK)
                break;
        }
        (void)closedir(d);
        if (rc != IV_OK)
            return rc;
        if (rmdir(path) != 0 && errno != ENOENT)
            return IV_EIO;
        return IV_OK;
    }
    if (unlink(path) != 0 && errno != ENOENT)
        return IV_EIO;
    return IV_OK;
}

static int copy_file(const char *src, const char *dst, uint32_t mode)
{
    uint8_t buf[IV_UPD_CHUNK];
    int     sfd;
    int     dfd;
    int     rc = IV_OK;

    sfd = open(src, O_RDONLY);
    if (sfd < 0)
        return IV_EIO;
    dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
    if (dfd < 0) {
        (void)close(sfd);
        return IV_EIO;
    }

    for (;;) {
        ssize_t n = read(sfd, buf, sizeof buf);

        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = IV_EIO;
            break;
        }
        if (write_full(dfd, buf, (size_t)n) != 0) {
            rc = IV_EIO;
            break;
        }
    }
    if (rc == IV_OK)
        (void)fchmod(dfd, (mode_t)mode);
    if (rc == IV_OK && fsync(dfd) != 0)
        rc = IV_EIO;
    (void)close(dfd);
    (void)close(sfd);
    return rc;
}

/* 整树复制（目录 / 普通文件 / 符号链接；其它类型跳过）。用于 adopt 与"暂存 → 版本目录"。 */
static int copy_tree(const char *src, const char *dst)
{
    struct stat st;

    if (lstat(src, &st) != 0)
        return (errno == ENOENT) ? IV_OK : IV_EIO;

    if (S_ISDIR(st.st_mode)) {
        DIR           *d;
        struct dirent *e;
        int            rc;

        rc = mkdir_p(dst);
        if (rc != IV_OK)
            return rc;
        d = opendir(src);
        if (d == NULL)
            return IV_EIO;
        while ((e = readdir(d)) != NULL) {
            char s2[IV_UPD_PATH_MAX];
            char d2[IV_UPD_PATH_MAX];

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            if (path3(s2, sizeof s2, src, e->d_name, NULL) != IV_OK ||
                path3(d2, sizeof d2, dst, e->d_name, NULL) != IV_OK) {
                rc = IV_ERANGE;
                break;
            }
            rc = copy_tree(s2, d2);
            if (rc != IV_OK)
                break;
        }
        (void)closedir(d);
        if (rc != IV_OK)
            return rc;
        (void)chmod(dst, st.st_mode & (mode_t)07777u);
        sync_dir(dst);
        return IV_OK;
    }

    if (S_ISLNK(st.st_mode)) {
        char    tgt[IV_UPD_PATH_MAX];
        ssize_t n = readlink(src, tgt, sizeof tgt - 1u);

        if (n < 0)
            return IV_EIO;
        tgt[n] = '\0';
        (void)unlink(dst);
        return (symlink(tgt, dst) == 0) ? IV_OK : IV_EIO;
    }

    if (S_ISREG(st.st_mode))
        return copy_file(src, dst, (uint32_t)(st.st_mode & (mode_t)07777u));

    return IV_OK; /* 设备节点 / FIFO 不做搬运 */
}

/* tmp ＋ fsync ＋ rename ＋ 父目录 fsync：掉电要么看到旧的、要么看到新的。 */
static int atomic_write(const char *path, const void *data, size_t len, mode_t mode)
{
    char tmp[IV_UPD_PATH_MAX];
    int  fd;
    int  rc = IV_OK;

    if (path_suffix(tmp, sizeof tmp, path, ".tmp") != IV_OK)
        return IV_ERANGE;
    {
        char par[IV_UPD_PATH_MAX];

        if (parent_of(path, par, sizeof par) == IV_OK && strcmp(par, ".") != 0) {
            if (mkdir_p(par) != IV_OK)
                return IV_EIO;
        }
    }
    (void)unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return IV_EIO;
    if (write_full(fd, data, len) != 0)
        rc = IV_EIO;
    if (rc == IV_OK && fsync(fd) != 0)
        rc = IV_EIO;
    (void)close(fd);
    if (rc == IV_OK && rename(tmp, path) != 0)
        rc = IV_EIO;
    if (rc != IV_OK) {
        (void)unlink(tmp);
        return rc;
    }
    sync_parent_of(path);
    return IV_OK;
}

/* dir/name → target（先写 dir/name.tmp 再 rename，切链接对读者是原子的）。 */
static int atomic_symlink(const char *dir, const char *name, const char *target)
{
    char lnk[IV_UPD_PATH_MAX];
    char tmp[IV_UPD_PATH_MAX];

    if (path3(lnk, sizeof lnk, dir, name, NULL) != IV_OK)
        return IV_ERANGE;
    if (path_suffix(tmp, sizeof tmp, lnk, ".tmp") != IV_OK)
        return IV_ERANGE;

    (void)unlink(tmp);
    if (symlink(target, tmp) != 0)
        return IV_EIO;
    if (rename(tmp, lnk) != 0) {
        (void)unlink(tmp);
        return IV_EIO;
    }
    sync_dir(dir);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 根路径解析
 * ------------------------------------------------------------------------- */

void iv_upd_cfg_default(iv_upd_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->app_root = IV_UPD_APP_ROOT_DEFAULT;
    cfg->data_root = IV_UPD_DATA_ROOT_DEFAULT;
    cfg->lib_paths = IV_UPD_LIB_PATHS_DEFAULT;
    cfg->dev_model = IV_UPD_DEV_MODEL_DEFAULT;
    cfg->dev_hw = IV_UPD_DEV_HW_DEFAULT;
    cfg->max_boot = IV_UPD_MAX_BOOT_DEFAULT;
    cfg->pubkey = NULL;
    cfg->fault_at = NULL;
}

/* 逐字段覆盖：调用方只填关心的项，其余保持默认。 */
static void cfg_resolve(const iv_upd_cfg_t *in, iv_upd_cfg_t *out)
{
    iv_upd_cfg_default(out);
    if (in == NULL)
        return;
    if (in->app_root != NULL)
        out->app_root = in->app_root;
    if (in->data_root != NULL)
        out->data_root = in->data_root;
    if (in->lib_paths != NULL)
        out->lib_paths = in->lib_paths;
    if (in->dev_model != NULL)
        out->dev_model = in->dev_model;
    if (in->dev_hw != 0u)
        out->dev_hw = in->dev_hw;
    if (in->pubkey != NULL)
        out->pubkey = in->pubkey;
    if (in->max_boot != 0u)
        out->max_boot = in->max_boot;
    if (in->fault_at != NULL)
        out->fault_at = in->fault_at;
}

static int fault_hit(const iv_upd_cfg_t *c, const char *phase)
{
    return c->fault_at != NULL && strcmp(c->fault_at, phase) == 0;
}

/* `<app_root>/<link>` 指向的目录名（不含路径）。没有则 IV_ENOENT。 */
static int link_target(const iv_upd_cfg_t *c, const char *name, char *out, size_t cap)
{
    char        p[IV_UPD_PATH_MAX];
    char        tgt[IV_UPD_PATH_MAX];
    ssize_t     n;
    const char *base;

    if (path3(p, sizeof p, c->app_root, name, NULL) != IV_OK)
        return IV_ERANGE;
    n = readlink(p, tgt, sizeof tgt - 1u);
    if (n < 0)
        return IV_ENOENT;
    tgt[n] = '\0';
    base = strrchr(tgt, '/');
    base = (base != NULL) ? base + 1 : tgt;
    if (strlen(base) >= cap)
        return IV_ERANGE;
    strcpy(out, base);
    return IV_OK;
}

static int current_target(const iv_upd_cfg_t *c, char *out, size_t cap)
{
    return link_target(c, IV_UPD_LINK_CURRENT, out, cap);
}

/* 本机当前版本：优先 current 指向的目录名，否则退回编译期 IVSBOX_VERSION_*。 */
static int self_version(const iv_upd_cfg_t *c, char *out, size_t cap)
{
    char         buf[IV_UPD_VER_MAX];
    iv_ota_ver_t v;

    if (current_target(c, buf, sizeof buf) == IV_OK &&
        iv_ota_ver_parse(buf, strlen(buf), &v) == IV_OK) {
        if (strlen(buf) >= cap)
            return IV_ERANGE;
        strcpy(out, buf);
        return IV_OK;
    }
    v.major = (uint16_t)IVSBOX_VERSION_MAJOR;
    v.minor = (uint16_t)IVSBOX_VERSION_MINOR;
    v.patch = (uint16_t)IVSBOX_VERSION_PATCH;
    iv_ota_ver_format(v, out, cap);
    return IV_OK;
}

static void self_from_cfg(const iv_upd_cfg_t *c, iv_ota_self_t *self)
{
    char buf[IV_UPD_VER_MAX];

    self->model = (c->dev_model != NULL) ? c->dev_model : IV_UPD_DEV_MODEL_DEFAULT;
    self->hw_ver = (c->dev_hw != 0u) ? c->dev_hw : IV_UPD_DEV_HW_DEFAULT;
    self->cur = (iv_ota_ver_t){ 0u, 0u, 0u };
    if (self_version(c, buf, sizeof buf) == IV_OK)
        (void)iv_ota_ver_parse(buf, strlen(buf), &self->cur);
}

static void key_from_cfg(const iv_upd_cfg_t *c, uint8_t out[IV_OTA_PUBKEY_LEN])
{
    if (c->pubkey != NULL) {
        memcpy(out, c->pubkey, IV_OTA_PUBKEY_LEN);
        return;
    }
    if (iv_ota_builtin_pubkey(out) != IV_OK)
        memset(out, 0, IV_OTA_PUBKEY_LEN); /* 未部署 ⇒ 全零 ⇒ iv_ota 判 no-pubkey */
}

/* 应启动的程序目录（绝对路径）：current 能解析就用它，否则裸布局根。 */
static int current_app_abs(const iv_upd_cfg_t *c, char *out, size_t cap)
{
    char p[IV_UPD_PATH_MAX];
    char rp[PATH_MAX];

    if (path3(p, sizeof p, c->app_root, IV_UPD_LINK_CURRENT, NULL) == IV_OK &&
        realpath(p, rp) != NULL) {
        if (strlen(rp) >= cap)
            return IV_ERANGE;
        strcpy(out, rp);
        return IV_OK;
    }
    if (strlen(c->app_root) >= cap)
        return IV_ERANGE;
    strcpy(out, c->app_root);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * pending.json（手写扁平 JSON，不引 json-c）
 * ------------------------------------------------------------------------- */

typedef struct {
    int          present;
    char         state[IV_UPD_STATE_MAX];
    char         ver_s[IV_UPD_VER_MAX];
    char         prev_s[IV_UPD_VER_MAX];
    char         app_dir[IV_UPD_PATH_MAX];
    uint32_t     bootcount;
    uint32_t     max_boot;
    char         reason[IV_UPD_REASON_MAX];
} pend_t;

static int pending_path(const iv_upd_cfg_t *c, char *out, size_t cap)
{
    return path3(out, cap, c->data_root, IV_UPD_OTA_SUBDIR, IV_UPD_PENDING_NAME);
}

static int events_path(const iv_upd_cfg_t *c, char *out, size_t cap)
{
    return path3(out, cap, c->data_root, IV_UPD_OTA_SUBDIR, IV_UPD_EVENTS_NAME);
}

/* 只放行 JSON 字符串字面量里安全的字符；有一个不合法就整体拒（不静默替换）。 */
static int json_safe(const char *src, size_t cap)
{
    size_t i;

    if (src == NULL || strlen(src) >= cap)
        return 0;
    for (i = 0u; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c <= 0x1fu || c >= 0x7fu || c == (unsigned char)'"' || c == (unsigned char)'\\')
            return 0;
    }
    return 1;
}

static int json_find(const char *buf, const char *key)
{
    char        pat[64];
    size_t      kl = strlen(key);
    const char *p;

    if (kl + 3u > sizeof pat)
        return -1;
    pat[0] = '"';
    memcpy(pat + 1, key, kl);
    pat[1 + kl] = '"';
    pat[2 + kl] = '\0';

    p = strstr(buf, pat);
    if (p == NULL)
        return -1;
    p += kl + 2u;
    while (*p == ' ')
        p++;
    if (*p != ':')
        return -1;
    p++;
    while (*p == ' ')
        p++;
    return (int)(p - buf);
}

static int json_get_str(const char *buf, const char *key, char *out, size_t cap)
{
    int   off = json_find(buf, key);
    const char *p;
    size_t n = 0u;

    if (off < 0)
        return -1;
    p = buf + off;
    if (*p != '"')
        return -1;
    p++;
    while (*p != '\0' && *p != '"') {
        if (n + 1u >= cap)
            return -1;
        out[n++] = *p++;
    }
    if (*p != '"')
        return -1;
    out[n] = '\0';
    return 0;
}

static int json_get_u32(const char *buf, const char *key, uint32_t *out)
{
    int      off = json_find(buf, key);
    const char *p;
    uint64_t v = 0u;
    int      any = 0;

    if (off < 0)
        return -1;
    p = buf + off;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > 0xffffffffu)
            return -1;
        p++;
        any = 1;
    }
    if (!any)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int pending_write(const iv_upd_cfg_t *c, const char *state, const char *ver,
                         const char *prev, const char *app_dir, uint32_t bootcount,
                         uint32_t max_boot, const char *reason)
{
    char  path[IV_UPD_PATH_MAX];
    char  buf[1024];
    sb_t  s;
    const char *st = (state != NULL) ? state : "pending";
    const char *v = (ver != NULL) ? ver : "0.0.0";
    const char *pv = (prev != NULL) ? prev : "";
    const char *ad = (app_dir != NULL) ? app_dir : "";
    const char *rs = (reason != NULL) ? reason : "";

    if (pending_path(c, path, sizeof path) != IV_OK)
        return IV_ERANGE;
    if (!json_safe(st, IV_UPD_STATE_MAX) || !json_safe(v, IV_UPD_VER_MAX) ||
        !json_safe(pv, IV_UPD_VER_MAX) || !json_safe(ad, IV_UPD_PATH_MAX) ||
        !json_safe(rs, IV_UPD_REASON_MAX))
        return IV_EINVAL;

    sb_init(&s, buf, sizeof buf);
    sb_str(&s, "{\"fmt\":1,\"state\":\"");
    sb_str(&s, st);
    sb_str(&s, "\",\"version\":\"");
    sb_str(&s, v);
    sb_str(&s, "\",\"previous\":\"");
    sb_str(&s, pv);
    sb_str(&s, "\",\"app_dir\":\"");
    sb_str(&s, ad);
    sb_str(&s, "\",\"bootcount\":");
    sb_putu(&s, bootcount);
    sb_str(&s, ",\"max_boot\":");
    sb_putu(&s, max_boot);
    sb_str(&s, ",\"reason\":\"");
    sb_str(&s, rs);
    sb_str(&s, "\"}\n");
    if (s.ovf)
        return IV_ERANGE;
    return atomic_write(path, buf, s.n, 0644);
}

static void pend_read(const iv_upd_cfg_t *c, pend_t *out)
{
    char   path[IV_UPD_PATH_MAX];
    char   buf[1024];
    int    fd;
    size_t got = 0u;

    memset(out, 0, sizeof *out);
    if (pending_path(c, path, sizeof path) != IV_OK)
        return;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    if (read_at(fd, buf, sizeof buf - 1u, 0, &got) != 0) {
        (void)close(fd);
        return;
    }
    (void)close(fd);
    buf[got] = '\0';
    if (got == 0u)
        return;

    out->present = 1;
    (void)json_get_str(buf, "state", out->state, sizeof out->state);
    (void)json_get_str(buf, "version", out->ver_s, sizeof out->ver_s);
    (void)json_get_str(buf, "previous", out->prev_s, sizeof out->prev_s);
    (void)json_get_str(buf, "app_dir", out->app_dir, sizeof out->app_dir);
    (void)json_get_str(buf, "reason", out->reason, sizeof out->reason);
    (void)json_get_u32(buf, "bootcount", &out->bootcount);
    (void)json_get_u32(buf, "max_boot", &out->max_boot);
}

/* ---------------------------------------------------------------------------
 * events.log（append ＋ fsync；读取端只做"数 rollback 行 ＋ 取最后一行"）
 * ------------------------------------------------------------------------- */

static int event_append(const iv_upd_cfg_t *c, const char *event, const char *ver,
                        const char *prev, const char *reason)
{
    char   path[IV_UPD_PATH_MAX];
    char   par[IV_UPD_PATH_MAX];
    char   line[IV_UPD_EVENT_MAX];
    sb_t   s;
    int    fd;
    int    rc;

    if (events_path(c, path, sizeof path) != IV_OK)
        return IV_ERANGE;
    if (parent_of(path, par, sizeof par) == IV_OK && strcmp(par, ".") != 0) {
        if (mkdir_p(par) != IV_OK)
            return IV_EIO;
    }

    sb_init(&s, line, sizeof line);
    {
        time_t now = time(NULL);

        sb_putu(&s, (now > (time_t)0) ? (uint64_t)now : 0u);
    }
    sb_ch(&s, ' ');
    sb_field(&s, event);
    sb_ch(&s, ' ');
    sb_field(&s, ver);
    sb_ch(&s, ' ');
    sb_field(&s, prev);
    sb_ch(&s, ' ');
    sb_field(&s, reason);
    sb_ch(&s, '\n');
    if (s.ovf)
        return IV_ERANGE;

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return IV_EIO;
    rc = (write_full(fd, line, s.n) == 0) ? IV_OK : IV_EIO;
    if (rc == IV_OK && fsync(fd) != 0)
        rc = IV_EIO;
    (void)close(fd);
    return rc;
}

static void ev_copy(char *dst, size_t cap, const char *src, size_t len)
{
    size_t i;

    if (cap == 0u)
        return;
    for (i = 0u; i + 1u < cap && i < len; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* 解析一行：`<epoch> <event> <ver> <prev> <reason>`（reason 不含空格，见 sb_field）。 */
static void ev_feed(const char *line, size_t len, uint32_t *rollbacks, char *lev, size_t lev_cap,
                    char *lrs, size_t lrs_cap)
{
    const char *tok[5];
    size_t      tlen[5];
    int         n = 0;
    size_t      i = 0u;

    while (n < 5) {
        size_t start;

        while (i < len && line[i] == ' ')
            i++;
        if (i >= len)
            break;
        start = i;
        while (i < len && line[i] != ' ')
            i++;
        tok[n] = line + start;
        tlen[n] = i - start;
        n++;
    }
    if (n < 2)
        return;
    if (tlen[1] == 8u && memcmp(tok[1], "rollback", 8u) == 0)
        *rollbacks += 1u;
    ev_copy(lev, lev_cap, tok[1], tlen[1]);
    if (n >= 5)
        ev_copy(lrs, lrs_cap, tok[4], tlen[4]);
    else
        ev_copy(lrs, lrs_cap, "", 0u);
}

static void events_scan(const char *path, uint32_t *rollbacks, char *lev, size_t lev_cap,
                        char *lrs, size_t lrs_cap)
{
    int     fd;
    off_t   size;
    off_t   start = 0;
    char    buf[4096];
    char    line[IV_UPD_EVENT_MAX];
    size_t  linelen = 0u;
    int     skip_head;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        (void)close(fd);
        return;
    }
    if ((uint64_t)size > IV_UPD_EVENTS_SCAN_MAX)
        start = size - (off_t)IV_UPD_EVENTS_SCAN_MAX;
    if (lseek(fd, start, SEEK_SET) < 0) {
        (void)close(fd);
        return;
    }
    skip_head = (start != 0) ? 1 : 0;

    for (;;) {
        ssize_t got = read(fd, buf, sizeof buf);
        ssize_t i;

        if (got <= 0)
            break;
        for (i = 0; i < got; i++) {
            char ch = buf[i];

            if (skip_head) {
                if (ch == '\n')
                    skip_head = 0;
                continue;
            }
            if (ch == '\n') {
                ev_feed(line, linelen, rollbacks, lev, lev_cap, lrs, lrs_cap);
                linelen = 0u;
            } else if (linelen + 1u < sizeof line) {
                line[linelen++] = ch;
            }
        }
    }
    if (linelen > 0u)
        ev_feed(line, linelen, rollbacks, lev, lev_cap, lrs, lrs_cap);
    (void)close(fd);
}

/* ---------------------------------------------------------------------------
 * 准入检查：ELF 架构 ＋ NEEDED 依赖
 * ------------------------------------------------------------------------- */

static uint16_t u16_at(const uint8_t *p, int le)
{
    if (le != 0)
        return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
    return (uint16_t)((uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t u32_at(const uint8_t *p, int le)
{
    if (le != 0) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);
    }
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint64_t u64_at(const uint8_t *p, int le)
{
    uint64_t v = 0u;
    unsigned i;

    /* 小端：第 i 字节是第 i 个 8 位组（最低位在前）；大端反之。
     * 两种写法不能混用：写成"循环左移 8 位再或"对**大端**成立，对小端会把字节序整个
     * 倒过来 —— 一个 0x40 的 e_phoff 会变成 0x4000000000000000。 */
    for (i = 0u; i < 8u; i++) {
        if (le != 0)
            v |= (uint64_t)p[i] << (8u * i);
        else
            v = (v << 8) | (uint64_t)p[i];
    }
    return v;
}

/* 在 `:` 分隔的目录表里找 name（DIR/name 存在即可）。 */
static int lib_in_paths(const char *name, size_t nlen, const char *paths, char *tmp, size_t cap)
{
    const char *p = paths;

    while (p != NULL && *p != '\0') {
        const char *colon = strchr(p, ':');
        size_t      dlen = (colon != NULL) ? (size_t)(colon - p) : strlen(p);

        if (dlen > 0u) {
            sb_t s;

            sb_init(&s, tmp, cap);
            sb_write(&s, p, dlen);
            if (s.n > 0u && s.b[s.n - 1u] != '/')
                sb_ch(&s, '/');
            sb_write(&s, name, nlen);
            if (!s.ovf && access(tmp, F_OK) == 0)
                return 1;
        }
        p = (colon != NULL) ? colon + 1 : NULL;
    }
    return 0;
}

int iv_upd_check_elf(const char *path, const char *lib_paths, char *missing, size_t miss_cap,
                     iv_upd_reason_t *why)
{
    struct {
        uint64_t off;
        uint64_t vaddr;
        uint64_t filesz;
    } loads[16];

    uint8_t  hdr[64];
    uint8_t  phbuf[64u * 56u];
    uint8_t  dynbuf[16384];
    char     strbuf[16384];
    char     pathbuf[IV_UPD_PATH_MAX];
    const char *lp = (lib_paths != NULL) ? lib_paths : IV_UPD_LIB_PATHS_DEFAULT;

    int      fd;
    size_t   got;
    int      nload = 0;
    int      cls;
    int      le;
    uint16_t etype;
    uint16_t emach;
    uint16_t phentsize;
    uint16_t phnum;
    uint64_t phoff;
    uint64_t dyn_off = 0u;
    uint64_t dyn_size = 0u;
    uint64_t strtab_va = 0u;
    uint64_t strsz = 0u;
    size_t   esz;
    size_t   dsize;
    size_t   k;
    size_t   expect_ph;
    int      i;
    int      rc = IV_OK;

    if (missing != NULL && miss_cap > 0u)
        missing[0] = '\0';
    if (path == NULL)
        return IV_EINVAL;
    set_why(why, IV_UPD_OK);

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return fail(why, IV_UPD_EIO);

    if (read_at(fd, hdr, sizeof hdr, 0, &got) != 0 || got < 52u) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF);
    }
    if (memcmp(hdr, "\x7f" "ELF", 4u) != 0) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF);
    }
    cls = (int)hdr[4];
    if (cls != 1 && cls != 2) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF);
    }
    if ((int)hdr[5] != (int)IV_ELF_DATA || cls != (int)IV_ELF_CLASS) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF);
    }
    if (cls == 2 && got < 64u) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF);
    }
    le = (hdr[5] == 1u) ? 1 : 0;

    etype = u16_at(hdr + 16, le);
    emach = u16_at(hdr + 18, le);
    if (etype != IV_ELF_ET_EXEC && etype != IV_ELF_ET_DYN) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF); /* .o / core 文件不是可安装的程序 */
    }
    if (emach != (uint16_t)IV_ELF_MACHINE) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF);
    }

    if (cls == 2) {
        phoff = u64_at(hdr + 32, le);
        phentsize = u16_at(hdr + 54, le);
        phnum = u16_at(hdr + 56, le);
        expect_ph = 56u;
    } else {
        phoff = (uint64_t)u32_at(hdr + 28, le);
        phentsize = u16_at(hdr + 42, le);
        phnum = u16_at(hdr + 44, le);
        expect_ph = 32u;
    }
    if (phentsize != expect_ph || phnum == 0u || phnum > 64u || phoff == 0u) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF);
    }
    if (read_at(fd, phbuf, (size_t)phnum * expect_ph, (off_t)phoff, &got) != 0 ||
        got != (size_t)phnum * expect_ph) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF);
    }

    for (i = 0; i < (int)phnum; i++) {
        const uint8_t *ph = phbuf + (size_t)i * expect_ph;
        uint32_t       ptype = u32_at(ph, le);
        uint64_t       poff;
        uint64_t       pva;
        uint64_t       pfs;

        if (cls == 2) {
            poff = u64_at(ph + 8, le);
            pva = u64_at(ph + 16, le);
            pfs = u64_at(ph + 32, le);
        } else {
            poff = (uint64_t)u32_at(ph + 4, le);
            pva = (uint64_t)u32_at(ph + 8, le);
            pfs = (uint64_t)u32_at(ph + 16, le);
        }
        if (ptype == IV_ELF_PT_LOAD) {
            if (nload < 16) {
                loads[nload].off = poff;
                loads[nload].vaddr = pva;
                loads[nload].filesz = pfs;
                nload++;
            }
        } else if (ptype == IV_ELF_PT_DYNAMIC) {
            dyn_off = poff;
            dyn_size = pfs;
        }
    }

    /* 静态链接的二进制没有 PT_DYNAMIC：没有 NEEDED 可查 ⇒ 直接放行。 */
    esz = (cls == 2) ? 16u : 8u;
    if (dyn_off == 0u || dyn_size < (uint64_t)esz) {
        (void)close(fd);
        return IV_OK;
    }

    dsize = (dyn_size < (uint64_t)sizeof dynbuf) ? (size_t)dyn_size : sizeof dynbuf;
    if (read_at(fd, dynbuf, dsize, (off_t)dyn_off, &got) != 0 || got != dsize) {
        (void)close(fd);
        return fail(why, IV_UPD_EELF);
    }

    for (k = 0u; k + esz <= dsize; k += esz) {
        uint64_t tag = (cls == 2) ? u64_at(dynbuf + k, le) : (uint64_t)u32_at(dynbuf + k, le);
        uint64_t val =
            (cls == 2) ? u64_at(dynbuf + k + 8u, le) : (uint64_t)u32_at(dynbuf + k + 4u, le);

        if (tag == IV_ELF_DT_NULL)
            break;
        if (tag == IV_ELF_DT_STRTAB)
            strtab_va = val;
        else if (tag == IV_ELF_DT_STRSZ)
            strsz = val;
    }
    if (strtab_va == 0u || strsz == 0u) {
        (void)close(fd);
        return IV_OK; /* 没有字符串表 ⇒ 读不出名字，不做误判 */
    }

    /* vaddr → 文件偏移（走 PT_LOAD）。 */
    {
        uint64_t soff = 0u;
        int      found = 0;

        for (i = 0; i < nload; i++) {
            if (strtab_va >= loads[i].vaddr && strtab_va < loads[i].vaddr + loads[i].filesz) {
                soff = loads[i].off + (strtab_va - loads[i].vaddr);
                found = 1;
                break;
            }
        }
        if (found == 0) {
            (void)close(fd);
            return IV_OK;
        }
        if (strsz >= (uint64_t)sizeof strbuf)
            strsz = (uint64_t)sizeof strbuf - 1u;
        if (read_at(fd, strbuf, (size_t)strsz, (off_t)soff, &got) != 0 || got != (size_t)strsz) {
            (void)close(fd);
            return IV_OK;
        }
        strbuf[got] = '\0';
    }

    for (k = 0u; k + esz <= dsize; k += esz) {
        uint64_t    tag = (cls == 2) ? u64_at(dynbuf + k, le) : (uint64_t)u32_at(dynbuf + k, le);
        uint64_t    val =
            (cls == 2) ? u64_at(dynbuf + k + 8u, le) : (uint64_t)u32_at(dynbuf + k + 4u, le);
        const char *nm;
        size_t      nlen;
        int         ok;

        if (tag == IV_ELF_DT_NULL)
            break;
        if (tag != IV_ELF_DT_NEEDED || val >= strsz)
            continue;
        nm = strbuf + val;
        nlen = strnlen(nm, (size_t)(strsz - val));
        if (nlen == 0u)
            continue;
        ok = lib_in_paths(nm, nlen, lp, pathbuf, sizeof pathbuf);
        if (ok == 0) {
            const char *env = getenv("LD_LIBRARY_PATH");

            if (env != NULL && env[0] != '\0')
                ok = lib_in_paths(nm, nlen, env, pathbuf, sizeof pathbuf);
        }
        if (ok == 0) {
            if (missing != NULL && miss_cap > 0u)
                ev_copy(missing, miss_cap, nm, nlen);
            rc = fail(why, IV_UPD_EDEPS);
            break;
        }
    }

    (void)close(fd);
    return rc;
}

/* ---------------------------------------------------------------------------
 * 落点计算
 * ------------------------------------------------------------------------- */

static uint32_t mode_of_rel(const char *rel)
{
    return (strncmp(rel, IV_UPD_PROG_PREFIX, sizeof(IV_UPD_PROG_PREFIX) - 1u) == 0)
               ? IV_UPD_MODE_PROG
               : IV_UPD_MODE_DATA;
}

static int is_prog_rel(const char *rel)
{
    return strncmp(rel, IV_UPD_PROG_PREFIX, sizeof(IV_UPD_PROG_PREFIX) - 1u) == 0;
}

static int fs_free(const char *path, uint64_t *out)
{
    struct statvfs vfs;

    if (statvfs(path, &vfs) != 0)
        return IV_EIO;
    *out = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    return IV_OK;
}

/* 包头的两个长度（只是"够不够格往下走"的边界检查；权威校验在 iv_ota）。 */
static int pkg_header(int fd, uint32_t *mlen, uint32_t *plen)
{
    uint8_t h[IV_OTA_HEADER_LEN];
    size_t  got = 0u;

    if (read_at(fd, h, sizeof h, 0, &got) != 0 || got != sizeof h)
        return IV_EPROTO;
    if (memcmp(h, IV_OTA_MAGIC, IV_OTA_MAGIC_LEN) != 0)
        return IV_EPROTO;
    *mlen = u32_at(h + 8, 1);
    *plen = u32_at(h + 12, 1);
    if (*mlen == 0u || *mlen > IV_OTA_MANIFEST_MAX || *plen == 0u)
        return IV_EPROTO;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 安装
 * ------------------------------------------------------------------------- */

/* 从包内 `off` 处取 len 字节写到 dst，**边写边算 SHA-256** 并与清单比对。
 * `chmod` 只当尽力而为（失败留给安装后的"回读"判 EPERM）。 */
static int extract_one(int fd, off_t off, uint32_t len, const char *dst, uint32_t mode,
                       const uint8_t want[IV_OTA_SHA256_LEN])
{
    uint8_t     buf[IV_UPD_CHUNK];
    iv_sha256_t sh;
    uint8_t     got_digest[IV_OTA_SHA256_LEN];
    char        par[IV_UPD_PATH_MAX];
    uint32_t    left = len;
    off_t       cur = off;
    int         dfd;
    int         rc = IV_OK;

    if (parent_of(dst, par, sizeof par) == IV_OK && strcmp(par, ".") != 0) {
        if (mkdir_p(par) != IV_OK)
            return IV_EIO;
    }

    dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
    if (dfd < 0)
        return IV_EIO;

    iv_sha256_init(&sh);
    while (left > 0u) {
        size_t want = (left < sizeof buf) ? (size_t)left : sizeof buf;
        size_t got = 0u;

        if (read_at(fd, buf, want, cur, &got) != 0 || got != want) {
            rc = IV_ECORRUPT; /* 包比清单声明的短 ⇒ 截断 */
            break;
        }
        if (write_full(dfd, buf, want) != 0) {
            rc = IV_EIO;
            break;
        }
        iv_sha256_update(&sh, buf, want);
        cur += (off_t)want;
        left -= (uint32_t)want;
    }
    if (rc == IV_OK) {
        iv_sha256_final(&sh, got_digest);
        if (memcmp(got_digest, want, IV_OTA_SHA256_LEN) != 0)
            rc = IV_ECORRUPT;
    }
    if (rc == IV_OK && fsync(dfd) != 0)
        rc = IV_EIO;
    (void)fchmod(dfd, (mode_t)mode); /* 尽力而为，失败由安装后的回读判 EPERM */
    (void)close(dfd);
    return rc;
}

/* 裸布局收养（幂等）。返回 IV_OK / IV_ENOENT（没有可收养的东西）/ 其它负码。 */
static int do_adopt(const iv_upd_cfg_t *c, iv_upd_reason_t *why)
{
    char cur[IV_UPD_VER_MAX];
    char rel[IV_UPD_PATH_MAX];
    char relv[IV_UPD_PATH_MAX];
    char src[IV_UPD_PATH_MAX];
    char dst[IV_UPD_PATH_MAX];
    char tgt[IV_UPD_PATH_MAX];
    int  copied = 0;
    int  rc;

    if (current_target(c, cur, sizeof cur) == IV_OK)
        return IV_OK; /* 已收养过 */

    if (self_version(c, cur, sizeof cur) != IV_OK)
        return fail(why, IV_UPD_EIO);
    if (path3(rel, sizeof rel, c->app_root, IV_UPD_RELEASES_SUBDIR, NULL) != IV_OK ||
        path3(relv, sizeof relv, rel, cur, NULL) != IV_OK)
        return fail(why, IV_UPD_EIO);

    if (!is_dir(relv)) {
        static const char *const parts[2] = { "bin", "scripts" };
        int i;

        if (mkdir_p(rel) != IV_OK || mkdir_p(relv) != IV_OK)
            return fail(why, IV_UPD_EIO);
        for (i = 0; i < 2; i++) {
            if (path3(src, sizeof src, c->app_root, parts[i], NULL) != IV_OK ||
                path3(dst, sizeof dst, relv, parts[i], NULL) != IV_OK)
                return fail(why, IV_UPD_EIO);
            if (!is_dir(src))
                continue;
            rc = copy_tree(src, dst);
            if (rc != IV_OK)
                return fail(why, IV_UPD_EIO);
            copied = 1;
        }
        if (copied == 0) {
            (void)rm_rf(relv);
            return IV_ENOENT; /* 裸布局也没有：全新设备，没有"上一版本"可言 */
        }
    }

    if (path3(tgt, sizeof tgt, IV_UPD_RELEASES_SUBDIR, cur, NULL) != IV_OK)
        return fail(why, IV_UPD_EIO);
    if (atomic_symlink(c->app_root, IV_UPD_LINK_CURRENT, tgt) != IV_OK)
        return fail(why, IV_UPD_ESWITCH);
    (void)event_append(c, "adopt", cur, "", "ok");
    return IV_OK;
}

int iv_upd_adopt(const iv_upd_cfg_t *cfg, iv_upd_reason_t *why)
{
    iv_upd_cfg_t c;

    set_why(why, IV_UPD_OK);
    cfg_resolve(cfg, &c);
    if (mkdir_p(c.app_root) != IV_OK)
        return fail(why, IV_UPD_EIO);
    return do_adopt(&c, why);
}

int iv_upd_install(const char *pkg_path, const iv_upd_cfg_t *cfg, iv_upd_reason_t *why)
{
    iv_upd_cfg_t      c;
    iv_ota_self_t     self;
    iv_ota_manifest_t man;
    iv_ota_reason_t   owhy = IV_OTA_OK;
    uint8_t           pub[IV_OTA_PUBKEY_LEN];
    char              ver[IV_UPD_VER_MAX];
    char              prev[IV_UPD_VER_MAX];
    char              rels[IV_UPD_PATH_MAX];
    char              stag[IV_UPD_PATH_MAX];
    char              stagv[IV_UPD_PATH_MAX];
    char              rel[IV_UPD_PATH_MAX];
    char              tgt[IV_UPD_PATH_MAX];
    char              missing[IV_UPD_PATH_MAX];
    uint32_t          mlen = 0u;
    uint32_t          plen = 0u;
    uint32_t          maxb;
    uint64_t          freeb = 0u;
    off_t             off;
    iv_ota_ver_t      pv;
    int               fd;
    int               rc;
    uint32_t          i;

    if (pkg_path == NULL)
        return IV_EINVAL;
    set_why(why, IV_UPD_OK);
    cfg_resolve(cfg, &c);
    memset(&man, 0, sizeof man);

    /* ---- 1. 验签（只读） ---- */
    self_from_cfg(&c, &self);
    key_from_cfg(&c, pub);
    rc = iv_ota_verify_file(pkg_path, &self, pub, &man, &owhy);
    if (rc != IV_OK) {
        (void)event_append(&c, "reject", "0.0.0", "", iv_ota_reason_name(owhy));
        return fail(why, IV_UPD_ESIG);
    }
    if (fault_hit(&c, "verify"))
        return fail(why, IV_UPD_EFAULT);

    iv_ota_ver_format(man.version, ver, sizeof ver);
    maxb = (c.max_boot != 0u) ? c.max_boot : IV_UPD_MAX_BOOT_DEFAULT;

    if (mkdir_p(c.app_root) != IV_OK || mkdir_p(c.data_root) != IV_OK)
        return fail(why, IV_UPD_EIO);

    /* ---- 2. 收养裸布局（首次；只增不减） ---- */
    rc = do_adopt(&c, why);
    if (rc != IV_OK && rc != IV_ENOENT)
        return rc;

    /* 上一版本名：current 指向的目录名；拿不到或与新版同名则视作"无旧版本"。 */
    prev[0] = '\0';
    if (current_target(&c, prev, sizeof prev) == IV_OK &&
        iv_ota_ver_parse(prev, strlen(prev), &pv) != IV_OK)
        prev[0] = '\0';
    if (strcmp(prev, ver) == 0)
        prev[0] = '\0'; /* 绝不把 current 指到自己，避免"自己覆盖自己" */

    fd = open(pkg_path, O_RDONLY);
    if (fd < 0)
        return fail(why, IV_UPD_EIO);
    rc = pkg_header(fd, &mlen, &plen);
    if (rc != IV_OK) {
        (void)close(fd);
        return fail(why, IV_UPD_ESIG);
    }

    /* ---- 3. 空间校核（两个分区各自都要够） ---- */
    if (fs_free(c.data_root, &freeb) != IV_OK || !iv_upd_space_ok(freeb, plen) ||
        fs_free(c.app_root, &freeb) != IV_OK || !iv_upd_space_ok(freeb, plen)) {
        (void)close(fd);
        return fail(why, IV_UPD_ENOSPC);
    }

    /* ---- 4. 暂存 ---- */
    if (path3(stag, sizeof stag, c.data_root, IV_UPD_OTA_SUBDIR, IV_UPD_STAGING_SUBDIR) != IV_OK ||
        path3(stagv, sizeof stagv, stag, ver, NULL) != IV_OK) {
        (void)close(fd);
        return fail(why, IV_UPD_EIO);
    }
    (void)rm_rf(stagv);
    if (mkdir_p(stagv) != IV_OK) {
        (void)close(fd);
        return fail(why, IV_UPD_EIO);
    }
    if (fault_hit(&c, "stage")) {
        (void)close(fd);
        return fail(why, IV_UPD_EFAULT);
    }

    off = (off_t)IV_OTA_HEADER_LEN + (off_t)mlen;
    for (i = 0u; i < man.n_files; i++) {
        char dst[IV_UPD_PATH_MAX];

        if (path3(dst, sizeof dst, stagv, man.files[i].path, NULL) != IV_OK) {
            (void)close(fd);
            return fail(why, IV_UPD_ESTAGE);
        }
        rc = extract_one(fd, off, man.files[i].size, dst, mode_of_rel(man.files[i].path),
                         man.files[i].sha256);
        if (rc != IV_OK) {
            (void)close(fd);
            return fail(why, IV_UPD_ESTAGE);
        }
        off += (off_t)man.files[i].size;
    }
    (void)close(fd);

    /* ---- 5. 准入检查（架构 ＋ 依赖），只对 `bin/` 下的程序做 ---- */
    missing[0] = '\0';
    for (i = 0u; i < man.n_files; i++) {
        char dst[IV_UPD_PATH_MAX];

        if (!is_prog_rel(man.files[i].path))
            continue;
        if (path3(dst, sizeof dst, stagv, man.files[i].path, NULL) != IV_OK)
            return fail(why, IV_UPD_ESTAGE);
        rc = iv_upd_check_elf(dst, c.lib_paths, missing, sizeof missing, why);
        if (rc != IV_OK) {
            /* check_elf 在每条失败路径上都会写好 reason，这里直接沿用；万一哪条路径
             * 漏了（reason 仍是 OK），按 EIO 处理 —— 绝不能当成成功。 */
            iv_upd_reason_t r = (*why != IV_UPD_OK) ? *why : IV_UPD_EIO;

            (void)event_append(&c, "reject", ver, prev,
                               (missing[0] != '\0') ? missing : "admission");
            return fail(why, r);
        }
    }

    /* ---- 6. 版本目录 ---- */
    if (path3(rels, sizeof rels, c.app_root, IV_UPD_RELEASES_SUBDIR, NULL) != IV_OK ||
        path3(rel, sizeof rel, rels, ver, NULL) != IV_OK)
        return fail(why, IV_UPD_EIO);
    if (mkdir_p(rels) != IV_OK)
        return fail(why, IV_UPD_EIO);

    /* 护栏：`rm_rf(rel)` 之前先确认它既不是 current 也不是 previous 指着的那份。
     * 版本号单调递增时本不会撞上，但"删掉正在跑的那份程序"这一类事故的代价是整机
     * 变砖，用一个显式判据换掉"不可能发生"的推理是划算的。 */
    {
        char lt[IV_UPD_VER_MAX];

        if (link_target(&c, IV_UPD_LINK_CURRENT, lt, sizeof lt) == IV_OK &&
            strcmp(lt, ver) == 0)
            return fail(why, IV_UPD_ESTATE);
        if (link_target(&c, IV_UPD_LINK_PREVIOUS, lt, sizeof lt) == IV_OK &&
            strcmp(lt, ver) == 0)
            return fail(why, IV_UPD_ESTATE);
    }
    (void)rm_rf(rel); /* 上次半途留下的残骸 */
    if (copy_tree(stagv, rel) != IV_OK)
        return fail(why, IV_UPD_EIO);
    sync_dir(rels);
    if (fault_hit(&c, "install"))
        return fail(why, IV_UPD_EFAULT);

    /* ---- 7. 回读：`bin/` 下的程序必须真的可执行 ---- */
    for (i = 0u; i < man.n_files; i++) {
        struct stat st;
        char        dst[IV_UPD_PATH_MAX];

        if (!is_prog_rel(man.files[i].path))
            continue;
        if (path3(dst, sizeof dst, rel, man.files[i].path, NULL) != IV_OK)
            return fail(why, IV_UPD_EIO);
        if (stat(dst, &st) != 0 || (st.st_mode & (mode_t)0111u) == 0u) {
            (void)event_append(&c, "reject", ver, prev, "no-exec-bit");
            return fail(why, IV_UPD_EPERM);
        }
    }

    /* ---- 8. pending 先落盘（见头文件"落盘顺序"） ---- */
    if (pending_write(&c, "pending", ver, prev, rel, 0u, maxb, "") != IV_OK)
        return fail(why, IV_UPD_ESTATE);

    /* ---- 9. previous → 旧版本 ---- */
    if (prev[0] != '\0') {
        if (path3(tgt, sizeof tgt, IV_UPD_RELEASES_SUBDIR, prev, NULL) != IV_OK)
            return fail(why, IV_UPD_EIO);
        if (atomic_symlink(c.app_root, IV_UPD_LINK_PREVIOUS, tgt) != IV_OK)
            return fail(why, IV_UPD_ESWITCH);
    }
    if (fault_hit(&c, "switch"))
        return fail(why, IV_UPD_EFAULT);

    /* ---- 10. current → 新版本（原子） ---- */
    if (path3(tgt, sizeof tgt, IV_UPD_RELEASES_SUBDIR, ver, NULL) != IV_OK)
        return fail(why, IV_UPD_EIO);
    if (atomic_symlink(c.app_root, IV_UPD_LINK_CURRENT, tgt) != IV_OK)
        return fail(why, IV_UPD_ESWITCH);

    (void)event_append(&c, "install", ver, prev, "ok");
    if (fault_hit(&c, "pending"))
        return fail(why, IV_UPD_EFAULT);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 回滚 / 启动包装
 * ------------------------------------------------------------------------- */

static int do_rollback(const iv_upd_cfg_t *c, const char *reason, iv_upd_reason_t *why)
{
    pend_t p;
    char   rels[IV_UPD_PATH_MAX];
    char   relp[IV_UPD_PATH_MAX];
    char   oldver[IV_UPD_VER_MAX];
    char   tgt[IV_UPD_PATH_MAX];
    const char *rs = (reason != NULL) ? reason : "manual";

    pend_read(c, &p);
    if (!p.present || strcmp(p.state, "pending") != 0)
        return fail(why, IV_UPD_ESTATE);
    if (p.prev_s[0] == '\0')
        return fail(why, IV_UPD_ESTATE); /* 旧版本本来就没有 ⇒ 无物可回 */

    if (path3(rels, sizeof rels, c->app_root, IV_UPD_RELEASES_SUBDIR, NULL) != IV_OK ||
        path3(relp, sizeof relp, rels, p.prev_s, NULL) != IV_OK)
        return fail(why, IV_UPD_EIO);
    if (!is_dir(relp))
        return fail(why, IV_UPD_ESTATE); /* 切到一个不存在的目录 = 把设备弄成砖 */

    /* 先让 previous 记住"当前这个坏版本"，再切 current —— 顺序反了会丢掉回滚目标。 */
    if (current_target(c, oldver, sizeof oldver) == IV_OK &&
        path3(tgt, sizeof tgt, IV_UPD_RELEASES_SUBDIR, oldver, NULL) == IV_OK)
        (void)atomic_symlink(c->app_root, IV_UPD_LINK_PREVIOUS, tgt);

    if (path3(tgt, sizeof tgt, IV_UPD_RELEASES_SUBDIR, p.prev_s, NULL) != IV_OK)
        return fail(why, IV_UPD_EIO);
    if (atomic_symlink(c->app_root, IV_UPD_LINK_CURRENT, tgt) != IV_OK)
        return fail(why, IV_UPD_ESWITCH);

    if (pending_write(c, "rolled-back", p.ver_s, p.prev_s, p.app_dir, p.bootcount, p.max_boot,
                      rs) != IV_OK)
        return fail(why, IV_UPD_ESTATE);
    (void)event_append(c, "rollback", p.ver_s, p.prev_s, rs);
    return IV_OK;
}

int iv_upd_rollback(const iv_upd_cfg_t *cfg, const char *reason, iv_upd_reason_t *why)
{
    iv_upd_cfg_t c;

    set_why(why, IV_UPD_OK);
    cfg_resolve(cfg, &c);
    return do_rollback(&c, reason, why);
}

int iv_upd_boot_fail(const iv_upd_cfg_t *cfg, iv_upd_reason_t *why)
{
    iv_upd_cfg_t c;

    set_why(why, IV_UPD_OK);
    cfg_resolve(cfg, &c);
    return do_rollback(&c, "selftest-failed", why);
}

int iv_upd_boot_ok(const iv_upd_cfg_t *cfg, iv_upd_reason_t *why)
{
    iv_upd_cfg_t c;
    pend_t       p;
    uint32_t     maxb;

    set_why(why, IV_UPD_OK);
    cfg_resolve(cfg, &c);
    pend_read(&c, &p);
    if (!p.present || strcmp(p.state, "pending") != 0)
        return IV_OK; /* 幂等：没有待确认的就没什么可确认 */

    maxb = (p.max_boot != 0u) ? p.max_boot
                              : ((c.max_boot != 0u) ? c.max_boot : IV_UPD_MAX_BOOT_DEFAULT);
    if (pending_write(&c, "confirmed", p.ver_s, p.prev_s, p.app_dir, 0u, maxb, "") != IV_OK)
        return fail(why, IV_UPD_ESTATE);
    (void)event_append(&c, "confirm", p.ver_s, p.prev_s, "ok");
    return IV_OK;
}

int iv_upd_boot_begin(const iv_upd_cfg_t *cfg, char *app_dir, size_t cap, int *rolled_back,
                      iv_upd_reason_t *why)
{
    iv_upd_cfg_t c;
    pend_t       p;
    uint32_t     maxb;
    uint32_t     bc;

    if (app_dir == NULL || cap == 0u)
        return IV_EINVAL;
    app_dir[0] = '\0';
    if (rolled_back != NULL)
        *rolled_back = 0;
    set_why(why, IV_UPD_OK);
    cfg_resolve(cfg, &c);

    pend_read(&c, &p);
    if (!p.present || strcmp(p.state, "pending") != 0)
        return current_app_abs(&c, app_dir, cap);

    maxb = (p.max_boot != 0u) ? p.max_boot
                              : ((c.max_boot != 0u) ? c.max_boot : IV_UPD_MAX_BOOT_DEFAULT);

    /* 待确认目录都不在了：没必要浪费一次启动机会，直接回滚。 */
    if (p.app_dir[0] == '\0' || !is_dir(p.app_dir)) {
        (void)do_rollback(&c, "missing-release", why);
        if (rolled_back != NULL)
            *rolled_back = 1;
        return current_app_abs(&c, app_dir, cap);
    }

    bc = p.bootcount + 1u;
    if (bc > maxb) {
        (void)do_rollback(&c, "bootcount-exceeded", why);
        if (rolled_back != NULL)
            *rolled_back = 1;
        return current_app_abs(&c, app_dir, cap);
    }

    (void)pending_write(&c, "pending", p.ver_s, p.prev_s, p.app_dir, bc, maxb, p.reason);
    (void)event_append(&c, "boot", p.ver_s, p.prev_s, "attempt");
    if (strlen(p.app_dir) >= cap)
        return IV_ERANGE;
    strcpy(app_dir, p.app_dir);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 状态与纯函数
 * ------------------------------------------------------------------------- */

void iv_upd_status(const iv_upd_cfg_t *cfg, iv_upd_status_t *out)
{
    iv_upd_cfg_t c;
    pend_t       p;
    char         buf[IV_UPD_PATH_MAX];
    char         ep[IV_UPD_PATH_MAX];

    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    cfg_resolve(cfg, &c);
    out->max_boot = c.max_boot;

    pend_read(&c, &p);
    if (p.present) {
        out->pending = 1;
        out->active = (strcmp(p.state, "pending") == 0) ? 1 : 0;
        (void)ev_copy(out->state, sizeof out->state, p.state, strlen(p.state));
        (void)iv_ota_ver_parse(p.ver_s, strlen(p.ver_s), &out->version);
        if (p.prev_s[0] != '\0')
            (void)iv_ota_ver_parse(p.prev_s, strlen(p.prev_s), &out->previous);
        out->bootcount = p.bootcount;
        if (p.max_boot != 0u)
            out->max_boot = p.max_boot;
    }

    if (current_target(&c, buf, sizeof buf) == IV_OK)
        (void)ev_copy(out->current_dir, sizeof out->current_dir, buf, strlen(buf));

    if (events_path(&c, ep, sizeof ep) == IV_OK)
        events_scan(ep, &out->rollbacks, out->last_event, sizeof out->last_event,
                    out->last_reason, sizeof out->last_reason);
}

const char *iv_upd_reason_name(iv_upd_reason_t r)
{
    switch (r) {
    case IV_UPD_OK:      return "ok";
    case IV_UPD_ESIG:    return "bad-package";
    case IV_UPD_ENOSPC:  return "no-space";
    case IV_UPD_EFAULT:  return "power-loss";
    case IV_UPD_ESTAGE:  return "stage-failed";
    case IV_UPD_EELF:    return "bad-elf";
    case IV_UPD_EDEPS:   return "missing-dep";
    case IV_UPD_EPERM:   return "no-exec-bit";
    case IV_UPD_ESWITCH: return "switch-failed";
    case IV_UPD_ESTATE:  return "bad-state";
    case IV_UPD_EIO:     return "io-error";
    default:             return "unknown";
    }
}

/* ---------------------------------------------------------------------------
 * 安装请求文件（S7.2 装配接线：ivsboxd 落盘，monitor.sh 消费）
 * ---------------------------------------------------------------------------
 * 为什么不让 ivsboxd 自己 fork/exec 安装器：安装要停掉并替换"正在运行的程序本体"，
 * 这段危险动作需要**自己的进程生命周期** —— 不该继承主控的看门狗 fd（子进程若成为
 * 最后一个持有者、退出时会触发复位），也不该在跑 reactor 的线程里阻塞几秒。走一个
 * 请求文件，由 monitor（本来就是唯一拉起方）在轮询里拉起独立进程，是代价最小的接法。
 *
 * 文件格式刻意做成**一行两段**（`<包绝对路径> <restart 0|1>`）：shell 端一句
 * `read -r pkg restart` 就能解析，不需要在 shell 里写 JSON 解析器。
 * 因此**路径里不允许空白字符**（否则 read 会歧义）—— 这条在写入侧就挡掉。 */

static int has_blank(const char *s)
{
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')
            return 1;
    }
    return 0;
}

int iv_upd_request_write(const iv_upd_cfg_t *cfg, const char *pkg_path, int restart,
                         iv_upd_reason_t *why)
{
    iv_upd_cfg_t c;
    char         ota_dir[IV_UPD_PATH_MAX];
    char         path[IV_UPD_PATH_MAX];
    char         line[IV_UPD_PATH_MAX + 4u];
    sb_t         s;
    struct stat  st;
    size_t       n;

    set_why(why, IV_UPD_OK);
    cfg_resolve(cfg, &c);

    if (pkg_path == NULL || pkg_path[0] != '/')
        return IV_EINVAL; /* 只收绝对路径：相对路径没有明确基准 */
    if (has_blank(pkg_path))
        return IV_EINVAL; /* 空白会拆坏 monitor 侧的 `read` 两段解析 */
    n = strlen(pkg_path);
    if (n >= IV_UPD_PATH_MAX)
        return IV_ERANGE;
    if (path3(ota_dir, sizeof ota_dir, c.data_root, IV_UPD_OTA_SUBDIR, NULL) != IV_OK)
        return IV_ERANGE;

    /* 围栏：包必须已经躺在 OTA 目录下（上传/拷贝的落点）。放行任意路径等于把
     * "本地通道能写什么"交给调用方，而这条通路未来会被 Web 复用。 */
    if (strncmp(pkg_path, ota_dir, strlen(ota_dir)) != 0 ||
        pkg_path[strlen(ota_dir)] != '/')
        return IV_EINVAL;
    if (stat(pkg_path, &st) != 0 || !S_ISREG(st.st_mode))
        return IV_ENOENT;
    if (n + 3u > sizeof line)
        return IV_ERANGE;

    if (path3(path, sizeof path, c.data_root, IV_UPD_OTA_SUBDIR, IV_UPD_REQUEST_NAME) != IV_OK)
        return IV_ERANGE;

    sb_init(&s, line, sizeof line);
    sb_str(&s, pkg_path);
    sb_ch(&s, ' ');
    sb_ch(&s, (restart != 0) ? '1' : '0');
    sb_ch(&s, '\n');
    if (s.ovf)
        return IV_ERANGE;
    return atomic_write(path, line, s.n, 0644);
}

int iv_upd_request_read(const iv_upd_cfg_t *cfg, char *pkg_out, size_t cap, int *restart_out)
{
    iv_upd_cfg_t c;
    char         path[IV_UPD_PATH_MAX];
    char         buf[IV_UPD_PATH_MAX + 4u];
    size_t       got = 0u;
    size_t       i;
    size_t       cut;
    int          fd;

    if (pkg_out == NULL || cap == 0u)
        return IV_EINVAL;
    pkg_out[0] = '\0';
    if (restart_out != NULL)
        *restart_out = 0;
    cfg_resolve(cfg, &c);

    if (path3(path, sizeof path, c.data_root, IV_UPD_OTA_SUBDIR, IV_UPD_REQUEST_NAME) != IV_OK)
        return IV_ERANGE;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return IV_ENOENT;
    if (read_at(fd, buf, sizeof buf - 1u, 0, &got) != 0) {
        (void)close(fd);
        return IV_EIO;
    }
    (void)close(fd);
    buf[got] = '\0';

    i = 0u;
    while (i < got && buf[i] != ' ' && buf[i] != '\n')
        i++;
    if (i == 0u || i >= cap)
        return IV_EPROTO;
    cut = i;
    while (i < got && buf[i] == ' ')
        i++;
    if (restart_out != NULL && i < got && buf[i] == '1')
        *restart_out = 1;

    memcpy(pkg_out, buf, cut);
    pkg_out[cut] = '\0';
    return IV_OK;
}

const char *iv_upd_ret_name(int rc)
{
    switch (rc) {
    case IV_OK:       return "OK";
    case IV_EFAIL:    return "EFAIL";
    case IV_EINVAL:   return "EINVAL";
    case IV_EBUSY:    return "EBUSY";
    case IV_EAGAIN:   return "EAGAIN";
    case IV_ETIMEDOUT:return "ETIMEDOUT";
    case IV_ENOMEM:   return "ENOMEM";
    case IV_ESTATE:   return "ESTATE";
    case IV_ENOTSUP:  return "ENOTSUP";
    case IV_EEXIST:   return "EEXIST";
    case IV_ENOENT:   return "ENOENT";
    case IV_ERANGE:   return "ERANGE";
    case IV_ECANCELED:return "ECANCELED";
    case IV_EFULL:    return "EFULL";
    case IV_EPROTO:   return "EPROTO";
    case IV_ECONN:    return "ECONN";
    case IV_EAUTH:    return "EAUTH";
    case IV_ENOSPC:   return "ENOSPC";
    case IV_EIO:      return "EIO";
    case IV_ECORRUPT: return "ECORRUPT";
    default:          return "unknown";
    }
}

int iv_upd_space_ok(uint64_t free_bytes, uint64_t pkg_payload_bytes)
{
    if (free_bytes <= IV_UPD_FREE_MARGIN)
        return 0;
    return (free_bytes - IV_UPD_FREE_MARGIN) > pkg_payload_bytes;
}
