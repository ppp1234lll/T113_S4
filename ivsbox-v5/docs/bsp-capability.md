# BSP 能力清单（M0 产出，2026-09-23 已回填）

> 用途：架构文档 §14 结尾要求"立项初期必须生成 BSP 能力清单"。
> **不填完这张表，后面写代码就是在赌。** 里面的每一项都直接决定某个方案能不能按现架构落地。
> 填法：在板子上跑 `scripts/bsp-survey.sh`，把输出贴到本文档 §2，然后逐行填"结论"列。
> **回填状态**：2026-09-23 在 TQT113 开发板（192.168.2.105）实测完成，盘点脚本 856 行输出已消化进下表，
> 关键原始行摘录见各表"实际结果"列；临时盘点文件已清理。

---

## 0. 一页结论（先看这个）

| 维度 | 结论 | 对任务的影响 |
|---|---|---|
| rootfs | **Buildroot 2019.02.1 + glibc 2.25**，SysV init（非 OpenWrt） | S02 用 syslogd 不用 logd；S17 用 lighttpd 不用 uhttpd；S18 无 procd respawn；S20 不打 ipk |
| 工具链匹配 | 板端 `ld-2.25.so` 与编译 sysroot **完全一致**，`ivsboxd` 已实机跑通 | S01 已闭环 |
| 内核 | 5.4.61-rt37（**PREEMPT_RT**）armv7l，双核 A7 + NEON | 实时性可用；媒体/网络延迟场景有利 |
| 内存 | 220 MB（可用 ~174 MB） | **G1 资源预算**的硬上限 |
| 存储 | root 2.0G（余 1.5G）+ `/mnt/UDISK` 27G（余 26G），均可写；**无 `/data`** | 应用装 `/opt`；媒体/录像/数据库大文件放 `/mnt/UDISK`；架构文档 §10 的 `/data` 路径需映射调整 |
| 看门狗 | `/dev/watchdog` 硬件狗（sunxi）存在 | S18 可做 |
| cgroup | **不可用**（子系统全关） | 媒体资源隔离降级为 `nice` 弱隔离 |
| ffmpeg | **无**，但有 `gst-launch-1.0` | 红线①触发 → S16 走 GStreamer 路线（待补查库完整性） |
| 4G | Quectel 模组（USB ID `2c7c:0901`，型号待确认），**ECM 网卡 usb0 已 DHCP 到 IP**，附 7 路 ttyUSB | S12 拨号走 ECM+udhcpc，无需 QMI 栈 |
| 现成服务 | lighttpd、postgresql、dnsmasq、dbus、telnet、adb、vsftpd 已装 | S17 复用 lighttpd；数据库选型时注意 postgresql 已占资源 |
| 板载时钟 | 开机为 **1970-01-01**（RTC/NTP 未同步） | 墙钟不可信，S02 单调时钟纪律必须严格执行 |

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
**2026-09-23 已按此流程实测一轮**（经编译 VM 中转 scp，输出 856 行）。

---

## 2. 盘点结论（已填，实测于 2026-09-23）

### 2.1 工具链与运行环境

| 检查项 | 命令/位置 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| libc 类型 | `ls -l /lib/ld-*` | `ld-linux.so.3 -> ld-2.25.so`、`libc.so.6 -> libc-2.25.so` | **glibc 2.25**，与编译 sysroot 完全一致（`ivsboxd` 实机运行成功已佐证） | S01 工具链前缀 ✅ |
| 内核版本 | `uname -a` | `Linux TQ113 5.4.61-rt37 #1 SMP PREEMPT ... armv7l`；编译器 Linaro GCC 5.3-2016.05 | **5.4.61 + PREEMPT_RT 实时补丁**；CPU 双核 Cortex-A7 rev5，带 NEON/VFPv4 | S01、S16 |
| procd / logd / ubus | `which logd procd ubus` | 三者**全无**；`/etc/init.d` 为 SysV 风格（`S01syslogd`、`S02klogd`、`rcS`/`rcK`），有 syslogd/klogd | init 体系是 **SysV + syslogd**，非 OpenWrt | S02（日志接 syslog）、S18（无 procd respawn，看护方案自管）、S20（不打 ipk） |
| 板上调试工具 | `which strace gdbserver` | 有：`ssh` `scp` `wget` `gdbserver` `lsof` `top`；无：`strace` `gdb` `tcpdump` `socat` `curl` `openssl`(CLI) | **gdbserver 可远程调试**；strace 缺失，深排障需交叉编译一个推上去 | 全线 |

### 2.2 串口（板间链路 / GPS / 调试）

| 检查项 | 命令 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| 串口设备清单 | `ls -l /dev/ttyS*` | `ttySAC2` `ttySAC3` `ttySAC4` `ttySAC5`（sunxi 命名）+ `ttyUSB0..6`（7 路全部来自 4G 模组，option 驱动） | 板载可用 UART 为 **ttySAC2~SAC5 共 4 个**；ttyUSB0~6 属模组，勿挪作他用 | S04 |
| 板间链路用哪个口 | 硬件确认 | **待硬件确认**（需对照底板原理图确定采集板接在 SAC 几） | 候选 ttySAC2~SAC5，接通前无法写死 | S04、S06 |
| GPS 用哪个口 | `cat /proc/tty/driver/serial` | 该 proc 文件不存在；GPS 口**待硬件确认** | 同上 | S07 |
| 波特率上限误差 | 查芯片手册 | **待确认**（T113 手册核对 460800 分频误差 <2%） | S04 打开前必须核实 | S04 |

> **注意**：T113 的 UART 名字在这块板上是 `ttySAC*`（不是 `ttyS*`），与部分资料不符，以实测为准。

### 2.3 网络与 4G

| 检查项 | 命令 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| 有线网口 | `ip -o link` | `eth0` UP，sunxi-gmac **1Gbps 全双工**（PHY ID `4f51e91b`），MAC `86:4c:be:da:a1:ef`；另有 `can0`/`can1` 两路 CAN（DOWN） | 有线口 `eth0` 可用；**CAN×2 是额外收获**（运维箱对接 CAN 设备可直接用） | S11、S12 |
| 默认路由 | `ip route` | 双默认路由：`via 192.168.2.1 dev eth0 metric 204` + `via 10.156.214.1 dev usb0 metric 208` | 有线 metric 更优；双出口已并存，S12 状态机可直接接管 metric 策略 | S12 |
| 4G 模组型号 | `lsusb` | `2c7c:0901`（**Quectel**，具体型号待确认——官方语雀资料未覆盖此 ID，`lsusb` 无字符串输出） | Quectel 系；`option` 驱动挂出 ttyUSB0~6 + `cdc_ether` 挂出 usb0 | S12 |
| 4G 出的网卡名 | `ls /sys/class/net` | `usb0`（cdc_ether），已 DHCP 到 `10.156.214.195/24` | **模组以 ECM 方式出网且已打通** | S12、S10 |
| 拨号栈 | `which udhcpc pppd quectel-CM` | 有 `udhcpc` `dhcpcd` `pppd` `chat`；无 `uqmi` `mmcli` `quectel-CM` `qmicli`；内核 `QMI_WWAN` 未启用 | 拨号方案 = **ECM + udhcpc**（现状已能上网）；PPP 备选；QMI 不可行 | S12 |
| 热插拔机制 | `ls /etc/hotplug.d` | 无该目录（OpenWrt 机制）；实际跑 **eudev**（udevd 3.2.7） | 4G 模组热插拔监听走 **udev 规则**，不是 OpenWrt hotplug.d | S12 |

### 2.4 看护（这是"设备莫名其妙重启"的根因区）

| 检查项 | 命令 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| 看门狗设备 | `ls -l /dev/watchdog*` | `/dev/watchdog`（10:130）与 `/dev/watchdog0` 存在，root 可读写 | **硬件看门狗存在**，S18 可落地 | S18 ✅ |
| 超时能力 | `cat /sys/class/watchdog/*/timeout` | 读出为空（`CONFIG_WATCHDOG_SYSFS` 未开启）；内核配置：`SUNXI_WATCHDOG=y`、`WATCHDOG_CORE=y`、`NOWAYOUT` 未设、`OPEN_TIMEOUT=0` | sysfs 不可读，超时范围**须经 `WDIOC_GETTIMEOUT/SETTIMEOUT` ioctl 实测**（S18 开工第一件事） | S18 |
| 驱动类型 | `cat /sys/class/watchdog/*/identity` | sysfs 为空；设备树路径 `20500a0.watchdog` = **sunxi 片上硬件狗** | 硬件狗（非软狗）；NOWAYOUT 关闭意味着进程退出可正常停喂，开发期友好 | S18 |

### 2.5 媒体与音频

| 检查项 | 命令/位置 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| ffmpeg | `which ffmpeg` + `ffmpeg -version` | **无**（ffmpeg/ffprobe 均不存在） | 红线①触发 → 备选②：`gst-launch-1.0` **存在** | S16 |
| libavformat 等库 | `ls /usr/lib/libavformat*` | 全无 | 同上；GStreamer 库完整性需补查（`gst-inspect-1.0` 清点插件） | S16 |
| ALSA | `ls /dev/snd`、`aplay -l` | 完整：`aplay`/`arecord`/`amixer`/`alsactl`、`libasound.so.2`，两张卡（`audiocodec` + `snddaudio0`，各 1 录 1 放） | **音频链路齐备**，S16 音频部分可直接 ALSA 起步 | S16 音频 ✅ |
| V4L2 | `ls /dev/video*` | 仅 `/dev/video4` 一个节点（用途待确认，`/dev/video0~3` 不存在） | 本机无通用摄像头节点；S16 抓图以**摄像机侧 HTTP 抓拍/RTSP**为主（架构本就如此），`video4` 待查 | S16 抓图 |

### 2.6 中间件

| 检查项 | 命令/位置 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| SQLite | `ls /usr/lib/libsqlite3*` | `libsqlite3.so.0.8.6` + CLI `sqlite3 3.25.3` | **可用**（3.25.3，2018 年版，无窗口函数等新特性，S13 建表时避开） | S13、S14 ✅ |
| JSON 库 | `ls /usr/lib/libcjson*` | 无 | 按原计划**自带 cJSON 源码**进仓库（S02 既定方案，不受影响） | S02、S10、S13 ✅ |
| TLS 库 | `ls /usr/lib/libssl* /usr/lib/libmbed*` | **OpenSSL 1.1**（`libssl.so.1.1`、`libcrypto.so.1.1`）；无 mbedTLS；`openssl` CLI 无 | TLS 走 **OpenSSL 1.1**（S19 验签、S10 平台 TLS） | S10、S19 |
| cgroup | `ls /sys/fs/cgroup` | **空目录**；内核 `CONFIG_CGROUPS=y` 但所有子系统（cpu/pids/devices…）全部未启用 | **cgroup 不可用** → 红线③成立一半；媒体隔离用 `nice`/`setpriority` 弱隔离，并在文档明确"进程级卡死由看门狗兜底" | S16 资源隔离 |
| netfilter | `which nft iptables` | 有 `iptables`/`ip6tables` + `libmnl`；无 nft；内核 XTABLES/MASQUERADE 可用，大量 match 模块被裁 | 策略路由/防火墙用 **iptables + `ip rule`**（内核 `IPV6_MULTIPLE_TABLES` 开，IPv4 多表需 S12 实测确认） | S12 |
| gpsd | `which gpsd` | 无 | S07 **自写最小 NMEA 解析**（`$GNRMC`/`$GNGGA`），原计划本就有此备选 | S07 ✅ |

### 2.7 存储

| 检查项 | 命令 | 实际结果 | 结论 | 影响任务 |
|---|---|---|---|---|
| 分区与容量 | `df -h` | eMMC 29.7GiB（mmcblk0）；`/`=mmcblk0p5 **2.0G ext4 rw**（余 1.5G）；`/mnt/UDISK`=mmcblk0p10 **27G ext4 rw**（余 26G）；tmpfs /tmp 111M | 双分区布局清晰：应用与配置放 root 分区，**大文件（录像/数据库/抓图）一律放 `/mnt/UDISK`** | S13、S16、S19 |
| `/data` 可写 | 脚本自动探测 | **NO SUCH DIRECTORY**（根目录无 /data） | 架构文档 §10 的 `/data/ivsbox/*` 路径需调整：配置/队列/数据库 → `/opt/ivsbox/` 或 `/mnt/UDISK/ivsbox/`（回填架构差异记录，见 §3.1） | S13 |
| `/opt` 可写 | 脚本自动探测 | **OK**（位于 root 分区，余 1.5G） | 双版本 OTA 空间足够（应用单版本仅几十 KB~几 MB） | S19 ✅ |
| SD 卡 | `df -h`、`cat /proc/partitions` | 未检测到 SD（仅 mmcblk0）；`/mnt/UDISK` 可承担媒体角色 | 媒体落盘点改为 `/mnt/UDISK/ivsbox/media/`；插卡热插拔逻辑降级为可选 | S16 |
| 内存总量 | `head -8 /proc/meminfo` | `MemTotal: 225344 kB`，`MemAvailable: ~174 MB`，无 swap | **G1 资源预算上限 ~220MB**；四进程（d/web/media/updater）+ lighttpd + postgresql 共存需精打细算 | **G1** |

**实测内存/资源数据填到** `docs/resource-budget.md`（缺口 G1）——**待建**（新建文档需按规则 7 第 2 条申请）。

### 2.8 盘点中发现的其它事实（原表未列，对选型有影响）

| 事实 | 证据 | 影响 |
|---|---|---|
| `lighttpd` 已随镜像安装并在启动项（`S50lighttpd`） | `/etc/init.d` 清单 | S17 不需要 uhttpd，直接复用 lighttpd（FastCGI/反代能力现成） |
| `postgresql` 已在启动项（`S50postgresql`） | 同上 | 已有一个数据库服务占用资源；S13/S14 选用 SQLite 时需评估二者共存，或确认 postgresql 可禁用 |
| CAN×2（`can0`/`can1`，DOWN 状态） | `ip -o link`、`/sys/class/net` | 运维箱对接 CAN 外设的硬件基础已有 |
| ADB 守护在跑（`S50adb_start`） | 同上 | 量产前应关闭（攻击面） |
| telnetd 在启动项（`S50telnet`） | 同上 | 同上，安全审查项 |
| 板载时间从 1970 起（RTC 未电池备份或未同步） | `date` 输出 `Jan 1 10:19 1970`、`/proc/uptime` | 墙钟完全不可信；S02 单调时钟纪律 + NTP/平台对时必须尽早做 |
| 内核由另一套工具链编译（Linaro GCC 5.3） | `/proc/version` | 正常现象；应用工具链（7.3.1 + glibc 2.25）与内核 ABI 兼容，实测已验证 |

---

## 3. 三条红线：判定结果

| 红线 | 判据 | 实测判定 | 后果与决策 |
|---|---|---|---|
| **① 没有 ffmpeg / libavformat** | `which ffmpeg`=NO 且 libav* 为空 | **触发**（两者皆无） | 选备选②：**GStreamer 路线**（`gst-launch-1.0` 在板）。S16 开工前先补查 `gst-inspect-1.0` 插件清单；若缺关键插件（matroskamux 等）则退回备选①交叉编译 FFmpeg。**S16 前置补查项** |
| **② `/opt` 只读或容量不足** | 写探测 FAIL 或空间不足 | **不触发**（`/opt` 可写，余 1.5G，应用双版本空间充裕） | 双版本放 `/opt/ivsbox/releases/`；媒体等大文件改放 `/mnt/UDISK`（27G） |
| **③ 没有 cgroup 或没有 /dev/watchdog** | cgroup 空且 watchdog 不存在 | **半触发**：watchdog ✅ 有；cgroup ❌ 无 | 选备选①：`nice`/`setpriority` 弱隔离 + 硬件看门狗兜底。原备选②"procd respawn"不可用（无 procd），改为 SysV init 脚本 + 自管监控进程，卡死恢复由 S18 看门狗负责 |

### 3.1 由盘点引出的架构差异记录（回填架构文档时统一处理）

1. `/data/ivsbox/*` → 本板无 `/data`：配置与队列改 `/opt/ivsbox/`，数据库与媒体改 `/mnt/UDISK/ivsbox/`（架构文档 §10、§11.2 路径映射）。
2. 日志体系 logd → syslogd（架构文档 §8 日志落点）。
3. 进程看护 procd respawn → SysV init + S18 自管（架构文档 §4.2、缺口 G8 的前提变了）。
4. 交付格式 ipk → tarball + 安装脚本（架构文档 §13）。
5. Web 服务器 uhttpd → lighttpd（架构文档 §9.1）。

---

## 4. 主机侧要单独确认的（不在板上跑）——已全部确认（2026-09-23）

| 检查项 | 命令 | 实际结果 | 说明 |
|---|---|---|---|
| cmake | `cmake --version` | **3.22.1**（编译 VM 上，2026-09-23 安装，见 `docs/环境搭建/cmake安装步骤.md`） | 满足工程 ≥3.16 要求 |
| SDK 根目录 | `echo $TINA_SDK_ROOT` | **不适用**——天嵌交付的是独立工具链包（`/opt/EmbedSky/Tina`），无完整 Tina SDK | `tina-arm.cmake` 已适配：`TINA_SDK_ROOT` 降级为可选路径 |
| 工具链前缀 | `ls /opt/EmbedSky/Tina/bin/` | **`arm-linux-gnueabi-`**（gcc 7.3.1 Linaro，软浮点，`--with-arch=armv7-a`） | 已固化为 `tina-arm.cmake` 默认值 |
| sysroot | 工具链 `-print-sysroot` | **`/opt/EmbedSky/Tina/arm-buildroot-linux-gnueabi/sysroot`**（glibc 2.25，与板端一致） | 已固化为 `tina-arm.cmake` 默认值 `IVSBOX_SYSROOT` |

---

## 5. 主机环境准备（S01 的第一步）——已完成

编译 VM（天嵌 TQT113 虚拟机，Ubuntu 22.04.2）已完成环境搭建并实测打通：

- cmake 3.22.1（apt 安装，步骤存档于 `docs/环境搭建/cmake安装步骤.md`）；
- 交叉工具链 `/opt/EmbedSky/Tina`（`arm-linux-gnueabi-`，PATH 已含）；
- `ivsboxd` ARM 构建 + 板端运行验证通过（2026-09-23，S01 验收）。

Windows 宿主机角色已调整为：串口控制台（COM4/CH340，115200）+ 代码仓库工作副本；
单测/交叉编译均可在 VM 内完成（`ssh tqt113`）。

---

## 6. 完成标志

- [x] `bsp-survey.txt` 已回传并贴进本文档 §2（2026-09-23，856 行输出已消化，关键实测值入表）
- [x] §2 各表"结论"列已填满（仅 2.2 两项硬件待确认项如实标注"待硬件确认"）
- [x] §3 三条红线全部确认（①触发→选 GStreamer 路线；②不触发；③半触发→nice 弱隔离 + 看门狗）
- [x] §4 主机侧四项已确认（cmake 3.22.1 / 独立工具链 / `arm-linux-gnueabi-` / sysroot glibc 2.25）
- [x] 主机已装 cmake 且 `cmake --version` 可执行（编译 VM 上 3.22.1）

完成本清单后，S01 才具备真正开工的条件。——S01 已于 2026-09-23 验收通过（见功能开发列表 §10）。

---

## 7. 实现记录

| 功能 | 状态 | 完成时间 | 说明 |
|---|---|---|---|
| 板端 BSP 盘点（bsp-survey.sh） | 已实现 | 2026-09-23 17:50 | 856 行输出，rootfs/串口/网络/看门狗/存储/中间件全覆盖 |
| §2 盘点结论回填 | 已实现 | 2026-09-23 17:50 | 7 张表 + 2.8 补充事实表 |
| §3 三条红线判定 + §3.1 架构差异记录 | 已实现 | 2026-09-23 17:50 | 5 项架构差异待回写架构文档 |
| §4/§5 主机侧确认 + §6 完成标志 | 已实现 | 2026-09-23 17:50 | S01 前置全部闭环 |
| 2.2 板间链路口 / GPS 口 / 460800 可行性 | 未实现 | — | 待硬件接通后确认 |
| S16 前置：GStreamer 插件清点 | 未实现 | — | `gst-inspect-1.0` 待板端执行 |
