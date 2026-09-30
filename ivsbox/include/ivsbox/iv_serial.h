/*
 * IVSBox 串口（libivhal，功能开发计划 M2-S2.1）
 *
 * ============================ 它是什么 ============================
 * `ivsboxd` 与采集板（MCU）之间那条 UART 链路的**物理层**：把设备节点按
 * "raw 模式 + 零流控 + 非阻塞"打开，提供 fd 级的读、写、冲、关闭。
 *
 * **它不理解协议**：不认帧头、不算 CRC、不做重传与去重 —— 那些是
 * `src/modules/link/`（S2.2 帧解析器、S2.3 可靠层）的事。本模块只搬字节。
 *
 * ============================ 为什么在 hal 层 ============================
 * 它只依赖 Linux 的 `termios` / `fcntl` / `flock`，不含任何业务策略，
 * 因此落 `src/hardware/`、进 `libivhal.a`（依赖方向见架构 §15.5：
 * `libivcore` → `libivhal` → `libivmodules`）。
 * 与本层其它模块同一条纪律：**不引入 pthread**（`iv_watchdog` 就是这么守住的），
 * 也**不引用 iv_reactor** —— 只交出 fd，由调用方注册进自己的事件循环。
 *
 * ============================ 平台事实（2026-09-30 板端实测） ============================
 *   - 板端串口设备是 **`/dev/ttySAC<n>`**（Allwinner 命名），**不是 `/dev/ttyS<n>`**；
 *     实测存在 `/dev/ttySAC2`、`ttySAC3`、`ttySAC4`、`ttySAC5`。
 *   - **`ttySAC3` 是内核 `console=`（也就是调试口 COM4）**，业务**不可占用**它。
 *   - **采集板确认接在 `/dev/ttySAC5`**（2026-09-30 用户确认）。`ttySAC2` / `ttySAC4`
 *     留作测试口（S2.1 的板端 `test_serial` 就是在两路上跑的）。
 *   - 板端**没有 `socat`**，虚拟串口对只能在编译 VM（Ubuntu）上造。
 *   - **串口电气参数（2026-09-30 用户确认）** —— 这类参数属**外部给定事实**，
 *     不是实现自由度，猜错的后果不是"报错"而是"不通"：
 *       · **采集板链路：`/dev/ttySAC5`，115200 8N1、无流控**；
 *       · **GPS 模组链路（UART1，PD21/PD22）：9600 8N1、无流控**（见 `iv_gps.h`）。
 *     两条链路波特率不同，是本模块最容易踩的坑（用默认值开 GPS 口 = 收不到数据，
 *     现象像"模块没上电"，而实际参数错了）。
 *     **流控：两条链路均无 RTS/CTS**（2026-09-30 用户确认 —— 此前是"按 8N1 惯例"
 *     的假设，现为确认事实），因此 `CRTSCTS` 显式关闭是正确设置，接线也不接 RTS/CTS。
 *     若将来换成有流控的对端，必须**同时改接线与这里的代码** —— 有流控而关掉它的
 *     表现是高速下静默丢数据，比"完全不通"更难定位。
 *   - **这些参数不进 `ivsbox.json`**（2026-09-30 用户决定：**硬件确定不变**）——
 *     以**编译期常量**形式存在（`IV_SERIAL_BAUD_DEFAULT` / `IV_GPS_UART_BAUD`），
 *     JSON 里不落键（登记口径见架构 §10.2）。理由两条：
 *       ① 它们不随现场变化，进配置等于**新增一个"配置写错 ⇒ 串口打不开"的故障源**；
 *       ② 串口配错的症状是"不通 / 满屏乱码"，**数据在流动但全是错的**，属现场最难查的
 *          一类；放在编译期能一眼看见，放在配置里只能在运行时炸。
 *     ⚠ 这与"把参数做成可传的结构体 `iv_serial_cfg_t`"**不矛盾**：结构体是本模块的
 *     **能力**（单测正是靠它造 7E2 等组合来验证 raw 设置是否正确），而"产品里用哪一组"
 *     是常量。**能力要留，默认值不留给现场改**。
 *
 * ============================ 单实例锁：为什么锁在 tty fd 上，而不是锁文件 ============================
 * 计划 §S2.1 原文写的是 `flock(/var/run/ivsbox/serial.lock)`。**本实现改为直接
 * `flock(串口 fd, LOCK_EX | LOCK_NB)`，不引入独立锁文件**，理由三条：
 *   1. **少一个路径依赖**。锁文件方案要额外依赖 `/var/run/ivsbox/` 已由 init 脚本
 *      预建；目录缺失时按"父目录不代建"的纪律只能报错，于是"串口打不开"与
 *      "锁目录没建"这两件毫不相干的事被绑在一起（`iv_config` 的单级 `mkdir`
 *      已经吃过一次这个亏，见缺口清单 G20）。
 *   2. **锁的生命周期与实际占用者严格同源**。`flock` 是**打开文件描述**上的锁，
 *      fd 一关（含进程崩溃退出）内核自动释放；锁文件方案要自己保证"锁 fd 与串口 fd
 *      同时关闭"，多一个可能泄漏、可能不一致的状态。
 *   3. **诊断能力不降级**：`fuser /dev/ttySAC2` / `lsof /dev/ttySAC2` 同样能看出谁占着。
 * 语义与验证都不变：同一路径**第二次打开一律 `IV_EBUSY`**（含同进程内重复打开，
 * 因为那是两个独立的打开文件描述，会互相冲突），第一次 close 之后可以重新打开。
 * 若将来遇到"某类设备节点不支持 flock"的硬件，再退回独立锁文件方案。
 *
 * ============================ 非阻塞与读写契约 ============================
 * `open` 带 `O_NONBLOCK | O_NOCTTY`，因此**读不会阻塞**：
 *   - `VMIN=0` / `VTIME=0` ⇒ 无数据时 `read()` 返回 0，本模块把它翻译成 `IV_EAGAIN`
 *     （调用方据此挂进 Reactor 等可读事件；绝不要把 0 当成"对端关闭"）。
 * 写是**字节流**语义，与 `iv_chan_send()`（整包语义）刻意不同：
 *   - `iv_serial_write()` 返回**实际写入的字节数**（可能小于请求长度），
 *     调用方负责续写剩余部分；返回负值才是错误。
 *     非阻塞 fd 在发送缓冲满时 `write()` 只写一部分或返回 `EAGAIN`，
 *     把它当成"全丢了"或"全写完了"都是错的。
 *
 * ============================ 返回码（全部来自 iv_ret.h） ============================
 *   IV_OK       成功
 *   IV_EINVAL   参数非法（NULL / 越界 / **不是 tty 设备**）
 *   IV_ENOENT   设备节点不存在
 *   IV_EAUTH    权限不足（EACCES / EPERM）
 *   IV_EBUSY    串口已被占用（`flock` 拿不到锁）
 *   IV_ENOTSUP  该设备不支持请求的参数（例如不支持的波特率）
 *   IV_EAGAIN   非阻塞且当前无数据可读
 *   IV_ECONN    设备已断开（USB 转串口被拔）
 *   IV_EIO      其它系统调用失败；**errno 保留原值**便于诊断
 * 注意：**`open` 的失败路径一律不留下"半开"状态** —— 锁或 termios 配置失败时
 * 会先把已打开的 fd 关掉再返回错误。
 */
#ifndef IVSBOX_IV_SERIAL_H
#define IVSBOX_IV_SERIAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 默认波特率 —— **这是"采集板链路"的既定值，不是"随便一个通用默认值"**。
 * 2026-09-30 用户确认：**采集板链路 = 115200 8N1、无流控**。
 * ⚠ 本板还有**第二条**串口链路：**GPS 模组接 UART1，参数是 9600 8N1**。
 *   两者波特率不同 ⇒ 打开 GPS 串口时**必须显式覆盖**，不要用这个默认值
 *   （对比见 `iv_gps.h` 的 `IV_GPS_UART_BAUD`）。*/
#define IV_SERIAL_BAUD_DEFAULT ((unsigned)115200)

/* 串口单次写入的**建议**上限：串口没有"包"概念，这只是给调用方一个参考值，
 * 超过它就该分片写，别一次性把几十 KB 塞进内核发送缓冲。*/
#define IV_SERIAL_WRITE_CHUNK ((size_t)1024u)

/* 校验位 */
typedef enum {
    IV_SERIAL_PARITY_NONE = 0,
    IV_SERIAL_PARITY_EVEN = 1,
    IV_SERIAL_PARITY_ODD  = 2
} iv_serial_parity_t;

/* 串口参数。`iv_serial_cfg_default()` 会填 115200 / 8 / 无校验 / 1 停止位。*/
typedef struct {
    unsigned           baud;      /* 仅接受标准波特率，见 iv_serial_open 的说明 */
    unsigned           data_bits; /* 5 ~ 8 */
    iv_serial_parity_t parity;
    unsigned           stop_bits; /* 1 或 2 */
} iv_serial_cfg_t;

/* 填默认参数：**115200 / 8 / 无校验 / 1 停止位 —— 即采集板链路的已确认值**
 * （2026-09-30 用户确认）。cfg 为 NULL 时本函数直接返回（不报错，便于"可选参数"式调用）。
 * ⚠ **GPS 链路是 9600 8N1**：拿本函数填出的值去开 GPS 串口是错的，
 *   必须把 `baud` 改成 `IV_GPS_UART_BAUD`（见 iv_gps.h）。*/
void iv_serial_cfg_default(iv_serial_cfg_t *cfg);

/* 打开串口并配置为 raw 模式。
 *   path : 设备节点，如 "/dev/ttySAC2"；**NULL 或空串 ⇒ IV_EINVAL**
 *         （刻意不给默认路径：串口用哪一路是部署决定，见文件头"平台事实"）
 *   cfg  : NULL ⇒ 用 iv_serial_cfg_default() 的值
 *
 * 成功返回 >= 0 的 fd（已设 `O_NONBLOCK | O_NOCTTY | O_CLOEXEC`，并**持锁**）；
 * 失败返回上表的 IV_* 负码。
 *
 * 打开流程（任一步失败都会把已开的 fd 关掉）：
 *   1. `open(O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC)`
 *   2. `flock(LOCK_EX|LOCK_NB)` —— 拿不到即 `IV_EBUSY`（防双实例抢串口）
 *   3. `isatty()` 校验 —— **不是 tty 一律拒绝**：否则会"成功"打开一个普通文件，
 *      后面每个 termios 调用都失败，故障点被推得很远
 *   4. `cfmakeraw` 后**显式**关 `CRTSCTS`、置 `CLOCAL|CREAD`、`VMIN=0`/`VTIME=0`，
 *      再按 cfg 设波特率/数据位/校验/停止位（顺序不能反：`cfmakeraw` 会把
 *      `CSIZE`/`PARENB` 一起清掉）
 *   5. `tcsetattr(TCSANOW)` + `tcflush(TCIOFLUSH)`
 *
 * 波特率只接受标准值（9600 / 19200 / 38400 / 57600 / 115200，以及本平台
 * 可用的 230400 / 460800 / 921600）。**不支持的波特率返回 IV_ENOTSUP**，
 * 不做"就近取整"—— 串口参数错配表现为乱码，很难在后期定位，必须在入口拦下。*/
int iv_serial_open(const char *path, const iv_serial_cfg_t *cfg);

/* 关闭串口并释放锁。fd < 0 ⇒ IV_EINVAL。
 * 对**已关闭的 fd** 再调一次会得到 IV_EIO（`close()` 的 EBADF），这是刻意的 ——
 * 容忍它会把"重复关闭"这种真实 bug 藏起来（同 iv_chan_listen_close）。
 * 每个 fd 只能关一次。*/
int iv_serial_close(int fd);

/* 非阻塞读。
 *   buf/cap : 接收缓冲；cap 为 0 ⇒ IV_EINVAL
 *   got     : 可为 NULL；非 NULL 时**仅在返回 IV_OK 时**写入读到的字节数
 * 返回 IV_OK（*got 可能为 0？**不会** —— 0 字节被翻译成 IV_EAGAIN）、
 * IV_EAGAIN（当前无数据）、IV_ECONN（设备断开）或其它 IV_* 错误。
 * 返回值是"是否读到"，不是字节数；字节数看 *got。*/
int iv_serial_read(int fd, void *buf, size_t cap, size_t *got);

/* 非阻塞写（字节流语义，详见文件头）。
 *   buf/len : len 为 0 ⇒ IV_EINVAL（写 0 字节是无意义的调用）
 * 返回 **>= 0 的实际写入字节数**（可能 < len），或负的 IV_* 错误码。
 * 被信号打断（EINTR）时本函数在同一调用内重试，不会因此提前返回。*/
int iv_serial_write(int fd, const void *buf, size_t len);

/* 冲掉收发缓冲（tcflush(TCIOFLUSH)）。重新同步链路状态时用。*/
int iv_serial_flush(int fd);

/* 读回设备上**实际生效**的参数，供自检与板端核对（对应计划 §S2.1 的
 * "`stty -F /dev/ttySAC<n>` 核对参数生效"）。返回 IV_OK / IV_* 错误。*/
int iv_serial_get_cfg(int fd, iv_serial_cfg_t *out);

/* 波特率数值 → 名字，仅供日志与错误提示。未登记的值返回 "unsupported"。*/
const char *iv_serial_baud_name(unsigned baud);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_SERIAL_H */
