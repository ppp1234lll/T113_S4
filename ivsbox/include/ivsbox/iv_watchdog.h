/*
 * IVSBox 看门狗设备薄包装（hal 层，功能开发计划 M1-S6）
 *
 * 定位：把板端 /dev/watchdog 的四个动作（打开 / 设超时 / 喂狗 / 关闭）包成
 * 四个 syscall 包装，**不做任何健康判定**。判据在 src/modules/iv_health.c：
 * 只有"主循环在走 + 慢任务没死锁"才调 iv_watchdog_keepalive()。
 *
 * 为什么要薄：判定逻辑必须能在宿主与 VM 上单测，而这两处都没有 /dev/watchdog。
 * 因此除 open() 外全部接口都接受**外部 fd**，测试注入一对 pipe 的写端即可
 * 观察到"喂了几次狗 / 有没有停喂"，不需要任何测试专用钩子。
 *
 * 板端实测（2026-09-28，TQ113 / armv7l / 内核 5.4.61-rt37，经 VM 跳板只读查证）：
 *   - /dev/watchdog  = crw-rw---- root:root 10,130
 *     /dev/watchdog0 = crw-rw---- root:root 249,0（misc 动态分配，/proc/devices 有 249 watchdog）
 *   - /sys/class/watchdog/watchdog0/device -> 20500a0.watchdog（即 sunxi-wdt）
 *   - /sys/class/watchdog/watchdog0/ 下**只有** dev/device/power/subsystem/uevent/
 *     waiting_for_supplier，**没有** timeout/state/name/identity 属性 —— sysfs 未启用，
 *     因此超时**只能走 ioctl**，sysfs 读写这条路不存在。
 *   - open() 会**立即激活**看门狗并开始倒计时：此后不喂就在 timeout 秒后整机复位。
 *   - 待实测项：WDIOC_SETTIMEOUT 是否真被该驱动接受（sysfs 无属性是个警示信号）。
 *     若返回 -1，调用方应降级为"使用驱动默认超时"，不得当致命错误。
 *
 * 依赖纪律：本模块属 libivhal（仅 libivcore/libc），只调 open/ioctl/write/close，
 * **不引入 pthread**。S5 的教训：凡是要用 pthread 的模块，先过一次"放哪一层"。
 */
#ifndef IVSBOX_IV_WATCHDOG_H
#define IVSBOX_IV_WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* 板端默认设备节点 */
#define IV_WATCHDOG_DEFAULT_PATH "/dev/watchdog"

/* 期望超时（秒）。取 30s 的理由：必须显著大于健康判据窗口
 * （IV_HEALTH_STUCK_MS_DEFAULT = 10s），否则"判据还没得出故障结论、
 * 看门狗就已经咬了"，门控形同虚设；同时给 S10 的 procd 重启窗口留余量。*/
#define IV_WATCHDOG_DEFAULT_TIMEOUT_SEC 30

/* 打开 /dev/watchdog（path 为 NULL 时用 IV_WATCHDOG_DEFAULT_PATH）。
 * 成功返回 fd，失败返回 -1 并置 errno。
 * **副作用（必须知道）**：一旦成功，设备即刻激活并开始倒计时；调用方必须尽快
 * keepalive()，或用 set_timeout() 先把窗口拉长。*/
int iv_watchdog_open(const char *path);

/* 设超时（秒）。返回内核**实际**接受的秒数（可能小于请求值）；失败返回 -1 并置
 * errno（如 EINVAL / ENOTTY = 该驱动不支持 SETTIMEOUT）。
 * seconds 传 0 表示"查询当前值"（Linux WDIOC_SETTIMEOUT 语义）。*/
int iv_watchdog_set_timeout(int fd, int seconds);

/* 喂狗。成功返回 0，失败返回 -1 并置 errno。
 * 实现即"向设备写 1 个字节"，这是 Linux watchdog 的标准契约。
 * **本函数不检查任何业务进度** —— 该不该喂由调用方决定。*/
int iv_watchdog_keepalive(int fd);

/* 关闭 fd。nowayout=0 时内核随之停止喂狗，timeout 秒后整机复位 ——
 * 这正是"判死 → 停喂 → 复位"链路依赖的行为。成功返回 0，失败返回 -1。*/
int iv_watchdog_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_WATCHDOG_H */
