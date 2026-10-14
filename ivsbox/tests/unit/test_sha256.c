/*
 * iv_sha256 单测（libivcore，功能开发计划 M6-S6.4 的"校验值"）
 *
 * 靶子是**标准向量**，不是自证：拿 FIPS/NIST 公开的三条向量核对，加上
 * "分片喂入与一次性喂入结果相同"（Web 下载是按 Range 分片读的，这条必须有）
 * 和 `iv_sha256_file()` 的真文件路径。
 *
 * 覆盖：
 *   s01 空串 / "abc" / 56 字节（跨 512 bit 填充边界）/ 百万个 'a'（多块）
 *   s02 分片喂入（1/3/7/63/64/65 字节步长）与一次性喂入一致
 *   s03 iv_sha256_file() 与内存摘要一致；不存在文件返回 IV_EIO
 *   s04 hex 转换与容量边界（cap 小于 65 也不越界）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_sha256.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void digest_hex(const uint8_t *d, char *out)
{
    iv_sha256_hex(d, out, IV_SHA256_HEX_LEN + 1u);
}

static void one_shot(const void *data, size_t len, char *hex)
{
    iv_sha256_t c;
    uint8_t     d[IV_SHA256_DIGEST_LEN];

    iv_sha256_init(&c);
    iv_sha256_update(&c, data, len);
    iv_sha256_final(&c, d);
    digest_hex(d, hex);
}

/* ---------------- s01 标准向量 ---------------- */
static void case_vectors(void)
{
    static const char *abc   = "abc";
    static const char *n56   = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    char               hex[IV_SHA256_HEX_LEN + 1u];

    one_shot("", 0u, hex);
    chk(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
        "s01: empty string vector");

    one_shot(abc, 3u, hex);
    chk(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
        "s01: 'abc' vector");

    one_shot(n56, strlen(n56), hex);
    chk(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
        "s01: 56-byte vector (padding boundary)");

    {
        char *big = malloc(1000000u);
        char  want[IV_SHA256_HEX_LEN + 1u];

        chk(big != NULL, "s01: malloc");
        if (big != NULL) {
            memset(big, 'a', 1000000u);
            one_shot(big, 1000000u, hex);
            /* 标准向量：'a' × 1,000,000 */
            memcpy(want, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", 65u);
            chk(strcmp(hex, want) == 0, "s01: one million 'a'");
            free(big);
        }
    }
}

/* ---------------- s02 分片喂入 ---------------- */
static void case_chunked(void)
{
    static const size_t steps[] = { 1u, 3u, 7u, 63u, 64u, 65u, 127u };
    char   data[600];
    char   ref[IV_SHA256_HEX_LEN + 1u];
    size_t si;
    size_t i;

    for (i = 0; i < sizeof(data); i++)
        data[i] = (char)('A' + (int)(i % 26u));

    one_shot(data, sizeof(data), ref);

    for (si = 0; si < sizeof(steps) / sizeof(steps[0]); si++) {
        iv_sha256_t c;
        uint8_t     d[IV_SHA256_DIGEST_LEN];
        char        hex[IV_SHA256_HEX_LEN + 1u];
        size_t      off = 0u;

        iv_sha256_init(&c);
        while (off < sizeof(data)) {
            size_t n = steps[si];

            if (n > sizeof(data) - off)
                n = sizeof(data) - off;
            iv_sha256_update(&c, data + off, n);
            off += n;
        }
        iv_sha256_final(&c, d);
        digest_hex(d, hex);
        chk(strcmp(hex, ref) == 0, "s02: chunked feed == one-shot");
    }
}

/* ---------------- s03 文件 ---------------- */
static void case_file(void)
{
    char    tmpl[] = "/tmp/ivsha_XXXXXX";
    char    path[128];
    char    body[300];
    char    ref[IV_SHA256_HEX_LEN + 1u];
    char    hex[IV_SHA256_HEX_LEN + 1u];
    uint8_t d[IV_SHA256_DIGEST_LEN];
    FILE   *f;
    int     fd;
    size_t  i;

    fd = mkstemp(tmpl);
    chk(fd >= 0, "s03: mkstemp");
    if (fd < 0)
        return;
    (void)close(fd);

    for (i = 0; i < sizeof(body); i++)
        body[i] = (char)('a' + (int)(i % 7u));
    f = fopen(tmpl, "wb");
    chk(f != NULL, "s03: fopen write");
    if (f != NULL) {
        chk(fwrite(body, 1u, sizeof(body), f) == sizeof(body), "s03: fwrite");
        (void)fclose(f);
    }
    one_shot(body, sizeof(body), ref);
    chk(iv_sha256_file(tmpl, d) == IV_OK, "s03: file digest ok");
    digest_hex(d, hex);
    chk(strcmp(hex, ref) == 0, "s03: file digest == memory digest");

    chk(iv_sha256_file("/tmp/ivsha_does_not_exist_xyz", d) == IV_EIO, "s03: missing file EIO");
    chk(iv_sha256_file(NULL, d) == IV_EINVAL, "s03: NULL path EINVAL");

    (void)snprintf(path, sizeof(path), "%s", tmpl);
    chk(unlink(path) == 0, "s03: cleanup");
}

/* ---------------- s04 hex 边界 ---------------- */
static void case_hex(void)
{
    uint8_t d[IV_SHA256_DIGEST_LEN];
    char    buf[IV_SHA256_HEX_LEN + 1u];
    int     i;

    for (i = 0; i < (int)IV_SHA256_DIGEST_LEN; i++)
        d[i] = (uint8_t)i;

    memset(buf, 'Z', sizeof(buf));
    iv_sha256_hex(d, buf, sizeof(buf));
    chk(strlen(buf) == IV_SHA256_HEX_LEN, "s04: hex length 64");
    chk(strncmp(buf, "000102030405060708090a0b0c0d0e0f", 32) == 0, "s04: hex prefix");
    chk(buf[63] == 'f' && buf[64] == '\0', "s04: hex tail + NUL");

    /* cap 小于 65：只写能装下的，且必须终止 */
    memset(buf, 'Z', sizeof(buf));
    iv_sha256_hex(d, buf, 5u);
    chk(buf[4] == '\0', "s04: short cap still NUL-terminated");
    chk(buf[0] == '0' && buf[1] == '0', "s04: short cap writes from the start");

    iv_sha256_hex(d, NULL, 10u);
    iv_sha256_hex(NULL, buf, sizeof(buf));
    chk(buf[0] == '\0', "s04: NULL digest -> empty string");
}

int main(void)
{
    case_vectors();
    case_chunked();
    case_file();
    case_hex();

    if (g_fail != 0) {
        fprintf(stderr, "test_sha256 FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_sha256 passed (FIPS vectors, chunked feed, file digest, hex bounds)\n");
    return 0;
}
