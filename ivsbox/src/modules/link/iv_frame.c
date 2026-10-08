/*
 * 采集板 UART 帧成帧 / 编码实现（功能开发计划 M2-S2.2）
 *
 * 帧规格与设计口径见 include/ivsbox/iv_frame.h 文件头与架构 §18.2。
 *
 * 状态机：SEARCH0 → SEARCH1 → CMD → LEN_H → LEN_L → DATA → CRC → TAIL0 → TAIL1
 *   - 任何阶段失配都回到同步字搜索（超长/坏 CRC 的长度域不可信，不做精确跳过）；
 *   - CRC 在 DATA 阶段逐字节累计（分段 seed 语义见 iv_crc.h），收完即比对；
 *   - 零 malloc：framer 结构由调用方持有，data 缓冲内嵌。
 */
#include <string.h>

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_frame.h"
#include "ivsbox/iv_ret.h"

enum {
    ST_SEARCH0 = 0, /* 等同步字高字节 */
    ST_SEARCH1,     /* 等同步字低字节 */
    ST_CMD,         /* 等命令字 */
    ST_LEN_H,       /* 等长度高字节 */
    ST_LEN_L,       /* 等长度低字节 */
    ST_DATA,        /* 收 data */
    ST_CRC,         /* 等 CRC */
    ST_TAIL0,       /* 等帧尾 0xFF */
    ST_TAIL1,       /* 等帧尾 0xFF */
};

int iv_framer_init(iv_framer_t *fr, uint16_t sync_head, uint32_t gap_ms)
{
    if (fr == NULL)
        return IV_EINVAL;
    memset(fr, 0, sizeof(*fr));
    fr->sync_head = sync_head;
    fr->gap_ms = gap_ms;
    return IV_OK;
}

void iv_framer_reset(iv_framer_t *fr)
{
    if (fr == NULL)
        return;
    fr->state = ST_SEARCH0;
    fr->cmd = 0;
    fr->need = 0;
    fr->got = 0;
    fr->crc = 0;
    memset(&fr->stats, 0, sizeof(fr->stats));
}

static void goto_search(iv_framer_t *fr)
{
    fr->state = ST_SEARCH0;
    fr->cmd = 0;
    fr->need = 0;
    fr->got = 0;
    fr->crc = 0;
}

void iv_framer_feed(iv_framer_t *fr, const uint8_t *buf, size_t len,
                    uint32_t now_ms, iv_frame_cb cb, void *arg)
{
    size_t i;

    if (fr == NULL || (buf == NULL && len != 0))
        return;
    fr->last_ms = now_ms;

    for (i = 0; i < len; i++) {
        uint8_t b = buf[i];

        switch (fr->state) {
        case ST_SEARCH0:
            if (b == (uint8_t)(fr->sync_head >> 8))
                fr->state = ST_SEARCH1;
            else
                fr->stats.noise++;
            break;

        case ST_SEARCH1:
            if (b == (uint8_t)(fr->sync_head & 0xFF)) {
                fr->state = ST_CMD;
            } else if (b == (uint8_t)(fr->sync_head >> 8)) {
                /* 连续同步字节（如 0F 0F 0F）：留在本态，等低字节 */
                fr->stats.noise++;
            } else {
                fr->stats.noise++;
                fr->state = ST_SEARCH0;
            }
            break;

        case ST_CMD:
            fr->cmd = b;
            fr->crc = iv_crc8(&b, 1, IV_CRC8_SEED_INIT);
            fr->state = ST_LEN_H;
            break;

        case ST_LEN_H:
            fr->need = b; /* 小端：高字节在后 */
            fr->crc = iv_crc8(&b, 1, fr->crc);
            fr->state = ST_LEN_L;
            break;

        case ST_LEN_L:
            fr->need = (uint16_t)(fr->need | ((uint16_t)b << 8));
            fr->crc = iv_crc8(&b, 1, fr->crc);
            if (fr->need > IV_FRAME_DATA_MAX) {
                fr->stats.too_long++;
                goto_search(fr); /* 长度域不可信，回同步搜索 */
            } else {
                /* len=0 的帧没有 data 段，直接等 CRC —— 若照常进 DATA 态，
                 * got==need 永不成立，会把 CRC/帧尾全部吞成 data（单测抓过） */
                fr->got = 0;
                fr->state = (fr->need == 0) ? ST_CRC : ST_DATA;
            }
            break;

        case ST_DATA:
            fr->crc = iv_crc8(&b, 1, fr->crc);
            fr->buf[fr->got++] = b;
            if (fr->got == fr->need)
                fr->state = ST_CRC;
            break;

        case ST_CRC:
            if (b != fr->crc) {
                fr->stats.crc_err++;
                goto_search(fr);
            } else {
                fr->state = ST_TAIL0;
            }
            break;

        case ST_TAIL0:
            if (b == 0xFF) {
                fr->state = ST_TAIL1;
            } else {
                fr->stats.tail_err++;
                goto_search(fr);
            }
            break;

        case ST_TAIL1:
            if (b == 0xFF) {
                fr->stats.frames++;
                if (cb != NULL) {
                    iv_frame_t view;
                    view.cmd = fr->cmd;
                    view.len = fr->need;
                    view.data = fr->buf;
                    cb(&view, arg);
                }
                goto_search(fr);
            } else {
                fr->stats.tail_err++;
                goto_search(fr);
            }
            break;

        default:
            fr->stats.noise++;
            goto_search(fr);
            break;
        }
    }
}

void iv_framer_tick(iv_framer_t *fr, uint32_t now_ms)
{
    if (fr == NULL || fr->gap_ms == 0)
        return;
    if (fr->state != ST_SEARCH0 && (uint32_t)(now_ms - fr->last_ms) >= fr->gap_ms) {
        fr->stats.resets++;
        goto_search(fr);
    }
}

const iv_frame_stats_t *iv_framer_stats(const iv_framer_t *fr)
{
    return fr == NULL ? NULL : &fr->stats;
}

int iv_frame_build(uint16_t head, uint8_t cmd, const void *data, uint16_t len,
                   uint8_t *out, size_t out_size)
{
    uint8_t crc;
    size_t total = (size_t)IV_FRAME_OVERHEAD + len;

    if (out == NULL || (data == NULL && len != 0))
        return IV_EINVAL;
    if (len > IV_FRAME_DATA_MAX)
        return IV_ERANGE;
    if (out_size < total)
        return IV_ENOSPC;

    out[0] = (uint8_t)(head >> 8);
    out[1] = (uint8_t)(head & 0xFF);
    out[2] = cmd;
    out[3] = (uint8_t)(len & 0xFF); /* 小端 */
    out[4] = (uint8_t)(len >> 8);
    if (len != 0)
        memcpy(&out[5], data, len);

    crc = iv_crc8(&out[2], 3, IV_CRC8_SEED_INIT); /* cmd + len */
    crc = iv_crc8(&out[5], len, crc);             /* data */
    out[5 + len] = crc;
    out[6 + len] = 0xFF;
    out[7 + len] = 0xFF;
    return (int)total;
}
