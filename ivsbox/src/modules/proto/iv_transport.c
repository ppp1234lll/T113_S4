/*
 * 平台通道 TCP 传输层实现（libivmodules，功能开发计划 M3-S3.5）
 *
 * 依赖方向：只用 libc（socket / getaddrinfo / poll）+ iv_ret.h，不引 pthread、
 * 不引 iv_reactor、不引任何 hal 符号 —— 落在 libivmodules.a（架构 §15.5）。
 *
 * 三个内部部件：
 *   1. 连接状态机（CLOSED/CONNECTING/UP/DOWN）＋退避重连；
 *   2. 线性发送暂存（tx_len 有效字节在 tbuf[0..tx_len)），部分写续写；
 *      头部可挂一件"取件泵在途件"（pump_len 字节），它写尽后回调 ack；
 *   3. 取件泵：仅当暂存空且无在途件时 pull 一件（保证"一件连续、写尽才 ack"）。
 *
 * 断线纪律：一旦判定链路断开，**丢弃未写出的暂存字节、且不 ack 在途件** ——
 * 在途件未被 ack，仍留在持久队列里，重连后整件重发（可能重复，由平台侧幂等
 * 与序号吸收，见 iv_queue.h 的"至少一次"语义）。绝不把半截帧续到新连接上。
 */
#include "ivsbox/iv_transport.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * 内置真实 socket I/O
 * ------------------------------------------------------------------------- */

static int real_sock_open(void *arg)
{
    int fd;

    (void)arg;
    /* SOCK_NONBLOCK：本层全部 socket 都是非阻塞，step() 驱动、绝不阻塞调用方。
     * SOCK_CLOEXEC：避免 fd 泄漏进子进程（如 media 拉起的外部命令）。 */
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    return fd;
}

static int real_sock_connect(void *arg, int fd, const char *host, uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char            portstr[8];
    int             rc;
    int             r = IV_ECONN;

    (void)arg;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    /* port ≤ 65535 ⇒ 最多 5 位十进制，char[8] 足够；有界格式不会触发截断告警 */
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0 || res == NULL) {
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return IV_ENOTSUP;
    }
    if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
        r = IV_OK;
    } else if (errno == EINPROGRESS) {
        r = IV_EAGAIN;
    } else {
        r = IV_ECONN;
    }
    freeaddrinfo(res);
    return r;
}

static int real_sock_write(void *arg, int fd, const uint8_t *buf, size_t len)
{
    ssize_t n;

    (void)arg;
    /* MSG_NOSIGNAL：对端已关时返回 EPIPE 而不是发 SIGPIPE 打死进程 */
    n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) {
        return (int)n;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return IV_EAGAIN;
    }
    return IV_EIO;
}

static int real_sock_read(void *arg, int fd, uint8_t *buf, size_t cap)
{
    ssize_t n;

    (void)arg;
    n = recv(fd, buf, cap, 0);
    if (n > 0) {
        return (int)n;
    }
    if (n == 0) {
        return 0; /* 对端关闭（EOF） */
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return IV_EAGAIN;
    }
    return IV_EIO;
}

static int real_sock_error(void *arg, int fd)
{
    struct pollfd p;
    int           err = 0;
    socklen_t     el  = sizeof err;

    (void)arg;
    p.fd      = fd;
    p.events  = POLLOUT;
    p.revents = 0;
    if (poll(&p, 1, 0) < 0) {
        return IV_EIO;
    }
    if (p.revents == 0) {
        return IV_EAGAIN; /* connect 尚未完成 */
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0) {
        return IV_EIO;
    }
    if (err != 0) {
        return IV_ECONN;
    }
    return IV_OK;
}

static void real_sock_close(void *arg, int fd)
{
    (void)arg;
    if (fd >= 0) {
        (void)close(fd);
    }
}

static const iv_transport_io_t g_real_io = {
    real_sock_open,
    real_sock_connect,
    real_sock_write,
    real_sock_read,
    real_sock_error,
    real_sock_close,
    NULL,
};

/* ---------------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------------- */

static void set_state(iv_transport_t *t, uint8_t st)
{
    if (t->state == st) {
        return;
    }
    t->state = st;
    if (t->on_state != NULL) {
        t->on_state(t->state_arg, (iv_transport_state_t)st);
    }
}

/*
 * 判定链路断开：关 fd、丢弃未写出暂存（不 ack 在途件）、进入 DOWN 排期重连。
 * now_ms 由调用方传入，退避间隔每次翻倍、封顶 backoff_max。
 */
static void link_down(iv_transport_t *t, uint64_t now_ms)
{
    if (t->fd >= 0) {
        t->io->sock_close(t->io->arg, t->fd);
        t->fd = -1;
    }
    /* 丢弃暂存与在途件标记 —— 在途件未被 ack，仍在持久队列，重连后整件重发 */
    t->tx_len      = 0;
    t->pump_len    = 0;
    t->pump_active = 0;
    t->pump_cookie = 0;

    t->drops++;
    t->retry_at_ms = now_ms + (uint64_t)t->backoff;

    /* 下次用双倍退避（溢出/越界都收敛到 backoff_max） */
    {
        uint32_t nb = t->backoff * 2u;
        if (nb < t->backoff || nb > t->backoff_max_ms) {
            nb = t->backoff_max_ms;
        }
        t->backoff = nb;
    }
    set_state(t, IV_TRANSPORT_DOWN);
}

/* 发起一次连接尝试（open / DOWN 到期时调用） */
static void connect_try(iv_transport_t *t, uint64_t now_ms)
{
    int fd;
    int r;

    fd = t->io->sock_open(t->io->arg);
    if (fd < 0) {
        link_down(t, now_ms);
        return;
    }
    t->fd = fd;
    r = t->io->sock_connect(t->io->arg, fd, t->host, t->port);
    if (r == IV_EAGAIN) {
        set_state(t, IV_TRANSPORT_CONNECTING); /* 进行中，等下个 step 查结果 */
        return;
    }
    if (r != IV_OK) {
        link_down(t, now_ms);
        return;
    }
    t->connects++;
    t->backoff = t->backoff_ms; /* 连上即把退避复位到起点 */
    set_state(t, IV_TRANSPORT_UP);
}

/*
 * 消费暂存头部 n 字节：前移剩余、维护在途件计数；写尽则回调 ack。
 * 前置：n ≤ tx_len。
 */
static void tx_consume(iv_transport_t *t, uint16_t n)
{
    if (n >= t->tx_len) {
        t->tx_len = 0;
    } else {
        memmove(t->tbuf, t->tbuf + n, (size_t)(t->tx_len - n));
        t->tx_len = (uint16_t)(t->tx_len - n);
    }

    if (t->pump_active) {
        if (n >= t->pump_len) {
            uint64_t cookie = t->pump_cookie;
            t->pump_len    = 0;
            t->pump_active = 0;
            t->pump_cookie = 0;
            if (t->pump != NULL && t->pump->ack != NULL) {
                t->pump->ack(t->pump->arg, cookie);
            }
        } else {
            t->pump_len = (uint16_t)(t->pump_len - n);
        }
    }
}

/* 把暂存尽量写出；返回 IV_OK 或 IV_ECONN（链路出错，需上层判死） */
static int tx_flush(iv_transport_t *t)
{
    while (t->tx_len > 0) {
        int n = t->io->sock_write(t->io->arg, t->fd, t->tbuf, t->tx_len);
        if (n > 0) {
            t->bytes_tx += (uint64_t)n;
            tx_consume(t, (uint16_t)n);
            continue;
        }
        if (n == IV_EAGAIN) {
            return IV_OK; /* 内核发送缓冲满，下次再写 */
        }
        return IV_ECONN;
    }
    return IV_OK;
}

/* 取一件入暂存（仅当暂存空且无在途件） */
static void pump_fill(iv_transport_t *t)
{
    const uint8_t *bytes  = NULL;
    size_t         len    = 0;
    uint64_t       cookie = 0;
    int            r;

    if (t->pump == NULL || t->pump->pull == NULL) {
        return;
    }
    if (t->pump_active || t->tx_len != 0) {
        return; /* 保证"一件连续"：在途或残留时不取新件 */
    }
    r = t->pump->pull(t->pump->arg, &bytes, &len, &cookie);
    if (r != IV_OK) {
        return; /* IV_EAGAIN=空；其它负=本条不可用，跳过等下一条 */
    }
    if (len == 0u || bytes == NULL) {
        return; /* 无件：不 ack（零长条目无意义，视为"暂无"） */
    }
    if (len > IV_TRANSPORT_TX_MAX) {
        /* 畸形件：ack 丢弃并计数，防止泵被一条超限件永久堵死 */
        t->pump_oversize++;
        if (t->pump->ack != NULL) {
            t->pump->ack(t->pump->arg, cookie);
        }
        return;
    }
    memcpy(t->tbuf, bytes, len);
    t->tx_len      = (uint16_t)len;
    t->pump_len    = (uint16_t)len;
    t->pump_active = 1;
    t->pump_cookie = cookie;
}

/* 读到无数据为止；返回 IV_OK 或 IV_ECONN（EOF / 出错 ⇒ 链路死） */
static int rx_pump(iv_transport_t *t)
{
    for (;;) {
        int n = t->io->sock_read(t->io->arg, t->fd, t->rbuf, sizeof t->rbuf);
        if (n > 0) {
            t->bytes_rx += (uint64_t)n;
            if (t->on_rx != NULL) {
                t->on_rx(t->rx_arg, t->rbuf, (size_t)n);
            }
            continue;
        }
        if (n == IV_EAGAIN) {
            return IV_OK;
        }
        return IV_ECONN; /* n==0(EOF) 或其它负值 —— 都判链路断开 */
    }
}

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

void iv_transport_init(iv_transport_t *t)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof *t);
    t->fd    = -1;
    t->state = IV_TRANSPORT_CLOSED;
}

int iv_transport_open(iv_transport_t *t, const iv_transport_cfg_t *c)
{
    size_t hl;

    if (t == NULL || c == NULL || c->host == NULL || c->host[0] == '\0') {
        return IV_EINVAL;
    }
    if (t->opened) {
        return IV_ESTATE;
    }
    hl = strlen(c->host);
    if (hl + 1u > sizeof t->host) {
        return IV_ERANGE;
    }

    memcpy(t->host, c->host, hl + 1u);
    t->port       = c->port;
    t->io         = (c->io != NULL) ? c->io : &g_real_io;
    t->pump       = c->pump;
    t->on_rx      = c->on_rx;
    t->rx_arg     = c->rx_arg;
    t->on_state   = c->on_state;
    t->state_arg  = c->state_arg;
    t->backoff_ms = (c->backoff_ms != 0u) ? c->backoff_ms
                                          : IV_TRANSPORT_DEF_BACKOFF_MS;
    t->backoff_max_ms = (c->backoff_max_ms != 0u) ? c->backoff_max_ms
                                                  : IV_TRANSPORT_DEF_BACKOFF_MAX_MS;
    if (t->backoff_max_ms < t->backoff_ms) {
        t->backoff_max_ms = t->backoff_ms;
    }

    t->fd          = -1;
    t->tx_len      = 0;
    t->pump_len    = 0;
    t->pump_active = 0;
    t->pump_cookie = 0;
    t->bytes_tx    = 0;
    t->bytes_rx    = 0;
    t->connects    = 0;
    t->drops       = 0;
    t->pump_oversize = 0;
    t->backoff     = t->backoff_ms;
    t->retry_at_ms = 0;
    t->opened      = 1;
    /* 直接置态、不回调：调用方刚发起 open，无需被告知"正在连" */
    t->state = IV_TRANSPORT_CONNECTING;
    return IV_OK;
}

int iv_transport_close(iv_transport_t *t)
{
    if (t == NULL || !t->opened) {
        return IV_ESTATE;
    }
    if (t->fd >= 0) {
        t->io->sock_close(t->io->arg, t->fd);
        t->fd = -1;
    }
    t->tx_len      = 0;
    t->pump_len    = 0;
    t->pump_active = 0;
    t->pump_cookie = 0;
    t->opened      = 0;
    t->state       = IV_TRANSPORT_CLOSED;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 驱动与收发
 * ------------------------------------------------------------------------- */

int iv_transport_step(iv_transport_t *t, uint64_t now_ms)
{
    if (t == NULL || !t->opened) {
        return IV_ESTATE;
    }

    if (t->state == IV_TRANSPORT_CONNECTING) {
        if (t->fd < 0) {
            /* 首次 step（retry_at=0）或重连排期后的一次尝试 */
            if (now_ms >= t->retry_at_ms) {
                connect_try(t, now_ms);
            }
        } else {
            int r = t->io->sock_error(t->io->arg, t->fd);
            if (r == IV_OK) {
                t->connects++;
                t->backoff = t->backoff_ms;
                set_state(t, IV_TRANSPORT_UP);
            } else if (r != IV_EAGAIN) {
                link_down(t, now_ms);
            }
        }
    } else if (t->state == IV_TRANSPORT_DOWN) {
        if (now_ms >= t->retry_at_ms) {
            connect_try(t, now_ms);
        }
    }

    if (t->state == IV_TRANSPORT_UP) {
        pump_fill(t);
        if (tx_flush(t) != IV_OK) {
            link_down(t, now_ms);
            return IV_OK;
        }
        if (rx_pump(t) != IV_OK) {
            link_down(t, now_ms);
            return IV_OK;
        }
    }
    return IV_OK;
}

int iv_transport_send(iv_transport_t *t, const uint8_t *bytes, size_t len)
{
    if (t == NULL || !t->opened) {
        return IV_EINVAL;
    }
    if (bytes == NULL && len != 0u) {
        return IV_EINVAL;
    }
    if (len == 0u) {
        return IV_OK;
    }
    /* 不 UP 不接收即时帧：宁可让调用方丢弃，也不积压过期 ACK/心跳 */
    if (t->state != IV_TRANSPORT_UP) {
        return IV_ESTATE;
    }
    if (len > IV_TRANSPORT_TX_MAX) {
        return IV_ERANGE;
    }
    if (t->tx_len + len > IV_TRANSPORT_TX_MAX) {
        return IV_EFULL;
    }
    memcpy(t->tbuf + t->tx_len, bytes, len);
    t->tx_len = (uint16_t)(t->tx_len + (uint16_t)len);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */

iv_transport_state_t iv_transport_state(const iv_transport_t *t)
{
    if (t == NULL) {
        return IV_TRANSPORT_CLOSED;
    }
    return (iv_transport_state_t)t->state;
}

size_t iv_transport_tx_pending(const iv_transport_t *t)
{
    if (t == NULL) {
        return 0u;
    }
    return (size_t)t->tx_len;
}

int iv_transport_fd(const iv_transport_t *t)
{
    if (t == NULL) {
        return -1;
    }
    return t->fd;
}

uint64_t iv_transport_bytes_tx(const iv_transport_t *t)
{
    return (t == NULL) ? 0u : t->bytes_tx;
}

uint64_t iv_transport_bytes_rx(const iv_transport_t *t)
{
    return (t == NULL) ? 0u : t->bytes_rx;
}

uint64_t iv_transport_connects(const iv_transport_t *t)
{
    return (t == NULL) ? 0u : t->connects;
}

uint64_t iv_transport_drops(const iv_transport_t *t)
{
    return (t == NULL) ? 0u : t->drops;
}

uint64_t iv_transport_pump_oversize(const iv_transport_t *t)
{
    return (t == NULL) ? 0u : t->pump_oversize;
}
