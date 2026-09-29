/*
 * iv_watchdog 单测（libivhal）。
 *
 * **本单测绝不打开真实 /dev/watchdog** —— open() 会立刻激活看门狗，在板子上
 * 不喂就是整机复位，在宿主/VM 上则根本没有该设备。全部用 pipe 假 fd 覆盖
 * 参数校验、write 契约与停机通道；ioctl 那两条路（SETTIMEOUT / GETTIMEOUT 是否
 * 被 sunxi-wdt 接受）只能在板端单独实测，不在本单测范围内。
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

/* 读端是否已 EOF（＝写端已全部关闭） */
static int at_eof(int fd)
{
    char c;

    return read(fd, &c, 1) == 0;
}

int main(void)
{
    int  p[2];
    int  q[2];
    char c = 0;

    if (pipe(p) != 0 || pipe(q) != 0) {
        fprintf(stderr, "FAIL: pipe(): %s\n", strerror(errno));
        return 1;
    }

    /* 1) keepalive 就是"写 1 个字节"：读端必须恰好收到 1 字节，
     *    且**不是 'V'** —— watchdog core 规定写单个 'V' 等于请求关闭设备。 */
    chk(iv_watchdog_keepalive(p[1]) == 0, "keepalive on writable fd returns 0");
    chk(read(p[0], &c, 1) == 1, "keepalive wrote exactly 1 byte");
    chk(c == '\0', "keepalive byte is 0x00, never the 'V' close magic");

    /* 2) 参数校验：负 fd / 非正秒数一律拒绝。
     *    0 也必须挡在入口：内核 SETTIMEOUT 没有"0 = 查询"语义，0 低于驱动最小超时，
     *    放下去只会白跑一次 ioctl 拿 EINVAL（查超时请用 iv_watchdog_get_timeout）。 */
    chk(iv_watchdog_keepalive(-1) == -1, "keepalive(-1) returns -1");
    chk(iv_watchdog_close(-1) == -1, "close(-1) returns -1");
    chk(iv_watchdog_disable(-1) == -1, "disable(-1) returns -1");
    chk(iv_watchdog_get_timeout(-1) == -1, "get_timeout(-1) returns -1");
    chk(iv_watchdog_set_timeout(-1, 10) == -1, "set_timeout(-1, 10) returns -1");
    chk(iv_watchdog_set_timeout(p[1], -1) == -1, "set_timeout(fd, -1) returns -1");
    errno = 0;
    chk(iv_watchdog_set_timeout(p[1], 0) == -1, "set_timeout(fd, 0) returns -1");
    chk(errno == EINVAL, "set_timeout(fd, 0) rejected locally with EINVAL");

    /* 3) 对非 watchdog 设备调 ioctl 必须失败，且 errno 有意义：
     *    这条同时锁死"ioctl 失败绝不能被当成成功超时"的语义，
     *    也对应板端"若驱动不支持 SETTIMEOUT 则降级用默认值"的处置。 */
    errno = 0;
    chk(iv_watchdog_set_timeout(p[1], 30) == -1, "set_timeout on a pipe fails");
    chk(errno == ENOTTY || errno == EINVAL, "set_timeout leaves ENOTTY/EINVAL in errno");
    errno = 0;
    chk(iv_watchdog_get_timeout(p[1]) == -1, "get_timeout on a pipe fails");
    chk(errno == ENOTTY || errno == EINVAL, "get_timeout leaves ENOTTY/EINVAL in errno");

    /* 4) 正常停机通道：必须同时"写了 magic 'V'"和"关掉了 fd" ——
     *    少了 'V' 内核就不会停狗（sunxi-wdt 声明了 WDIOF_MAGICCLOSE），
     *    少了 close 则 release 路径不触发。两件事都验。 */
    chk(iv_watchdog_disable(q[1]) == 0, "disable on writable fd returns 0");
    chk(read(q[0], &c, 1) == 1, "disable wrote exactly 1 byte");
    chk(c == 'V', "disable writes the magic 'V' that lets the kernel stop the dog");
    chk(at_eof(q[0]) == 1, "disable also closed the fd");
    close(q[0]);

    /* 5) close 真关 fd；重复 close / 关闭后喂狗都必须失败 */
    chk(iv_watchdog_close(p[1]) == 0, "close on valid fd returns 0");
    chk(iv_watchdog_close(p[1]) == -1, "double close returns -1");
    chk(iv_watchdog_keepalive(p[1]) == -1, "keepalive after close returns -1 (EBADF)");

    /* 6) open 不存在的节点必须失败且不崩（宿主与 VM 都没有 /dev/watchdog） */
    chk(iv_watchdog_open("/dev/ivsbox-no-such-watchdog") == -1,
        "open(nonexistent path) returns -1");

    close(p[0]);

    if (g_fail != 0) {
        fprintf(stderr, "test_watchdog failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_watchdog passed (write contract, param checks, disable, close, no real device)\n");
    return 0;
}
