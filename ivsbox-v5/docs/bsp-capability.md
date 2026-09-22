# BSP 能力清单（M0 产出，待填）

> 用途：架构文档 §14 结尾要求"立项初期必须生成 BSP 能力清单"。
> **不填完这张表，后面写代码就是在赌。** 里面的每一项都直接决定某个方案能不能按现架构落地。
> 填法：在板子上跑 `scripts/bsp-survey.sh`，把输出贴到本文档 §2，然后逐行填"结论"列。

---

## 1. 怎么跑盘点

```sh
# 板子上（注意：脚本要能执行）
chmod +x bsp-survey.sh
./bsp-survey.sh > /tmp/bsp-survey.txt 2>&1

# 主机上拷回来
scp root@<板子IP>:/tmp/bsp-survey.txt .
```

脚本只读不写（除了三个目录的写权限探测，探测文件会立即删除），可反复跑。

---

## 2. 盘点结论（待填）

### 2.1 工具链与运行环境

| 检查项 | 命令/位置 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| libc 类型 | `ls -l /lib/ld-*` | | musl / glibc | S01 工具链前缀 |
| 内核版本 | `uname -a` | | | S01 |
| procd / logd / ubus | `which logd procd ubus` | | 有无 | S18、S02 |
| 板上调试工具 | `which strace gdbserver` | | 有无 | 全线 |

### 2.2 串口（板间链路 / GPS / 调试）

| 检查项 | 命令 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| 串口设备清单 | `ls -l /dev/ttyS*` | | 有几个、叫什么 | S04 |
| 板间链路用哪个口 | 硬件确认 | | `/dev/ttyS?` + 波特率 | S04、S06 |
| GPS 用哪个口 | `cat /proc/tty/driver/serial` | | `/dev/ttyS?` + 9600? | S07 |
| 波特率上限误差 | 查芯片手册 | | 460800 是否可行 | S04 |

> **注意**：T113 的 UART 名字在不同 Tina 配置里可能是 `ttyS0..ttyS5`，也可能叫别的。
> 必须实测确认，不能照抄文档。

### 2.3 网络与 4G

| 检查项 | 命令 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| 有线网口 | `ip -o link` | | 接口名（eth0?） | S11、S12 |
| 默认路由 | `ip route` | | metric 策略 | S12 |
| 4G 模组型号 | `lsusb` | | EC200G / ML307 / 其他 | S12 |
| 4G 出的网卡名 | `ls /sys/class/net` | | usb0 / wwan0 | S12、S10 |
| 拨号栈 | `which udhcpc pppd quectel-CM` | | 用哪一个 | S12 |
| 热插拔机制 | `ls /etc/hotplug.d` | | 有无 | S12 |

### 2.4 看护（这是"设备莫名其妙重启"的根因区）

| 检查项 | 命令 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| 看门狗设备 | `ls -l /dev/watchdog*` | | 有无、权限 | S18 |
| 超时能力 | `cat /sys/class/watchdog/*/timeout` | | 可设范围 | S18 |
| 驱动类型 | `cat /sys/class/watchdog/*/identity` | | 硬件狗 / 软狗 | S18 |

### 2.5 媒体与音频

| 检查项 | 命令/位置 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| ffmpeg | `which ffmpeg` + `ffmpeg -version` | | 有无、版本 | S16 |
| libavformat 等库 | `ls /usr/lib/libavformat*` | | 有无 | S16 |
| ALSA | `ls /dev/snd`、`aplay -l` | | 有无声卡 | S16 音频 |
| V4L2 | `ls /dev/video*` | | 有无 | S16 抓图 |

### 2.6 中间件

| 检查项 | 命令/位置 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| SQLite | `ls /usr/lib/libsqlite3*` | | 有无 | S13、S14 |
| JSON 库 | `ls /usr/lib/libcjson*` | | 有则直接用，无则自带源码 | S02、S10、S13 |
| TLS 库 | `ls /usr/lib/libssl* /usr/lib/libmbed*` | | mbedTLS / OpenSSL | S10、S19 |
| cgroup | `ls /sys/fs/cgroup` | | v1 / v2 / 无 | S16 资源隔离 |
| netfilter | `which nft iptables` | | 有无 | S12 策略路由配合 |
| gpsd | `which gpsd` | | 有则直接用 | S07 |

### 2.7 存储

| 检查项 | 命令 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| 分区与容量 | `df -h` | | `/data` 多大、`/opt` 多大 | S13、S19 |
| `/data` 可写 | 脚本自动探测 | | OK / FAIL | S13 |
| `/opt` 可写 | 脚本自动探测 | | OK / FAIL | S19 双版本 |
| SD 卡 | `df -h`、`cat /proc/partitions` | | 挂载点、文件系统 | S16 |
| 内存总量 | `head -8 /proc/meminfo` | | MemTotal | **G1 资源预算** |

**实测内存/资源数据填到** `docs/resource-budget.md`（缺口 G1）。

---

## 3. 三条红线：如果这几项不满足，架构要改

| 红线 | 判据 | 不满足的后果 | 备选方案 |
|---|---|---|---|
| **① 没有 ffmpeg / libavformat** | `which ffmpeg` = NO 且 `ls /usr/lib/libavformat*` 为空 | 录像与抓图的"码流直封装"方案失效 | ① 自己交叉编译 FFmpeg 进镜像；② 查是否已有 `gstreamer`；③ 降级为只做图片抓取 |
| **② `/opt` 只读或容量不足** | 写探测 FAIL，或 `/opt` 剩余空间 < 两个版本总和 | OTA 的"应用双版本 + 符号链接切换"无法实现 | ① `/opt` 改为可写分区；② 双版本目录改放 `/data`（需确认 `/data` 是否在 eMMC/SPI NAND 上且可靠） |
| **③ 没有 cgroup 或没有 /dev/watchdog** | `ls /sys/fs/cgroup` 空 且 `/dev/watchdog*` 不存在 | 媒体资源隔离失效；"进程活着但卡死"无法检测 | ① 用 `nice`/`setpriority` 做弱隔离；② 退化为"仅 procd 进程级 respawn"（必须明确告知：卡死无法恢复） |

---

## 4. 主机侧要单独确认的（不在板上跑）

| 检查项 | 命令 | 实际结果 | 说明 |
|---|---|---|---|
| cmake | `cmake --version` | **本机实测：不存在** | S01 前必须装，否则无法配置 ARM 构建 |
| SDK 根目录 | `echo $TINA_SDK_ROOT` | | `source build/envsetup.sh && lunch` 后导出 |
| 工具链前缀 | `ls $TINA_SDK_ROOT/out/*/staging_dir/toolchain-*/bin/` | | 填到 `-DIVSBOX_CROSS_PREFIX=` |
| sysroot | `ls $TINA_SDK_ROOT/out/*/staging_dir/target` | | 与 `tina-arm.cmake` 里的路径核对 |

---

## 5. 主机环境准备（S01 的第一步）

**本机当前没有 cmake**（已验证：`Get-Command cmake` 返回空，WindowsApps 别名与 Program Files 都没有）。
先装一个，二选一：

```powershell
# 方式 A：winget（推荐）
winget install --id Kitware.CMake -e

# 方式 B：官网下载 zip 版，解压后把 bin 目录加进 PATH
# https://cmake.org/download/  → cmake-3.x.x-windows-x86_64.zip
```

装完验证：

```powershell
cmake --version
```

> 另外：在这台机器上做**主机侧**单测可以完全绕开 cmake，直接调 gcc 就行
> （骨架就是这么验证的，见 `build/_verify.txt`）。
> 但**ARM 侧必须用 cmake** 走 `cmake/toolchains/tina-arm.cmake`。

---

## 6. 完成标志

- [ ] `bsp-survey.txt` 已回传并贴进本文档 §2
- [ ] §2 各表"结论"列已填满
- [ ] §3 三条红线全部确认（不满足的已选定备选方案）
- [ ] §4 主机侧四项已确认
- [ ] 主机已装 cmake 且 `cmake --version` 可执行

完成本清单后，S01 才具备真正开工的条件。
