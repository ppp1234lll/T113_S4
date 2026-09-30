/*
 * GPS / NMEA 定位与时间（libivmodules，功能开发计划 M2-S2.5）
 *
 * 设计口径、硬件事实与校时安全策略全部写在 `include/ivsbox/iv_gps.h` 的文件头，
 * 这里只补三条实现层面的说明：
 *
 * 1) **不采信"无效定位下的坐标"**。NMEA 在未定位时有两种表现：字段留空（minmea 解析
 *    成 `-1` / `scale = 0`），或者**沿用上一个有效值的坐标**（不少模块如此）。因此
 *    坐标只在 `fix_quality > 0`（GGA）或 `valid == 'A'`（RMC）时才写入 fix；一旦看到
 *    无效标志，立刻把 `fix.valid` 清 0 —— 否则上层会把"模块停在原地"误读成
 *    "设备一直在某个坐标"。
 *
 * 2) **日期与时刻分开到达，必须合并**。GGA 只给时刻、RMC 同时给日期与时刻、ZDA 也给
 *    两者。只有"日期来源"（RMC / ZDA）到过之后 `fix.have_time` 才会成立 —— 这是
 *    校时的前提，避免拿"1970-01-01 + 有效时刻"去调系统时钟。
 *
 * 3) **第三方解析器只在本文件里出现**。`iv_gps.h` 不暴露 minmea 的任何类型，
 *    调用方不必 include 它，也就不存在"某个模块绕过 iv_gps 直接用 minmea"的路径。
 */
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "ivsbox/iv_gps.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

#include "minmea.h"

#define GPS_MOD "gps"

/* 地球平均半径（米），用于位移判定的近似换算 */
#define GPS_EARTH_R_M 6371000.0

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */
static long abs_long(long v)
{
    return (v < 0) ? -v : v;
}

static double deg2rad(double deg)
{
    return deg * (M_PI / 180.0);
}

/* 两点距离（米）。用 equirectangular 近似：本函数**只**用来判"位移是否超过几十米"，
 * 在几公里量级误差 < 0.1%，比 haversine 少两次三角运算。
 * 需要 `-lm`（cos/sqrt）—— 已在两个工具链文件的 LDLIBS 里登记。*/
static double distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = deg2rad(lat2 - lat1);
    double dlon = deg2rad(lon2 - lon1);
    double mlat = deg2rad((lat1 + lat2) / 2.0);
    double x    = dlon * cos(mlat);

    return GPS_EARTH_R_M * sqrt((dlat * dlat) + (x * x));
}

static void fix_reset(iv_gps_fix_t *f)
{
    memset(f, 0, sizeof(*f));
    f->latitude   = IV_GPS_NA;
    f->longitude  = IV_GPS_NA;
    f->altitude_m = IV_GPS_NA;
    f->speed_kph  = IV_GPS_NA;
    f->course_deg = IV_GPS_NA;
    f->hdop       = IV_GPS_NA;
    f->valid      = 0;
    f->have_time  = 0;
}

/* 把内部暂存的日期 + 时刻合并进对外快照（见文件头第 2 条） */
static void merge_time(iv_gps_t *gps)
{
    struct minmea_date d;
    struct minmea_time t;
    struct tm          tm;

    if (!gps->internal_have_date || !gps->internal_have_utc)
        return;

    d.day    = gps->internal_day;
    d.month  = gps->internal_month;
    d.year   = gps->internal_year;
    t.hours  = gps->internal_hour;
    t.minutes = gps->internal_minute;
    t.seconds = gps->internal_second;
    t.microseconds = (int)gps->internal_usec;

    if (minmea_getdatetime(&tm, &d, &t) != 0)
        return;

    gps->fix.have_time = 1;
    gps->fix.year      = tm.tm_year + 1900;
    gps->fix.month     = tm.tm_mon + 1;
    gps->fix.day       = tm.tm_mday;
    gps->fix.hour      = tm.tm_hour;
    gps->fix.minute    = tm.tm_min;
    gps->fix.second    = tm.tm_sec;
    gps->fix.usec      = gps->internal_usec;
}

/* ---------------------------------------------------------------------------
 * 语句处理
 * ------------------------------------------------------------------------- */
static int on_gga(iv_gps_t *gps, const char *line)
{
    struct minmea_sentence_gga f;

    if (!minmea_parse_gga(&f, line)) {
        gps->stats.malformed++;
        return 0;
    }
    gps->stats.sentences_ok++;

    gps->fix.fix_quality = f.fix_quality;
    gps->fix.satellites  = f.satellites_tracked;

    {
        float hdop = minmea_tofloat(&f.hdop);
        float alt  = minmea_tofloat(&f.altitude);
        if (!isnan(hdop))
            gps->fix.hdop = hdop;
        if (!isnan(alt))
            gps->fix.altitude_m = alt;
    }

    if (f.fix_quality > 0) {
        double lat = minmea_tocoord(&f.latitude);
        double lon = minmea_tocoord(&f.longitude);
        if (!isnan(lat) && !isnan(lon)) {
            gps->fix.latitude  = lat;
            gps->fix.longitude = lon;
            gps->fix.valid     = 1;
            gps->stats.fix_updates++;
        }
    } else {
        /* 无效定位：坐标可能是"上一个有效值"，必须作废（见文件头第 1 条）*/
        gps->fix.valid = 0;
    }

    if (f.time.hours >= 0) {
        gps->internal_hour     = f.time.hours;
        gps->internal_minute   = f.time.minutes;
        gps->internal_second   = f.time.seconds;
        gps->internal_usec     = f.time.microseconds;
        gps->internal_have_utc = 1;
    }
    merge_time(gps);
    return 1;
}

static int on_rmc(iv_gps_t *gps, const char *line)
{
    struct minmea_sentence_rmc f;

    if (!minmea_parse_rmc(&f, line)) {
        gps->stats.malformed++;
        return 0;
    }
    gps->stats.sentences_ok++;

    if (f.valid) {
        double lat = minmea_tocoord(&f.latitude);
        double lon = minmea_tocoord(&f.longitude);
        if (!isnan(lat) && !isnan(lon)) {
            gps->fix.latitude  = lat;
            gps->fix.longitude = lon;
            gps->fix.valid     = 1;
            gps->stats.fix_updates++;
        }

        {
            float spd = minmea_tofloat(&f.speed);  /* RMC 给的是节 */
            float crs = minmea_tofloat(&f.course);
            if (!isnan(spd))
                gps->fix.speed_kph = spd * 1.852f;
            if (!isnan(crs))
                gps->fix.course_deg = crs;
        }
    } else {
        gps->fix.valid = 0;
    }

    /* RMC 是"同时带日期"的首选时间源 */
    if (f.date.year != -1 && f.time.hours >= 0) {
        gps->internal_year      = f.date.year;
        gps->internal_month     = f.date.month;
        gps->internal_day       = f.date.day;
        gps->internal_have_date = 1;
        gps->internal_hour      = f.time.hours;
        gps->internal_minute    = f.time.minutes;
        gps->internal_second    = f.time.seconds;
        gps->internal_usec      = f.time.microseconds;
        gps->internal_have_utc  = 1;
    }
    merge_time(gps);
    return 1;
}

static int on_vtg(iv_gps_t *gps, const char *line)
{
    struct minmea_sentence_vtg f;

    if (!minmea_parse_vtg(&f, line)) {
        gps->stats.malformed++;
        return 0;
    }
    gps->stats.sentences_ok++;

    {
        float kph = minmea_tofloat(&f.speed_kph);          /* VTG 直接给 km/h */
        float trk = minmea_tofloat(&f.true_track_degrees);
        if (!isnan(kph))
            gps->fix.speed_kph = kph;
        if (!isnan(trk))
            gps->fix.course_deg = trk;
    }
    return 1;
}

static int on_zda(iv_gps_t *gps, const char *line)
{
    struct minmea_sentence_zda f;

    if (!minmea_parse_zda(&f, line)) {
        gps->stats.malformed++;
        return 0;
    }
    gps->stats.sentences_ok++;

    /* ZDA 是"明确带四位年 + 时刻"的第二个时间源（且不受 fix 有效性影响）*/
    if (f.date.year != -1 && f.time.hours >= 0) {
        gps->internal_year      = f.date.year;
        gps->internal_month     = f.date.month;
        gps->internal_day       = f.date.day;
        gps->internal_have_date = 1;
        gps->internal_hour      = f.time.hours;
        gps->internal_minute    = f.time.minutes;
        gps->internal_second    = f.time.seconds;
        gps->internal_usec      = f.time.microseconds;
        gps->internal_have_utc  = 1;
        merge_time(gps);
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * 语句"形状"检查：$ttsss,...*CS
 * ------------------------------------------------------------------------- */
/* **只看形状，不看校验和** —— 这是本模块唯一能区分"校验和对不上"与"行都错位了"
 * 的办法，因为上游 `minmea_check()` / `minmea_sentence_id()` 都会**先校验校验和**：
 * 只要校验和错，两者都直接说"非法"，于是"电气噪声"与"丢字节错位"就分不开了。
 * 而在现场这两者指向完全不同的原因：前者是波特率/电气问题，后者是行同步问题。
 * 判据取自 NMEA 0183 的规定：`$` 之后是 2 位 talker ＋ 3 位语句类型，都是字母，
 * 再往后必须是分隔符，且行内必须有一个带两位十六进制数的 `*`。*/
static int shape_ok(const char *line)
{
    const char *star;
    int         i;

    if (line[0] != '$')
        return 0;

    for (i = 1; i <= 5; i++) {
        if (!isalpha((unsigned char)line[i]))
            return 0;
    }

    if (line[6] != ',' && line[6] != '*')
        return 0;

    star = strchr(line, '*');
    if (star == NULL)
        return 0;
    /* star[1] 最多是结尾 NUL，isxdigit 对 NUL 返回 0，不会越界 */
    if (!isxdigit((unsigned char)star[1]) || !isxdigit((unsigned char)star[2]))
        return 0;

    return 1;
}

/* 解析一条已装配完整的语句。返回 1 = 成功计入 sentences_ok。*/
static int parse_line(iv_gps_t *gps, const char *line)
{
    enum minmea_sentence_id id;

    if (!minmea_check(line, true)) {
        if (shape_ok(line))
            gps->stats.checksum_bad++; /* 形状没毛病 ⇒ 内容/校验和对不上 */
        else
            gps->stats.malformed++;    /* 形状就不对 ⇒ 行同步或协议异常 */
        return 0;
    }

    id = minmea_sentence_id(line, true);
    switch (id) {
    case MINMEA_SENTENCE_GGA:
        return on_gga(gps, line);
    case MINMEA_SENTENCE_RMC:
        return on_rmc(gps, line);
    case MINMEA_SENTENCE_VTG:
        return on_vtg(gps, line);
    case MINMEA_SENTENCE_ZDA:
        return on_zda(gps, line);
    case MINMEA_SENTENCE_GSA:
    case MINMEA_SENTENCE_GSV:
    case MINMEA_SENTENCE_GBS:
    case MINMEA_SENTENCE_GLL:
    case MINMEA_SENTENCE_GST:
        /* 这些语句当前只用于"证明链路在来数据"，不参与定位快照与上报。
         * 卫星明细（GSV）留给 M5 的 Web 状态页时再取。*/
        gps->stats.sentences_ok++;
        return 1;
    case MINMEA_INVALID:
        gps->stats.malformed++;
        return 0;
    default:
        /* MINMEA_UNKNOWN：未支持的语句（如 GPTXT），校验正确即视为正常流量 */
        gps->stats.sentences_ok++;
        return 1;
    }
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */
void iv_gps_cfg_default(iv_gps_cfg_t *cfg)
{
    if (cfg == NULL)
        return;

    cfg->report_distance_m     = 50u;
    cfg->report_interval_ms    = 60000u;
    cfg->report_on_fix_change  = 1;

    cfg->sync_system_time = 1;
    cfg->min_year         = 2024;
    cfg->small_adj_ms     = 1000L;
    cfg->big_adj_ms       = 3600000L;
    cfg->sync_interval_ms = 60000u;

    cfg->timeops = NULL;
}

void iv_gps_init(iv_gps_t *gps, const iv_gps_cfg_t *cfg)
{
    iv_gps_cfg_t c;
    int          i;

    if (gps == NULL)
        return;

    iv_gps_cfg_default(&c);
    if (cfg != NULL)
        c = *cfg;

    /* 归一化：把会破坏语义的取值拉回默认，而不是照用。
     * 0 间隔意味着"每个定位周期都上报/校时"，那是资源浪费与时钟抖动源。*/
    if (c.report_interval_ms == 0u)
        c.report_interval_ms = 60000u;
    if (c.sync_interval_ms == 0u)
        c.sync_interval_ms = 60000u;
    if (c.min_year < 2000)
        c.min_year = 2000;
    if (c.small_adj_ms < 0L)
        c.small_adj_ms = 0L;
    if (c.big_adj_ms <= c.small_adj_ms)
        c.big_adj_ms = c.small_adj_ms; /* 防倒挂：区间为空时"大偏差"永不成立 */

    /* 逐字段清零：结构体里没有需要释放的资源，也没有需要保留的状态 */
    for (i = 0; i < (int)sizeof(*gps); i++)
        ((char *)gps)[i] = '\0';

    gps->cfg = c;

    fix_reset(&gps->fix);
    fix_reset(&gps->internal_reported);

    /* 未初始化时 minmea 依赖的"空"哨兵：日期/时刻都置 -1，
     * 与 minmea 自己解析空字段时的取值一致（见 minmea.c 的 'D' / 'T' 分支）。*/
    gps->internal_year  = -1;
    gps->internal_month = -1;
    gps->internal_day   = -1;
    gps->internal_hour  = -1;
    gps->internal_minute = -1;
    gps->internal_second = -1;
}

int iv_gps_feed(iv_gps_t *gps, const char *buf, size_t len)
{
    size_t i;
    int    parsed = 0;

    if (gps == NULL || buf == NULL || len == 0u)
        return IV_EINVAL;

    for (i = 0u; i < len; i++) {
        char c = buf[i];

        if (!gps->internal_in_sentence) {
            if (c == '$') {
                gps->internal_in_sentence = 1;
                gps->internal_overflow    = 0;
                gps->internal_line[0]     = '$';
                gps->internal_len         = 1u;
            } else {
                gps->stats.noise_bytes++;
            }
            continue;
        }

        if (c == '\r')
            continue; /* CR 不进缓冲（NMEA 行尾是 CRLF，也有模块只发 LF）*/

        if (c == '\n') {
            if (!gps->internal_overflow) {
                gps->internal_line[gps->internal_len] = '\0';
                if (gps->internal_len >= 8u)
                    parsed += parse_line(gps, gps->internal_line);
                else
                    gps->stats.malformed++; /* 最短合法语句也不止 8 字符 */
            }
            gps->internal_in_sentence = 0;
            gps->internal_len         = 0u;
            continue;
        }

        if (gps->internal_overflow)
            continue; /* 已判超长，丢弃到行尾（不截断解析，见头文件说明）*/

        if (gps->internal_len + 1u >= sizeof(gps->internal_line)) {
            gps->internal_overflow = 1;
            gps->stats.too_long++;
            continue;
        }

        gps->internal_line[gps->internal_len++] = c;
    }

    return parsed;
}

int iv_gps_fix(const iv_gps_t *gps, iv_gps_fix_t *out)
{
    if (gps == NULL || out == NULL)
        return IV_EINVAL;

    *out = gps->fix;
    return IV_OK;
}

int iv_gps_should_report(const iv_gps_t *gps, uint64_t now_ms)
{
    if (gps == NULL)
        return 0;

    /* ① 首次：只有拿到有效定位才值得报（无效定位没有信息量）*/
    if (!gps->internal_have_reported)
        return gps->fix.valid ? 1 : 0;

    /* ② 定位有效性变化：这正是上层最需要立刻知道的事（失锁 / 复锁）*/
    if (gps->cfg.report_on_fix_change != 0 &&
        gps->fix.valid != gps->internal_reported.valid)
        return 1;

    if (!gps->fix.valid)
        return 0; /* 无效定位不重复上报（变化时已由 ② 覆盖）*/

    /* ③ 位移超阈值 */
    if (!isnan(gps->fix.latitude) && !isnan(gps->internal_reported.latitude)) {
        double d = distance_m(gps->internal_reported.latitude,
                              gps->internal_reported.longitude,
                              gps->fix.latitude,
                              gps->fix.longitude);
        if (d > (double)gps->cfg.report_distance_m)
            return 1;
    }

    /* ④ 最长周期 */
    if (now_ms - gps->internal_reported_ms >= (uint64_t)gps->cfg.report_interval_ms)
        return 1;

    return 0;
}

void iv_gps_mark_reported(iv_gps_t *gps, uint64_t now_ms)
{
    if (gps == NULL)
        return;

    gps->internal_reported      = gps->fix;
    gps->internal_reported_ms   = now_ms;
    gps->internal_have_reported = 1;
}

int iv_gps_sync_time(iv_gps_t *gps, uint64_t now_ms)
{
    struct minmea_date d;
    struct minmea_time t;
    struct tm          tm;
    struct timespec    ts_gps;
    struct timespec    ts_now;
    struct timeval     delta;
    const iv_gps_timeops_t *ops;
    long               diff_ms;
    int                rc;

    if (gps == NULL)
        return IV_EINVAL;

    if (gps->cfg.sync_system_time == 0)
        return IV_EAGAIN; /* 明确关闭：不碰时钟，也不算失败 */

    if (!gps->internal_have_date || !gps->internal_have_utc)
        return IV_ESTATE; /* 还没有可信日期（见文件头第 2 条）*/

    if (gps->internal_synced_once &&
        (now_ms - gps->internal_last_sync_ms) < (uint64_t)gps->cfg.sync_interval_ms)
        return IV_EAGAIN; /* 节流 */

    d.day   = gps->internal_day;
    d.month = gps->internal_month;
    d.year  = gps->internal_year;
    t.hours = gps->internal_hour;
    t.minutes = gps->internal_minute;
    t.seconds = gps->internal_second;
    t.microseconds = (int)gps->internal_usec;

    if (minmea_getdatetime(&tm, &d, &t) != 0 || minmea_gettime(&ts_gps, &d, &t) != 0) {
        gps->stats.time_sync_rejected++;
        IV_LOG_W(GPS_MOD, "NMEA date/time cannot be converted to epoch, sync skipped");
        return IV_ESTATE;
    }

    /* 日期合理性：这是最重要的一道闸 —— 模块冷启动或空字段很容易产出
     * 1970/1980/2000 之类"看起来是数字"的日期，照调会把全机时间线打乱 */
    if (tm.tm_year + 1900 < gps->cfg.min_year) {
        gps->stats.time_sync_rejected++;
        IV_LOG_W(GPS_MOD, "NMEA date %04d-%02d-%02d is older than min_year %d, sync skipped",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, gps->cfg.min_year);
        return IV_ESTATE;
    }

    if (clock_gettime(CLOCK_REALTIME, &ts_now) != 0)
        return IV_EIO;

    diff_ms = (long)(((int64_t)ts_gps.tv_sec - (int64_t)ts_now.tv_sec) * 1000LL) +
              (long)((ts_gps.tv_nsec - ts_now.tv_nsec) / 1000000L);

    /* 大偏差：只审计、不调整。一次离谱跳变会打乱全机日志时间线，
     * 而"GPS 时间离谱"本身是更值得记录的故障信号。*/
    if (abs_long(diff_ms) > gps->cfg.big_adj_ms) {
        gps->stats.time_sync_rejected++;
        IV_LOG_W(GPS_MOD,
                 "system time delta %ld ms exceeds big_adj_ms %ld, ADJUSTMENT SKIPPED "
                 "(audit only; NMEA UTC %04d-%02d-%02d %02d:%02d:%02d)",
                 diff_ms, gps->cfg.big_adj_ms, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        return IV_ERANGE;
    }

    ops = gps->cfg.timeops;

    if (abs_long(diff_ms) < gps->cfg.small_adj_ms) {
        /* 小偏差：adjtime 渐进调整，不产生时间跳变 */
        delta.tv_sec  = diff_ms / 1000L;
        delta.tv_usec = (diff_ms % 1000L) * 1000L;
        rc = (ops != NULL && ops->adjtime_fn != NULL)
                 ? ops->adjtime_fn(&delta, NULL)
                 : adjtime(&delta, NULL);
    } else {
        struct timeval target;
        target.tv_sec  = ts_gps.tv_sec;
        target.tv_usec = ts_gps.tv_nsec / 1000L;
        rc = (ops != NULL && ops->settimeofday_fn != NULL)
                 ? ops->settimeofday_fn(&target, NULL)
                 : settimeofday(&target, NULL);
    }

    if (rc != 0) {
        IV_LOG_E(GPS_MOD, "clock adjustment failed (errno captured by caller)");
        return IV_EIO;
    }

    gps->internal_last_sync_ms = now_ms;
    gps->internal_synced_once  = 1;
    gps->stats.time_sync_ok++;

    IV_LOG_I(GPS_MOD, "system time adjusted by %ld ms (NMEA UTC %04d-%02d-%02d %02d:%02d:%02d)",
             diff_ms, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    return IV_OK;
}
