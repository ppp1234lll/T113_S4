/*
 * 单调时钟实现：clock_gettime(CLOCK_MONOTONIC) 薄封装（hal 层）。
 * 依赖 POSIX 时钟接口，构建已统一 -D_GNU_SOURCE（host / tina-arm 两侧一致）。
 */
#include <time.h>

#include "ivsbox/iv_clock.h"

/* CLOCK_MONOTONIC 是内核维护的合法时钟，Linux 下调用不会失败；
 * 兜底返回 0 仅防御异常环境，保证"连续调用差值 >= 0"恒成立。 */
static int iv_clock_now(struct timespec *ts)
{
    return clock_gettime(CLOCK_MONOTONIC, ts);
}

uint64_t iv_clock_monotonic_ms(void)
{
    struct timespec ts;

    if (iv_clock_now(&ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t iv_clock_monotonic_us(void)
{
    struct timespec ts;

    if (iv_clock_now(&ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
