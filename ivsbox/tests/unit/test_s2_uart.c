/* S2 UART 装配单测：假串口与假 Reactor 驱动真实 link/status。 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_frame.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_s2_uart.h"
#include "ivsbox/iv_serial.h"

static int failures;
static uint64_t fake_now = 1000;
static iv_event_fn event_cb;
static void *event_arg;
static iv_timer_fn timer_cb;
static void *timer_arg;
static uint8_t read_buf[IV_FRAME_MAX];
static size_t read_len;
static uint8_t written[IV_FRAME_MAX];
static size_t write_len;
static int write_eagain_once;
static size_t write_limit_once;
static int open_count;
static int close_count;
static int event_token, timer_token, reactor_token;

static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

uint64_t iv_clock_monotonic_ms(void) { return fake_now; }

iv_event_t *iv_reactor_add(iv_reactor_t *r, int fd, uint32_t events,
                           iv_event_fn cb, void *arg)
{
    (void)r; (void)fd; (void)events;
    event_cb = cb;
    event_arg = arg;
    return (iv_event_t *)&event_token;
}

int iv_reactor_mod(iv_reactor_t *r, iv_event_t *ev, uint32_t events)
{
    (void)r; (void)ev; (void)events;
    return IV_OK;
}

int iv_reactor_del(iv_reactor_t *r, iv_event_t *ev)
{
    (void)r; (void)ev;
    event_cb = NULL;
    return IV_OK;
}

iv_timer_t *iv_timer_add(iv_reactor_t *r, uint32_t timeout_ms,
                         iv_timer_fn cb, void *arg)
{
    (void)r; (void)timeout_ms;
    timer_cb = cb;
    timer_arg = arg;
    return (iv_timer_t *)&timer_token;
}

int iv_timer_cancel(iv_reactor_t *r, iv_timer_t *t)
{
    (void)r; (void)t;
    timer_cb = NULL;
    return IV_OK;
}

int iv_serial_open(const char *path, const iv_serial_cfg_t *cfg)
{
    (void)cfg;
    check(strcmp(path, IV_S2_UART_PATH_DEFAULT) == 0, "default UART path");
    open_count++;
    return 7;
}

int iv_serial_close(int fd)
{
    check(fd == 7, "close expected serial fd");
    close_count++;
    return IV_OK;
}

int iv_serial_read(int fd, void *buf, size_t cap, size_t *got)
{
    (void)fd;
    if (read_len == 0)
        return IV_EAGAIN;
    if (read_len > cap)
        return IV_ERANGE;
    memcpy(buf, read_buf, read_len);
    *got = read_len;
    read_len = 0;
    return IV_OK;
}

int iv_serial_write(int fd, const void *buf, size_t len)
{
    if (write_eagain_once) {
        write_eagain_once = 0;
        return IV_EAGAIN;
    }
    if (write_limit_once != 0 && len > write_limit_once) {
        len = write_limit_once;
        write_limit_once = 0;
    }
    check(fd == 7 && write_len + len <= sizeof(written), "serial write bounds");
    if (write_len + len > sizeof(written))
        return IV_EIO;
    memcpy(written + write_len, buf, len);
    write_len += len;
    return (int)len;
}

int main(void)
{
    static iv_s2_uart_t ctx;
    iv_reactor_t *reactor = (iv_reactor_t *)&reactor_token;
    const iv_status_t *st;
    const char *json = "{\"V\":220.5,\"DS\":1}";
    const uint8_t zero = 0;
    uint8_t expected[IV_FRAME_MAX];
    int n;

    check(iv_s2_uart_start(&ctx, reactor, NULL) == IV_OK, "start");
    check(open_count == 1 && event_cb != NULL && timer_cb != NULL, "attached");
    n = iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY,
                       &zero, 1, expected, sizeof(expected));
    write_eagain_once = 1;
    write_limit_once = 3;
    event_cb(7, IV_EV_WRITE, event_arg);
    check(write_len == 0, "EAGAIN retains queued frame");
    event_cb(7, IV_EV_WRITE, event_arg);
    check(write_len == 3, "partial write retains remaining bytes");
    event_cb(7, IV_EV_WRITE, event_arg);
    check(n > 0 && write_len == (size_t)n &&
          memcmp(written, expected, (size_t)n) == 0, "initial E1 query sent");

    n = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY,
                       json, (uint16_t)strlen(json), read_buf, sizeof(read_buf));
    check(n > 0, "build upstream reply");
    read_len = (size_t)n;
    event_cb(7, IV_EV_READ, event_arg);
    st = iv_s2_uart_status(&ctx);
    check(st != NULL && st->valid.V && st->data.V == 220.5f &&
          st->valid.DS && st->data.DS == 1, "reply updates mirror");

    event_cb(7, IV_EV_HUP, event_arg);
    st = iv_s2_uart_status(&ctx);
    check(close_count == 1 && st != NULL && !st->valid.V,
          "disconnect clears stale mirror");
    fake_now += 1000;
    timer_cb(timer_arg);
    check(open_count == 2 && event_cb != NULL, "timer reconnects");
    iv_s2_uart_stop(&ctx);
    check(close_count == 2 && timer_cb == NULL &&
          iv_s2_uart_status(&ctx) == NULL, "stop releases resources");

    if (failures == 0)
        puts("test_s2_uart passed (queue-partial/reply/disconnect/reconnect/stop)");
    return failures ? 1 : 0;
}
