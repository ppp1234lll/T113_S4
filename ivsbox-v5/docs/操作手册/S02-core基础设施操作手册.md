# S02 core 基础设施 — 操作手册（精简版）

> 任务来源：`ivsbox-v5/docs/IVSBox-功能开发列表.md` §3-S02
> 前置：S01 已完成（ARM 二进制可上板运行）
> 选型总规则（用户定，2026-09-24）：**处理代码优先使用现成的 Linux 开发库**；板上没有时**先自研实现保证进度**，再把需要的库登记进待安装清单，等做镜像时一并加入。项目级落点：开发列表 §6 纪律第 11 条；待安装库登记处：`bsp-capability.md` §7.6。
> 编制：2026-09-23 17:55　**2026-09-24 11:35 精简重写**（用户反馈"只要需要实现的步骤、每步怎么做、怎么验证"）。

---

## 0. 要实现的步骤

| 步骤 | 内容 | 状态 |
|---|---|---|
| 1 | CRC 校验（zlib crc32 薄包装） | ✅ 已完成 |
| 2 | 环形缓冲 | ❌ **删除**（决策 D3） |
| 3 | 配置读写（key=value，掉电安全） | 待做 |
| 4 | 日志接 syslogd | 待做 |
| 5 | 错误码/时钟核对 + 单实例锁 | 待做 |
| 6 | main 接线 | 待做 |

只做 3/4/5/6 这四件，不做别的。步骤 2 已删除：串口读在主 Reactor 线程（决策 D4 硬约束），`read()` 到 `EAGAIN` 直接喂流式帧解析器，内核 tty 缓冲已是缓冲，用户态环只多一次拷贝。查证记录在 `bsp-capability.md` §7。

---

## 1. 环境与前提

| 项 | 值 |
|---|---|
| 编译机 | `ssh tqt113`（192.168.2.115），VM 内仓库 `~/work/T113_S4` |
| 开发板 | 192.168.2.105（root，口令见现场登记，不入库） |
| 串口 | Windows COM4 115200，`serial_run.py COM4 115200 cmds.txt out.txt`（命令一行一条，详见《日常开发闭环流程》§7） |
| 会话隔离 | `git worktree add .stint/<前缀>-<主题> -b stint/<前缀>-<主题>`，改动只在 worktree 内，收尾 merge 回 main（git-stint 本机失效，用等价 worktree，详见闭环流程 §9） |

**编码纪律**（开发列表 §6，写码前读一遍）：系统调用判返回值 + errno 进日志；EINTR 重试；回调内不阻塞；日志 ASCII；有界无 malloc；时长一律 `ivs_clock_mono_ms()`。

---

## 2. 步骤 1：CRC（已完成，只留复现要点）

`ivs_crc32(crc, data, len)` 薄包装 zlib `crc32()`，分段累计：首次传 0，后续传上次返回值（S06 半包用）。CRC8/16 不预写，等 S06 确认帧格式。

- 验证：`test_crc.c` 已入 ctest，向量 `"123456789"` → `0xCBF43926` 等 4 条全过。

---

## 3. 步骤 3：配置读写

**文件**：`src/core/iv_config.c` + `include/ivsbox/iv_config.h` + `tests/unit/test_config.c`

**格式**（`/opt/ivsbox/config/ivsbox.conf`）：

```text
# IVSBox config (v1)
log.level = info
link.baudrate = 115200
```

**API**：

```c
typedef struct { char key[32]; char val[64]; } ivs_config_kv_t;
int ivs_config_load(const char *path, ivs_config_kv_t *kv, size_t max_kv, size_t *out_n);
int ivs_config_save(const char *path, const ivs_config_kv_t *kv, size_t n);
```

**怎么做**：

1. 读：`fopen` + `getline()` 逐行；`#` 注释/空行跳过；首个 `=` 分键值，去两侧空白；键/值超限、重复键、空键 → `IVS_ERR_INVAL`。getline 自带扩容，不用手写 realloc。
2. 写：`open_memstream()` 拼全量文本 → 按原子写序列落盘（下）。
3. 原子写序列（核心考点，一步不能省）：
   ```c
   mkdir(dir, 0755);                         /* EEXIST 不算错 */
   fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC, 0644);
   循环 write（部分写/EINTR）；fsync(fd); close(fd);
   rename(tmp, final);                        /* 同目录，原子替换 */
   fd = open(dir, O_RDONLY|O_DIRECTORY); fsync(fd); close(fd);   /* fsync 目录，rename 才落盘 */
   ```
4. 加载失败 → 回退默认值 + `IVS_LOGW`，进程不死。
5. 库选型：glibc 自带 `getline`/`open_memstream`，**无第三方库，无需登记**。

**验证**：

- 主机单测 `test_config.c`（并入 ctest）：
  - 往返：save→load 后每对 key/value 一致（含注释行、`key = value` 空白容错）；
  - 拒绝：未知键/超长键/重复键/空键 → `IVS_ERR_INVAL`；坏文件加载后调用方拿到默认值。
- 板端：跑 `ivsboxd` 后 `cat /opt/ivsbox/config/ivsbox.conf` 有内容。

---

## 4. 步骤 4：日志接 syslogd

**文件**：改 `src/core/iv_log.c`（不动 `iv_log.h` 接口）+ 根 `CMakeLists.txt` 加开关。

**板端事实（实测，别再查）**：busybox syslogd v1.33.2，**忽略 `/etc/syslog.conf`** → `LOG_LOCAL0` 不分流文件、只当 grep 标签；轮转它自己做（默认 200KB × 2 个文件）；想长期留痕改板端 `/etc/default/syslogd`（S13/固件层，不在 S02）。

**怎么做**：

1. CMake 加开关：
   ```cmake
   option(IVSBOX_LOG_SYSLOG "syslog output (ON for board, default OFF=stderr)" OFF)
   if(IVSBOX_LOG_SYSLOG)
       target_compile_definitions(ivcore PUBLIC IVSBOX_LOG_SYSLOG)
   endif()
   ```
2. `iv_log.c` 内 `#ifdef IVSBOX_LOG_SYSLOG`：
   ```c
   #include <syslog.h>
   /* init: openlog("ivsboxd", LOG_PID|LOG_NDELAY, LOG_LOCAL0); */
   /* map: ERROR→LOG_ERR WARN→LOG_WARNING INFO→LOG_INFO DEBUG→LOG_DEBUG */
   ```
3. **不实现轮转**，只负责把日志行交给 syslog(3)。
4. 板端构建加 `-DIVSBOX_LOG_SYSLOG=ON`；主机单测保持 stderr（logread 是板端专属）。
5. `iv_log.h` 里"由 logd 汇聚"字样顺带改成 syslogd。
6. 库选型：glibc 自带 `syslog(3)`，**无第三方库，无需登记**。

**验证**：

- 主机：单测仍走 stderr，`IVS_LOGI` 输出正常、ctest 全绿。
- 板端：`logread | grep ivsbox` 能看到 `LOG_INFO`/`LOGW` 行。

---

## 5. 步骤 5：核对 + 单实例锁

**文件**：`src/app/main.c`（加锁代码）；`iv_err.h`/`iv_clock.h` 只核对不修改。

**怎么做**：

1. 核对 `iv_err.h`：`0=成功/负数=失败`，与 `-errno` 惯例一致，直接复用 errno 取值，**不另造错误码**。
2. 核对 `iv_clock`：确认 `ivs_clock_mono_ms()` 用 `CLOCK_MONOTONIC`（单测见 §7）。
3. main 最前（版本打印前）加单实例锁：
   ```c
   #include <sys/file.h>
   #include <fcntl.h>
   int g_lock_fd = open("/var/run/ivsboxd.pid", O_CREAT|O_RDWR, 0644);
   if (g_lock_fd < 0) { IVS_LOGE("main", "open pidfile: %s", strerror(errno)); return 1; }
   if (flock(g_lock_fd, LOCK_EX|LOCK_NB) != 0) { IVS_LOGE("main", "another instance running, exit"); return 1; }
   /* 不 close：进程退出时内核自动释放 */
   ```
4. 库选型：`flock`/`open` 是内核接口，**无第三方库，无需登记**。

**验证**：

- 主机：`./ivsboxd && ./ivsboxd` 第二次启动退出码 1。
- 板端：连续启动两次，第二次 `SECOND_EXIT=1` 且日志有 "another instance"。

---

## 6. 步骤 6：main 接线（冒烟覆盖）

**文件**：`src/app/main.c`。

**怎么做**：

```c
/* after version print */
int rc = ivs_config_load(IVS_CONFIG_PATH, kv, MAX_KV, &n);
if (rc != 0) IVS_LOGW("cfg", "load failed, using defaults: %s", ivs_strerror(rc));
else         IVS_LOGI("cfg", "config loaded, entries=%u", n);
```

**验证**：板端跑一次，`logread | grep ivsbox` 出现 cfg 行、`/opt/ivsbox/config/ivsbox.conf` 生成。

---

## 7. 单元测试与整体验证

**新增 `tests/unit/`：`test_config.c`（往返+拒绝）、`test_clock.c`（单调性）**；`tests/CMakeLists.txt`：

```cmake
foreach(t test_crc test_config test_clock)
    add_executable(${t} unit/${t}.c)
    target_link_libraries(${t} PRIVATE ivcore ivhal)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
```

**主机全量验证（VM 内）**：

```sh
cd ~/work/T113_S4 && git pull --ff-only
cmake -S ivsbox-v5 -B ivsbox-v5/build/host -DIVSBOX_BUILD_TESTS=ON
cmake --build ivsbox-v5/build/host -j
ctest --test-dir ivsbox-v5/build/host --output-on-failure   # 期望全绿
```

**ARM 交叉编译 + 板端冒烟**：

```sh
cmake -S ivsbox-v5 -B ivsbox-v5/build/arm \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/ivsbox-v5/cmake/toolchains/tina-arm.cmake \
  -DIVSBOX_BUILD_TESTS=OFF -DIVSBOX_LOG_SYSLOG=ON -DCMAKE_BUILD_TYPE=Release
cmake --build ivsbox-v5/build/arm -j
scp ivsbox-v5/build/arm/ivsboxd root@192.168.2.105:/tmp/ivsboxd
```

板端（COM4，命令写入 cmds.txt）：

```sh
chmod +x /tmp/ivsboxd
/tmp/ivsboxd; echo RUN_EXIT=$?
/tmp/ivsboxd; echo SECOND_EXIT=$?        # 期望 1（单实例锁生效）
logread | grep ivsbox
cat /opt/ivsbox/config/ivsbox.conf
rm -rf /opt/ivsbox                       # 冒烟完清残留
```

**判读标准**：`RUN_EXIT=0`；`SECOND_EXIT=1`；logread 有 cfg 行；conf 文件生成。全过 → S02 完成。

---

## 8. 收尾（规则 1/2/4/6/8）

1. 开发列表 §10 实现记录：S02 行改"已实现 + 时间"。
2. `修改记录.md` 末尾追加（先重读最新内容）。
3. 有代码产出 → `代码说明.md` 追加一节（四要点）。
4. 清理临时物，`git status` 无任务外未跟踪文件。
5. worktree 会话收尾：worktree 内 commit → main `merge --ff-only` → `worktree remove` + `branch -d` → push。

---

## 附录 A：SPSC 环形缓冲原设计（存档）

> 已删除（决策 D3，2026-09-24 用户确认）。**启用条件唯一**：确认 UART 读不在主 Reactor 线程（独立线程/厂商 SDK 回调）时才启用（决策 D4，见 `bsp-capability.md` §7 与开发列表 §6 第 11 条）。
> 启用时必做：`volatile` 换成 C11 `<stdatomic.h>` acquire/release；补满/空/回绕/非 2 幂拒绝单测。

```c
typedef struct {
    uint8_t        *buf;
    size_t          cap;    /* 2 的幂 */
    volatile size_t head;   /* 仅生产者写（启用时换 stdatomic） */
    volatile size_t tail;   /* 仅消费者写（启用时换 stdatomic） */
} ivs_ring_t;
int    ivs_ring_init(ivs_ring_t *r, uint8_t *storage, size_t cap);
size_t ivs_ring_write(ivs_ring_t *r, const uint8_t *data, size_t len);
size_t ivs_ring_read (ivs_ring_t *r, uint8_t *out, size_t len);
size_t ivs_ring_used (const ivs_ring_t *r);
```

要点：cap 强制 2 的幂，索引用 `& (cap-1)`；静态存储不 malloc；`head==tail` 空、`head-tail==cap` 满。
