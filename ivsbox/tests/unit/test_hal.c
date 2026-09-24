/*
 * HAL 层单元测试桩
 */
#include <stdio.h>

extern int iv_hal_init(void);

int main(void)
{
    if (iv_hal_init() != 0) {
        fprintf(stderr, "hal init failed\n");
        return 1;
    }
    printf("test_hal passed\n");
    return 0;
}
