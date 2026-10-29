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
 *   h08 `probe.timeout_ms` 是**延时门限**（落 `degrade_rtt_ms`），等回包超时由
 *       单包间隔推导（包间隔/2，包间隔＝轮间隔/4）；只改门限也必须真的落地（2026-10-26）
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

/* h08 `probe.timeout_ms` 是**延时门限**（2026-10-26 用户口径），不是"等回包超时"。
 *
 * 修之前 `probe_cfg_of()` 只读 interval/timeout/fail_n，**从不读** `degrade_rtt_ms`
 * ⇒ 现场把这个键设成 100，平台说的"平均时延超过 100 ms 就算延时严重"从来没生效，
 * 反倒把探测自己的等回包时间压到 100 ms（一轮 200 ms 的节拍下几乎必然判丢包）。
 * 现在：门限 → `degrade_rtt_ms`；等回包超时由**单包间隔**推导（包间隔/2），而
 * 单包间隔 ＝ 轮间隔/4（一轮 4 个包，2026-10-29 用户口径）。
 *
 * 反向证伪：把 `probe_cfg_of()` 里那一行 `pc->degrade_rtt_ms = cfg->probe_delay_ms`
 * 去掉 ⇒ h08 的 degrade 三条全红；把 `probe_io_timeout_of()` 换成常量 1000 ⇒
 * 包间隔那两条全红、且 retune 会因 timeout >= interval 直接回 EINVAL。 */
static void h08_probe_delay_threshold(void)
{
    iv_agent_t     a;
    iv_probe_cfg_t pc;

    make_agent(&a);
    /* 目标都不配（0/0 ⇒ 前后相同）⇒ 走"目标没变"的 retune 分支，它要求引擎已 start */
    iv_probe_cfg_default(&pc);
    (void)iv_probe_init(&a.pb, &pc);
    chk(iv_probe_start(&a.pb, 0u) == IV_OK, "h08 probe started");

    chk(iv_agent_reload_probe(&a, 0u, 0u, 5000u, 300u, 3u, 1000u) == IV_OK,
        "h08 reload -> OK");
    chk(a.pb.cfg.degrade_rtt_ms == 300u,
        "h08 delay threshold lands in iv_probe.degrade_rtt_ms");
    chk(a.cfg.probe_delay_ms == 300u, "h08 snapshot probe_delay_ms = 300");
    /* 2026-10-29：配置值是**轮间隔**，引擎收**单包间隔** = 轮/4；
     * 等回包超时 = 单包间隔/2。5000/4=1250 ⇒ 625。 */
    chk(a.pb.cfg.interval_ms == 1250u, "h08 packet interval = round/4 (5000/4)");
    chk(a.pb.cfg.timeout_ms == 625u, "h08 io timeout = packet interval/2");
    chk(a.pb.cfg.timeout_ms < a.pb.cfg.interval_ms,
        "h08 io timeout < interval (retune 的硬约束)");

    /* 极小轮间隔：包间隔被钳到下限 250 ⇒ 超时 250 已不小于包间隔，退到 249。
     * 这一步卡的是 `probe_io_timeout_of()` 的兜底，别让 retune 因 timeout >= interval
     * 回 EINVAL（那会让整次热更失败，现场表现为"改了配置没反应"）。 */
    chk(iv_agent_reload_probe(&a, 0u, 0u, 500u, 200u, 3u, 1000u) == IV_OK,
        "h08 reload at a tiny round interval -> OK");
    chk(a.pb.cfg.interval_ms == IV_AGENT_PROBE_PKT_MIN_MS,
        "h08 packet interval clamped to the floor");
    chk(a.pb.cfg.timeout_ms == IV_AGENT_PROBE_PKT_MIN_MS - 1u,
        "h08 io timeout falls back to interval-1 at the floor");
    chk(a.pb.cfg.timeout_ms < a.pb.cfg.interval_ms, "h08 io timeout still < interval");

    /* 只改门限（同目标、同周期、同次数）：早返回的比对里若漏掉 degrade，
     * 这次调用会静默什么都不做 —— 正是 0xFE"回了 0x01 却没效果"的样子。 */
    chk(iv_agent_reload_probe(&a, 0u, 0u, 500u, 120u, 3u, 2000u) == IV_OK,
        "h08 delay-only reload -> OK");
    chk(a.pb.cfg.degrade_rtt_ms == 120u, "h08 delay-only change really landed");
}

/* h09 SNMP 目标名与反向解析（2026-10-29 目标从 2 条扩到 7 条）。
 *
 * 名字只在一处定义（`iv_agent_snmp_target_name()`）：慢池任务名、结果落库的
 * `snmp_latest.target`、界面按路显示、ctl 的 `which` 解析全走它。各写一份的话，
 * 扩到 6 路时就会出现"日志 snmp-cam、库里 camera、界面 cam"三套叫法，
 * 现场按 target 查库永远查不到东西。
 *
 * 反向证伪：把 `iv_agent_snmp_target_of()` 里 `name[3] < '0'+CAM_N` 的上界判据
 * 去掉，`"cam6"`（只有 6 路）会被当成合法下标 6 ＝**交换机** ⇒ h09 立刻红。
 * 这正是"想采第 7 路、结果采了交换机"的那类错。 */
static void h09_snmp_target_names(void)
{
    int i;
    int ok = 1;

    chk(IV_AGENT_SNMP_TARGET_N == 7, "h09 6 cameras + 1 switch = 7 targets");
    chk(strcmp(iv_agent_snmp_target_name(0), "cam0") == 0, "h09 target 0 -> cam0");
    chk(strcmp(iv_agent_snmp_target_name(5), "cam5") == 0, "h09 target 5 -> cam5");
    chk(strcmp(iv_agent_snmp_target_name(IV_AGENT_SNMP_SW), "switch") == 0,
        "h09 the switch sits at index 6 (no hole in the array)");
    chk(strcmp(iv_agent_snmp_target_name(IV_AGENT_SNMP_TARGET_N), "?") == 0,
        "h09 out of range -> \"?\" (never NULL)");

    chk(iv_agent_snmp_target_of("cam") == 0, "h09 \"cam\" alone means camera 1");
    chk(iv_agent_snmp_target_of("cam0") == 0, "h09 cam0 -> 0");
    chk(iv_agent_snmp_target_of("cam5") == 5, "h09 cam5 -> 5");
    chk(iv_agent_snmp_target_of("sw") == IV_AGENT_SNMP_SW, "h09 sw -> switch");
    chk(iv_agent_snmp_target_of("switch") == IV_AGENT_SNMP_SW, "h09 switch -> switch");
    chk(iv_agent_snmp_target_of("cam6") < 0, "h09 cam6 does not exist (only 6 cameras)");
    chk(iv_agent_snmp_target_of("cam55") < 0, "h09 cam55 rejected (trailing byte checked)");
    chk(iv_agent_snmp_target_of("") < 0, "h09 empty string rejected");
    chk(iv_agent_snmp_target_of(NULL) < 0, "h09 NULL rejected");

    /* 名字与解析必须互逆：两处各写一套比较，迟早只对一半 */
    for (i = 0; i < IV_AGENT_SNMP_TARGET_N; i++) {
        if (iv_agent_snmp_target_of(iv_agent_snmp_target_name(i)) != i)
            ok = 0;
    }
    chk(ok, "h09 name -> index round-trip for all 7 targets");
}

int main(void)
{
    h01_guards();
    h02_rec_sources();
    h04_snmp_targets();
    h08_probe_delay_threshold();
    h09_snmp_target_names();

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_hotcfg FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_agent_hotcfg passed (guards, same-value early return, rec table "
           "apply, snmp normalise, in-flight stale, late apply, probe delay "
           "threshold, snmp target names)\n");
    return 0;
}
