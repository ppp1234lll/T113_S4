/*
 * iv_serial 单测（libivhal，M2-S2.1）
 *
 * ============================ 为什么用 pty 而不是真实串口 ============================
 * 本模块要验的是"打开一个 tty、把它配成 raw、能收发、能独占"—— 这些行为在
 * **pty（伪终端）** 上完全一样：`/dev/pts/N` 是真正的 tty 设备，`isatty/tcgetattr/
 * tcsetattr/flock` 全部照常工作，而且**不需要任何硬件**。若改用真实串口：
 * 宿主机上常常没有可用串口；板端占用 `ttySAC3` 会打断 console（COM4）；
 * 测"写"还得有物理短路环。所以真实设备只做**可选**用例 —— 命令行传路径才跑
 * （板端核对"参数生效"用，对应计划 §S2.1 的验证方式）。
 *
 * pty 只用 POSIX 接口建立（`posix_openpt/grantpt/unlockpt/ptsname_r`，都在 libc 里），
 * **不引入 `-lutil`**，因此两端工具链的 `LDLIBS` 一个字都不用改。
 *
 * ============================ 覆盖范围 ============================
 *   1. 默认参数；
 *   2. 打开 pty slave，并**直接 tcgetattr 逐项断言** raw 与流控等位（不只信 get_cfg）；
 *   3. 参数化往返（115200/8N1 与 9600/7E2）；
 *   4. 非法参数与不支持的波特率；
 *   5. 非 tty 被拒（普通文件）；  6. 设备不存在；
 *   7. **单实例锁**：第二次打开报 `IV_EBUSY`，close 后可再开；
 *   8. 双向收发字节透明（含 `\r\n` 与高位字节，验证不做换行加工）；
 *   9. 无数据时 read 返回 `IV_EAGAIN`（不是 0、不是 EOF），且不动 `*got`；
 *  10. flush 清空待读数据；  11. 关闭后的 fd 与重复关闭；  12. 不泄漏 fd。
 *
 * 临时文件全部落在 `mkdtemp` 造的目录里，退出前清理干净（AGENTS.md 规则 6）。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_serial.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* /proc/self/fd 的条目数，用于"循环开关不泄漏 fd"的断言 */
static int count_open_fds(void)
{
    DIR           *d;
    struct dirent *e;
    int            n = 0;

    d = opendir("/proc/self/fd");
    if (d == NULL)
        return -1;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.')
            n++;
    }
    (void)closedir(d);
    return n;
}

/* 建一对 pty，返回 master fd（非阻塞）；slave_path 填 slave 的节点路径 */
static int pty_pair(char *slave_path, size_t cap)
{
    int m;

    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;

    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        (void)close(m);
        return -1;
    }

    if (ptsname_r(m, slave_path, cap) != 0) {
        (void)close(m);
        return -1;
    }

    /* master 也设非阻塞：测试里的"等数据"一律靠 poll，避免任何一处挂死 */
    if (fcntl(m, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(m);
        return -1;
    }
    return m;
}

/* 等 fd 可读，最多 ms 毫秒；返回 1 = 可读，0 = 超时 */
static int wait_readable(int fd, int ms)
{
    struct pollfd p;

    p.fd      = fd;
    p.events  = POLLIN;
    p.revents = 0;

    return (poll(&p, 1, ms) > 0) ? 1 : 0;
}

/* --------------------------------------------------------------------------- */

static void case_cfg_default(void)
{
    iv_serial_cfg_t c;

    iv_serial_cfg_default(&c);
    chk(c.baud == 115200u, "cfg_default: baud is 115200");
    chk(c.data_bits == 8u, "cfg_default: 8 data bits");
    chk(c.parity == IV_SERIAL_PARITY_NONE, "cfg_default: no parity");
    chk(c.stop_bits == 1u, "cfg_default: 1 stop bit");

    iv_serial_cfg_default(NULL); /* 允许传 NULL，且不得崩 */
    chk(1, "cfg_default(NULL) does not crash");
}

static void case_open_and_termios(void)
{
    char            slave[128];
    struct termios  t;
    iv_serial_cfg_t c;
    int             master;
    int             fd;

    master = pty_pair(slave, sizeof slave);
    chk(master >= 0, "pty_pair created a master");
    if (master < 0)
        return;

    fd = iv_serial_open(slave, NULL);
    chk(fd >= 0, "open the pty slave through iv_serial_open");
    if (fd >= 0) {
        chk(tcgetattr(fd, &t) == 0, "tcgetattr on the opened port");
        chk((t.c_cflag & CRTSCTS) == 0, "CRTSCTS cleared (no hardware flow control)");
        chk((t.c_cflag & CLOCAL) != 0, "CLOCAL set");
        chk((t.c_cflag & CREAD) != 0, "CREAD set");
        chk((t.c_cflag & CSIZE) == CS8, "8 data bits");
        chk((t.c_cflag & PARENB) == 0, "parity disabled");
        chk((t.c_cflag & CSTOPB) == 0, "1 stop bit");
        chk((t.c_lflag & (ICANON | ECHO)) == 0, "canonical mode and echo are off (raw)");
        chk((t.c_oflag & OPOST) == 0, "OPOST off");
        chk((t.c_iflag & (IXON | IXOFF)) == 0, "software flow control off");
        chk(t.c_cc[VMIN] == 0, "VMIN == 0");
        chk(t.c_cc[VTIME] == 0, "VTIME == 0");
        chk(cfgetospeed(&t) == B115200, "output speed is B115200");

        chk(iv_serial_get_cfg(fd, &c) == IV_OK, "get_cfg succeeds");
        chk(c.baud == 115200u && c.data_bits == 8u &&
                c.parity == IV_SERIAL_PARITY_NONE && c.stop_bits == 1u,
            "get_cfg reports 115200/8N1");

        chk(iv_serial_close(fd) == IV_OK, "close returns IV_OK");
    }
    (void)close(master);
}

/*
 * 9600 / 7E2 的参数往返。
 *
 * **pty 会把数据位与校验位规范化掉** —— 内核 `pty_set_termios()` 里那两行是
 * `c_cflag &= ~(CSIZE | PARENB); c_cflag |= CS8;`（源码注释自己写的就是
 * "This is a bit strong, but ..."）。这不是本模块的缺陷，是 pty 的既定行为。
 * 所以这里：
 *   - 只断言 pty 会**如实保留**的字段（波特率、停止位）；
 *   - 另外把"被规范化成 8N"这条事实**也断言出来**，让输出反映真实情况，
 *     而不是假装 7 位与校验位往返成功了；
 *   - 真正需要"7E2 落到硬件"的证据在 **case_real_device**（板端用
 *     `ttySAC2/4/5` 跑），对应计划 §S2.1 的"核对参数生效"。
 */
static void case_params_roundtrip(void)
{
    char            slave[128];
    iv_serial_cfg_t in;
    iv_serial_cfg_t out;
    int             master;
    int             fd;

    master = pty_pair(slave, sizeof slave);
    if (master < 0) {
        chk(0, "pty_pair for the parameter case");
        return;
    }

    iv_serial_cfg_default(&in);
    in.baud      = 9600u;
    in.data_bits = 7u;
    in.parity    = IV_SERIAL_PARITY_EVEN;
    in.stop_bits = 2u;

    fd = iv_serial_open(slave, &in);
    chk(fd >= 0, "open with 9600/7E2");
    if (fd >= 0) {
        memset(&out, 0, sizeof out);
        chk(iv_serial_get_cfg(fd, &out) == IV_OK, "get_cfg after 9600/7E2");
        chk(out.baud == 9600u, "9600 round-trips on a pty");
        chk(out.stop_bits == 2u, "2 stop bits round-trip on a pty");
        chk(out.data_bits == 8u, "pty normalises 7 data bits to 8 (kernel pty_set_termios)");
        chk(out.parity == IV_SERIAL_PARITY_NONE, "pty clears parity (kernel pty_set_termios)");
        (void)iv_serial_close(fd);
    }
    (void)close(master);
}

static void case_bad_params(void)
{
    char            slave[128];
    iv_serial_cfg_t c;
    int             master;
    int             fd;

    chk(iv_serial_open(NULL, NULL) == IV_EINVAL, "open(NULL) -> IV_EINVAL");
    chk(iv_serial_open("", NULL) == IV_EINVAL, "open(\"\") -> IV_EINVAL");
    chk(iv_serial_open("/dev/null", NULL) == IV_EINVAL,
        "a character device that is not a tty (/dev/null) -> IV_EINVAL");

    /* 波特率是显式白名单：不接受"就近取整"，在 open 之前就拦下 */
    iv_serial_cfg_default(&c);
    c.baud = 12345u;
    chk(iv_serial_open("/dev/null", &c) == IV_ENOTSUP, "non-standard baud 12345 -> IV_ENOTSUP");
    c.baud = 500000u;
    chk(iv_serial_open("/dev/null", &c) == IV_ENOTSUP, "unsupported baud 500000 -> IV_ENOTSUP");

    iv_serial_cfg_default(&c);
    c.data_bits = 9u;
    chk(iv_serial_open("/dev/null", &c) == IV_EINVAL, "9 data bits -> IV_EINVAL");
    iv_serial_cfg_default(&c);
    c.data_bits = 4u;
    chk(iv_serial_open("/dev/null", &c) == IV_EINVAL, "4 data bits -> IV_EINVAL");
    iv_serial_cfg_default(&c);
    c.stop_bits = 3u;
    chk(iv_serial_open("/dev/null", &c) == IV_EINVAL, "3 stop bits -> IV_EINVAL");
    iv_serial_cfg_default(&c);
    c.parity = (iv_serial_parity_t)9;
    chk(iv_serial_open("/dev/null", &c) == IV_EINVAL, "a bogus parity value -> IV_EINVAL");

    chk(iv_serial_read(-1, slave, 1u, NULL) == IV_EINVAL, "read(fd < 0) -> IV_EINVAL");
    chk(iv_serial_write(-1, slave, 1u) == IV_EINVAL, "write(fd < 0) -> IV_EINVAL");
    chk(iv_serial_flush(-1) == IV_EINVAL, "flush(fd < 0) -> IV_EINVAL");
    chk(iv_serial_get_cfg(-1, NULL) == IV_EINVAL, "get_cfg(fd < 0, NULL) -> IV_EINVAL");
    chk(iv_serial_close(-1) == IV_EINVAL, "close(fd < 0) -> IV_EINVAL");

    /* 缓冲区/长度边界要在**真实打开的 fd** 上测，避免被 fd 检查提前拦掉 */
    master = pty_pair(slave, sizeof slave);
    if (master < 0) {
        chk(0, "pty_pair for the buffer-boundary case");
        return;
    }
    fd = iv_serial_open(slave, NULL);
    if (fd >= 0) {
        chk(iv_serial_read(fd, NULL, 8u, NULL) == IV_EINVAL, "read(buf = NULL) -> IV_EINVAL");
        chk(iv_serial_read(fd, slave, 0u, NULL) == IV_EINVAL, "read(cap = 0) -> IV_EINVAL");
        chk(iv_serial_write(fd, slave, 0u) == IV_EINVAL, "write(len = 0) -> IV_EINVAL");
        chk(iv_serial_write(fd, NULL, 4u) == IV_EINVAL, "write(buf = NULL) -> IV_EINVAL");
        chk(iv_serial_get_cfg(fd, NULL) == IV_EINVAL, "get_cfg(out = NULL) -> IV_EINVAL");
        (void)iv_serial_close(fd);
    } else {
        chk(0, "open the slave for the buffer-boundary case");
    }
    (void)close(master);
}

static void case_not_a_tty(void)
{
    char dir[] = "/tmp/ivser_XXXXXX";
    char path[192];
    int  fd;

    if (mkdtemp(dir) == NULL) {
        chk(0, "mkdtemp for the not-a-tty case");
        return;
    }

    (void)snprintf(path, sizeof path, "%s/regular", dir);
    fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0) {
        (void)close(fd);
        chk(iv_serial_open(path, NULL) == IV_EINVAL, "a regular file is rejected (not a tty)");
    } else {
        chk(0, "create the regular file");
    }

    (void)unlink(path);
    (void)rmdir(dir);
}

static void case_no_such_device(void)
{
    chk(iv_serial_open("/dev/ivsbox-no-such-tty", NULL) == IV_ENOENT,
        "a missing device node -> IV_ENOENT");
}

static void case_exclusive_lock(void)
{
    char slave[128];
    int  master;
    int  a;
    int  b;
    int  c;

    master = pty_pair(slave, sizeof slave);
    if (master < 0) {
        chk(0, "pty_pair for the lock case");
        return;
    }

    a = iv_serial_open(slave, NULL);
    chk(a >= 0, "the first open takes the port");
    if (a >= 0) {
        b = iv_serial_open(slave, NULL);
        chk(b == IV_EBUSY, "a second open on the same port -> IV_EBUSY");

        chk(iv_serial_close(a) == IV_OK, "close the first holder");
        c = iv_serial_open(slave, NULL);
        chk(c >= 0, "after close the port can be opened again");
        if (c >= 0)
            (void)iv_serial_close(c);
    }
    (void)close(master);
}

static void case_io_roundtrip(void)
{
    static const char k_tx[] = "AT+ABC\r\n\x01\x02\x7f\xff"; /* 含 CR LF 与高位字节 */
    char              slave[128];
    char              buf[64];
    ssize_t           n;
    size_t            got   = 0u;
    size_t            total = 0u;
    size_t            txlen = sizeof k_tx - 1u;
    int               master;
    int               fd;

    master = pty_pair(slave, sizeof slave);
    if (master < 0) {
        chk(0, "pty_pair for the io case");
        return;
    }
    fd = iv_serial_open(slave, NULL);
    if (fd < 0) {
        chk(0, "open the slave for the io case");
        (void)close(master);
        return;
    }

    /* master -> slave：由模块读出来 */
    n = write(master, k_tx, txlen);
    chk(n == (ssize_t)txlen, "write to the master side");
    chk(wait_readable(fd, 500) == 1, "the slave became readable");

    while (total < txlen) {
        int rc = iv_serial_read(fd, buf + total, sizeof buf - total, &got);
        if (rc == IV_EAGAIN)
            break;
        chk(rc == IV_OK, "read from the slave");
        if (rc != IV_OK)
            break;
        total += got;
    }
    chk(total == txlen, "all bytes arrived through the raw port");
    if (total == txlen)
        chk(memcmp(buf, k_tx, txlen) == 0, "byte-for-byte identical (nothing was translated)");

    /* slave -> master：由模块写出去 */
    {
        static const char k_msg[] = "hello-mcu";
        size_t            len     = sizeof k_msg - 1u;
        int               w       = iv_serial_write(fd, k_msg, len);

        chk(w == (int)len, "iv_serial_write reports the full length");
        chk(wait_readable(master, 500) == 1, "the master side became readable");
        n = read(master, buf, sizeof buf);
        chk(n == (ssize_t)len, "the master received the same byte count");
        if (n == (ssize_t)len)
            chk(memcmp(buf, k_msg, len) == 0, "the master received the same bytes");
    }

    (void)iv_serial_close(fd);
    (void)close(master);
}

static void case_no_data_eagain(void)
{
    char   slave[128];
    char   buf[8];
    size_t got = 123u; /* 先写脏：验证失败路径不会误写 *got */
    int    master;
    int    fd;

    master = pty_pair(slave, sizeof slave);
    if (master < 0) {
        chk(0, "pty_pair for the EAGAIN case");
        return;
    }

    fd = iv_serial_open(slave, NULL);
    if (fd >= 0) {
        chk(iv_serial_read(fd, buf, sizeof buf, &got) == IV_EAGAIN,
            "no data -> IV_EAGAIN (not 0, not EOF)");
        chk(got == 123u, "*got is left untouched on IV_EAGAIN");
        (void)iv_serial_close(fd);
    } else {
        chk(0, "open the slave for the EAGAIN case");
    }
    (void)close(master);
}

static void case_flush(void)
{
    char   slave[128];
    char   buf[8];
    size_t got = 0u;
    int    master;
    int    fd;

    master = pty_pair(slave, sizeof slave);
    if (master < 0) {
        chk(0, "pty_pair for the flush case");
        return;
    }

    fd = iv_serial_open(slave, NULL);
    if (fd >= 0) {
        chk(write(master, "XXXX", 4u) == 4, "write 4 bytes before flush");
        chk(wait_readable(fd, 500) == 1, "readable before flush");
        chk(iv_serial_flush(fd) == IV_OK, "flush returns IV_OK");
        chk(iv_serial_read(fd, buf, sizeof buf, &got) == IV_EAGAIN,
            "flush discarded the pending input");
        (void)iv_serial_close(fd);
    } else {
        chk(0, "open the slave for the flush case");
    }
    (void)close(master);
}

static void case_closed_fd(void)
{
    char slave[128];
    char buf[4];
    int  master;
    int  fd;

    master = pty_pair(slave, sizeof slave);
    if (master < 0) {
        chk(0, "pty_pair for the closed-fd case");
        return;
    }

    fd = iv_serial_open(slave, NULL);
    if (fd >= 0) {
        chk(iv_serial_close(fd) == IV_OK, "close returns IV_OK");
        chk(iv_serial_read(fd, buf, sizeof buf, NULL) == IV_EIO,
            "reading a closed fd -> IV_EIO (EBADF)");
        chk(iv_serial_close(fd) == IV_EIO,
            "closing twice -> IV_EIO (deliberately not tolerated)");
    } else {
        chk(0, "open the slave for the closed-fd case");
    }
    (void)close(master);
}

static void case_fd_leak(void)
{
    char slave[128];
    int  master;
    int  before;
    int  after;
    int  i;
    int  bad = 0;

    master = pty_pair(slave, sizeof slave);
    if (master < 0) {
        chk(0, "pty_pair for the fd-leak case");
        return;
    }

    before = count_open_fds();
    for (i = 0; i < 30; i++) {
        int fd = iv_serial_open(slave, NULL);
        if (fd < 0) {
            bad++;
            break;
        }
        (void)iv_serial_close(fd);
    }
    after = count_open_fds();

    chk(bad == 0, "30 open/close cycles all succeeded (the lock is released each time)");
    if (before >= 0 && after >= 0)
        chk(before == after, "open/close cycles do not leak file descriptors");

    (void)close(master);
}

/* 可选：真实设备（板端核对参数生效用；命令行传路径才跑）。
 * 这里是**唯一**能证明"7 位数据 ＋ 偶校验真的落到硬件"的地方 —— pty 会把它们规范化掉。
 * 探测完必须把参数还原成默认，别把测试参数留在硬件上。*/
static void case_real_device(const char *path)
{
    iv_serial_cfg_t c;
    iv_serial_cfg_t out;
    int             fd;

    fd = iv_serial_open(path, NULL);
    chk(fd >= 0, "open the real device given on the command line");
    if (fd < 0)
        return;

    memset(&out, 0, sizeof out);
    chk(iv_serial_get_cfg(fd, &out) == IV_OK, "get_cfg on the real device");
    chk(out.baud == IV_SERIAL_BAUD_DEFAULT, "the real device is at the default baud");
    chk(out.data_bits == 8u && out.parity == IV_SERIAL_PARITY_NONE && out.stop_bits == 1u,
        "the real device is 8N1");
    chk(iv_serial_flush(fd) == IV_OK, "flush on the real device");
    chk(iv_serial_close(fd) == IV_OK, "close the real device");

    /* 参数生效的正面证据：真实 UART 会如实保留 9600/7E2 */
    iv_serial_cfg_default(&c);
    c.baud      = 9600u;
    c.data_bits = 7u;
    c.parity    = IV_SERIAL_PARITY_EVEN;
    c.stop_bits = 2u;

    fd = iv_serial_open(path, &c);
    chk(fd >= 0, "open the real device with 9600/7E2");
    if (fd < 0)
        return;

    memset(&out, 0, sizeof out);
    chk(iv_serial_get_cfg(fd, &out) == IV_OK, "get_cfg after 9600/7E2");
    chk(out.baud == 9600u, "real device: 9600 round-trips");
    chk(out.data_bits == 7u, "real device: 7 data bits round-trip");
    chk(out.parity == IV_SERIAL_PARITY_EVEN, "real device: even parity round-trips");
    chk(out.stop_bits == 2u, "real device: 2 stop bits round-trip");
    chk(iv_serial_close(fd) == IV_OK, "close after the 7E2 probe");

    /* 还原默认，别把测试参数留在硬件上 */
    fd = iv_serial_open(path, NULL);
    chk(fd >= 0, "re-open the real device to restore the defaults");
    if (fd >= 0)
        chk(iv_serial_close(fd) == IV_OK, "restore the real device to defaults");
}

int main(int argc, char *argv[])
{
    case_cfg_default();
    case_open_and_termios();
    case_params_roundtrip();
    case_bad_params();
    case_not_a_tty();
    case_no_such_device();
    case_exclusive_lock();
    case_io_roundtrip();
    case_no_data_eagain();
    case_flush();
    case_closed_fd();
    case_fd_leak();

    if (argc > 1)
        case_real_device(argv[1]);

    if (g_fail != 0) {
        fprintf(stderr, "test_serial FAILED (%d failed checks)\n", g_fail);
        return 1;
    }

    printf("test_serial passed (cfg, raw termios, params, errno paths, exclusive lock, "
           "io, eagain, flush, closed fd, fd leak)\n");
    return 0;
}
