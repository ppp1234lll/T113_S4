/*
 * CRC 校验实现 —— 系统 zlib 的薄包装。
 * 决策与依赖事实见 include/ivsbox/iv_crc.h 头注，此处不重复。
 */
#include "ivsbox/iv_crc.h"

#include <zlib.h>

uint32_t ivs_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    /* zlib crc32() 的长度参数为 uInt（unsigned int，32 位）。
     * 项目内单段缓冲远小于 4GiB，size_t -> uInt 截断在此安全。 */
    return crc32(crc, data, (uInt)len);
}
