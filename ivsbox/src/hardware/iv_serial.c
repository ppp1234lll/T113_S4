/*
 * 串口（libivhal，功能开发计划 M2-S2.1）
 *
 * 职责与全部设计口径见 `include/ivsbox/iv_serial.h` 的文件头；这里只补三条
 * 实现层面的说明：
 *
 * 1) **不用 `iv_log`**。本层是 syscall 薄包装（同 `iv_watchdog`），错误一律靠
 *    返回码 + `errno` 表达，不在这里写日志 —— 一来 hal 层不该决定"什么值得记"，
 *    二来单测里日志系统未初始化也能安全调用本模块。
 *
 * 2) **波特率表是显式白名单**，不做"就近取整"。串口参数错配的现象是乱码，
 *    在链路层看起来像"帧解析失败"，排查代价极高，所以在入口就拦掉。
 *
 * 3) **`cfmakeraw` 与 `c_cflag` 的顺序不能调换**：`cfmakeraw` 会把 `CSIZE`
 *    一并清掉（等于 8 位？不是 —— 是 `CSIZE` 掩码位全 0，行为取决于驱动），
 *    所以它必须在设置数据位/校验/停止位**之前**调用。
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>

#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_serial.h"

/* ---------------------------------------------------------------------------
 * 标准波特率白名单
 * ------------------------------------------------------------------------- */
typedef struct {
    unsigned baud;
    speed_t  speed;
} baud_map_t;

/* 只列标准值。230400 以上的常量在部分工具链的 <termios.h> 里不一定存在，
 * 用 #ifdef 兜住 —— 有则支持，无则老老实实报 IV_ENOTSUP（不猜）。*/
static const baud_map_t k_bauds[] = {
    { 9600u,   B9600   },
    { 19200u,  B19200  },
    { 38400u,  B38400  },
    { 57600u,  B57600  },
    { 115200u, B115200 },
#ifdef B230400
    { 230400u, B230400 },
#endif
#ifdef B460800
    { 460800u, B460800 },
#endif
#ifdef B921600
    { 921600u, B921600 },
#endif
};

static int baud_lookup(unsigned baud, speed_t *out)
{
    size_t i;

    for (i = 0u; i < sizeof(k_bauds) / sizeof(k_bauds[0]); i++) {
        if (k_bauds[i].baud == baud) {
            *out = k_bauds[i].speed;
            return IV_OK;
        }
    }
    return IV_ENOTSUP;
}

/* 反查：未登记的速率返回 0（**不编造一个"看起来接近"的值**）*/
static unsigned speed_lookup(speed_t sp)
{
    size_t i;

    for (i = 0u; i < sizeof(k_bauds) / sizeof(k_bauds[0]); i++) {
        if (k_bauds[i].speed == sp)
            return k_bauds[i].baud;
    }
    return 0u;
}

/* ---------------------------------------------------------------------------
 * errno → IV_* 映射
 *
 * 注意：Linux 上 `EWOULDBLOCK == EAGAIN`、`EOPNOTSUPP == ENOTSUP`，
 * 同一 case 里并列写会直接编译失败（duplicate case value）—— 故每类只用其一。
 * `flock` 失败（EWOULDBLOCK）**不走这里**：它要报 IV_EBUSY，而 EAGAIN 要报
 * IV_EAGAIN，同一个 errno 值对应两种语义，只能在各调用点分别判。
 * ------------------------------------------------------------------------- */
static int open_errno_to_iv(int e)
{
    switch (e) {
    case ENOENT:
    case ENODEV:
        return IV_ENOENT;
    case EACCES:
    case EPERM:
        return IV_EAUTH;
    default:
        return IV_EIO;
    }
}

static int io_errno_to_iv(int e)
{
    switch (e) {
    case EAGAIN:
        return IV_EAGAIN;
    case ENXIO:
    case ENODEV:
        return IV_ECONN; /* 设备消失：USB 转串口被拔 */
    case EINVAL:
        return IV_EINVAL;
    default:
        return IV_EIO;
    }
}

/* ---------------------------------------------------------------------------
 * 参数
 * ------------------------------------------------------------------------- */
void iv_serial_cfg_default(iv_serial_cfg_t *cfg)
{
    if (cfg == NULL)
        return;

    cfg->baud      = IV_SERIAL_BAUD_DEFAULT;
    cfg->data_bits = 8u;
    cfg->parity    = IV_SERIAL_PARITY_NONE;
    cfg->stop_bits = 1u;
}

static int cfg_check(const iv_serial_cfg_t *c)
{
    if (c->data_bits < 5u || c->data_bits > 8u)
        return IV_EINVAL;
    if (c->stop_bits != 1u && c->stop_bits != 2u)
        return IV_EINVAL;
    if (c->parity != IV_SERIAL_PARITY_NONE && c->parity != IV_SERIAL_PARITY_EVEN &&
        c->parity != IV_SERIAL_PARITY_ODD)
        return IV_EINVAL;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 打开 / 关闭
 * ------------------------------------------------------------------------- */
int iv_serial_open(const char *path, const iv_serial_cfg_t *cfg)
{
    iv_serial_cfg_t c;
    struct termios  t;
    speed_t         sp;
    tcflag_t        csz;
    int             fd;
    int             rc;
    int             e;

    /* 刻意不给默认设备路径：用哪一路串口是部署决定，写死一个默认值
     * 只会让"配置漏了"表现为"打开了错误的串口"。*/
    if (path == NULL || path[0] == '\0')
        return IV_EINVAL;

    iv_serial_cfg_default(&c);
    if (cfg != NULL)
        c = *cfg;

    rc = cfg_check(&c);
    if (rc != IV_OK)
        return rc;

    rc = baud_lookup(c.baud, &sp);
    if (rc != IV_OK)
        return rc; /* IV_ENOTSUP */

    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return open_errno_to_iv(errno);

    /* 单实例锁：锁在 tty fd 本身（理由见头文件）。拿不到即 IV_EBUSY —— 包括
     * 同进程内第二次打开同一串口（两次 open 是两个打开文件描述，照样冲突）。*/
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        e = errno;
        (void)close(fd);
        errno = e;
        return IV_EBUSY;
    }

    /* 必须是 tty。否则会"成功"打开一个普通文件，之后每个 termios 调用都失败，
     * 故障点被推到很远的地方（而那正是最难查的一类）。*/
    if (!isatty(fd)) {
        (void)close(fd);
        errno = ENOTTY;
        return IV_EINVAL;
    }

    if (tcgetattr(fd, &t) != 0) {
        e = errno;
        (void)close(fd);
        errno = e;
        return io_errno_to_iv(e);
    }

    /* 1) 先 raw：清 ICANON/ECHO/ISIG/IEXTEN/OPOST 等一切"加工字节"的开关 */
    cfmakeraw(&t);

    /* 2) 再 c_cflag：cfmakeraw **不清** CRTSCTS，也不保证数据位（它清 CSIZE） */
    t.c_cflag &= ~CRTSCTS;                /* 计划 §S2.1 的硬要求：显式关硬件流控 */
    t.c_cflag |= (CLOCAL | CREAD);        /* 忽略 modem 控制线；允许接收 */

    t.c_cflag &= ~CSIZE;
    csz = CS8;
    if (c.data_bits == 5u)
        csz = CS5;
    else if (c.data_bits == 6u)
        csz = CS6;
    else if (c.data_bits == 7u)
        csz = CS7;
    t.c_cflag |= csz;

    t.c_cflag &= ~(PARENB | PARODD);
    if (c.parity == IV_SERIAL_PARITY_EVEN)
        t.c_cflag |= PARENB;
    else if (c.parity == IV_SERIAL_PARITY_ODD)
        t.c_cflag |= (PARENB | PARODD);

    if (c.stop_bits == 2u)
        t.c_cflag |= CSTOPB;
    else
        t.c_cflag &= ~CSTOPB;

    /* VMIN=0 / VTIME=0：与 O_NONBLOCK 配套，无数据时 read() 立即返回 0 */
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;

    /* 输入侧再关一遍软件流控与换行加工（cfmakeraw 已关大部分，这里把
     * 与"字节级透明传输"直接相关的几项写全，防止将来有人加了 IXON 而没注意）*/
    t.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR | ISTRIP | INPCK);
    t.c_oflag &= ~OPOST;

    if (cfsetispeed(&t, sp) != 0 || cfsetospeed(&t, sp) != 0) {
        (void)close(fd);
        errno = ENOTSUP;
        return IV_ENOTSUP;
    }

    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        e = errno;
        (void)close(fd);
        errno = e;
        return io_errno_to_iv(e);
    }

    (void)tcflush(fd, TCIOFLUSH);
    return fd;
}

int iv_serial_close(int fd)
{
    if (fd < 0)
        return IV_EINVAL;

    /* 锁随 close 由内核自动释放（flock 是打开文件描述上的锁）——
     * 这正是"锁在 fd 上"比"另开锁文件"少一个泄漏点的原因。*/
    if (close(fd) != 0)
        return IV_EIO; /* EBADF（重复关闭）也走这里，errno 原样透出 */

    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 收发
 * ------------------------------------------------------------------------- */
int iv_serial_read(int fd, void *buf, size_t cap, size_t *got)
{
    ssize_t n;

    if (fd < 0 || buf == NULL || cap == 0u)
        return IV_EINVAL;

    for (;;) {
        n = read(fd, buf, cap);
        if (n < 0 && errno == EINTR)
            continue; /* 被信号打断不算错误，同一调用内重试 */
        break;
    }

    if (n < 0)
        return io_errno_to_iv(errno);

    if (n == 0) {
        /* VMIN=0 下 read()==0 的语义是"当前没有数据"，**不是**"对端关闭"。
         * 串口没有"连接"概念，把它当 EOF 会让上层误判断链。*/
        return IV_EAGAIN;
    }

    if (got != NULL)
        *got = (size_t)n;
    return IV_OK;
}

int iv_serial_write(int fd, const void *buf, size_t len)
{
    ssize_t n;

    if (fd < 0 || buf == NULL || len == 0u)
        return IV_EINVAL;
    if (len > (size_t)INT_MAX)
        return IV_ERANGE; /* 返回 int，超了就不能保证"实际写入字节数"不失真 */

    for (;;) {
        n = write(fd, buf, len);
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }

    if (n < 0) {
        /* 发送缓冲满：本次一字节都没写进去，调用方可稍后重试 */
        if (errno == EAGAIN)
            return IV_EAGAIN;
        return io_errno_to_iv(errno);
    }

    return (int)n; /* 字节流语义：可能小于 len，调用方负责续写 */
}

int iv_serial_flush(int fd)
{
    if (fd < 0)
        return IV_EINVAL;

    if (tcflush(fd, TCIOFLUSH) != 0)
        return io_errno_to_iv(errno);

    return IV_OK;
}

int iv_serial_get_cfg(int fd, iv_serial_cfg_t *out)
{
    struct termios t;

    if (fd < 0 || out == NULL)
        return IV_EINVAL;

    if (tcgetattr(fd, &t) != 0)
        return io_errno_to_iv(errno);

    out->baud = speed_lookup(cfgetospeed(&t)); /* 未登记则 0，不猜 */

    if ((t.c_cflag & CSIZE) == CS5)
        out->data_bits = 5u;
    else if ((t.c_cflag & CSIZE) == CS6)
        out->data_bits = 6u;
    else if ((t.c_cflag & CSIZE) == CS7)
        out->data_bits = 7u;
    else
        out->data_bits = 8u;

    if ((t.c_cflag & PARENB) == 0)
        out->parity = IV_SERIAL_PARITY_NONE;
    else if ((t.c_cflag & PARODD) != 0)
        out->parity = IV_SERIAL_PARITY_ODD;
    else
        out->parity = IV_SERIAL_PARITY_EVEN;

    out->stop_bits = ((t.c_cflag & CSTOPB) != 0) ? 2u : 1u;
    return IV_OK;
}

const char *iv_serial_baud_name(unsigned baud)
{
    switch (baud) {
    case 9600u:
        return "9600";
    case 19200u:
        return "19200";
    case 38400u:
        return "38400";
    case 57600u:
        return "57600";
    case 115200u:
        return "115200";
#ifdef B230400
    case 230400u:
        return "230400";
#endif
#ifdef B460800
    case 460800u:
        return "460800";
#endif
#ifdef B921600
    case 921600u:
        return "921600";
#endif
    default:
        return "unsupported";
    }
}
