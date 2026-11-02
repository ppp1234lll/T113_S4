/*
 * 平台 0xB3 的**结果帧**（升级成功/失败上报）—— 单测  2026-11-02
 *
 * ============================ 它验的是什么 ============================
 * 结果帧是这条链路上唯一**跨重启**的东西：下载成功那一刻还不知道"升级成不成功"，
 * 要等新版本重启后跑通装配才算成功。所以判据全在 `pending.json` 与
 * `ota/report` 这两个文件上，而它们的内容由 `iv_upd` 与 `monitor.sh` 写 ——
 * 这个单测直接**自己造这两个文件**，把四种结论都钉住：
 *
 *   s1 monitor 写的安装失败（`ota/report` = `0 install-failed`）⇒ 报 0x00
 *   s2 pending.json state=pending（新版本首启成功）          ⇒ 报 0x01
 *   s3 同一版本同一结论第二次开机                              ⇒ **不重报**（去重）
 *   s4 pending.json state=rolled-back（自检超时已回滚）        ⇒ 报 0x00
 *   s5 链路未 UP 时 flush 不发、结果留在队列里（retries++）    ⇒ 链路 UP 后发出
 *
 * 这里不发真 socket：`iv_proto_send()` 只要求一个 `on_tx` 回调，抓字节比开端口
 * 更能定位（能直接读 cmd/载荷）。真链路上的那一段由 `platcmd_e2e.py` 覆盖。
 *
 * ============================ 反向证伪（改完必须能看到红） ============================
 *   F1 把 `ota/request`… 那句 `ota_report_take()` 的 `unlink()` 注释掉 ⇒ s1 第二次调用
 *      仍会重新排队（"读到不消费"），`ota_result_pending` 断言红；
 *   F2 把 `ota_reported_has()` 恒返回 0（去掉去重）⇒ s3 红；
 *   F3 把 `st.active` 判据换成 `st.pending`（pending 存在就算成功，不看 state）⇒
 *      s4 红（回滚后的状态被当成成功报 0x01）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_agent.h"
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

/* ---------------------------------------------------------------------------
 * 抓帧：把 `iv_proto_send()` 出去的字节留在内存里
 * ------------------------------------------------------------------------- */

static unsigned char g_tx[512];
static size_t        g_txlen;
static unsigned      g_txcnt;
static int           g_fr_cmd = -1;
static int           g_fr_data0 = -1;

/* 上行帧按**固定偏移**读：`iv_proto_parser_*` 只认下行帧头 `0xF0F0`
 * （`iv_proto.c` 文件头写着"滑动窗口找头 0xF0F0"），拿它解上行帧会一条都解不出来
 * —— 这个坑在 e2e 里也踩过一次（那里是照下行头去认 ACK）。帧布局与下行同构：
 * `0F 0F | ver | type | devid(3) | cmd | qn1 | qn2 | len | data…`，共 21 + len 字节。 */
#define UP_OFF_CMD  8u
#define UP_OFF_LEN  17u
#define UP_OFF_DATA 18u

static int tx_stub(const uint8_t *bytes, size_t len, void *arg)
{
    (void)arg;
    if (len > sizeof g_tx)
        return IV_ERANGE;
    memcpy(g_tx, bytes, len);
    g_txlen = len;
    g_txcnt++;

    g_fr_cmd   = (len > UP_OFF_CMD) ? (int)bytes[UP_OFF_CMD] : -1;
    g_fr_data0 = (len > UP_OFF_DATA && bytes[UP_OFF_LEN] > 0u) ? (int)bytes[UP_OFF_DATA]
                                                               : -1;
    return IV_OK;
}

static int last_frame_cmd(void)
{
    return g_fr_cmd;
}

static int last_frame_data0(void)
{
    return g_fr_data0;
}

/* ---------------------------------------------------------------------------
 * 造文件
 * ------------------------------------------------------------------------- */

static char g_root[128];

static void put(const char *rel, const char *text)
{
    char  path[256];
    FILE *f;

    (void)snprintf(path, sizeof path, "%s/%s", g_root, rel);
    f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "SKIP: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    (void)fputs(text, f);
    (void)fclose(f);
}

static void rm_rel(const char *rel)
{
    char path[256];

    (void)snprintf(path, sizeof path, "%s/%s", g_root, rel);
    (void)unlink(path);
}

static int exists(const char *rel)
{
    char path[256];

    (void)snprintf(path, sizeof path, "%s/%s", g_root, rel);
    return (access(path, F_OK) == 0) ? 1 : 0;
}

static int slurp_rel(const char *rel, char *out, size_t cap)
{
    char  path[256];
    FILE *f;
    char *p;

    (void)snprintf(path, sizeof path, "%s/%s", g_root, rel);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    out[0] = '\0';
    if (fgets(out, (int)cap, f) == NULL)
        out[0] = '\0';
    (void)fclose(f);
    p = out + strlen(out);
    while (p > out && (p[-1] == '\n' || p[-1] == '\r'))
        *--p = '\0';
    return 0;
}

/* `iv_agent_t` 约 20 KiB（含 report/updf 两个大句柄）：**不要放栈上** */
static iv_agent_t g_a;

int main(void)
{
    char  buf[128];
    char  cmd[256];
    int   rc;
    unsigned before;

    (void)snprintf(g_root, sizeof g_root, "/tmp/ivtares_%d", (int)getpid());
    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    (void)system(cmd);
    (void)snprintf(cmd, sizeof cmd, "mkdir -p '%s/ota'", g_root);
    if (system(cmd) != 0) {
        fprintf(stderr, "SKIP: cannot create %s\n", g_root);
        return 0;
    }

    memset(&g_a, 0, sizeof g_a);
    g_a.cfg.ota_root = g_root;
    g_a.opened       = 1;
    g_a.now_ms       = 1000u;
    /* 造一个能组帧的最小协议上下文（不连 socket：on_tx 就是存内存） */
    g_a.rp.pf.id.devtype = 0x0001u;
    g_a.rp.pf.id.devid   = 0x123456u;
    g_a.rp.pf.on_tx      = tx_stub;
    g_a.rp.tr.state      = (uint8_t)IV_TRANSPORT_UP;

    /* ---- s1：monitor 写的安装失败 ⇒ 0x00 ---- */
    put("ota/report", "0 install-failed\n");
    rc = iv_agent_ota_boot_result(&g_a);
    chk(rc == 1, "s1 install-failed report -> queued");
    chk(iv_agent_ota_result_pending(&g_a) == 2, "s1 pending result == fail(2)");
    chk(exists("ota/report") == 0, "s1 report file consumed (deleted)");
    before = g_txcnt;
    iv_agent_ota_flush(&g_a);
    chk(g_txcnt == before + 1u, "s1 one frame sent");
    if (last_frame_cmd() != (int)IV_PROTO_CMD_UPDATE || last_frame_data0() != 0x00) {
        size_t k; /* 只在不符合预期时把整帧打出来，正常路径不刷屏 */

        fprintf(stderr, "  s1 frame (%u B):", (unsigned)g_txlen);
        for (k = 0u; k < g_txlen; k++)
            fprintf(stderr, " %02X", g_tx[k]);
        fprintf(stderr, "\n");
    }
    chk(last_frame_cmd() == (int)IV_PROTO_CMD_UPDATE, "s1 frame cmd == 0xB3");
    chk(last_frame_data0() == 0x00, "s1 frame payload == 0x00 (fail)");
    chk(iv_agent_ota_result_pending(&g_a) == 0, "s1 queue cleared after send");
    chk(slurp_rel("ota/reported", buf, sizeof buf) == 0 && strcmp(buf, "- fail") == 0,
        "s1 ota/reported == \"- fail\"");

    /* 重复调用：report 已被消费，pending.json 也不存在 ⇒ 没有可报的 */
    rc = iv_agent_ota_boot_result(&g_a);
    chk(rc == 0, "s1b nothing to report the second time");

    /* ---- s2：pending.json state=pending ⇒ 0x01 ---- */
    put("ota/pending.json",
        "{\"fmt\":1,\"state\":\"pending\",\"version\":\"9.9.9\",\"previous\":\"1.0.0\","
        "\"app_dir\":\"/opt/ivsbox/releases/9.9.9\",\"bootcount\":1,\"max_boot\":3,"
        "\"reason\":\"\"}\n");
    rc = iv_agent_ota_boot_result(&g_a);
    chk(rc == 1, "s2 pending upgrade -> queued");
    chk(iv_agent_ota_result_pending(&g_a) == 1, "s2 pending result == ok(1)");
    before = g_txcnt;
    iv_agent_ota_flush(&g_a);
    chk(g_txcnt == before + 1u, "s2 one frame sent");
    chk(last_frame_data0() == 0x01, "s2 frame payload == 0x01 (ok)");
    chk(slurp_rel("ota/reported", buf, sizeof buf) == 0 && strcmp(buf, "9.9.9 ok") == 0,
        "s2 ota/reported == \"9.9.9 ok\"");

    /* ---- s3：同一版本同一结论，第二次开机不重报 ---- */
    rc = iv_agent_ota_boot_result(&g_a);
    chk(rc == 0, "s3 same version+result is de-duplicated");
    chk(iv_agent_ota_result_pending(&g_a) == 0, "s3 nothing queued");

    /* ---- s4：state=rolled-back ⇒ 0x00 ---- */
    put("ota/pending.json",
        "{\"fmt\":1,\"state\":\"rolled-back\",\"version\":\"9.9.9\",\"previous\":\"1.0.0\","
        "\"app_dir\":\"/opt/ivsbox/releases/9.9.9\",\"bootcount\":4,\"max_boot\":3,"
        "\"reason\":\"selftest-failed\"}\n");
    rc = iv_agent_ota_boot_result(&g_a);
    chk(rc == 1, "s4 rolled-back -> queued");
    chk(iv_agent_ota_result_pending(&g_a) == 2, "s4 pending result == fail(2)");
    iv_agent_ota_flush(&g_a);
    chk(last_frame_data0() == 0x00, "s4 frame payload == 0x00 (fail)");
    chk(slurp_rel("ota/reported", buf, sizeof buf) == 0 && strcmp(buf, "9.9.9 fail") == 0,
        "s4 ota/reported == \"9.9.9 fail\"");

    /* ---- s5：链路未 UP 时留在队列里，UP 之后才发 ---- */
    (void)unlink("/tmp/does-not-matter");
    rm_rel("ota/pending.json");
    put("ota/pending.json",
        "{\"fmt\":1,\"state\":\"pending\",\"version\":\"10.1.0\",\"previous\":\"9.9.9\","
        "\"app_dir\":\"/opt/ivsbox/releases/10.1.0\",\"bootcount\":1,\"max_boot\":3,"
        "\"reason\":\"\"}\n");
    chk(iv_agent_ota_boot_result(&g_a) == 1, "s5 new version queued");
    g_a.rp.tr.state = (uint8_t)IV_TRANSPORT_DOWN;
    before = g_txcnt;
    iv_agent_ota_flush(&g_a);
    chk(g_txcnt == before, "s5 link down -> nothing sent");
    chk(iv_agent_ota_result_pending(&g_a) == 1, "s5 result kept in queue");
    chk(g_a.ota_result_retries >= 1u, "s5 retry counter incremented");
    g_a.rp.tr.state = (uint8_t)IV_TRANSPORT_UP;
    iv_agent_ota_flush(&g_a);
    chk(g_txcnt == before + 1u, "s5 sent after link came up");
    chk(last_frame_data0() == 0x01, "s5 payload == 0x01");

    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    (void)system(cmd);

    if (g_fail != 0) {
        fprintf(stderr, "agent_otares: %d checks FAILED\n", g_fail);
        return 1;
    }
    printf("agent_otares: all checks passed\n");
    return 0;
}
