/*
 * 帧校验（开发计划 M1-S3）
 *
 * 只实现现网**实际在用**的两种校验，规格与单片机工程
 * `General_Version/main/Middlewares/tool/src/crc.c` 严格对齐
 * （2026-09-28 实测：该固件的链接器 map 显示 `usMBCRC16` 与 `usSumFunction`
 *  均未被引用已被剔除，真正在跑的只有 `calc_crc8` 与 `CRC16_MODBUS`）：
 *
 *   CRC-8/SMBUS   : poly 0x07, init 0x00, 不反射, xorout 0x00
 *   CRC-16/MODBUS : poly 0x8005(反射后 0xA001), init 0xFFFF, 反射, xorout 0x0000
 *
 * 标准向量（"123456789"）：CRC8 = 0xF4、CRC16 = 0x4B37。
 *
 * 分段累计：两者都满足 `f(a||b) == f(b, seed=f(a, init))`，即把上一段返回值当
 * seed 传入即可（因为 xorout=0 且无最终反射，中间寄存器值就是返回值）。
 *
 * 为什么不用查表 / 不引外部库：
 *   1. libivcore 的硬约定是"仅 libc"，逐位实现零依赖正好合规；
 *   2. 查表要 768 字节 ROM（MCU 侧三张表就有这么大），逐位只有十几行代码；
 *   3. 帧长度只有几十到几百字节，逐位 8 次移位/字节在 T113（Cortex-A7）上
 *      开销可忽略。
 * 也不实现 CRC-32：现网没有该需求，留着重合库就是死代码。
 */
#ifndef IVSBOX_IV_CRC_H
#define IVSBOX_IV_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 起算种子，避免各调用方各写各的魔数 */
#define IV_CRC8_SEED_INIT         0x00u
#define IV_CRC16_MODBUS_SEED_INIT 0xFFFFu

/*
 * CRC-8/SMBUS。首次计算传 IV_CRC8_SEED_INIT，分段时传上一段的返回值。
 * data 为 NULL 时直接返回 seed（不访问内存）。
 */
uint8_t iv_crc8(const void *data, size_t len, uint8_t seed);

/*
 * CRC-16/MODBUS。首次计算传 IV_CRC16_MODBUS_SEED_INIT，分段时传上一段返回值。
 * data 为 NULL 时直接返回 seed（不访问内存）。
 *
 * 返回值为 CRC 寄存器原值；写进报文的字节序由帧格式（架构 §18.2）另行规定，
 * 本层不隐含任何字节序假设。
 */
uint16_t iv_crc16_modbus(const void *data, size_t len, uint16_t seed);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_CRC_H */
