# S02 操作卡片

> 本文件是给**干活的人**看的操作卡：只写步骤、命令、判读。背景论证一概不写，查证记录都在 `bsp-capability.md`，决策 D1~D4 都在 `修改记录.md`。
> 编制：2026-09-24 11:45（自 240 行手册压缩而来）

## 待办

| # | 任务 | 文件 |
|---|---|---|
| 3 | 配置读写 | `src/core/iv_config.c` `include/ivsbox/iv_config.h` `tests/unit/test_config.c` |
| 4 | 日志接 syslogd | 改 `src/core/iv_log.c`，根 CMakeLists 加开关 |
| 5 | 单实例锁 | `src/app/main.c` |
| 6 | main 接线 | `src/app/main.c` |

（1 CRC 已完成；2 环形缓冲已删除）

**纪律**：选库优先用现成 Linux 库（开发列表 §6.11）；无现成的→先自研→登记 `bsp-capability.md` §7.6→做镜像时替换。syscall 判返回值+errno；EINTR 重试；日志 ASCII；有界无 malloc；时长用 `ivs_clock_mono_ms()`。

---

## 步骤 3：配置读写

**做**：

```c
/* iv_config.h */
typedef struct { char key[32]; char val[64]; } ivs_config_kv_t;
int ivs_config_load(const char *path, ivs_config_kv_t *kv, size_t max_kv, size_t *out_n);
int ivs_config_save(const char *path, const ivs_config_kv_t *kv, size_t n);
```

- 格式 `/opt/ivsbox/config/ivsbox.conf`：每行 `key = value`，`#` 注释。
- load：`fopen`+`getline` 逐行 → 首个 `=` 切开 → 去空白 → 存 kv；注释/空行跳过；超长/重复/空键 → `IVS_ERR_INVAL`。
- save：`open_memstream` 拼文本 → mkdir 目录（EEXIST 不算错）→ 写 `*.tmp`（循环 write，fsync，close）→ `rename` → fsync(目录fd)。
- load 失败 → 调用方回退默认值，进程不死。

**测**（VM）：

```sh
cmake -S ivsbox-v5 -B ivsbox-v5/build/host -DIVSBOX_BUILD_TESTS=ON
cmake --build ivsbox-v5/build/host -j && ctest --test-dir ivsbox-v5/build/host --output-on-failure
```

`test_config.c`：①save→load 往返一致；②未知/超长/重复/空键拒绝；③坏文件→默认值。

**上板**：

```sh
scp ivsbox-v5/build/host/ivsboxd root@192.168.2.105:/tmp/ivsboxd   # 应为 build/arm
chmod +x /tmp/ivsboxd && /tmp/ivsboxd && cat /opt/ivsbox/config/ivsbox.conf
```

---

## 步骤 4：日志接 syslogd

**做**：

- 根 CMakeLists：`option(IVSBOX_LOG_SYSLOG ... OFF)`，ON 时给 ivcore 挂 `IVSBOX_LOG_SYSLOG` 定义。
- `iv_log.c`：`#ifdef` 内 `openlog("ivsboxd", LOG_PID|LOG_NDELAY, LOG_LOCAL0)`，ERROR→LOG_ERR / WARN→LOG_WARNING / INFO→LOG_INFO / DEBUG→LOG_DEBUG。
- 板端构建 `-DIVSBOX_LOG_SYSLOG=ON`；主机单测保持 stderr。不写轮转（syslogd 自己做）。

**测**：主机 ctest 全绿（仍 stderr）。

**上板**：

```sh
cmake -S ivsbox-v5 -B ivsbox-v5/build/arm \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/ivsbox-v5/cmake/toolchains/tina-arm.cmake \
  -DIVSBOX_BUILD_TESTS=OFF -DIVSBOX_LOG_SYSLOG=ON -DCMAKE_BUILD_TYPE=Release
cmake --build ivsbox-v5/build/arm -j
scp ivsbox-v5/build/arm/ivsboxd root@192.168.2.105:/tmp/ivsboxd
```

判读：`logread | grep ivsbox` 有输出。

---

## 步骤 5：单实例锁

**做**（main 最前）：

```c
#include <sys/file.h>
#include <fcntl.h>
int fd = open("/var/run/ivsboxd.pid", O_CREAT|O_RDWR, 0644);
if (fd < 0 || flock(fd, LOCK_EX|LOCK_NB) != 0) return 1;   /* 已有实例 */
/* 不 close，退出时内核释放 */
```

**测**：板端连跑两次，第二次退出码 1。

---

## 步骤 6：main 接线

**做**（版本打印后）：

```c
int rc = ivs_config_load(path, kv, MAX, &n);
rc ? IVS_LOGW("cfg", "load failed, defaults: %s", ivs_strerror(rc))
   : IVS_LOGI("cfg", "loaded %u", n);
```

**判读**：板端跑一次，logread 有 cfg 行、conf 文件生成。全过 → S02 完成，收尾见下。

---

## 收尾

- 开发列表 §10、`修改记录.md`、`代码说明.md` 各追加一条；`git status` 无多余文件。
- worktree 会话：commit → main `merge --ff-only` → `worktree remove`（先 cd 出来，否则 Windows 句柄占用删不掉）→ `branch -d` → push。

## 板端命令速查

```sh
/tmp/ivsboxd; echo RUN_EXIT=$?        # 期望 0
/tmp/ivsboxd; echo SECOND_EXIT=$?     # 期望 1（单实例锁）
logread | grep ivsbox
cat /opt/ivsbox/config/ivsbox.conf
rm -rf /opt/ivsbox                    # 冒烟完清残留
```
