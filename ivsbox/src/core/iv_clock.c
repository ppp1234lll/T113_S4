/*
 * 单调时钟桩（S02 中替换为 clock_gettime(CLOCK_MONOTONIC) 封装）
 */
#include <stdint.h>

uint64_t iv_clock_monotonic_ms(void)
{
    return 0;
}
