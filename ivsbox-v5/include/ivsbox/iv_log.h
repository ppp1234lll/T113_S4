#ifndef IVS_LOG_H
#define IVS_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 极简分级日志（骨架实现）。
 *
 * 项目硬约束：**日志文本一律 ASCII，中文只允许写在注释里**
 * （板端控制台为 GBK，中文日志抓下来必然乱码）。
 *
 * 板端后续接入 syslog(LOG_LOCAL0) 由 logd 汇聚，运行日志与故障日志双通道落盘
 * （架构文档 §12.1）。当前先输出到 stderr，保证主机单元测试与板端早期调试都可用。
 *
 * 日志字段至少应包含：时间、级别、进程、模块、事件码、请求号、设备目标（§12.1）。
 * TODO(M1): 补事件码与模块枚举，并切换为 logd 通道。
 */

typedef enum {
    IVS_LOG_ERROR = 0,
    IVS_LOG_WARN  = 1,
    IVS_LOG_INFO  = 2,
    IVS_LOG_DEBUG = 3
} ivs_log_level_t;

void ivs_log_set_level(ivs_log_level_t level);
ivs_log_level_t ivs_log_get_level(void);

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void ivs_log_emit(ivs_log_level_t level, const char *module, const char *fmt, ...);

#define IVS_LOGE(mod, ...) ivs_log_emit(IVS_LOG_ERROR, (mod), __VA_ARGS__)
#define IVS_LOGW(mod, ...) ivs_log_emit(IVS_LOG_WARN, (mod), __VA_ARGS__)
#define IVS_LOGI(mod, ...) ivs_log_emit(IVS_LOG_INFO, (mod), __VA_ARGS__)
#define IVS_LOGD(mod, ...) ivs_log_emit(IVS_LOG_DEBUG, (mod), __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* IVS_LOG_H */
