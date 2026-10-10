/* 采集板 UART 装配。所有入口均由 ivsboxd 的 Reactor 单线程调用。 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_frame.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_s2_uart.h"
#include "ivsbox/iv_serial.h"

#define S2_TICK_MS 1000u
#define S2_QUERY_INTERVAL_MS 30000u

static void on_serial(int fd, uint32_t events, void *arg);
static void on_tick(void *arg);

static uint32_t monotonic_ms(void)
{
    return (uint32_t)iv_clock_monotonic_ms();
}

static void detach_serial(iv_s2_uart_t *ctx)
{
    if (ctx->serial_ev != NULL) {
        (void)iv_reactor_del(ctx->reactor, ctx->serial_ev);
        ctx->serial_ev = NULL;
    }
    if (ctx->fd >= 0) {
        (void)iv_serial_close(ctx->fd);
        ctx->fd = -1;
    }
    ctx->tx_count = 0;
    ctx->tx_head = 0;
    ctx->tx_offset = 0;
    iv_link_clear(&ctx->link, IV_ECONN);
    iv_link_reset(&ctx->link);
    ctx->query_pending = 0;
    iv_status_init(&ctx->status);
}

static int on_tx(const uint8_t *bytes, size_t len, void *arg)
{
    iv_s2_uart_t *ctx = arg;
    unsigned tail;
    int rc;

    if (ctx->fd < 0)
        return IV_ECONN;
    if (ctx->tx_count == IV_S2_UART_TX_SLOTS)
        return IV_EFULL;
    if (len == 0 || len > IV_FRAME_MAX)
        return IV_ERANGE;
    tail = (ctx->tx_head + ctx->tx_count) % IV_S2_UART_TX_SLOTS;
    memcpy(ctx->tx[tail].bytes, bytes, len);
    ctx->tx[tail].len = len;
    rc = iv_reactor_mod(ctx->reactor, ctx->serial_ev, IV_EV_READ | IV_EV_WRITE);
    if (rc != IV_OK)
        return rc;
    ctx->tx_count++;
    return IV_OK;
}

static void on_query_done(int rc, uint8_t cmd, const uint8_t *data,
                          uint16_t len, void *arg)
{
    iv_s2_uart_t *ctx = arg;
    ctx->query_pending = 0;
    if (rc == IV_OK && cmd == IV_FRAME_CMD_QUERY)
        iv_status_handle_query(&ctx->status, data, len);
}

static void send_query(iv_s2_uart_t *ctx)
{
    static const uint8_t zero = 0;
    uint32_t now = monotonic_ms();

    if (ctx->fd < 0 || ctx->query_pending)
        return;
    ctx->query_pending = 1;
    if (iv_link_send(&ctx->link, IV_FRAME_CMD_QUERY, &zero, 1,
                     now, on_query_done, ctx) != IV_OK)
        ctx->query_pending = 0;
    else
        ctx->last_query_ms = now;
}

static void on_upstream(uint8_t cmd, const uint8_t *data, uint16_t len, void *arg)
{
    iv_s2_uart_t *ctx = arg;
    iv_status_handle_upstream(cmd, data, len, &ctx->status);
}

static void on_change(int up, void *arg)
{
    if (up)
        send_query((iv_s2_uart_t *)arg);
}

static void attach_serial(iv_s2_uart_t *ctx)
{
    int fd = iv_serial_open(ctx->path, NULL);
    if (fd < 0)
        return;
    ctx->fd = fd;
    ctx->serial_ev = iv_reactor_add(ctx->reactor, fd, IV_EV_READ, on_serial, ctx);
    if (ctx->serial_ev == NULL) {
        detach_serial(ctx);
        return;
    }
    send_query(ctx);
}

static void flush_tx(iv_s2_uart_t *ctx)
{
    while (ctx->tx_count != 0) {
        iv_s2_uart_tx_t *item = &ctx->tx[ctx->tx_head];
        int n = iv_serial_write(ctx->fd, item->bytes + ctx->tx_offset,
                                item->len - ctx->tx_offset);
        if (n == IV_EAGAIN)
            return;
        if (n <= 0) {
            detach_serial(ctx);
            return;
        }
        ctx->tx_offset += (size_t)n;
        if (ctx->tx_offset < item->len)
            return;
        ctx->tx_head = (ctx->tx_head + 1u) % IV_S2_UART_TX_SLOTS;
        ctx->tx_count--;
        ctx->tx_offset = 0;
    }
    if (ctx->serial_ev != NULL)
        (void)iv_reactor_mod(ctx->reactor, ctx->serial_ev, IV_EV_READ);
}

static void on_serial(int fd, uint32_t events, void *arg)
{
    iv_s2_uart_t *ctx = arg;
    uint8_t buf[256];
    size_t got;
    int rc;

    if (events & (IV_EV_ERR | IV_EV_HUP | IV_EV_RDHUP)) {
        detach_serial(ctx);
        return;
    }
    if (events & IV_EV_READ) {
        for (;;) {
            rc = iv_serial_read(fd, buf, sizeof(buf), &got);
            if (rc == IV_EAGAIN)
                break;
            if (rc != IV_OK) {
                detach_serial(ctx);
                return;
            }
            iv_link_recv(&ctx->link, buf, got, monotonic_ms());
        }
    }
    if (ctx->fd >= 0 && (events & IV_EV_WRITE))
        flush_tx(ctx);
}

static void on_tick(void *arg)
{
    iv_s2_uart_t *ctx = arg;
    uint32_t now = monotonic_ms();
    ctx->tick_timer = NULL;
    if (ctx->fd < 0)
        attach_serial(ctx);
    else {
        iv_link_tick(&ctx->link, now);
        if (!ctx->query_pending &&
            (uint32_t)(now - ctx->last_query_ms) >= S2_QUERY_INTERVAL_MS)
            send_query(ctx);
    }
    if (ctx->started)
        ctx->tick_timer = iv_timer_add(ctx->reactor, S2_TICK_MS, on_tick, ctx);
}

int iv_s2_uart_start(iv_s2_uart_t *ctx, iv_reactor_t *reactor, const char *path)
{
    int n;
    int rc;
    if (ctx == NULL || reactor == NULL)
        return IV_EINVAL;
    if (path == NULL)
        path = IV_S2_UART_PATH_DEFAULT;
    n = snprintf(ctx->path, sizeof(ctx->path), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(ctx->path))
        return IV_ERANGE;
    memset(&ctx->tx, 0, sizeof(ctx->tx));
    ctx->reactor = reactor;
    ctx->serial_ev = NULL;
    ctx->tick_timer = NULL;
    ctx->fd = -1;
    ctx->started = 1;
    ctx->query_pending = 0;
    ctx->last_query_ms = 0;
    ctx->tx_head = ctx->tx_count = 0;
    ctx->tx_offset = 0;
    iv_status_init(&ctx->status);
    rc = iv_link_init(&ctx->link, NULL, on_tx, on_upstream, on_change, ctx);
    if (rc != IV_OK) {
        ctx->started = 0;
        return rc;
    }
    ctx->tick_timer = iv_timer_add(reactor, S2_TICK_MS, on_tick, ctx);
    if (ctx->tick_timer == NULL) {
        ctx->started = 0;
        return IV_EFULL;
    }
    attach_serial(ctx);
    return IV_OK;
}

void iv_s2_uart_stop(iv_s2_uart_t *ctx)
{
    if (ctx == NULL || !ctx->started)
        return;
    ctx->started = 0;
    if (ctx->tick_timer != NULL) {
        (void)iv_timer_cancel(ctx->reactor, ctx->tick_timer);
        ctx->tick_timer = NULL;
    }
    detach_serial(ctx);
}

const iv_status_t *iv_s2_uart_status(const iv_s2_uart_t *ctx)
{
    return ctx != NULL && ctx->started ? &ctx->status : NULL;
}
