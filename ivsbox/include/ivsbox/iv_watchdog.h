/*
 * IVSBox 看门狗设备薄包装（hal 层，功能开发计划 M1-S6）
 *
 * 定位：把板端 /dev/watchdog 的动作（打开 / 设超时 / 查超时 / 喂狗 / 正常停机 /
 * 直接关闭）包成若干 syscall 薄包装，**不做任何健康判定**。判据在
 * src/modules/iv_health.c：只有"主循环在走 + 慢任务没死锁"才调
 * iv_watchdog_keepalive()。
 *
 * 为什么要薄：判定逻辑必须能在宿主与 VM 上单测，而这两处都没有 /dev/watchdog。
 * 因此除 open() 外全部接口都接受**外部 fd**，测试注入一对 pipe 的写端即可
 * 观察到"喂了几次狗 / 有没有停喂"，不需要任何测试专用钩子。
 *
 * 板端实测（2026-09-28 与 2026-09-29，TQ113 / armv7l / 内核 5.4.61-rt37，经 VM 跳板只读查证）：
 *   - /dev/watchdog  = crw-rw---- root:root 10,130
 *     /dev/watchdog0 = crw-rw---- root:root 249,0（misc 动态分配，/proc/devices 有 249 watchdog）
 *   - /sys/class/watchdog/watchdog0/device -> 20500a0.watchdog（即 sunxi-wdt）
 *   - /sys/class/watchdog/watchdog0/ 下**只有** dev/device/power/subsystem/uevent/
 *     waiting_for_supplier，**没有** timeout/state/name/identity 属性 —— 即
 *     `CONFIG_WATCHDOG_SYSFS` 未启用，因此超时**只能走 ioctl**，sysfs 读写这条路不存在。
 *   - 设备树节点 `soc@3000000/watchdog@20500A0`：`compatible = "allwinner,sun6i-a31-wdt"`，
 *     只有 compatible/interrupts/name/reg 四个属性，**没有 timeout / min-timeout /
 *     max-timeout** ⇒ 默认超时不会被 DT 改写，取驱动常量。
 *   - `sunxi_wdt` 与 `watchdog` 都是**内建**（`/lib/modules/5.4.61-rt37/modules.builtin`
 *     里列着 drivers/watchdog/{watchdog,sunxi_wdt}.ko，`/sys/module/sunxi_wdt/parameters/`
 *     不存在）⇒ 不能靠 rmmod/modprobe 重跑 probe 去读它的参数。
 *   - **超时上限 = 16s**：内核 `drivers/watchdog/sunxi_wdt.c` 的
 *     `WDT_MAX_TIMEOUT = 16` / `WDT_MIN_TIMEOUT = 1`，probe 里
 *     `timeout = max_timeout = 16`、`min_timeout = 1`，`wdt_timeout_map[]` 最大项即 `[16]`。
 *     内核 `watchdog_set_timeout()` 对越界值是 `return -EINVAL`，**拒绝而不是 clamp**，
 *     所以 `set_timeout(fd, 30)` 在板上必失败，实际窗口仍落在 16s。
 *   - open() 会**立即激活**看门狗并开始倒计时：此后不喂就在 timeout 秒后整机复位。
 *   - **close() 不会停狗**（2026-09-29 由内核 `watchdog_release()` 源码实证）：
 *     内核只在"接收到过 magic 字符 'V'"或"驱动未声明 WDIOF_MAGICCLOSE"时才停；
 *     sunxi-wdt 声明了 `WDIOF_MAGICCLOSE`，而本工程 keepalive 固定写 '\0' ⇒ close 之后
 *     内核补 1 次 ping 并打 `watchdog did not stop!`，狗继续倒计时。**这正是"判死 → 停喂 →
 *     复位"所依赖的行为**；反过来，正常停机必须显式写 'V'（见 iv_watchdog_disable()）。
 *   - 2026-09-29 核验时**无任何进程持有 /dev/watchdog**（硬件计时器是停的）。
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

/* 期望超时（秒）。**16s 是板端 sunxi-wdt 的上限**（`WDT_MAX_TIMEOUT`），不是随手取的数：
 * 健康判据的最坏结论时延 = stuck_ms(10s) + interval_ms(1s) = 11s，必须严格小于超时窗口，
 * 否则"判据还没得出故障结论、看门狗就已经咬了"，门控形同虚设。
 * 曾经取 30s（理由"给 S10 的 procd 重启窗口留余量"）—— 该值在 T113 上**不可达**：
 * 内核把超过 max_timeout 的请求直接 `-EINVAL` 拒绝且不 clamp，实际窗口仍落到 16s。
 * 现改为驱动上限值，使"请求值 = 实际值"，余量 ~5s。
 * 若要在别的板子上拉长窗口：先实测该驱动的 max_timeout，再改这里
 * （同时会触发 src/modules/iv_health.c 顶部的编译期不变量校验）。*/
#define IV_WATCHDOG_DEFAULT_TIMEOUT_SEC 16

/* 打开 /dev/watchdog（path 为 NULL 时用 IV_WATCHDOG_DEFAULT_PATH）。
 * 成功返回 fd，失败返回 -1 并置 errno。
 * **副作用（必须知道）**：一旦成功，设备即刻激活并开始倒计时；调用方必须尽快
 * keepalive()，或用 set_timeout() 先把窗口拉长。*/
int iv_watchdog_open(const char *path);

/* 设超时（秒）。成功返回内核**实际接受**的秒数；失败返回 -1 并置 errno
 * （如 EINVAL = 越界或参数非法 / ENOTTY = 该驱动不支持 SETTIMEOUT）。
 * seconds 必须为正：内核 SETTIMEOUT **没有**"0 表示查询"的语义，0 小于驱动最小超时
 * （sunxi-wdt 为 1s），传下去只会白跑一次 ioctl 拿 EINVAL；本函数在入口就挡掉。
 * **要读当前超时请用 iv_watchdog_get_timeout()**。
 * 注意越界**不会**被 clamp：sunxi-wdt 上限 16s，传 30 直接失败、窗口维持原值，
 * 调用方应据此降级并**重新校验"stuck_ms + interval_ms < 实际超时"**。*/
int iv_watchdog_set_timeout(int fd, int seconds);

/* 查询当前超时（秒）。成功返回内核记录的超时值，失败返回 -1 并置 errno
 * （ENOTTY = 设备/驱动不支持 GETTIMEOUT）。
 * 这条走的是**独立的 WDIOC_GETTIMEOUT**，与 SETTIMEOUT 是两码事。*/
int iv_watchdog_get_timeout(int fd);

/* 喂狗。成功返回 0，失败返回 -1 并置 errno。
 * 实现即"向设备写 1 个字节"，这是 Linux watchdog 的标准契约。
 * **本函数不检查任何业务进度** —— 该不该喂由调用方决定。*/
int iv_watchdog_keepalive(int fd);

/* 正常停机专用：写 magic 字符 'V' 再 close，令内核真正停掉看门狗。
 * 为什么需要单独一个接口：见文件头 —— 本工程 keepalive 写 '\0'，close() 本身**不会**
 * 停狗；有序退出（procd stop）时只 close 会让内核在约 timeout 秒后把整机复位一次。
 * **纪律：只有正常停机路径才调本函数；判死路径必须用 iv_watchdog_close()**，
 * 让狗继续跑、由整机复位兜底 —— 那才是门控的目的。
 * 成功返回 0；写 'V' 失败或 close 失败返回 -1 并置 errno（fd 一定已被关闭，
 * 不因停机动作失败而泄漏 fd）。*/
int iv_watchdog_disable(int fd);

/* 直接关闭 fd（**不停狗**，见文件头）。判死路径用这个。
 * 成功返回 0，失败返回 -1 并置 errno。*/
int iv_watchdog_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_WATCHDOG_H */
