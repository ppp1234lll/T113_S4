# S02 core 基础设施 — 详细操作手册

> 任务来源：`ivsbox-v5/docs/IVSBox-功能开发列表.md` §3-S02
> 前置状态：S01 已完成（2026-09-23 17:42，ARM 二进制已上板跑通）
> 平台基线：天嵌 TQT113（全志 T113-S4），Buildroot 2019.02.1 + glibc 2.25，SysV init
> 编制日期：2026-09-23 17:55　编制依据：开发列表 + `docs/bsp-capability.md` 回填结论 + AGENTS.md 规则 1~8

---

## 1. S02 是什么

**一句话**：把后面所有任务（S03 事件循环、S04 串口、S06 链路、S13 配置……）都要用到的底层小件做好，并全部通过主机侧单元测试。

S01 解决的是"能编译、能上板"；S02 解决的是"地基材料"。它**不做任何业务功能**，全部是可以独立单测的小模块——这一步做扎实了，后面的任务就是在稳定的积木上搭东西。

### 1.1 六件小事与现状

开发列表 §3-S02 列了 7 项（原文编号 1~7），对应现状如下：

| # | 事项 | 目标目录 | 现状 | 本手册处理 |
|---|---|---|---|---|
| 1 | 日志 | `src/core/iv_log.c` | 已有骨架（stderr 输出） | 步骤 4：接 syslogd |
| 2 | 错误码 | `src/core/iv_err.c` | 已有（0=成功/负数=失败） | 步骤 5：仅核对，无改动 |
| 3 | 时钟 | `src/hal/iv_clock.c` | 已有（`ivs_clock_mono_ms`） | 步骤 5：仅核对 + 补单调性单测 |
| 4 | JSON | — | 无 | **决策 D1：不引入 cJSON**，见 §2.3 |
| 5 | CRC8 / CRC16 | `src/core/`（新建） | 无 | 步骤 1 |
| 6 | 环形缓冲 | `src/core/`（新建） | 无 | 步骤 2 |
| 7 | 配置读写 | `src/core/`（新建） | 无 | 步骤 3 |

### 1.2 为什么这七样排第二

- **日志**：后面每个任务的验收都靠"看日志"，没有日志等于盲调。
- **错误码 / 时钟**：开发列表 §6 纪律第 1、4 条的载体，所有新代码一开工就要用。
- **CRC / 环形缓冲**：S04 串口 HAL、S06 字节流解析状态机的直接依赖；这两样在单片机时代写过，经验可直接迁移。
- **配置读写**：进程随时可能被杀（开发列表 §7 坑 14），"无状态启动 + 掉电安全写"是 Linux 设备与单片机的本质差异之一。

---

## 2. 平台事实与需求修正（依据 bsp-capability.md 回填结论）

开发列表编制于 2026-09-22，早于 M0 板端盘点。以下三处修正**已由板端实测确认**（见 `docs/bsp-capability.md` §2/§3.1），动手前必须知道：

### 2.1 日志接 syslogd，不是 logd

- 开发列表与架构文档写的是 OpenWrt 体系的 logd；**板端实测无 logd/procd/ubus**，init 体系是 SysV + busybox `syslogd`（`/etc/init.d/S01syslogd`）。
- 因此 S02 日志后端接标准 `syslog(3)`（`LOG_LOCAL0`），板端用 `logread` 查看。

### 2.2 配置路径用 /opt/ivsbox/config/，不是 /data

- 架构文档 §10 写 `/data/ivsbox/config/*.json`；**板端实测根目录无 `/data`**。
- 路径映射结论（bsp-capability §3.1）：配置 → `/opt/ivsbox/config/`；大文件（媒体/数据库）→ `/mnt/UDISK/ivsbox/`（S13/S16 才用）。

### 2.3 决策 D1：JSON 用直接字符串解析，不引入 cJSON

| 项 | 内容 |
|---|---|
| 开发列表原文 | 选 cJSON（2 个文件进仓库），包一层 `iv_json_*` 适配层 |
| 用户硬性约束 | **避免使用 JSON 库进行解析，采用直接字符串解析方式**（2026-09-23 会话确认，晚于开发列表编制） |
| 本手册执行口径 | **不引入 cJSON**。配置文件采用 `key=value` 文本格式（见步骤 3），用逐行字符串解析 |
| 连带影响 | 开发列表验收项"JSON 往返"改为"**配置写入→读回往返**"；S02 收尾时同步更正开发列表 §3-S02 |

### 2.4 时钟纪律（缺口 G9）

- 板载时钟上电为 1970（bsp-capability §2.8），墙钟不可信 → **所有时长/超时/窗口一律 `ivs_clock_mono_ms()`**，此纪律从 S02 起执行。

---

## 3. 开工前准备

### 3.1 环境清单

| 角色 | 地址 / 位置 | 用途 |
|---|---|---|
| Windows 本机 | `f:\debug\T113_TCP` | 代码编辑、git 主副本、COM4 串口控制台 |
| 编译 VM | `ssh tqt113`（192.168.2.115，embedsky） | 主机侧单测（`build/host`）+ 交叉编译（`build/arm`） |
| 开发板 | 192.168.2.105（root / 123456） | scp 推送目标、冒烟验证 |
| 板端控制台 | Windows COM4，115200 | 运行程序、`logread` 查日志 |

VM 内仓库：`/home/embedsky/work/T113_S4`；工具链：`arm-linux-gnueabi-` gcc 7.3.1（天嵌官方，sysroot glibc 2.25 与板端一致）；VM cmake 3.22.1。

### 3.2 会话隔离（AGENTS.md 规则 8，Windows 本机执行）

```powershell
# 1) 对齐远端，确认干净
git fetch origin
git status -sb          # 必须是 ## main...origin/main 且无未提交改动
# 2) 开会话
git stint start trae-s02
cd .stint/trae-s02      # 之后所有改动只在这个 worktree 内
# 3) 每改一个文件登记一次
git stint track <文件...> --session trae-s02
# 4) 随时查重叠（另一 AI 会话是否改了同一文件）
git stint conflicts --session trae-s02
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

### 步骤 1：CRC8 / CRC16（`src/core/iv_crc.c` + `include/ivsbox/iv_crc.h`）

**API 草图**

```c
/* CRC-8：poly=0x07，init=0x00，不反射（SMBus/ATM 风格） */
uint8_t  ivs_crc8(const uint8_t *data, size_t len);
/* CRC-16/Modbus：poly=0x8005 反射，init=0xFFFF，输出字节序待 S06 按现网帧确认 */
uint16_t ivs_crc16_modbus(const uint8_t *data, size_t len);
```

**实现要点**

- 逐位算法即可（查表法是优化，20 行内先跑对；协议实际跑在 220MB RAM 的 A7 上，查表可后补）。
- **现网帧用哪种 CRC、字节序如何，属 S06 对帧格式的确认项，本步骤不拍板**（标注"待确认"）；先把两个常用变体做对并向量验证，S06 需要变体时在此文件扩展。
- 已知向量（单测硬编码）：`"123456789"`（9 字节 ASCII）→ CRC-8 = `0xF4`；CRC-16/Modbus = `0x4B37`。

**坑**：CRC-16/Modbus 是反射算法，"左移实现 + 结果反转"与"右移实现"结果必须一致，用向量验证即可发现写错。

### 步骤 2：SPSC 环形缓冲（`src/core/iv_ring.c` + `include/ivsbox/iv_ring.h`）

**API 草图**

```c
typedef struct {
    uint8_t        *buf;
    size_t          cap;    /* 容量，必须为 2 的幂 */
    volatile size_t head;   /* 仅生产者写 */
    volatile size_t tail;   /* 仅消费者写 */
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
- 加载失败的策略：**拒绝非法配置，回退默认值继续运行并 `IVS_LOGW` 告警**（"非法配置被拒绝"验收项；S13 再升级为完整事务）。
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
- **ASCII 纪律**：日志文本一律 ASCII，中文只写注释（板端控制台 GBK）。
- `iv_log.h` 注释里"由 logd 汇聚"的表述顺带更正为 syslogd（步骤 4 一起改）。

### 步骤 5：已有三项核对（不改行为）

- `iv_err.h`：确认 `0=成功/负数=失败` 约定与 `ivs_strerror()` 可用（G12 字典冻结仍留 M1，本步骤不动）。
- `iv_clock.h/.c`：确认 `ivs_clock_mono_ms()` 单调不回退（单测见 §5-⑤）。

### 步骤 6：main 接线（让板端冒烟真正覆盖 S02 代码，约 10 行）

`src/app/main.c` 在版本打印之后追加：

```c
/* S02 smoke: config load (fallback to defaults on error) + syslog line */
```

- 调 `ivs_config_load()`：失败则打 `IVS_LOGW("cfg", "load failed, using defaults: %s", ivs_strerror(rc))`；
- 成功则 `IVS_LOGI("cfg", "config loaded, entries=%u", n)`；
- 目的：板端跑一次后 `logread` 能看到 ivsboxd 的日志（验证 syslog 后端）、`/opt/ivsbox/config/` 有产物（验证配置模块），S02 不再是"只在主机绿"的代码。

---

## 5. 单元测试（对应开发列表 5 项验收）

新建 `tests/unit/` 下 4 个文件（原"JSON 往返"项按决策 D1 改为配置往返）：

| 测试 | 文件 | 验收点 |
|---|---|---|
| ① CRC 已知向量 | `test_crc.c` | `"123456789"` → CRC-8=0xF4、CRC-16/Modbus=0x4B37；空串、单字节 |
| ② 环形缓冲边界 | `test_ring.c` | 空 read=0、满 write 拒绝、恰好写满、写读交替回绕、cap 非 2 幂被拒 |
| ③ 配置往返 | `test_config.c` | save→load 内容一致（含注释行、空白容错） |
| ④ 非法配置拒绝 | （并入 `test_config.c`） | 未知键/超长键/重复键/空键 → `IVS_ERR_INVAL`；坏文件加载后调用方拿到默认值 |
| ⑤ 时钟单调性 | `test_clock.c` | 连续采样 100 次，`t[i+1] >= t[i]` 且间隔 < 100ms |

`tests/CMakeLists.txt` 追加：

```cmake
foreach(t test_crc test_ring test_config test_clock)
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
ctest --test-dir ivsbox-v5/build/host --output-on-failure   # 5 个测试全过（含原 ipc_hdr）
```

注意：`_GNU_SOURCE` 已由根 `CMakeLists.txt` 对 UNIX 目标统一启用（S01 修复），新文件直接用 `fsync`/`rename` 等不再报隐式声明；若新增文件报同类错，先检查是否被加进了正确 target。

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
logread | grep ivsbox                    # 期望：能看到 ivsboxd 的 LOG_INFO/LOGW 行（syslog 后端生效）
ls -l /opt/ivsbox/config/                # 期望：ivsbox.conf 已生成（main 接线成功）
cat /opt/ivsbox/config/ivsbox.conf
```

冒烟通过后清理板端残留配置（`rm -rf /opt/ivsbox`，避免与 S13 联调时旧文件混淆），`/tmp/ivsboxd` 保留。

---

## 8. 收尾固定动作（规则 1/2/4/6/8）

1. **规则 1**：`docs/IVSBox-功能开发列表.md` §10 实现记录，S02 行改为"已实现 + 完成时间 + 说明"；§3-S02 原文按决策 D1 更正（JSON→key=value 直接字符串解析、配置路径 `/opt/ivsbox/config/`、日志→syslogd）。
2. **规则 2**：`docs/修改记录.md` 末尾追加一条（修改文件清单 / 问题目的 / 处理方式 / 验证结果），追加前先重读文件最新内容。
3. **规则 4**：`docs/代码说明.md` 追加一节（干了什么事 / 解决什么问题 / 影响范围 / 验证方式与结果）。
4. **规则 6**：清理临时物（板端配置目录、VM 侧一次性脚本等），`git status` 自检无任务外未跟踪文件。
5. **规则 8 收尾顺序**：
   ```powershell
   git stint commit -m "feat(core): S02 基础设施（CRC/环形缓冲/配置原子写/syslog）" --session trae-s02
   git stint squash  -m "feat(core): S02 core 基础设施" --session trae-s02
   git fetch origin
   git stint merge --session trae-s02    # 合并回 main 并清理
   git push
   git stint list                        # 应为 No active sessions
   ```
   若与另一 AI 会话在共用文档（修改记录/代码说明/AGENTS.md）冲突：**双方记录都保留、按时间倒序**，绝不丢弃对方条目。

---

## 9. 验收清单（对照开发列表 §3-S02 + 本手册修正）

- [ ] CRC8 / CRC16 已知向量测试通过（①）
- [ ] 环形缓冲空/满/回绕/非 2 幂拒绝测试通过（②）
- [ ] 配置往返测试通过（③，替代原 JSON 往返，决策 D1）
- [ ] 非法配置被拒绝、回退默认值测试通过（④）
- [ ] 时钟单调性测试通过（⑤）
- [ ] `ctest` 全绿（主机侧，VM）
- [ ] ARM 构建通过 + 板端 `RUN_EXIT=0`
- [ ] 板端 `logread | grep ivsbox` 能看到日志（syslog 后端生效）
- [ ] 板端 `/opt/ivsbox/config/ivsbox.conf` 生成成功
- [ ] 开发列表 / 修改记录 / 代码说明 均已回写并推送 GitHub（`https://github.com/ppp1234lll/T113_S4` main）
- [ ] stint 会话已 merge + end，`.stint/` 无残留

## 10. 常见坑速查（S02 相关）

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 忘了 fsync 目录 | rename 本身没落盘，掉电后新旧文件都丢 | 步骤 3 第 4 步不可省 |
| `.tmp` 放别的目录 | rename 跨文件系统 → `EXDEV`，非原子 | tmp 与目标同目录 |
| 不处理 `EINTR` / 部分写 | 偶发写失败/半截文件 | 循环 write + EINTR 重试 |
| 环形缓冲取模用 `%` | 每字节一次除法，串口高峰浪费 CPU | cap 取 2 的幂，`& (cap-1)` |
| 环形缓冲动态 malloc | 违反上限纪律，长稳难查漏 | 调用方传静态存储 |
| 日志里打中文 | 板端 GBK 控制台乱码，故障日志不可用 | 日志 ASCII，中文进注释 |
| 用 `gettimeofday` 算时长 | 对时后时长变负/巨大 | 一律 `ivs_clock_mono_ms()` |
| 板端用 `/data` 路径 | 目录不存在，open 直接失败 | `/opt/ivsbox/config/` |

## 11. 实现记录

| 功能 | 状态 | 完成时间 | 说明 |
|---|---|---|---|
| 本操作手册编制 | 已实现 | 2026-09-23 17:55 | 依据开发列表 §3-S02 与 bsp-capability 回填结论编制，含决策 D1（不引入 cJSON） |
| S02 core 基础设施 | 未实现 | — | 按本手册步骤 1~6 执行 |
