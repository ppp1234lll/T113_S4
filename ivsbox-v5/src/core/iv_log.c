/* iv_log.c —— 分级日志（骨架实现，仅 libc）
 *
 * 层内约定：core 不得依赖 hal（架构文档 §6），因此这里自带一个最小的单调
 * 时间取值函数，而不引用 hal 的 ivs_clock_mono_ms()。
 *
 * 日志文本一律 ASCII，中文只允许出现在注释里 —— 板端控制台为 GBK，
 * 中文日志抓下来必然是乱码。
 */
#include "ivsbox/iv_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static ivs_log_level_t g_level = IVS_LOG_INFO;

void ivs_log_set_level(ivs_log_level_t level)
{
    g_level = level;
}

ivs_log_level_t ivs_log_get_level(void)
{
    return g_level;
}

static const char *log_level_tag(ivs_log_level_t level)
{
    switch (level) {
    case IVS_LOG_ERROR: return "E";
    case IVS_LOG_WARN:  return "W";
    case IVS_LOG_INFO:  return "I";
    case IVS_LOG_DEBUG: return "D";
    default:            return "?";
    }
}

static double log_mono_seconds(void)
{
#if defined(__linux__) || defined(__unix__)
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#else
    return 0.0; /* 主机（Windows）无单调时钟，日志时间戳退化为 0 */
#endif
}

void ivs_log_emit(ivs_log_level_t level, const char *module, const char *fmt, ...)
{
    va_list ap;

    if ((int)level > (int)g_level) {
        return;
    }

    (void)fprintf(stderr, "[%10.3f][%s][%s] ", log_mono_seconds(), log_level_tag(level),
                  (module != NULL) ? module : "-");

    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);

    (void)fputc('\n', stderr);
}
