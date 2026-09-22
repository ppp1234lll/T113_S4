#ifndef IVS_CLOCK_H
#define IVS_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 时间源（hal 层，只提供能力，不含业务策略）。
 *
 * 时钟纪律（架构文档 §7.7 + 缺口清单 G9）：
 *   1. 所有**时长、超时、窗口、退避、限流**一律使用 ivs_clock_mono_ms()；
 *   2. 墙钟只用于「给人看的时间戳」与证书有效期校验；
 *   3. 墙钟在被判定为可信之前，不得用于门锁有效期、隧道 token ttl、
 *      自愈冷却与频率上限等安全相关判断。
 *
 * TODO(G9): ivs_clock_wall_trusted() 的可信判定策略（平台授时 / GPS / NTP / RTC
 *           优先级，以及大偏差跳变的处理）尚未定，第一版程序跑通后补齐。
 *           另需处理 TLS 时间死锁：上电 RTC 未同步时证书校验失败会导致平台
 *           永远连不上，须有受控的降级路径。
 */

/* 单调时钟（毫秒）。不受对时影响，用于所有时长与超时计算。 */
uint64_t ivs_clock_mono_ms(void);

/* 墙钟（UTC 毫秒，自 Unix epoch）。仅用于展示与日志时间戳。 */
uint64_t ivs_clock_wall_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* IVS_CLOCK_H */
