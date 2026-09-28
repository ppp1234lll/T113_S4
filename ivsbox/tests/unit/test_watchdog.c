/*
 * iv_watchdog 单测（libivhal）。
 *
 * **本单测绝不打开真实 /dev/watchdog** —— open() 会立刻激活看门狗，在板子上
 * 不喂就是整机复位，在宿主/VM 上则根本没有该设备。全部用 pipe 假 fd 覆盖
 * 参数校验与 write 契约；ioctl 那条路（SETTIMEOUT 是否被 sunxi-wdt 接受）
 * 只能在板端单独实测，不在本单测范围内。
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_watchdog.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

int main(void)
{
    int  p[2];
    char c = 0;

    if (pipe(p) != 0) {
        fprintf(stderr, "FAIL: pipe(): %s\n", strerror(errno));
        return 1;
    }

    /* 1) keepalive 就是"写 1 个字节"：读端必须恰好收到 1 字节，
     *    且**不是 'V'** —— watchdog core 规定写单个 'V' 等于请求关闭设备。 */
    chk(iv_watchdog_keepalive(p[1]) == 0, "keepalive on writable fd returns 0");
    chk(read(p[0], &c, 1) == 1, "keepalive wrote exactly 1 byte");
    chk(c == '\0', "keepalive byte is 0x00, never the 'V' close magic");

    /* 2) 参数校验：负 fd / 负秒数一律拒绝 */
    chk(iv_watchdog_keepalive(-1) == -1, "keepalive(-1) returns -1");
    chk(iv_watchdog_close(-1) == -1, "close(-1) returns -1");
    chk(iv_watchdog_set_timeout(-1, 10) == -1, "set_timeout(-1, 10) returns -1");
    chk(iv_watchdog_set_timeout(p[1], -1) == -1, "set_timeout(fd, -1) returns -1");

    /* 3) 对非 watchdog 设备调 set_timeout 必须失败，且 errno 有意义：
     *    这条同时锁死"ioctl 失败绝不能被当成成功超时"的语义，
     *    也对应板端"若驱动不支持 SETTIMEOUT 则降级用默认值"的处置。 */
    errno = 0;
    chk(iv_watchdog_set_timeout(p[1], 30) == -1, "set_timeout on a pipe fails");
    chk(errno == ENOTTY || errno == EINVAL, "set_timeout leaves ENOTTY/EINVAL in errno");

    /* 4) close 真关 fd；重复 close / 关闭后喂狗都必须失败 */
    chk(iv_watchdog_close(p[1]) == 0, "close on valid fd returns 0");
    chk(iv_watchdog_close(p[1]) == -1, "double close returns -1");
    chk(iv_watchdog_keepalive(p[1]) == -1, "keepalive after close returns -1 (EBADF)");

    /* 5) open 不存在的节点必须失败且不崩（宿主与 VM 都没有 /dev/watchdog） */
    chk(iv_watchdog_open("/dev/ivsbox-no-such-watchdog") == -1,
        "open(nonexistent path) returns -1");

    close(p[0]);

    if (g_fail != 0) {
        fprintf(stderr, "test_watchdog failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_watchdog passed (write contract, param checks, close, no real device)\n");
    return 0;
}
