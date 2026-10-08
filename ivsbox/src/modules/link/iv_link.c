/*
 * 采集板 UART 可靠层实现（功能开发计划 M2-S2.3）
 *
 * 设计取舍与协议事实见 include/ivsbox/iv_link.h 文件头与架构 §18.2。
 *
 * 关键实现点：
 *   - 请求 data 原件存进事务槽（txn.data）：首发与每次重传都由
 *     iv_frame_build 现组帧，保证重传帧与首发帧字节一致；
 *   - 应答配对＝同 cmd 在飞事务；帧回调（feed 内同步嵌套）只做"配对 +
 *     拷贝到 stage"，回到 recv 外层后**先摘槽、后回调**——回调里再
 *     send()/clear() 不会复用正在处理的槽，也不会与遍历交叉；
 *   - 链路状态：事务最终失败 fail_streak++（达阈值判 DOWN）；
 *     成功/合法帧清零并置 UP（DOWN→UP 触发 on_change(1)）。
 *   - 超时比较用 (int32_t)(now - deadline) 差值语义（单调毫秒回绕安全）。
 */
#include <string.h>

#include "ivsbox/iv_link.h"
#include "ivsbox/iv_ret.h"

static void on_framed(const iv_frame_t *fr, void *arg);

int iv_link_init(iv_link_t *lk, const iv_link_cfg_t *cfg,
                 iv_link_tx_cb on_tx, iv_link_upstream_cb on_upstream,
                 iv_link_change_cb on_change, void *arg)
{
    static const iv_link_cfg_t def = IV_LINK_CFG_DEFAULT;

    if (lk == NULL || on_tx == NULL)
        return IV_EINVAL;
    memset(lk, 0, sizeof(*lk));
    lk->cfg = (cfg != NULL) ? *cfg : def;
    lk->on_tx = on_tx;
    lk->on_upstream = on_upstream;
    lk->on_change = on_change;
    lk->arg = arg;
    return iv_framer_init(&lk->framer, IV_FRAME_HEAD_UP, 0);
}

void iv_link_reset(iv_link_t *lk)
{
    if (lk == NULL)
        return;
    iv_framer_reset(&lk->framer);
    memset(lk->txn, 0, sizeof(lk->txn));
    lk->link_up = 0;
    lk->fail_streak = 0;
}

static void link_set_up(iv_link_t *lk, int up)
{
    int old = lk->link_up;

    lk->link_up = (uint8_t)(up != 0);
    if (up)
        lk->fail_streak = 0;
    if (old != lk->link_up && lk->on_change != NULL)
        lk->on_change(lk->link_up, lk->arg);
}

/*
 * 帧视图回调（framer feed 内同步嵌套调用）：**逐帧即时处置**。
 *
 * 为什么不在回调里只做"配对记录"、回到 recv 外层再统一处置：一次 feed
 * 可能交付多个粘包帧，交接区只有一组变量，后一帧会覆盖前一帧的配对结果，
 * 导致前一帧的事务永远不被完成（首版实现踩过，单测"应答+上报粘包"抓出）。
 * 而在回调内直接处置是安全的：本回调就是唯一的表遍历者，同步嵌套、无并发；
 * stage 的生命周期只覆盖当次回调，下一帧到来时上一回调已返回。
 */
static void on_framed(const iv_frame_t *fr, void *arg)
{
    iv_link_t *lk = arg;
    int i;
    int slot = -1;

    /* 任一合法帧都是链路存活的凭据（DOWN→UP 触发 on_change） */
    link_set_up(lk, 1);

    if (fr->len != 0)
        memcpy(lk->stage, fr->data, fr->len);

    for (i = 0; i < IV_LINK_INFLIGHT_MAX; i++) {
        if (lk->txn[i].active && lk->txn[i].cmd == fr->cmd) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* 无配对事务：上报帧 / 意外应答，交上层 */
        if (lk->on_upstream != NULL)
            lk->on_upstream(fr->cmd, lk->stage, fr->len, lk->arg);
        return;
    }

    /* 有配对事务：先摘槽（清 active），再回调 —— 回调内可安全 send/clear */
    {
        iv_link_done_cb done = lk->txn[slot].on_done;
        void *cb_arg = lk->txn[slot].cb_arg;

        memset(&lk->txn[slot], 0, sizeof(lk->txn[slot]));
        if (done != NULL)
            done(IV_OK, fr->cmd, lk->stage, fr->len, cb_arg);
    }
}

void iv_link_recv(iv_link_t *lk, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    if (lk == NULL || (buf == NULL && len != 0))
        return;
    iv_framer_feed(&lk->framer, buf, len, now_ms, on_framed, lk);
}

void iv_link_tick(iv_link_t *lk, uint32_t now_ms)
{
    int i;

    if (lk == NULL)
        return;

    for (i = 0; i < IV_LINK_INFLIGHT_MAX; i++) {
        uint8_t frame[IV_FRAME_MAX];
        int n;

        if (!lk->txn[i].active)
            continue;
        if ((int32_t)(now_ms - lk->txn[i].deadline_ms) < 0)
            continue;

        if (lk->txn[i].retries < lk->cfg.max_retries) {
            /* 重传：用槽内原件重组同一帧，重排 deadline */
            n = iv_frame_build(IV_FRAME_HEAD_DOWN, lk->txn[i].cmd,
                               lk->txn[i].data, lk->txn[i].len,
                               frame, sizeof(frame));
            lk->txn[i].retries++;
            lk->txn[i].deadline_ms = now_ms + lk->cfg.timeout_ms;
            if (n > 0)
                lk->on_tx(frame, (size_t)n, lk->arg);
            continue;
        }

        /* 重传耗尽：摘槽后判失败 */
        {
            iv_link_done_cb done = lk->txn[i].on_done;
            void *cb_arg = lk->txn[i].cb_arg;
            uint8_t cmd = lk->txn[i].cmd;

            memset(&lk->txn[i], 0, sizeof(lk->txn[i]));
            if (lk->link_up) {
                lk->fail_streak++;
                if (lk->fail_streak >= lk->cfg.fail_threshold)
                    link_set_up(lk, 0);
            }
            if (done != NULL)
                done(IV_ETIMEDOUT, cmd, NULL, 0, cb_arg);
        }
    }
}

int iv_link_send(iv_link_t *lk, uint8_t cmd, const void *data, uint16_t len,
                 uint32_t now_ms, iv_link_done_cb on_done, void *cb_arg)
{
    uint8_t frame[IV_FRAME_MAX];
    int n;
    int i;
    int slot = -1;

    if (lk == NULL || (data == NULL && len != 0))
        return IV_EINVAL;
    if (len > IV_FRAME_DATA_MAX)
        return IV_ERANGE;

    for (i = 0; i < IV_LINK_INFLIGHT_MAX; i++) {
        if (lk->txn[i].active) {
            if (lk->txn[i].cmd == cmd)
                return IV_EBUSY; /* 同 cmd 并发：协议无法区分应答归属 */
        } else if (slot < 0) {
            slot = i;
        }
    }
    if (slot < 0)
        return IV_EFULL;

    n = iv_frame_build(IV_FRAME_HEAD_DOWN, cmd, data, len, frame, sizeof(frame));
    if (n < 0)
        return n;

    lk->txn[slot].active = 1;
    lk->txn[slot].cmd = cmd;
    lk->txn[slot].retries = 0;
    lk->txn[slot].len = len;
    lk->txn[slot].deadline_ms = now_ms + lk->cfg.timeout_ms;
    lk->txn[slot].on_done = on_done;
    lk->txn[slot].cb_arg = cb_arg;
    if (len != 0)
        memcpy(lk->txn[slot].data, data, len);

    lk->on_tx(frame, (size_t)n, lk->arg);
    return IV_OK;
}

void iv_link_clear(iv_link_t *lk, int rc)
{
    int i;

    if (lk == NULL)
        return;
    for (i = 0; i < IV_LINK_INFLIGHT_MAX; i++) {
        iv_link_done_cb done;
        void *cb_arg;
        uint8_t cmd;

        if (!lk->txn[i].active)
            continue;
        done = lk->txn[i].on_done;
        cb_arg = lk->txn[i].cb_arg;
        cmd = lk->txn[i].cmd;
        memset(&lk->txn[i], 0, sizeof(lk->txn[i]));
        if (done != NULL)
            done(rc, cmd, NULL, 0, cb_arg);
    }
}

int iv_link_is_up(const iv_link_t *lk)
{
    return (lk != NULL && lk->link_up) ? 1 : 0;
}
