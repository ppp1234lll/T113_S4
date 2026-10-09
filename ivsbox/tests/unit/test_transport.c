/*
 * iv_transport 单测（libivmodules，功能开发计划 M3-S3.5）
 *
 * ============================ 测法 ============================
 * 两路并用：
 *   - c01~c09：**注入式假 I/O**（fake_t）—— 确定性驱动，可脚本化"异步 connect
 *     未完成/失败、部分写、写中途出错、对端 EOF、退避重连"，这些用真 socket
 *     很难稳定复现。断言点包括"客户端**发出的字节**"（逐字节比对）。
 *   - c10：**真实回环 TCP**（127.0.0.1 上临时 listen/accept）—— 验证内置真实
 *     socket I/O 与端到端字节一致性（这是计划 §S3.5 的验证口径）。
 * 全程不引 pthread、不连外网；socket 用完即关（AGENTS.md 规则 6）。
 *
 * ============================ 反向证伪点（每条对应一个真机制） ============================
 *   c04 部分写必须**按偏移续写**（不是每次从头写）—— 从头写会在链路上产生重复字节；
 *   c06 取件泵必须**整件写尽才 ack**（不是入队即 ack）—— 提前 ack 会丢帧；
 *   c08 断线必须**丢弃未 ack 在途件**（不是 ack 后丢，也不是续写到新连接）——
 *       ack 了就没机会重发，续写会拼出半截帧；
 *   c05 退避**未到期不得重试**（不是每 step 都试）—— 否则断网时每秒 flood connect。
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_transport.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 注入式假 I/O
 * ------------------------------------------------------------------------- */
typedef struct {
    int      fd;
    int      open_fail; /* 1 = sock_open 返回 -1 */
    /* connect 返回值序列（用尽后重复最后一个）；默认 IV_OK */
    int      connect_ret[8];
    int      connect_n;
    int      connect_i;
    int      connect_calls;
    /* 异步 connect 结果序列（sock_error）；默认 IV_EAGAIN */
    int      error_ret[8];
    int      error_n;
    int      error_i;
    /* write：单次最多写 write_chunk 字节（<=0 不限）；write_calls 达
     * write_fail_after 后返回 IV_EIO（<0 表示不失败） */
    int      write_chunk;
    int      write_fail_after;
    int      write_calls;
    uint8_t  sent[4096]; /* 对端**实际收到**的字节（供逐字节比对） */
    size_t   sent_len;
    /* read：脚本化字节流 */
    const uint8_t *rx;
    size_t         rx_len;
    size_t         rx_off;
    int            rx_chunk; /* 单次最多读出字节；<=0 不限 */
    int            rx_eof;   /* 1 = 读尽后返回 0(EOF)，否则 IV_EAGAIN */
    int            close_calls;
} fake_t;

static int f_open(void *arg)
{
    fake_t *f = (fake_t *)arg;
    if (f->open_fail) {
        return -1;
    }
    return f->fd;
}

static int f_connect(void *arg, int fd, const char *host, uint16_t port)
{
    fake_t *f = (fake_t *)arg;
    int     r;
    (void)fd;
    (void)host;
    (void)port;
    if (f->connect_i < f->connect_n) {
        r = f->connect_ret[f->connect_i++];
    } else {
        r = (f->connect_n > 0) ? f->connect_ret[f->connect_n - 1] : IV_OK;
    }
    f->connect_calls++;
    return r;
}

static int f_error(void *arg, int fd)
{
    fake_t *f = (fake_t *)arg;
    int     r;
    (void)fd;
    if (f->error_i < f->error_n) {
        r = f->error_ret[f->error_i++];
    } else {
        r = (f->error_n > 0) ? f->error_ret[f->error_n - 1] : IV_EAGAIN;
    }
    return r;
}

static int f_write(void *arg, int fd, const uint8_t *buf, size_t len)
{
    fake_t *f = (fake_t *)arg;
    size_t  n;

    (void)fd;
    if (f->write_fail_after >= 0 && f->write_calls >= f->write_fail_after) {
        return IV_EIO;
    }
    f->write_calls++;
    n = len;
    if (f->write_chunk > 0 && n > (size_t)f->write_chunk) {
        n = (size_t)f->write_chunk;
    }
    if (f->sent_len + n <= sizeof f->sent) {
        memcpy(f->sent + f->sent_len, buf, n);
        f->sent_len += n;
    }
    return (int)n;
}

static int f_read(void *arg, int fd, uint8_t *buf, size_t cap)
{
    fake_t *f = (fake_t *)arg;
    size_t  avail;

    (void)fd;
    if (f->rx_off >= f->rx_len) {
        return f->rx_eof ? 0 : IV_EAGAIN;
    }
    avail = f->rx_len - f->rx_off;
    if (f->rx_chunk > 0 && avail > (size_t)f->rx_chunk) {
        avail = (size_t)f->rx_chunk;
    }
    if (avail > cap) {
        avail = cap;
    }
    memcpy(buf, f->rx + f->rx_off, avail);
    f->rx_off += avail;
    return (int)avail;
}

static void f_close(void *arg, int fd)
{
    fake_t *f = (fake_t *)arg;
    (void)fd;
    f->close_calls++;
    /* 每条连接是独立字节流：关连接即清空"对端已收"，这样"断线后把半截帧
     * 续写到新连接"的缺陷才无处遁形（否则 HE + 新连接 LLO 拼起来看着像完整帧） */
    f->sent_len = 0;
}

static const iv_transport_io_t g_fake_io = {
    f_open, f_connect, f_write, f_read, f_error, f_close, NULL,
};

/* ---------------------------------------------------------------------------
 * 取件泵假实现：peek 语义（**ack 才推进**），模拟 iv_queue 的"最老未确认"
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *items[8];
    size_t      lens[8];
    uint64_t    cookies[8];
    int         n;
    int         idx;
    uint64_t    acked[8];
    size_t      ack_sent_len[8]; /* ack 时刻 fake 已收到的字节数（验"写尽才 ack"） */
    int         acked_n;
    fake_t     *io;
} pumpfake_t;

static int pf_pull(void *arg, const uint8_t **b, size_t *len, uint64_t *ck)
{
    pumpfake_t *p = (pumpfake_t *)arg;
    if (p->idx >= p->n) {
        return IV_EAGAIN;
    }
    *b   = (const uint8_t *)p->items[p->idx];
    *len = p->lens[p->idx];
    *ck  = p->cookies[p->idx];
    return IV_OK;
}

static void pf_ack(void *arg, uint64_t ck)
{
    pumpfake_t *p = (pumpfake_t *)arg;
    (void)ck;
    if (p->acked_n < 8) {
        p->acked[p->acked_n]        = p->cookies[p->idx];
        p->ack_sent_len[p->acked_n] = p->io->sent_len;
        p->acked_n++;
    }
    p->idx++; /* 仅确认后推进到下一件 */
}

/* ---------------------------------------------------------------------------
 * 回调接收端
 * ------------------------------------------------------------------------- */
static uint8_t g_rx[4096];
static size_t  g_rx_len;
static int     g_saw_up;
static int     g_saw_down;

static void rx_sink(void *arg, const uint8_t *b, size_t n)
{
    (void)arg;
    if (g_rx_len + n <= sizeof g_rx) {
        memcpy(g_rx + g_rx_len, b, n);
        g_rx_len += n;
    }
}

static void st_cb(void *arg, iv_transport_state_t st)
{
    (void)arg;
    if (st == IV_TRANSPORT_UP) {
        g_saw_up = 1;
    }
    if (st == IV_TRANSPORT_DOWN) {
        g_saw_down = 1;
    }
}

static void reset_sinks(void)
{
    g_rx_len   = 0;
    g_saw_up   = 0;
    g_saw_down = 0;
}

/* ---------------------------------------------------------------------------
 * c01 参数与守卫
 * ------------------------------------------------------------------------- */
static void c01_args(void)
{
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    iv_transport_io_t   io;
    fake_t              f;
    char                big[200];

    memset(&f, 0, sizeof f);
    f.fd               = 7;
    f.write_fail_after = -1;
    f.connect_ret[0]   = IV_EAGAIN; /* 停在 CONNECTING，不真连 */
    f.connect_n        = 1;
    io     = g_fake_io;
    io.arg = &f;

    iv_transport_init(&t);
    memset(&c, 0, sizeof c);
    c.io = &io;

    chk(iv_transport_open(NULL, &c) == IV_EINVAL, "c01 open(NULL t)");
    chk(iv_transport_open(&t, NULL) == IV_EINVAL, "c01 open(NULL cfg)");
    c.host = "";
    chk(iv_transport_open(&t, &c) == IV_EINVAL, "c01 empty host");
    c.host = NULL;
    chk(iv_transport_open(&t, &c) == IV_EINVAL, "c01 NULL host");

    memset(big, 'a', sizeof big);
    big[sizeof big - 1u] = '\0';
    c.host = big;
    chk(iv_transport_open(&t, &c) == IV_ERANGE, "c01 host too long");

    /* 未 open 的守卫 */
    chk(iv_transport_step(&t, 0) == IV_ESTATE, "c01 step before open");
    chk(iv_transport_close(&t) == IV_ESTATE, "c01 close before open");
    chk(iv_transport_send(&t, (const uint8_t *)"x", 1) == IV_EINVAL,
        "c01 send before open");
    chk(iv_transport_state(&t) == IV_TRANSPORT_CLOSED, "c01 state closed");

    c.host = "1.2.3.4";
    c.port = 1234;
    chk(iv_transport_open(&t, &c) == IV_OK, "c01 open ok");
    chk(iv_transport_open(&t, &c) == IV_ESTATE, "c01 double open");
    chk(iv_transport_close(&t) == IV_OK, "c01 close ok");
    chk(iv_transport_close(&t) == IV_ESTATE, "c01 double close");
}

/* ---------------------------------------------------------------------------
 * c02 立即建连 + 发送 + 接收
 * ------------------------------------------------------------------------- */
static void c02_connect_send_rx(void)
{
    static const uint8_t tx[] = { 0x10u, 0x20u, 0x30u, 0x40u, 0x50u };
    static const uint8_t rx[] = { 0xA1u, 0xA2u, 0xA3u };
    fake_t              f;
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    iv_transport_io_t   io;
    uint64_t            now = 0;

    memset(&f, 0, sizeof f);
    f.fd               = 9;
    f.write_fail_after = -1;
    f.connect_ret[0]   = IV_OK;
    f.connect_n        = 1;
    io     = g_fake_io;
    io.arg = &f;
    reset_sinks();

    memset(&c, 0, sizeof c);
    c.host     = "127.0.0.1";
    c.port     = 5000;
    c.io       = &io;
    c.on_rx    = rx_sink;
    c.on_state = st_cb;

    iv_transport_init(&t);
    chk(iv_transport_open(&t, &c) == IV_OK, "c02 open");

    now += 10;
    chk(iv_transport_step(&t, now) == IV_OK, "c02 step1");
    chk(iv_transport_state(&t) == IV_TRANSPORT_UP, "c02 immediate connect => UP");
    chk(g_saw_up == 1, "c02 on_state UP fired");
    chk(iv_transport_connects(&t) == 1u, "c02 connects=1");
    chk(f.connect_calls == 1, "c02 one connect call");

    chk(iv_transport_send(&t, tx, sizeof tx) == IV_OK, "c02 send");
    now += 10;
    iv_transport_step(&t, now);
    chk(iv_transport_tx_pending(&t) == 0u, "c02 tx drained");
    chk(f.sent_len == sizeof tx, "c02 peer got 5 bytes");
    chk(memcmp(f.sent, tx, sizeof tx) == 0, "c02 tx byte-identical");
    chk(iv_transport_bytes_tx(&t) == sizeof tx, "c02 bytes_tx");

    f.rx     = rx;
    f.rx_len = sizeof rx;
    now += 10;
    iv_transport_step(&t, now);
    chk(g_rx_len == sizeof rx, "c02 rx len");
    chk(memcmp(g_rx, rx, sizeof rx) == 0, "c02 rx bytes");
    chk(iv_transport_bytes_rx(&t) == sizeof rx, "c02 bytes_rx");

    chk(iv_transport_close(&t) == IV_OK, "c02 close");
    chk(f.close_calls == 1, "c02 fd closed");
}

/* ---------------------------------------------------------------------------
 * c03 异步建连（EINPROGRESS → 完成）
 * ------------------------------------------------------------------------- */
static void c03_async_connect(void)
{
    fake_t              f;
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    iv_transport_io_t   io;
    uint64_t            now = 0;

    memset(&f, 0, sizeof f);
    f.fd               = 3;
    f.write_fail_after = -1;
    f.connect_ret[0]   = IV_EAGAIN;
    f.connect_n        = 1;
    f.error_ret[0]     = IV_EAGAIN;
    f.error_ret[1]     = IV_OK;
    f.error_n          = 2;
    io     = g_fake_io;
    io.arg = &f;
    reset_sinks();

    memset(&c, 0, sizeof c);
    c.host     = "10.0.0.1";
    c.port     = 2000;
    c.io       = &io;
    c.on_state = st_cb;

    iv_transport_init(&t);
    chk(iv_transport_open(&t, &c) == IV_OK, "c03 open");
    now += 10;
    iv_transport_step(&t, now);
    chk(iv_transport_state(&t) == IV_TRANSPORT_CONNECTING, "c03 still connecting");
    chk(iv_transport_connects(&t) == 0u, "c03 no connect yet");

    now += 10;
    iv_transport_step(&t, now);
    chk(iv_transport_state(&t) == IV_TRANSPORT_CONNECTING, "c03 connecting #2");
    now += 10;
    iv_transport_step(&t, now);
    chk(iv_transport_state(&t) == IV_TRANSPORT_UP, "c03 async completed => UP");
    chk(iv_transport_connects(&t) == 1u, "c03 connects=1");
    chk(g_saw_up == 1, "c03 UP callback");
    iv_transport_close(&t);
}

/* ---------------------------------------------------------------------------
 * c04 部分写必须按偏移续写
 * ------------------------------------------------------------------------- */
static void c04_partial_write(void)
{
    static const uint8_t tx[] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u };
    fake_t              f;
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    iv_transport_io_t   io;
    uint64_t            now = 0;
    int                 i;

    memset(&f, 0, sizeof f);
    f.fd               = 5;
    f.write_fail_after = -1;
    f.write_chunk      = 3; /* 每次最多写 3 字节 */
    f.connect_ret[0]   = IV_OK;
    f.connect_n        = 1;
    io     = g_fake_io;
    io.arg = &f;
    reset_sinks();

    memset(&c, 0, sizeof c);
    c.host = "127.0.0.1";
    c.port = 1;
    c.io   = &io;

    iv_transport_init(&t);
    iv_transport_open(&t, &c);
    now += 10;
    iv_transport_step(&t, now);
    chk(iv_transport_state(&t) == IV_TRANSPORT_UP, "c04 UP");

    chk(iv_transport_send(&t, tx, sizeof tx) == IV_OK, "c04 send 10");
    for (i = 0; i < 10 && iv_transport_tx_pending(&t) > 0u; i++) {
        now += 10;
        iv_transport_step(&t, now);
    }
    chk(iv_transport_tx_pending(&t) == 0u, "c04 fully drained");
    /* 关键：对端收到恰好 10 字节且**逐字节一致、无重复**（续写从偏移开始；
     * 若每 step 从头写，sent 会出现 1,2,3,1,2,3… 的重复） */
    chk(f.sent_len == sizeof tx, "c04 peer got exactly 10 bytes");
    chk(memcmp(f.sent, tx, sizeof tx) == 0, "c04 resumed at offset, no dup");
    chk(iv_transport_bytes_tx(&t) == sizeof tx, "c04 bytes_tx=10");
    iv_transport_close(&t);
}

/* ---------------------------------------------------------------------------
 * c05 退避重连：未到期不得重试
 * ------------------------------------------------------------------------- */
static void c05_backoff(void)
{
    fake_t              f;
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    iv_transport_io_t   io;
    uint64_t            now = 0;

    memset(&f, 0, sizeof f);
    f.fd               = 11;
    f.write_fail_after = -1;
    f.connect_ret[0]   = IV_ECONN; /* 首次失败 */
    f.connect_ret[1]   = IV_OK;    /* 退避后成功 */
    f.connect_n        = 2;
    io     = g_fake_io;
    io.arg = &f;
    reset_sinks();

    memset(&c, 0, sizeof c);
    c.host           = "127.0.0.1";
    c.port           = 9;
    c.io             = &io;
    c.backoff_ms     = 100;
    c.backoff_max_ms = 1000;
    c.on_state       = st_cb;

    iv_transport_init(&t);
    iv_transport_open(&t, &c);

    now = 0;
    iv_transport_step(&t, now); /* 首次尝试 → 失败 → DOWN */
    chk(iv_transport_state(&t) == IV_TRANSPORT_DOWN, "c05 failed => DOWN");
    chk(iv_transport_drops(&t) == 1u, "c05 drops=1");
    chk(f.connect_calls == 1, "c05 one attempt");
    chk(g_saw_down == 1, "c05 DOWN callback");

    now = 50; /* 未到 retry_at(=100) */
    iv_transport_step(&t, now);
    chk(f.connect_calls == 1, "c05 no retry before backoff expires");

    now = 100; /* 到点 */
    iv_transport_step(&t, now);
    chk(f.connect_calls == 2, "c05 retried at backoff");
    chk(iv_transport_state(&t) == IV_TRANSPORT_UP, "c05 reconnected => UP");
    chk(iv_transport_connects(&t) == 1u, "c05 connects=1");
    chk(iv_transport_drops(&t) == 1u, "c05 drops still 1");
    iv_transport_close(&t);
}

/* ---------------------------------------------------------------------------
 * c06 取件泵：顺序 + 整件写尽才 ack + 对端收到正确字节
 * ------------------------------------------------------------------------- */
static void c06_pump(void)
{
    pumpfake_t          p;
    fake_t              f;
    iv_transport_io_t   io;
    iv_transport_pump_t pump;
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    uint64_t            now = 0;
    int                 i;

    memset(&p, 0, sizeof p);
    p.items[0]   = "AA";
    p.lens[0]    = 2;
    p.cookies[0] = 1;
    p.items[1]   = "BBBB";
    p.lens[1]    = 4;
    p.cookies[1] = 2;
    p.n          = 2;

    memset(&f, 0, sizeof f);
    f.fd               = 13;
    f.write_fail_after = -1;
    f.connect_ret[0]   = IV_OK;
    f.connect_n        = 1;
    p.io               = &f;
    io                 = g_fake_io;
    io.arg             = &f;
    reset_sinks();

    memset(&pump, 0, sizeof pump);
    pump.pull = pf_pull;
    pump.ack  = pf_ack;
    pump.arg  = &p;

    memset(&c, 0, sizeof c);
    c.host = "127.0.0.1";
    c.port = 2;
    c.io   = &io;
    c.pump = &pump;

    iv_transport_init(&t);
    iv_transport_open(&t, &c);

    for (i = 0; i < 20 && p.acked_n < 2; i++) {
        now += 10;
        iv_transport_step(&t, now);
    }
    chk(p.acked_n == 2, "c06 both items acked");
    chk(p.acked[0] == 1u && p.acked[1] == 2u, "c06 ack order is seq order");
    chk(p.ack_sent_len[0] == 2u, "c06 ack0 after item0 fully written");
    chk(p.ack_sent_len[1] == 6u, "c06 ack1 after item1 fully written");
    chk(f.sent_len == 6u, "c06 peer got 6 bytes");
    chk(memcmp(f.sent, "AABBBB", 6u) == 0, "c06 concatenated in order");
    iv_transport_close(&t);
}

/* ---------------------------------------------------------------------------
 * c07 发送守卫：非 UP / 超限 / 暂存满
 * ------------------------------------------------------------------------- */
static void c07_send_guard(void)
{
    fake_t              f;
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    iv_transport_io_t   io;
    uint8_t             big[IV_TRANSPORT_TX_MAX + 8u];
    uint64_t            now = 0;

    memset(&f, 0, sizeof f);
    f.fd               = 17;
    f.write_fail_after = -1;
    f.connect_ret[0]   = IV_EAGAIN; /* 停在 CONNECTING */
    f.connect_n        = 1;
    f.error_ret[0]     = IV_EAGAIN;
    f.error_n          = 1;
    io     = g_fake_io;
    io.arg = &f;
    reset_sinks();

    memset(&c, 0, sizeof c);
    c.host = "127.0.0.1";
    c.port = 3;
    c.io   = &io;
    memset(big, 0x5A, sizeof big);

    iv_transport_init(&t);
    iv_transport_open(&t, &c);
    now += 10;
    iv_transport_step(&t, now);
    chk(iv_transport_state(&t) == IV_TRANSPORT_CONNECTING, "c07 connecting");
    chk(iv_transport_send(&t, (const uint8_t *)"x", 1) == IV_ESTATE,
        "c07 send not-UP => ESTATE");

    /* 让下一次 error 返回成功 → UP */
    f.error_ret[0] = IV_OK;
    f.error_n      = 1;
    f.error_i      = 0;
    now += 10;
    iv_transport_step(&t, now);
    chk(iv_transport_state(&t) == IV_TRANSPORT_UP, "c07 UP now");

    chk(iv_transport_send(&t, big, sizeof big) == IV_ERANGE, "c07 oversize => ERANGE");
    {
        uint8_t half[IV_TRANSPORT_TX_MAX / 2u];
        memset(half, 0x11, sizeof half);
        chk(iv_transport_send(&t, half, sizeof half) == IV_OK, "c07 fill 1/2");
        chk(iv_transport_send(&t, half, sizeof half) == IV_OK, "c07 fill 2/2");
        chk(iv_transport_send(&t, (const uint8_t *)"x", 1) == IV_EFULL,
            "c07 full => EFULL");
    }
    iv_transport_close(&t);
}

/* ---------------------------------------------------------------------------
 * c08 断线丢未 ack 在途件，重连后重发
 * ------------------------------------------------------------------------- */
static void c08_drop_repull(void)
{
    pumpfake_t          p;
    fake_t              f;
    iv_transport_io_t   io;
    iv_transport_pump_t pump;
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    uint64_t            now = 0;
    int                 i;

    memset(&p, 0, sizeof p);
    p.items[0]   = "HELLO";
    p.lens[0]    = 5;
    p.cookies[0] = 42;
    p.n          = 1;

    memset(&f, 0, sizeof f);
    f.fd               = 19;
    f.write_chunk      = 2; /* 一次只写 2 字节 */
    f.write_fail_after = 1; /* 第 2 次写就失败（此时 5 字节只写出 2） */
    f.connect_ret[0]   = IV_OK;
    f.connect_ret[1]   = IV_OK; /* 重连也成功 */
    f.connect_n        = 2;
    p.io               = &f;
    io                 = g_fake_io;
    io.arg             = &f;
    reset_sinks();

    memset(&pump, 0, sizeof pump);
    pump.pull = pf_pull;
    pump.ack  = pf_ack;
    pump.arg  = &p;

    memset(&c, 0, sizeof c);
    c.host           = "127.0.0.1";
    c.port           = 4;
    c.io             = &io;
    c.pump           = &pump;
    c.backoff_ms     = 100;
    c.backoff_max_ms = 100;

    iv_transport_init(&t);
    iv_transport_open(&t, &c);

    now += 10;
    iv_transport_step(&t, now); /* UP → 拉件 → 写 2 字节 → 第 2 次写失败 → DOWN */
    chk(iv_transport_state(&t) == IV_TRANSPORT_DOWN, "c08 write error => DOWN");
    chk(p.acked_n == 0, "c08 in-flight item NOT acked on drop");
    chk(p.idx == 0, "c08 pump not advanced");
    chk(iv_transport_drops(&t) == 1u, "c08 drops=1");

    f.write_fail_after = -1;
    f.write_chunk      = 0;
    now += 100;
    for (i = 0; i < 20 && p.acked_n < 1; i++) {
        iv_transport_step(&t, now);
        now += 10;
    }
    chk(iv_transport_state(&t) == IV_TRANSPORT_UP, "c08 reconnected");
    chk(p.acked_n == 1, "c08 item re-pulled and acked once");
    chk(p.acked[0] == 42u, "c08 acked cookie=42");
    chk(f.sent_len == 5u, "c08 fresh connection carried exactly the full item");
    chk(memcmp(f.sent, "HELLO", 5u) == 0,
        "c08 full item re-sent, no torn frame across connections");
    chk(iv_transport_connects(&t) == 2u, "c08 connects=2");
    iv_transport_close(&t);
}

/* ---------------------------------------------------------------------------
 * c09 对端关闭（EOF）→ DOWN
 * ------------------------------------------------------------------------- */
static void c09_eof(void)
{
    fake_t              f;
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    iv_transport_io_t   io;
    uint64_t            now = 0;

    memset(&f, 0, sizeof f);
    f.fd               = 23;
    f.write_fail_after = -1;
    f.connect_ret[0]   = IV_OK;
    f.connect_n        = 1;
    f.rx_eof           = 1; /* 无数据，读即 EOF */
    io     = g_fake_io;
    io.arg = &f;
    reset_sinks();

    memset(&c, 0, sizeof c);
    c.host     = "127.0.0.1";
    c.port     = 6;
    c.io       = &io;
    c.on_state = st_cb;

    iv_transport_init(&t);
    iv_transport_open(&t, &c);
    now += 10;
    iv_transport_step(&t, now);
    chk(iv_transport_state(&t) == IV_TRANSPORT_DOWN, "c09 EOF => DOWN");
    chk(iv_transport_drops(&t) == 1u, "c09 drops=1");
    chk(g_saw_down == 1, "c09 DOWN callback");
    iv_transport_close(&t);
}

/* ---------------------------------------------------------------------------
 * c10 真实回环 TCP：真 socket I/O 端到端字节一致
 * ------------------------------------------------------------------------- */
static void c10_real_loopback(void)
{
    int                ls;
    int                cs = -1;
    struct sockaddr_in sa;
    socklen_t          sl = sizeof sa;
    iv_transport_t      t;
    iv_transport_cfg_t  c;
    uint16_t            port;
    uint64_t            now = 0;
    int                 i;
    struct pollfd       pfd;
    uint8_t             b[64];
    ssize_t             rn;
    static const uint8_t msg[] = { 0x01u, 0x02u, 0x03u, 0xA5u, 0xFFu };
    static const uint8_t in[]  = { 0xDEu, 0xADu, 0xBEu, 0xEFu };

    ls = socket(AF_INET, SOCK_STREAM, 0);
    chk(ls >= 0, "c10 listen socket");
    if (ls < 0) {
        return;
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0; /* 临时端口 */
    chk(bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0, "c10 bind");
    chk(listen(ls, 4) == 0, "c10 listen");
    chk(getsockname(ls, (struct sockaddr *)&sa, &sl) == 0, "c10 getsockname");
    port = ntohs(sa.sin_port);

    reset_sinks();
    memset(&c, 0, sizeof c);
    c.host     = "127.0.0.1";
    c.port     = port;
    c.on_rx    = rx_sink;
    c.on_state = st_cb;
    /* c.io = NULL → 内置真实 socket */

    iv_transport_init(&t);
    chk(iv_transport_open(&t, &c) == IV_OK, "c10 open");

    for (i = 0; i < 500 && iv_transport_state(&t) != IV_TRANSPORT_UP; i++) {
        now += 10;
        iv_transport_step(&t, now);
    }
    chk(iv_transport_state(&t) == IV_TRANSPORT_UP, "c10 reached UP");

    pfd.fd      = ls;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    chk(poll(&pfd, 1, 2000) > 0, "c10 pending accept");
    cs = accept(ls, NULL, NULL);
    chk(cs >= 0, "c10 accept");
    close(ls); /* 关掉 listener，确保后面重连尝试不会立即成功 */

    chk(iv_transport_send(&t, msg, sizeof msg) == IV_OK, "c10 send");
    for (i = 0; i < 50 && iv_transport_tx_pending(&t) > 0u; i++) {
        now += 10;
        iv_transport_step(&t, now);
    }
    chk(iv_transport_tx_pending(&t) == 0u, "c10 tx drained");
    rn = recv(cs, b, sizeof b, MSG_DONTWAIT);
    chk(rn == (ssize_t)sizeof msg, "c10 peer got msg len");
    chk(rn == (ssize_t)sizeof msg && memcmp(b, msg, sizeof msg) == 0,
        "c10 sent bytes byte-identical");

    chk(send(cs, in, sizeof in, MSG_NOSIGNAL) == (ssize_t)sizeof in, "c10 peer write");
    for (i = 0; i < 50 && g_rx_len < sizeof in; i++) {
        now += 10;
        iv_transport_step(&t, now);
    }
    chk(g_rx_len == sizeof in, "c10 rx len");
    chk(memcmp(g_rx, in, sizeof in) == 0, "c10 rx byte-identical");

    close(cs);
    cs = -1;
    for (i = 0; i < 50 && !g_saw_down; i++) {
        now += 10;
        iv_transport_step(&t, now);
    }
    chk(g_saw_down == 1, "c10 peer close observed (DOWN)");
    chk(iv_transport_drops(&t) >= 1u, "c10 drop counted");

    chk(iv_transport_close(&t) == IV_OK, "c10 close");
    if (cs >= 0) {
        close(cs);
    }
}

int main(void)
{
    c01_args();
    c02_connect_send_rx();
    c03_async_connect();
    c04_partial_write();
    c05_backoff();
    c06_pump();
    c07_send_guard();
    c08_drop_repull();
    c09_eof();
    c10_real_loopback();

    if (g_fail != 0) {
        fprintf(stderr, "test_transport FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_transport passed (args, connect/send/rx, async-connect, "
           "partial-write, backoff, pump-order-ack, send-guard, drop-repull, "
           "eof, real-loopback)\n");
    return 0;
}
