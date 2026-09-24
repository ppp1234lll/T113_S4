#ifndef IVS_CRC_H
#define IVS_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CRC 校验（core 层，S06 板间链路 / S08 IPC 等共用）。
 *
 * 实现策略（2026-09-24 用户决策，D2）：不自写多项式位运算，统一使用
 * 系统 zlib 库的 crc32()。理由：
 *   1. zlib 是 Linux 用户态事实标准的 CRC 库（glibc 不提供 CRC 函数），
 *      实现久经考验，避免手写查表/位运算引入向量错误；
 *   2. 开发列表原计划的 CRC8 / CRC16-Modbus 属手写实现，且现网帧实际
 *      采用的 CRC 类型在 S06 对帧格式确认前为"待确认"（开发列表 §3-S06
 *      兼容纪律：第一版严格按现网既有帧实现），确认后再按需扩展本文件。
 *
 * 依赖事实（2026-09-24 实测）：
 *   - 板端：/usr/lib/libz.so.1.2.11（含 libz.so 开发符号链接）；
 *   - 交叉 sysroot：usr/include/zlib.h + usr/lib/libz.so（同 1.2.11）。
 */

/*
 * CRC-32（zlib 口径：多项式 0x04C11DB7 反射，初值取反，结果取反，
 * 即常见 "CRC-32/ISO-HDLC"，与 zip / 以太网 FCS 一致）。
 *
 * 支持分段累计（S06 半包场景）：首次调用 crc 传 0，后续把上次返回值
 * 传回即可：
 *   uint32_t c = ivs_crc32(0, buf, n);   // 第一段
 *   c = ivs_crc32(c, buf + n, m);        // 后续段累计
 */
uint32_t ivs_crc32(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* IVS_CRC_H */
