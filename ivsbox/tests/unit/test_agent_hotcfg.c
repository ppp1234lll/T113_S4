/*
 * 配置热生效（`iv_agent_reload_rec` / `iv_agent_reload_snmp`）单测  2026-10-22
 *
 * 背景：`camera.ch.N.url` / `.record` / `camera.rec.hold_ms` / `camera.rec.debounce_ms`
 * 与 `camera.snmp.*` / `switch.snmp.*` 原先**只在 `iv_agent_open()` 里读一次**
 * ⇒ 网页上改完必须重启主控才生效（"改配置不生效"清单里的两条）。本层把它们做成
 * 运行期热更函数，由 `main.c` 的 `on_cfg_committed()` 调用。
 *
 * 覆盖：
 *   h01 入参守卫：NULL / 未 open / 路数越界 / media 未起
 *   h02 录像源**同值早返回**：`hold_ms`/`debounce_ms` 传 0 归一成模块默认后比对，
 *       整类提交（改的是别的键）必须一字不动 —— 否则每存一次配置就拆掉一轮录像
 *   h03 录像源真变了才落到状态机（含 `0 → 默认值` 的归一，`a->cfg` 同步更新）
 *   h04 SNMP **同值早返回**：`port 0` 与 `161` 等价，作业不许重建
 *       （重建 = `memset` 掉就绪结果，页面上的采集值会凭空清空）
 *   h05 SNMP 真变了：作业按新目标重建（community / OID 表 / host / 状态归 IDLE）
 *   h06 作业在飞：**不动 `jb`**，只置 `snmp_stale`，`a->cfg` 立即更新
 *   h07 stale 由 `iv_agent_snmp_start()` 落地，且必须在"有没有池"这类守卫**之前**
 *       （否则池缺/开关关期间 stale 一直挂着，等就绪了第一次仍按旧表采）
 *
 * 为什么这一层能测：两个 reload 只依赖 `a->cfg` / `a->recm` / `a->snmp_job`，
 * 不需要 `iv_agent_open()` 的串口/netlink。**"改配置会不会走到它们"** 取决于
 * `main.c` 的订阅掩码，那由装配层 e2e（`tests/integration/hotcfg_e2e.py`）真拉
 * 进程取证 —— 掩码漏 class 时分支写得再对也永远不触发（10-21 与 10-22 各踩过一次）。
 *
 * 反向证伪（撤掉对应实现后哪条会红）：
 *   F1 去掉 `iv_agent_reload_rec()` 的同值早返回 ⇒ h02 的哨兵被 init 清零；
 *   F2 去掉 `snmp_cfg_norm()`（直接比原值）⇒ h04 因 `0 != 161` 判"变了"，哨兵被拍回 2；
 *   F3 `snmp_job_apply()` 不判在飞（照样 memset）⇒ h06 的 community 立即变成新值；
 *   F4 `iv_agent_snmp_start()` 里 stale 落地挪到 `pool == NULL` 之后 ⇒ h07 全红。
 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_recctl.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_snmp.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 造一个"已 open、媒体桥接已 init"的 agent 壳：刻意**不**调 `iv_agent_open()`
 * （那要串口/netlink，与本次被测逻辑无关）。`media.inited = 1` 是
 * `iv_agent_reload_rec()` 的前置：没桥接就没有 `recm` 可改。 */
static void make_agent(iv_agent_t *a)
{
    memset(a, 0, sizeof(*a));
    iv_agent_init(a);
    a->opened       = 1;
    a->media.inited = 1;
}

/* `ip_be` 的口径＝`struct in_addr.s_addr`（网络序），所以直接用 `inet_addr()`
 * 拿到的就是它；手拼位移在小端宿主上恰好对、在大端上就颠倒了，别那么写。 */
static uint32_t ip4(const char *dotted)
{
    return (uint32_t)inet_addr(dotted);
}

static void snmp_cfg_basic(iv_agent_snmp_cfg_t *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc[IV_AGENT_SNMP_CAM].enable = 1;
    sc[IV_AGENT_SNMP_CAM].ip_be  = ip4("10.0.0.7");
    sc[IV_AGENT_SNMP_CAM].port   = 0u; /* → 161（归一化对象） */
    (void)snprintf(sc[IV_AGENT_SNMP_CAM].community,
                   sizeof(sc[IV_AGENT_SNMP_CAM].community), "%s", "public");
    sc[IV_AGENT_SNMP_CAM].n_oid  = 2;
    (void)snprintf(sc[IV_AGENT_SNMP_CAM].oid[0],
                   sizeof(sc[IV_AGENT_SNMP_CAM].oid[0]), "%s", "1.3.6.1.2.1.1.1.0");
    (void)snprintf(sc[IV_AGENT_SNMP_CAM].oid[1],
                   sizeof(sc[IV_AGENT_SNMP_CAM].oid[1]), "%s", "1.3.6.1.2.1.1.3.0");
    /* 交换机目标一律留空：用来证明"只改了一个目标，另一个一个字都不动" */
}

/* ---------------- h01 入参守卫 ---------------- */
static void h01_guards(void)
{
    iv_agent_t          a;
    iv_agent_snmp_cfg_t sc[IV_AGENT_SNMP_TARGET_N];
    const char         *urls[IV_RECCTL_CH_N];
    int                 i;

    for (i = 0; i < IV_RECCTL_CH_N; i++)
        urls[i] = "";
    urls[0] = "rtsp://10.0.0.5/ch1";
    snmp_cfg_basic(sc);

    chk(iv_agent_reload_rec(NULL, urls, 1, 0u, 0u) == IV_EINVAL,
        "h01 reload_rec(NULL a) -> EINVAL");
    make_agent(&a);
    chk(iv_agent_reload_rec(&a, NULL, 0, 0u, 0u) == IV_EINVAL,
        "h01 reload_rec(NULL urls) -> EINVAL");
    chk(iv_agent_reload_rec(&a, urls, -1, 0u, 0u) == IV_EINVAL,
        "h01 reload_rec(n = -1) -> EINVAL");
    chk(iv_agent_reload_rec(&a, urls, IV_RECCTL_CH_N + 1, 0u, 0u) == IV_EINVAL,
        "h01 reload_rec(n > CH_N) -> EINVAL");

    a.opened = 0;
    chk(iv_agent_reload_rec(&a, urls, 1, 0u, 0u) == IV_ESTATE,
        "h01 reload_rec before open -> ESTATE");
    chk(iv_agent_reload_snmp(&a, sc) == IV_ESTATE,
        "h01 reload_snmp before open -> ESTATE");

    make_agent(&a);
    a.media.inited = 0; /* 媒体桥接没起来：没有 recm 可改 */
    chk(iv_agent_reload_rec(&a, urls, 1, 0u, 0u) == IV_ESTATE,
        "h01 reload_rec without the media bridge -> ESTATE");
    chk(a.cfg.rec_n_url == 0, "h01 snapshot NOT touched on ESTATE (下次 open 自然按新值)");

    chk(iv_agent_reload_snmp(NULL, sc) == IV_EINVAL, "h01 reload_snmp(NULL a) -> EINVAL");
    chk(iv_agent_reload_snmp(&a, NULL) == IV_EINVAL, "h01 reload_snmp(NULL sc) -> EINVAL");
}

/* ---------------- h02/h03 录像源 ---------------- */
static void h02_rec_sources(void)
{
    iv_agent_t  a;
    const char *urls1[IV_RECCTL_CH_N];
    const char *urls2[IV_RECCTL_CH_N];
    int         i;

    for (i = 0; i < IV_RECCTL_CH_N; i++) {
        urls1[i] = "";
        urls2[i] = "";
    }
    urls1[0] = "rtsp://10.0.0.5/ch1";
    urls2[0] = "rtsp://10.0.0.5/ch1";
    urls2[1] = "rtsp://10.0.0.6/ch2";

    /* 先摆成"open 时就是这么装的"：1 路、默认 hold/debounce */
    make_agent(&a);
    a.cfg.rec_n_url       = 1;
    a.cfg.rec_hold_ms     = IV_RECCTL_HOLD_MS_DEFAULT;
    a.cfg.rec_debounce_ms = IV_RECCTL_DEBOUNCE_MS_DEFAULT;
    (void)snprintf(a.cfg.rec_url[0], sizeof(a.cfg.rec_url[0]), "%s", urls1[0]);
    a.recm.down_since_ms = 4242; /* 哨兵：真的换了表就会被 init 清零 */

    /* `0` ＝ 这一项没给 ⇒ 归一成模块默认，与现值相等 ⇒ 早返回 */
    chk(iv_agent_reload_rec(&a, urls1, 1, 0u, 0u) == IV_OK,
        "h02 same values (0 = default) -> OK");
    chk(a.recm.down_since_ms == 4242,
        "h02 the state machine is untouched on a same-value commit (F1)");
    chk(a.recm.cfg.n_url == 0,
        "h02 recm was never initialised by the reload (still the shell's zero)");

    /* 真变了：改了一路地址 ＋ 两个时间参数 */
    chk(iv_agent_reload_rec(&a, urls2, 2, 3000u, 20000u) == IV_OK,
        "h03 changed urls -> OK");
    chk(a.recm.cfg.n_url == 2, "h03 state machine got the new table length");
    chk(strcmp(a.recm.cfg.url[0], urls2[0]) == 0, "h03 url[0] applied");
    chk(strcmp(a.recm.cfg.url[1], urls2[1]) == 0, "h03 url[1] applied");
    chk(a.recm.cfg.hold_ms == 3000u, "h03 hold_ms applied");
    chk(a.recm.cfg.debounce_ms == 20000u, "h03 debounce_ms applied");
    chk(a.recm.down_since_ms == 0, "h03 the state machine WAS re-initialised (对照 h02)");
    chk(a.cfg.rec_n_url == 2, "h03 snapshot n_url = 2");
    chk(a.cfg.rec_hold_ms == 3000u, "h03 snapshot hold_ms = 3000");
    chk(a.cfg.rec_debounce_ms == 20000u, "h03 snapshot debounce_ms = 20000");

    /* 再提交同样的值 ⇒ 早返回（`a->cfg` 已是归一后的值，比对不会永远不等） */
    a.recm.down_since_ms = 99;
    chk(iv_agent_reload_rec(&a, urls2, 2, 3000u, 20000u) == IV_OK,
        "h03 re-applying the same table -> OK");
    chk(a.recm.down_since_ms == 99, "h03 same table again -> early return");
}

/* ---------------- h04/h05/h06/h07 SNMP 目标 ---------------- */
static void h04_snmp_targets(void)
{
    iv_agent_t          a;
    iv_agent_snmp_cfg_t sc[IV_AGENT_SNMP_TARGET_N];

    snmp_cfg_basic(sc);
    make_agent(&a);

    chk(iv_agent_reload_snmp(&a, sc) == IV_OK, "h04 first apply -> OK");
    chk(a.snmp_job[IV_AGENT_SNMP_CAM].t.port == (uint16_t)IV_SNMP_PORT_DEFAULT,
        "h04 port 0 normalised to 161 in the job");
    chk(a.snmp_job[IV_AGENT_SNMP_CAM].n_slot == 2, "h04 two OID slots in the job");
    chk(strcmp(a.snmp_job[IV_AGENT_SNMP_CAM].t.host, "10.0.0.7") == 0,
        "h04 host rendered from ip_be");
    chk(a.snmp_reqid[IV_AGENT_SNMP_CAM] == 1u, "h04 request-id seed reset for target 0");

    /* 同值：只把 `0` 写成显式的 `161`，归一后完全相等 ⇒ 作业不许重建。
     * 重建会 `memset` 掉就绪结果（页面上采集值凭空清空），是最难解释的"灵异"。 */
    a.snmp_job[IV_AGENT_SNMP_CAM].n_slot = 99; /* 哨兵 */
    sc[IV_AGENT_SNMP_CAM].port = (uint16_t)IV_SNMP_PORT_DEFAULT;
    chk(iv_agent_reload_snmp(&a, sc) == IV_OK, "h04 port 161 == default -> OK");
    chk(a.snmp_job[IV_AGENT_SNMP_CAM].n_slot == 99,
        "h04 the job is NOT rebuilt for 0 vs 161 (F2)");

    /* 真变了：换团体字 ＋ 换第二个 OID */
    (void)snprintf(sc[IV_AGENT_SNMP_CAM].community,
                   sizeof(sc[IV_AGENT_SNMP_CAM].community), "%s", "priv");
    (void)snprintf(sc[IV_AGENT_SNMP_CAM].oid[1],
                   sizeof(sc[IV_AGENT_SNMP_CAM].oid[1]), "%s", "1.3.6.1.4.1.9.9.0");
    chk(iv_agent_reload_snmp(&a, sc) == IV_OK, "h05 changed community/oid -> OK");
    chk(a.snmp_job[IV_AGENT_SNMP_CAM].n_slot == 2, "h05 the job WAS rebuilt");
    chk(strcmp(a.snmp_job[IV_AGENT_SNMP_CAM].t.community, "priv") == 0,
        "h05 community applied");
    chk(strcmp(a.snmp_job[IV_AGENT_SNMP_CAM].slot[1].oid, "1.3.6.1.4.1.9.9.0") == 0,
        "h05 oid slot applied");
    chk(a.snmp_state[IV_AGENT_SNMP_CAM] == IV_AGENT_SNMP_IDLE, "h05 state -> IDLE");
    chk(a.snmp_stale[IV_AGENT_SNMP_CAM] == 0, "h05 no stale flag after a clean rebuild");
    chk(a.snmp_job[IV_AGENT_SNMP_SW].n_slot == 0,
        "h05 the untouched switch target is NOT rebuilt");

    /* 在飞：慢池里的 worker 正读 `jb`，只许置 stale */
    a.snmp_state[IV_AGENT_SNMP_CAM] = IV_AGENT_SNMP_RUN;
    (void)snprintf(sc[IV_AGENT_SNMP_CAM].community,
                   sizeof(sc[IV_AGENT_SNMP_CAM].community), "%s", "other");
    chk(iv_agent_reload_snmp(&a, sc) == IV_OK, "h06 reload while the job is in flight -> OK");
    chk(a.snmp_stale[IV_AGENT_SNMP_CAM] == 1, "h06 stale flag set for the late apply");
    chk(strcmp(a.snmp_job[IV_AGENT_SNMP_CAM].t.community, "priv") == 0,
        "h06 the in-flight job object is NOT touched (F3)");
    chk(strcmp(a.cfg.snmp[IV_AGENT_SNMP_CAM].community, "other") == 0,
        "h06 a->cfg updated immediately (下一次 start 用的就是它)");

    /* 落地：`snmp_start()` 在任何"能不能发"的守卫之前先把 stale 应用掉。
     * 这里 pool 为 NULL（ESTATE），stale 仍必须落地。 */
    a.snmp_state[IV_AGENT_SNMP_CAM] = IV_AGENT_SNMP_IDLE;
    a.cfg.pool = NULL;
    chk(iv_agent_snmp_start(&a, IV_AGENT_SNMP_CAM, 0u) == IV_ESTATE,
        "h07 no task pool -> ESTATE (guard unchanged)");
    chk(a.snmp_stale[IV_AGENT_SNMP_CAM] == 0, "h07 stale landed before the guard (F4)");
    chk(strcmp(a.snmp_job[IV_AGENT_SNMP_CAM].t.community, "other") == 0,
        "h07 the job now carries the new target");
    chk(a.snmp_job[IV_AGENT_SNMP_CAM].n_slot == 2,
        "h07 the OID table came back with the rebuild");
}

int main(void)
{
    h01_guards();
    h02_rec_sources();
    h04_snmp_targets();

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_hotcfg FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_agent_hotcfg passed (guards, same-value early return, rec table "
           "apply, snmp normalise, in-flight stale, late apply)\n");
    return 0;
}
