/*
 * IVSBox 单调时钟（hal 层时间源，功能开发计划 M1-S2 第 3 条）
 *
 * 时钟纪律：
 *   - 所有时长、超时、窗口、退避、限流计算一律使用本接口（CLOCK_MONOTONIC，
 *     不受对时 / NTP 跳变影响）；全工程禁止直接用墙钟做时长测量。
 *   - 墙钟（time / gettimeofday）只用于给人看的时间戳与证书有效期校验；
 *     在被判定可信之前不得用于安全相关判断（缺口 G9，可信判定策略待定）。
 */
#ifndef IVSBOX_IV_CLOCK_H
#define IVSBOX_IV_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单调时钟毫秒（内核启动起算，只增不减，不受对时影响） */
uint64_t iv_clock_monotonic_ms(void);

/* 单调时钟微秒（同一时钟源，粒度更细；uint64 下约 58.5 万年不回绕） */
uint64_t iv_clock_monotonic_us(void);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_CLOCK_H */
