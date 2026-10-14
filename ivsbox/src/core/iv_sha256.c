/*
 * IVSBox SHA-256 实现（libivcore，功能开发计划 M6-S6.4）
 *
 * 契约与用途见 include/ivsbox/iv_sha256.h 文件头。这里只写算法本身：
 * FIPS 180-4 的 SHA-256（64 轮、大端字数化、512 bit 分组、标准填充）。
 * 纯 libc、无表查（K 常量表是算法定义的一部分，不是"查表加速"）。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_sha256.h"

/* 每轮常数：前 64 个素数立方根小数部分的前 32 bit（FIPS 180-4 §4.2.2） */
static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (uint32_t)((x >> n) | (x << (32u - n)));
}

static void sha256_block(iv_sha256_t *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, d, e, f, g, h, cc;
    int      i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a  = c->h[0];
    b  = c->h[1];
    cc = c->h[2];
    d  = c->h[3];
    e  = c->h[4];
    f  = c->h[5];
    g  = c->h[6];
    h  = c->h[7];

    for (i = 0; i < 64; i++) {
        uint32_t s1  = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch  = (e & f) ^ ((~e) & g);
        uint32_t t1  = h + s1 + ch + K256[i] + w[i];
        uint32_t s0  = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2  = s0 + maj;

        h  = g;
        g  = f;
        f  = e;
        e  = d + t1;
        d  = cc;
        cc = b;
        b  = a;
        a  = t1 + t2;
    }

    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

void iv_sha256_init(iv_sha256_t *c)
{
    if (c == NULL)
        return;
    memset(c, 0, sizeof(*c));
    c->h[0] = 0x6a09e667u;
    c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u;
    c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu;
    c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu;
    c->h[7] = 0x5be0cd19u;
}

void iv_sha256_update(iv_sha256_t *c, const void *data, size_t len)
{
    const uint8_t *p = data;

    if (c == NULL || (p == NULL && len != 0u))
        return;

    c->total += (uint64_t)len;

    if (c->buflen != 0u) {
        size_t need = 64u - c->buflen;

        if (need > len)
            need = len;
        memcpy(c->buf + c->buflen, p, need);
        c->buflen += need;
        p += need;
        len -= need;
        if (c->buflen == 64u) {
            sha256_block(c, c->buf);
            c->buflen = 0u;
        }
    }

    while (len >= 64u) {
        sha256_block(c, p);
        p += 64u;
        len -= 64u;
    }

    if (len != 0u) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

void iv_sha256_final(iv_sha256_t *c, uint8_t out[IV_SHA256_DIGEST_LEN])
{
    uint64_t bits;
    uint8_t  pad[72];
    size_t   padlen;
    int      i;

    if (c == NULL || out == NULL)
        return;

    bits = c->total * 8u;

    /* 填充：0x80 ＋ 若干 0x00 使长度 ≡ 56 (mod 64)，再接 64 bit 大端位长 */
    memset(pad, 0, sizeof(pad));
    pad[0]   = 0x80u;
    padlen   = (c->buflen < 56u) ? (56u - c->buflen) : (120u - c->buflen);
    for (i = 0; i < 8; i++)
        pad[padlen + (size_t)i] = (uint8_t)((bits >> (56 - 8 * i)) & 0xffu);

    /* update 会把 total 也加上去 —— 这里直接压块，绕开 total 计数 */
    {
        size_t   len = padlen + 8u;
        const uint8_t *p = pad;

        if (c->buflen != 0u) {
            size_t need = 64u - c->buflen;

            memcpy(c->buf + c->buflen, p, need);
            sha256_block(c, c->buf);
            p += need;
            len -= need;
            c->buflen = 0u;
        }
        while (len >= 64u) {
            sha256_block(c, p);
            p += 64u;
            len -= 64u;
        }
    }

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)((c->h[i] >> 24) & 0xffu);
        out[i * 4 + 1] = (uint8_t)((c->h[i] >> 16) & 0xffu);
        out[i * 4 + 2] = (uint8_t)((c->h[i] >> 8) & 0xffu);
        out[i * 4 + 3] = (uint8_t)(c->h[i] & 0xffu);
    }
}

int iv_sha256_file(const char *path, uint8_t out[IV_SHA256_DIGEST_LEN])
{
    /* 分块缓冲放静态区：本函数可能被 Web 请求路径调用，不该在栈上放 16 KiB */
    static uint8_t buf[16384];
    iv_sha256_t    c;
    FILE          *f;

    if (path == NULL || out == NULL)
        return IV_EINVAL;

    f = fopen(path, "rb");
    if (f == NULL)
        return IV_EIO;

    iv_sha256_init(&c);
    for (;;) {
        size_t n = fread(buf, 1u, sizeof(buf), f);

        if (n != 0u)
            iv_sha256_update(&c, buf, n);
        if (n < sizeof(buf)) {
            if (ferror(f)) {
                (void)fclose(f);
                return IV_EIO;
            }
            break;
        }
    }
    (void)fclose(f);
    iv_sha256_final(&c, out);
    return IV_OK;
}

void iv_sha256_hex(const uint8_t digest[IV_SHA256_DIGEST_LEN], char *out, size_t cap)
{
    static const char hexd[] = "0123456789abcdef";
    size_t            i;

    if (out == NULL || cap == 0u)
        return;
    if (digest == NULL) {
        out[0] = '\0';
        return;
    }
    for (i = 0; i < IV_SHA256_DIGEST_LEN && (i * 2u + 2u) <= cap; i++) {
        out[i * 2u]      = hexd[(digest[i] >> 4) & 0x0fu];
        out[i * 2u + 1u] = hexd[digest[i] & 0x0fu];
    }
    out[(i * 2u < cap) ? i * 2u : cap - 1u] = '\0';
}
