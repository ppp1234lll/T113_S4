/*
 * ivsboxd 主控进程入口
 */
#include <stdio.h>
#include "ivsbox/ivsbox.h"

extern int iv_log_init(const char *ident);
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
    printf("ivsboxd initialized\n");
    ivsbox_fini();
    return 0;
}
