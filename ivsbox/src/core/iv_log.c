/*
 * IVSBox 统一日志门面（架构 §12.1，计划 M1-S2 第 2 条）
 * 板端：自建 <root>/YYYY-MM-DD/HH.log（root 默认 /mnt/UDISK/log，保留 3 个日历日）；
 * Host 单测 / 调试：注入口或 stderr。不再走 syslog(3)（本板无 logread）。
 */
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_err.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define IV_LOG_MSG_MAX 256u                       /* 单条消息正文上限 */
#define IV_LOG_BODY_MAX (IV_LOG_MSG_MAX + 64u)    /* 正文 + 码名前缀 */
#define IV_LOG_SUP_MAX 32u                        /* 抑制表上限（有界，架构 §1.3） */

#define IV_LOG_ROOT_MAX 192u                      /* 根目录字符串上限 */
#define IV_LOG_PATH_MAX 256u                      /* <root>/YYYY-MM-DD/HH.log 上限 */
#define IV_LOG_DATE_LEN 10u                       /* "YYYY-MM-DD" 字符数 */
#define IV_LOG_PURGE_SEC 86400u                   /* 过期清理节流间隔（秒） */

typedef struct {
    uint32_t key;      /* (module, code) 的 32 位指纹 */
    uint64_t first_ms; /* 窗口起点（CLOCK_MONOTONIC 毫秒，不受墙钟回拨影响） */
    uint32_t count;    /* 窗口内总次数（含首次） */
    int      used;
} iv_sup_ent_t;

static char             s_ident[IV_LOG_IDENT_MAX] = "ivsbox";
static int              s_stderr     = 0;
static int              s_level      = LOG_DEBUG;
static uint32_t         s_window     = 60;
static iv_log_sink_t    s_sink;
static void            *s_sink_user;
static iv_sup_ent_t     s_sup[IV_LOG_SUP_MAX];

/* ---- 落盘状态 ---- */
static char     s_root[IV_LOG_ROOT_MAX] = IV_LOG_ROOT_DEFAULT;
static unsigned s_keep_days             = IV_LOG_KEEP_DAYS_DEFAULT;
static size_t   s_file_max              = 1024u * 1024u; /* 单小时文件字节上限，0 = 不限制 */
static FILE    *s_fp;                         /* 当前小时文件句柄，跨小时才重开 */
static char     s_fp_path[IV_LOG_PATH_MAX];   /* 与 s_fp 对应的路径 */
static time_t   s_last_purge;                 /* 上次过期清理时刻，0 = 从未 */
static int      s_file_warned;                /* 落盘失败只提醒一次，避免刷屏 */
static int      s_cap_marked;                 /* 当前小时文件已写过触顶标记 */

/* ---- 并发控制 ----
 * 全模块唯一的一把 C11 atomic_flag 自旋锁：串行化上述全部状态（抑制表、文件句柄、
 * 各设置项），格式化在锁外完成。锁内不回调任何用户代码，唯一例外是 sink 回调
 * （见 emit），其"回调中禁止再入本模块"的契约写在 iv_log.h。
 */
static atomic_flag s_log_lock = ATOMIC_FLAG_INIT;

static void log_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_log_lock, memory_order_acquire))
        ;
}

static void log_unlock(void)
{
    atomic_flag_clear_explicit(&s_log_lock, memory_order_release);
}

/* ---------------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------------- */

static const char *lvl_str(int level)
{
    switch (level) {
    case LOG_ERR:     return "ERR";
    case LOG_WARNING: return "WARN";
    case LOG_INFO:    return "INFO";
    case LOG_DEBUG:   return "DEBG";
    default:          return "LOG";
    }
}

/* 控制字符折叠为空格，保证每条日志严格单行（板端按行读取） */
static void sanitize(char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20u)
            *s = ' ';
    }
}

/* 单调毫秒（CLOCK_MONOTONIC）。抑制窗口用它计时，不吃墙钟回拨的亏；
 * 与 iv_reactor.c 同口径：core 层不依赖 libivhal 的 iv_clock。 */
static uint64_t now_mono_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void fmt_body(char *buf, size_t cap, const char *fmt, va_list ap)
{
    int n = vsnprintf(buf, cap, fmt, ap);
    if (n < 0) {
        buf[0] = '\0';
        return;
    }
    if ((size_t)n >= cap) {
        /* 截断：以 "..." 标记 */
        buf[cap - 4] = '.';
        buf[cap - 3] = '.';
        buf[cap - 2] = '.';
        buf[cap - 1] = '\0';
    }
    sanitize(buf);
}

/* "YYYY-MM-DD HH:MM:SS"；失败时置空串 */
static void now_stamp(char *out, size_t cap)
{
    time_t    now = time(NULL);
    struct tm tmv;

    out[0] = '\0';
    if (!localtime_r(&now, &tmv))
        return;
    (void)strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

/*
 * dst = a + "/" + b。手工拼接而非 snprintf：路径长度由调用方保证，
 * 这样 GCC 的 -Wformat-truncation 不会对未知长度参数误报（-Werror 下会直接断编译）。
 */
static int join2(char *dst, size_t cap, const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);

    if (la + 1u + lb + 1u > cap)
        return -1;

    memcpy(dst, a, la);
    dst[la] = '/';
    memcpy(dst + la + 1u, b, lb + 1u);
    return 0;
}

/* ---------------------------------------------------------------------------
 * 落盘：<root>/YYYY-MM-DD/HH.log
 * ------------------------------------------------------------------------- */

static void file_reset(void)
{
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }
    s_fp_path[0] = '\0';
}

/* 创建 root 及其下子目录 sub（单层）。返回 0 成功 */
static int ensure_dir(const char *root, const char *sub)
{
    char path[IV_LOG_PATH_MAX];

    if (mkdir(root, 0755) != 0 && errno != EEXIST)
        return -1;
    if (join2(path, sizeof(path), root, sub) != 0)
        return -1;
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

/* "HH.log"：tm_hour 已限定 0~23，两位数字手工生成 */
static int hour_name(char *out, size_t cap, int hour)
{
    if (cap < 7u || hour < 0 || hour > 23)
        return -1;

    out[0] = (char)('0' + (hour / 10) % 10);
    out[1] = (char)('0' + hour % 10);
    out[2] = '.';
    out[3] = 'l';
    out[4] = 'o';
    out[5] = 'g';
    out[6] = '\0';
    return 0;
}

/*
 * 取当前应写入的文件句柄。路径与已打开的一致则直接复用（正常路径下每条日志只做
 * 一次 strcmp）；跨小时/跨天或首次打开才建目录、开文件。
 * 失败返回 NULL，并只在首次失败时往 stderr 打一行（避免日志系统自身刷屏）。
 */
static FILE *file_current(void)
{
    char      day[IV_LOG_DATE_LEN + 1];
    char      name[8];
    char      dir[IV_LOG_PATH_MAX];
    char      path[IV_LOG_PATH_MAX];
    struct tm tmv;
    time_t    now;
    FILE     *fp;

    if (!s_root[0])
        return NULL;

    now = time(NULL);
    if (!localtime_r(&now, &tmv))
        return NULL;
    if (strftime(day, sizeof(day), "%Y-%m-%d", &tmv) == 0)
        return NULL;
    if (hour_name(name, sizeof(name), tmv.tm_hour) != 0)
        return NULL;

    if (join2(dir, sizeof(dir), s_root, day) != 0)
        return NULL;
    if (join2(path, sizeof(path), dir, name) != 0)
        return NULL;

    if (s_fp && strcmp(path, s_fp_path) == 0)
        return s_fp;

    if (ensure_dir(s_root, day) != 0) {
        if (!s_file_warned) {
            s_file_warned = 1;
            fprintf(stderr, "iv_log: cannot create %s, file output off\n", dir);
        }
        return NULL;
    }

    fp = fopen(path, "a");
    if (!fp) {
        if (!s_file_warned) {
            s_file_warned = 1;
            fprintf(stderr, "iv_log: cannot open %s, file output off\n", path);
        }
        return NULL;
    }

    setvbuf(fp, NULL, _IOLBF, 0); /* 行缓冲：逐行落盘，进程异常退出不丢行 */

    file_reset();
    s_fp = fp;
    s_cap_marked = 0; /* 新小时文件：触顶标记重新计，恢复写入 */
    memcpy(s_fp_path, path, sizeof(s_fp_path)); /* 两侧同宽，末尾 NUL 一并复制 */
    return s_fp;
}

/* 目录名须严格为 10 位 "YYYY-MM-DD"（ISO 格式，字典序即时间序） */
static int is_date_name(const char *s)
{
    size_t i;

    if (strlen(s) != IV_LOG_DATE_LEN)
        return 0;
    for (i = 0; i < IV_LOG_DATE_LEN; i++) {
        if (i == 4u || i == 7u) {
            if (s[i] != '-')
                return 0;
        } else if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return 1;
}

/*
 * 保留期下界（含）的日期串：今天 -(keep-1) 天。
 * 用 mktime 归一化 tm_mday 的越界（跨月/跨年）比手算日历可靠。
 */
static int cutoff_day(unsigned keep, char *out, size_t cap)
{
    time_t    now = time(NULL);
    struct tm tmv;

    if (!localtime_r(&now, &tmv))
        return -1;
    tmv.tm_hour  = 0;
    tmv.tm_min   = 0;
    tmv.tm_sec   = 0;
    tmv.tm_isdst = -1;
    tmv.tm_mday -= (int)(keep - 1u);

    now = mktime(&tmv);
    if (now == (time_t)-1)
        return -1;
    if (!localtime_r(&now, &tmv))
        return -1;
    if (strftime(out, cap, "%Y-%m-%d", &tmv) == 0)
        return -1;

    return 0;
}

/*
 * 清空日期目录内的普通文件并删掉该目录（root_fd 锚定父目录）。
 * 全程 openat/unlinkat 家族且不跟随符号链接：日期项若是 symlink 或普通文件，
 * openat 带 O_NOFOLLOW|O_DIRECTORY 必然失败，直接跳过不删，杜绝被链接诱导
 * 删到别处。不递归（子目录一律跳过，防误删）。仅在锁内被调。
 */
static int drop_day_dir_at(int root_fd, const char *name)
{
    int            day_fd;
    DIR           *d;
    struct dirent *e;

    day_fd = openat(root_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (day_fd < 0)
        return -1; /* symlink / 普通文件 / 已消失：一律跳过 */

    d = fdopendir(day_fd);
    if (!d) {
        close(day_fd);
        return -1;
    }

    while ((e = readdir(d)) != NULL) {
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (fstatat(day_fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (!S_ISREG(st.st_mode))
            continue; /* 子目录 / 链接等一律不动 */
        (void)unlinkat(day_fd, e->d_name, 0);
    }
    closedir(d); /* 连同 day_fd 一并关闭 */

    /* AT_REMOVEDIR 只删空目录，对非目录 / symlink 天然失败，安全 */
    return (unlinkat(root_fd, name, AT_REMOVEDIR) == 0) ? 0 : -1;
}

/*
 * 过期清理主体。调用方必须已持有 s_log_lock（emit → purge_if_due 在锁内走到这里，
 * 公有 iv_log_purge 也由锁内转发，绝不能在这里再上锁，否则自旋死锁）。
 * root 经 open(O_NOFOLLOW) 锚定后全部走 openat 家族，不跟随符号链接。
 * 返回删除的日期目录数；root 打不开（还没建 / 是链接 / 无权限）返回 -1。
 */
static long purge_locked(void)
{
    char           cutoff[IV_LOG_DATE_LEN + 1];
    int            root_fd;
    DIR           *d;
    struct dirent *e;
    long           removed = 0;

    s_last_purge = time(NULL);

    if (!s_root[0] || s_keep_days == 0u)
        return 0;
    if (cutoff_day(s_keep_days, cutoff, sizeof(cutoff)) != 0)
        return -1;

    root_fd = open(s_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root_fd < 0)
        return -1;

    d = fdopendir(root_fd);
    if (!d) {
        close(root_fd);
        return -1;
    }

    while ((e = readdir(d)) != NULL) {
        struct stat st;

        if (!is_date_name(e->d_name))
            continue;
        if (strcmp(e->d_name, cutoff) >= 0)
            continue; /* 保留期内（含 cutoff 当天） */
        /* 日期项本身也不跟随链接：AT_SYMLINK_NOFOLLOW 下 symlink 不算目录 */
        if (fstatat(root_fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISDIR(st.st_mode))
            continue;

        if (drop_day_dir_at(root_fd, e->d_name) == 0)
            removed++;
    }
    closedir(d); /* 连同 root_fd 一并关闭 */

    return removed;
}

/* 写入路径上的节流清理：距上次满 24h（或时钟回拨）才真正扫盘。
 * 只在锁内被调，因此直接走 purge_locked()。 */
static void purge_if_due(void)
{
    time_t now = time(NULL);

    if (s_last_purge == 0 || now < s_last_purge
        || (unsigned long)(now - s_last_purge) >= IV_LOG_PURGE_SEC)
        (void)purge_locked();
}

/* ---------------------------------------------------------------------------
 * 出口
 * ------------------------------------------------------------------------- */

static void emit(int level, const char *module, const char *body)
{
    char ts[24];

    if (s_sink) {
        s_sink(level, module, body, s_sink_user);
        return; /* 注入口独占：单测不落盘、不打屏 */
    }

    ts[0] = '\0';
    if (s_root[0] || s_stderr)
        now_stamp(ts, sizeof(ts));

    if (s_root[0]) {
        FILE *fp;

        purge_if_due();
        fp = file_current();
        if (fp) {
            if (s_file_max > 0u) {
                struct stat st;

                /* 触顶：丢弃本条。首次触顶先写一行标记（标记行允许超限一次），
                 * 换新小时文件后由 file_current 重置标记、恢复写入。仅在锁内执行。 */
                if (fstat(fileno(fp), &st) == 0 && (size_t)st.st_size >= s_file_max) {
                    if (!s_cap_marked) {
                        s_cap_marked = 1;
                        (void)fprintf(fp, "[log] file size cap reached (%lu bytes), "
                                          "messages dropped until next hour\n",
                                      (unsigned long)s_file_max);
                    }
                    return;
                }
            }
            if (fprintf(fp, "%s %s [%s] %s\n", ts, lvl_str(level), module, body) < 0)
                file_reset(); /* 磁盘满 / 卡被拔：关掉，下次写入重开 */
        }
    }

    if (s_stderr)
        fprintf(stderr, "%s %s [%s] %s\n", ts, lvl_str(level), module, body);
}

/* ---------------------------------------------------------------------------
 * 风暴抑制表（仅 iv_log_code 使用）
 * ------------------------------------------------------------------------- */

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* (module, code) 的 32 位指纹：理论上可碰撞，碰撞后果仅是抑制行少打/多打一行 */
static uint32_t sup_key(const char *module, uint32_t code)
{
    return fnv1a(module) ^ (code * 2654435761u);
}

static iv_sup_ent_t *sup_find(uint32_t key)
{
    size_t i;

    for (i = 0; i < IV_LOG_SUP_MAX; i++) {
        if (s_sup[i].used && s_sup[i].key == key)
            return &s_sup[i];
    }
    return NULL;
}

/* 取空槽；表满时驱逐窗口起点最老的条目 */
static iv_sup_ent_t *sup_slot(void)
{
    iv_sup_ent_t *oldest = NULL;
    size_t        i;

    for (i = 0; i < IV_LOG_SUP_MAX; i++) {
        if (!s_sup[i].used)
            return &s_sup[i];
        if (!oldest || s_sup[i].first_ms < oldest->first_ms)
            oldest = &s_sup[i];
    }
    return oldest;
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

int iv_log_init(const char *ident)
{
    log_lock();
    if (ident && *ident) {
        strncpy(s_ident, ident, sizeof(s_ident) - 1u);
        s_ident[sizeof(s_ident) - 1u] = '\0';
    }

    /* 落盘是懒创建的：这里只保证根目录存在并先清一次过期，失败不阻断启动
     * （/mnt/UDISK/log 不可写时由首次写入打一行 stderr 提示）。已在锁内，走 purge_locked */
    if (s_root[0])
        (void)mkdir(s_root, 0755);
    (void)purge_locked();
    log_unlock();

    return 0;
}

void iv_log_set_level(int level)
{
    log_lock();
    s_level = level;
    log_unlock();
}

int iv_log_get_level(void)
{
    int level;

    log_lock();
    level = s_level;
    log_unlock();
    return level;
}

int iv_log_set_root(const char *root)
{
    int ret = 0;

    log_lock();
    file_reset();
    s_file_warned = 0;

    if (!root || !*root) {
        s_root[0]    = '\0';
        s_last_purge = 0;
    } else if (strlen(root) >= sizeof(s_root)) {
        ret = -1;
    } else {
        memcpy(s_root, root, strlen(root) + 1u);
        s_last_purge = 0;
    }
    log_unlock();

    return ret;
}

int iv_log_get_root(char *out, size_t cap)
{
    size_t len;
    int    ret = 0;

    if (out == NULL || cap == 0u)
        return -1;

    log_lock();
    len = strlen(s_root) + 1u;
    if (len > cap)
        ret = -1;
    else
        memcpy(out, s_root, len);
    log_unlock();
    return ret;
}

void iv_log_set_keep_days(unsigned days)
{
    log_lock();
    s_keep_days  = days;
    s_last_purge = 0;
    log_unlock();
}

unsigned iv_log_get_keep_days(void)
{
    unsigned days;

    log_lock();
    days = s_keep_days;
    log_unlock();
    return days;
}

/* 单个小时文件的字节上限。0 = 不限制（默认 1 MiB，见 iv_log.h） */
void iv_log_set_file_max(size_t max_bytes)
{
    log_lock();
    s_file_max = max_bytes;
    log_unlock();
}

int iv_log_purge(void)
{
    long removed;

    log_lock();
    removed = purge_locked();
    log_unlock();

    return (int)removed;
}

void iv_log_set_stderr(int enable)
{
    log_lock();
    s_stderr = enable;
    log_unlock();
}

void iv_log_set_window(uint32_t seconds)
{
    log_lock();
    s_window = seconds;
    log_unlock();
}

void iv_log_set_sink(iv_log_sink_t sink, void *user)
{
    log_lock();
    s_sink      = sink;
    s_sink_user = user;
    log_unlock();
}

void iv_log_write(int level, const char *module, const char *fmt, ...)
{
    char        body[IV_LOG_MSG_MAX];
    char        module_buf[IV_LOG_IDENT_MAX];
    const char *emit_module = module;
    int         enabled;
    va_list     ap;

    /* 级别和默认 ident 都是可并发修改的全局状态：必须在锁内读取。
     * ident 复制到栈上后再解锁，避免格式化期间引用可变全局缓冲。 */
    log_lock();
    enabled = (level <= s_level);
    if (enabled && emit_module == NULL) {
        memcpy(module_buf, s_ident, sizeof(module_buf));
        emit_module = module_buf;
    }
    log_unlock();
    if (!enabled)
        return;

    va_start(ap, fmt);
    fmt_body(body, sizeof(body), fmt, ap);
    va_end(ap);

    log_lock();
    /* 格式化期间运行级别可能被收紧；最终出口前再判一次，保证设置生效后不漏出
     * 一条旧级别消息。 */
    if (level <= s_level)
        emit(level, emit_module, body);
    log_unlock();
}

void iv_log_code(int level, const char *module, uint32_t code, const char *fmt, ...)
{
    char         body[IV_LOG_BODY_MAX];
    char         prev[IV_LOG_BODY_MAX];
    char         module_buf[IV_LOG_IDENT_MAX];
    const char  *emit_module = module;
    uint32_t     key;
    uint64_t     now;
    iv_sup_ent_t *e;
    int          enabled;
    va_list      ap;

    log_lock();
    enabled = (level <= s_level);
    if (enabled && emit_module == NULL) {
        memcpy(module_buf, s_ident, sizeof(module_buf));
        emit_module = module_buf;
    }
    log_unlock();
    if (!enabled)
        return;

    va_start(ap, fmt);
    {
        char msg[IV_LOG_MSG_MAX];

        fmt_body(msg, sizeof(msg), fmt, ap);
        snprintf(body, sizeof(body), "%s(0x%08X): %s",
                 iv_strerror(code), (unsigned)code, msg);
    }
    va_end(ap);

    log_lock();

    if (level > s_level) {
        log_unlock();
        return;
    }

    if (s_window == 0u) {
        /* 抑制关闭：逐条输出 */
        emit(level, emit_module, body);
        log_unlock();
        return;
    }

    key = sup_key(emit_module, code);
    now = now_mono_ms();
    e   = sup_find(key);
    if (!e) {
        /* 首次 */
        e           = sup_slot();
        e->used     = 1;
        e->key      = key;
        e->first_ms = now;
        e->count    = 1;
        emit(level, emit_module, body);
        log_unlock();
        return;
    }

    if (now - e->first_ms < (uint64_t)s_window * 1000u) {
        /* 窗口内：静默计数（单调差，墙钟回拨不影响窗口） */
        e->count++;
        log_unlock();
        return;
    }

    /* 窗口过期：先补计数行，再按新窗口打本次首次行 */
    snprintf(prev, sizeof(prev), "%s(0x%08X): x%u in %lds",
             iv_strerror(code), (unsigned)code, (unsigned)e->count,
             (long)((now - e->first_ms) / 1000u));
    emit(level, emit_module, prev);
    e->first_ms = now;
    e->count = 1;
    emit(level, emit_module, body);
    log_unlock();
}

void iv_log_recover(const char *module, uint32_t code)
{
    char         body[IV_LOG_BODY_MAX];
    uint32_t     key;
    iv_sup_ent_t *e;

    log_lock();
    if (!module)
        module = s_ident; /* 只在持锁区间内引用内部缓冲 */

    key = sup_key(module, code);
    e = sup_find(key);
    if (!e) {
        log_unlock();
        return; /* 无记录：静默 */
    }

    if (e->count > 1u) {
        /* 窗口内有静默计数：补恢复行（已静默时长用单调差） */
        uint64_t now = now_mono_ms();

        snprintf(body, sizeof(body), "%s(0x%08X): recovered, x%u in %lds",
                 iv_strerror(code), (unsigned)code, (unsigned)e->count,
                 (long)((now - e->first_ms) / 1000u));
        emit(LOG_INFO, module, body);
    }
    e->used = 0;
    log_unlock();
}
