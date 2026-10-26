/*
 * IVSBox 4G AT 口探测（libivmodules）
 *
 * ============================ 它解决什么问题 ============================
 * `iv_cellular` 是纯解析器，它假定"调用方已经打开了一个能用的 AT 口"。
 * 而"哪个 `/dev/ttyUSB*` 才是 AT 口"这件事**不能写死**：
 *
 *   1. **编号会漂**。模组 `AT+CFUN=1,1` 会让 USB 重新枚举（自动重拨就是这么干的，
 *      见 `IV_AGENT_CELL_DIAL_DEFAULT`），重新枚举后 ttyUSB 的分配可能换一套。
 *      写死 `/dev/ttyUSB0` 的表现是"重拨之后模块再也不回话"。
 *   2. **同一颗模组有多个口，只有一个认 AT**。EC200G 枚举出 4～5 个 tty：
 *      2026-10-23 板端逐口实测 —— `ttyUSB0` 回 `\r\nOK\r\n`（真 AT）、
 *      `ttyUSB1` 空、`ttyUSB2` 空、`ttyUSB3` 是持续吐二进制流的 diag 口、
 *      `ttyUSB4` 也回 `OK`。**打开的 fd 能读、有字节在涨，不等于 AT 通**
 *      —— diag 口就是典型：`rx_bytes` 一路涨到 5800 万，`valid` 恒 false。
 *   3. **设备节点会被替换**。USB 重枚举后 `/dev/ttyUSB1` 的 inode 从 3557 变成
 *      1157610，而进程手里的旧 fd 仍指向旧 inode，读取恒定 `EIO(-5)`。
 *      这种"fd 看着是开的、其实接在已消失的设备上"的状态，只靠 `fd >= 0`
 *      判断永远发现不了。
 *
 * 所以本模块只做一件事：**给一个设备路径，判定它是不是一个会回 `AT` 的 tty**。
 * 判定通过就把它**保持打开**交回去（调用方直接注册 Reactor），不通过就关掉。
 *
 * ============================ 为什么单独一个文件 ============================
 * `iv_cellular.h` 把边界划得很硬：**不做 IO**（不 open、不 read/write），
 * 因此能在 Host 上纯字符串单测。探口天生是 IO（open + 写 AT + 等 OK + termios），
 * 塞进去就把那条边界弄脏了。分开之后：
 *   - `iv_cellular` 继续是"喂字节→出快照"的纯解析器；
 *   - `iv_cellport` 是"哪个口能用"的物理层判定；
 *   - `iv_agent` 负责把两者接起来（并把探测结果记下来供重开时复用）。
 *
 * ============================ 只发 `AT`，不改模块状态 ============================
 * 探测命令只有 `AT\r` —— 这是模组规范里**唯一保证不改变任何状态**的命令。
 * 绝不在这里发 `AT+CFUN` / `AT+QCFG` / `AT+QNETDEVCTL`：AT 口与出厂拨号脚本
 * 共用（`iv_cellular.h` 的三条边界之一），探口是开机与重连路径上的高频动作，
 * 一旦发了会改状态的命令，就会把现场正在用的链路弄断。
 *
 * ============================ 为什么"打开成功"不算探测成功 ============================
 * `iv_serial_open()` 只要节点存在、权限够、flock 拿得到就会成功
 * —— 空的 `ttyUSB2` 也满足这三条。唯一可靠的判据是**对端回了 `OK`**。
 * 因此 `iv_cellport_probe()` 的成功条件是"写出去的 `AT` 得到了 `OK` 终结行"，
 * 而不是"open 返回值 >= 0"。
 *
 * ============================ 锁的副作用（重要） ============================
 * `iv_serial_open()` 会对 fd 加 `flock(LOCK_EX|LOCK_NB)` 独占锁。所以：
 *   - 探测某个口时如果它正被**本进程之外**的 `iv_serial_open()` 持有，会得到
 *     `IV_EBUSY`，本模块把它当成"这个口不是给我们的"，跳过继续扫；
 *   - 但**拨号脚本（`sh` + `cat`/`printf`）不加锁**，探测期间它可能同时在读写
 *     同一个口，于是我们的 `AT` 与它的会话交错、等不到 `OK`。
 *     这不是错误：调用方会周期性重试，拨号序列结束（几秒）后即可探到。
 */
#ifndef IVSBOX_IV_CELLPORT_H
#define IVSBOX_IV_CELLPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 候选设备名前缀与扫描上限：`/dev/ttyUSB0` … `/dev/ttyUSB5`。
 * EC200G 实测占 0～4，留一个余量；再多就是别的 USB 串口设备了
 * （采集板是 `/dev/ttySAC5`，GPS 是 `/dev/ttySAC1`，都不在这个前缀下）。 */
#define IV_CELLPORT_PREFIX   "/dev/ttyUSB"
#define IV_CELLPORT_SCAN_MAX 6u

/* AT 口参数：115200 8N1、无流控（2026-10-19 板端实测，属外部给定事实）。 */
#define IV_CELLPORT_BAUD_DEFAULT ((unsigned)115200)

/* 单个口的等待上限。EC200G 的 `AT` 往返实测 < 20 ms，200 ms 有 10 倍余量；
 * 取值同时也是**扫描最坏耗时**的分母：6 个口全不通时 ≈ 1.2 s。
 * 因此调用方必须只在"该重开"时调，不要在每拍定时器里扫。 */
#define IV_CELLPORT_WAIT_MS ((unsigned)200u)

/* 生成的路径缓冲：`/dev/ttyUSB5` 共 13 字节，32 足够并留出替换前缀的余量。 */
#define IV_CELLPORT_PATH_MAX 32u

/* 按 `prefix + 下标` 生成一个候选设备路径。
 *   prefix  : NULL ⇒ `IV_CELLPORT_PREFIX`（给单测换前缀用）
 *   idx     : 下标（0 起）
 * 返回 IV_OK；IV_EINVAL（out 为 NULL / cap 为 0）；IV_ERANGE（放不下，且已把
 * `out[0]` 置 '\0' —— 不返回半截路径，半截路径会被 `open()` 打开成别的东西）。 */
int iv_cellport_name(char *out, size_t cap, const char *prefix, unsigned idx);

/* 探测单个设备：打开 → 发 `AT\r` → 等 `OK` 终结行。
 *   baud    : 0 ⇒ `IV_CELLPORT_BAUD_DEFAULT`
 *   wait_ms : 0 ⇒ `IV_CELLPORT_WAIT_MS`
 * 返回 >0 = **fd（串口保持打开**，调用方负责注册事件与关闭）；
 *     负值 = 失败：'open' 的错误码原样透出（`IV_ENOENT` / `IV_EBUSY` / `IV_EAUTH`…），
 *            或 `IV_ETIMEDOUT`（打开了但没等到 `OK` ⇒ 这个口不是 AT 口 /
 *            正被别的会话占用）。**调用方必须区分这两种**：`IV_ETIMEDOUT`
 *            意味着"口存在但没有 AT"，是扫描里正常的跳过理由，不是故障。 */
int iv_cellport_probe(const char *dev, unsigned baud, unsigned wait_ms);

/* 扫描 `IV_CELLPORT_PREFIX` + `0..max_index-1`，返回**第一个**探测成功的 fd。
 *   path_out/cap : 成功时写入实际命中的路径（可为 NULL）；失败时写空串
 *   max_index    : 0 ⇒ `IV_CELLPORT_SCAN_MAX`
 * 返回 >0 = fd；负值 = 一个都没探到（无候选节点时为 `IV_ENOENT`）。
 * 节点不存在时用 `access()` 先跳过 —— 没插模组的板子上这条路径必须是**零耗时**，
 * 否则 5 s 一次的周期性重探会白白阻塞 Reactor 一秒钟。 */
int iv_cellport_scan(char *path_out, size_t cap, unsigned max_index, unsigned wait_ms);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_CELLPORT_H */
