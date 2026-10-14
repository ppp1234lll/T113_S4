/*
 * iv_upd 单测（libivmodules，功能开发计划 M7-S7.2 ＋ S7.3）
 *
 * ============================ 靶子是什么 ============================
 * 靶子是**盘上的最终状态**，不是"函数返回了 0"：升级这件事的失败方式几乎都是
 * "返回成功但盘上留了个说不清的中间态"。所以每组用例都在问同一个问题 ——
 * **这一次操作之后，current/previous/pending/releases 各是什么？掉电窗口里呢？**
 *
 * ============================ 覆盖 ============================
 *   t01 空间判据（纯函数）：余量、边界、零
 *   t02 默认配置与名字表：cfg_default / reason_name / ret_name
 *   t03 正例全链路：验签 → adopt → 暂存 → 准入 → releases → previous → current → pending
 *        （含权限位 0755/0644、内容逐字节、pending.json 字节形态、事件日志）
 *   t04 bootcount：连跑 3 次 → 第 4 次自动回滚（§16.2 场景 8 前半）
 *   t05 boot-ok 确认：pending → confirmed，bootcount 归零，幂等
 *   t06 boot-fail：自检失败立刻回滚
 *   t07 pending 指向的版本目录不存在 → 直接回滚（missing-release）
 *   t08 掉电注入 5 个阶段：逐个钉住"盘上该停在哪儿"＋"下一次 boot-begin 该起谁"
 *        ＋"重跑安装会怎样"（§16.2 场景 8 的掉电部分）
 *   t09 拒绝路径不留残骸：坏包 / 无公钥 / 版本不递增
 *   t10 ENOSPC：目标分区可用空间报 0 必须拒（用 procfs 钉住该分支）
 *   t11 ELF 准入检查：架构 / 类型 / 截断 / 非 ELF / NEEDED 找到与找不到
 *   t12 安装期的准入拒绝：包内 bin/ 不是 ELF、或缺依赖 → 不留 current/pending
 *   t13 请求文件：往返 ＋ 路径围栏（相对路径 / 含空白 / 越出 OTA 目录 / 不存在）
 *   t14 回滚护栏：无 pending / previous 目录缺失 → ESTATE，current 一动不动
 *
 * ============================ 两点如实说明 ============================
 *   ① 手造 ELF 只支持 **64 位本机**（与 `make test` 的宿主一致）。若在 32 位宿主上
 *      跑，t11/t12 会打印 SKIP 而不是假装通过（SKIP 不等于通过，基线见 readme）。
 *   ② **EPERM（chmod 未生效）与 install 的其它 IO 失败分支没有用例**：它们需要
 *      "只读 / 不支持权限位的文件系统"这种环境，宿主上不可移植地构造。已登记在
 *      tests/unit/readme.txt，不假装覆盖。
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "ivsbox/iv_ota.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_sha256.h"
#include "ivsbox/iv_upd.h"

static int g_fail;
static int g_skip;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void skip(const char *what)
{
    fprintf(stderr, "SKIP: %s\n", what);
    g_skip++;
}

/* ---------------------------------------------------------------------------
 * 测试侧材料与工具
 * ------------------------------------------------------------------------- */

#define PKG_CAP 65536u
#define ELF_CAP 4096u

static uint8_t g_pkg[PKG_CAP];
static uint8_t g_elf[ELF_CAP];
static size_t  g_elf_len;

static char g_root[128];
static char g_app[192];
static char g_data[192];
static char g_lib[192];

static uint8_t PUB[IV_OTA_PUBKEY_LEN];
static const uint8_t SEED[IV_OTA_SEED_LEN] = {
    0x5a, 0x5a, 0x01, 0x77, 0x33, 0x21, 0x9c, 0x4e, 0x8b, 0xd0, 0x62, 0x1f, 0x05, 0xa7, 0xc3, 0x38,
    0xe9, 0x14, 0x70, 0xbb, 0x2d, 0x46, 0x88, 0x13, 0xfe, 0x6a, 0x02, 0x95, 0x4c, 0xb7, 0x30, 0xd1
};

static const char *const MODEL = "IVSBOX-T113";

typedef struct {
    const char    *path;
    const uint8_t *data;
    size_t         len;
} tf_t;

/* ---- 路径 / 文件小工具（测试自己用，不走被测代码） ---- */

/* a ＋ b 拼进 dst（截断到 cap-1，永远 NUL 终止）。
 * **先拼进局部缓冲再一次性拷出**：本文件多处写成 `cat(p, n, p, x)`（原地追加），
 * 直接 memcpy 会踩到 ASan 的 memcpy-param-overlap。 */
static void cat(char *dst, size_t cap, const char *a, const char *b)
{
    char   tmp[512];
    size_t n = 0u;

    if (cap == 0u)
        return;
    for (; *a != '\0' && n + 1u < sizeof tmp; a++)
        tmp[n++] = *a;
    for (; *b != '\0' && n + 1u < sizeof tmp; b++)
        tmp[n++] = *b;
    if (n >= cap)
        n = cap - 1u;
    memcpy(dst, tmp, n);
    dst[n] = '\0';
}

static int exists(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0;
}

static int is_dir(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static unsigned mode_of(const char *p)
{
    struct stat st;

    if (stat(p, &st) != 0)
        return 0u;
    return (unsigned)(st.st_mode & (mode_t)07777);
}

static int rm_rf(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR           *d = opendir(path);
        struct dirent *e;

        if (d == NULL)
            return -1;
        while ((e = readdir(d)) != NULL) {
            char child[512];

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            cat(child, sizeof child, path, "/");
            cat(child, sizeof child, child, e->d_name);
            (void)rm_rf(child);
        }
        (void)closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

static int mkp(const char *path, unsigned mode)
{
    char   tmp[512];
    size_t n = strlen(path);
    size_t i;

    if (n == 0u || n >= sizeof tmp)
        return -1;
    memcpy(tmp, path, n + 1u);
    for (i = 1u; tmp[i] != '\0'; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, (mode_t)mode) != 0 && !is_dir(tmp))
            return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, (mode_t)mode) != 0 && !is_dir(tmp))
        return -1;
    return 0;
}

static int wr_file(const char *path, const void *data, size_t len, unsigned mode)
{
    char par[512];
    int  fd;
    char *slash;

    memcpy(par, path, strlen(path) + 1u);
    slash = strrchr(par, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (mkp(par, 0755) != 0)
            return -1;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
    if (fd < 0)
        return -1;
    if (len > 0u && write(fd, data, len) != (ssize_t)len) {
        (void)close(fd);
        return -1;
    }
    (void)close(fd);
    return 0;
}

static int wtxt(const char *path, const char *txt)
{
    return wr_file(path, txt, strlen(txt), 0644);
}

static int rd_file(const char *path, char *buf, size_t cap)
{
    int    fd;
    ssize_t n;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, cap - 1u);
    (void)close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

/* `<dir>/<name>` 的链接目标；不存在或不是链接返回 NULL。 */
static const char *link_of(const char *dir, const char *name, char *out, size_t cap)
{
    char    p[512];
    ssize_t n;

    cat(p, sizeof p, dir, "/");
    cat(p, sizeof p, p, name);
    n = readlink(p, out, cap - 1u);
    if (n < 0)
        return NULL;
    out[n] = '\0';
    return out;
}

/* ---- ELF 手造（64 位本机） ---- */

static void p16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void p32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void p64(uint8_t *p, uint64_t v)
{
    unsigned i;

    for (i = 0u; i < 8u; i++)
        p[i] = (uint8_t)((v >> (8u * i)) & 0xffu);
}

/* 造一个 ELF64：ET_EXEC ＋ 1 个 PT_LOAD（覆盖整文件），needed 非空时再加
 * PT_DYNAMIC ＋ DT_NEEDED/DT_STRTAB/DT_STRSZ。返回总长（0 = 造不出来）。 */
static size_t mk_elf(uint8_t *b, size_t cap, uint16_t machine, uint16_t etype,
                     const char *needed)
{
    size_t phnum = (needed != NULL) ? 2u : 1u;
    size_t dyn_off = 64u + 56u * phnum;
    size_t dyn_sz = (needed != NULL) ? 4u * 16u : 0u;
    size_t str_off = dyn_off + dyn_sz;
    size_t str_sz = (needed != NULL) ? strlen(needed) + 1u : 0u;
    size_t total = str_off + str_sz;
    uint8_t *ph;
    size_t   i;

    if (total > cap)
        return 0u;
    memset(b, 0, total);

    b[0] = 0x7fu;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = IV_UPD_ELF_CLASS;
    b[5] = IV_UPD_ELF_DATA;
    b[6] = 1u;
    p16(b + 16, etype);
    p16(b + 18, machine);
    p32(b + 20, 1u);
    p64(b + 24, 0x400000u);
    p64(b + 32, 64u);
    p16(b + 52, 64u);
    p16(b + 54, 56u);
    p16(b + 56, (uint16_t)phnum);
    p16(b + 58, 64u);

    ph = b + 64u;
    p32(ph + 0, 1u);
    p32(ph + 4, 5u);
    p64(ph + 8, 0u);
    p64(ph + 16, 0x400000u);
    p64(ph + 24, 0x400000u);
    p64(ph + 32, (uint64_t)total);
    p64(ph + 40, (uint64_t)total);
    p64(ph + 48, 0x1000u);

    if (needed == NULL)
        return total;

    ph = b + 64u + 56u;
    p32(ph + 0, 2u);
    p32(ph + 4, 6u);
    p64(ph + 8, (uint64_t)dyn_off);
    p64(ph + 16, 0x400000u + (uint64_t)dyn_off);
    p64(ph + 24, 0x400000u + (uint64_t)dyn_off);
    p64(ph + 32, (uint64_t)dyn_sz);
    p64(ph + 40, (uint64_t)dyn_sz);
    p64(ph + 48, 8u);

    p64(b + dyn_off + 0u, 1u);  /* DT_NEEDED */
    p64(b + dyn_off + 8u, 0u);  /*   -> 名字在 strtab 偏移 0 */
    p64(b + dyn_off + 16u, 5u); /* DT_STRTAB */
    p64(b + dyn_off + 24u, 0x400000u + (uint64_t)str_off);
    p64(b + dyn_off + 32u, 10u); /* DT_STRSZ */
    p64(b + dyn_off + 40u, (uint64_t)str_sz);
    p64(b + dyn_off + 48u, 0u);  /* DT_NULL */

    for (i = 0u; i < str_sz; i++)
        b[str_off + i] = (uint8_t)needed[i];
    return total;
}

/* ---- 造升级包（只用 OpenSSL 签名，不碰被测代码） ---- */

static void wr_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static int pubkey_from_seed(uint8_t out[IV_OTA_PUBKEY_LEN])
{
    EVP_PKEY *k;
    size_t    n  = IV_OTA_PUBKEY_LEN;
    int       ok = 0;

    k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, SEED, IV_OTA_SEED_LEN);
    if (k != NULL) {
        ok = EVP_PKEY_get_raw_public_key(k, out, &n) == 1 && n == IV_OTA_PUBKEY_LEN;
        EVP_PKEY_free(k);
    }
    return ok;
}

static int sign_msg(const uint8_t *msg, size_t mlen, uint8_t sig[IV_OTA_SIG_LEN])
{
    EVP_PKEY   *k;
    EVP_MD_CTX *ctx;
    size_t      sl = IV_OTA_SIG_LEN;
    int         ok = 0;

    k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, SEED, IV_OTA_SEED_LEN);
    if (k == NULL)
        return 0;
    ctx = EVP_MD_CTX_new();
    if (ctx != NULL) {
        if (EVP_DigestSignInit(ctx, NULL, NULL, NULL, k) == 1 &&
            EVP_DigestSign(ctx, sig, &sl, msg, mlen) == 1 && sl == IV_OTA_SIG_LEN)
            ok = 1;
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(k);
    return ok;
}

/* 装配包到 g_pkg；返回总长。返回 0 表示装不下。 */
static size_t mk_pkg(const char *model, unsigned hwmin, unsigned hwmax, const char *ver,
                     const char *minrb, const tf_t *f, size_t nf)
{
    char    man[4096];
    size_t  used = 0u;
    size_t  off;
    size_t  i;

    used += (size_t)snprintf(man + used, sizeof man - used,
                             "pkgfmt=1\nmodel=%s\nhwmin=%u\nhwmax=%u\nversion=%s\n"
                             "minrollback=%s\nfiles=%u\n",
                             model, hwmin, hwmax, ver, minrb, (unsigned)nf);
    for (i = 0u; i < nf; i++) {
        uint8_t     d[IV_OTA_SHA256_LEN];
        iv_sha256_t c;
        char        hex[IV_OTA_SHA256_HEX_LEN + 1u];

        iv_sha256_init(&c);
        iv_sha256_update(&c, f[i].data, f[i].len);
        iv_sha256_final(&c, d);
        iv_sha256_hex(d, hex, sizeof(hex));
        used += (size_t)snprintf(man + used, sizeof man - used, "file=%s:%u:%s\n", f[i].path,
                                 (unsigned)f[i].len, hex);
    }

    off = IV_OTA_HEADER_LEN + used;
    {
        size_t plen = 0u;

        for (i = 0u; i < nf; i++)
            plen += f[i].len;
        if (off + plen + IV_OTA_SIG_LEN > sizeof g_pkg)
            return 0u;
        memcpy(g_pkg, IV_OTA_MAGIC, IV_OTA_MAGIC_LEN);
        wr_u32_le(g_pkg + 8, (uint32_t)used);
        wr_u32_le(g_pkg + 12, (uint32_t)plen);
        memcpy(g_pkg + IV_OTA_HEADER_LEN, man, used);
        for (i = 0u; i < nf; i++) {
            memcpy(g_pkg + off, f[i].data, f[i].len);
            off += f[i].len;
        }
    }
    if (!sign_msg(g_pkg, off, g_pkg + off))
        return 0u;
    return off + IV_OTA_SIG_LEN;
}

/* 默认两个文件：bin/ivsboxd = 本机架构的真 ELF；etc/ivsbox.json = 文本。 */
/* 每次都**重造** g_elf：t11/t12 会往这块共享缓冲里写别的变体（错架构、带 NEEDED…），
 * 不重造的话后面用默认文件集的用例会拿到"长度记的是旧值、内容是新的"的半截 ELF。 */
static void files_default(tf_t f[2])
{
    static const uint8_t CFG[] = "{\"sys\":{\"name\":\"ivupd-test\"}}";

    g_elf_len = mk_elf(g_elf, sizeof g_elf, (uint16_t)IV_UPD_ELF_MACHINE, 2u, NULL);
    f[0].path = "bin/ivsboxd";
    f[0].data = g_elf;
    f[0].len = g_elf_len;
    f[1].path = "etc/ivsbox.json";
    f[1].data = CFG;
    f[1].len = sizeof(CFG) - 1u;
}

/* 造一个"正常升级包"并落盘到 <root>/pkg.ivota，返回其路径。 */
static const char *write_good_pkg(const char *ver, char *path_out, size_t cap)
{
    tf_t   f[2];
    size_t n;

    files_default(f);
    n = mk_pkg(MODEL, 1u, 2u, ver, "1.0.0", f, 2u);
    chk(n > 0u, "build package");
    cat(path_out, cap, g_root, "/pkg.ivota");
    chk(wr_file(path_out, g_pkg, n, 0644) == 0, "write package file");
    return path_out;
}

/* ---- 临时根 ---- */

static void root_new(void)
{
    char  tmpl[] = "/tmp/ivupdXXXXXX";
    char *d = mkdtemp(tmpl);
    char  pkg[256];

    if (d == NULL) {
        fprintf(stderr, "FATAL: mkdtemp failed\n");
        exit(1);
    }
    cat(g_root, sizeof g_root, d, "");
    cat(g_app, sizeof g_app, g_root, "/app");
    cat(g_data, sizeof g_data, g_root, "/data");
    cat(g_lib, sizeof g_lib, g_root, "/lib");
    (void)mkp(g_app, 0755);
    (void)mkp(g_data, 0755);
    (void)mkp(g_lib, 0755);
    (void)pkg;
}

static void root_drop(void)
{
    (void)rm_rf(g_root);
}

/* 裸布局：<app>/bin/ivsboxd（真 ELF）＋ <app>/scripts/monitor.sh（文本）。 */
static void seed_bare(void)
{
    char p[512];

    cat(p, sizeof p, g_app, "/bin/ivsboxd");
    chk(wr_file(p, g_elf, g_elf_len, 0755) == 0, "seed bare bin/ivsboxd");
    cat(p, sizeof p, g_app, "/scripts/monitor.sh");
    chk(wtxt(p, "#!/bin/sh\n") == 0, "seed bare scripts/monitor.sh");
}

static void cfg_make(iv_upd_cfg_t *c)
{
    iv_upd_cfg_default(c);
    c->app_root = g_app;
    c->data_root = g_data;
    c->pubkey = PUB;
}

static void data_path(char *out, size_t cap, const char *leaf)
{
    cat(out, cap, g_data, "/ota/");
    cat(out, cap, out, leaf);
}

static void app_path(char *out, size_t cap, const char *leaf)
{
    cat(out, cap, g_app, "/");
    cat(out, cap, out, leaf);
}

/* ---------------------------------------------------------------------------
 * t01 空间判据
 * ------------------------------------------------------------------------- */

static void t01_space(void)
{
    chk(iv_upd_space_ok(0u, 0u) == 0, "t01: zero free is not ok");
    chk(iv_upd_space_ok(IV_UPD_FREE_MARGIN, 0u) == 0, "t01: free == margin is not ok");
    chk(iv_upd_space_ok(IV_UPD_FREE_MARGIN + 1u, 0u) == 1, "t01: margin+1 with empty pkg ok");
    chk(iv_upd_space_ok(IV_UPD_FREE_MARGIN + 100u, 100u) == 0,
        "t01: margin+100 for a 100-byte pkg is not ok (margin is exclusive)");
    chk(iv_upd_space_ok(IV_UPD_FREE_MARGIN + 101u, 100u) == 1, "t01: strict boundary ok");
    chk(iv_upd_space_ok(8u * 1024u * 1024u, 64u * 1024u * 1024u) == 0,
        "t01: 8MiB free for 64MiB payload is not ok");
}

/* ---------------------------------------------------------------------------
 * t02 默认配置与名字表
 * ------------------------------------------------------------------------- */

static void t02_defaults(void)
{
    iv_upd_cfg_t c;
    int          i;

    iv_upd_cfg_default(&c);
    chk(strcmp(c.app_root, IV_UPD_APP_ROOT_DEFAULT) == 0, "t02: app_root default");
    chk(strcmp(c.data_root, IV_UPD_DATA_ROOT_DEFAULT) == 0, "t02: data_root default");
    chk(strcmp(c.dev_model, IV_UPD_DEV_MODEL_DEFAULT) == 0, "t02: model default");
    chk(c.dev_hw == IV_UPD_DEV_HW_DEFAULT, "t02: hw default");
    chk(c.max_boot == IV_UPD_MAX_BOOT_DEFAULT, "t02: max_boot default");
    chk(c.pubkey == NULL && c.fault_at == NULL, "t02: pubkey/fault_at default NULL");
    iv_upd_cfg_default(NULL); /* 必须容错 */

    chk(strcmp(iv_upd_reason_name(IV_UPD_OK), "ok") == 0, "t02: reason ok");
    chk(strcmp(iv_upd_reason_name(IV_UPD_EFAULT), "power-loss") == 0, "t02: reason power-loss");
    chk(iv_upd_reason_name((iv_upd_reason_t)999) != NULL, "t02: reason unknown non-NULL");
    chk(strcmp(iv_upd_ret_name(IV_OK), "OK") == 0, "t02: ret OK");
    chk(strcmp(iv_upd_ret_name(IV_ENOSPC), "ENOSPC") == 0, "t02: ret ENOSPC");
    chk(strcmp(iv_upd_ret_name(IV_EAUTH), "EAUTH") == 0, "t02: ret EAUTH");
    chk(strcmp(iv_upd_ret_name(-12345), "unknown") == 0, "t02: ret unknown");

    /* 枚举值稳定性：这些数值会进日志与上报，不能随手改 */
    chk(IV_UPD_OK == 0 && IV_UPD_ESIG == 1 && IV_UPD_EIO == 10, "t02: reason values stable");
    for (i = 0; i < 4; i++)
        chk(iv_upd_ret_name(IV_EFAIL - i) != NULL, "t02: ret names walkable");
}

/* ---------------------------------------------------------------------------
 * t03 正例全链路
 * ------------------------------------------------------------------------- */

static void t03_happy_path(void)
{
    iv_upd_cfg_t    c;
    iv_upd_status_t st;
    iv_upd_reason_t why = IV_UPD_OK;
    char            pkg[256];
    char            p[512];
    char            lnk[512];
    char            buf[1024];
    int             rc;

    root_new();
    seed_bare();
    cfg_make(&c);
    (void)write_good_pkg("1.0.1", pkg, sizeof pkg);

    rc = iv_upd_install(pkg, &c, &why);
    chk(rc == IV_OK, "t03: install ok");
    if (rc != IV_OK)
        fprintf(stderr, "      why=%s\n", iv_upd_reason_name(why));

    /* 版本目录内容与权限 */
    app_path(p, sizeof p, "releases/1.0.1/bin/ivsboxd");
    chk(exists(p), "t03: releases/1.0.1/bin/ivsboxd exists");
    chk(mode_of(p) == 0755u, "t03: bin file mode is 0755");
    chk(rd_file(p, buf, sizeof buf) == (int)g_elf_len, "t03: installed elf byte length");
    app_path(p, sizeof p, "releases/1.0.1/etc/ivsbox.json");
    chk(exists(p), "t03: releases/1.0.1/etc/ivsbox.json exists");
    chk(mode_of(p) == 0644u, "t03: etc file mode is 0644");
    chk(rd_file(p, buf, sizeof buf) > 0 && strcmp(buf, "{\"sys\":{\"name\":\"ivupd-test\"}}") == 0,
        "t03: etc file content byte-equal");

    /* adopt 出来的"上一版本"必须真实存在，否则回滚无处可去 */
    app_path(p, sizeof p, "releases/1.0.0/bin/ivsboxd");
    chk(exists(p), "t03: adopted releases/1.0.0/bin/ivsboxd exists");

    /* 链接切换 */
    chk(link_of(g_app, "current", lnk, sizeof lnk) != NULL &&
            strcmp(lnk, "releases/1.0.1") == 0,
        "t03: current -> releases/1.0.1");
    chk(link_of(g_app, "previous", lnk, sizeof lnk) != NULL &&
            strcmp(lnk, "releases/1.0.0") == 0,
        "t03: previous -> releases/1.0.0");

    /* pending.json 的字节形态（读取端 iv_upd 与它是一份契约） */
    data_path(p, sizeof p, "pending.json");
    chk(rd_file(p, buf, sizeof buf) > 0, "t03: pending.json readable");
    chk(strstr(buf, "\"state\":\"pending\"") != NULL, "t03: pending state byte form");
    chk(strstr(buf, "\"version\":\"1.0.1\"") != NULL, "t03: pending version byte form");
    chk(strstr(buf, "\"previous\":\"1.0.0\"") != NULL, "t03: pending previous byte form");
    chk(strstr(buf, "\"bootcount\":0") != NULL, "t03: pending bootcount starts at 0");

    /* status */
    iv_upd_status(&c, &st);
    chk(st.pending == 1 && st.active == 1, "t03: status pending+active");
    chk(strcmp(st.state, "pending") == 0, "t03: status state");
    chk(st.version.major == 1u && st.version.minor == 0u && st.version.patch == 1u,
        "t03: status version parsed");
    chk(st.previous.patch == 0u, "t03: status previous parsed");
    chk(st.bootcount == 0u && st.max_boot == 3u, "t03: status boot counts");
    chk(st.rollbacks == 0u, "t03: status rollbacks zero");
    chk(strcmp(st.last_event, "install") == 0, "t03: last event is install");
    chk(strcmp(st.current_dir, "1.0.1") == 0, "t03: current_dir");

    data_path(p, sizeof p, "events.log");
    chk(rd_file(p, buf, sizeof buf) > 0 && strstr(buf, " install ") != NULL,
        "t03: events.log has install line");

    /* 暂存目录**有意保留**（失败现场要留得住），如实断言一下，免得将来被"顺手清理" */
    data_path(p, sizeof p, "staging/1.0.1/bin/ivsboxd");
    chk(exists(p), "t03: staging dir intentionally kept");

    root_drop();
}

/* ---------------------------------------------------------------------------
 * t04 bootcount 超限 → 自动回滚
 * ------------------------------------------------------------------------- */

static void t04_bootcount_rollback(void)
{
    iv_upd_cfg_t    c;
    iv_upd_status_t st;
    iv_upd_reason_t why = IV_UPD_OK;
    char            pkg[256];
    char            dir[IV_UPD_PATH_MAX];
    char            exp[IV_UPD_PATH_MAX];
    char            lnk[512];
    int             rolled = 0;
    int             i;

    root_new();
    seed_bare();
    cfg_make(&c);
    (void)write_good_pkg("1.0.1", pkg, sizeof pkg);
    chk(iv_upd_install(pkg, &c, &why) == IV_OK, "t04: install ok");

    cat(exp, sizeof exp, g_app, "/releases/1.0.1");
    for (i = 1; i <= 3; i++) {
        rolled = -1;
        chk(iv_upd_boot_begin(&c, dir, sizeof dir, &rolled, &why) == IV_OK,
            "t04: boot_begin ok");
        chk(rolled == 0, "t04: not rolled back yet");
        chk(strcmp(dir, exp) == 0, "t04: boots the new release");
        iv_upd_status(&c, &st);
        chk(st.bootcount == (uint32_t)i, "t04: bootcount increments");
    }

    /* 第 4 次超限 ⇒ 回滚 */
    rolled = 0;
    chk(iv_upd_boot_begin(&c, dir, sizeof dir, &rolled, &why) == IV_OK, "t04: 4th boot_begin ok");
    chk(rolled == 1, "t04: 4th boot_begin rolled back");
    cat(exp, sizeof exp, g_app, "/releases/1.0.0");
    chk(strcmp(dir, exp) == 0, "t04: boots the previous release");
    chk(link_of(g_app, "current", lnk, sizeof lnk) != NULL &&
            strcmp(lnk, "releases/1.0.0") == 0,
        "t04: current switched back");

    iv_upd_status(&c, &st);
    chk(st.active == 0, "t04: no longer active");
    chk(strcmp(st.state, "rolled-back") == 0, "t04: state rolled-back");
    chk(st.rollbacks == 1u, "t04: rollback counted");
    chk(strcmp(st.last_event, "rollback") == 0, "t04: last event rollback");
    chk(strcmp(st.last_reason, "bootcount-exceeded") == 0, "t04: reason recorded");

    /* 已是终态：再回滚必须拒（不许乱切） */
    chk(iv_upd_rollback(&c, NULL, &why) == IV_ESTATE, "t04: rollback again is ESTATE");
    chk(why == IV_UPD_ESTATE, "t04: reason ESTATE");

    /* 回滚后再 boot_begin：无 pending，走 current */
    chk(iv_upd_boot_begin(&c, dir, sizeof dir, NULL, &why) == IV_OK, "t04: boot after rollback");
    chk(strcmp(dir, exp) == 0, "t04: boot after rollback uses current");

    root_drop();
}

/* ---------------------------------------------------------------------------
 * t05 boot-ok 确认
 * ------------------------------------------------------------------------- */

static void t05_confirm(void)
{
    iv_upd_cfg_t    c;
    iv_upd_status_t st;
    iv_upd_reason_t why = IV_UPD_OK;
    char            pkg[256];
    char            dir[IV_UPD_PATH_MAX];
    char            exp[IV_UPD_PATH_MAX];

    root_new();
    seed_bare();
    cfg_make(&c);
    (void)write_good_pkg("1.0.1", pkg, sizeof pkg);
    chk(iv_upd_install(pkg, &c, &why) == IV_OK, "t05: install ok");
    chk(iv_upd_boot_begin(&c, dir, sizeof dir, NULL, &why) == IV_OK, "t05: boot_begin ok");

    chk(iv_upd_boot_ok(&c, &why) == IV_OK, "t05: boot_ok ok");
    iv_upd_status(&c, &st);
    chk(st.pending == 1 && st.active == 0, "t05: pending file kept but not active");
    chk(strcmp(st.state, "confirmed") == 0, "t05: state confirmed");
    chk(st.bootcount == 0u, "t05: bootcount cleared");
    chk(strcmp(st.last_event, "confirm") == 0, "t05: last event confirm");

    /* 幂等 */
    chk(iv_upd_boot_ok(&c, &why) == IV_OK, "t05: boot_ok idempotent");
    iv_upd_status(&c, &st);
    chk(strcmp(st.state, "confirmed") == 0, "t05: still confirmed");

    /* 确认之后 boot_begin 走 current，且不再消耗 bootcount */
    cat(exp, sizeof exp, g_app, "/releases/1.0.1");
    chk(iv_upd_boot_begin(&c, dir, sizeof dir, NULL, &why) == IV_OK, "t05: boot after confirm");
    chk(strcmp(dir, exp) == 0, "t05: boots current release");
    iv_upd_status(&c, &st);
    chk(st.bootcount == 0u, "t05: bootcount stays 0 after confirm");

    /* 确认之后不允许再回滚（终态） */
    chk(iv_upd_rollback(&c, "late", &why) == IV_ESTATE, "t05: rollback after confirm is ESTATE");

    root_drop();
}

/* ---------------------------------------------------------------------------
 * t06 boot-fail 立刻回滚
 * ------------------------------------------------------------------------- */

static void t06_selftest_fail(void)
{
    iv_upd_cfg_t    c;
    iv_upd_status_t st;
    iv_upd_reason_t why = IV_UPD_OK;
    char            pkg[256];
    char            lnk[512];

    root_new();
    seed_bare();
    cfg_make(&c);
    (void)write_good_pkg("1.0.1", pkg, sizeof pkg);
    chk(iv_upd_install(pkg, &c, &why) == IV_OK, "t06: install ok");
    chk(iv_upd_boot_fail(&c, &why) == IV_OK, "t06: boot_fail ok");

    chk(link_of(g_app, "current", lnk, sizeof lnk) != NULL &&
            strcmp(lnk, "releases/1.0.0") == 0,
        "t06: current rolled back");
    iv_upd_status(&c, &st);
    chk(strcmp(st.state, "rolled-back") == 0, "t06: state rolled-back");
    chk(strcmp(st.last_reason, "selftest-failed") == 0, "t06: reason selftest-failed");
    chk(st.rollbacks == 1u, "t06: rollback counted");

    /* 无 pending 时 boot_fail 必须拒（不许把"没在升级"当成"升级失败"） */
    iv_upd_cfg_t c2;

    cfg_make(&c2);
    c2.data_root = g_root; /* 换个没有 pending.json 的根 */
    chk(iv_upd_boot_fail(&c2, &why) == IV_ESTATE, "t06: boot_fail without pending is ESTATE");

    root_drop();
}

/* ---------------------------------------------------------------------------
 * t07 pending 指向的版本目录不存在 → 直接回滚
 * ------------------------------------------------------------------------- */

static void t07_missing_release(void)
{
    iv_upd_cfg_t    c;
    iv_upd_reason_t why = IV_UPD_OK;
    char            pkg[256];
    char            dir[IV_UPD_PATH_MAX];
    char            exp[IV_UPD_PATH_MAX];
    char            p[512];
    int             rolled = 0;

    root_new();
    seed_bare();
    cfg_make(&c);
    (void)write_good_pkg("1.0.1", pkg, sizeof pkg);
    chk(iv_upd_install(pkg, &c, &why) == IV_OK, "t07: install ok");

    /* 模拟"新版目录被清掉 / 从没落全"：pending 指向的 app_dir 消失 */
    app_path(p, sizeof p, "releases/1.0.1");
    chk(rm_rf(p) == 0, "t07: remove new release dir");

    chk(iv_upd_boot_begin(&c, dir, sizeof dir, &rolled, &why) == IV_OK, "t07: boot_begin ok");
    chk(rolled == 1, "t07: rolled back");
    cat(exp, sizeof exp, g_app, "/releases/1.0.0");
    chk(strcmp(dir, exp) == 0, "t07: boots previous");
    chk(why == IV_UPD_OK, "t07: rollback itself succeeded");

    root_drop();
}

/* ---------------------------------------------------------------------------
 * t08 掉电注入五个阶段
 * ------------------------------------------------------------------------- */

static void t08_power_loss(void)
{
    static const char *const phases[5] = { "verify", "stage", "install", "switch", "pending" };
    int i;

    for (i = 0; i < 5; i++) {
        iv_upd_cfg_t    c;
        iv_upd_reason_t why = IV_UPD_OK;
        char            pkg[256];
        char            p[512];
        char            buf[1024];
        char            dir[IV_UPD_PATH_MAX];
        const char     *ph = phases[i];
        int             rc;

        root_new();
        seed_bare();
        cfg_make(&c);
        c.fault_at = ph;
        (void)write_good_pkg("1.0.1", pkg, sizeof pkg);

        rc = iv_upd_install(pkg, &c, &why);
        chk(rc == IV_EFAIL && why == IV_UPD_EFAULT, ph);
        if (rc != IV_EFAIL)
            fprintf(stderr, "      phase=%s rc=%d why=%s\n", ph, rc, iv_upd_reason_name(why));

        /* 每个阶段"盘上该停在哪儿"逐个钉住 */
        app_path(p, sizeof p, "releases/1.0.1");
        if (strcmp(ph, "verify") == 0 || strcmp(ph, "stage") == 0)
            chk(!exists(p), "t08: verify/stage must not create the release dir");

        /* adopt 在 verify 之后才开始（verify 阶段必须一个字节都没写） */
        app_path(p, sizeof p, "releases/1.0.0");
        if (strcmp(ph, "verify") == 0)
            chk(!exists(p), "t08: verify phase must not adopt");
        else
            chk(exists(p), "t08: adopt ran for this phase");

        data_path(p, sizeof p, "staging/1.0.1");
        if (strcmp(ph, "verify") == 0)
            chk(!exists(p), "t08: verify must not create staging");
        if (strcmp(ph, "stage") == 0)
            chk(is_dir(p), "t08: stage phase leaves a (partial) staging dir");

        data_path(p, sizeof p, "pending.json");
        if (strcmp(ph, "verify") == 0 || strcmp(ph, "stage") == 0 || strcmp(ph, "install") == 0)
            chk(!exists(p), "t08: pending must not exist before the pending phase");
        else
            chk(exists(p), "t08: pending exists from switch phase on");

        if (strcmp(ph, "pending") == 0) {
            chk(rd_file(p, buf, sizeof buf) > 0 && strstr(buf, "\"state\":\"pending\"") != NULL,
                "t08: pending phase leaves state=pending");
        }

        /* 关键判据：下一次启动该起谁 */
        chk(iv_upd_boot_begin(&c, dir, sizeof dir, NULL, &why) == IV_OK, "t08: boot_begin ok");
        if (strcmp(ph, "switch") == 0 || strcmp(ph, "pending") == 0) {
            char exp[IV_UPD_PATH_MAX];

            cat(exp, sizeof exp, g_app, "/releases/1.0.1");
            chk(strcmp(dir, exp) == 0,
                "t08: switch/pending phase must boot the NEW release (its dir is complete)");
        } else if (strcmp(ph, "verify") == 0) {
            chk(strcmp(dir, g_app) == 0, "t08: verify phase has no current -> bare layout root");
        } else {
            char exp[IV_UPD_PATH_MAX];

            /* stage / install：current 仍指在 adopt 出来的 1.0.0 上 */
            cat(exp, sizeof exp, g_app, "/releases/1.0.0");
            chk(strcmp(dir, exp) == 0, "t08: stage/install phase keeps booting the old release");
        }

        /* 重跑安装：只有 pending 阶段（current 已切到 1.0.1）应当因"版本不递增"被拒 */
        {
            iv_upd_cfg_t    c2;
            iv_upd_reason_t w2 = IV_UPD_OK;
            int             rc2;

            cfg_make(&c2);
            rc2 = iv_upd_install(pkg, &c2, &w2);
            if (strcmp(ph, "pending") == 0) {
                chk(rc2 == IV_EAUTH, "t08: retry after pending phase is rejected (not newer)");
                chk(w2 == IV_UPD_ESIG, "t08: reject reason is bad-package");
            } else {
                chk(rc2 == IV_OK, "t08: retry install succeeds");
            }
        }

        root_drop();
    }
}

/* ---------------------------------------------------------------------------
 * t09 拒绝路径不留残骸
 * ------------------------------------------------------------------------- */

static void t09_reject_no_residue(void)
{
    iv_upd_cfg_t    c;
    iv_upd_reason_t why = IV_UPD_OK;
    tf_t            f[2];
    char            pkg[256];
    char            p[512];
    uint8_t         zero[IV_OTA_PUBKEY_LEN];
    size_t          n;
    int             rc;

    /* 型号不匹配 */
    root_new();
    seed_bare();
    cfg_make(&c);
    files_default(f);
    n = mk_pkg("OTHER-BOARD", 1u, 2u, "1.0.1", "1.0.0", f, 2u);
    cat(pkg, sizeof pkg, g_root, "/bad.ivota");
    (void)wr_file(pkg, g_pkg, n, 0644);
    rc = iv_upd_install(pkg, &c, &why);
    chk(rc != IV_OK && (why == IV_UPD_ESIG || why == IV_UPD_ESTAGE),
        "t09: wrong model rejected");
    app_path(p, sizeof p, "releases");
    chk(!exists(p), "t09: no releases dir created on reject");
    data_path(p, sizeof p, "pending.json");
    chk(!exists(p), "t09: no pending on reject");
    root_drop();

    /* 没有公钥（全零）⇒ fail-closed */
    root_new();
    seed_bare();
    cfg_make(&c);
    memset(zero, 0, sizeof zero);
    c.pubkey = zero;
    (void)write_good_pkg("1.0.1", pkg, sizeof pkg);
    rc = iv_upd_install(pkg, &c, &why);
    chk(rc != IV_OK, "t09: zero pubkey rejected (fail-closed)");
    chk(why == IV_UPD_ESIG, "t09: reason bad-package");
    app_path(p, sizeof p, "releases/1.0.1");
    chk(!exists(p), "t09: no release dir on key reject");
    root_drop();
}

/* ---------------------------------------------------------------------------
 * t10 ENOSPC：目标分区可用空间报 0 必须拒
 * ------------------------------------------------------------------------- */

static void t10_nospace(void)
{
    iv_upd_cfg_t    c;
    iv_upd_reason_t why = IV_UPD_OK;
    char            pkg[256];
    char            p[512];
    struct statvfs  vfs;
    int             rc;

    /* 用 procfs 当 data_root：它恒报可用空间 0，正好钉住"报 0 就必须拒"这条分支。
     * 不这么做就只能靠"磁盘永远不会满"来免测 —— 而那恰恰是最不该免的判据。 */
    if (statvfs("/proc/self", &vfs) != 0 ||
        ((uint64_t)vfs.f_bavail != 0u && (uint64_t)vfs.f_blocks != 0u)) {
        skip("t10: /proc/self does not report a zero-space filesystem on this host");
        return;
    }

    root_new();
    seed_bare();
    cfg_make(&c);
    c.data_root = "/proc/self";
    (void)write_good_pkg("1.0.1", pkg, sizeof pkg);

    rc = iv_upd_install(pkg, &c, &why);
    chk(rc == IV_ENOSPC && why == IV_UPD_ENOSPC, "t10: install reports ENOSPC");
    app_path(p, sizeof p, "releases/1.0.1");
    chk(!exists(p), "t10: nothing installed when out of space");
    root_drop();
}

/* ---------------------------------------------------------------------------
 * t11 ELF 准入检查
 * ------------------------------------------------------------------------- */

static void t11_mk_elf_dir(void)
{
    char p[512];

    cat(p, sizeof p, g_lib, "/libivupd-ok.so.9");
    (void)wtxt(p, "not-a-real-lib\n");
}

static void t11_check_elf(void)
{
    iv_upd_reason_t why = IV_UPD_OK;
    char            p[512];
    char            missing[128];
    size_t          n;
    int             rc;

    if (IV_UPD_ELF_CLASS != 2u) {
        skip("t11: hand-built ELF is 64-bit only");
        return;
    }

    root_new();
    t11_mk_elf_dir();

    /* 正常：无 PT_DYNAMIC ⇒ 没有 NEEDED 可查，直接放行 */
    cat(p, sizeof p, g_root, "/ok.elf");
    n = mk_elf(g_elf, sizeof g_elf, (uint16_t)IV_UPD_ELF_MACHINE, 2u, NULL);
    chk(n > 0u, "t11: build plain elf");
    (void)wr_file(p, g_elf, n, 0755);
    chk(iv_upd_check_elf(p, g_lib, missing, sizeof missing, &why) == IV_OK,
        "t11: plain elf passes");

    /* NEEDED 找得到 */
    cat(p, sizeof p, g_root, "/dep-ok.elf");
    n = mk_elf(g_elf, sizeof g_elf, (uint16_t)IV_UPD_ELF_MACHINE, 2u, "libivupd-ok.so.9");
    chk(n > 0u, "t11: build elf with needed");
    (void)wr_file(p, g_elf, n, 0755);
    chk(iv_upd_check_elf(p, g_lib, missing, sizeof missing, &why) == IV_OK,
        "t11: needed lib found");

    /* NEEDED 找不到 ⇒ EDEPS ＋ 缺失库名 */
    missing[0] = '\0';
    rc = iv_upd_check_elf(p, g_root, missing, sizeof missing, &why);
    chk(rc == IV_ENOENT && why == IV_UPD_EDEPS, "t11: missing dep -> EDEPS");
    chk(strcmp(missing, "libivupd-ok.so.9") == 0, "t11: missing name reported");

    /* 架构不符 */
    cat(p, sizeof p, g_root, "/badmach.elf");
    n = mk_elf(g_elf, sizeof g_elf, (uint16_t)(IV_UPD_ELF_MACHINE + 1000u), 2u, NULL);
    chk(n > 0u, "t11: build wrong-machine elf");
    (void)wr_file(p, g_elf, n, 0755);
    chk(iv_upd_check_elf(p, g_lib, missing, sizeof missing, &why) == IV_EPROTO &&
            why == IV_UPD_EELF,
        "t11: wrong machine -> EELF");

    /* 类型不对（ET_REL：这是 .o，不是能跑的程序） */
    cat(p, sizeof p, g_root, "/rel.elf");
    n = mk_elf(g_elf, sizeof g_elf, (uint16_t)IV_UPD_ELF_MACHINE, 1u, NULL);
    chk(n > 0u, "t11: build rel elf");
    (void)wr_file(p, g_elf, n, 0755);
    chk(iv_upd_check_elf(p, g_lib, missing, sizeof missing, &why) == IV_EPROTO,
        "t11: ET_REL -> EELF");

    /* 截断 */
    cat(p, sizeof p, g_root, "/trunc.elf");
    (void)wr_file(p, g_elf, 32u, 0755);
    chk(iv_upd_check_elf(p, g_lib, missing, sizeof missing, &why) == IV_EPROTO,
        "t11: truncated -> EELF");

    /* 根本不是 ELF */
    cat(p, sizeof p, g_root, "/text.elf");
    (void)wtxt(p, "#!/bin/sh\necho hi\n");
    chk(iv_upd_check_elf(p, g_lib, missing, sizeof missing, &why) == IV_EPROTO,
        "t11: shell script -> EELF");

    /* 不存在的文件 / NULL 路径 */
    cat(p, sizeof p, g_root, "/nonexistent.elf");
    chk(iv_upd_check_elf(p, g_lib, missing, sizeof missing, &why) == IV_EIO,
        "t11: missing file -> EIO");
    chk(iv_upd_check_elf(NULL, g_lib, missing, sizeof missing, &why) == IV_EINVAL,
        "t11: NULL path -> EINVAL");

    root_drop();
}

/* ---------------------------------------------------------------------------
 * t12 安装期的准入拒绝
 * ------------------------------------------------------------------------- */

static void t12_admission_reject(void)
{
    iv_upd_cfg_t    c;
    iv_upd_reason_t why = IV_UPD_OK;
    tf_t            f[2];
    static const uint8_t TXT[] = "#!/bin/sh\necho not-an-elf\n";
    char            pkg[256];
    char            p[512];
    size_t          n;
    int             rc;

    if (IV_UPD_ELF_CLASS != 2u) {
        skip("t12: hand-built ELF is 64-bit only");
        return;
    }

    /* 包里的 bin/ivsboxd 不是 ELF ⇒ 不许装 */
    root_new();
    seed_bare();
    cfg_make(&c);
    f[0].path = "bin/ivsboxd";
    f[0].data = TXT;
    f[0].len = sizeof(TXT) - 1u;
    f[1].path = "etc/ivsbox.json";
    f[1].data = TXT;
    f[1].len = sizeof(TXT) - 1u;
    n = mk_pkg(MODEL, 1u, 2u, "1.0.1", "1.0.0", f, 2u);
    cat(pkg, sizeof pkg, g_root, "/notelf.ivota");
    (void)wr_file(pkg, g_pkg, n, 0644);
    rc = iv_upd_install(pkg, &c, &why);
    chk(rc == IV_EPROTO && why == IV_UPD_EELF, "t12: non-elf program rejected");
    app_path(p, sizeof p, "releases/1.0.1");
    chk(!exists(p), "t12: no release dir for non-elf");
    data_path(p, sizeof p, "pending.json");
    chk(!exists(p), "t12: no pending for non-elf");
    root_drop();

    /* 缺依赖 ⇒ 不许装（用 lib_paths 指到一个没有那个库的目录） */
    root_new();
    seed_bare();
    cfg_make(&c);
    c.lib_paths = g_root; /* 空目录，什么都没有 */
    f[0].path = "bin/ivsboxd";
    f[0].len = mk_elf(g_elf, sizeof g_elf, (uint16_t)IV_UPD_ELF_MACHINE, 2u,
                      "libivupd-nosuch.so.9");
    chk(f[0].len > 0u, "t12: build elf with unsatisfiable dep");
    f[0].data = g_elf;
    f[1].path = "etc/ivsbox.json";
    f[1].data = TXT;
    f[1].len = sizeof(TXT) - 1u;
    n = mk_pkg(MODEL, 1u, 2u, "1.0.1", "1.0.0", f, 2u);
    cat(pkg, sizeof pkg, g_root, "/dep.ivota");
    (void)wr_file(pkg, g_pkg, n, 0644);
    rc = iv_upd_install(pkg, &c, &why);
    chk(rc == IV_ENOENT && why == IV_UPD_EDEPS, "t12: unsatisfiable dep rejected");
    app_path(p, sizeof p, "releases/1.0.1");
    chk(!exists(p), "t12: no release dir for missing dep");
    root_drop();
}

/* ---------------------------------------------------------------------------
 * t13 请求文件
 * ------------------------------------------------------------------------- */

static void t13_request(void)
{
    iv_upd_cfg_t c;
    char         p[512];
    char         got[512];
    char         buf[512];
    int          restart = -1;

    root_new();
    cfg_make(&c);
    (void)mkp(g_data, 0755);

    /* 没有请求文件 */
    chk(iv_upd_request_read(&c, got, sizeof got, &restart) == IV_ENOENT,
        "t13: read without file is ENOENT");

    /* 合法包（必须落在 <data>/ota/ 下） */
    cat(p, sizeof p, g_data, "/ota/upload.ivota");
    chk(wr_file(p, "pkg", 3u, 0644) == 0, "t13: write fake package");
    chk(iv_upd_request_write(&c, p, 1, NULL) == IV_OK, "t13: request write ok");

    data_path(p, sizeof p, "request");
    chk(rd_file(p, buf, sizeof buf) > 0, "t13: request file readable");
    {
        char exp[512];

        cat(exp, sizeof exp, g_data, "/ota/upload.ivota 1\n");
        chk(strcmp(buf, exp) == 0, "t13: request byte form is '<path> <restart>\\n'");
    }
    restart = -1;
    chk(iv_upd_request_read(&c, got, sizeof got, &restart) == IV_OK, "t13: request read ok");
    {
        char exp[512];

        cat(exp, sizeof exp, g_data, "/ota/upload.ivota");
        chk(strcmp(got, exp) == 0, "t13: round-trip path");
    }
    chk(restart == 1, "t13: round-trip restart flag");

    /* 围栏 */
    chk(iv_upd_request_write(&c, "relative/pkg.ivota", 0, NULL) == IV_EINVAL,
        "t13: relative path rejected");
    cat(p, sizeof p, g_data, "/ota/bad name.ivota");
    chk(wr_file(p, "x", 1u, 0644) == 0, "t13: write spaced-name file");
    chk(iv_upd_request_write(&c, p, 0, NULL) == IV_EINVAL, "t13: whitespace path rejected");
    cat(p, sizeof p, g_root, "/outside.ivota");
    chk(wr_file(p, "x", 1u, 0644) == 0, "t13: write outside file");
    chk(iv_upd_request_write(&c, p, 0, NULL) == IV_EINVAL, "t13: outside ota dir rejected");
    cat(p, sizeof p, g_data, "/ota/ghost.ivota");
    chk(iv_upd_request_write(&c, p, 0, NULL) == IV_ENOENT, "t13: nonexistent pkg rejected");
    cat(p, sizeof p, g_data, "/ota/sub");
    chk(mkp(p, 0755) == 0, "t13: make subdir under ota");
    chk(iv_upd_request_write(&c, p, 0, NULL) == IV_ENOENT, "t13: directory is not a package");

    root_drop();
}

/* ---------------------------------------------------------------------------
 * t14 回滚护栏
 * ------------------------------------------------------------------------- */

static void t14_rollback_guards(void)
{
    iv_upd_cfg_t    c;
    iv_upd_reason_t why = IV_UPD_OK;
    char            pkg[256];
    char            p[512];
    char            lnk[512];
    char            before[512];

    /* 无 pending ⇒ 拒 */
    root_new();
    seed_bare();
    cfg_make(&c);
    chk(iv_upd_rollback(&c, NULL, &why) == IV_ESTATE, "t14: rollback without pending is ESTATE");
    root_drop();

    /* previous 目录被删 ⇒ 拒，且 current 一动不动 */
    root_new();
    seed_bare();
    cfg_make(&c);
    (void)write_good_pkg("1.0.1", pkg, sizeof pkg);
    chk(iv_upd_install(pkg, &c, &why) == IV_OK, "t14: install ok");
    app_path(p, sizeof p, "releases/1.0.0");
    chk(rm_rf(p) == 0, "t14: remove previous release");

    chk(link_of(g_app, "current", before, sizeof before) != NULL, "t14: read current before");
    chk(iv_upd_rollback(&c, NULL, &why) == IV_ESTATE, "t14: rollback with missing previous");
    chk(link_of(g_app, "current", lnk, sizeof lnk) != NULL && strcmp(lnk, before) == 0,
        "t14: current untouched after refused rollback");
    root_drop();

    /* 全局默认根上的只读查询：不该崩、不该写盘 */
    {
        iv_upd_cfg_t    d;
        iv_upd_status_t st;

        iv_upd_cfg_default(&d);
        d.app_root = g_app; /* 此时这个目录刚被删掉 */
        iv_upd_status(&d, &st);
        chk(st.pending == 0 && st.state[0] == '\0', "t14: status on empty roots is all-default");
        chk(st.current_dir[0] == '\0', "t14: current_dir empty when no link");
    }
    iv_upd_status(NULL, NULL); /* NULL 容错 */
}

/* ---------------------------------------------------------------------------
 * t15 复装"回滚目标"那一版必须被拒
 * ---------------------------------------------------------------------------
 * install 在 copy 之前会 `rm_rf(rel)` 清掉上次的残骸，护栏是"rel 不许是 current 或
 * previous 指着的版本"。版本号单调递增时本不会撞上 —— 但**回滚会把 current 退回旧版本**，
 * 于是"比 current 新、却正好等于 previous"的版本号重新变成可安装。少了护栏，安装会先
 * 删掉**回滚目标那一份**；此后任何一步失败，"回滚"就再也无处可去（现场表现为设备变砖）。
 * 判据钉两件事：调用被拒，且回滚目标一个字节没动。
 */
static void t15_guard_rollback_target(void)
{
    iv_upd_cfg_t    c;
    iv_upd_reason_t why = IV_UPD_OK;
    char            pkg[256];
    char            lnk[512];
    char            p[512];

    root_new();
    seed_bare();
    cfg_make(&c);
    (void)write_good_pkg("1.0.1", pkg, sizeof pkg);
    chk(iv_upd_install(pkg, &c, &why) == IV_OK, "t15: install 1.0.1 ok");
    chk(iv_upd_boot_fail(&c, &why) == IV_OK, "t15: rollback ok");

    /* 回滚之后：current -> 1.0.0，previous -> 1.0.1（正好落在"可装却等于 previous"那一格） */
    chk(link_of(g_app, "current", lnk, sizeof lnk) != NULL &&
            strcmp(lnk, "releases/1.0.0") == 0,
        "t15: current is back at 1.0.0");
    chk(link_of(g_app, "previous", lnk, sizeof lnk) != NULL &&
            strcmp(lnk, "releases/1.0.1") == 0,
        "t15: previous remembers the rolled-back version");

    /* 同一个包再装：1.0.1 > current(1.0.0) ⇒ 过得去"版本递增"，只能靠护栏拦下 */
    why = IV_UPD_OK;
    chk(iv_upd_install(pkg, &c, &why) == IV_ESTATE,
        "t15: reinstall of the rollback target is refused");
    chk(why == IV_UPD_ESTATE, "t15: reason is ESTATE");

    /* 护栏的实义：回滚目标那一份必须还在 */
    app_path(p, sizeof p, "releases/1.0.1/bin/ivsboxd");
    chk(exists(p), "t15: rollback target dir survived");
    chk(link_of(g_app, "previous", lnk, sizeof lnk) != NULL &&
            strcmp(lnk, "releases/1.0.1") == 0,
        "t15: previous link untouched");

    root_drop();
}

/* ---------------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------------- */

int main(void)
{
    /* 让 NEEDED 查找只认显式给的 lib_paths：否则宿主的 LD_LIBRARY_PATH 会
     * 让"缺依赖"的用例随环境漂移。 */
    (void)unsetenv("LD_LIBRARY_PATH");

    if (!pubkey_from_seed(PUB)) {
        fprintf(stderr, "FATAL: cannot derive test pubkey\n");
        return 1;
    }
    g_elf_len = mk_elf(g_elf, sizeof g_elf, (uint16_t)IV_UPD_ELF_MACHINE, 2u, NULL);
    if (g_elf_len == 0u) {
        fprintf(stderr, "FATAL: cannot build test elf\n");
        return 1;
    }

    t01_space();
    t02_defaults();
    t03_happy_path();
    t04_bootcount_rollback();
    t05_confirm();
    t06_selftest_fail();
    t07_missing_release();
    t08_power_loss();
    t09_reject_no_residue();
    t10_nospace();
    t11_check_elf();
    t12_admission_reject();
    t13_request();
    t14_rollback_guards();
    t15_guard_rollback_target();

    if (g_fail != 0) {
        fprintf(stderr, "test_upd: %d FAIL, %d SKIP\n", g_fail, g_skip);
        return 1;
    }
    printf("test_upd: all passed (%d skip)\n", g_skip);
    return 0;
}
