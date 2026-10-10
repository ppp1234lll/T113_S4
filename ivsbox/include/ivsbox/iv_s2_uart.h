/* M2 采集板 UART 装配：Reactor 单写者持有串口、可靠层和状态镜像。 */
#ifndef IVSBOX_IV_S2_UART_H
#define IVSBOX_IV_S2_UART_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_link.h"
#include "ivsbox/iv_reactor.h"
#include "ivsbox/iv_status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IV_S2_UART_PATH_DEFAULT "/dev/ttySAC5"
#define IV_S2_UART_TX_SLOTS 4

typedef struct {
    uint8_t bytes[IV_FRAME_MAX];
    size_t len;
} iv_s2_uart_tx_t;

typedef struct {
    iv_reactor_t *reactor;
    iv_event_t *serial_ev;
    iv_timer_t *tick_timer;
    int fd;
    int started;
    int query_pending;
    uint32_t last_query_ms;
    char path[128];
    iv_s2_uart_tx_t tx[IV_S2_UART_TX_SLOTS];
    unsigned tx_head;
    unsigned tx_count;
    size_t tx_offset;
    iv_link_t link;
    iv_status_t status;
} iv_s2_uart_t;

/* 串口不可用时保持降级运行并定时重试；只发送 0xE1 查询，不下发控制命令。 */
int iv_s2_uart_start(iv_s2_uart_t *ctx, iv_reactor_t *reactor, const char *path);
void iv_s2_uart_stop(iv_s2_uart_t *ctx);
const iv_status_t *iv_s2_uart_status(const iv_s2_uart_t *ctx);

#ifdef __cplusplus
}
#endif
#endif
