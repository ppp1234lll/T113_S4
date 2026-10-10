/*
 * iv_link 模糊测试（M2-S2.3：随机事件序列无 crash / 挂死）
 *
 * 用法：fuzz_link [秒数]（默认 10；0 只跑一轮自检）
 *
 * 事件流：xorshift64* 随机选择 recv（随机块＋1/8 概率混入合法帧）、
 * send（随机 cmd/长度）、tick（随机推进 0~3s）。回调里访问 data 边界，
 * ASan/UBSan 下越界立即暴露。时长由 wall clock 控制。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ivsbox/iv_link.h"
#include "ivsbox/iv_ret.h"

static uint64_t g_rs;

static uint64_t rs_next(void)
{
    g_rs ^= g_rs >> 12;
    g_rs ^= g_rs << 25;
    g_rs ^= g_rs >> 27;
    return g_rs * 2685821657736338717ULL;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int on_tx(const uint8_t *b, size_t len, void *arg)
{
    volatile uint8_t sink = 0;
    (void)arg;
    while (len--)
        sink ^= *b++;
    (void)sink;
    return IV_OK;
}

static void on_done(int rc, uint8_t cmd, const uint8_t *data, uint16_t len, void *arg)
{
    volatile uint8_t sink = 0;
    (void)arg;
    sink ^= (uint8_t)rc ^ cmd;
    if (data != NULL)
        while (len--)
            sink ^= *data++;
    (void)sink;
}

static void on_up(uint8_t cmd, const uint8_t *data, uint16_t len, void *arg)
{
    volatile uint8_t sink = cmd;
    (void)arg;
    if (data != NULL)
        while (len--)
            sink ^= *data++;
    (void)sink;
}

static void on_chg(int up, void *arg)
{
    (void)arg;
    (void)up;
}

int main(int argc, char **argv)
{
    double budget = argc > 1 ? atof(argv[1]) : 10.0;
    double t0 = now_sec();
    static iv_link_t lk;
    static uint8_t chunk[2048];
    static iv_link_cfg_t cfg = { 100, 1, 3 };
    unsigned long ev = 0;
    uint32_t clock_ms = 0;

    g_rs = (uint64_t)time(NULL) * 2654435761ULL + 0x9E3779B97F4A7C15ULL;
    if (g_rs == 0)
        g_rs = 1;

    if (iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL) != IV_OK)
        return 1;

    while (now_sec() - t0 < budget) {
        uint64_t r = rs_next();
        size_t n = 0;

        if ((r & 3u) == 0) { /* 1/4：recv 随机块（1/8 再混一帧合法应答） */
            while (n < sizeof(chunk))
                chunk[n++] = (uint8_t)rs_next();
            if ((rs_next() & 7u) == 0) {
                uint8_t f[IV_FRAME_MAX];
                uint8_t payload[24];
                uint16_t plen = (uint16_t)(rs_next() % (sizeof(payload) + 1));
                static const uint8_t cmds[6] = { 0xE1, 0xF1, 0xD1, 0xD2, 0xC1, 0xC2 };
                uint8_t cmd = cmds[rs_next() % 6];
                uint16_t k;
                for (k = 0; k < plen; k++)
                    payload[k] = (uint8_t)rs_next();
                if (iv_frame_build(IV_FRAME_HEAD_UP, cmd, payload, plen,
                                   f, sizeof(f)) > 0) {
                    size_t cut = (size_t)(rs_next() % 64);
                    if (cut > sizeof(chunk) - IV_FRAME_MAX)
                        cut = 0;
                    memcpy(chunk + cut, f, IV_FRAME_OVERHEAD + plen);
                }
            }
            iv_link_recv(&lk, chunk, sizeof(chunk), clock_ms);
        } else if ((r & 3u) == 1) { /* send */
            static const uint8_t cmds[4] = { 0xE1, 0xF1, 0xD1, 0xD2 };
            uint8_t payload[32];
            uint16_t plen = (uint16_t)(rs_next() % (sizeof(payload) + 1));
            uint16_t k;
            for (k = 0; k < plen; k++)
                payload[k] = (uint8_t)rs_next();
            (void)iv_link_send(&lk, cmds[rs_next() % 4], payload, plen,
                               clock_ms, on_done, NULL);
        } else { /* tick 推进 0~3000ms */
            clock_ms += (uint32_t)(rs_next() % 3001);
            iv_link_tick(&lk, clock_ms);
        }
        ev++;
    }

    printf("fuzz_link done: %.1fs, %lu events, link_up=%d\n", now_sec() - t0,
           ev, iv_link_is_up(&lk));
    return 0;
}
