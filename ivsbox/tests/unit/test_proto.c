/*
 * iv_proto 单测（开发计划 M2-S2.4）
 *
 * 覆盖：
 *   1) 二进制组帧黄金校验：build 的字节流与独立手工组帧逐字节比对；
 *   2) 二进制解析：完整帧 → 字段全对；半包逐字节喂 → 同样交付；
 *   3) 粘包：两帧一次 feed 各自交付；帧头噪声前置 → 仍交付；
 *   4) 坏 CRC/帧尾整帧丢弃且不影响后续帧；LEN 撒谎不误交付；
 *   5) ## 文本帧组包黄金校验（含样例回归：长度域 4 位、CRC 2 位小写 hex）；
 *   6) ## 文本帧长度探测与 IV_ERANGE；
 *   7) 路由：命中 handler、未命中自动 ACK(0x01)、重注册 IV_EEXIST、表满 IV_EFULL；
 *   8) ACK/心跳帧字节黄金校验（含 QN 回填 last_qn1/qn2）；
 *   9) 参数非法（NULL、len 超上限）。
 *
 * 期望帧由独立手工组帧生成（不经过被测代码），与 test_link 同一手法。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_proto.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---- 手工组帧（下行帧头 0xF0F0），不经过被测代码 ---- */
static size_t mk_down(uint8_t *out, uint16_t devtype, uint32_t devid,
                      uint8_t cmd, uint32_t qn1, uint32_t qn2,
                      const uint8_t *data, uint16_t len)
{
    size_t i;
    uint8_t crc;

    out[0] = 0xF0; out[1] = 0xF0;
    out[2] = IV_PROTO_VER;
    out[3] = (uint8_t)(devtype >> 8); out[4] = (uint8_t)devtype;
    out[5] = (uint8_t)(devid >> 16); out[6] = (uint8_t)(devid >> 8);
    out[7] = (uint8_t)devid;
    out[8] = cmd;
    out[9]  = (uint8_t)(qn1 >> 24); out[10] = (uint8_t)(qn1 >> 16);
    out[11] = (uint8_t)(qn1 >> 8);  out[12] = (uint8_t)qn1;
    out[13] = (uint8_t)(qn2 >> 24); out[14] = (uint8_t)(qn2 >> 16);
    out[15] = (uint8_t)(qn2 >> 8);  out[16] = (uint8_t)qn2;
    out[17] = (uint8_t)len;
    for (i = 0; i < len; i++)
        out[18 + i] = data[i];
    crc = iv_crc8(&out[2], 16u + len, IV_CRC8_SEED_INIT); /* ver..data 末尾 */
    out[18 + len] = crc;
    out[19 + len] = 0xFF;
    out[20 + len] = 0xFF;
    return 21u + len;
}

/* ---- 发送记录 ---- */
static struct {
    uint8_t  buf[8][512];
    size_t   len[8];
    int      n;
} g_tx;
static int g_tx_rc;

static int on_tx(const uint8_t *bytes, size_t len, void *arg)
{
    (void)arg;
    if (g_tx_rc != IV_OK)
        return g_tx_rc;
    if (g_tx.n < 8 && len <= sizeof(g_tx.buf[0])) {
        memcpy(g_tx.buf[g_tx.n], bytes, len);
        g_tx.len[g_tx.n] = len;
    }
    g_tx.n++;
    return IV_OK;
}

/* ---- 帧回调记录 ---- */
static struct {
    iv_proto_frame_t f[8];
    int n;
} g_rx;

static void on_frame(const iv_proto_frame_t *f, void *arg)
{
    (void)arg;
    if (g_rx.n < 8) {
        g_rx.f[g_rx.n] = *f;
        /* data 指向解析器内部缓冲，回调返回后失效——测试里立刻拷一份 */
        if (f->data != NULL && f->len != 0) {
            static uint8_t data_copy[8][64];
            memcpy(data_copy[g_rx.n], f->data, f->len);
            g_rx.f[g_rx.n].data = data_copy[g_rx.n];
        }
        g_rx.n++;
    }
}

/* ---- 路由 handler 记录 ---- */
static int g_hit;
static iv_proto_frame_t g_hit_f;

static void handler(const iv_proto_frame_t *f, void *arg)
{
    (void)arg;
    g_hit++;
    g_hit_f = *f;
}

static void rec_reset(void)
{
    memset(&g_tx, 0, sizeof(g_tx));
    g_tx_rc = IV_OK;
    memset(&g_rx, 0, sizeof(g_rx));
    g_hit = 0;
    memset(&g_hit_f, 0, sizeof(g_hit_f));
}

int main(void)
{
    /* ---- 1) 二进制组帧黄金校验 ---- */
    {
        static iv_proto_t pf;
        iv_proto_id_t id = { 0x0400u, 0x010203u };
        uint8_t data[3] = { 0x00, 0x11, 0x22 };
        uint8_t want[64];
        size_t want_len, blen = 0;
        uint8_t out[64];
        int rc;

        chk(iv_proto_init(&pf, &id, on_tx, NULL) == IV_OK, "init ok");
        chk(iv_proto_init(NULL, &id, on_tx, NULL) == IV_EINVAL, "init NULL pf");
        chk(iv_proto_init(&pf, NULL, on_tx, NULL) == IV_EINVAL, "init NULL id");
        chk(iv_proto_init(&pf, &id, NULL, NULL) == IV_EINVAL, "init NULL tx");

        /* 重新 init（上面失败用例可能已部分写 pf） */
        chk(iv_proto_init(&pf, &id, on_tx, NULL) == IV_OK, "init ok 2");

        rc = iv_proto_bin_build(out, sizeof(out), &blen, id.devtype, id.devid,
                                0xE1u, 0x013461c9u, 0x0decba58u, data, 3);
        chk(rc == IV_OK, "bin_build ok");
        want_len = mk_down(want, id.devtype, id.devid, 0xE1u,
                           0x013461c9u, 0x0decba58u, data, 3);
        /* 手工组帧用的是下行头 0xF0F0，build 是上行头 0x0F0F——只比第 3 字节起 */
        chk(blen == want_len, "bin_build len");
        chk(memcmp(&out[2], &want[2], blen - 2) == 0, "bin_build bytes");

        /* len 超上限 */
        chk(iv_proto_bin_build(out, sizeof(out), &blen, 0x400, 1, 0xE1, 0, 0,
                               NULL, IV_PROTO_BIN_DATA_MAX + 1u) == IV_ERANGE,
            "bin_build range");
        chk(iv_proto_bin_build(out, sizeof(out), &blen, 0x400, 1, 0xE1, 0, 0,
                               NULL, 1) == IV_EINVAL, "bin_build rejects NULL data");

        /* 长度探测 */
        blen = 0;
        chk(iv_proto_bin_build(NULL, 0, &blen, 0x400, 1, 0xE1, 0, 0, NULL, 5) == IV_OK,
            "bin_build probe");
        chk(blen == 21u + 5u, "bin_build probe len");
    }

    /* ---- 2) 解析：完整帧 / 半包 / 粘包 / 噪声 ---- */
    {
        static iv_proto_parser_t ps;
        uint8_t data[2] = { 0x00 };
        uint8_t f1[64], f2[64];
        size_t l1, l2;
        uint8_t stream[160];
        size_t off = 0;
        size_t i;

        iv_proto_parser_init(&ps);
        l1 = mk_down(f1, 0x0400u, 0x010203u, 0xE1u, 1u, 2u, data, 1);
        l2 = mk_down(f2, 0x0400u, 0x010203u, 0xE2u, 3u, 4u, NULL, 0);

        /* 一次整帧 */
        iv_proto_parser_feed(&ps, f1, l1, on_frame, NULL);
        chk(g_rx.n == 1, "feed full frame");
        chk(g_rx.f[0].cmd == 0xE1u, "frame cmd");
        chk(g_rx.f[0].devtype == 0x0400u, "frame devtype");
        chk(g_rx.f[0].devid == 0x010203u, "frame devid");
        chk(g_rx.f[0].qn1 == 1u && g_rx.f[0].qn2 == 2u, "frame qn");
        chk(g_rx.f[0].len == 1u && g_rx.f[0].data[0] == 0x00, "frame data");

        /* 半包逐字节 */
        rec_reset();
        iv_proto_parser_init(&ps);
        for (i = 0; i < l1; i++)
            iv_proto_parser_feed(&ps, &f1[i], 1, on_frame, NULL);
        chk(g_rx.n == 1 && g_rx.f[0].cmd == 0xE1u, "byte-by-byte halfpack");

        /* 前置噪声 + 粘包 */
        rec_reset();
        iv_proto_parser_init(&ps);
        stream[off++] = 0x12; stream[off++] = 0x34;
        memcpy(&stream[off], f1, l1); off += l1;
        memcpy(&stream[off], f2, l2); off += l2;
        iv_proto_parser_feed(&ps, stream, off, on_frame, NULL);
        chk(g_rx.n == 2, "noise+sticky: two frames");
        chk(g_rx.f[0].cmd == 0xE1u && g_rx.f[1].cmd == 0xE2u, "sticky order");
        chk(g_rx.f[1].len == 0 && g_rx.f[1].data == NULL, "zero-len data NULL");
    }

    /* ---- 3) 坏 CRC / 帧尾 / LEN 撒谎 ---- */
    {
        static iv_proto_parser_t ps;
        uint8_t f1[64], f2[64];
        size_t l1, l2;

        iv_proto_parser_init(&ps);
        l1 = mk_down(f1, 0x0400u, 0x010203u, 0xE1u, 1u, 2u, NULL, 0);
        l2 = mk_down(f2, 0x0400u, 0x010203u, 0xE2u, 3u, 4u, NULL, 0);

        rec_reset();
        f1[l1 - 3] ^= 0xFF; /* 破坏 CRC 字节 */
        {
            uint8_t both[128];
            memcpy(both, f1, l1);
            memcpy(&both[l1], f2, l2);
            iv_proto_parser_feed(&ps, both, l1 + l2, on_frame, NULL);
            chk(g_rx.n == 1 && g_rx.f[0].cmd == 0xE2u,
                "bad CRC dropped, next frame ok");
        }

        rec_reset();
        iv_proto_parser_init(&ps);
        l1 = mk_down(f1, 0x0400u, 0x010203u, 0xE1u, 1u, 2u, NULL, 0);
        f1[l1 - 1] = 0x00; /* CRC 仍正确，仅帧尾损坏 */
        {
            uint8_t both[128];
            memcpy(both, f1, l1);
            memcpy(&both[l1], f2, l2);
            iv_proto_parser_feed(&ps, both, l1 + l2, on_frame, NULL);
            chk(g_rx.n == 1 && g_rx.f[0].cmd == 0xE2u,
                "bad tail dropped, next frame ok");
        }

        /* LEN 撒谎：把 LEN 改成 0xFF，帧体按小 LEN 给——会吞掉后续字节直到
         * 缓冲上限或重新同步。构造：坏 LEN 帧后紧跟好帧，好帧应被吞掉一部分
         * 后在缓冲未满时保持等待（这里只验证不崩溃、不误交付）。
         * 单独喂坏 LEN 帧头即可（后面没有更多字节）。 */
        rec_reset();
        iv_proto_parser_init(&ps);
        f1[17] = 0xFE; /* 撒谎 LEN=254 */
        iv_proto_parser_feed(&ps, f1, l1, on_frame, NULL);
        chk(g_rx.n == 0, "lying LEN: no delivery");
    }

    /* ---- 4) ## 文本帧黄金校验（含协议样例回归） ---- */
    {
        uint8_t out[64];
        size_t olen = sizeof(out);
        const char *seg = "QN=0;TID=22855;VER=11;DEVTYPE=0400;CP=&&DT=1&&";
        size_t seg_len = strlen(seg);
        uint8_t crc;
        char crc_hex[3];
        int rc;

        rc = iv_proto_text_frame(seg, seg_len, out, &olen);
        chk(rc == IV_OK, "text_frame ok");
        chk(olen == 8u + seg_len, "text_frame len");
        chk(out[0] == '#' && out[1] == '#', "text head");
        chk(out[2] == '0' && out[3] == '0' &&
            out[4] == (uint8_t)('0' + seg_len / 10u) &&
            out[5] == (uint8_t)('0' + seg_len % 10u), "text len ascii");
        chk(memcmp(&out[6], seg, seg_len) == 0, "text seg");
        chk(out[6 + seg_len + 2] == '#' && out[6 + seg_len + 3] == '#',
            "text tail");

        crc = iv_crc8(seg, seg_len, IV_CRC8_SEED_INIT);
        snprintf(crc_hex, sizeof(crc_hex), "%02x", crc);
        chk(out[6 + seg_len] == (uint8_t)crc_hex[0] &&
            out[6 + seg_len + 1] == (uint8_t)crc_hex[1], "text crc hex");

        /* 长度探测 */
        olen = 0;
        rc = iv_proto_text_frame(seg, seg_len, NULL, &olen);
        chk(rc == IV_OK && olen == 8u + seg_len, "text probe");

        /* 缓冲不足 */
        olen = 7u + seg_len;
        rc = iv_proto_text_frame(seg, seg_len, out, &olen);
        chk(rc == IV_ERANGE && olen == 8u + seg_len, "text ERANGE");

        /* 空数据段 */
        olen = sizeof(out);
        rc = iv_proto_text_frame("", 0, out, &olen);
        chk(rc == IV_OK && olen == 8u, "text empty seg");

        /* 超上限 */
        {
            static char big[IV_PROTO_TXT_DATA_MAX + 2];
            olen = sizeof(out);
            rc = iv_proto_text_frame(big, IV_PROTO_TXT_DATA_MAX + 1u, out, &olen);
            chk(rc == IV_ERANGE, "text over max");
        }
    }

    /* ---- 5) 路由 / ACK / 心跳 ---- */
    {
        static iv_proto_t pf;
        iv_proto_id_t id = { 0x0400u, 0x010203u };
        uint8_t f_e1[64], f_e9[64];
        size_t l;
        uint8_t want[64];
        size_t want_len;
        uint8_t ack_data = IV_PROTO_ACK_OK;
        uint8_t hb_data = 0x01;
        uint8_t i;

        rec_reset();
        chk(iv_proto_init(&pf, &id, on_tx, NULL) == IV_OK, "ctx init");
        chk(iv_proto_route(&pf, 0xE1u, handler, NULL) == IV_OK, "route add");
        chk(iv_proto_route(&pf, 0xE1u, handler, NULL) == IV_EEXIST,
            "route dup EEXIST");
        for (i = 0; i < IV_PROTO_ROUTE_MAX - 1u; i++)
            (void)iv_proto_route(&pf, (uint8_t)(0x40u + i), handler, NULL);
        chk(iv_proto_route(&pf, 0x00u, handler, NULL) == IV_EFULL,
            "route full EFULL");

        /* 命中：E1 查询 → handler 被调、无自动 ACK */
        l = mk_down(f_e1, 0x0400u, 0x010203u, 0xE1u, 0xAu, 0xBu, NULL, 0);
        iv_proto_recv(&pf, f_e1, l);
        chk(g_hit == 1 && g_hit_f.cmd == 0xE1u, "route hit");
        chk(g_hit_f.qn1 == 0xAu && g_hit_f.qn2 == 0xBu, "hit qn");
        chk(g_tx.n == 0, "no auto ack on hit");

        /* 未命中：E9 → 自动 ACK，QN 回填、字节黄金校验 */
        l = mk_down(f_e9, 0x0400u, 0x010203u, 0xE9u, 0x11223344u, 0x55667788u,
                    NULL, 0);
        iv_proto_recv(&pf, f_e9, l);
        chk(g_hit == 1 && g_tx.n == 1, "miss -> auto ack");
        want_len = mk_down(want, 0x0400u, 0x010203u, 0xE9u,
                           0x11223344u, 0x55667788u, &ack_data, 1);
        /* want 用下行头；实际发的是上行头——跳过前 2 字节比对 */
        chk(g_tx.len[0] == want_len, "ack len");
        chk(memcmp(&g_tx.buf[0][2], &want[2], want_len - 2) == 0, "ack bytes");

        /* 显式 ACK（用 last_qn）：E1 注册 no-op handler，避免自动 ACK 干扰 */
        rec_reset();
        chk(iv_proto_init(&pf, &id, on_tx, NULL) == IV_OK, "ctx init 2");
        chk(iv_proto_route(&pf, 0xE1u, handler, NULL) == IV_OK, "route2 add");
        l = mk_down(f_e1, 0x0400u, 0x010203u, 0xE1u, 0x11111111u,
                    0x22222222u, NULL, 0);
        iv_proto_recv(&pf, f_e1, l);
        chk(g_tx.n == 0, "route2 no auto ack");
        chk(iv_proto_ack(&pf, 0xE1u, IV_PROTO_ACK_OK) == IV_OK, "ack send");
        want_len = mk_down(want, 0x0400u, 0x010203u, 0xE1u,
                           0x11111111u, 0x22222222u, &ack_data, 1);
        chk(g_tx.n == 1 && g_tx.len[0] == want_len, "ack len 2");
        chk(memcmp(&g_tx.buf[0][2], &want[2], want_len - 2) == 0, "ack bytes 2");
        chk(pf.rx_frames == 1u && pf.tx_frames == 1u, "counters rx1tx1");

        /* 心跳 */
        rec_reset();
        chk(iv_proto_init(&pf, &id, on_tx, NULL) == IV_OK, "ctx init 3");
        chk(iv_proto_heartbeat(&pf) == IV_OK, "heartbeat send");
        want_len = mk_down(want, 0x0400u, 0x010203u, 0xFFu, 0u, 0u, &hb_data, 1);
        chk(g_tx.n == 1 && g_tx.len[0] == want_len, "heartbeat len");
        chk(memcmp(&g_tx.buf[0][2], &want[2], want_len - 2) == 0,
            "heartbeat bytes");

        /* send_text */
        rec_reset();
        chk(iv_proto_init(&pf, &id, on_tx, NULL) == IV_OK, "ctx init 4");
        chk(iv_proto_send_text(&pf, "QN=0;", 5) == IV_OK, "send_text ok");
        chk(g_tx.n == 1 && g_tx.len[0] == 8u + 5u, "send_text len");
        chk(memcmp(g_tx.buf[0], "##0005QN=0;", 11) != 0 ? 1 : 1, "no-op");
        /* 前 6 字节 '##0005' + seg 'QN=0;' */
        chk(g_tx.buf[0][0] == '#' && g_tx.buf[0][1] == '#' &&
            g_tx.buf[0][2] == '0' && g_tx.buf[0][3] == '0' &&
            g_tx.buf[0][4] == '0' && g_tx.buf[0][5] == '5' &&
            memcmp(&g_tx.buf[0][6], "QN=0;", 5) == 0, "send_text bytes");

        g_tx_rc = IV_EFULL;
        chk(iv_proto_send(&pf, 0xE1u, 0, 0, NULL, 0) == IV_EFULL,
            "binary queue failure propagated");
        chk(iv_proto_send_text(&pf, "x", 1) == IV_EFULL,
            "text queue failure propagated");
        chk(pf.tx_frames == 1u, "failed queue writes not counted");
        g_tx_rc = IV_OK;

        /* NULL 入参 */
        chk(iv_proto_send(NULL, 0xE1, 0, 0, NULL, 0) == IV_EINVAL, "send NULL");
        chk(iv_proto_ack(NULL, 0xE1, 1) == IV_EINVAL, "ack NULL");
        chk(iv_proto_heartbeat(NULL) == IV_EINVAL, "hb NULL");
        chk(iv_proto_send_text(NULL, "x", 1) == IV_EINVAL, "text NULL");
        iv_proto_recv(NULL, f_e1, 8); /* NULL 不崩溃 */

        /* 统计计数（最后一次 init4 + send_text） */
        chk(pf.rx_frames == 0u && pf.tx_frames == 1u, "counters rx0tx1");
    }

    if (g_fail == 0)
        printf("test_proto passed (build/parse/sticky/crc/tail/text/route/ack/hb/null)\n");
    else
        printf("test_proto FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
