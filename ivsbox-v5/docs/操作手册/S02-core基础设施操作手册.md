# S02 core 基础设施 — 详细操作手册

> 任务来源：`ivsbox-v5/docs/IVSBox-功能开发列表.md` §3-S02
> 前置状态：S01 已完成（2026-09-23 17:42，ARM 二进制已上板跑通）
> 平台基线：天嵌 TQT113（全志 T113-S4），Buildroot 2019.02.1 + glibc 2.25，SysV init
> 编制日期：2026-09-23 17:55　编制依据：开发列表 + `docs/bsp-capability.md` 回填结论 + AGENTS.md 规则 1~8
> 修订记录：2026-09-24（决策 D3 步骤 2 删除、决策 D4 线程模型与四条 Linux 原生小件、日志口径按板端实测校正、§3.1 口令改占位符）

---

## 1. S02 是什么

**一句话**：把后面所有任务（S03 事件循环、S04 串口、S06 链路、S13 配置……）都要用到的底层小件做好，并全部通过主机侧单元测试。

S01 解决的是"能编译、能上板"；S02 解决的是"地基材料"。它**不做任何业务功能**，全部是可以独立单测的小模块——这一步做扎实了，后面的任务就是在稳定的积木上搭东西。

### 1.1 七件事项与现状

开发列表 §3-S02 列了 7 项（原文编号 1~7），对应现状如下：

| # | 事项 | 目标目录 | 现状 | 本手册处理 |
|---|---|---|---|---|
| 1 | 日志 | `src/core/iv_log.c` | 已有骨架（stderr 输出） | 步骤 4：接 syslogd，轮转交给它自己 |
| 2 | 错误码 | `src/core/iv_err.c` | 已有（0=成功/负数=失败） | 步骤 5：仅核对，无改动 |
| 3 | 时钟 | `src/hal/iv_clock.c` | 已有（`ivs_clock_mono_ms`） | 步骤 5：仅核对 + 补单调性单测 |
| 4 | JSON | — | 无 | **决策 D1：不引入 cJSON**，见 §2.3 |
| 5 | CRC 校验 | `src/core/iv_crc.c`（新建） | **已实现（2026-09-24）**：zlib crc32 包装 | 步骤 1，**决策 D2**，见 §2.5 |
| 6 | 环形缓冲 | — | **本轮判定不做** | **决策 D3：从 S02 删除**，见 §2.6；原设计存档于附录 A |
| 7 | 配置读写 | `src/core/`（新建） | 无 | 步骤 3（含单实例 flock 归步骤 6） |

### 1.2 为什么这些排第二

- **日志**：后面每个任务的验收都靠"看日志"，没有日志等于盲调。
- **错误码 / 时钟**：开发列表 §6 纪律第 1、4 条的载体，所有新代码一开工就要用。
- **CRC**：S04 串口 HAL、S06 字节流解析状态机的直接依赖。至于**环形缓冲，本轮判定不做**——Linux 内核已经替你把"字节缓冲"这一层做完了，用户在用户态再造一个字节环只是多一次拷贝。完整论证见 §2.6，判据见 §2.7 的前置结论。
- **配置读写**：进程随时可能被杀（开发列表 §7 坑 14），"无状态启动 + 掉电安全写"是 Linux 设备与单片机的本质差异之一。

---

## 2. 平台事实与需求修正（依据 bsp-capability.md 回填结论）

开发列表编制于 2026-09-22，早于 M0 板端盘点。以下修正**已由板端实测确认**（见 `docs/bsp-capability.md` §2/§3.1/§7），动手前必须知道：

### 2.1 日志接 syslogd，不是 logd —— 且**轮转不用自己写**

- 开发列表与架构文档写的是 OpenWrt 体系的 logd；**板端实测无 logd/procd/ubus**，init 体系是 SysV + busybox `syslogd`（`/etc/init.d/S01syslogd`）。
- 因此 S02 日志后端接标准 `syslog(3)`（`LOG_LOCAL0`），板端用 `logread` 查看。

**板端实测补充（2026-09-24，BusyBox v1.33.2）**——这三条直接改变步骤 4 的工作量：

| 实测项 | 结果 | 对本任务的含义 |
|---|---|---|
| `/etc/init.d/S01syslogd` | `SYSLOGD_ARGS=""`，即全默认参数启动 `syslogd -n` | `/sbin/syslogd` 的其余参数都取默认值 |
| `syslogd --help` | `-s SIZE`（**单位 KB**，轮转阈值，默认 **200KB**，0=off）、`-b N`（保留份数，默认 **1**，最大 99）、`-O FILE`（默认 `/var/log/messages`） | **日志文件轮转由 syslogd 自己完成，S02 不要实现任何轮转逻辑** |
| `--help` 自述 + `/etc/syslog.conf` 实测不存在 | `this version of syslogd ignores /etc/syslog.conf` | **`LOG_LOCAL0` 不会分流到独立文件**：所有 facility 都写进同一个 `/var/log/messages`。`LOG_LOCAL0` 在本板只当**标签**用（便于 `grep`），不要据此以为会有单独日志文件 |

> **一个要记在心里的现场风险**：默认只保留 200KB × 2 个文件（`/var/log/messages` + `/var/log/messages.0`），故障日志很容易被冲掉。真要长期留痕，需改板端 `/etc/default/syslogd`（例如 `SYSLOGD_ARGS="-s 1024 -b 5"`）——这属板端配置，**放到 S13 或固件层处理，不塞进 S02**。

### 2.2 配置路径用 /opt/ivsbox/config/，不是 /data

- 架构文档 §10 写 `/data/ivsbox/config/*.json`；**板端实测根目录无 `/data`**。
- 路径映射结论（bsp-capability §3.1）：配置 → `/opt/ivsbox/config/`；大文件（媒体/数据库）→ `/mnt/UDISK/ivsbox/`（S13/S16 才用）。

### 2.3 决策 D1：JSON 用直接字符串解析，不引入 cJSON

| 项 | 内容 |
|---|---|
| 开发列表原文 | 选 cJSON（2 个文件进仓库），包一层 `iv_json_*` 适配层 |
| 用户硬性约束 | **避免使用 JSON 库进行解析，采用直接字符串解析方式**（2026-09-23 会话确认，晚于开发列表编制） |
| 本手册执行口径 | **不引入 cJSON**。配置文件采用 `key=value` 文本格式（见步骤 3），用逐行字符串解析 |
| 连带影响 | 开发列表验收项"JSON 往返"改为"**配置文件写入→读回往返**"；S02 收尾时同步更正开发列表 §3-S02 |

### 2.4 时钟纪律（缺口 G9）

- 板载时钟上电为 1970（bsp-capability §2.8），墙钟不可信 → **所有时长/超时/窗口一律 `ivs_clock_mono_ms()`**，此纪律从 S02 起执行。
- `CLOCK_MONOTONIC` 从内核启动起算，因此上电 1970 这件事**不影响**时长测量——这是该纪律成立的技术依据。

### 2.5 决策 D2：CRC 用系统 zlib 库，不手写多项式位运算（2026-09-24 用户指示）

| 项 | 内容 |
|---|---|
| 开发列表原文 | "CRC8 / CRC16：自己写，20 行" |
| 用户决策 | **使用现有 Linux 开发库计算**（2026-09-24，执行步骤 1 时指示） |
| 本手册执行口径 | CRC 统一走 **zlib `crc32()`**（Linux 用户态事实标准，glibc 无 CRC 函数）；CRC8/CRC16 属手写实现且现网帧实际 CRC 类型待 S06 确认，**确认前不预写**，S06 按需在本文件扩展 |
| 平台事实（2026-09-24 实测） | 板端 `/usr/lib/libz.so.1.2.11` 在位；交叉 sysroot `usr/include/zlib.h` + `usr/lib/libz.so` 在位；VM 主机 `zlib1g-dev` 已安装 |
| 连带影响 | 验收项"CRC 已知向量"改为 CRC-32 向量（`"123456789"` → `0xCBF43926`）；`ivcore` 在 UNIX 下链接 `ZLIB::ZLIB`（Windows 主机验证不编 `iv_crc`） |

### 2.6 决策 D3：环形缓冲（步骤 2）—— **从 S02 删除**（2026-09-24）

沿用决策 D2 的口径（"优先使用现有 Linux 开发库"）去查步骤 2，第一层结论是**没有库可用**；第二层结论更关键——**这一步本身问错了**，用户态不需要重造一个字节环。

| 项 | 内容 |
|---|---|
| 开发列表原文 | "环形缓冲：自己写，单生产者单消费者，用于串口收数据"；本手册步骤 2 原设计"SPSC 无锁 + 静态存储 + 容量 2 的幂" |
| 平台事实（2026-09-24 实测） | **交叉 sysroot 与板端都没有任何无锁/环形缓冲库**：liburcu、concurrencykit(`ck_ring`)、liblfds、DPDK(`rte_ring`)、libkfifo 全无；libubox 只有 `avl/blob/kvlist/list/runqueue/uloop/usock`；glib `GQueue`/`GAsyncQueue` 与 GStreamer `GstAdapter` 虽是**有锁 + 堆分配**，违反"静态存储、不动态 malloc"纪律。清单见 `bsp-capability.md` §7 |
| 公开候选体检 | `szanni/ringbuf`（单头文件、C11 原子、ISC 许可）形状最接近，但 `ringbuf_new()` **内部堆分配且无 caller 传缓冲 API**，且 2020 年后停更、自带并发测试自述偶发失败；Zephyr `spsc_lockfree.h` 是**编译期静态数组**、形状最对，但绑 Zephyr 原子原语（`z_spsc_in/out`），移植成本 ≥ 自研；`RomanHorshkov/SPSCring` 只存 `int` 且动态分配 → **三者均不采用**，见 `bsp-capability.md` §7.3 与 §7.5 |
| **结论 A：要不要做** | **"接收数据"这件事本身不需要环形缓冲。** 串口按架构 §7.4 纳入主 Reactor，`O_NONBLOCK` + epoll 模型下每轮 `read()` 到 `EAGAIN`、直接喂流式帧解析器（解析器自己攒半包）即可；**内核 tty 翻转缓冲已经充当了缓冲**，用户态再加一层环只是**多一次拷贝** |
| **结论 B：何时才需要** | 仅三种情况：① **UART 的读不在 Reactor 线程**（独立线程 / 厂商 SDK 回调线程投递）→ 这时才需要跨线程通道、也才需要原子操作；② 主循环可能被长时间阻塞，需"快吸慢解"防内核 tty 缓冲溢出（架构已有"有界慢任务池"专门防这个，属保险）；③ 需要保留最近 N KB 原始字节做现场留痕（"黑盒"），与"接收"无关 |
| **连带的护栏（比环更值钱）** | **环不能防丢，只把溢出点后移**。而板端 `/proc/tty/driver/uart`（sunxi 驱动）实测**只有 `tx:`/`rx:` 累计，没有 overrun / frame-error 计数** → 丢字节**无法从驱动层观测**。所以该先做的是应用层统计：`read()` 到的字节数、解析出帧数、半包等待数、CRC 错次数、重同步次数，并与 `/proc/tty/driver/uart` 的 `rx:` 累计比对以发现丢失。**该护栏本轮提为独立事项，见 §2.7 第 4 条** |
| **本轮处置（已确认）** | **删除**：本轮不写 `iv_ring.c`，步骤 2 从 S02 待办中移除，验收清单中"环形缓冲满/空边界"一项随之取消，`tests/unit/test_ring.c` 不建。原 API 与实现要点**降为附录 A 存档**，仅在 §2.7 的前置结论被打破时才回到那里 |

### 2.7 决策 D4：线程模型与四条 Linux 原生小件（2026-09-24 用户确认，本轮一并纳入 S02）

**前置结论（后续所有任务的硬约束）**：

> **串口的 `read()` 在主 Reactor 线程内执行。** 依据架构 §7.4"串口使用 `termios` raw 模式并纳入主 Reactor"。串口回调只做 `read()` 排空 + `ivs_frame_feed()`；任何可能阻塞的活（等应答、写文件、DNS）一律丢慢任务池，不占回调。

该结论的两个直接后果：

1. **不需要任何跨线程同步**——帧解析状态、统计计数都只被一个线程访问，**不用原子操作、不用锁**。这是步骤 2 被删除的根本依据。
2. **回调必须短**——`read()` 到 `EAGAIN` 立即返回，绝不在回调里 `sleep()` 或等 `write()` 完成。

**若 S04 实际打破了该结论**（出现独立读线程或厂商 SDK 回调线程），**必须先回到本节重新决策**：届时改用 `eventfd` 唤醒主 Reactor + 有界帧队列传数据，或采用附录 A 的自研 SPSC 环。**无论哪种，都不允许用 `volatile` 充当原子变量**——`volatile` 只阻止编译器缓存，**不是内存屏障**；在 ARMv7 上对齐的 32 位读写本身是原子的，所以 `volatile` 版本"通常能跑"，这正是最危险的地方。

**四条 Linux 原生小件**（本轮 S02 一并做，均不引入第三方依赖）：

| # | 小件 | 做法 | 落点 |
|---|---|---|---|
| 1 | **单实例锁** | `open()` 打开 `/var/run/ivsboxd.pid`（带 `O_CREAT` 与 `O_RDWR`），随后 `flock()` 取 `LOCK_EX` 加 `LOCK_NB`；拿到 `EWOULDBLOCK` 即"已有实例在跑"，打日志后退出。fd 故意不关，进程退出时内核自动释放 | 步骤 6，`main.c` 最开头 |
| 2 | **串口开设备后清残留** | 打开 `/dev/ttyS*` 后 `tcflush(fd, TCIOFLUSH)`，丢掉上电/上次运行残留的脏字节 | S04 落地（本手册先登记为约定） |
| 3 | **绝对时刻睡眠** | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL)`，**不用** `nanosleep`——相对睡眠被信号打断后会少睡，累积漂移 | S03 落地（本手册先登记为约定） |
| 4 | **收包对账统计** | 计数器：`read()` 到的字节数 / 出帧数 / 半包等待数 / CRC 错次数 / 重同步次数；定期与 `/proc/tty/driver/uart` 的 `rx:` 累计比对 | S04/S06 落地（本手册先登记为约定） |

- **第 1 条为什么提到 S02**：它是**后面所有调试的前提**。两个 `ivsboxd` 同时跑会抢 `/dev/ttyS*`，现场表现为"收帧随机丢、谁也复现不出"，是最难查的一类故障。10 行代码换掉这个风险，性价比极高。
- **第 4 条的一个待实测点**：先试标准 ioctl `TIOCGICOUNT`（返回 `struct serial_icounter_struct`，可拿到驱动级 `overrun` / `frame` / `parity` 计数）。已知 sunxi 驱动的 `/proc/tty/driver/uart` **只报 `tx:`/`rx:` 累计、没有错误计数**，但 `TIOCGICOUNT` 是否被该驱动填充 **待 S04 实测确认**；不填则退回纯应用层统计。
- **不引入清单**（明确排除，避免反复讨论）：libevent / libuv（与自研 Reactor 职责重叠）、cJSON（决策 D1）、任何无锁环形缓冲库（已体检，见 §2.6）。

---

## 3. 开工前准备

### 3.1 环境清单

| 角色 | 地址 / 位置 | 用途 |
|---|---|---|
| Windows 本机 | `f:\debug\T113_TCP` | 代码编辑、git 主副本、COM4 串口控制台 |
| 编译 VM | `ssh tqt113`（192.168.2.115，用户 `embedsky`） | 主机侧单测（`build/host`）+ 交叉编译（`build/arm`） |
| 开发板 | 192.168.2.105（用户 `root`，口令见现场登记，**不入库**） | scp 推送目标、冒烟验证 |
| 板端控制台 | Windows COM4，115200 | 运行程序、`logread` 查日志 |

> 口令纪律：按 AGENTS.md 规则 3 第 6 款，账号口令、密钥、设备序列号一律**不进仓库**。VM→板子已配置密钥免密（实测 `ssh -o BatchMode=yes root@192.168.2.105` 直接成功），因此**本手册全部命令都不需要写口令**；Windows 侧如需密码登录，用 PuTTY 的 `plink -pw`（见用户级记忆）。

VM 内仓库：`/home/embedsky/work/T113_S4`；工具链：`arm-linux-gnueabi-` gcc 7.3.1（天嵌官方，sysroot glibc 2.25 与板端一致）；VM cmake 3.22.1。

### 3.2 会话隔离（AGENTS.md 规则 8，Windows 本机执行）

> **本机 git-stint 0.6.1 已实测失效**（`git stint list/start` 报 `spawnSync git EBUSY` / `Not inside a git repository`，同目录 `git status` 正常）。改用等价的 git worktree 手工降级方案，物理隔离效果一致。完整说明见 `日常开发闭环流程.md` §9。

```sh
# 1) 对齐远端，确认干净（必须无 ahead/behind、无未提交改动）
git fetch origin
git status -sb
# 2) 开会话（WorkBuddy 用 wb- 前缀，TRAE 用 trae- 前缀）
git worktree add .stint/wb-s02linux -b stint/wb-s02linux
cd .stint/wb-s02linux      # 本次所有改动只在这个 worktree 内
# 3) 随时确认自己没有写到 main 工作副本
git worktree list
```

### 3.3 同步 VM 代码

```sh
# VM 内（ssh tqt113 后）
cd ~/work/T113_S4
git fetch origin && git checkout main && git pull --ff-only
git log --oneline -1    # 应与本机 main 一致
```

---

## 4. 实现步骤

> 约定：以下"新建文件"均相对 `ivsbox-v5/`；公共头一律放 `include/ivsbox/`（架构 §6 依赖规则：模块只经公共头互访）。
> 编码纪律（开发列表 §6）：所有系统调用判断返回值 + `errno` 进日志；`EINTR` 重试不当错误；日志文本一律 ASCII。

### 步骤 1：CRC 校验（`src/core/iv_crc.c` + `include/ivsbox/iv_crc.h`）——已按决策 D2 执行

**实现方式（2026-09-24 实际落地）**：薄包装 zlib `crc32()`，不自写多项式位运算。

**API（已实现）**

```c
/* CRC-32（zlib 口径 = CRC-32/ISO-HDLC，与 zip/以太网 FCS 一致）。
 * 支持分段累计：首次 crc 传 0，后续传回上次返回值（S06 半包场景）。 */
uint32_t ivs_crc32(uint32_t crc, const uint8_t *data, size_t len);
```

**要点**

- `iv_crc.c` 仅 1 个函数，直接转调 `zlib crc32(crc, data, (uInt)len)`；算法正确性由 zlib 保证。
- CRC8/CRC16-Modbus **不预写**：现网帧实际 CRC 类型待 S06 对帧格式确认（开发列表 §3-S06 兼容纪律），确认后再按需扩展。
- CMake：仅 `UNIX` 下 `find_package(ZLIB REQUIRED)` 并给 `ivcore` 挂 `ZLIB::ZLIB`（PUBLIC 传递给测试与上层）；Windows(MinGW) 主机验证脚本无 zlib，不编 `iv_crc.c`。
- 单测向量（`tests/unit/test_crc.c`，CRC-32/ISO-HDLC 公开检验值）：`""` → `0x00000000`、`"A"` → `0xD3D99E8B`、`"123456789"` → `0xCBF43926`，另验证分段累计 == 一次性计算。

### 步骤 2：SPSC 环形缓冲 —— **已从 S02 删除**（决策 D3）

> **状态（2026-09-24，用户确认）：删除。** 本轮**不写** `src/core/iv_ring.c`、不建 `tests/unit/test_ring.c`，验收清单中该项取消。
>
> 理由浓缩成一句：**串口按架构 §7.4 纳入主 Reactor 线程（见 §2.7 前置结论），`read()` 到 `EAGAIN` 直接喂流式帧解析器即可，内核 tty 缓冲已经是缓冲，用户态再加环只多一次拷贝、多一层原子操作。**
>
> 完整论证（含库可用性实测、公开候选否决理由、何种情况才需要环）见 **§2.6 决策 D3**。
>
> 原 API 草图与实现要点**已移入附录 A 存档**，仅在 §2.7 的前置结论被打破（UART 读跨线程）时启用；届时必须把 `volatile` 换成 C11 `<stdatomic.h>` 的 acquire/release 语义。

**删除后接收路径长什么样**（这就是替代方案的全部，无缓冲代码）：

```c
/* 主 Reactor 线程内的串口可读回调 */
uint8_t buf[4096];              /* 栈上，容量固定，容量上限纪律 */
for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
        ivs_frame_feed(&parser, buf, (size_t)n, on_frame, ud);
    } else if (n < 0 && errno == EAGAIN) {
        break;                  /* 已排空，返回事件循环 */
    } else if (n < 0 && errno == EINTR) {
        continue;               /* 被信号打断不是错误 */
    } else {
        IVS_LOGE("link", "read failed: %s", ivs_strerror(-errno));
        break;
    }
}
```

半包由 `ivs_frame_feed()` 内部自己攒（S06 实现），粘包由它自己切，这里不需要任何缓存结构。

### 步骤 3：配置读写（`src/core/iv_config.c` + `include/ivsbox/iv_config.h`）

**文件格式**（`/opt/ivsbox/config/ivsbox.conf`，ASCII 文本）

```text
# IVSBox config (v1)
log.level = info
link.baudrate = 115200
probe.interval_ms = 5000
```

**API 草图**

```c
typedef struct { char key[32]; char val[64]; } ivs_config_kv_t;

/* 加载：逐行解析 key=value；非法行/未知键/超范围 → 返回错误码并让调用方回退默认值 */
int ivs_config_load(const char *path, ivs_config_kv_t *kv, size_t max_kv, size_t *out_n);
/* 保存：tmp → fsync → rename → fsync(目录)，掉电安全（开发列表 §7 坑 13） */
int ivs_config_save(const char *path, const ivs_config_kv_t *kv, size_t n);
```

**原子写序列（本任务的核心考点，缺一步掉电就得到半截文件）**

```c
/* 1. 目录存在性：mkdir(path_dir, 0755)，EEXIST 不算错 */
/* 2. 写 <final>.tmp：open(O_WRONLY|O_CREAT|O_TRUNC, 0644)
      → 循环 write 处理部分写与 EINTR → fsync(fd) → close */
/* 3. rename("<final>.tmp", <final>)      同一文件系统内原子替换 */
/* 4. open(<dir>, O_RDONLY|O_DIRECTORY) → fsync(dirfd) → close   （fsync 目录才保证 rename 落盘） */
```

**实现要点**

- 解析规则（直接字符串解析，遵守决策 D1）：`#` 起注释行跳过；空行跳过；行内第一个 `=` 分隔键值，两侧空白去除；键长/值长超限、重复键、空键 → 拒绝（返回 `IVS_ERR_INVAL`）。
- **读取用 `getline()`，不要手写 `fgets` + 续行判断**（§2.7 类 Linux 原生做法）：`getline` 按需自动扩容，长行不会截断，glibc 2.25 已提供（POSIX 2008）。用完 `free(line)` 即可，属函数内部临时分配，不违反"业务数据静态存储"纪律。
- **写入用 `open_memstream()` 拼接，不要手算 `snprintf` 缓冲区长度**：它给你一个随写随长的内存流，末尾 `fflush` 后取 `buf`/`size` 一次性写文件，不会因为长度估错而截断。
- 加载失败的策略：**拒绝非法配置，回退默认值继续运行并 `IVS_LOGW` 告警**（"非法配置被拒绝"验收项；S13 再升级为完整事务）。
- **不要在 `save()` 里静默修正非法值**：现场更需要看到"我写的值被拒绝了"，静默改成默认值会让故障排查失去线索。
- `.tmp` 与目标文件必须同目录（保证同一文件系统，`rename` 才原子）。
- 路径宏集中定义：`#define IVS_CONFIG_DIR "/opt/ivsbox/config"`（勿散落硬编码）。

### 步骤 4：日志接 syslogd（改 `src/core/iv_log.c`，不动 `iv_log.h` 对外接口）

**CMake 开关**（根 `CMakeLists.txt`，`ivcore` 定义之后）

```cmake
option(IVSBOX_LOG_SYSLOG "日志输出到 syslog（板端 ON，主机默认 OFF 走 stderr）" OFF)
if(IVSBOX_LOG_SYSLOG)
    target_compile_definitions(ivcore PUBLIC IVSBOX_LOG_SYSLOG)
endif()
```

**实现要点**

```c
#ifdef IVSBOX_LOG_SYSLOG
#include <syslog.h>
/* ivs_log_init()：openlog("ivsboxd", LOG_PID|LOG_NDELAY, LOG_LOCAL0); */
/* 级别映射：ERROR→LOG_ERR  WARN→LOG_WARNING  INFO→LOG_INFO  DEBUG→LOG_DEBUG */
#endif
```

- 板端 ARM 构建加 `-DIVSBOX_LOG_SYSLOG=ON`；主机单测保持 stderr（`logread` 是板端专属）。
- **轮转不要自己实现**（§2.1 板端实测）：busybox syslogd 按 `-s`/`-b` 自动轮转，默认 200KB × 2 个文件。`iv_log.c` 只负责"把一条日志交给 syslog"，不碰文件。
- **`LOG_LOCAL0` 只是标签，不是分流开关**（§2.1 实测：该版本 syslogd 忽略 `/etc/syslog.conf`，所有 facility 都写 `/var/log/messages`）。所以：
  - 不要写"配置 `syslog.conf` 把 ivsbox 日志单独落一个文件"这类步骤——本板做不到；
  - 板端筛选统一用 `logread | grep ivsbox`（`openlog` 的 ident 已提供稳定筛选锚点）。
- **ASCII 纪律**：日志文本一律 ASCII，中文只写注释（板端控制台 GBK）。
- `iv_log.h` 注释里"由 logd 汇聚"的表述顺带更正为 syslogd（步骤 4 一起改）。

### 步骤 5：已有三项核对（不改行为）

- `iv_err.h`：确认 `0=成功/负数=失败` 约定与 `ivs_strerror()` 可用（G12 字典冻结仍留 M1，本步骤不动）。该约定与 Linux 的 `-errno` 惯例一致，**不要另造一套错误码**，直接复用 `errno` 取值。
- `iv_clock.h/.c`：确认 `ivs_clock_mono_ms()` 单调不回退（单测见 §5-④）。

### 步骤 6：main 接线（让板端冒烟真正覆盖 S02 代码）

`src/app/main.c` 在版本打印**之前**加单实例锁（§2.7 第 1 条）：

```c
/* S02: single instance guard (Linux native, ~10 lines) */
int g_lock_fd = -1;
g_lock_fd = open("/var/run/ivsboxd.pid", O_CREAT | O_RDWR, 0644);
if (g_lock_fd < 0) {
    IVS_LOGE("main", "open pidfile failed: %s", ivs_strerror(-errno));
    return 1;
}
if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
    IVS_LOGE("main", "another instance is running, exit");
    return 1;
}
/* 注意：不 close(g_lock_fd)，进程退出时锁由内核自动释放 */
```

在版本打印之后追加配置加载：

```c
/* S02 smoke: config load (fallback to defaults on error) + syslog line */
```

- 调 `ivs_config_load()`：失败则打 `IVS_LOGW("cfg", "load failed, using defaults: %s", ivs_strerror(rc))`；
- 成功则 `IVS_LOGI("cfg", "config loaded, entries=%u", n)`；
- 目的：板端跑一次后 `logread` 能看到 ivsboxd 的日志（验证 syslog 后端）、`/opt/ivsbox/config/` 有产物（验证配置模块），S02 不再是"只在主机绿"的代码。

> 注意：`flock` 需要 `#include <sys/file.h>`，`open` 需要 `<fcntl.h>`；ARM 目标已由根 `CMakeLists.txt` 统一启用 `_GNU_SOURCE`（S01 修复），不会报隐式声明。

---

## 5. 单元测试（对应开发列表验收项：原 5 项 → 本轮 4 项）

新建 `tests/unit/` 下 3 个文件（原"JSON 往返"项按决策 D1 改为配置往返；原"环形缓冲满/空边界"项按决策 D3 取消）：

| 测试 | 文件 | 验收点 |
|---|---|---|
| ① CRC 已知向量 | `test_crc.c`（**已实现**） | CRC-32/ISO-HDLC 向量：`""`→0、`"A"`→0xD3D99E8B、`"123456789"`→0xCBF43926；分段累计==一次性（决策 D2：zlib crc32） |
| ~~② 环形缓冲边界~~ | ~~`test_ring.c`~~ | **已删除**（决策 D3：不做环形缓冲，无此项） |
| ③ 配置往返 | `test_config.c` | save→load 内容一致（含注释行、空白容错） |
| ④ 非法配置拒绝 | （并入 `test_config.c`） | 未知键/超长键/重复键/空键 → `IVS_ERR_INVAL`；坏文件加载后调用方拿到默认值 |
| ⑤ 时钟单调性 | `test_clock.c` | 连续采样 100 次，`t[i+1] >= t[i]` 且间隔 < 100ms |

`tests/CMakeLists.txt` 追加：

```cmake
foreach(t test_crc test_config test_clock)
    add_executable(${t} unit/${t}.c)
    target_link_libraries(${t} PRIVATE ivcore ivhal)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
```

---

## 6. 主机侧验证（VM 内，S02 的主要验收场）

```sh
cd ~/work/T113_S4
# 把 Windows 会话 worktree 的改动推进 VM（本机 push 会话分支后 VM fetch，或 scp 改动文件）
cmake -S ivsbox-v5 -B ivsbox-v5/build/host -DIVSBOX_BUILD_TESTS=ON
cmake --build ivsbox-v5/build/host -j
ctest --test-dir ivsbox-v5/build/host --output-on-failure   # 全绿（本任务新增 3 个 + 既有用例）
```

注意：`_GNU_SOURCE` 已由根 `CMakeLists.txt` 对 UNIX 目标统一启用（S01 修复），新文件直接用 `fsync`/`rename`/`getline`/`open_memstream` 等不再报隐式声明；若新增文件报同类错，先检查是否被加进了正确 target。

---

## 7. 交叉编译与板端冒烟（COM4 串口 + scp）

```sh
# VM 内：ARM 构建（板端不构建测试）
cmake -S ivsbox-v5 -B ivsbox-v5/build/arm \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/ivsbox-v5/cmake/toolchains/tina-arm.cmake \
  -DIVSBOX_BUILD_TESTS=OFF -DIVSBOX_LOG_SYSLOG=ON -DCMAKE_BUILD_TYPE=Release
cmake --build ivsbox-v5/build/arm -j
file ivsbox-v5/build/arm/ivsboxd        # 确认 ARM ELF
scp ivsbox-v5/build/arm/ivsboxd root@192.168.2.105:/tmp/ivsboxd
```

板端（Windows COM4，用 `serial-console-debug` skill 的 `serial_run.py`，实际调用为位置参数
`serial_run.py COM4 115200 cmds.txt out.txt`（命令一行一条写进 `cmds.txt`）；
完整用法见 `docs/操作手册/日常开发闭环流程.md` §7）：

```sh
chmod +x /tmp/ivsboxd
/tmp/ivsboxd; echo RUN_EXIT=$?          # 期望：版本输出 + cfg 日志行，RUN_EXIT=0
/tmp/ivsboxd; echo SECOND_EXIT=$?       # 期望：SECOND_EXIT=1 且日志提示已有实例（验证单实例锁）
logread | grep ivsbox                    # 期望：能看到 ivsboxd 的 LOG_INFO/LOGW 行（syslog 后端生效）
ls -l /var/log/messages*                 # 期望：messages 与 messages.0 在位（轮转由 syslogd 负责）
ls -l /opt/ivsbox/config/                # 期望：ivsbox.conf 已生成（main 接线成功）
cat /opt/ivsbox/config/ivsbox.conf
```

冒烟通过后清理板端残留配置（`rm -rf /opt/ivsbox`，避免与 S13 联调时旧文件混淆），`/tmp/ivsboxd` 保留。

---

## 8. 收尾固定动作（规则 1/2/4/6/8）

1. **规则 1**：`docs/IVSBox-功能开发列表.md` §10 实现记录，S02 行改为"已实现 + 完成时间 + 说明"；§3-S02 原文按决策 D1/D3 更正（JSON→`key=value` 直接字符串解析、配置路径 `/opt/ivsbox/config/`、日志→syslogd、环形缓冲→删除）。
2. **规则 2**：`docs/修改记录.md` 末尾追加一条（修改文件清单 / 问题目的 / 处理方式 / 验证结果），追加前先重读文件最新内容。
3. **规则 4**：有代码产出时 `docs/代码说明.md` 追加一节（干了什么事 / 解决什么问题 / 影响范围 / 验证方式与结果）；纯文档修改不写。
4. **规则 6**：清理临时物（板端配置目录、VM 侧一次性脚本等），`git status` 自检无任务外未跟踪文件。
5. **规则 8 收尾顺序（本机 git-stint 失效，用等价 worktree 方案；详见 `日常开发闭环流程.md` §9）**：
   ```sh
   cd .stint/wb-s02          # 在会话 worktree 内提交
   git add <本次改动的文件...>   # 显式列文件，禁止 git add -A
   git commit -m "feat(core): S02 基础设施（CRC/配置原子写/syslog/单实例锁）"
   cd ../..                   # 回 main 工作副本
   git merge --ff-only stint/wb-s02
   git worktree remove .stint/wb-s02
   git branch -d stint/wb-s02
   git push
   git worktree list          # 应只剩主工作副本；.stint/ 下无残留
   ```
   若与另一 AI 会话在共用文档（修改记录/代码说明/AGENTS.md）冲突：**双方记录都保留、按时间倒序**，绝不丢弃对方条目。

---

## 9. 验收清单（对照开发列表 §3-S02 + 本手册修正）

- [x] CRC 已知向量测试通过（①，zlib crc32 四条向量全过，决策 D2）
- [x] 环形缓冲项**已删除**（决策 D3：不做环形缓冲，无此项；原测试②随之取消，见 §2.6）
- [ ] 配置往返测试通过（③，替代原 JSON 往返，决策 D1）
- [ ] 非法配置被拒绝、回退默认值测试通过（④）
- [ ] 时钟单调性测试通过（⑤）
- [ ] `ctest` 全绿（主机侧，VM）
- [ ] ARM 构建通过 + 板端 `RUN_EXIT=0`
- [ ] 板端重复启动被单实例锁拒绝（`SECOND_EXIT=1`，§2.7 第 1 条）
- [ ] 板端 `logread | grep ivsbox` 能看到日志（syslog 后端生效）
- [ ] 板端 `/opt/ivsbox/config/ivsbox.conf` 生成成功
- [ ] 开发列表 / 修改记录 / 代码说明 均已回写并推送 GitHub（`https://github.com/ppp1234lll/T113_S4` main）
- [ ] 会话 worktree 已 merge + 清理，`.stint/` 无残留

## 10. 常见坑速查（S02 相关）

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 忘了 fsync 目录 | rename 本身没落盘，掉电后新旧文件都丢 | 步骤 3 第 4 步不可省 |
| `.tmp` 放别的目录 | rename 跨文件系统 → `EXDEV`，非原子 | tmp 与目标同目录 |
| 不处理 `EINTR` / 部分写 | 偶发写失败/半截文件 | 循环 write + EINTR 重试 |
| 自己实现日志轮转 | 白写一堆代码，还可能和 syslogd 抢同一个文件 | 交给 syslogd（`-s`/`-b` 自动轮转，§2.1） |
| 以为 `LOG_LOCAL0` 会分流到独立文件 | 板端该版本 syslogd 忽略 `/etc/syslog.conf`，找不到预期文件 | 全部日志在 `/var/log/messages`，用 `grep` 筛 |
| 以为日志会长期保留 | 默认 200KB × 2 个文件，故障证据被冲掉 | 需要长期留痕改板端 `/etc/default/syslogd`（S13/固件层） |
| 没做单实例锁 | 两个 `ivsboxd` 抢 `/dev/ttyS*`，收帧随机丢、无法复现 | 步骤 6 的 `flock` 单实例 |
| 用 `nanosleep` 做定时 | 被信号打断后少睡，长跑累积漂移 | `clock_nanosleep` + `TIMER_ABSTIME`（§2.7 第 3 条） |
| 手算 `snprintf` 缓冲长度 | 估短了截断，估长了浪费 | `open_memstream()` 拼配置（步骤 3） |
| 若将来做环：取模用 `%` 或动态 malloc | 每字节一次除法；违反上限纪律 | 附录 A：cap 取 2 的幂 + 调用方传静态存储 |
| 若将来做环：用 `volatile` 当原子 | 只挡编译器缓存、无内存序保证，换架构/换优化等级即坏 | 附录 A：C11 `<stdatomic.h>` acquire/release |
| 日志里打中文 | 板端 GBK 控制台乱码，故障日志不可用 | 日志 ASCII，中文进注释 |
| 用 `gettimeofday` 算时长 | 对时后时长变负/巨大 | 一律 `ivs_clock_mono_ms()` |
| 板端用 `/data` 路径 | 目录不存在，open 直接失败 | `/opt/ivsbox/config/` |

## 11. 实现记录

| 功能 | 状态 | 完成时间 | 说明 |
|---|---|---|---|
| 本操作手册编制 | 已实现 | 2026-09-23 17:55 | 依据开发列表 §3-S02 与 bsp-capability 回填结论编制，含决策 D1（不引入 cJSON） |
| S02 步骤 1：CRC 校验（zlib crc32 包装，决策 D2） | 已实现 | 2026-09-24 10:07 | 用户指示改用现有库：`ivs_crc32` 薄包装 zlib crc32；VM 主机 ctest 2/2 全绿（含 4 条 CRC-32 向量），ARM 交叉编译通过，板端链接 libz 运行 `RUN_EXIT=0`（ivsboxd 20828B）；CRC8/CRC16 待 S06 确认帧格式后再定 |
| S02 步骤 2：环形缓冲选型体检（决策 D3） | 已实现（结论：暂缓实现） | 2026-09-24 11:05 | 实测交叉 sysroot 与板端均无可用环形缓冲库；公开候选 szanni/ringbuf、Zephyr spsc_lockfree、SPSCring 逐个体检后均不采用；进一步论证"主 Reactor 模型下接收数据不需要环"。步骤 2 暂缓，转为前置确认线程模型；平台清单见 `bsp-capability.md` §7，决策见 §2.6 |
| S02 手册修订：步骤 2 删除 + 决策 D4 + 日志口径校正 | 已实现 | 2026-09-24 12:20 | 用户确认三项决策：① 步骤 2 **从 S02 删除**（原设计移入附录 A 存档）；② 四条 Linux 原生小件（单实例 `flock`、`tcflush`、`clock_nanosleep`、收包对账统计）纳入本轮 S02；③ 线程模型定为主 Reactor 线程（§2.7 前置结论）。另按板端实测校正日志口径（syslogd 自带轮转、`syslog.conf` 被忽略）、步骤 3 改用 `getline`/`open_memstream`、§3.1 明端口令改占位符（规则 3.6） |
| S02 core 基础设施（整体） | 未实现 | — | 步骤 1 完成；步骤 3（配置读写）、4（syslog）、5（核对）、6（main 接线 + 单实例锁）待执行；步骤 2 已删除 |

---

## 附录 A：SPSC 环形缓冲原设计（存档）

> **状态：已从 S02 待办中删除**（决策 D3，2026-09-24 用户确认）。本附录仅为存档，不属于本轮实施范围。
>
> **启用条件（唯一）**：§2.7 的前置结论被打破，即确认串口 `read()` 确实运行在**独立线程或厂商 SDK 回调线程**中，需要向主 Reactor 跨线程投递字节。其他任何理由（"可能有用""以后也许要"）都不构成启用条件。
>
> **启用时必做的两处修正**：
> 1. `volatile size_t head/tail` **必须**换成 C11 `<stdatomic.h>` 的 `_Atomic size_t` + `memory_order_acquire`/`memory_order_release`。`volatile` 只阻止编译器缓存，**不是内存屏障**；在 ARMv7 上对齐的 32 位读写本身是原子的，所以 `volatile` 版"通常能跑"——这正是它最危险的地方。
> 2. 补满/空/回绕/非 2 的幂拒绝的单测（即原测试②），覆盖判据的每一侧边界。

**API 草图**

```c
typedef struct {
    uint8_t        *buf;
    size_t          cap;    /* 容量，必须为 2 的幂 */
    volatile size_t head;   /* 仅生产者写 —— 见上方修正 1 */
    volatile size_t tail;   /* 仅消费者写 —— 见上方修正 1 */
} ivs_ring_t;

int    ivs_ring_init(ivs_ring_t *r, uint8_t *storage, size_t cap);
size_t ivs_ring_write(ivs_ring_t *r, const uint8_t *data, size_t len); /* 返回实际写入，满则部分写 */
size_t ivs_ring_read (ivs_ring_t *r, uint8_t *out, size_t len);
size_t ivs_ring_used (const ivs_ring_t *r);
```

**实现要点**

- **单生产者单消费者无锁**：head 只有写者碰、tail 只有读者碰，用 `head - tail` 判占用即可，无需锁（S04 串口一收一发场景刚好匹配）。
- `cap` 强制 2 的幂（`init` 里校验，非 2 的幂返回 `IVS_ERR_INVAL`），索引用 `& (cap-1)` 代替取模。
- **满/空判据**：`head==tail` 为空；`head-tail==cap` 为满（牺牲 1 格的方案亦可，但判据必须单测覆盖）。
- 遵守上限纪律：容量由调用方传入静态存储（如 `static uint8_t buf[1024]`），**不动态 malloc**。
