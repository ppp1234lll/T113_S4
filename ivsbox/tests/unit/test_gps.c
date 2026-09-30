/*
 * iv_gps 单测（libivmodules，M2-S2.5）
 *
 * ============================ 样本从哪来 ============================
 * 带校验和的语句**直接取自上游 minmea 的测试集**（`tests.c` 的
 * `valid_sentences_checksum[]` / `invalid_sentences[]`，pin 在 commit `c43c9e7c`）——
 * 用上游自己的样本，等于让"我们的调用方式"与"上游认证过的解析结果"对表，
 * 比自己手搓 NMEA 字符串可信得多。
 *
 * 另一类样本必须**动态构造**：校时用例要精确控制"GPS 时间与系统时间的偏差"，
 * 静态样本做不到。构造器 `nmea_wrap()` 自己算校验和，因此仍会被上游的
 * `minmea_check(strict)` 接受 —— 这一点本身也是对"我们构造的对不对"的检验。
 *
 * ============================ 绝不碰测试机的时钟 ============================
 * 校时用例一律注入 `iv_gps_timeops_t` 的假实现（`fake_adjtime` / `fake_settimeofday`），
 * 断言的是"走了哪条分支、传了什么偏差"，**没有任何一处真的调整本机时间**。
 *
 * ============================ 覆盖范围 ============================
 *   1. 默认配置与非法参数；  2. 初始化后快照是"未知"而不是 0；
 *   3. 完整有效链路（GGA+RMC+VTG）的字段与坐标换算；
 *   4. 无效定位（fix=0 / RMC 的 V）不得采信坐标；
 *   5. 坏校验和与结构非法**分开**计数；
 *   6. 噪声字节、半句分片、粘包、超长丢弃后重新同步；
 *   7. ZDA 作为第二个时间源；
 *   8. 上报策略：首次立即、有效性变化、位移阈值、最长周期；
 *   9. 校时策略：无日期 / 日期过早 / 小偏差走 adjtime / 中偏差走 settimeofday /
 *      大偏差只审计不调整 / 节流 / 关闭开关。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h> /* LOG_EMERG：给 iv_log_set_level 用 */
#include <time.h>

#include "ivsbox/iv_gps.h"
#include "ivsbox/iv_log.h"
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
 * 上游真实样本（含正确校验和）
 * ------------------------------------------------------------------------- */
/* 有效定位：51°06.94086'N 017°01.51680'E，fix=1，6 颗星，HDOP 3.86，海拔 127.9 m */
static const char *const k_gga_valid =
    "$GPGGA,123204.00,5106.94086,N,01701.51680,E,1,06,3.86,127.9,M,40.5,M,,*51\r\n";
/* 同一时刻的 RMC：A（有效）、速度 0.016 节、日期 2014-02-28 */
static const char *const k_rmc_valid =
    "$GPRMC,123205.00,A,5106.94085,N,01701.51689,E,0.016,,280214,,,A*7B\r\n";
static const char *const k_vtg_valid = "$GPVTG,,T,,M,0.016,N,0.030,K,A*27\r\n";
/* 无效定位：GGA 全空且 fix=0 */
static const char *const k_gga_nofix = "$GPGGA,,,,,,0,00,99.99,,,,,,*48\r\n";
/* 无效定位：RMC 状态位 V */
static const char *const k_rmc_invalid = "$GPRMC,,V,,,,,,,,,,N*53\r\n";
/* 校验和错（上游 invalid 集：把 *25 写成 *26） */
static const char *const k_bad_checksum = "$GPTXT,01,01,02,ANTSTATUS=INIT*26\r\n";
/* 结构非法：多了一个 '$' */
static const char *const k_malformed = "$$GPGGA,,,,,,0,00,99.99,,,,,,*48\r\n";
/* ZDA：2004-03-11 16:00:12.71 UTC（四位年） */
static const char *const k_zda_old = "$GPZDA,160012.71,11,03,2004,-1,00*7D\r\n";

/* ---------------------------------------------------------------------------
 * 假时钟操作：只计数、只记录，绝不真的改时间
 * ------------------------------------------------------------------------- */
static int  g_adjtime_calls;
static int  g_settimeofday_calls;
static long g_adjtime_delta_ms;
static long g_settimeofday_target;

static void fakes_reset(void)
{
    g_adjtime_calls       = 0;
    g_settimeofday_calls  = 0;
    g_adjtime_delta_ms    = 0;
    g_settimeofday_target = 0;
}

static int fake_adjtime(const struct timeval *delta, struct timeval *olddelta)
{
    (void)olddelta;
    g_adjtime_calls++;
    if (delta != NULL)
        g_adjtime_delta_ms = (long)delta->tv_sec * 1000L + (long)delta->tv_usec / 1000L;
    return 0;
}

static int fake_settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tz;
    g_settimeofday_calls++;
    if (tv != NULL)
        g_settimeofday_target = (long)tv->tv_sec;
    return 0;
}

/* ---------------------------------------------------------------------------
 * NMEA 构造器：给定 body（不含 '$' 与 "*CS"），算好校验和并补齐 CRLF
 * ------------------------------------------------------------------------- */
static void nmea_wrap(char *out, size_t cap, const char *body)
{
    unsigned    cs = 0u;
    const char *p;

    for (p = body; *p != '\0'; p++)
        cs ^= (unsigned char)*p;

    (void)snprintf(out, cap, "$%s*%02X\r\n", body, cs & 0xFFu);
}

/* 生成一条"坐标固定、时间由参数决定"的 RMC。坐标与上游样本一致，
 * 便于断言坐标换算的结果。*/
static void make_rmc_at(char *out, size_t cap, time_t when)
{
    struct tm tm;
    char      body[192];

    (void)gmtime_r(&when, &tm);
    (void)snprintf(body, sizeof body,
                   "GPRMC,%02d%02d%02d.00,A,5106.94086,N,01701.51680,E,0.016,,%02d%02d%02d,,,A",
                   tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_mday, tm.tm_mon + 1,
                   tm.tm_year % 100);
    nmea_wrap(out, cap, body);
}

/* 生成一条"坐标在赤道附近"的 RMC（用于位移阈值用例） */
static void make_rmc_far(char *out, size_t cap, time_t when, const char *lat, const char *lon)
{
    struct tm tm;
    char      body[192];

    (void)gmtime_r(&when, &tm);
    (void)snprintf(body, sizeof body,
                   "GPRMC,%02d%02d%02d.00,A,%s,N,%s,E,0.016,,%02d%02d%02d,,,A", tm.tm_hour,
                   tm.tm_min, tm.tm_sec, lat, lon, tm.tm_mday, tm.tm_mon + 1,
                   tm.tm_year % 100);
    nmea_wrap(out, cap, body);
}

static void feed_str(iv_gps_t *gps, const char *s)
{
    (void)iv_gps_feed(gps, s, strlen(s));
}

/* --------------------------------------------------------------------------- */

static void case_cfg_and_bad_args(void)
{
    iv_gps_cfg_t c;
    iv_gps_t     gps;
    iv_gps_fix_t fix;

    iv_gps_cfg_default(&c);
    chk(c.report_distance_m == 50u, "cfg_default: 50 m report distance");
    chk(c.report_interval_ms == 60000u, "cfg_default: 60 s report interval");
    chk(c.sync_system_time == 1, "cfg_default: time sync enabled");
    chk(c.small_adj_ms == 1000L, "cfg_default: 1 s small adjustment threshold");
    chk(c.big_adj_ms == 3600000L, "cfg_default: 1 h big adjustment threshold");
    chk(c.timeops == NULL, "cfg_default: real syscalls");

    iv_gps_cfg_default(NULL); /* 允许且不得崩 */
    iv_gps_init(NULL, NULL);  /* 允许且不得崩 */
    chk(1, "NULL arguments do not crash cfg/init");

    iv_gps_init(&gps, NULL);

    chk(iv_gps_feed(&gps, NULL, 4u) == IV_EINVAL, "feed(NULL) -> IV_EINVAL");
    chk(iv_gps_feed(&gps, "x", 0u) == IV_EINVAL, "feed(len = 0) -> IV_EINVAL");
    chk(iv_gps_fix(&gps, NULL) == IV_EINVAL, "fix(out = NULL) -> IV_EINVAL");
    chk(iv_gps_fix(NULL, &fix) == IV_EINVAL, "fix(gps = NULL) -> IV_EINVAL");
    chk(iv_gps_sync_time(NULL, 0u) == IV_EINVAL, "sync_time(NULL) -> IV_EINVAL");
    chk(iv_gps_should_report(NULL, 0u) == 0, "should_report(NULL) -> 0");

    /* 初始化后必须是"未知"，不能是 0 —— 纬度 0 是真实位置（几内亚湾）*/
    chk(iv_gps_fix(&gps, &fix) == IV_OK, "fix() on a fresh instance");
    chk(isnan(fix.latitude) && isnan(fix.longitude), "fresh: coordinates are NA, not 0");
    chk(fix.valid == 0, "fresh: not valid");
    chk(fix.have_time == 0, "fresh: no time");
    chk(iv_gps_should_report(&gps, 0u) == 0, "fresh: nothing to report");
}

static void case_valid_fix(void)
{
    iv_gps_t     gps;
    iv_gps_fix_t fix;

    iv_gps_init(&gps, NULL);

    chk(iv_gps_feed(&gps, k_gga_valid, strlen(k_gga_valid)) == 1,
        "the valid GGA sentence is accepted");
    chk(iv_gps_feed(&gps, k_rmc_valid, strlen(k_rmc_valid)) == 1,
        "the valid RMC sentence is accepted");
    chk(iv_gps_feed(&gps, k_vtg_valid, strlen(k_vtg_valid)) == 1,
        "the valid VTG sentence is accepted");

    chk(iv_gps_fix(&gps, &fix) == IV_OK, "fix() after feeding valid sentences");
    chk(fix.valid == 1, "fix is valid");
    chk(fix.fix_quality == 1, "GGA fix quality 1");
    chk(fix.satellites == 6, "6 satellites tracked");
    chk(fabs(fix.hdop - 3.86) < 0.01, "HDOP 3.86");
    chk(fabs(fix.altitude_m - 127.9) < 0.05, "altitude 127.9 m");

    /* 5106.94086' = 51 + 6.94086/60 = 51.115681 */
    chk(fabs(fix.latitude - 51.115681) < 1e-5, "latitude in decimal degrees");
    /* 01701.51680' = 17 + 1.51680/60 = 17.025280 */
    chk(fabs(fix.longitude - 17.025280) < 1e-5, "longitude in decimal degrees");

    /* VTG 给的是 km/h，覆盖 RMC 的节换算结果 */
    chk(fabs(fix.speed_kph - 0.030) < 0.001, "speed taken from VTG (km/h)");

    /* 时间：RMC 的日期 + 时刻 ⇒ 2014-02-28 12:32:05 UTC */
    chk(fix.have_time == 1, "complete UTC time assembled from RMC");
    chk(fix.year == 2014 && fix.month == 2 && fix.day == 28,
        "date is 2014-02-28 (two-digit year handled by minmea)");
    chk(fix.hour == 12 && fix.minute == 32 && fix.second == 5, "time is 12:32:05 UTC");

    chk(gps.stats.sentences_ok == 3u, "three sentences counted as ok");
    chk(gps.stats.fix_updates >= 2u, "fix updates counted");
}

static void case_invalid_fix_not_trusted(void)
{
    iv_gps_t     gps;
    iv_gps_fix_t fix;

    iv_gps_init(&gps, NULL);

    feed_str(&gps, k_gga_valid);
    chk(iv_gps_fix(&gps, &fix) == IV_OK && fix.valid == 1, "valid fix established first");

    /* GGA 明确报 fix quality = 0 ⇒ 必须作废坐标，而不是保留上一次的值 */
    feed_str(&gps, k_gga_nofix);
    chk(iv_gps_fix(&gps, &fix) == IV_OK, "fix() after a no-fix GGA");
    chk(fix.valid == 0, "a no-fix GGA invalidates the fix");

    /* RMC 的 V 同理 */
    feed_str(&gps, k_rmc_invalid);
    chk(iv_gps_fix(&gps, &fix) == IV_OK, "fix() after an invalid RMC");
    chk(fix.valid == 0, "an invalid RMC keeps the fix invalid");

    /* 恢复有效定位 */
    feed_str(&gps, k_gga_valid);
    chk(iv_gps_fix(&gps, &fix) == IV_OK && fix.valid == 1, "validity is restored");
}

static void case_checksum_and_malformed_are_separate(void)
{
    iv_gps_t gps;

    iv_gps_init(&gps, NULL);

    feed_str(&gps, k_bad_checksum);
    chk(gps.stats.checksum_bad == 1u, "a bad checksum is counted as checksum_bad");
    chk(gps.stats.malformed == 0u, "a bad checksum is NOT counted as malformed");

    feed_str(&gps, k_malformed);
    chk(gps.stats.checksum_bad == 1u, "malformed does not bump checksum_bad");

    /* 过短的一行（不足 8 字符）按结构非法处理 */
    feed_str(&gps, "$GP\r\n");
    chk(gps.stats.malformed >= 1u, "an over-short sentence is malformed");

    feed_str(&gps, k_gga_valid);
    chk(gps.stats.sentences_ok == 1u, "a good sentence still parses afterwards");
}

static void case_noise_split_and_coalescing(void)
{
    iv_gps_t     gps;
    iv_gps_fix_t fix;
    size_t       half;
    char         big[256];

    iv_gps_init(&gps, NULL);

    /* 噪声：普通文本里没有 '$'，应全部计入 noise_bytes 且不产生定位 */
    feed_str(&gps, "garbage without a sentence\r\n");
    chk(gps.stats.noise_bytes > 0u, "noise bytes are counted");
    chk(gps.stats.sentences_ok == 0u, "noise produces no sentences");
    chk(iv_gps_fix(&gps, &fix) == IV_OK && fix.valid == 0, "noise produces no fix");

    /* 半句：拆成两段喂，第一段不能产出任何语句 */
    half = strlen(k_gga_valid) / 2u;
    chk(iv_gps_feed(&gps, k_gga_valid, half) == 0, "a partial sentence yields nothing");
    chk(iv_gps_feed(&gps, k_gga_valid + half, strlen(k_gga_valid) - half) == 1,
        "the remainder completes the sentence");
    chk(iv_gps_fix(&gps, &fix) == IV_OK && fix.valid == 1, "the split sentence produced a fix");

    /* 粘包：两条语句连在一起一次喂入 */
    iv_gps_init(&gps, NULL);
    {
        char both[256];
        (void)snprintf(both, sizeof both, "%s%s", k_gga_valid, k_rmc_valid);
        chk(iv_gps_feed(&gps, both, strlen(both)) == 2, "two concatenated sentences both parse");
    }

    /* 超长：整行丢弃，且**不得**截断解析；随后一条正常语句必须还能解析 */
    iv_gps_init(&gps, NULL);
    {
        char body[200];
        memset(body, 'A', sizeof body - 1u);
        body[0] = 'G';
        body[1] = 'P';
        body[2] = 'T';
        body[3] = 'X';
        body[4] = 'T';
        body[5] = ',';
        body[sizeof body - 1u] = '\0';
        nmea_wrap(big, sizeof big, body); /* 长度远超 IV_GPS_LINE_MAX */
        chk(iv_gps_feed(&gps, big, strlen(big)) == 0, "an over-long sentence yields nothing");
        chk(gps.stats.too_long == 1u, "the over-long sentence is counted");
        chk(gps.stats.checksum_bad == 0u && gps.stats.malformed == 0u,
            "an over-long sentence is neither a checksum nor a malformed error");

        feed_str(&gps, k_gga_valid);
        chk(gps.stats.sentences_ok == 1u, "re-synchronises after an over-long sentence");
    }
}

static void case_zda_time_source(void)
{
    iv_gps_t     gps;
    iv_gps_fix_t fix;

    iv_gps_init(&gps, NULL);

    feed_str(&gps, k_zda_old);
    chk(gps.stats.sentences_ok == 1u, "ZDA is accepted");
    chk(iv_gps_fix(&gps, &fix) == IV_OK, "fix() after ZDA");
    chk(fix.have_time == 1, "ZDA alone provides a complete UTC time");
    chk(fix.year == 2004 && fix.month == 3 && fix.day == 11, "ZDA date 2004-03-11");
    chk(fix.hour == 16 && fix.minute == 0 && fix.second == 12, "ZDA time 16:00:12");
    chk(fix.valid == 0, "ZDA carries no position, so the fix stays invalid");
}

static void case_report_policy(void)
{
    iv_gps_t gps;
    char     line[192];
    time_t   base = time(NULL);

    iv_gps_init(&gps, NULL);

    /* 首次：无效定位不报 */
    chk(iv_gps_should_report(&gps, 0u) == 0, "nothing to report before a valid fix");
    feed_str(&gps, k_gga_valid);
    chk(iv_gps_should_report(&gps, 0u) == 1, "the first valid fix reports immediately");

    iv_gps_mark_reported(&gps, 0u);
    chk(iv_gps_should_report(&gps, 100u) == 0, "no report without any change");

    /* 周期到：60 s 后必须报 */
    chk(iv_gps_should_report(&gps, 60000u) == 1, "the maximum interval forces a report");
    iv_gps_mark_reported(&gps, 60000u);

    /* 有效性变化：立即报（上层要尽快知道失锁） */
    feed_str(&gps, k_gga_nofix);
    chk(iv_gps_should_report(&gps, 60100u) == 1, "losing the fix reports immediately");
    iv_gps_mark_reported(&gps, 60100u);

    /* 位移阈值：换到 1 度以外（约 111 km）必须报 */
    make_rmc_far(line, sizeof line, base, "5206.94086", "01701.51680");
    feed_str(&gps, line);
    chk(iv_gps_should_report(&gps, 60200u) == 1, "a large displacement forces a report");

    /* 先回到原位置并把它设为新基线 —— 否则"回到出发点"这件事本身又会被算成一次
     * 上百公里的位移，下面那条"亚米级位移不报"就测不出想测的东西了。*/
    make_rmc_far(line, sizeof line, base, "5106.94086", "01701.51680");
    feed_str(&gps, line);
    iv_gps_mark_reported(&gps, 60200u);

    /* 亚米级位移（0.00001 分 ≈ 1.9 cm）且周期未到：不报 */
    make_rmc_far(line, sizeof line, base, "5106.94087", "01701.51681");
    feed_str(&gps, line);
    chk(iv_gps_should_report(&gps, 60300u) == 0,
        "a sub-metre displacement within the interval does not report");
}

static void case_sync_needs_date(void)
{
    iv_gps_t        gps;
    iv_gps_cfg_t    cfg;
    iv_gps_timeops_t ops;
    char            line[192];

    ops.adjtime_fn      = fake_adjtime;
    ops.settimeofday_fn = fake_settimeofday;
    iv_gps_cfg_default(&cfg);
    cfg.timeops = &ops;
    iv_gps_init(&gps, &cfg);

    fakes_reset();
    chk(iv_gps_sync_time(&gps, 0u) == IV_ESTATE, "no date at all -> IV_ESTATE");
    chk(g_adjtime_calls == 0 && g_settimeofday_calls == 0, "no clock call without a date");

    /* 只有 GGA（带时刻、无日期）：仍然不能校时 */
    feed_str(&gps, k_gga_valid);
    chk(iv_gps_sync_time(&gps, 0u) == IV_ESTATE, "time without a date -> IV_ESTATE");
    chk(g_adjtime_calls == 0 && g_settimeofday_calls == 0, "no clock call with time only");

    /* 日期过早（2004 < min_year 2024）⇒ 拒绝，且计入 rejected */
    feed_str(&gps, k_zda_old);
    chk(iv_gps_sync_time(&gps, 0u) == IV_ESTATE, "a date before min_year -> IV_ESTATE");
    chk(gps.stats.time_sync_rejected == 1u, "the rejection is counted");
    chk(g_adjtime_calls == 0 && g_settimeofday_calls == 0, "no clock call for an old date");

    (void)line;
}

static void case_sync_small_delta(void)
{
    iv_gps_t         gps;
    iv_gps_cfg_t     cfg;
    iv_gps_timeops_t ops;
    char             line[192];
    time_t           now = time(NULL);

    ops.adjtime_fn      = fake_adjtime;
    ops.settimeofday_fn = fake_settimeofday;
    iv_gps_cfg_default(&cfg);
    cfg.timeops = &ops;
    iv_gps_init(&gps, &cfg);

    /* NMEA 只到整秒，而真实时间带小数秒 ⇒ 偏差必然小于 1 s，走 adjtime */
    make_rmc_at(line, sizeof line, now);
    feed_str(&gps, line);

    fakes_reset();
    chk(iv_gps_sync_time(&gps, 1000u) == IV_OK, "a sub-second delta is adjusted");
    chk(g_adjtime_calls == 1, "a small delta goes through adjtime (no time jump)");
    chk(g_settimeofday_calls == 0, "settimeofday is not used for a small delta");
    chk(g_adjtime_delta_ms > -1000L && g_adjtime_delta_ms < 1000L,
        "the delta passed to adjtime is within a second");
    chk(gps.stats.time_sync_ok == 1u, "a successful sync is counted");

    /* 节流：60 s 内再来一次不调 */
    chk(iv_gps_sync_time(&gps, 1100u) == IV_EAGAIN, "a second sync within the interval is throttled");
    chk(g_adjtime_calls == 1, "the throttled call did not touch the clock");

    /* 过了节流窗口就能再调 */
    chk(iv_gps_sync_time(&gps, 1000u + 61000u) == IV_OK, "after the interval a sync happens again");
    chk(g_adjtime_calls == 2, "the second sync reached adjtime");
}

static void case_sync_medium_and_big_delta(void)
{
    iv_gps_t         gps;
    iv_gps_cfg_t     cfg;
    iv_gps_timeops_t ops;
    char             line[192];
    time_t           now = time(NULL);

    ops.adjtime_fn      = fake_adjtime;
    ops.settimeofday_fn = fake_settimeofday;
    iv_gps_cfg_default(&cfg);
    cfg.timeops = &ops;

    /* 中偏差：30 s ⇒ 超过 small(1 s) 但不超过 big(1 h) ⇒ settimeofday */
    iv_gps_init(&gps, &cfg);
    make_rmc_at(line, sizeof line, now + 30);
    feed_str(&gps, line);
    fakes_reset();
    chk(iv_gps_sync_time(&gps, 0u) == IV_OK, "a 30 s delta is adjusted");
    chk(g_settimeofday_calls == 1, "a medium delta uses settimeofday");
    chk(g_adjtime_calls == 0, "adjtime is not used for a medium delta");
    chk(g_settimeofday_target > now, "the target time is the GPS time");

    /* 大偏差：2 h ⇒ 只审计、不调整 */
    iv_gps_init(&gps, &cfg);
    make_rmc_at(line, sizeof line, now + 7200);
    feed_str(&gps, line);
    fakes_reset();
    chk(iv_gps_sync_time(&gps, 0u) == IV_ERANGE, "a 2 h delta is refused (audit only)");
    chk(g_adjtime_calls == 0 && g_settimeofday_calls == 0,
        "a big delta must NOT touch the clock at all");
    chk(gps.stats.time_sync_rejected == 1u, "the refusal is counted");
    chk(gps.stats.time_sync_ok == 0u, "the refusal is not counted as a sync");
}

static void case_sync_disabled(void)
{
    iv_gps_t         gps;
    iv_gps_cfg_t     cfg;
    iv_gps_timeops_t ops;
    char             line[192];

    ops.adjtime_fn      = fake_adjtime;
    ops.settimeofday_fn = fake_settimeofday;
    iv_gps_cfg_default(&cfg);
    cfg.timeops          = &ops;
    cfg.sync_system_time = 0; /* 明确关闭 */
    iv_gps_init(&gps, &cfg);

    make_rmc_at(line, sizeof line, time(NULL) + 30);
    feed_str(&gps, line);

    fakes_reset();
    chk(iv_gps_sync_time(&gps, 0u) == IV_EAGAIN, "sync disabled -> IV_EAGAIN");
    chk(g_adjtime_calls == 0 && g_settimeofday_calls == 0, "sync disabled touches nothing");
}

static void case_cfg_normalisation(void)
{
    iv_gps_cfg_t cfg;
    iv_gps_t     gps;

    /* 0 间隔会把"每次定位都上报/校时"变成默认行为，必须被拉回默认值 */
    iv_gps_cfg_default(&cfg);
    cfg.report_interval_ms = 0u;
    cfg.sync_interval_ms   = 0u;
    cfg.min_year           = 1;   /* 明显不合法 */
    cfg.small_adj_ms       = 5000L;
    cfg.big_adj_ms         = 100L; /* 倒挂 */
    iv_gps_init(&gps, &cfg);

    chk(gps.cfg.report_interval_ms == 60000u, "a zero report interval falls back to the default");
    chk(gps.cfg.sync_interval_ms == 60000u, "a zero sync interval falls back to the default");
    chk(gps.cfg.min_year == 2000, "an absurd min_year is clamped");
    chk(gps.cfg.big_adj_ms >= gps.cfg.small_adj_ms, "an inverted threshold pair is repaired");
}

int main(void)
{
    /* 校时用例会触发 iv_gps 的审计日志（IV_LOG_W/I）。测试机上没有 `/mnt/UDISK`，
     * 落盘失败会往 stderr 打一行噪音；这里把级别压到 EMERG 静音，保持输出干净。
     * 这不是"掩盖错误" —— 断言全部落在返回值与统计计数上，与日志无关。*/
    iv_log_set_level(LOG_EMERG);

    case_cfg_and_bad_args();
    case_valid_fix();
    case_invalid_fix_not_trusted();
    case_checksum_and_malformed_are_separate();
    case_noise_split_and_coalescing();
    case_zda_time_source();
    case_report_policy();
    case_sync_needs_date();
    case_sync_small_delta();
    case_sync_medium_and_big_delta();
    case_sync_disabled();
    case_cfg_normalisation();

    if (g_fail != 0) {
        fprintf(stderr, "test_gps FAILED (%d failed checks)\n", g_fail);
        return 1;
    }

    printf("test_gps passed (parse, checksum split, noise/split/coalesce/overlong, ZDA, "
           "report policy, time sync branches)\n");
    return 0;
}
