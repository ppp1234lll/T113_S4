#include "logger.h"
#include "defs.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>  // 用于 dirname 函数

void log_init(void) {
    // 从 PATH_LOG_FILE 中提取目录部分，例如从 "/opt/log/media.log" 提取出 "/opt/log"
    char path_buf[256];
    strncpy(path_buf, PATH_LOG_FILE, sizeof(path_buf));
    char *dir = dirname(path_buf);

    struct stat st = {0};
    if (stat(dir, &st) == -1) {
        mkdir(dir, 0755);
    }
}

void log_write(const char *level, const char *fmt, ...) {
    struct stat st;
    
    if (stat(PATH_LOG_FILE, &st) == 0 && st.st_size > LOG_MAX_FILE_SIZE) {
        char backup[300];
        // 滚动逻辑：media.log -> media.log.bak
        snprintf(backup, sizeof(backup), "%s.bak", PATH_LOG_FILE);
        rename(PATH_LOG_FILE, backup);
    }

    // 3. 打开统一配置的路径
    FILE *fp = fopen(PATH_LOG_FILE, "a");
    if (!fp) return;

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);

    fprintf(fp, "%s [%s] ", time_str, level);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fprintf(fp, "\n");
    fclose(fp);
}
