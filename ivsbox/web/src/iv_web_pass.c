/*
 * ivsbox-web 登录口令：哈希、复杂度校验、落盘（M10，2026-10-16）
 *
 * ============================ 为什么单独一个文件 ============================
 * 口令这件事有三份知识：**怎么哈希**（登录比对与落盘用同一个值）、**什么算合格**
 * （服务端唯一权威）、**落在哪、什么权限**。三者分散在 api.c / main.c 里就会各写一套，
 * 迟早漂移（改密接口按 A 规则收、登录按 B 规则比）。故集中在本文件，
 * api.c 只调用、不复制规则。
 *
 * ============================ 内存里放什么 ============================
 * **不放明文**。`iv_web_pass_init()` 把启动期的种子口令（出厂默认 / `--pass` /
 * `IVSBOX_WEB_PASS`）哈希成 `ctx->pass_hash` 后**立即擦除** `ctx->pass`；
 * 登录与改密一律比对哈希。这样"进程内存里有一份明文口令"这件事在启动后就不成立了。
 *
 * ============================ 落盘口径（`<secure>/web.passwd`） ============================
 * 一行：`v1 <账号> <sha256 十六进制 64 位>`；`0600`；`tmp → fsync → rename` 原子替换。
 *   - **只存哈希**，不存明文：该目录在本工程不是真机密存储（F8 密钥分离未实现，
 *     见 `web/src/readme.txt` 的登记），那么至少别把口令原样放进去。
 *   - **摘要输入带域名前缀**：`sha256("ivsbox-web|1|" + 账号 + "|" + 口令)`。
 *     不这么做的话，同一口令在别处（例如 `tunnel.token`）的裸 sha256 可以互相搬运。
 *   - **权限不是 0600 就拒收**（计划 §F8 验证口径"非 0600 被拒"）：读到宽权限文件
 *     说明它可能已被别人看过，此时"照用"等于默认一次未知泄露 ⇒ 宁可退回出厂口径
 *     并要求改密，也不悄悄采信。
 *   - 文件不在 = 正常（出厂机器还没改过密码），不是错误。
 *
 * ============================ 复杂度规则（与参考实现一致） ============================
 * 8~12 个字符，且**同时**含小写字母、大写字母、数字。参考实现（雅下项目
 * `web_server/makefsdata-4g/fs/index.shtml` 第 975 行）用正则
 * `^(?=.*[a-z])(?=.*[A-Z])(?=.*\d)[\s\S]{8,12}$`，本文件用逐字符扫描等价实现
 * —— 正则库（POSIX `regcomp`）在板端 glibc 里可用，但"多链一个正则引擎去判 12 个
 * 字符"不值当，而且扫描版能顺手把**控制字符**拒掉（行式文件里放 '\n' 会把格式写坏）。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_sha256.h"
#include "iv_web.h"

#define PASS_MOD  "web"
/* 摘要域名前缀：把"Web 登录口令"与其它用途的 sha256 隔开（见文件头）。 */
#define PASS_SALT "ivsbox-web|1|"
/* 落盘文件版本标记。将来换哈希算法/加盐方式时靠它区分，不靠"猜"。 */
#define PASS_FILE_VER "v1"

/* 把敏感缓冲擦掉。volatile 让编译器不敢把这次写优化掉（普通 memset 可能被删）。 */
static void wipe(char *p, size_t n)
{
    volatile char *v = (volatile char *)p;

    while (n-- > 0u)
        *v++ = '\0';
}

void iv_web_pass_hash(const char *user, const char *pass, char out[IV_WEB_PASS_HEX_MAX])
{
    iv_sha256_t c;
    uint8_t     d[IV_SHA256_DIGEST_LEN];

    iv_sha256_init(&c);
    iv_sha256_update(&c, PASS_SALT, strlen(PASS_SALT));
    iv_sha256_update(&c, (user != NULL) ? user : "", (user != NULL) ? strlen(user) : 0u);
    iv_sha256_update(&c, "|", 1u);
    iv_sha256_update(&c, (pass != NULL) ? pass : "", (pass != NULL) ? strlen(pass) : 0u);
    iv_sha256_final(&c, d);
    iv_sha256_hex(d, out, IV_WEB_PASS_HEX_MAX);
}

int iv_web_pass_rule(const char *pass)
{
    size_t n;
    size_t i;
    int    lo = 0;
    int    up = 0;
    int    dg = 0;

    if (pass == NULL)
        return IV_EINVAL;
    n = strlen(pass);
    if (n < (size_t)IV_WEB_PASS_LEN_MIN || n > (size_t)IV_WEB_PASS_LEN_MAX)
        return IV_EINVAL;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)pass[i];

        /* 控制字符（含 '\n' 与 DEL）直接不合格：落盘是行式的，'\n' 会把格式写坏。
         * 非 ASCII（≥0x80，例如中文）允许：参考实现的正则 `[\s\S]` 也是"任意字符"，
         * 它们只计长度，不算大小写/数字。 */
        if (ch < 0x20u || ch == 0x7fu)
            return IV_EINVAL;
        if (ch >= 'a' && ch <= 'z')
            lo = 1;
        else if (ch >= 'A' && ch <= 'Z')
            up = 1;
        else if (ch >= '0' && ch <= '9')
            dg = 1;
    }
    return (lo && up && dg) ? IV_OK : IV_EINVAL;
}

/* 64 位十六进制（小写）自检 —— 文件被手改/写坏时不要采信半个值。 */
static int hex64_ok(const char *s)
{
    size_t i;

    if (s == NULL || strlen(s) != IV_SHA256_HEX_LEN)
        return 0;
    for (i = 0; i < IV_SHA256_HEX_LEN; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

int iv_web_pass_load(iv_web_ctx_t *ctx)
{
    struct stat sb;
    char        buf[192];
    char        user[IV_WEB_USER_MAX];
    char        hex[IV_WEB_PASS_HEX_MAX];
    ssize_t     n;
    int         fd;

    if (ctx->pass_file[0] == '\0')
        return IV_ENOTSUP;                     /* 未配置落盘（验证场景） */
    /* O_NOFOLLOW：不跟软链（`secure/` 由 root 预建，软链说明有人动过手脚）。 */
    fd = open(ctx->pass_file, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return (errno == ENOENT) ? IV_ENOENT : IV_EIO;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        (void)close(fd);
        return IV_ECORRUPT;
    }
    if ((sb.st_mode & 0077) != 0) {            /* 计划 §F8：非 0600 拒收 */
        IV_LOG_W(PASS_MOD, "%s mode %04o is too open, refuse to trust it",
                 ctx->pass_file, (unsigned)(sb.st_mode & 07777));
        (void)close(fd);
        return IV_ECORRUPT;
    }
    n = read(fd, buf, sizeof(buf) - 1u);
    (void)close(fd);
    if (n <= 0)
        return IV_EIO;
    buf[n] = '\0';

    user[0] = '\0';
    hex[0]  = '\0';
    if (sscanf(buf, PASS_FILE_VER " %23s %64s", user, hex) != 2)
        return IV_ECORRUPT;
    if (user[0] == '\0' || !hex64_ok(hex))
        return IV_ECORRUPT;

    (void)snprintf(ctx->user, sizeof(ctx->user), "%s", user);
    (void)snprintf(ctx->pass_hash, sizeof(ctx->pass_hash), "%s", hex);
    return IV_OK;
}

int iv_web_pass_save(const iv_web_ctx_t *ctx, const char *user, const char *hex)
{
    char tmp[IV_WEB_PASS_PATH_MAX + 8u];
    char line[IV_WEB_USER_MAX + IV_WEB_PASS_HEX_MAX + 16u];
    size_t len;
    int    fd;
    int    nne;

    if (ctx->pass_file[0] == '\0')
        return IV_ENOTSUP;
    nne = snprintf(tmp, sizeof(tmp), "%s.tmp", ctx->pass_file);
    if (nne < 0 || (size_t)nne >= sizeof(tmp))
        return IV_ERANGE;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return IV_EIO;
    /* 显式 fchmod：O_CREAT 的模式会被 umask 削（这里恰好还是 0600，但别赌现场 umask）。 */
    if (fchmod(fd, 0600) != 0) {
        (void)close(fd);
        (void)unlink(tmp);
        return IV_EIO;
    }
    len = (size_t)snprintf(line, sizeof(line), PASS_FILE_VER " %s %s\n", user, hex);
    if (write(fd, line, len) != (ssize_t)len) {
        (void)close(fd);
        (void)unlink(tmp);
        return IV_EIO;
    }
    (void)fsync(fd);                           /* 尽力；失败只说明这一拍没落盘 */
    (void)close(fd);
    if (rename(tmp, ctx->pass_file) != 0) {
        (void)unlink(tmp);
        return IV_EIO;
    }
    return IV_OK;
}

int iv_web_pass_init(iv_web_ctx_t *ctx)
{
    int is_default_pair;
    int rc;

    is_default_pair = (strcmp(ctx->user, IV_WEB_USER_DEF) == 0) &&
                      (strcmp(ctx->pass, IV_WEB_PASS_DEF) == 0);

    rc = iv_web_pass_load(ctx);
    if (rc == IV_OK) {
        /* 改过密码：以文件为准（即使命令行给了 `--pass`，盘上那份才是事实）。 */
        ctx->must_change = 0;
    } else {
        ctx->must_change = is_default_pair ? 1 : 0;
        iv_web_pass_hash(ctx->user, ctx->pass, ctx->pass_hash);
        if (rc != IV_ENOENT && rc != IV_ENOTSUP)
            IV_LOG_W(PASS_MOD, "password file %s unusable (rc=%d), fall back to defaults",
                     ctx->pass_file, rc);
    }
    wipe(ctx->pass, sizeof(ctx->pass));        /* 明文到此为止（见文件头） */
    return rc;
}
