/*
 * iv_frame 模糊测试（开发计划 M2-S2.2：随机字节流灌 10 分钟无 crash / 挂死）
 *
 * 用法：fuzz_frame [秒数]（默认 10；0 表示只跑一轮自检）
 *
 * 字节流来源：xorshift64* 伪随机 + 按概率混入合法帧（提高同步字/CRC 路径
 * 命中率，纯均匀随机几乎打不进 DATA 之后的状态）。回调里访问 data 边界
 * （首尾字节），ASan/UBSan 下越界立即暴露。每块随机 now_ms 喂 tick，
 * 覆盖超时路径。运行时长由 wall clock 控制，结束打印统计。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ivsbox/iv_frame.h"

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

static void on_frame(const iv_frame_t *fr, void *arg)
{
    volatile uint8_t sink = 0;
    (void)arg;
    if (fr->len != 0) {
        sink ^= fr->data[0];
        sink ^= fr->data[fr->len - 1];
    }
    (void)sink;
}

int main(int argc, char **argv)
{
    double budget = argc > 1 ? atof(argv[1]) : 10.0;
    double t0 = now_sec();
    static iv_framer_t fr;
    static uint8_t chunk[4096];
    static uint8_t good[IV_FRAME_MAX];
    static uint8_t payload[128];
    const iv_frame_stats_t *st;
    uint64_t fed = 0;
    size_t i;

    for (i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i & 0xFF);

    /* 播种：xorshift 全零态会永远输出 0，必须保证非零 */
    g_rs = (uint64_t)time(NULL) * 2654435761ULL + 0x9E3779B97F4A7C15ULL;
    if (g_rs == 0)
        g_rs = 1;

    iv_framer_init(&fr, IV_FRAME_HEAD_UP, 500);

    while (now_sec() - t0 < budget) {
        uint64_t r = rs_next();
        size_t n = 0;

        if ((r & 7u) == 0) { /* 1/8 概率插入合法帧 */
            uint16_t dlen = (uint16_t)(rs_next() % (sizeof(payload) + 1));
            int bn = iv_frame_build(IV_FRAME_HEAD_UP,
                                    (uint8_t)(rs_next() & 0xFF), payload, dlen,
                                    good, sizeof(good));
            if (bn > 0) {
                memcpy(chunk, good, (size_t)bn);
                n = (size_t)bn;
            }
        }
        while (n < sizeof(chunk))
            chunk[n++] = (uint8_t)rs_next();

        iv_framer_feed(&fr, chunk, sizeof(chunk), (uint32_t)(fed & 0x7FFFFFFFu),
                       on_frame, NULL);
        if ((rs_next() & 15u) == 0)
            iv_framer_tick(&fr, (uint32_t)((fed + 600) & 0x7FFFFFFFu));
        fed += sizeof(chunk);
    }

    st = iv_framer_stats(&fr);
    printf("fuzz_frame done: %.1fs, %llu bytes\n", now_sec() - t0,
           (unsigned long long)fed);
    printf("  frames=%u crc_err=%u tail_err=%u too_long=%u noise=%u resets=%u\n",
           st->frames, st->crc_err, st->tail_err, st->too_long, st->noise,
           st->resets);
    return 0;
}
