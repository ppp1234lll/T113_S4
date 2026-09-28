/*
 * IVSBox 统一日志门面（架构 §12.1，计划 M1-S2 第 2 条）
 * 板端：syslog(3) -> logd（logread 查看）；Host 单测 / 调试：stderr 或注入口。
 */
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_err.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define IV_LOG_MSG_MAX 256u                       /* 单条消息正文上限 */
#define IV_LOG_BODY_MAX (IV_LOG_MSG_MAX + 64u)    /* 正文 + 码名前缀 */
#define IV_LOG_SUP_MAX 32u                        /* 抑制表上限（有界，架构 §1.3） */

typedef struct {
    uint32_t key;   /* (module, code) 的 32 位指纹 */
    time_t   first; /* 窗口起点 */
    uint32_t count; /* 窗口内总次数（含首次） */
    int      used;
} iv_sup_ent_t;

static const char      *s_ident      = "ivsbox";
static int              s_stderr     = 0;
static int              s_level      = LOG_DEBUG;
static uint32_t         s_window     = 60;
static iv_log_sink_t    s_sink;
static void            *s_sink_user;
static iv_sup_ent_t     s_sup[IV_LOG_SUP_MAX];

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

/* 控制字符折叠为空格，保证每条日志严格单行（板端串行控制台 / logd 均按行取） */
static void sanitize(char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20u)
            *s = ' ';
    }
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

static void emit(int level, const char *module, const char *body)
{
    if (s_sink) {
        s_sink(level, module, body, s_sink_user);
        return;
    }
    if (s_stderr) {
        time_t    now = time(NULL);
        struct tm tmv;
        char      ts[24];

        localtime_r(&now, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        fprintf(stderr, "%s %s [%s] %s\n", ts, lvl_str(level), module, body);
        return;
    }
    syslog(level, "[%s] %s", module, body);
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
        if (!oldest || s_sup[i].first < oldest->first)
            oldest = &s_sup[i];
    }
    return oldest;
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

int iv_log_init(const char *ident)
{
    if (ident && *ident)
        s_ident = ident;
    openlog(s_ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    return 0;
}

void iv_log_set_level(int level)
{
    s_level = level;
}

int iv_log_get_level(void)
{
    return s_level;
}

void iv_log_set_stderr(int enable)
{
    s_stderr = enable;
}

void iv_log_set_window(uint32_t seconds)
{
    s_window = seconds;
}

void iv_log_set_sink(iv_log_sink_t sink, void *user)
{
    s_sink      = sink;
    s_sink_user = user;
}

void iv_log_write(int level, const char *module, const char *fmt, ...)
{
    char    body[IV_LOG_MSG_MAX];
    va_list ap;

    if (level > s_level)
        return;
    if (!module)
        module = s_ident;

    va_start(ap, fmt);
    fmt_body(body, sizeof(body), fmt, ap);
    va_end(ap);

    emit(level, module, body);
}

void iv_log_code(int level, const char *module, uint32_t code, const char *fmt, ...)
{
    char    body[IV_LOG_BODY_MAX];
    char    prev[IV_LOG_BODY_MAX];
    uint32_t key;
    time_t   now;
    iv_sup_ent_t *e;
    va_list ap;

    if (level > s_level)
        return;
    if (!module)
        module = s_ident;

    va_start(ap, fmt);
    {
        char msg[IV_LOG_MSG_MAX];

        fmt_body(msg, sizeof(msg), fmt, ap);
        snprintf(body, sizeof(body), "%s(0x%08X): %s",
                 iv_strerror(code), (unsigned)code, msg);
    }
    va_end(ap);

    if (s_window == 0u) {
        /* 抑制关闭：逐条输出 */
        emit(level, module, body);
        return;
    }

    key = sup_key(module, code);
    now = time(NULL);
    e   = sup_find(key);
    if (!e) {
        /* 首次 */
        e        = sup_slot();
        e->used  = 1;
        e->key   = key;
        e->first = now;
        e->count = 1;
        emit(level, module, body);
        return;
    }

    if (now - e->first < (time_t)s_window) {
        /* 窗口内：静默计数 */
        e->count++;
        return;
    }

    /* 窗口过期：先补计数行，再按新窗口打本次首次行 */
    snprintf(prev, sizeof(prev), "%s(0x%08X): x%u in %lds",
             iv_strerror(code), (unsigned)code, (unsigned)e->count,
             (long)(now - e->first));
    emit(level, module, prev);
    e->first = now;
    e->count = 1;
    emit(level, module, body);
}

void iv_log_recover(const char *module, uint32_t code)
{
    char    body[IV_LOG_BODY_MAX];
    uint32_t key;
    iv_sup_ent_t *e;

    if (!module)
        module = s_ident;

    key = sup_key(module, code);
    e   = sup_find(key);
    if (!e)
        return; /* 无记录：静默 */

    if (e->count > 1u) {
        /* 窗口内有静默计数：补恢复行 */
        time_t now = time(NULL);

        snprintf(body, sizeof(body), "%s(0x%08X): recovered, x%u in %lds",
                 iv_strerror(code), (unsigned)code, (unsigned)e->count,
                 (long)(now - e->first));
        emit(LOG_INFO, module, body);
    }
    e->used = 0;
}
