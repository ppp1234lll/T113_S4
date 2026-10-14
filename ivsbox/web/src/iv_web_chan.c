/*
 * ivsbox-web 本地通道客户端（M5-S5.1）
 *
 * 与 ivsboxd 之间**只有这一条**数据通路（见 iv_web.h 文件头）。职责就是把一段
 * JSON 请求发过去、把一段 JSON 响应拿回来，顺手维护连接的生命周期。
 *
 * 两个刻意的简单化，都写在这里免得以后当成 bug：
 *   1. **连接复用但不做并发**：本进程是单线程事件循环，一次只处理一个请求，
 *      所以一条 fd 足够；不需要连接池。
 *   2. **断连只重试一次**：`iv_chan_send` 报 `IV_ECONN` 多半是 ivsboxd 重启了。
 *      丢帧重连再试一次即可；再失败就如实把错误抛给上层（HTTP 502），
 *      **不在这儿死循环重试** —— 那会把一次 502 拖成浏览器侧的超时。
 */
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_ret.h"
#include "iv_web.h"

/* 单包接收缓冲：给到通道的最小完整容量，保证任何合法应答都收得下。 */
static unsigned char s_rx[IV_CHAN_RECV_CAP_MIN];

/* 请求关联号：进程内单调递增即可（响应里回填，用来配对）。 */
static uint64_t s_reqid = 1u;

/* 单次请求的等待预算。ivsboxd 侧全是内存操作 / 立即返回，3 s 足够宽裕。 */
#define IV_WEB_CHAN_TIMEOUT_MS 3000

void iv_web_chan_close(iv_web_ctx_t *ctx)
{
    if (ctx != NULL && ctx->chan_fd >= 0) {
        (void)close(ctx->chan_fd);
        ctx->chan_fd = -1;
    }
}

/* 一次"连（如需）→ 发 → 收"。不做重试。 */
static int chan_call_once(iv_web_ctx_t *ctx, const char *req, size_t req_len,
                          char *out, size_t cap, size_t *out_len)
{
    ivs_chan_hdr_t hdr;
    size_t         plen = 0;
    int            rc;
    int            tries;

    if (ctx->chan_fd < 0) {
        ctx->chan_fd = iv_chan_connect(ctx->chan_path);
        if (ctx->chan_fd < 0)
            return ctx->chan_fd; /* 负的 IV_* 码直接透传（IV_ENOENT = 主控没起） */
    }

    if (iv_chan_hdr_init(&hdr, IV_CHAN_TYPE_REQ, s_reqid++, 0u,
                         (uint32_t)req_len) != IV_OK)
        return IV_EFAIL;

    rc = iv_chan_send(ctx->chan_fd, &hdr, req, req_len);
    if (rc != IV_OK)
        return rc;

    /* poll 报可读之后 recv 仍可能 EAGAIN（通道头文件已备案），重试几次即可。 */
    for (tries = 0; tries < 3; tries++) {
        rc = iv_chan_recv_timeout(ctx->chan_fd, s_rx, sizeof(s_rx), &hdr, &plen,
                                  IV_WEB_CHAN_TIMEOUT_MS);
        if (rc != IV_EAGAIN)
            break;
    }
    if (rc != IV_OK)
        return rc;
    if (hdr.type != IV_CHAN_TYPE_RSP)
        return IV_EPROTO;
    if (plen == 0u || plen >= cap)
        return IV_ERANGE;

    memcpy(out, s_rx + IV_CHAN_HDR_SIZE, plen);
    out[plen] = '\0';
    *out_len = plen;
    return IV_OK;
}

int iv_web_chan_call(iv_web_ctx_t *ctx, const char *req, size_t req_len,
                     char *out, size_t cap, size_t *out_len)
{
    int rc;

    if (ctx == NULL || req == NULL || out == NULL || out_len == NULL)
        return IV_EINVAL;
    if (req_len == 0u || req_len > (size_t)IV_CHAN_MAX_PAYLOAD)
        return IV_EINVAL;
    if (ctx->chan_path == NULL)
        return IV_EINVAL;

    rc = chan_call_once(ctx, req, req_len, out, cap, out_len);
    if (rc == IV_ECONN) {
        /* 多半是 ivsboxd 重启：丢掉旧连接，重连再试一次。 */
        iv_web_chan_close(ctx);
        rc = chan_call_once(ctx, req, req_len, out, cap, out_len);
    }
    return rc;
}
