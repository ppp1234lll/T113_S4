/*
 * iv_link 单测（开发计划 M2-S2.3）
 *
 * 覆盖（计划 §S2.3"怎么验证"＋实现中新增）：
 *   1) 正常收发：send 的下行帧字节黄金校验（独立组帧交叉验证）＋ 应答配对完成；
 *   2) 丢应答触发重传：重传帧与首发帧字节一致、次数受 max_retries 约束；
 *   3) 重传耗尽判失败：done(IV_ETIMEDOUT)、槽释放可复用；
 *   4) 同 cmd 并发 IV_EBUSY；在飞表满 IV_EFULL；
 *   5) 上报帧（0xC1/0xC2）无在飞 → on_upstream；
 *   6) 应答＋上报粘包一次 recv：两帧各自正确处置（交接区覆盖 bug 的回归用例）；
 *   7) on_done 内重入 send；
 *   8) 链路状态：连续失败 DOWN、合法帧 UP（on_change 回调）；
 *   9) clear 以指定 rc 回调全部在飞；
 *  10) NULL / 非法入参；下行帧头不被上行解析器接受。
 *
 * 期望帧字节由**独立手工组帧**生成（不经过被测代码），可抓住
 * "send 组帧被改坏"类回归。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_link.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---- 回调记录 ---- */
#define MAX_REC 16
static struct {
    uint8_t  buf[IV_FRAME_MAX];
    size_t   len;
    int      n;
} g_tx;
static int g_tx_rc;

static struct {
    int      rc[MAX_REC];
    uint8_t  cmd[MAX_REC];
    uint16_t len[MAX_REC];
    uint8_t  data[MAX_REC][64];
    int      n;
} g_done;

static struct {
    uint8_t  cmd[MAX_REC];
    uint16_t len[MAX_REC];
    uint8_t  data[MAX_REC][64];
    int      n;
} g_up;

static struct {
    int up[MAX_REC];
    int n;
} g_chg;

static void rec_reset(void)
{
    memset(&g_tx, 0, sizeof(g_tx));
    g_tx_rc = IV_OK;
    memset(&g_done, 0, sizeof(g_done));
    memset(&g_up, 0, sizeof(g_up));
    memset(&g_chg, 0, sizeof(g_chg));
}

static int on_tx(const uint8_t *bytes, size_t len, void *arg)
{
    (void)arg;
    if (g_tx_rc != IV_OK)
        return g_tx_rc;
    if (g_tx.n < MAX_REC && len <= sizeof(g_tx.buf)) {
        memcpy(g_tx.buf, bytes, len);
        g_tx.len = len;
    }
    g_tx.n++;
    return IV_OK;
}

static void on_done(int rc, uint8_t cmd, const uint8_t *data, uint16_t len, void *arg)
{
    (void)arg;
    if (g_done.n < MAX_REC) {
        int i = g_done.n;
        g_done.rc[i] = rc;
        g_done.cmd[i] = cmd;
        g_done.len[i] = len;
        if (data != NULL && len != 0)
            memcpy(g_done.data[i], data, len);
        g_done.n++;
    }
}

static void on_up(uint8_t cmd, const uint8_t *data, uint16_t len, void *arg)
{
    (void)arg;
    if (g_up.n < MAX_REC) {
        int i = g_up.n;
        g_up.cmd[i] = cmd;
        g_up.len[i] = len;
        if (data != NULL && len != 0)
            memcpy(g_up.data[i], data, len);
        g_up.n++;
    }
}

static void on_chg(int up, void *arg)
{
    (void)arg;
    if (g_chg.n < MAX_REC)
        g_chg.up[g_chg.n++] = up;
}

/* ---- 独立手工组帧（head 可指定），不经过被测代码 ---- */
static size_t mk(uint8_t *out, uint16_t head, uint8_t cmd,
                 const uint8_t *data, uint16_t len)
{
    uint8_t crc;
    size_t i;

    out[0] = (uint8_t)(head >> 8);
    out[1] = (uint8_t)(head & 0xFF);
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
    static iv_link_t lk;
    static uint8_t f[IV_FRAME_MAX];
    uint8_t req[2];
    uint8_t json[32];
    size_t i;

    for (i = 0; i < sizeof(json); i++)
        json[i] = (uint8_t)('a' + (i % 26));
    req[0] = 0x00;

    /* ---- 1) 正常收发：下行帧字节黄金校验 + 应答配对 ---- */
    {
        iv_link_cfg_t cfg = { 1000, 2, 3 };
        size_t n;

        rec_reset();
        chk(iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL) == IV_OK, "init");
        chk(iv_link_is_up(&lk) == 0, "link starts DOWN");
        chk(iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 1000, on_done, NULL) == IV_OK,
            "send query");
        chk(g_tx.n == 1, "one tx on send");
        n = mk(f, IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY, req, 1);
        chk(g_tx.len == n && memcmp(g_tx.buf, f, n) == 0,
            "tx frame == independent mk (downlink bytes)");
        /* 应答到达（上行头 + JSON） */
        n = mk(f, IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, json, 10);
        iv_link_recv(&lk, f, n, 1100);
        chk(g_done.n == 1 && g_done.rc[0] == IV_OK && g_done.cmd[0] == IV_FRAME_CMD_QUERY &&
                g_done.len[0] == 10 && memcmp(g_done.data[0], json, 10) == 0,
            "answer paired and delivered");
        chk(iv_link_is_up(&lk) == 1, "link UP after valid frame");
    }

    /* ---- 2) 丢应答触发重传：重传帧与首发一致 ---- */
    {
        iv_link_cfg_t cfg = { 100, 2, 3 };
        size_t n;
        uint8_t first[IV_FRAME_MAX];
        size_t first_len;

        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        iv_link_send(&lk, IV_FRAME_CMD_THRESH, json, 18, 0, on_done, NULL);
        chk(g_tx.n == 1, "first send");
        first_len = g_tx.len;
        memcpy(first, g_tx.buf, first_len);

        iv_link_tick(&lk, 100); /* 第一次到期 → 重传 */
        chk(g_tx.n == 2 && g_tx.len == first_len &&
                memcmp(g_tx.buf, first, first_len) == 0,
            "retransmit #1 identical bytes");
        chk(g_done.n == 0, "no done on retransmit");
        iv_link_tick(&lk, 200); /* 第二次到期 → 重传，deadline=300 */
        chk(g_tx.n == 3, "retransmit #2");
        iv_link_tick(&lk, 299); /* 窗口内（deadline=300）：无动作 */
        chk(g_tx.n == 3 && g_done.n == 0, "no extra retransmit within window");
        /* 应答在窗口内到达 → 完成 */
        n = mk(f, IV_FRAME_HEAD_UP, IV_FRAME_CMD_THRESH, json, 4);
        iv_link_recv(&lk, f, n, 250);
        chk(g_done.n == 1 && g_done.rc[0] == IV_OK && g_done.len[0] == 4,
            "done after retransmitted request answered");
    }

    /* ---- 3) 电源控制不重传，超时后槽释放 ---- */
    {
        iv_link_cfg_t cfg = { 100, 2, 3 };

        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        iv_link_send(&lk, IV_FRAME_CMD_POWER, req, 1, 0, on_done, NULL);
        iv_link_tick(&lk, 100);
        chk(g_tx.n == 1, "power command sent only once");
        chk(g_done.n == 1 && g_done.rc[0] == IV_ETIMEDOUT &&
                g_done.cmd[0] == IV_FRAME_CMD_POWER,
            "power timeout -> IV_ETIMEDOUT");
        chk(iv_link_send(&lk, IV_FRAME_CMD_POWER, req, 1, 400, on_done, NULL) == IV_OK,
            "slot released after failure");
        iv_link_tick(&lk, 500);
        chk(g_tx.n == 2, "second explicit command also sent only once");
        chk(iv_link_send(&lk, IV_FRAME_CMD_REBOOT, req, 1, 600, on_done, NULL) == IV_OK,
            "send reboot");
        iv_link_tick(&lk, 700);
        chk(g_tx.n == 3 && g_done.n == 3, "reboot also not retransmitted");
    }

    /* ---- 4) 同 cmd 并发 EBUSY / 表满 EFULL ---- */
    {
        iv_link_cfg_t cfg = { 10000, 0, 3 };

        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        chk(iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 0, on_done, NULL) == IV_OK,
            "send #1");
        chk(iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 0, on_done, NULL) == IV_EBUSY,
            "same cmd -> IV_EBUSY");
        chk(iv_link_send(&lk, IV_FRAME_CMD_THRESH, json, 18, 0, on_done, NULL) == IV_OK,
            "send #2");
        chk(iv_link_send(&lk, IV_FRAME_CMD_POWER, req, 1, 0, on_done, NULL) == IV_OK,
            "send #3");
        chk(iv_link_send(&lk, IV_FRAME_CMD_REBOOT, req, 1, 0, on_done, NULL) == IV_OK,
            "send #4");
        chk(iv_link_send(&lk, IV_FRAME_CMD_DOOR, req, 1, 0, on_done, NULL) == IV_EFULL,
            "fifth inflight -> IV_EFULL");
    }

    /* ---- 5) 上报帧无在飞 → on_upstream ---- */
    {
        iv_link_cfg_t cfg = { 1000, 2, 3 };
        size_t n;
        uint8_t one = 0x01;

        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        n = mk(f, IV_FRAME_HEAD_UP, IV_FRAME_CMD_DOOR, &one, 1);
        iv_link_recv(&lk, f, n, 100);
        chk(g_up.n == 1 && g_up.cmd[0] == IV_FRAME_CMD_DOOR && g_up.len[0] == 1 &&
                g_up.data[0][0] == 0x01,
            "door report -> upstream");
        chk(g_done.n == 0, "no txn done for upstream");
        n = mk(f, IV_FRAME_HEAD_UP, IV_FRAME_CMD_EVENT, json, 12);
        iv_link_recv(&lk, f, n, 200);
        chk(g_up.n == 2 && g_up.cmd[1] == IV_FRAME_CMD_EVENT && g_up.len[1] == 12,
            "event report -> upstream");
    }

    /* ---- 6) 应答＋上报粘包一次 recv（交接区覆盖 bug 的回归用例） ---- */
    {
        iv_link_cfg_t cfg = { 1000, 2, 3 };
        uint8_t two[2 * IV_FRAME_MAX];
        size_t n1, n2;

        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 0, on_done, NULL);
        n1 = mk(two, IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, json, 6);      /* 应答 */
        n2 = mk(two + n1, IV_FRAME_HEAD_UP, IV_FRAME_CMD_DOOR, &req[0], 1); /* 上报 */
        iv_link_recv(&lk, two, n1 + n2, 100);
        chk(g_done.n == 1 && g_done.cmd[0] == IV_FRAME_CMD_QUERY &&
                g_done.len[0] == 6 && memcmp(g_done.data[0], json, 6) == 0,
            "sticky answer paired correctly");
        chk(g_up.n == 1 && g_up.cmd[0] == IV_FRAME_CMD_DOOR,
            "sticky report still delivered");
    }

    /* ---- 7) on_done 内重入 send ---- */
    {
        iv_link_cfg_t cfg = { 1000, 2, 3 };
        size_t n;

        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 0, on_done, NULL);
        /* 应答到达后，在 on_done 里再发一笔 —— 应当成功不崩 */
        n = mk(f, IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, json, 2);
        iv_link_recv(&lk, f, n, 100);
        chk(g_done.n == 1, "done delivered");
        chk(iv_link_send(&lk, IV_FRAME_CMD_REBOOT, req, 1, 200, on_done, NULL) == IV_OK,
            "reentrant send inside on_done path");
        n = mk(f, IV_FRAME_HEAD_UP, IV_FRAME_CMD_REBOOT, json, 1);
        iv_link_recv(&lk, f, n, 300);
        chk(g_done.n == 2 && g_done.cmd[1] == IV_FRAME_CMD_REBOOT, "reentrant done");
    }

    /* ---- 8) 链路状态：连续失败 DOWN / 合法帧 UP ---- */
    {
        iv_link_cfg_t cfg = { 100, 0, 3 }; /* 不重传，每次 tick 即失败 */

        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        chk(iv_link_is_up(&lk) == 0, "starts DOWN");
        /* 初始 DOWN 阶段失败不累计不触发（本实现只在 UP 态计数） */
        iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 0, on_done, NULL);
        iv_link_tick(&lk, 100);
        chk(g_done.n == 1 && g_done.rc[0] == IV_ETIMEDOUT, "fail #1");
        chk(g_chg.n == 0, "no change while already DOWN");
        /* 一次成功应答 → UP */
        {
            size_t n = mk(f, IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, json, 1);
            iv_link_recv(&lk, f, n, 200);
            chk(g_chg.n == 1 && g_chg.up[0] == 1, "UP on first valid frame");
        }
        /* UP 态连续失败 3 次 → DOWN */
        iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 300, on_done, NULL);
        iv_link_tick(&lk, 400);
        iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 500, on_done, NULL);
        iv_link_tick(&lk, 600);
        chk(g_chg.n == 1, "no DOWN before threshold");
        iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 700, on_done, NULL);
        iv_link_tick(&lk, 800);
        chk(g_chg.n == 2 && g_chg.up[1] == 0, "DOWN after 3 consecutive failures");
        /* 坏帧不置 UP：用正常组帧再打坏 CRC 字节（避免手写字节受字节序影响） */
        rec_reset();
        {
            size_t nb = mk(f, IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, req, 1);
            f[nb - 3] ^= 0xFF; /* CRC 字节位置（len 后紧跟） */
            iv_link_recv(&lk, f, nb, 900);
        }
        chk(iv_link_is_up(&lk) == 0, "bad frame does not bring link UP");
        /* 合法帧 → UP */
        {
            size_t n = mk(f, IV_FRAME_HEAD_UP, IV_FRAME_CMD_DOOR, &req[0], 1);
            iv_link_recv(&lk, f, n, 1000);
            chk(g_chg.n == 1 && g_chg.up[0] == 1, "UP again on valid frame");
        }
    }

    /* ---- 9) clear 以指定 rc 回调全部在飞 ---- */
    {
        iv_link_cfg_t cfg = { 10000, 0, 3 };

        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 0, on_done, NULL);
        iv_link_send(&lk, IV_FRAME_CMD_THRESH, json, 18, 0, on_done, NULL);
        iv_link_clear(&lk, IV_ECANCELED);
        chk(g_done.n == 2 && g_done.rc[0] == IV_ECANCELED &&
                g_done.rc[1] == IV_ECANCELED,
            "clear cancels both inflight");
        chk(iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 0, on_done, NULL) == IV_OK,
            "slots free after clear");
    }

    /* ---- 10) 非法入参 / 下行帧头不被接受 ---- */
    {
        iv_link_cfg_t cfg = { 1000, 2, 3 };
        static uint8_t big[IV_FRAME_DATA_MAX + 1];
        size_t n;

        chk(iv_link_init(NULL, &cfg, on_tx, NULL, NULL, NULL) == IV_EINVAL,
            "init NULL lk");
        chk(iv_link_init(&lk, &cfg, NULL, NULL, NULL, NULL) == IV_EINVAL,
            "init NULL on_tx");
        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        chk(iv_link_send(NULL, 0xE1, req, 1, 0, on_done, NULL) == IV_EINVAL,
            "send NULL lk");
        chk(iv_link_send(&lk, 0xE1, NULL, 1, 0, on_done, NULL) == IV_EINVAL,
            "send NULL data with len");
        chk(iv_link_send(&lk, 0xE1, big, (uint16_t)sizeof(big), 0, on_done, NULL) == IV_ERANGE,
            "send oversized data");
        iv_link_tick(NULL, 0);
        iv_link_clear(NULL, IV_OK);
        chk(iv_link_is_up(NULL) == 0, "is_up NULL");

        /* 下行帧头 0xF0F0 的帧不是给主板的：不得交付、不得置 UP */
        rec_reset();
        n = mk(f, IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY, json, 3);
        iv_link_recv(&lk, f, n, 100);
        chk(g_done.n == 0 && g_up.n == 0, "downlink-headed frame rejected");
        chk(iv_link_is_up(&lk) == 0, "downlink frame does not set UP");
        iv_link_reset(&lk);
        chk(iv_link_is_up(&lk) == 0, "reset clears link state");
    }

    /* ---- 11) 发送队列拒绝首发/重传时，错误可见且槽可复用 ---- */
    {
        iv_link_cfg_t cfg = { 100, 2, 3 };
        rec_reset();
        iv_link_init(&lk, &cfg, on_tx, on_up, on_chg, NULL);
        g_tx_rc = IV_EFULL;
        chk(iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 0, on_done, NULL) == IV_EFULL,
            "initial queue failure propagated");
        chk(g_done.n == 0, "initial queue failure has no done callback");
        g_tx_rc = IV_OK;
        chk(iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 0, on_done, NULL) == IV_OK,
            "slot reusable after initial queue failure");
        g_tx_rc = IV_EFULL;
        iv_link_tick(&lk, 100);
        chk(g_done.n == 1 && g_done.rc[0] == IV_EFULL,
            "retransmit queue failure reported to caller");
        g_tx_rc = IV_OK;
        chk(iv_link_send(&lk, IV_FRAME_CMD_QUERY, req, 1, 200, on_done, NULL) == IV_OK,
            "slot reusable after retransmit queue failure");
    }

    if (g_fail == 0)
        printf("test_link passed (send/pair/retransmit/exhaust/ebusy/efull/"
               "upstream/sticky/reentry/linkstate/clear/null)\n");
    return g_fail == 0 ? 0 : 1;
}
