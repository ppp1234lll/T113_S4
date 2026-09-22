/* iv_clock.c —— 时间源实现（hal 层，仅 libc + POSIX）
 *
 * 时钟纪律见 iv_clock.h 与架构文档 §7.7：时长/超时/窗口/退避一律走单调时钟，
 * 墙钟只用于展示、日志时间戳与证书有效期校验。
 */
#include "ivsbox/iv_clock.h"

#include <time.h>

uint64_t ivs_clock_wall_ms(void)
{
    struct timespec ts;

    (void)timespec_get(&ts, TIME_UTC); /* C11 标准，MinGW 与 glibc 均支持 */
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

uint64_t ivs_clock_mono_ms(void)
{
#if defined(__linux__) || defined(__unix__)
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
    }
#endif
    return ivs_clock_wall_ms(); /* 主机（Windows）无 CLOCK_MONOTONIC 时的回退 */
}
