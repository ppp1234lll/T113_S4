/*
 * core 层单元测试桩
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

extern uint32_t iv_crc32(const void *data, size_t len);

int main(void)
{
    if (iv_crc32("", 0) != 0) {
        fprintf(stderr, "crc stub failed\n");
        return 1;
    }
    printf("test_core passed\n");
    return 0;
}
