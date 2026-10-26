/*
 * IVSBox 4G AT 口探测（libivmodules）—— 实现
 *
 * 设计口径见 `include/ivsbox/iv_cellport.h`，此处只记实现上容易踩的三点：
 *
 *  1. **探测期间必须真等**：`iv_serial_open()` 给的是非阻塞 fd，写出去 `AT` 之后
 *     读通常立刻返回 `IV_EAGAIN`。用 `poll()` 等可读而不是 `usleep()` 轮询 ——
 *     前者在数据到达时**立刻**返回（正常情况 ~20 ms），后者要么睡太短空转、
 *     要么睡太长白等。扫描 6 个口时这个差别是"总共 1.2 s"与"总共 20 ms"。
 *
 *  2. **认 `OK` 要按行认，不能用 `strstr`**：diag 口吐的是二进制流，
 *     里面偶尔会出现 `'O','K'` 两个连续字节。按行切分后比较整行，
 *     误判概率降到可以忽略；真 AT 口的响应是 `\r\nOK\r\n`，切出来正好是 `OK`。
 *
 *  3. **`access()` 先行**：没插模组时 `/dev/ttyUSB*` 一个都不存在，
 *     此时扫描必须是零耗时。若对不存在的节点直接 `open()`，虽然也很快，
 *     但日志会被 6 条 `ENOENT` 刷满（每 5 s 一轮），把真正的问题淹掉。
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_cellport.h"
#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_serial.h"

#define CP_MOD "cellport"

/* 探测响应缓冲。`\r\nOK\r\n` 只有 6 字节，留 192 是为了在 diag 口上也能装下
 * 一段流再判（判不出来就滚动丢弃，见下）。 */
#define CP_BUF_MAX 192u

int iv_cellport_name(char *out, size_t cap, const char *prefix, unsigned idx)
{
    int n;

    if (out == NULL || cap == 0u)
        return IV_EINVAL;
    if (prefix == NULL)
        prefix = IV_CELLPORT_PREFIX;

    n = snprintf(out, cap, "%s%u", prefix, idx);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';   /* 不留半截路径：截断的路径可能打开成别的设备 */
        return IV_ERANGE;
    }
    return IV_OK;
}

/* 按行判断缓冲里有没有一行等于 `OK`（允许行尾空格）。
 * 不用 `strstr(buf, "OK")`：diag 口是二进制流，`O`,`K` 相邻的概率并不为零，
 * 而一次误判就会让我们把一个持续吐诊断数据的口当成 AT 口，之后永远查不到状态
 * —— 这正是 2026-10-23 那次故障的形态，代价是**整块 4G 信息消失**。 */
static int resp_has_ok(const char *buf, size_t len)
{
    size_t i = 0;

    while (i < len) {
        size_t s = i;
        size_t n;

        while (i < len && buf[i] != '\r' && buf[i] != '\n')
            i++;
        n = i - s;
        while (n > 0u && (buf[s + n - 1u] == ' ' || buf[s + n - 1u] == '\t'))
            n--;
        if (n == 2u && buf[s] == 'O' && buf[s + 1u] == 'K')
            return 1;
        i++;   /* 跳过行尾那个 \r 或 \n */
    }
    return 0;
}

int iv_cellport_probe(const char *dev, unsigned baud, unsigned wait_ms)
{
    iv_serial_cfg_t cfg;
    char            buf[CP_BUF_MAX];
    size_t          acc      = 0;
    uint64_t        deadline;
    int             fd;
    int             w;

    if (dev == NULL || dev[0] == '\0')
        return IV_EINVAL;
    if (baud == 0u)
        baud = IV_CELLPORT_BAUD_DEFAULT;
    if (wait_ms == 0u)
        wait_ms = IV_CELLPORT_WAIT_MS;

    iv_serial_cfg_default(&cfg);
    cfg.baud = baud;   /* 显式写：默认值将来若变，AT 口不能跟着变 */

    fd = iv_serial_open(dev, &cfg);
    if (fd < 0)
        return fd;   /* ENOENT / EBUSY / EAUTH：原样透出，由调用方决定跳过还是报错 */

    w = iv_serial_write(fd, "AT\r", 3u);
    if (w != 3) {
        (void)iv_serial_close(fd);
        return (w < 0) ? w : IV_EIO;
    }

    deadline = iv_clock_monotonic_ms() + (uint64_t)wait_ms;
    buf[0]   = '\0';
    for (;;) {
        struct pollfd p;
        uint64_t      now = iv_clock_monotonic_ms();
        size_t        got = 0;
        int           rc;
        int           pr;

        if (now >= deadline)
            break;
        p.fd      = fd;
        p.events  = POLLIN;
        p.revents = 0;
        pr = poll(&p, 1, (int)(deadline - now));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            break;   /* 等满了都没有数据 ⇒ 这个口不回话 */

        rc = iv_serial_read(fd, buf + acc, sizeof buf - 1u - acc, &got);
        if (rc == IV_EAGAIN)
            continue;
        if (rc != IV_OK || got == 0u)
            break;   /* 读 0 字节配上"永远可读"会变成忙等：直接判定这个口不行 */
        acc += got;
        buf[acc] = '\0';
        if (resp_has_ok(buf, acc)) {
            IV_LOG_I(CP_MOD, "%s: AT responded OK, port accepted", dev);
            return fd;   /* **保持打开**：调用方直接注册 Reactor */
        }
        if (acc >= sizeof buf - 1u) {
            /* diag 口会一直吐：整段吐满还没见到 OK，就丢掉重来。
             * 不丢的话下一轮 `acc` 再也涨不动，永远看不到新数据。 */
            acc    = 0;
            buf[0] = '\0';
        }
    }

    (void)iv_serial_close(fd);
    return IV_ETIMEDOUT;
}

int iv_cellport_scan(char *path_out, size_t cap, unsigned max_index, unsigned wait_ms)
{
    char     dev[IV_CELLPORT_PATH_MAX];
    unsigned i;

    if (path_out != NULL && cap > 0u)
        path_out[0] = '\0';
    if (max_index == 0u)
        max_index = IV_CELLPORT_SCAN_MAX;

    for (i = 0; i < max_index; i++) {
        int fd;

        if (iv_cellport_name(dev, sizeof dev, NULL, i) != IV_OK)
            break;
        /* 节点不存在：静默跳过。没插模组时这条路径必须零耗时且零日志。 */
        if (access(dev, F_OK) != 0)
            continue;

        fd = iv_cellport_probe(dev, IV_CELLPORT_BAUD_DEFAULT, wait_ms);
        if (fd < 0) {
            /* `IV_ETIMEDOUT` 是**扫描里的正常结论**（这个口不认 AT），
             * 用 DEBUG 留痕即可；其余错误（EBUSY/EAUTH/ENOENT）才值得 W。 */
            if (fd == IV_ETIMEDOUT)
                IV_LOG_D(CP_MOD, "%s: no AT response, skipped", dev);
            else
                IV_LOG_W(CP_MOD, "%s: probe failed (%d), skipped", dev, fd);
            continue;
        }
        if (path_out != NULL && cap > 0u) {
            size_t n = strlen(dev);

            if (n >= cap)
                n = cap - 1u;
            memcpy(path_out, dev, n);
            path_out[n] = '\0';
        }
        IV_LOG_I(CP_MOD, "AT port auto-detected: %s", dev);
        return fd;
    }
    return IV_ENOENT;
}
