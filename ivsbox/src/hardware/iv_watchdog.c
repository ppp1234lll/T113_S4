/*
 * 看门狗设备薄包装（libivhal）。
 *
 * syscall 薄包装，不含任何判定逻辑；该不该喂狗由 src/modules/iv_health.c
 * 决定（见 iv_watchdog.h 的定位说明）。
 *
 * 关于 ioctl 宏为什么不直接 include <linux/watchdog.h>：
 *   - 宿主 MinGW 没有该头，include 了 host 单测就编不过；
 *   - 而 Linux 用户态 watchdog 的这两个 ioctl 码是**跨版本稳定**的 ABI
 *     （_IOWR(0x57,6,int) / _IOR(0x57,7,int)），就地取值不会因工具链不同而漂移。
 *   注意：**只在真正会用到的 ioctl 上这么做**。曾经这里还内联了 KEEPALIVE，却写成
 *   `_IO(0x57,5)` —— 内核真值是 `_IOR('W',5,int)` = 0x80045705，两者不等，而 ioctl
 *   按完整 cmd 值分发，那个宏永远命中不了（且全工程零调用，是死宏）。已删除：
 *   喂狗一律走 write()，不需要 KEEPALIVE。
 *   单测只传假 fd，永远走不到 ioctl 成功分支，所以这里不引入额外分支。
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ivsbox/iv_watchdog.h"

/* Linux 用户态 watchdog 契约（对应 <linux/watchdog.h> 的同名宏） */
#define IV_WDIOC_SETTIMEOUT _IOWR(0x57, 6, int) /* 内核 WDIOC_SETTIMEOUT */
#define IV_WDIOC_GETTIMEOUT _IOR(0x57, 7, int)  /* 内核 WDIOC_GETTIMEOUT */

/* 喂狗写入的字节。Linux watchdog core 的 write() 处理是"长度 > 0 即 ping"，
 * 任何字节都有效；这里固定用 '\0'，并刻意**避开** 'V' —— watchdog core 规定
 * "write 单个 'V' = 请求关闭设备"，用别的字节可以杜绝将来被误改。
 * （正常停机确实要停狗时，另有 iv_watchdog_disable() 显式写 'V'，那条是有意为之。）*/
static const char k_keepalive_byte = '\0';

/* 正常停机的 magic 字符：watchdog core 扫到它才把"允许本次 close 停狗"的标志置起来 */
static const char k_magic_close_byte = 'V';

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

    /* seconds <= 0 一律挡在入口：0 不是"查询"（查询走 GETTIMEOUT），
     * 且 0 低于任何驱动的 min_timeout，放下去只是白跑一次 ioctl。*/
    if (fd < 0 || seconds <= 0) {
        errno = EINVAL;
        return -1;
    }

    v = seconds;
    if (ioctl(fd, IV_WDIOC_SETTIMEOUT, &v) != 0)
        return -1; /* errno 原样透出：EINVAL（越界/驱动拒绝）/ ENOTTY（不支持）*/

    /* Linux 语义：内核把**实际接受**的秒数回写到 v。*/
    return v;
}

int iv_watchdog_get_timeout(int fd)
{
    int v = 0;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (ioctl(fd, IV_WDIOC_GETTIMEOUT, &v) != 0)
        return -1; /* errno 原样透出：ENOTTY = 设备/驱动不支持 */

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

int iv_watchdog_disable(int fd)
{
    ssize_t n;
    int     werr;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

    /* 先写 'V'：内核的 write 处理据此把"本次 close 允许停狗"的标志置起来；
     * 再 close：关掉 fd 本身也是 release 路径被触发的前提。
     * fd 无论成败都必须关掉 —— 停机失败还搭一个 fd 泄漏是最差的结果。*/
    n = write(fd, &k_magic_close_byte, 1u);
    /* n == -1 时 errno 一定有效；n == 0（1 字节写在实践中不会出现）时 errno
     * 不会被内核触碰，可能是进函数前的陈旧值 —— 兜成 EIO，绝不让"没写进 'V'"
     * 被误报成成功。*/
    werr = (n == 1) ? 0 : (errno != 0 ? errno : EIO);

    if (close(fd) != 0)
        return -1;

    if (werr != 0) {
        errno = werr;
        return -1;
    }
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
