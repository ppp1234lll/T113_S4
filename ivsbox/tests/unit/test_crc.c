/*
 * iv_crc 单测（开发计划 M1-S3）
 *
 * 覆盖：
 *   1) 标准向量 "123456789" -> CRC8 0xF4 / CRC16 0x4B37（CRC-8/SMBUS、CRC-16/MODBUS）；
 *   2) 空串与 NULL：返回 init seed，不访问内存；
 *   3) 黄金表：由**单片机现网实现**（calc_crc8 / CRC16_MODBUS）生成的长度扫描向量，
 *      逐字节钉死跨端一致；数据填充 buf[i] = i & 0xFF；
 *   4) 字符串与短帧向量（同样是 MCU 现网实现产出的值）；
 *   5) 分段累计与一次算完等价（二段全切分点 + 三段）；
 *   6) 超长缓冲（1024 字节）边界。
 *
 * 注意：本文件的期望值**不是**从被测实现反推的，而是先用 MCU 固件里的现网实现
 * 算出来再写的（生成过程见 docs/修改记录.md），因此能抓住"实现被改坏"的回归。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_crc.h"

static int    g_fail;
static uint8_t buf[1024];

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 由 MCU 现网实现 calc_crc8 / CRC16_MODBUS 生成，数据 buf[i] = i & 0xFF */
static const struct {
    size_t   len;
    uint8_t  c8;
    uint16_t c16;
} g_golden[] = {
    {    0, 0x00, 0xFFFF },
    {    1, 0x00, 0x40BF },
    {    2, 0x07, 0x70C0 },
    {    3, 0x1B, 0x91F1 },
    {    4, 0x48, 0x8510 },
    {    7, 0x2F, 0xC6A1 },
    {    8, 0xD8, 0x7A46 },
    {   15, 0x14, 0x757A },
    {   16, 0x41, 0xE7B4 },
    {   31, 0xC7, 0xEBD5 },
    {   32, 0x06, 0x576B },
    {   33, 0xF2, 0x3717 },
    {   63, 0xBE, 0x5921 },
    {   64, 0x8E, 0x08D9 },
    {  100, 0x0F, 0x2BEB },
    {  127, 0x31, 0x5A79 },
    {  128, 0xED, 0x02DA },
    {  255, 0x21, 0xADD6 },
    {  256, 0x14, 0xDE6C },
    {  512, 0x17, 0xFD70 },
    { 1024, 0x12, 0xECFE },
};

int main(void)
{
    static const char *V = "123456789";
    size_t  i;
    size_t  k;
    size_t  split;
    int     seg_bad8  = 0;
    int     seg_bad16 = 0;
    int     gold_bad8 = 0;
    int     gold_bad16 = 0;

    for (i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i & 0xFF);

    /* 1) 标准向量 */
    chk(iv_crc8(V, 9, IV_CRC8_SEED_INIT) == 0xF4u, "vector crc8(\"123456789\") == 0xF4");
    chk(iv_crc16_modbus(V, 9, IV_CRC16_MODBUS_SEED_INIT) == 0x4B37u,
        "vector crc16(\"123456789\") == 0x4B37");

    /* 2) 空串 / NULL */
    chk(iv_crc8(V, 0, IV_CRC8_SEED_INIT) == 0x00u, "empty crc8 == init seed");
    chk(iv_crc16_modbus(V, 0, IV_CRC16_MODBUS_SEED_INIT) == 0xFFFFu, "empty crc16 == init seed");
    chk(iv_crc8(NULL, 0, 0x5Au) == 0x5Au, "NULL crc8 returns seed");
    chk(iv_crc16_modbus(NULL, 0, 0x1234u) == 0x1234u, "NULL crc16 returns seed");
    chk(iv_crc8(NULL, 16, 0x5Au) == 0x5Au, "NULL with len>0 does not crash, returns seed");

    /* 3) 黄金表（跨端一致的钉死点） */
    for (k = 0; k < sizeof(g_golden) / sizeof(g_golden[0]); k++) {
        if (iv_crc8(buf, g_golden[k].len, IV_CRC8_SEED_INIT) != g_golden[k].c8)
            gold_bad8++;
        if (iv_crc16_modbus(buf, g_golden[k].len, IV_CRC16_MODBUS_SEED_INIT) != g_golden[k].c16)
            gold_bad16++;
    }
    chk(gold_bad8 == 0, "golden table: crc8 matches MCU for all lengths");
    chk(gold_bad16 == 0, "golden table: crc16 matches MCU for all lengths");

    /* 4) 字符串与短帧向量（值同样来自 MCU 现网实现） */
    chk(iv_crc8("a", 1, IV_CRC8_SEED_INIT) == 0x20u, "crc8(\"a\")");
    chk(iv_crc16_modbus("a", 1, IV_CRC16_MODBUS_SEED_INIT) == 0xA87Eu, "crc16(\"a\")");
    chk(iv_crc8("abc", 3, IV_CRC8_SEED_INIT) == 0x5Fu, "crc8(\"abc\")");
    chk(iv_crc16_modbus("abc", 3, IV_CRC16_MODBUS_SEED_INIT) == 0x5749u, "crc16(\"abc\")");
    chk(iv_crc8("hello, ivsbox", 13, IV_CRC8_SEED_INIT) == 0x09u, "crc8(\"hello, ivsbox\")");
    chk(iv_crc16_modbus("hello, ivsbox", 13, IV_CRC16_MODBUS_SEED_INIT) == 0x588Fu,
        "crc16(\"hello, ivsbox\")");
    {
        static const uint8_t f1[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
        static const uint8_t f2[] = {0x01, 0x04, 0x02, 0x00, 0x00};
        static const uint8_t f3[] = {0x00};
        static const uint8_t f4[] = {0xAA, 0x55};
        static const uint8_t f5[] = {0xAA, 0x55, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

        chk(iv_crc8(f1, sizeof(f1), IV_CRC8_SEED_INIT) == 0x88u, "framelike 01 03 00 00 00 01 crc8");
        chk(iv_crc16_modbus(f1, sizeof(f1), IV_CRC16_MODBUS_SEED_INIT) == 0x0A84u,
            "framelike 01 03 00 00 00 01 crc16");
        chk(iv_crc8(f2, sizeof(f2), IV_CRC8_SEED_INIT) == 0xECu, "framelike 01 04 02 00 00 crc8");
        chk(iv_crc16_modbus(f2, sizeof(f2), IV_CRC16_MODBUS_SEED_INIT) == 0x30B9u,
            "framelike 01 04 02 00 00 crc16");
        chk(iv_crc8(f3, sizeof(f3), IV_CRC8_SEED_INIT) == 0x00u, "single 0x00 crc8");
        chk(iv_crc16_modbus(f3, sizeof(f3), IV_CRC16_MODBUS_SEED_INIT) == 0x40BFu,
            "single 0x00 crc16");
        chk(iv_crc8(f4, sizeof(f4), IV_CRC8_SEED_INIT) == 0x36u, "AA 55 crc8");
        chk(iv_crc16_modbus(f4, sizeof(f4), IV_CRC16_MODBUS_SEED_INIT) == 0x2FBFu, "AA 55 crc16");
        chk(iv_crc8(f5, sizeof(f5), IV_CRC8_SEED_INIT) == 0x7Cu, "AA 55 01..06 crc8");
        chk(iv_crc16_modbus(f5, sizeof(f5), IV_CRC16_MODBUS_SEED_INIT) == 0xCF75u,
            "AA 55 01..06 crc16");
    }

    /* 5) 分段累计：任意切分点都应等于一次算完 */
    for (split = 0; split <= 64u; split++) {
        uint8_t  s8  = iv_crc8(buf + split, 64u - split,
                               iv_crc8(buf, split, IV_CRC8_SEED_INIT));
        uint16_t s16 = iv_crc16_modbus(buf + split, 64u - split,
                                       iv_crc16_modbus(buf, split, IV_CRC16_MODBUS_SEED_INIT));

        if (s8 != iv_crc8(buf, 64u, IV_CRC8_SEED_INIT))
            seg_bad8++;
        if (s16 != iv_crc16_modbus(buf, 64u, IV_CRC16_MODBUS_SEED_INIT))
            seg_bad16++;
    }
    chk(seg_bad8 == 0, "crc8 segmented accumulate equals whole (split 0..64)");
    chk(seg_bad16 == 0, "crc16 segmented accumulate equals whole (split 0..64)");

    /* 三段切分 */
    {
        uint8_t  a8  = iv_crc8(buf, 10, IV_CRC8_SEED_INIT);
        uint8_t  b8  = iv_crc8(buf + 10, 20, a8);
        uint8_t  c8  = iv_crc8(buf + 30, 34, b8);
        uint16_t a16 = iv_crc16_modbus(buf, 10, IV_CRC16_MODBUS_SEED_INIT);
        uint16_t b16 = iv_crc16_modbus(buf + 10, 20, a16);
        uint16_t c16 = iv_crc16_modbus(buf + 30, 34, b16);

        chk(c8 == iv_crc8(buf, 64u, IV_CRC8_SEED_INIT), "crc8 three-way split equals whole");
        chk(c16 == iv_crc16_modbus(buf, 64u, IV_CRC16_MODBUS_SEED_INIT),
            "crc16 three-way split equals whole");
    }

    /* seed 真的参与运算（非默认起算能得到不同结果，且空输入原样返回） */
    chk(iv_crc8(buf, 4, 0xFFu) != iv_crc8(buf, 4, 0x00u), "crc8 seed affects result");
    chk(iv_crc16_modbus(buf, 4, 0x0000u) != iv_crc16_modbus(buf, 4, 0xFFFFu),
        "crc16 seed affects result");

    /* 6) 超长缓冲（1024 字节，已含在黄金表里，这里再显式跑一次边界） */
    chk(iv_crc8(buf, sizeof(buf), IV_CRC8_SEED_INIT) == 0x12u, "1024-byte crc8");
    chk(iv_crc16_modbus(buf, sizeof(buf), IV_CRC16_MODBUS_SEED_INIT) == 0xECFEu,
        "1024-byte crc16");

    if (g_fail) {
        fprintf(stderr, "test_crc failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_crc passed (crc8=0xF4 crc16=0x4B37, golden %u lengths)\n",
           (unsigned)(sizeof(g_golden) / sizeof(g_golden[0])));
    return 0;
}
