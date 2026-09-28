/*
 * ivsboxd 主控进程入口
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/ivsbox.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_version.h"

extern int iv_hal_init(void);
extern int iv_modules_init(void);

int ivsbox_init(void)
{
    if (iv_log_init("ivsboxd") != 0)
        return -1;
    if (iv_hal_init() != 0)
        return -1;
    if (iv_modules_init() != 0)
        return -1;
    IV_LOG_I("app", "ivsboxd started, version %s", iv_version_string());
    return 0;
}

void ivsbox_fini(void)
{
}

int main(int argc, char *argv[])
{
    int i;

    /*
     * --version 放在任何子系统初始化之前处理：不写日志、不建 /opt/log、
     * 不碰 HAL，保证只读环境（出厂、检修）下也能查到版本。
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("ivsboxd %s\n", iv_version_string());
            return 0;
        }
    }

    if (ivsbox_init() != 0) {
        fprintf(stderr, "ivsboxd init failed\n");
        return 1;
    }
    printf("hello, ivsbox %s\n", iv_version_string());
    ivsbox_fini();
    return 0;
}
