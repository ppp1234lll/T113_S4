/*
 * 看门狗设备薄包装（libivhal）。
 *
 * 四个 syscall 包装，不含任何判定逻辑；该不该喂狗由 src/modules/iv_health.c
 * 决定（见 iv_watchdog.h 的定位说明）。
 *
 * 关于 ioctl 宏为什么不直接 include <linux/watchdog.h>：
 *   - 宿主 MinGW 没有该头，include 了 host 单测就编不过；
 *   - 而 Linux 用户态 watchdog 的这两个 ioctl 码是**跨版本稳定**的 ABI
 *     （_IO(0x57,5) / _IOWR(0x57,6,int)），就地取值不会因工具链不同而漂移。
 * 单测只传假 fd，永远走不到 ioctl 成功分支，所以这里不引入额外分支。
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ivsbox/iv_watchdog.h"

/* Linux 用户态 watchdog 契约（对应 <linux/watchdog.h> 的同名宏） */
#define IV_WDIOC_KEEPALIVE  _IO(0x57, 5)
#define IV_WDIOC_SETTIMEOUT _IOWR(0x57, 6, int)

/* 喂狗写入的字节。Linux watchdog core 的 write() 处理是"长度 > 0 即 ping"，
 * 任何字节都有效；这里固定用 '\0'，并刻意**避开** 'V' —— watchdog core 规定
 * "write 单个 'V' = 请求关闭设备"，用别的字节可以杜绝将来被误改。*/
static const char k_keepalive_byte = '\0';

int iv_watchdog_open(const char *path)
{
    int fd;

    if (path == NULL)
        path = IV_WATCHDOG_DEFAULT_PATH;

    /* O_RDWR：watchdog 设备普遍要求读写双向权限。
     * O_CLOEXEC：与 S4 iv_reactor 同一纪律，防止 fd 泄漏到 exec 后的子进程。*/
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    return fd;
}

int iv_watchdog_set_timeout(int fd, int seconds)
{
    int v;

    if (fd < 0 || seconds < 0) {
        errno = EINVAL;
        return -1;
    }

    v = seconds;
    if (ioctl(fd, IV_WDIOC_SETTIMEOUT, &v) != 0)
        return -1; /* errno 原样透出：EINVAL / ENOTTY 表示驱动不支持 */

    /* Linux 语义：内核把**实际接受**的秒数回写到 v。*/
    return v;
}

int iv_watchdog_keepalive(int fd)
{
    ssize_t n;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

    n = write(fd, &k_keepalive_byte, 1u);
    if (n != 1)
        return -1; /* 短写、EPIPE 等一律视为失败 */

    return 0;
}

int iv_watchdog_close(int fd)
{
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return close(fd);
}
