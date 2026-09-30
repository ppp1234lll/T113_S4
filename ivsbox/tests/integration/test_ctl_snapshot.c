/*
 * ivsboxd 本地通道端到端往返 —— 集成测试（功能开发计划 M1 退出条件第 2 条）
 *
 * ============================ 它与单测的分工 ============================
 * `tests/unit/test_chan.c` 验的是**通道模块本身**：用 socketpair 或自建接听点
 * 覆盖头布局、鉴权、超长/短包、超时等，全程不依赖任何外部进程。
 * 本用例验的是**装好之后的 ivsboxd 进程**：它按 S10 的装配顺序把日志/配置/
 * SQLite/慢任务池/Reactor/本地通道/健康线程全部拉起，在默认路径挂上接听点，
 * 并且真的能对 `system.snapshot` 作出应答。
 *
 * 因此本用例**是客户端，不自己起服务端** —— 服务必须已经在跑：
 *   - 板端：`/etc/init.d/S65ivsboxd start` 拉起（或手工 `build/arm/ivsboxd &`）；
 *   - 宿主机：`build/host/ivsboxd &`。
 * 这也正是 M1 退出条件那条"板端本地通道收发可验证"要证明的东西 —— 单测证明
 * 不了 `main.c` 的装配是否真的把接听点挂上了。
 *
 * ============================ 覆盖的四件事 ============================
 *   1) 能连上接听点（服务确实在跑，且 `main.c` 的装配把它建起来了）；
 *   2) `system.snapshot` 收到 `IV_CHAN_TYPE_RSP`，且 `request_id` **原样回填**；
 *   3) 同一连接上第二次请求同样正确配对（连接可复用、配对不串号）；
 *   4) 未实现的方法**不产生任何应答**（S10 现状：静默忽略）—— 用 `recv_timeout`
 *      到期来证明"没回"，而不是"回了个错的"；随后再发一次合法请求，确认这条
 *      连接没有被那条无应答的请求打乱。
 *
 * ============================ 用法 ============================
 *   test_ctl_snapshot [socket_path]
 * socket_path 省略时用 `IV_CHAN_PATH_DEFAULT`（`/var/run/ivsbox/ctl`）。
 * 若服务尚未就绪，本用例会先重试连接最多 5 秒（装配 + 挂接听点需要一点时间），
 * 超时后报 FAIL 并明确提示"ivsboxd 没在跑"，不会给出含义模糊的失败。
 * 退出码：0 = 全部通过；1 = 有用例失败。
 *
 * 本用例不写任何文件，因此没有需要清理的临时产物（AGENTS.md 规则 6）。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_ret.h"

#define METHOD_SNAPSHOT     "system.snapshot"
#define METHOD_UNKNOWN      "no.such.method"

/* 等待响应的预算。本地 UDS 往返是微秒级，这里留足余量只为抗宿主/板端调度抖动。*/
#define RECV_TIMEOUT_MS     3000
/* "应当没有应答"的等待窗口：够长以排除慢应答，够短以免拖长用例。*/
#define SILENCE_TIMEOUT_MS  800
/* 连接重试：等服务完成装配并挂上接听点。*/
#define CONNECT_RETRY_MS    5000
#define CONNECT_RETRY_STEP  250

static int g_fail;

/* 收发缓冲放静态区：IV_CHAN_RECV_CAP_MIN 是 64 KiB 量级，不占栈。*/
static unsigned char g_rx[IV_CHAN_RECV_CAP_MIN];

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 连接接听点，最多等 CONNECT_RETRY_MS —— 用例常与服务几乎同时被拉起，
 * 服务"尚未挂上接听点"是预期内的瞬态，不是失败。*/
static int connect_retry(const char *path)
{
    int waited = 0;
    int fd;

    for (;;) {
        fd = iv_chan_connect(path);
        if (fd >= 0)
            return fd;
        /* IV_ENOENT = 接听点还不存在（服务没起或还没挂上）；IV_EBUSY = backlog 满，
         * 头文件明确要求按"可重试"处理。其余错误（如路径权限）重试也不会好。*/
        if (fd != IV_ENOENT && fd != IV_EBUSY)
            return fd;
        if (waited >= CONNECT_RETRY_MS)
            return fd;
        (void)usleep((unsigned)(CONNECT_RETRY_STEP * 1000));
        waited += CONNECT_RETRY_STEP;
    }
}

/* 发一条请求并等一条响应。成功返回 IV_OK，并把载荷拷成 C 串放进 out。*/
static int round_trip(int fd, const char *method, unsigned long long request_id,
                      char *out, size_t outcap)
{
    ivs_chan_hdr_t hdr;
    size_t         payload_len = 0;
    size_t         n           = strlen(method);
    int            rc;
    int            i;

    if (iv_chan_hdr_init(&hdr, IV_CHAN_TYPE_REQ, (uint64_t)request_id, 0u,
                         (uint32_t)n) != IV_OK)
        return IV_EFAIL;

    rc = iv_chan_send(fd, &hdr, method, n);
    if (rc != IV_OK)
        return rc;

    /* poll 报可读之后 recv 仍可能返回 IV_EAGAIN（头文件已备案），重试即可；
     * 每次重试都是全新的 timeout 预算，而本地往返本来就该在毫秒内完成。*/
    for (i = 0; i < 3; i++) {
        rc = iv_chan_recv_timeout(fd, g_rx, sizeof g_rx, &hdr, &payload_len,
                                  RECV_TIMEOUT_MS);
        if (rc != IV_EAGAIN)
            break;
    }
    if (rc != IV_OK)
        return rc;

    if (hdr.type != IV_CHAN_TYPE_RSP)
        return IV_EPROTO;
    if (hdr.request_id != (uint64_t)request_id)
        return IV_EPROTO;
    if (payload_len == 0u || payload_len >= outcap)
        return IV_ERANGE;

    memcpy(out, g_rx + IV_CHAN_HDR_SIZE, payload_len);
    out[payload_len] = '\0';
    return IV_OK;
}

int main(int argc, char *argv[])
{
    const char *path = (argc > 1) ? argv[1] : IV_CHAN_PATH_DEFAULT;
    char        body[512];
    int         fd;
    int         rc;

    printf("test_ctl_snapshot: target %s\n", path);

    fd = connect_retry(path);
    if (fd < 0) {
        fprintf(stderr,
                "FAIL: cannot connect to %s (rc=%d)%s\n",
                path, fd,
                (fd == IV_ENOENT) ? " -- ivsboxd is not running, or it never created the "
                                    "listen point (start it first: build/host/ivsboxd &)"
                                  : "");
        return 1;
    }

    /* 1) 首次请求 + request_id 配对 */
    rc = round_trip(fd, METHOD_SNAPSHOT, 0x0102030405060708ull, body, sizeof body);
    chk(rc == IV_OK, "system.snapshot is answered with a response");
    if (rc == IV_OK) {
        chk(strstr(body, "system.snapshot") != NULL, "the answer names the method");
        chk(strstr(body, "\"status\":\"ok\"") != NULL, "the answer reports status ok");
        chk(strstr(body, "\"version\":\"") != NULL, "the answer carries a version string");
    } else {
        fprintf(stderr, "  (round_trip rc=%d, expected %d=IV_OK)\n", rc, IV_OK);
    }

    /* 2) 同一连接上第二次请求：连接可复用，且配对必须重新算 */
    rc = round_trip(fd, METHOD_SNAPSHOT, 0x8877665544332211ull, body, sizeof body);
    chk(rc == IV_OK, "a second request on the same connection is answered");

    /* 3) 未实现的方法：不得有任何应答。用"等到超时"证明没回，
     *    而不是接受一个内容不对的响应。*/
    {
        ivs_chan_hdr_t hdr;
        size_t         plen = 0u;
        size_t         n    = strlen(METHOD_UNKNOWN);

        chk(iv_chan_hdr_init(&hdr, IV_CHAN_TYPE_REQ, 0xAABBCCDDull, 0u,
                             (uint32_t)n) == IV_OK,
            "build the unknown-method request header");
        chk(iv_chan_send(fd, &hdr, METHOD_UNKNOWN, n) == IV_OK,
            "send the unknown-method request");
        rc = iv_chan_recv_timeout(fd, g_rx, sizeof g_rx, &hdr, &plen, SILENCE_TIMEOUT_MS);
        chk(rc == IV_ETIMEDOUT, "an unknown method produces no response (recv times out)");
        if (rc != IV_ETIMEDOUT)
            fprintf(stderr, "  (unknown-method recv rc=%d, expected %d=IV_ETIMEDOUT)\n",
                    rc, IV_ETIMEDOUT);
    }

    /* 4) 那条无应答的请求不得打乱连接：紧接着的合法请求仍应正常配对 */
    rc = round_trip(fd, METHOD_SNAPSHOT, 0x55ull, body, sizeof body);
    chk(rc == IV_OK, "the connection still works after an unanswered request");

    (void)close(fd);

    if (g_fail != 0) {
        fprintf(stderr, "test_ctl_snapshot FAILED (%d failed checks)\n", g_fail);
        return 1;
    }
    printf("test_ctl_snapshot passed (connect, snapshot, id pairing, unknown-method silence, reuse)\n");
    return 0;
}
