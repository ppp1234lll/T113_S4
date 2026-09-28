/*
 * 帧校验实现（开发计划 M1-S3）
 *
 * 逐字节按位实现：无表、无外部库依赖，仅 stdint/stddef（libivcore 仅 libc）。
 * 参数与标准向量见 iv_crc.h。
 *
 * 与单片机工程 `General_Version/main/Middlewares/tool/src/crc.c` 的现网实现
 * 已验证逐字节等价（长度 0..512 全扫、0 处不一致，见 docs/修改记录.md）。
 */
#include "ivsbox/iv_crc.h"

/* X^8 + X^2 + X^1 + 1，MSB-first 取用 */
#define IV_CRC8_POLY   0x07u
/* 0x8005 的反射形式，LSB-first 取用（CRC-16/MODBUS） */
#define IV_CRC16_POLY  0xA001u

uint8_t iv_crc8(const void *data, size_t len, uint8_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t        c = seed;
    size_t         i;
    int            b;

    if (p == NULL)
        return seed;

    for (i = 0; i < len; i++) {
        uint8_t hi;

        c ^= p[i];
        for (b = 0; b < 8; b++) {
            /* 先算出左移一位，再按移位前的最高位决定是否异或多项式 */
            hi = (uint8_t)(c << 1);
            c  = (uint8_t)((c & 0x80u) ? (uint8_t)(hi ^ IV_CRC8_POLY) : hi);
        }
    }
    return c;
}

uint16_t iv_crc16_modbus(const void *data, size_t len, uint16_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t       c = seed;
    size_t         i;
    int            b;

    if (p == NULL)
        return seed;

    for (i = 0; i < len; i++) {
        uint16_t lo;

        c ^= (uint16_t)p[i];
        for (b = 0; b < 8; b++) {
            lo = (uint16_t)(c >> 1);
            c  = (uint16_t)((c & 0x0001u) ? (uint16_t)(lo ^ IV_CRC16_POLY) : lo);
        }
    }
    return c;
}
