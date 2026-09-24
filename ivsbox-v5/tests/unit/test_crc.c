/*
 * CRC 单元测试（S02 步骤 1）。
 *
 * 向量为 CRC-32/ISO-HDLC 公开检验值（RFC 1952 / zip 口径）：
 *   ""          -> 0x00000000
 *   "123456789" -> 0xCBF43926
 *   "A"         -> 0xD3D99E8B
 * 另验证分段累计与一次性计算一致（对应 S06 半包到达场景）。
 * 日志文本一律 ASCII（工程纪律，开发列表 §6 第 8 条）。
 */
#include "ivsbox/iv_crc.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %s: got=0x%08X want=0x%08X\n", name, got, want);
        g_fail++;
    } else {
        printf("PASS %s: 0x%08X\n", name, got);
    }
}

int main(void)
{
    static const char vec[] = "123456789";
    uint32_t split;

    expect_u32("crc32_empty",
               ivs_crc32(0, (const uint8_t *)"", 0), 0x00000000u);
    expect_u32("crc32_123456789",
               ivs_crc32(0, (const uint8_t *)vec, sizeof(vec) - 1),
               0xCBF43926u);
    expect_u32("crc32_single_a",
               ivs_crc32(0, (const uint8_t *)"A", 1), 0xD3D99E8Bu);

    /* 分段累计 == 一次性计算（4 + 5 两段） */
    split = ivs_crc32(0, (const uint8_t *)vec, 4);
    split = ivs_crc32(split, (const uint8_t *)vec + 4, sizeof(vec) - 1 - 4);
    expect_u32("crc32_split_eq_whole", split, 0xCBF43926u);

    if (g_fail == 0) {
        printf("ALL CRC TESTS PASSED\n");
        return 0;
    }
    return 1;
}
