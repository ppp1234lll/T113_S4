/*
 * iv_frame 单测（开发计划 M2-S2.2）
 *
 * 覆盖（计划 §S2.2"怎么验证"全项）：
 *   1) 正常帧：六个命令字 build→feed 往返，cmd/len/data 逐字节核对；
 *   2) 粘包：两帧一次 feed；
 *   3) 半包跨 feed：逐字节喂到极限；
 *   4) 噪声重同步：帧前插垃圾字节；
 *   5) 坏 CRC：整帧丢弃并计数，后续帧不受影响；
 *   6) 超长：len=0xFFFF 整帧丢弃，后续正常帧照常解出；
 *   7) 帧尾错：CRC 过但 end 非 0xFFFF（尾高/尾低两种）→ tail_err；
 *   8) 帧超时：tick 越过 gap_ms 复位计数；gap_ms=0 时关闭；
 *   9) build 错误路径：EVAL / ERANGE / ENOSPC；
 *  10) 满载 data（1024B）与 len=0 空载荷；
 *  11) init/feed/tick 的 NULL 与非法入参不炸；
 *  12) 连续同步字节流（0F 0F 0F 0F ...）不死锁、不越界；
 *  13) 统计计数综合核对。
 *
 * 手工组帧 helper 独立于 iv_frame_build 实现（同表不同码），防止
 * "build 和 parser 犯同一个错、往返测试互相掩盖"。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_frame.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---- 回调侧收集（拷贝 data，回调返回后视图即失效） ---- */
#define MAX_GOT 16
static struct {
    uint8_t cmd;
    uint16_t len;
    uint8_t data[IV_FRAME_DATA_MAX];
} g_got[MAX_GOT];
static int g_got_n;

static void on_frame(const iv_frame_t *fr, void *arg)
{
    (void)arg;
    if (g_got_n < MAX_GOT) {
        g_got[g_got_n].cmd = fr->cmd;
        g_got[g_got_n].len = fr->len;
        if (fr->len != 0)
            memcpy(g_got[g_got_n].data, fr->data, fr->len);
        g_got_n++;
    }
}

/* ---- 独立组帧（不用被测的 iv_frame_build） ---- */
static size_t mk(uint8_t *out, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t crc;
    size_t i;

    out[0] = 0x0F;
    out[1] = 0x0F;
    out[2] = cmd;
    out[3] = (uint8_t)(len & 0xFF); /* 小端：单片机源码 com.c 确认 */
    out[4] = (uint8_t)(len >> 8);
    for (i = 0; i < len; i++)
        out[5 + i] = data[i];
    crc = iv_crc8(&out[2], 3, IV_CRC8_SEED_INIT);
    crc = iv_crc8(&out[5], len, crc);
    out[5 + len] = crc;
    out[6 + len] = 0xFF;
    out[7 + len] = 0xFF;
    return 8 + (size_t)len;
}

int main(void)
{
    static iv_framer_t fr;
    static uint8_t frame[IV_FRAME_MAX];
    static uint8_t big[IV_FRAME_DATA_MAX];
    uint8_t data[32];
    size_t n, i;
    int round;

    for (i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7 + 1);
    for (i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i & 0xFF);

    /* ---- 1) 六命令字往返（用被测 build 组帧、独立 mk 交叉验证） ---- */
    {
        static const uint8_t cmds[6] = {
            IV_FRAME_CMD_QUERY, IV_FRAME_CMD_THRESH, IV_FRAME_CMD_POWER,
            IV_FRAME_CMD_REBOOT, IV_FRAME_CMD_DOOR, IV_FRAME_CMD_EVENT,
        };
        chk(iv_framer_init(&fr, IV_FRAME_HEAD_UP, 1000) == IV_OK, "init ok");
        g_got_n = 0;
        for (round = 0; round < 6; round++) {
            uint16_t dlen = (uint16_t)(round + 1);
            int bn = iv_frame_build(IV_FRAME_HEAD_UP, cmds[round], data, dlen,
                                    frame, sizeof(frame));
            chk(bn == (int)(IV_FRAME_OVERHEAD + dlen), "build len ok");
            /* build 与独立组帧逐字节一致（两种组帧互为交叉验证） */
            {
                uint8_t ref[64];
                size_t rn = mk(ref, cmds[round], data, dlen);
                chk(memcmp(frame, ref, rn) == 0, "build == independent mk");
            }
            iv_framer_feed(&fr, frame, (size_t)bn, 100 + (uint32_t)round,
                           on_frame, NULL);
        }
        chk(g_got_n == 6, "6 frames delivered");
        for (round = 0; round < g_got_n; round++) {
            chk(g_got[round].cmd == cmds[round], "roundtrip cmd");
            chk(g_got[round].len == (uint16_t)(round + 1), "roundtrip len");
            chk(memcmp(g_got[round].data, data, (size_t)(round + 1)) == 0,
                "roundtrip data");
        }
        chk(iv_framer_stats(&fr)->frames == 6, "stats frames == 6");
        chk(iv_framer_stats(&fr)->crc_err == 0 && iv_framer_stats(&fr)->noise == 0 &&
                iv_framer_stats(&fr)->too_long == 0 && iv_framer_stats(&fr)->tail_err == 0,
            "stats no errors");
    }

    /* ---- 2) 逐字节喂（半包最细切分） ---- */
    {
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 0);
        g_got_n = 0;
        n = mk(frame, IV_FRAME_CMD_QUERY, data, 1);
        for (i = 0; i < n; i++)
            iv_framer_feed(&fr, &frame[i], 1, 200, on_frame, NULL);
        chk(g_got_n == 1 && g_got[0].cmd == IV_FRAME_CMD_QUERY &&
                g_got[0].len == 1 && g_got[0].data[0] == data[0],
            "byte-by-byte reassembly");
    }

    /* ---- 3) 粘包：两帧一次 feed ---- */
    {
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 0);
        g_got_n = 0;
        {
            uint8_t two[2 * IV_FRAME_MAX];
            size_t n1 = mk(two, IV_FRAME_CMD_DOOR, data, 1);
            size_t n2 = mk(two + n1, IV_FRAME_CMD_EVENT, data, 20);
            iv_framer_feed(&fr, two, n1 + n2, 300, on_frame, NULL);
            chk(g_got_n == 2 && g_got[0].cmd == IV_FRAME_CMD_DOOR &&
                    g_got[1].cmd == IV_FRAME_CMD_EVENT && g_got[1].len == 20,
                "back-to-back frames in one feed");
        }
    }

    /* ---- 4) 前置噪声 ---- */
    {
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 0);
        g_got_n = 0;
        n = mk(frame, IV_FRAME_CMD_REBOOT, data, 1);
        {
            uint8_t noisy[8 + IV_FRAME_MAX];
            noisy[0] = 0x12;
            noisy[1] = 0x34;
            noisy[2] = 0x0F; /* 半个同步字后断掉（该字节本身不计噪声） */
            noisy[3] = 0xAB;
            memcpy(noisy + 4, frame, n);
            iv_framer_feed(&fr, noisy, 4 + n, 400, on_frame, NULL);
            chk(g_got_n == 1 && g_got[0].cmd == IV_FRAME_CMD_REBOOT, "resync after noise");
            chk(iv_framer_stats(&fr)->noise == 3, "noise counted == 3 (12/34/AB)");
        }
    }

    /* ---- 5) 坏 CRC ---- */
    {
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 0);
        g_got_n = 0;
        n = mk(frame, IV_FRAME_CMD_THRESH, data, 18);
        frame[5 + 18] ^= 0xFF; /* 破坏 CRC 字节 */
        iv_framer_feed(&fr, frame, n, 500, on_frame, NULL);
        chk(g_got_n == 0, "bad crc not delivered");
        chk(iv_framer_stats(&fr)->crc_err == 1, "crc_err counted");
        /* 后续正常帧照常 */
        n = mk(frame, IV_FRAME_CMD_QUERY, data, 1);
        iv_framer_feed(&fr, frame, n, 501, on_frame, NULL);
        chk(g_got_n == 1, "frame after bad crc ok");
    }

    /* ---- 6) 超长（len=0xFFFF）---- */
    {
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 0);
        g_got_n = 0;
        frame[0] = 0x0F;
        frame[1] = 0x0F;
        frame[2] = IV_FRAME_CMD_EVENT;
        frame[3] = 0xFF; /* len = 0xFFFF */
        frame[4] = 0xFF;
        iv_framer_feed(&fr, frame, 5, 600, on_frame, NULL);
        chk(iv_framer_stats(&fr)->too_long == 1, "too_long counted");
        n = mk(frame, IV_FRAME_CMD_QUERY, data, 1);
        iv_framer_feed(&fr, frame, n, 601, on_frame, NULL);
        chk(g_got_n == 1, "frame after too_long ok");
    }

    /* ---- 7) 帧尾错（两种位置） ---- */
    {
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 0);
        g_got_n = 0;
        n = mk(frame, IV_FRAME_CMD_POWER, data, 2);
        frame[n - 1] = 0x00; /* 尾低字节错 */
        iv_framer_feed(&fr, frame, n, 700, on_frame, NULL);
        chk(g_got_n == 0, "tail1 err not delivered");
        n = mk(frame, IV_FRAME_CMD_POWER, data, 2);
        frame[n - 2] = 0x00; /* 尾高字节错 */
        iv_framer_feed(&fr, frame, n, 701, on_frame, NULL);
        chk(g_got_n == 0, "tail0 err not delivered");
        chk(iv_framer_stats(&fr)->tail_err == 2, "tail_err counted == 2");
    }

    /* ---- 8) 帧超时 ---- */
    {
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 1000);
        n = mk(frame, IV_FRAME_CMD_QUERY, data, 1);
        iv_framer_feed(&fr, frame, 4, 800, on_frame, NULL); /* 喂半截 */
        iv_framer_tick(&fr, 800 + 999);                      /* 未到阈值 */
        chk(iv_framer_stats(&fr)->resets == 0, "no reset before gap");
        iv_framer_tick(&fr, 800 + 1000); /* 到阈值 */
        chk(iv_framer_stats(&fr)->resets == 1, "reset at gap");
        /* 复位后余下的半截当噪声，下一帧完整可解 */
        g_got_n = 0;
        iv_framer_feed(&fr, frame + 4, n - 4, 2000, on_frame, NULL);
        n = mk(frame, IV_FRAME_CMD_DOOR, data, 1);
        iv_framer_feed(&fr, frame, n, 2001, on_frame, NULL);
        chk(g_got_n == 1 && g_got[0].cmd == IV_FRAME_CMD_DOOR,
            "new frame after timeout reset");
        /* gap_ms=0 关闭 */
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 0);
        iv_framer_feed(&fr, frame, 3, 3000, on_frame, NULL);
        iv_framer_tick(&fr, 99999999);
        chk(iv_framer_stats(&fr)->resets == 0, "gap 0 disables timeout");
    }

    /* ---- 9) build 错误路径 ---- */
    {
        uint8_t out[IV_FRAME_MAX];
        chk(iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY, data, 1,
                           NULL, sizeof(out)) == IV_EINVAL, "build NULL out");
        chk(iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY, NULL, 1,
                           out, sizeof(out)) == IV_EINVAL, "build NULL data");
        chk(iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_EVENT, big,
                           (uint16_t)(IV_FRAME_DATA_MAX + 1), out,
                           sizeof(out)) == IV_ERANGE, "build data too big");
        chk(iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY, data, 1,
                           out, 7) == IV_ENOSPC, "build out too small");
        chk(iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY, NULL, 0,
                           out, IV_FRAME_OVERHEAD) == (int)IV_FRAME_OVERHEAD,
            "build exact fit (len=0)");
    }

    /* ---- 10) 满载与空载荷 ---- */
    {
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 0);
        g_got_n = 0;
        n = mk(frame, IV_FRAME_CMD_EVENT, big, IV_FRAME_DATA_MAX);
        iv_framer_feed(&fr, frame, n, 900, on_frame, NULL);
        chk(g_got_n == 1 && g_got[0].len == IV_FRAME_DATA_MAX &&
                memcmp(g_got[0].data, big, IV_FRAME_DATA_MAX) == 0,
            "max-size frame");
        n = mk(frame, IV_FRAME_CMD_QUERY, NULL, 0);
        iv_framer_feed(&fr, frame, n, 901, on_frame, NULL);
        chk(g_got_n == 2 && g_got[1].len == 0, "zero-length payload");
    }

    /* ---- 11) 非法入参不炸 ---- */
    {
        chk(iv_framer_init(NULL, IV_FRAME_HEAD_UP, 0) == IV_EINVAL, "init NULL");
        iv_framer_feed(NULL, frame, 8, 0, on_frame, NULL);
        iv_framer_feed(&fr, NULL, 0, 0, on_frame, NULL); /* 合法空喂 */
        iv_framer_feed(&fr, NULL, 4, 0, on_frame, NULL); /* NULL 且 len!=0 忽略 */
        iv_framer_tick(NULL, 0);
        chk(iv_framer_stats(NULL) == NULL, "stats NULL");
    }

    /* ---- 12) 连续同步字节流：0F 流被按"每 5 字节一帧"切成 cmd=0x0F、
     * len=0x0F0F 的超长候选帧丢弃（同步字两字节相同的固有歧义，真实帧
     * cmd 不会是 0x0F）—— 不死锁、不越界、计数正确；残留的半截帧由帧超时
     * 复位兜底（否则后续正常帧会被当作它的 data 吞掉），之后正常帧照常解出 ---- */
    {
        uint8_t syncs[64];
        memset(syncs, 0x0F, sizeof(syncs));
        iv_framer_init(&fr, IV_FRAME_HEAD_UP, 100);
        g_got_n = 0;
        iv_framer_feed(&fr, syncs, sizeof(syncs), 1000, on_frame, NULL);
        chk(g_got_n == 0, "sync flood delivers nothing");
        chk(iv_framer_stats(&fr)->too_long == 12, "0F0F flood -> 12 too-long drops");
        iv_framer_tick(&fr, 1200); /* 残留在 LEN_L，帧超时兜底复位 */
        chk(iv_framer_stats(&fr)->resets == 1, "flood leftover reset by tick");
        n = mk(frame, IV_FRAME_CMD_QUERY, data, 1);
        iv_framer_feed(&fr, frame, n, 1300, on_frame, NULL);
        chk(g_got_n == 1, "frame after sync flood ok");
    }

    /* ---- 13) reset 清零 ---- */
    {
        iv_framer_reset(&fr);
        chk(iv_framer_stats(&fr)->frames == 0 && iv_framer_stats(&fr)->noise == 0,
            "reset clears stats");
        iv_framer_reset(NULL); /* 不炸 */
    }

    if (g_fail == 0)
        printf("test_frame passed (sync/sticky/half/noise/crc/toolong/tail/"
               "timeout/build/errors/maxlen/null)\n");
    return g_fail == 0 ? 0 : 1;
}
