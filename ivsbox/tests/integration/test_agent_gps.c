/*
 * iv_agent 装配层 GPS 集成用例（功能开发计划 M2-S2.5 的装配与对外出口）
 *
 * ============================ 它验的是什么 ============================
 * `tests/unit/test_gps.c` 证明的是 **解析器本身**对不对（NMEA 语法、校时策略、
 * 上报判据）。本用例证明的是**装配层把它接出去之后**这几件事成立：
 *
 *   1. 字节经 `iv_agent_gps_feed()` 进去 ⇒ 快照经 `iv_agent_gps()` 出来；
 *   2. `gps.status` 这条 ctl 方法的 JSON 形状**与前端约定一致**（未知量是 null，
 *      不是 0 —— 纬度 0 是几内亚湾里的真实位置，拿 0 表示未知会让人以为设备在赤道）；
 *   3. 串口**没开出来**时必须只降级、不致命：agent 仍 open 成功，
 *      `iv_agent_gps_link()` 如实回报"未打开"，而不是假装一切正常。
 *
 * ============================ 为什么不用 Reactor ============================
 * `r == NULL` 模式下本层**刻意不打开 GPS 串口**（没有读驱动，开了也只是白占 fd），
 * 字节来源交给调用方 —— 这正是本用例覆盖的那条路径。Reactor 那条（`on_gps_fd`
 * 真读串口）只能在板端实测，见 `docs/修改记录.md` 同日条目。
 *
 * ============================ 反向证伪点（每条都对应一个真缺陷） ============================
 *   g02 有定位的语句必须让 `valid` 变 true 并把经纬度解出来（含 `fix_updates` 计数）；
 *   g04 全新句柄上只有"没定位"的语句时，经纬度必须是 **JSON null**；
 *       改成 0 顶替会被 `"lat":null` 的断言抓住（`"lat":0` 也会被单独否定）；
 *   g05 空输入必须被拒且不改变状态（`IV_EINVAL`）。
 *
 * ============================ 已登记的口径（不是缺陷） ============================
 *   定位丢失后**经纬度保留上一次有效值**、只把 `valid` 置 0（`on_gga` 的 else 分支）。
 *   这是刻意的：现场要能看"最后定在哪"，但**任何显示都必须以 `valid` 为闸门**，
 *   否则会把旧坐标当成当前位置。Web 侧就是按这个口径做的（`valid=false` 时坐标显示 `—`）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_ctl.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

/* iv_agent_t 约 35 KB ＋ ctl 响应缓冲 ⇒ 一律静态区 */
static iv_agent_t   g_agent;
static iv_ctl_ctx_t g_ctx;
static int          g_fail;
static uint64_t     g_now = 1000u;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void log_discard(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

/* 跑一条 ctl 请求，返回响应文本（静态缓冲；单线程够用）。 */
static const char *ctl(const char *req)
{
    static char out[IV_CTL_RSP_MAX];
    size_t      n = 0;
    int         rc;

    out[0] = '\0';
    rc = iv_ctl_handle(&g_ctx, req, strlen(req), out, sizeof(out), &n);
    if (rc != IV_OK) {
        fprintf(stderr, "ctl(%s) rc=%d\n", req, rc);
        out[0] = '\0';
    }
    return out;
}

static void feed(const char *s)
{
    (void)iv_agent_gps_feed(&g_agent, s, strlen(s));
}

/* 装配一个"串口不开、由调用方喂字节"的 agent（本用例的唯一形态）。 */
static int agent_up(void)
{
    iv_agent_cfg_t cfg;

    iv_agent_cfg_default(&cfg);
    cfg.gps_path     = "";                          /* 刻意不打开串口 */
    cfg.serial_path  = "/dev/nonexistent-board-uart"; /* 采集板链路也不碰真设备 */
    cfg.queue_dir    = "/tmp";
    cfg.report_ms    = 3600u * 1000u;
    cfg.heartbeat_ms = 3600u * 1000u;
    cfg.poll_ms      = 3600u * 1000u;
    iv_agent_init(&g_agent);
    return iv_agent_open(&g_agent, &cfg, NULL, g_now);
}

int main(void)
{
    const iv_gps_t *g;
    const char     *path = NULL;
    int             fd   = -1;
    int             lrc;

    iv_log_set_sink(log_discard, NULL);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.agent = &g_agent;

    /* ---- g01 装配：串口刻意不开 ---- */
    chk(agent_up() == IV_OK, "g01 agent open");
    lrc = iv_agent_gps_link(&g_agent, &path, &fd);
    chk(path != NULL && path[0] != '\0', "g01 link path is reported");
    chk(fd == -1, "g01 no reactor -> serial NOT opened (no dangling fd)");
    chk(lrc == IV_ENOTSUP, "g01 link rc reports 'disabled by config'");
    g = iv_agent_gps(&g_agent);
    chk(g != NULL, "g01 gps handle available");
    chk(g != NULL && g->fix.valid == 0, "g01 no data -> valid = 0");
    chk(iv_agent_gps_rx_bytes(&g_agent) == 0u, "g01 no bytes yet");
    chk(iv_agent_gps_last_parse_ms(&g_agent) == 0u, "g01 nothing parsed yet");

    /* ---- g02 有定位：GGA(quality=1) ＋ RMC(A) ----
     * 校验和是按 NMEA 规则算出来的真值（不是随便填的），否则整句会被丢弃。 */
    feed("$GNGGA,032133.000,3112.8254,N,12127.1234,E,1,09,0.9,12.3,M,0.0,M,,*4B\r\n");
    feed("$GNRMC,032133.000,A,3112.8254,N,12127.1234,E,0.15,88.5,171026,,,A*7B\r\n");
    g = iv_agent_gps(&g_agent);
    chk(g != NULL && g->fix.valid == 1, "g02 valid = 1 after fixed sentences");
    chk(g != NULL && fabs(g->fix.latitude - 31.2137567) < 1e-5, "g02 latitude decoded");
    chk(g != NULL && fabs(g->fix.longitude - 121.4520567) < 1e-5, "g02 longitude decoded");
    chk(g != NULL && g->fix.satellites == 9, "g02 satellites = 9");
    chk(g != NULL && g->fix.fix_quality == 1, "g02 fix_quality = 1");
    /* GGA 与 RMC **各带一份定位信息**，`fix_updates` 是"有效定位更新次数"，
     * 故两句各计一次 = 2（不是 1 —— 别按"一次定位"去理解这个计数）。 */
    chk(g != NULL && g->stats.fix_updates == 2, "g02 fix_updates = 2 (GGA + RMC)");
    chk(iv_agent_gps_rx_bytes(&g_agent) > 0u, "g02 rx bytes counted");
    chk(iv_agent_gps_last_parse_ms(&g_agent) > 0u, "g02 last parse time recorded");

    /* ---- g03 ctl 出口：形状与 null 语义 ---- */
    {
        const char *js = ctl("{\"m\":\"gps.status\"}");

        chk(js[0] != '\0', "g03 gps.status answered");
        chk(strstr(js, "\"valid\":true") != NULL, "g03 JSON valid=true");
        chk(strstr(js, "\"lat\":31.21") != NULL, "g03 JSON lat present as number");
        chk(strstr(js, "\"lon\":121.45") != NULL, "g03 JSON lon present as number");
        chk(strstr(js, "\"satellites\":9") != NULL, "g03 JSON satellites");
        chk(strstr(js, "\"open\":false") != NULL, "g03 JSON link open=false");
        chk(strstr(js, "\"sentences_ok\":2") != NULL, "g03 JSON stats sentences_ok=2");
        chk(strstr(js, "NaN") == NULL, "g03 no NaN leaks into JSON (would be invalid JSON)");
    }

    /* ---- g04 全新句柄 ＋ 只有"没定位"的语句：未知量必须是 null ---- */
    iv_agent_close(&g_agent);
    chk(agent_up() == IV_OK, "g04 reopen");
    /* 这两句是板端真实抓到的无定位样本（校验和 42 / 64 来自设备本身） */
    feed("$GNGGA,032133.000,,,,,0,00,2.8,,,,,,*42\r\n");
    feed("$GNGLL,,,,,032133.000,V,N*64\r\n");
    g = iv_agent_gps(&g_agent);
    chk(g != NULL && g->fix.valid == 0, "g04 valid = 0 without a fix");
    chk(g != NULL && isnan(g->fix.latitude), "g04 latitude stays NA (not 0)");
    chk(g != NULL && isnan(g->fix.longitude), "g04 longitude stays NA (not 0)");
    chk(g != NULL && g->fix.satellites == 0, "g04 satellites = 0");
    {
        const char *js = ctl("{\"m\":\"gps.status\"}");

        chk(strstr(js, "\"valid\":false") != NULL, "g04 JSON valid=false");
        chk(strstr(js, "\"lat\":null") != NULL, "g04 unknown latitude is JSON null");
        chk(strstr(js, "\"lon\":null") != NULL, "g04 unknown longitude is JSON null");
        chk(strstr(js, "\"lat\":0") == NULL, "g04 latitude must NOT be faked as 0");
        chk(strstr(js, "\"age_ms\":") != NULL, "g04 age_ms present once parsed");
    }

    /* ---- g05 空输入被拒（不崩、不改状态）---- */
    chk(iv_agent_gps_feed(&g_agent, NULL, 4u) == IV_EINVAL, "g05 feed(NULL) -> EINVAL");
    chk(iv_agent_gps_feed(&g_agent, "x", 0u) == IV_EINVAL, "g05 feed(len=0) -> EINVAL");
    chk(iv_agent_gps_link(NULL, &path, &fd) == IV_EINVAL, "g05 link(NULL) -> EINVAL");
    chk(iv_agent_gps(NULL) == NULL, "g05 gps(NULL) -> NULL");

    iv_agent_close(&g_agent);
    chk(iv_agent_is_open(&g_agent) == 0, "g06 closed");

    if (g_fail == 0)
        printf("test_agent_gps passed (degraded open, fix decode, ctl json, null semantics, guards)\n");
    else
        printf("test_agent_gps FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
