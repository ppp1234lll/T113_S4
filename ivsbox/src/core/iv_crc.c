/*
 * CRC 桩（S02 中替换为 zlib crc32 薄包装）
 */
#include <stdint.h>
#include <stddef.h>

uint32_t iv_crc32(const void *data, size_t len)
{
    (void)data;
    (void)len;
    return 0;
}
