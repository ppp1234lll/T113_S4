/*
 * S2 基础件综合单测：iv_log 日志门面（运行期级别 / 单行格式 / 风暴抑制）
 * - 错误码 44 码黄金表在 tests/unit/test_err.c（更严格），此处仅对 iv_strerror 冒烟；
 * - iv_clock 单调性 / 走时在此验证；iv_version 待 S2 后续补齐，届时并入本文件。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_err.h"
#include "ivsbox/iv_log.h"

#define MAX_LINES 256

static char g_lines[MAX_LINES][384];
static int  g_n;
static int  g_fail;

static void sink(int level, const char *module, const char *line, void *user)
{
    (void)user;
    if (g_n < MAX_LINES) {
        snprintf(g_lines[g_n], sizeof(g_lines[0]), "%d|%s|%s", level, module, line);
    }
    g_n++;
}

static void lines_reset(void)
{
    g_n = 0;
}

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void chk_count(int n, const char *what)
{
    if (g_n != n) {
        fprintf(stderr, "FAIL: %s (expect %d lines, got %d)\n", what, n, g_n);
        g_fail++;
    }
}

static void chk_has(int idx, const char *sub, const char *what)
{
    if (idx >= g_n || !strstr(g_lines[idx], sub)) {
        fprintf(stderr, "FAIL: %s (line[%d] missing \"%s\"; got: %s)\n",
                what, idx, sub, idx < g_n ? g_lines[idx] : "<none>");
        g_fail++;
    }
}

int main(void)
{
    /* 落盘出口显式关闭：本测试全程 sink 模式，不需要文件出口；
     * 不关的话 iv_log_init() 会在宿主机上 mkdir 默认 /opt/log（12:02 条目遗留 1） */
    iv_log_set_root(NULL);
    chk(iv_log_init("test_basic") == 0, "iv_log_init");
    iv_log_set_sink(sink, NULL);

    /* 1) 默认级别 DEBUG 全放行；行格式 级别|模块|正文 */
    IV_LOG_D("app", "debug visible by default");
    chk_count(1, "default level passes DEBUG");
    chk_has(0, "7|app|debug visible by default", "level+module+message");

    /* 2) 运行期过滤：set_level 后 DEBUG 被丢、WARN 通过 */
    iv_log_set_level(LOG_INFO);
    IV_LOG_D("app", "debug filtered");
    IV_LOG_W("app", "warn visible");
    chk_count(2, "set_level filters DEBUG but not WARN");

    /* 3) 普通日志不参与抑制；控制字符折叠为单行 */
    iv_log_set_level(LOG_DEBUG);
    for (int i = 0; i < 5; i++)
        IV_LOG_I("link", "plain line %d", i);
    chk_count(7, "plain logs are never suppressed");
    IV_LOG_I("app", "two\nlines");
    chk(g_n == 8 && strchr(g_lines[7], '\n') == NULL, "newline folded to single line");

    /* 4) 码日志：窗口内刷 100 次只出 1 行（首次），正文带码名与码值 */
    lines_reset();
    for (int i = 0; i < 100; i++)
        iv_log_code(LOG_ERR, "link", IV_ERR_NET_CAMERA1_FAULT, "cam1 rtsp timeout");
    chk_count(1, "100 repeats of same code -> 1 line (first only)");
    chk_has(0, "3|link|NET_CAMERA1_FAULT(0x20410000): cam1 rtsp timeout", "first line format");

    /* 5) 恢复行：首次 + 计数共 2 行（计划 §S2 验收口径）；无记录的 recover 静默 */
    iv_log_recover("link", IV_ERR_NET_CAMERA1_FAULT);
    chk_count(2, "recover emits count line -> total 2 lines");
    chk_has(1, "6|link|NET_CAMERA1_FAULT(0x20410000): recovered, x100", "recover count line");
    iv_log_recover("link", IV_ERR_NET_CAMERA1_FAULT);
    chk_count(2, "recover without record is silent");

    /* 6) 未登记码 -> UNKNOWN(0x........) */
    iv_log_code(LOG_ERR, "netmgr", 0xDEADBEEFu, "raw code");
    chk_has(2, "UNKNOWN(0xDEADBEEF): raw code", "unregistered code prints UNKNOWN");

    /* 7) 窗口过期：先补计数行，再按新窗口打本次首次行 */
    iv_log_set_window(1);
    for (int i = 0; i < 5; i++)
        iv_log_code(LOG_ERR, "link", IV_ERR_ELEC_MAIN_AC, "ac lost");
    chk_count(4, "new code logs first line only");
    sleep(2); /* 等 1s 窗口过期 */
    iv_log_code(LOG_ERR, "link", IV_ERR_ELEC_MAIN_AC, "ac lost");
    chk_count(6, "expired window emits count line then new first");
    chk_has(4, "ELEC_MAIN_AC(0x10100000): x5 in", "expired count line");
    chk_has(5, "ELEC_MAIN_AC(0x10100000): ac lost", "new window first line");

    /* 8) 窗口 = 0 关闭抑制：逐条输出 */
    iv_log_set_window(0);
    iv_log_code(LOG_ERR, "link", IV_ERR_SENSOR_TEMP_HIGH, "temp high");
    iv_log_code(LOG_ERR, "link", IV_ERR_SENSOR_TEMP_HIGH, "temp high");
    chk_count(8, "window=0 disables suppression");
    iv_log_set_window(60);

    /* 9) iv_strerror 冒烟（完整 44 码黄金表见 test_err.c） */
    chk(iv_strerror(IV_ERR_OK) != NULL && strcmp(iv_strerror(IV_ERR_OK), "OK") == 0,
        "iv_strerror(OK)");
    chk(iv_strerror(0x7FFFFFFFu) != NULL && strcmp(iv_strerror(0x7FFFFFFFu), "UNKNOWN") == 0,
        "iv_strerror(UNKNOWN)");

    /* 10) iv_clock：时钟活跃（非全 0 桩）+ 单调不减 + 睡 10ms 后必有推进 */
    {
        struct timespec req = {0, 10 * 1000 * 1000};
        uint64_t ms1 = iv_clock_monotonic_ms();
        uint64_t us1 = iv_clock_monotonic_us();
        uint64_t ms2, us2;

        chk(ms1 > 0 || us1 > 0, "clock is live (not the zero stub)");
        nanosleep(&req, NULL);
        ms2 = iv_clock_monotonic_ms();
        us2 = iv_clock_monotonic_us();
        chk(ms2 >= ms1, "monotonic ms non-decreasing");
        chk(us2 >= us1, "monotonic us non-decreasing");
        chk(ms2 - ms1 >= 5, "ms advances across 10ms sleep");
        chk(us2 - us1 >= 5000, "us advances across 10ms sleep");
    }

    if (g_fail) {
        fprintf(stderr, "test_basic failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_basic passed\n");
    return 0;
}
