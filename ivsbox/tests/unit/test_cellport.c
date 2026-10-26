/*
 * iv_cellport 单测（libivmodules）—— AT 口探测
 *
 * ============================ 为什么用 pty ============================
 * 要验的是"**对端回了 `OK`** 才算 AT 口"这条判据，而不是"open 成功"。这条判据
 * 只有让对端真的回话才测得到，所以用 pty：`/dev/pts/N` 是真正的 tty，
 * `isatty/tcgetattr/tcsetattr/flock` 全部照常工作，且**我们要扮演的那个"会回话的
 * 模组"就是 master 端**——往 master 写 `OK`，slave 侧就读得到。
 *
 * 这组用例覆盖的正是 2026-10-23 那次故障的两种形态：
 *   - `cp03`：口开着、能读、但一个字节都不回 → 必须超时，不能当成可用；
 *   - `cp04`：口在吐数据（diag 口就是这样），但吐的不是 `OK` → 同样必须判失败。
 * 早先的实现只判 `open() >= 0`，于是 diag 口被当成 AT 口用了 15 h。
 *
 * ============================ 覆盖范围 ============================
 *   1. 候选路径生成（正常 / 换前缀 / 目标太小 / NULL）；
 *   2. 回 `OK` → 返回**保持打开**的 fd，且 `AT\r` 真的发到了对端；
 *   3. 不回话 → `IV_ETIMEDOUT`（不是"打开失败"，也不是"成功"）；
 *   4. 回 `ERROR` → `IV_ETIMEDOUT`（有数据 ≠ 是 AT 口）；
 *   5. 节点不存在 → `IV_ENOENT`（与 3 区分开，调用方据此决定"跳过"还是"报错"）；
 *   6. 不是 tty → `IV_EINVAL`；
 *   7. `scan` 在没有 `/dev/ttyUSB*` 的机器上返回 `IV_ENOENT` 且不留半截路径。
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "ivsbox/iv_cellport.h"
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
    if (fcntl(m, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(m);
        return -1;
    }
    return m;
}

/* 往 master 写一段（扮演"模组的应答"）。写不进去就是测试环境问题，直接判失败。 */
static void mock_reply(int master, const char *s)
{
    ssize_t n = write(master, s, strlen(s));

    if (n != (ssize_t)strlen(s))
        chk(0, "mock reply written to the pty master");
}

/* 从 master 读一段，最多等 ms 毫秒。返回读到的字节数（0 = 没读到）。 */
static size_t mock_read(int master, char *out, size_t cap, int ms)
{
    struct pollfd p;
    ssize_t       n;

    out[0]    = '\0';
    p.fd      = master;
    p.events  = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, ms) <= 0)
        return 0;
    n = read(master, out, cap - 1u);
    if (n <= 0)
        return 0;
    out[n] = '\0';
    return (size_t)n;
}

/* ---- 「会回话的模组」：收到 AT 再回，不预先塞队列 ---------------------------
 *
 * 早先这几个用例是**先 `mock_reply()` 再 `probe()`**，注释还写着"顺序无关"。
 * 这条假设是错的：`iv_serial_open()` 在 `tcsetattr()` 之后会
 * `tcflush(fd, TCIOFLUSH)` 清空输入输出队列（见 `iv_serial.c`），预置的
 * `OK` 在 open 的瞬间就被冲掉了。后果有两种，都很难看：
 *
 *   - `cp01`（期望成功）：直接红；
 *   - `cp05`/`cp06`（期望超时）：因为数据被冲掉，超时依然成立，于是**假绿**
 *     —— 明明要验"有数据但不是 OK 不能算 AT 口"，实际验的是"哑口超时"，
 *     和 `cp04` 重复。
 *
 * 所以改成真机的时序：探测方写完 `AT`，模组才回。`got_at` 作为出参，
 * 让用例能断言"`AT` 真的发出去了"，假绿就藏不住了。 */
struct mock_mod {
    int         master;
    const char *reply;   /* NULL = 收到 AT 后什么都不回（哑口） */
    int         got_at;  /* 出参：1 = 确实收到了 AT */
};

static void *mock_mod_thread(void *arg)
{
    struct mock_mod *m = arg;
    char             buf[64];
    size_t           n;

    n = mock_read(m->master, buf, sizeof buf, 1500);
    if (n > 0 && strstr(buf, "AT") != NULL)
        m->got_at = 1;
    if (m->reply != NULL)
        mock_reply(m->master, m->reply);
    return NULL;
}

/* 起一个模组线程；失败时已填好 FAIL 并返回 -1（调用方直接收尾） */
static int mod_start(pthread_t *th, struct mock_mod *mm, int master, const char *reply)
{
    memset(mm, 0, sizeof *mm);
    mm->master = master;
    mm->reply  = reply;
    if (pthread_create(th, NULL, mock_mod_thread, mm) != 0) {
        chk(0, "mock mod thread started");
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------- */

static void case_name(void)
{
    char buf[IV_CELLPORT_PATH_MAX];

    chk(iv_cellport_name(buf, sizeof buf, NULL, 0) == IV_OK, "n01 name built");
    chk(strcmp(buf, "/dev/ttyUSB0") == 0, "n01 index 0 -> /dev/ttyUSB0");

    chk(iv_cellport_name(buf, sizeof buf, NULL, 5) == IV_OK, "n02 index 5 built");
    chk(strcmp(buf, "/dev/ttyUSB5") == 0, "n02 index 5 -> /dev/ttyUSB5");

    chk(iv_cellport_name(buf, sizeof buf, "/dev/pts/", 7) == IV_OK, "n03 prefix override");
    chk(strcmp(buf, "/dev/pts/7") == 0, "n03 prefix is honoured");

    /* 放不下时**不许留半截**：截断的路径会 open() 成别的设备，
     * 那是最坏的一种错 —— 看起来成功了，接的却是别的东西。 */
    memset(buf, 'X', sizeof buf);
    chk(iv_cellport_name(buf, 8, NULL, 12) == IV_ERANGE, "n04 too small -> IV_ERANGE");
    chk(buf[0] == '\0', "n04 buffer cleared on truncation");

    chk(iv_cellport_name(NULL, 16, NULL, 0) == IV_EINVAL, "n05 NULL out -> IV_EINVAL");
    chk(iv_cellport_name(buf, 0, NULL, 0) == IV_EINVAL, "n06 cap 0 -> IV_EINVAL");
}

/* 核心用例：对端回 OK ⇒ 探测成功，fd 保持打开，且命令真的发出去了。 */
static void case_probe_ok(void)
{
    char            slave[128];
    int             master;
    int             fd;
    int             rc;
    pthread_t       th;
    struct mock_mod mm;

    master = pty_pair(slave, sizeof slave);
    chk(master >= 0, "p01 pty pair created");
    if (master < 0)
        return;

    if (mod_start(&th, &mm, master, "\r\nOK\r\n") != 0) {
        (void)close(master);
        return;
    }

    fd = iv_cellport_probe(slave, 0u, 400u);
    chk(fd > 0, "p01 probe accepts a port that answers OK");
    (void)pthread_join(th, NULL);
    /* 证明 `AT\r` 真的写出去了（不是"没发命令就宣布成功"） */
    chk(mm.got_at == 1, "p02 the probe really sent AT first");
    if (fd <= 0) {
        (void)close(master);
        return;
    }

    /* fd 必须是**可用且保持打开**的（调用方要直接注册 Reactor） */
    rc = iv_serial_write(fd, "AT\r", 3u);
    chk(rc == 3, "p03 returned fd is usable for writing");
    chk(iv_serial_close(fd) == IV_OK, "p03 close the returned fd");
    (void)close(master);
}

/* 口开着、能读，但一个字节都不回 ⇒ 必须超时。这是"接了个哑口"的形态。 */
static void case_probe_silent(void)
{
    char slave[128];
    int  master;
    int  fd;

    master = pty_pair(slave, sizeof slave);
    chk(master >= 0, "p04 pty pair created");
    if (master < 0)
        return;

    fd = iv_cellport_probe(slave, 0u, 60u);
    chk(fd == IV_ETIMEDOUT, "p04 silent port -> IV_ETIMEDOUT (not success)");
    (void)close(master);
}

/* 口在吐数据，但吐的不是 OK ⇒ 同样失败。这是 diag 口的形态：
 * `rx_bytes` 一路涨，却被当成 AT 口用了 15 h（2026-10-23 现场）。 */
static void case_probe_wrong_content(void)
{
    char            slave[128];
    int             master;
    int             fd;
    pthread_t       th;
    struct mock_mod mm;

    master = pty_pair(slave, sizeof slave);
    chk(master >= 0, "p05 pty pair created");
    if (master < 0)
        return;

    if (mod_start(&th, &mm, master, "\r\nERROR\r\n+CME ERROR: 10\r\n") != 0) {
        (void)close(master);
        return;
    }
    fd = iv_cellport_probe(slave, 0u, 200u);
    (void)pthread_join(th, NULL);
    /* `got_at` 是这两条用例的命门：没有它，"超时"也可能只是数据没送到
     * （预置写入被 `tcflush` 冲掉），被测的分支一次都没跑过 —— 就是假绿。 */
    chk(mm.got_at == 1, "p05 the probe sent AT before the ERROR reply");
    chk(fd == IV_ETIMEDOUT, "p05 data without OK -> IV_ETIMEDOUT");
    if (fd > 0)   /* 真到这一步说明实现有 bug：别把它当成功悄悄放过 */
        (void)iv_serial_close(fd);
    (void)close(master);

    /* 二进制流里恰好出现 "OK" 两个字节也不能算（按行认，不按字节认）。
     * 用 `\x01`/`\x02` 而不是 `\x11`/`\x12`：XON/XOFF 在非 raw 的 tty 上会被
     * 流控吃掉，"数据确实到了"这个前提就不成立，用例又会变成假绿。 */
    master = pty_pair(slave, sizeof slave);
    if (master >= 0) {
        if (mod_start(&th, &mm, master, "\x01OK\x02\r\n") != 0) {
            (void)close(master);
            return;
        }
        fd = iv_cellport_probe(slave, 0u, 200u);
        (void)pthread_join(th, NULL);
        chk(mm.got_at == 1, "p06 the probe sent AT before the binary reply");
        chk(fd == IV_ETIMEDOUT, "p06 OK inside a binary stream is not accepted");
        if (fd > 0)
            (void)iv_serial_close(fd);
        (void)close(master);
    }
}

static void case_probe_errors(void)
{
    char  path[128];
    int   fd;

    fd = iv_cellport_probe("/dev/ttyUSB99", 0u, 50u);
    chk(fd == IV_ENOENT, "p07 missing node -> IV_ENOENT, not a timeout");

    /* 普通文件：`iv_serial_open` 会以 IV_EINVAL 拒掉（不是 tty） */
    (void)snprintf(path, sizeof path, "/tmp/ivcp_%ld.txt", (long)getpid());
    {
        FILE *f = fopen(path, "w");

        chk(f != NULL, "p08 temp file created");
        if (f != NULL) {
            (void)fputs("x", f);
            (void)fclose(f);
        }
    }
    fd = iv_cellport_probe(path, 0u, 50u);
    chk(fd == IV_EINVAL, "p08 regular file -> IV_EINVAL");
    (void)unlink(path);

    chk(iv_cellport_probe(NULL, 0u, 0u) == IV_EINVAL, "p09 NULL dev -> IV_EINVAL");
    chk(iv_cellport_probe("", 0u, 0u) == IV_EINVAL, "p10 empty dev -> IV_EINVAL");
}

/* 扫描：没有 /dev/ttyUSB* 的机器上必须立刻返回 ENOENT 且不留半截路径。
 * 接了真模组的机器上这条不成立 —— 那种情况下只断言"不崩、不留半截"。 */
static void case_scan(void)
{
    char buf[IV_CELLPORT_PATH_MAX];
    int  fd;
    int  has_node = (access("/dev/ttyUSB0", F_OK) == 0);

    memset(buf, 'X', sizeof buf);
    fd = iv_cellport_scan(buf, sizeof buf, 2u, 50u);
    if (!has_node) {
        chk(fd == IV_ENOENT, "s01 no candidate node -> IV_ENOENT");
        chk(buf[0] == '\0', "s01 path_out cleared on failure");
    } else {
        chk(fd != 0, "s01 with a real ttyUSB0 present: fd or error, never 0");
        if (fd > 0)
            (void)iv_serial_close(fd);
    }

    /* path_out 为 NULL 也必须能跑（调用方不关心命中哪个口时） */
    fd = iv_cellport_scan(NULL, 0, 1u, 50u);
    if (fd > 0)
        (void)iv_serial_close(fd);
    chk(1, "s02 scan tolerates path_out == NULL");
}

int main(void)
{
    case_name();
    case_probe_ok();
    case_probe_silent();
    case_probe_wrong_content();
    case_probe_errors();
    case_scan();

    if (g_fail != 0) {
        fprintf(stderr, "test_cellport FAILED (%d failed checks)\n", g_fail);
        return 1;
    }
    printf("test_cellport passed (name, probe OK/silent/wrong-content, errno paths, scan)\n");
    return 0;
}
