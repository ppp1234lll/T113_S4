/*
 * IVSBox 统一日志门面（架构 §12.1，计划 M1-S2 第 2 条）
 *
 * 职责边界：
 *   - 全进程唯一的日志出口：板端走 syslog(3) 由 logd 汇聚（logread 查看），
 *     Host 单测 / 前台调试可切 stderr；不自己做文件轮转与落盘（logd 职责）。
 *   - 级别直接采用 <syslog.h> 的 LOG_ERR~LOG_DEBUG（数值越小越严重），
 *     编译期（IV_LOG_COMPILE_LEVEL）+ 运行期（iv_log_set_level）双重过滤。
 *   - 日志字段：时间（syslog 自带 / stderr 路径自拼）、级别、模块、事件码。
 *   - 带故障码（IV_ERR_*）的事件支持"首次 / 计数 / 恢复"风暴抑制：
 *     同一 (module, code) 在窗口内只打首次，其余静默计数；
 *     窗口过期再遇先补计数行；恢复时补 "recovered, xN"。
 *   - 脱敏是调用方纪律（§12.1）：fmt 与参数中禁止出现平台密钥、摄像机口令、
 *     SNMP community 和完整授权令牌；本模块不自动识别。
 *
 * 线程说明：libivcore 仅依赖 libc，不引入 pthread。抑制表读改写存在良性竞态
 * （多线程下最坏少计/多计一次抑制），日志行本身每条独立，不影响正确性。
 */
#ifndef IVSBOX_IV_LOG_H
#define IVSBOX_IV_LOG_H

#include <stdint.h>
#include <syslog.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 编译期最高输出级别：高于它的 IV_LOG_* 宏调用在常量折叠后被剔除（默认全放行） */
#ifndef IV_LOG_COMPILE_LEVEL
#define IV_LOG_COMPILE_LEVEL LOG_DEBUG
#endif

/* 模块名建议 3~15 字符纯 ASCII（如 "app"、"link"、"netmgr"）；过长不截断仅不推荐 */
int  iv_log_init(const char *ident);

/* 运行期过滤：数值大于该级别的日志被丢弃。默认 LOG_DEBUG（全放行） */
void iv_log_set_level(int level);
int  iv_log_get_level(void);

/* 输出目的地：0 = syslog(3)（默认，板端 logd 汇聚），1 = stderr（单测 / 前台调试） */
void iv_log_set_stderr(int enable);

/* 风暴抑制窗口（秒）。默认 60；仅作用于 iv_log_code()，0 = 关闭抑制 */
void iv_log_set_window(uint32_t seconds);

/* 单测注入口：设置后所有输出改走该回调（line 为单行正文，不含时间戳）。
 * 优先级最高，设置后 syslog / stderr 均不再输出。 */
typedef void (*iv_log_sink_t)(int level, const char *module, const char *line, void *user);
void iv_log_set_sink(iv_log_sink_t sink, void *user);

/*
 * 常规日志（不参与抑制）。消息中的控制字符（\n、\r 等）折叠为空格保证单行；
 * 超长截断时以 "..." 结尾标记。
 */
void iv_log_write(int level, const char *module, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
;

/*
 * 带故障码日志。输出形如：
 *   ERR [link] NET_CAMERA1_FAULT(0x20410000): cam1 rtsp timeout
 * 抑制规则（窗口默认 60s）：
 *   - 窗口内重复：静默，仅计数；
 *   - 窗口过期再遇：先补一行 "...(0x...): xN in Ws"，再按新窗口打本次首次行；
 *   - iv_log_recover()：存在静默计数时补一行 "...(0x...): recovered, xN in Ws"，
 *     无记录或仅打过 1 次则静默。
 * code 取 IVSBox 统一码空间（include/ivsbox/iv_err.h），名字由 iv_strerror() 给出。
 */
void iv_log_code(int level, const char *module, uint32_t code, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
;

/* 故障恢复：补计数行（见 iv_log_code 说明），级别固定 LOG_INFO */
void iv_log_recover(const char *module, uint32_t code);

/* ---------------------------------------------------------------------------
 * 组合宏：编译期 + 运行期双重过滤
 * ------------------------------------------------------------------------- */
#define IV_LOG(level, mod, ...)                                                        \
    do {                                                                               \
        if ((level) <= IV_LOG_COMPILE_LEVEL)                                           \
            iv_log_write((level), (mod), __VA_ARGS__);                                 \
    } while (0)

#define IV_LOG_CODE(level, mod, code, ...)                                             \
    do {                                                                               \
        if ((level) <= IV_LOG_COMPILE_LEVEL)                                           \
            iv_log_code((level), (mod), (code), __VA_ARGS__);                          \
    } while (0)

#define IV_LOG_E(mod, ...) IV_LOG(LOG_ERR, mod, __VA_ARGS__)
#define IV_LOG_W(mod, ...) IV_LOG(LOG_WARNING, mod, __VA_ARGS__)
#define IV_LOG_I(mod, ...) IV_LOG(LOG_INFO, mod, __VA_ARGS__)
#define IV_LOG_D(mod, ...) IV_LOG(LOG_DEBUG, mod, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_LOG_H */
