/*
 * iv_ota —— 升级包解析与验签（libivmodules，功能开发计划 M7-S7.1）
 *
 * ============================ 干了什么事 ============================
 * 实现 `include/ivsbox/iv_ota.h` 定义的全部接口：
 *   1. 版本号 `M.m.p` 的解析 / 比较 / 渲染（纯计算）；
 *   2. 清单文本的行式解析（纯计算，**不改动输入缓冲**，因为文件版是 mmap 只读区）；
 *   3. `iv_ota_verify_mem()` 的九步验签流程（结构 → 清单 → 负载长度 → 公钥 →
 *      签名 → 型号 → 硬件版本 → 软件版本 → 逐文件 SHA-256）；
 *   4. `iv_ota_verify_file()`：`open` + `fstat` + `mmap(MAP_PRIVATE)`，**不落盘、不写文件**；
 *   5. `iv_ota_builtin_pubkey()`：量产公钥占位（当前返回 IV_ENOTSUP）。
 *
 * 数字签名走 OpenSSL EVP（Ed25519）；升级包校验值走本工程自写的 iv_sha256（core 层）。
 * 两者分工与"为什么不自己写椭圆曲线"的理由写在头文件里（注释）。
 *
 * ============================ 解决什么问题 ============================
 * §16.2 场景 9 要求"未签名或型号不匹配的升级包必须被拒绝"，而在此之前工程里
 * **没有任何包格式、也没有任何验签代码**（`src/modules/ota/` 只有一个 readme 占位）：
 * 升级链路连"什么算一个合法包"都没定义。本文件把格式与拒绝条件钉死，并让每条
 * 拒绝理由都能被单测单独触发（见 tests/unit/test_ota.c 的反向证伪）。
 *
 * ============================ 影响范围 ============================
 *   - 新增依赖：`-lcrypto`（两端 `mk/toolchain-*.mk` 的 LDLIBS）。板端 rootfs 的
 *     libcrypto.so.1.1 属**待现场核对**项（板子当前不在线），已在修改记录登记；
 *   - 落在 libivmodules：`ivsboxd` / `ivsbox-updater` / 单测都会链到它，
 *     **本步不改任何既有调用路径**（没有装配层接线，属 S7.2 的流程）；
 *   - 不在本文件内做日志与上报：验签器只管判定，日志/上报由调用方按 reason 决定。
 *
 * ============================ 关键实现口径 ============================
 *   - **零 malloc**：清单结构由调用方持有；文件版靠 mmap，不整包进堆；
 *   - **拒绝要精确**：任何"看不懂"的输入一律进 EBADFMT，不做宽容解析
 *     （清单里出现未知键、重复单值键、无 `\n` 结尾的末行、大写 hex 全部判非法）；
 *   - **验签前不采信清单内容**：语义判断全部排在签名校验之后（见头文件"验签顺序"）；
 *   - 验签失败后调 `ERR_clear_error()`：OpenSSL 的错误队列是线程局部的，
 *     留着会污染同线程后续调用方看到的错误原因。
 */
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>

#include "ivsbox/iv_ota.h"
#include "ivsbox/iv_sha256.h"

/* 清单单行上限（`file=` + 95 路径 + 10 尺寸 + 64 hex + 分隔符 ≈ 175，取 256）。 */
#define OTA_LINE_MAX 256u

/* 包总长上限（应用级 A/B 的二进制量级约 10 MB；512 MB 是明显的畸形容器）。 */
#define OTA_PKG_MAX ((off_t)512 * 1024 * 1024)

/* ---------------------------------------------------------------------------
 * 内部小工具
 * ------------------------------------------------------------------------- */

static int fail(iv_ota_reason_t *why, iv_ota_reason_t r, int rc)
{
    if (why != NULL)
        *why = r;
    return rc;
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* 十进制无符号：至少 1 位、无前导 '+'/空白、<= max。成功返回 1 并写 *out。 */
static int parse_u32(const char *s, size_t n, uint32_t max, uint32_t *out)
{
    uint64_t v = 0;
    size_t   i;

    if (n == 0u)
        return 0;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10u + (uint64_t)(s[i] - '0');
        if (v > (uint64_t)max)
            return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* 恰好 64 个小写 hex（大写不接受：清单只允许一种规范写法）。 */
static int parse_hex32(const char *s, size_t n, uint8_t out[IV_OTA_SHA256_LEN])
{
    size_t i;

    if (n != IV_OTA_SHA256_HEX_LEN)
        return 0;
    for (i = 0; i < n; i++) {
        char    c = s[i];
        uint8_t nib;

        if (c >= '0' && c <= '9')
            nib = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            nib = (uint8_t)(c - 'a' + 10);
        else
            return 0;
        if ((i & 1u) == 0u)
            out[i >> 1] = (uint8_t)(nib << 4);
        else
            out[i >> 1] = (uint8_t)(out[i >> 1] | nib);
    }
    return 1;
}

/* 路径围栏：包内相对路径，逐分量检查。
 * 拒：空路径、超长、绝对路径、空分量（前导/尾随/连续 '/'）、`.` 分量、`..` 分量、
 *     `\` 与 `:`（分隔符歧义）、控制字符。
 * 放：`ivsbox.json` 这类含点的正常文件名（点不是问题，"整段就是 .." 才是）。 */
static int path_ok(const char *p, size_t n)
{
    size_t seg = 0u;
    size_t i;

    if (n == 0u || n >= IV_OTA_PATH_MAX)
        return 0;
    for (i = 0; i <= n; i++) {
        if (i == n || p[i] == '/') {
            size_t seglen = i - seg;

            if (seglen == 0u)
                return 0;
            if (seglen == 1u && p[seg] == '.')
                return 0;
            if (seglen == 2u && p[seg] == '.' && p[seg + 1u] == '.')
                return 0;
            seg = i + 1u;
        } else {
            unsigned char c = (unsigned char)p[i];

            if (c < 0x20u || c == 0x7Fu || c == '\\' || c == ':')
                return 0;
        }
    }
    return 1;
}

/* 型号串：非空、<= 上限、字符限 [A-Za-z0-9._-]（不含空格与分隔符）。 */
static int model_ok(const char *s, size_t n)
{
    size_t i;

    if (n == 0u || n >= IV_OTA_MODEL_MAX)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * 版本号
 * ------------------------------------------------------------------------- */

int iv_ota_ver_parse(const char *s, size_t len, iv_ota_ver_t *out)
{
    iv_ota_ver_t v;
    const char  *p = s;
    size_t       n = len;
    size_t       seg;
    uint32_t     tmp;

    if (s == NULL || out == NULL || len == 0u)
        return IV_EINVAL;
    for (seg = 0u; seg < 3u; seg++) {
        size_t dots = 0u;

        while (dots < n && p[dots] != '.')
            dots++;
        if (!parse_u32(p, dots, 65535u, &tmp))
            return IV_EINVAL;
        if (seg == 0u)
            v.major = (uint16_t)tmp;
        else if (seg == 1u)
            v.minor = (uint16_t)tmp;
        else
            v.patch = (uint16_t)tmp;
        if (dots == n) {
            /* 第三段正好用完输入才算合法；"1.2" 这种缺段形式在这里被拒 */
            if (seg != 2u)
                return IV_EINVAL;
            *out = v;
            return IV_OK;
        }
        p += dots + 1u;
        n -= dots + 1u;
    }
    /* 三段之后还有内容（多余的点或字符） */
    return IV_EINVAL;
}

int iv_ota_ver_cmp(iv_ota_ver_t a, iv_ota_ver_t b)
{
    if (a.major != b.major)
        return (a.major < b.major) ? -1 : 1;
    if (a.minor != b.minor)
        return (a.minor < b.minor) ? -1 : 1;
    if (a.patch != b.patch)
        return (a.patch < b.patch) ? -1 : 1;
    return 0;
}

void iv_ota_ver_format(iv_ota_ver_t v, char *out, size_t cap)
{
    uint16_t parts[3];
    int      i;
    size_t   used = 0u;

    if (out == NULL || cap == 0u)
        return;
    out[0]   = '\0';
    parts[0] = v.major;
    parts[1] = v.minor;
    parts[2] = v.patch;
    for (i = 0; i < 3; i++) {
        /* 有界手工拼装：不用 snprintf（目标带已知边界时 GCC 会按最坏情况断 -Werror）。 */
        char     num[8];
        size_t   nl = 0u;
        uint16_t x  = parts[i];

        do {
            num[nl++] = (char)('0' + (x % 10u));
            x         = (uint16_t)(x / 10u);
        } while (x != 0u && nl < sizeof(num));
        while (nl > 0u) {
            /* 装不下就停：**必须走 done 补终止符**，"提前 return" 会把上一轮的残留
             * 字节留在缓冲区里（头文件承诺"始终 NUL 终止"，不能靠运气）。 */
            if (used + 1u >= cap)
                goto done;
            out[used++] = num[--nl];
        }
        if (i < 2) {
            if (used + 1u >= cap)
                goto done;
            out[used++] = '.';
        }
    }
done:
    out[used] = '\0';
}

/* ---------------------------------------------------------------------------
 * 清单解析
 * ------------------------------------------------------------------------- */

/* 在 [s, s+n) 里找一个字符，返回到该字符的距离；没有则返回 n。 */
static size_t span_to(const char *s, size_t n, char c)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (s[i] == c)
            return i;
    }
    return n;
}

int iv_ota_manifest_parse(const char *buf, size_t len, iv_ota_manifest_t *out,
                          iv_ota_reason_t *why)
{
    enum {
        S_PKGFMT = 1u << 0,
        S_MODEL  = 1u << 1,
        S_HWMIN  = 1u << 2,
        S_HWMAX  = 1u << 3,
        S_VER    = 1u << 4,
        S_MINRB  = 1u << 5,
        S_FILES  = 1u << 6
    };
    const unsigned need_all = (unsigned)(S_PKGFMT | S_MODEL | S_HWMIN | S_HWMAX | S_VER |
                                         S_MINRB | S_FILES);
    unsigned seen_single = 0u; /* 已见的单值键位图（重复即非法） */
    uint32_t seen_files  = 0u; /* 已解析的 file= 行数 */
    size_t   off         = 0u;

    if (buf == NULL || out == NULL || len == 0u || len > IV_OTA_MANIFEST_MAX)
        return IV_EINVAL;
    if (why != NULL)
        *why = IV_OTA_OK;
    memset(out, 0, sizeof(*out));

    /* 末行必须带 '\n'：否则"没换行的半截文件"会被当合法清单接受。 */
    if (buf[len - 1u] != '\n')
        return fail(why, IV_OTA_EBADFMT, IV_EPROTO);

    while (off < len) {
        char   line[OTA_LINE_MAX];
        size_t ll = span_to(buf + off, len - off, '\n');
        size_t eq;
        size_t klen;
        size_t vlen;

        if (ll == 0u || ll >= OTA_LINE_MAX)
            return fail(why, IV_OTA_EBADFMT, IV_EPROTO); /* 空行或超长行 */
        memcpy(line, buf + off, ll);
        line[ll] = '\0';
        off += ll + 1u;

        eq = span_to(line, ll, '=');
        if (eq == 0u || eq == ll)
            return fail(why, IV_OTA_EBADFMT, IV_EPROTO); /* 无 '='、或空键/空值 */
        klen = eq;
        vlen = ll - eq - 1u;

        if (klen == 6u && memcmp(line, "pkgfmt", 6u) == 0) {
            uint32_t v;

            if ((seen_single & S_PKGFMT) != 0u ||
                !parse_u32(line + eq + 1u, vlen, 0xFFFFu, &v) || v != 1u)
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            out->pkgfmt = v;
            seen_single |= S_PKGFMT;
        } else if (klen == 5u && memcmp(line, "model", 5u) == 0) {
            if ((seen_single & S_MODEL) != 0u || !model_ok(line + eq + 1u, vlen))
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            memcpy(out->model, line + eq + 1u, vlen);
            out->model[vlen] = '\0';
            seen_single |= S_MODEL;
        } else if (klen == 5u && memcmp(line, "hwmin", 5u) == 0) {
            if ((seen_single & S_HWMIN) != 0u ||
                !parse_u32(line + eq + 1u, vlen, 0xFFFFFFFFu, &out->hw_min))
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            seen_single |= S_HWMIN;
        } else if (klen == 5u && memcmp(line, "hwmax", 5u) == 0) {
            if ((seen_single & S_HWMAX) != 0u ||
                !parse_u32(line + eq + 1u, vlen, 0xFFFFFFFFu, &out->hw_max))
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            seen_single |= S_HWMAX;
        } else if (klen == 7u && memcmp(line, "version", 7u) == 0) {
            if ((seen_single & S_VER) != 0u ||
                iv_ota_ver_parse(line + eq + 1u, vlen, &out->version) != IV_OK)
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            seen_single |= S_VER;
        } else if (klen == 11u && memcmp(line, "minrollback", 11u) == 0) {
            if ((seen_single & S_MINRB) != 0u ||
                iv_ota_ver_parse(line + eq + 1u, vlen, &out->min_rollback) != IV_OK)
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            seen_single |= S_MINRB;
        } else if (klen == 5u && memcmp(line, "files", 5u) == 0) {
            if ((seen_single & S_FILES) != 0u ||
                !parse_u32(line + eq + 1u, vlen, IV_OTA_FILES_MAX, &out->n_files) ||
                out->n_files == 0u)
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            seen_single |= S_FILES;
        } else if (klen == 4u && memcmp(line, "file", 4u) == 0) {
            iv_ota_file_t *f;
            const char    *v  = line + eq + 1u;
            size_t         c1 = span_to(v, vlen, ':');
            size_t         c2;
            size_t         plen;

            if (seen_files >= IV_OTA_FILES_MAX || c1 >= vlen)
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            c2 = c1 + 1u + span_to(v + c1 + 1u, vlen - c1 - 1u, ':');
            if (c2 >= vlen)
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            plen = c1;
            if (!path_ok(v, plen))
                return fail(why, IV_OTA_EPATH, IV_EPROTO);

            f = &out->files[seen_files];
            memcpy(f->path, v, plen);
            f->path[plen] = '\0';
            if (!parse_u32(v + c1 + 1u, c2 - c1 - 1u, 0xFFFFFFFFu, &f->size))
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            if (!parse_hex32(v + c2 + 1u, vlen - c2 - 1u, f->sha256))
                return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
            seen_files++;
        } else {
            /* 未知键：包是我们自己的格式，容忍未知键只会让两端对"包内容"理解不一致 */
            return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
        }
    }

    if ((seen_single & need_all) != need_all)
        return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
    if (seen_files != out->n_files)
        return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
    if (out->hw_min > out->hw_max)
        return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * Ed25519 验签（OpenSSL EVP；密钥与上下文都在函数内建与释放）
 * ------------------------------------------------------------------------- */

static int ed25519_verify(const uint8_t *msg, size_t mlen, const uint8_t *sig,
                          const uint8_t *pub)
{
    EVP_PKEY   *key;
    EVP_MD_CTX *ctx;
    int         ok = 0;

    key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, IV_OTA_PUBKEY_LEN);
    if (key == NULL)
        return 0;
    ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        EVP_PKEY_free(key);
        return 0;
    }
    if (EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1 &&
        EVP_DigestVerify(ctx, sig, IV_OTA_SIG_LEN, msg, mlen) == 1)
        ok = 1;
    else
        ERR_clear_error(); /* 清掉本线程错误队列，别留给后续调用方 */
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

static int pubkey_usable(const uint8_t *pub)
{
    size_t  i;
    uint8_t acc = 0u;

    if (pub == NULL)
        return 0;
    for (i = 0; i < IV_OTA_PUBKEY_LEN; i++)
        acc |= pub[i];
    return acc != 0u; /* 全零 = 未配置 */
}

/* ---------------------------------------------------------------------------
 * 验签主流程
 * ------------------------------------------------------------------------- */

int iv_ota_verify_mem(const void *buf, size_t len, const iv_ota_self_t *self,
                      const uint8_t pubkey[IV_OTA_PUBKEY_LEN], iv_ota_manifest_t *out,
                      iv_ota_reason_t *why)
{
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *manifest;
    const uint8_t *payload;
    const uint8_t *sig;
    size_t         mlen;
    size_t         plen;
    size_t         signed_len;
    uint64_t       sum = 0u;
    uint32_t       i;

    if (buf == NULL || out == NULL || self == NULL || self->model == NULL ||
        self->model[0] == '\0')
        return IV_EINVAL;
    if (why != NULL)
        *why = IV_OTA_OK;

    /* 1. 结构（magic / 两段长度 / 总长精确匹配） ---------------------------- */
    if (len < IV_OTA_HEADER_LEN + IV_OTA_SIG_LEN)
        return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
    if (memcmp(p, IV_OTA_MAGIC, IV_OTA_MAGIC_LEN) != 0)
        return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
    mlen = rd_u32_le(p + 8);
    plen = rd_u32_le(p + 12);
    if (mlen == 0u || mlen > IV_OTA_MANIFEST_MAX)
        return fail(why, IV_OTA_EBADFMT, IV_EPROTO);
    if ((uint64_t)IV_OTA_HEADER_LEN + (uint64_t)mlen + (uint64_t)plen +
            (uint64_t)IV_OTA_SIG_LEN !=
        (uint64_t)len)
        return fail(why, IV_OTA_EBADFMT, IV_EPROTO);

    manifest   = p + IV_OTA_HEADER_LEN;
    payload    = manifest + mlen;
    sig        = payload + plen;
    signed_len = IV_OTA_HEADER_LEN + mlen + plen;

    /* 2. 清单（reason 由解析函数写入，EBADFMT 与 EPATH 都映射到 IV_EPROTO） --- */
    if (iv_ota_manifest_parse((const char *)manifest, mlen, out, why) != IV_OK)
        return IV_EPROTO;

    /* 3. 负载长度 vs 清单合计 --------------------------------------------- */
    for (i = 0u; i < out->n_files; i++)
        sum += (uint64_t)out->files[i].size;
    if (sum != (uint64_t)plen)
        return fail(why, IV_OTA_EFILE, IV_ECORRUPT);

    /* 4. 公钥 ------------------------------------------------------------ */
    if (!pubkey_usable(pubkey))
        return fail(why, IV_OTA_ENOKEY, IV_EAUTH);

    /* 5. 签名（覆盖 头+清单+负载，即签名之前的全部字节） --------------------- */
    if (!ed25519_verify(p, signed_len, sig, pubkey))
        return fail(why, IV_OTA_ESIG, IV_EAUTH);

    /* 6. 型号 ------------------------------------------------------------ */
    if (strcmp(out->model, self->model) != 0)
        return fail(why, IV_OTA_EMODEL, IV_EINVAL);

    /* 7. 硬件版本区间 ----------------------------------------------------- */
    if (self->hw_ver < out->hw_min || self->hw_ver > out->hw_max)
        return fail(why, IV_OTA_EHWVER, IV_ERANGE);

    /* 8. 软件版本（禁降级与同版本重装；本机版本不得低于 minrollback） --------- */
    if (iv_ota_ver_cmp(out->version, self->cur) <= 0)
        return fail(why, IV_OTA_EVER, IV_ESTATE);
    if (iv_ota_ver_cmp(self->cur, out->min_rollback) < 0)
        return fail(why, IV_OTA_EVER, IV_ESTATE);

    /* 9. 逐文件 SHA-256 与负载切片核对 ------------------------------------- */
    {
        size_t off = 0u;

        for (i = 0u; i < out->n_files; i++) {
            uint8_t     digest[IV_OTA_SHA256_LEN];
            iv_sha256_t c;

            iv_sha256_init(&c);
            iv_sha256_update(&c, payload + off, out->files[i].size);
            iv_sha256_final(&c, digest);
            if (memcmp(digest, out->files[i].sha256, IV_OTA_SHA256_LEN) != 0)
                return fail(why, IV_OTA_EFILE, IV_ECORRUPT);
            off += out->files[i].size;
        }
    }
    return IV_OK;
}

int iv_ota_verify_file(const char *path, const iv_ota_self_t *self,
                       const uint8_t pubkey[IV_OTA_PUBKEY_LEN], iv_ota_manifest_t *out,
                       iv_ota_reason_t *why)
{
    struct stat st;
    void       *map;
    size_t      len;
    int         fd;
    int         rc;

    if (path == NULL || out == NULL || self == NULL)
        return IV_EINVAL;
    if (why != NULL)
        *why = IV_OTA_OK;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return fail(why, IV_OTA_EIOFAIL, IV_EIO);
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        (void)close(fd);
        return fail(why, IV_OTA_EIOFAIL, IV_EIO);
    }
    if (st.st_size > OTA_PKG_MAX) {
        (void)close(fd);
        return fail(why, IV_OTA_EBADFMT, IV_ERANGE);
    }
    len = (size_t)st.st_size;
    /* 只读映射：本函数**不写、不创建、不改该文件**。 */
    map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    (void)close(fd); /* 映射已建立，fd 可以立刻关掉 */
    if (map == MAP_FAILED)
        return fail(why, IV_OTA_EIOFAIL, IV_EIO);

    rc = iv_ota_verify_mem(map, len, self, pubkey, out, why);
    (void)munmap(map, len);
    return rc;
}

int iv_ota_builtin_pubkey(uint8_t out[IV_OTA_PUBKEY_LEN])
{
    if (out == NULL)
        return IV_EINVAL;
    memset(out, 0, IV_OTA_PUBKEY_LEN);
    /* 当前工程**没有**量产发布公钥：见头文件"安全边界"。
     * 返回 ENOTSUP 而不是给一把假密钥 —— 假密钥会让所有包都被签名校验拒掉（行为同样
     * 是"全拒"），但调用方拿不到"是我没配密钥"这个信息，现场会把"密钥未部署"
     * 误判成"包被篡改"。 */
    return IV_ENOTSUP;
}

const char *iv_ota_reason_name(iv_ota_reason_t r)
{
    switch (r) {
    case IV_OTA_OK:
        return "ok";
    case IV_OTA_EBADFMT:
        return "bad-format";
    case IV_OTA_EPATH:
        return "bad-path";
    case IV_OTA_EFILE:
        return "file-mismatch";
    case IV_OTA_ENOKEY:
        return "no-pubkey";
    case IV_OTA_ESIG:
        return "bad-signature";
    case IV_OTA_EMODEL:
        return "model-mismatch";
    case IV_OTA_EHWVER:
        return "hw-out-of-range";
    case IV_OTA_EVER:
        return "bad-version";
    case IV_OTA_EIOFAIL:
        return "io-error";
    default:
        return "unknown";
    }
}
