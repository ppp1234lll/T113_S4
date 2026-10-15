/*
 * ivsboxd 受控远程通道端到端 —— 集成测试（功能开发计划 M8-S8.3）
 *
 * ============================ 它与单测的分工 ============================
 * `tests/unit/test_tunnel.c` 钉的是**模块本身**（白名单 / 钥匙 / 落盘 / 到期边界），
 * `tests/unit/test_ctl.c` 用假落点钉**方法路由**。两者都证明不了
 * `main.c` 的装配真的把 `iv_tunnel` 接进了 `iv_ctl` 的上下文 —— 接线错误恰恰是
 * 单测抓不到的那类缺陷（见项目教训：接线在装配层）。
 * 本用例因此是**客户端，不自己起服务端**，与 test_ctl_snapshot 同一口径：
 *   - VM：`build/host/ivsboxd &` 之后跑本用例；
 *   - 板端：`/etc/init.d/S65ivsboxd start` 之后直接跑 build/arm/tests/integration/ 下的用例。
 *
 * ============================ 覆盖的五件事 ============================
 *   1) `tunnel.status` 有应答 ⇒ 上下文里的 tunnel 指针真的接上了（没接是 IV_ENOTSUP，
 *      那是"能调但没接"，装配层必须背锅）；
 *   2) `tunnel.enable {"svcs":["web"]}` ⇒ **标记文件真的出现**、`frpc.toml` 真的落盘
 *      且权限 **0600**（里面有 secretKey）、内容里只有环回地址；
 *   3) 状态从 off 翻到 on，且响应里**从不出现** secretKey 与令牌；
 *   4) 重复开启被 IV_ESTATE 拒掉；
 *   5) `tunnel.disable` ⇒ 标记消失、配置保留（0600 不因关闭而丢）。
 *
 * ============================ 用法 ============================
 *   test_tunnel_e2e [socket_path] [conf_path] [flag_path]
 * conf/flag 省略时取环境变量 IVSBOX_TUNNEL_CONF / IVSBOX_TUNNEL_FLAG；两者都没有
 * 就跳过文件断言（只验协议外形）。验证时建议：
 *   IVSBOX_TUNNEL_CONF=/tmp/x/frpc.toml IVSBOX_TUNNEL_FLAG=/tmp/x/t.enabled \
 *   IVSBOX_TUNNEL_SN=TESTSN IVSBOX_TUNNEL_TOKEN=tok \
 *   build/host/ivsboxd &   然后   build/host/tests/integration/test_tunnel_e2e
 * 退出码：0 = 全部通过；1 = 有用例失败。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_ret.h"

#define RECV_TIMEOUT_MS    3000
#define SILENCE_TIMEOUT_MS 800
#define CONNECT_RETRY_MS   5000
#define CONNECT_RETRY_STEP 250

static int g_fail;

static unsigned char g_rx[IV_CHAN_RECV_CAP_MIN];

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static int connect_retry(const char *path)
{
    int waited = 0;
    int fd;

    for (;;) {
        fd = iv_chan_connect(path);
        if (fd >= 0)
            return fd;
        if (fd != IV_ENOENT && fd != IV_EBUSY)
            return fd;
        if (waited >= CONNECT_RETRY_MS)
            return fd;
        (void)usleep((unsigned)(CONNECT_RETRY_STEP * 1000));
        waited += CONNECT_RETRY_STEP;
    }
}

/* 发一条 JSON 请求并收响应，载荷拷成 C 串放进 out（含 NUL 的长度回 *out_len）。 */
static int round_trip(int fd, const char *json, char *out, size_t outcap, size_t *out_len)
{
    ivs_chan_hdr_t hdr;
    size_t         payload_len = 0;
    size_t         n           = strlen(json);
    int            rc;
    int            i;

    if (iv_chan_hdr_init(&hdr, IV_CHAN_TYPE_REQ, 0x0DDBA11ull, 0u, (uint32_t)n) != IV_OK)
        return IV_EFAIL;
    rc = iv_chan_send(fd, &hdr, json, n);
    if (rc != IV_OK)
        return rc;

    for (i = 0; i < 3; i++) {
        rc = iv_chan_recv_timeout(fd, g_rx, sizeof g_rx, &hdr, &payload_len,
                                  RECV_TIMEOUT_MS);
        if (rc != IV_EAGAIN)
            break;
    }
    if (rc != IV_OK)
        return rc;
    if (hdr.type != IV_CHAN_TYPE_RSP || hdr.request_id != 0x0DDBA11ull)
        return IV_EPROTO;
    if (payload_len == 0u || payload_len >= outcap)
        return IV_ERANGE;
    memcpy(out, g_rx + IV_CHAN_HDR_SIZE, payload_len);
    out[payload_len] = '\0';
    if (out_len != NULL)
        *out_len = payload_len;
    return IV_OK;
}

/* 请求并要求 ok:true；失败时把响应打出来方便定位。 */
static int expect_ok(int fd, const char *json, char *out, size_t outcap)
{
    int rc = round_trip(fd, json, out, outcap, NULL);

    if (rc != IV_OK) {
        fprintf(stderr, "  (%s: transport rc=%d)\n", json, rc);
        return 0;
    }
    if (strstr(out, "\"ok\":true") == NULL) {
        fprintf(stderr, "  (%s: got %s)\n", json, out);
        return 0;
    }
    return 1;
}

int main(int argc, char *argv[])
{
    const char *path = (argc > 1) ? argv[1] : IV_CHAN_PATH_DEFAULT;
    const char *conf = (argc > 2) ? argv[2] : getenv("IVSBOX_TUNNEL_CONF");
    const char *flag = (argc > 3) ? argv[3] : getenv("IVSBOX_TUNNEL_FLAG");
    char        body[1024];
    int         fd;
    int         rc;

    printf("test_tunnel_e2e: target %s conf=%s flag=%s\n", path,
           (conf != NULL) ? conf : "-", (flag != NULL) ? flag : "-");

    fd = connect_retry(path);
    if (fd < 0) {
        fprintf(stderr,
                "FAIL: cannot connect to %s (rc=%d)%s\n", path, fd,
                (fd == IV_ENOENT) ? " -- ivsboxd is not running, or it never created the "
                                    "listen point (start it first)"
                                  : "");
        return 1;
    }

    /* 1) 状态有应答 ⇒ 装配层真的把通道接进了路由上下文 */
    rc = round_trip(fd, "{\"m\":\"tunnel.status\"}", body, sizeof body, NULL);
    chk(rc == IV_OK, "tunnel.status is answered (tunnel wired into iv_ctl ctx)");
    if (rc == IV_OK) {
        chk(strstr(body, "\"ok\":true") != NULL, "status reports ok");
        chk(strstr(body, "\"sn\":\"") != NULL,
            "status carries the device sn (identity was wired)");
        chk(strstr(body, "secret") == NULL, "no secretKey in a status response");
    }

    /* 2) 初始为关 */
    rc = round_trip(fd, "{\"m\":\"tunnel.status\"}", body, sizeof body, NULL);
    if (rc == IV_OK)
        chk(strstr(body, "\"on\":false") != NULL, "channel starts closed");

    /* 3) 开通道 */
    chk(expect_ok(fd, "{\"m\":\"tunnel.enable\",\"p\":{\"svcs\":[\"web\"],\"ttl\":120}}",
                  body, sizeof body),
        "tunnel.enable {web} accepted");
    if (conf != NULL) {
        struct stat st;

        chk(stat(conf, &st) == 0, "frpc.toml was written");
        if (stat(conf, &st) == 0) {
            chk((st.st_mode & 0777u) == 0600u,
                "frpc.toml mode is 0600 (secretKey inside)");
        }
    }
    if (flag != NULL) {
        struct stat st;

        chk(stat(flag, &st) == 0, "flag file was created");
        if (stat(flag, &st) == 0) {
            chk((st.st_mode & 0777u) == 0644u, "flag file mode is 0644");
        }
    }

    /* 4) 配置里只有环回目标、代理一律 stcp，且绝不出现私网摄像机地址 */
    if (conf != NULL) {
        FILE  *fp = fopen(conf, "r");
        char   buf[4096];
        size_t n;

        chk(fp != NULL, "frpc.toml readable");
        if (fp != NULL) {
            n = fread(buf, 1u, sizeof buf - 1u, fp);
            buf[n] = '\0';
            (void)fclose(fp);
            chk(strstr(buf, "localIP = \"127.0.0.1\"") != NULL,
                "web proxy targets the loopback adapter");
            chk(strstr(buf, "type = \"stcp\"") != NULL, "proxies are stcp");
            chk(strstr(buf, "type = \"tcp\"") == NULL, "no plain tcp proxy ever");
            chk(strstr(buf, "192.168.") == NULL && strstr(buf, "10.") == NULL,
                "no private-lan address ever lands in frpc.toml");
            chk(strstr(buf, "secretKey = \"") != NULL, "per-service secretKey present");
        }
    }

    /* 5) 状态翻到 on，重复开启被拒 */
    rc = round_trip(fd, "{\"m\":\"tunnel.status\"}", body, sizeof body, NULL);
    if (rc == IV_OK) {
        chk(strstr(body, "\"on\":true") != NULL, "status now reports on");
        chk(strstr(body, "\"remain_s\":") != NULL, "status carries remaining ttl");
    }
    rc = round_trip(fd, "{\"m\":\"tunnel.enable\",\"p\":{\"svcs\":[\"web\"]}}", body,
                    sizeof body, NULL);
    chk(rc == IV_OK && strstr(body, "\"e\":-7") != NULL,
        "double enable is refused with IV_ESTATE(-7)");

    /* 6) 关闭：标记消失、配置保留 */
    chk(expect_ok(fd, "{\"m\":\"tunnel.disable\"}", body, sizeof body),
        "tunnel.disable accepted");
    if (flag != NULL) {
        struct stat st;

        chk(stat(flag, &st) != 0, "flag file removed on disable");
    }
    if (conf != NULL) {
        struct stat st;

        chk(stat(conf, &st) == 0, "frpc.toml kept after disable (0600, not lost)");
    }
    rc = round_trip(fd, "{\"m\":\"tunnel.status\"}", body, sizeof body, NULL);
    if (rc == IV_OK)
        chk(strstr(body, "\"stop\":\"local\"") != NULL,
            "last session records stop reason local");

    (void)close(fd);

    if (g_fail != 0) {
        fprintf(stderr, "test_tunnel_e2e FAILED (%d failed checks)\n", g_fail);
        return 1;
    }
    printf("test_tunnel_e2e passed (wiring, 0600 conf, loopback-only, estate, disable)\n");
    return 0;
}
