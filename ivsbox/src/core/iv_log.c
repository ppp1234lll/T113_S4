/*
 * 日志桩（S02 中替换为对接 syslogd 的实现）
 */
#include <stdio.h>

int iv_log_init(const char *ident)
{
    (void)ident;
    return 0;
}

void iv_log_info(const char *fmt, ...)
{
    (void)fmt;
}
