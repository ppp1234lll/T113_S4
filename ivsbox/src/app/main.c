/*
 * ivsboxd 主控进程入口
 */
#include <stdio.h>
#include "ivsbox/ivsbox.h"
#include "ivsbox/iv_log.h"

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
    IV_LOG_I("app", "ivsboxd started, version %d.%d.%d",
             IVSBOX_VERSION_MAJOR, IVSBOX_VERSION_MINOR, IVSBOX_VERSION_PATCH);
    return 0;
}

void ivsbox_fini(void)
{
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (ivsbox_init() != 0) {
        fprintf(stderr, "ivsboxd init failed\n");
        return 1;
    }
    printf("hello, ivsbox %d.%d.%d\n",
           IVSBOX_VERSION_MAJOR, IVSBOX_VERSION_MINOR, IVSBOX_VERSION_PATCH);
    ivsbox_fini();
    return 0;
}
