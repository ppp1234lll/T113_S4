#ifndef LOGGER_H
#define LOGGER_H

void log_init(void);
void log_write(const char *level, const char *fmt, ...);

// 重新设计的平滑命名
#define log_info(fmt, ...) log_write("info",  fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...) log_write("warn",  fmt, ##__VA_ARGS__)
#define log_err(fmt, ...)  log_write("error", fmt, ##__VA_ARGS__)

#endif
