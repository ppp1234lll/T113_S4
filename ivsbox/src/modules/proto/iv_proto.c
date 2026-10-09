/*
 * 平台协议层实现（M2-S2.4）
 *
 * 布局对照 iv_proto.h：
 *   - 文本帧组包 iv_proto_text_frame()
 *   - 二进制组包 iv_proto_bin_build()（静态 core_bin，供各发帧入口复用）
 *   - 解析器     iv_proto_parser_feed()（滑动窗口找头 0xF0F0，按位置取 LEN）
 *   - 协议上下文 iv_proto_recv()/ack()/heartbeat()/send/send_text()
 *
 * 设计依据：include/ivsbox/iv_proto.h 文件头（冻结口径）。
 * CRC 全部走 S3 的 iv_crc8（seed 0），与 MCU calc_crc8 同参。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_proto.h"

/* ---------------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------------- */

/* 帧布局偏移（下行帧，固定 21 字节 + DATA） */
#define OFF_HEAD   0
#define OFF_VER    2
#define OFF_TYPE   3
#define OFF_ID     5
#define OFF_CMD    8
#define OFF_QN1    9
#define OFF_QN2    13
#define OFF_LEN    17
#define OFF_DATA   18
/* OFF_DATA + LEN = CRC 偏移；CRC 后 2 字节帧尾 */

/* CRC 覆盖范围：version 起到 DATA 末尾（《指令-通用版》"crc（校验）"列） */
static uint8_t frame_crc(const uint8_t *buf, size_t total)
{
    /* total = OFF_DATA + len + 3（CRC+尾2）；CRC 覆盖 [OFF_VER, OFF_DATA+len) */
    size_t cover = total - 3u - OFF_VER;
    return iv_crc8(&buf[OFF_VER], cover, IV_CRC8_SEED_INIT);
}

/* ---------------------------------------------------------------------------
 * ## 文本帧
 * ------------------------------------------------------------------------- */
int iv_proto_text_frame(const char *seg, size_t seg_len,
                        uint8_t *out, size_t *out_len)
{
    size_t need;

    if (seg == NULL && seg_len != 0)
        return IV_EINVAL;
    if (seg_len > IV_PROTO_TXT_DATA_MAX)
        return IV_ERANGE;

    need = IV_PROTO_TXT_FIXED + seg_len; /* ## + 4 + seg + 2 + ## */
    if (out == NULL || *out_len == 0) {
        *out_len = need;
        return IV_OK;
    }
    if (*out_len < need) {
        *out_len = need;
        return IV_ERANGE;
    }

    /* 手工拼，避免 snprintf 依赖 locale 的 %04u 行为差异（两端一致优先） */
    out[0] = '#';
    out[1] = '#';
    out[2] = (uint8_t)('0' + (seg_len / 1000u) % 10u);
    out[3] = (uint8_t)('0' + (seg_len / 100u) % 10u);
    out[4] = (uint8_t)('0' + (seg_len / 10u) % 10u);
    out[5] = (uint8_t)('0' + seg_len % 10u);
    if (seg_len != 0)
        memcpy(&out[6], seg, seg_len);

    {
        static const char hex[] = "0123456789abcdef";
        uint8_t crc = iv_crc8(seg, seg_len, IV_CRC8_SEED_INIT);
        out[6 + seg_len]     = (uint8_t)hex[crc >> 4];
        out[6 + seg_len + 1] = (uint8_t)hex[crc & 0x0Fu];
    }
    out[6 + seg_len + 2] = '#';
    out[6 + seg_len + 3] = '#';

    *out_len = need;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 二进制组包
 * ------------------------------------------------------------------------- */
int iv_proto_bin_build(uint8_t *out, size_t out_cap, size_t *out_len,
                       uint16_t devtype, uint32_t devid, uint8_t cmd,
                       uint32_t qn1, uint32_t qn2,
                       const void *data, uint16_t len)
{
    size_t total;
    uint8_t crc;

    if (out_len == NULL)
        return IV_EINVAL;
    if (len > IV_PROTO_BIN_DATA_MAX)
        return IV_ERANGE;

    total = OFF_DATA + (size_t)len + 3u; /* +CRC +尾2 */
    *out_len = total;
    if (out == NULL || out_cap == 0)
        return IV_OK;
    if (out_cap < total)
        return IV_ERANGE;

    out[OFF_HEAD + 0] = (uint8_t)(IV_PROTO_HEAD_UP >> 8);
    out[OFF_HEAD + 1] = (uint8_t)(IV_PROTO_HEAD_UP & 0xFF);
    out[OFF_VER]      = IV_PROTO_VER;
    out[OFF_TYPE + 0] = (uint8_t)(devtype >> 8);
    out[OFF_TYPE + 1] = (uint8_t)(devtype & 0xFF);
    out[OFF_ID + 0]   = (uint8_t)(devid >> 16) & 0xFFu;
    out[OFF_ID + 1]   = (uint8_t)(devid >> 8) & 0xFFu;
    out[OFF_ID + 2]   = (uint8_t)(devid) & 0xFFu;
    out[OFF_CMD]      = cmd;
    out[OFF_QN1 + 0]  = (uint8_t)(qn1 >> 24);
    out[OFF_QN1 + 1]  = (uint8_t)(qn1 >> 16);
    out[OFF_QN1 + 2]  = (uint8_t)(qn1 >> 8);
    out[OFF_QN1 + 3]  = (uint8_t)(qn1);
    out[OFF_QN2 + 0]  = (uint8_t)(qn2 >> 24);
    out[OFF_QN2 + 1]  = (uint8_t)(qn2 >> 16);
    out[OFF_QN2 + 2]  = (uint8_t)(qn2 >> 8);
    out[OFF_QN2 + 3]  = (uint8_t)(qn2);
    out[OFF_LEN]      = (uint8_t)len;
    if (len != 0 && data != NULL)
        memcpy(&out[OFF_DATA], data, len);

    crc = frame_crc(out, total);
    out[OFF_DATA + len]      = crc;
    out[OFF_DATA + len + 1]  = (uint8_t)(IV_PROTO_TAIL >> 8);
    out[OFF_DATA + len + 2]  = (uint8_t)(IV_PROTO_TAIL & 0xFF);
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 解析器
 * ------------------------------------------------------------------------- */
void iv_proto_parser_init(iv_proto_parser_t *ps)
{
    if (ps == NULL)
        return;
    memset(ps, 0, sizeof(*ps));
}

static void parser_reset(iv_proto_parser_t *ps)
{
    ps->pos = 0;
    ps->tail = 0;
    ps->want = 0;
    ps->got_head = 0;
}

void iv_proto_parser_feed(iv_proto_parser_t *ps, const uint8_t *buf, size_t len,
                          void (*on_frame)(const iv_proto_frame_t *f, void *arg),
                          void *arg)
{
    size_t i;

    if (ps == NULL || buf == NULL)
        return;

    for (i = 0; i < len; i++) {
        uint8_t d = buf[i];

        /* 帧头滑动窗口：未锁定前只找 0xF0F0 */
        ps->head = (uint16_t)((ps->head << 8) | d);
        if (!ps->got_head) {
            if (ps->head != (uint16_t)IV_PROTO_HEAD_DOWN)
                continue;
            ps->got_head = 1;
            ps->pos = 0;
            ps->tail = 0;
            ps->want = 0;
            /* 预写帧头第一字节；当前字节 d 本身就是帧头第二字节，
             * 由下面统一的 buf[pos++]=d 落进缓冲——OFF_* 偏移与协议布局一致 */
            ps->buf[ps->pos++] = (uint8_t)(IV_PROTO_HEAD_DOWN >> 8);
        }

        if (ps->pos >= sizeof(ps->buf)) {
            /* 超出缓冲（LEN 撒谎的坏帧）：整帧丢弃回同步 */
            parser_reset(ps);
            continue;
        }
        ps->buf[ps->pos++] = d;
        ps->tail = (uint16_t)((ps->tail << 8) | d);

        /* LEN 字段到达 → 记录 DATA 期望长度 */
        if (ps->pos == OFF_LEN + 1u)
            ps->want = d;

        /* 完整帧判定：固定部分 + DATA + CRC + 尾2 */
        {
            uint16_t total = (uint16_t)(OFF_DATA + ps->want + 3u);
            if (ps->pos < total)
                continue;

            /* 校验 CRC */
            {
                uint8_t crc = frame_crc(ps->buf, ps->pos);
                if (crc != ps->buf[OFF_DATA + ps->want]) {
                    /* CRC 错：整帧丢弃（pos 复位重新找头） */
                    parser_reset(ps);
                    continue;
                }
            }

            /* 帧尾必须在最后两字节（帧内其它位置出现 0xFFFF 不管——LEN 已定界） */
            if (on_frame != NULL) {
                iv_proto_frame_t f;
                f.devtype = (uint16_t)(((uint16_t)ps->buf[OFF_TYPE] << 8) |
                                        ps->buf[OFF_TYPE + 1]);
                f.devid = ((uint32_t)ps->buf[OFF_ID] << 16) |
                          ((uint32_t)ps->buf[OFF_ID + 1] << 8) |
                          (uint32_t)ps->buf[OFF_ID + 2];
                f.cmd = ps->buf[OFF_CMD];
                f.qn1 = ((uint32_t)ps->buf[OFF_QN1] << 24) |
                        ((uint32_t)ps->buf[OFF_QN1 + 1] << 16) |
                        ((uint32_t)ps->buf[OFF_QN1 + 2] << 8) |
                        (uint32_t)ps->buf[OFF_QN1 + 3];
                f.qn2 = ((uint32_t)ps->buf[OFF_QN2] << 24) |
                        ((uint32_t)ps->buf[OFF_QN2 + 1] << 16) |
                        ((uint32_t)ps->buf[OFF_QN2 + 2] << 8) |
                        (uint32_t)ps->buf[OFF_QN2 + 3];
                f.len = ps->want;
                f.data = (ps->want != 0) ? &ps->buf[OFF_DATA] : NULL;
                on_frame(&f, arg);
            }
            parser_reset(ps);
        }
    }
}

/* ---------------------------------------------------------------------------
 * 协议上下文
 * ------------------------------------------------------------------------- */
int iv_proto_init(iv_proto_t *pf, const iv_proto_id_t *id,
                  iv_proto_tx_cb on_tx, void *tx_arg)
{
    if (pf == NULL || id == NULL || on_tx == NULL)
        return IV_EINVAL;
    memset(pf, 0, sizeof(*pf));
    pf->id = *id;
    pf->on_tx = on_tx;
    pf->tx_arg = tx_arg;
    return IV_OK;
}

int iv_proto_route(iv_proto_t *pf, uint8_t cmd, iv_proto_cmd_cb cb, void *arg)
{
    uint8_t i;

    if (pf == NULL)
        return IV_EINVAL;
    for (i = 0; i < pf->n_route; i++) {
        if (pf->route[i].cmd == cmd)
            return IV_EEXIST;
    }
    if (pf->n_route >= IV_PROTO_ROUTE_MAX)
        return IV_EFULL;
    pf->route[pf->n_route].cmd = cmd;
    pf->route[pf->n_route].cb = cb;
    pf->route[pf->n_route].arg = arg;
    pf->n_route++;
    return IV_OK;
}

/* 组帧并交传输层；组包缓冲在栈上（最大 21+255=276 字节，可接受） */
int iv_proto_send(iv_proto_t *pf, uint8_t cmd, uint32_t qn1, uint32_t qn2,
                  const void *data, uint16_t len)
{
    uint8_t buf[IV_PROTO_BIN_FIXED + IV_PROTO_BIN_DATA_MAX];
    size_t blen = 0;
    int rc;

    if (pf == NULL)
        return IV_EINVAL;
    rc = iv_proto_bin_build(buf, sizeof(buf), &blen, pf->id.devtype,
                            pf->id.devid, cmd, qn1, qn2, data, len);
    if (rc != IV_OK)
        return rc;
    pf->on_tx(buf, blen, pf->tx_arg);
    pf->tx_frames++;
    return IV_OK;
}

int iv_proto_ack(iv_proto_t *pf, uint8_t cmd, uint8_t code)
{
    if (pf == NULL)
        return IV_EINVAL;
    return iv_proto_send(pf, cmd, pf->last_qn1, pf->last_qn2, &code, 1);
}

int iv_proto_heartbeat(iv_proto_t *pf)
{
    static const uint8_t one = 0x01;
    if (pf == NULL)
        return IV_EINVAL;
    return iv_proto_send(pf, IV_PROTO_CMD_HEARTBEAT, 0u, 0u, &one, 1);
}

int iv_proto_send_text(iv_proto_t *pf, const char *seg, size_t seg_len)
{
    uint8_t buf[IV_PROTO_TXT_FIXED + IV_PROTO_TXT_DATA_MAX];
    size_t tlen = sizeof(buf);
    int rc;

    if (pf == NULL)
        return IV_EINVAL;
    rc = iv_proto_text_frame(seg, seg_len, buf, &tlen);
    if (rc != IV_OK)
        return rc;
    pf->on_tx(buf, tlen, pf->tx_arg);
    pf->tx_frames++;
    return IV_OK;
}

/* feed 的回调：路由分发 + 记录 QN */
static void dispatch(const iv_proto_frame_t *f, void *arg)
{
    iv_proto_t *pf = (iv_proto_t *)arg;
    uint8_t i;

    pf->rx_frames++;
    pf->last_qn1 = f->qn1;
    pf->last_qn2 = f->qn2;

    for (i = 0; i < pf->n_route; i++) {
        if (pf->route[i].cmd == f->cmd) {
            pf->route[i].cb(f, pf->route[i].arg);
            return;
        }
    }
    /* 未注册命令：保守回 OK（MCU 参考实现 default 分支同语义） */
    (void)iv_proto_ack(pf, f->cmd, IV_PROTO_ACK_OK);
}

void iv_proto_recv(iv_proto_t *pf, const uint8_t *buf, size_t len)
{
    if (pf == NULL || buf == NULL)
        return;
    iv_proto_parser_feed(&pf->rx, buf, len, dispatch, pf);
}
