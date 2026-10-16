============================================================
docs/操作手册  ——  分任务操作手册目录（怎么把代码变成板上跑起来的东西）
============================================================

【目录作用】
  存放按任务或链路细分的操作手册，讲"怎么操作"（环境、命令、验证步骤），与 docs/ 下的
  计划、记录类文档互补：任务各自的"做什么"看《功能开发计划.md》，本目录只讲"怎么落地、
  怎么看到输出"。经用户主动要求新建（规则 7 第 2 条）。

【文件说明】
  日常开发闭环流程.md  通用日常链路：Windows 编辑 → SSH 进编译 VM 交叉编译 → SCP 上板 →
                       COM4 串口调试验证；各阶段任务都走（正文命令待按 Makefile 口径重写）
  linux开发指令.md     编译 VM 内自检指令：按顺序确认主机编译 / ARM 交叉编译 / 测试三类链路可用
                      （首次拿到 VM、重装或换工具链时用）
  frpc编入镜像操作手册.md  把 frpc 二进制编进 T113 rootfs 并打包的完整链路（取包校验 → 放 rootfs_overlay
                      → ./build.sh + pack → debugfs 验证 → 刷机确认）。全文绝对路径、不用变量。
                      2026-10-15 经用户要求新建。
  frpc编入镜像操作手册.html  上者由 .md 生成的**单文件 HTML 副本**（样式内联、浅色主题、可打印、无外部依赖），
                      供阅读/打印/转发。**只作副本、不单独维护，内容一律以 .md 为准**；
                      .md 改了要重新生成。2026-10-15 经用户要求由 .md 转换而来。
  UART1设备树启用操作手册.html  启用设备树里的 UART1/GPS（`&uart1` 置 okay、补 PD21/PD22 引脚、
                      关 `&pwm7` 让出被背光占用的 PD22）→ `cpp`/`dtc` 自检 → 编译
                      （**官方口径优先：`./build.sh kernel`，设备树与内核一起编**；只改 dts 时可用
                      `dts` ＋ `bootimg` 支路，二者等效二选一）＋ `pack` → dtb 与 boot.img 双重取证
                      → 板端确认的完整链路；含三个坑：内核树那份 `board.dts` 才是编译用的、
                      dtb 装在 `boot.img` 里、官方文档路径是通用板名（`out/t113/...`）。
                      **全文命令按用户要求"全部参考官方手册来写"**：每条命令标【官方】/【补充】，
                      开头有「命令来源总览」表、结尾 §12 逐条对照官方来源（12.1 官方 8 条 /
                      12.2 补充 8 条）并写明路径适配；官方出处＝《TQT113_linux_系统开发手册》
                      §2.1.1/§2.1.5、《TQT113_linux_使用手册》§3.1.2/§3.1.5、SDK 自带脚本与
                      `tools/` 下 readme。**直接以 HTML 形态维护（无 .md 母本）**；
                      全文命令单条可执行、绝对路径、不用变量。
                      2026-10-16 经用户要求新建（同日两轮复核：先按官方口径重写 §6，再按官方口径逐条核命令）。
  镜像主机名与账号密码修改操作手册.html  改 T113 镜像的**主机名**（主线：串口提示符 `root@TQ113:/#`
                      里被框住的 `TQ113`）/ 账号 / 密码，**只此一条主线：改 overlay 里的文件 → 重建 rootfs
                      → `pack` → 刷机**。现状实测（**主机名 `TQ113`**，**只在 `etc/hostname` 与 `etc/hosts`
                      两处**，开机由 `etc/inittab:27 ::sysinit:/bin/hostname -F /etc/hostname` 生效、
                      提示符来自 `etc/profile:10 PS1='\u@\h:\w # '` 的 `\h` 动态取主机名 ⇒ 改文件即改提示符；
                      **账号 `root`、密码 `embedsky`**；串口 `::respawn:-/bin/sh` 免密直进、本手册不动它；
                      网络登录服务未自启；`BR2_TARGET_GENERIC_ROOT_PASSWD=""` 故密码不被 Buildroot 覆盖）
                      → §1 要改的 4 个文件（overlay 的 `etc/{hostname,hosts,passwd,shadow}`）
                      → **§2.1 改主机名（主任务，4 条命令：备份 → 改 `hostname` → 改 `hosts` → `grep -Hn .`
                      核对 ＋ 还原）**、§2.2 改账号/密码（**可选**，6 条命令：`sed` 在 `passwd`+`shadow`
                      两处改名 → `sed` 换哈希 → 核对）、**§2.3 收尾：必须用「无参」`./build.sh`
                      —— 不能用 `./build.sh buildroot_rootfs`！＋ `./build.sh pack` ＋ 两级验证
                      （先 `debugfs` 读 Buildroot 的 `rootfs.ext2`，再用 `grep -ao "127\.0\.1\.1..."`
                      直接搜打包输入 `rootfs.ext4` 与最终 `.img`）＋ 刷机** → §3 改任意名字/账号的
                      通用模板（改主机名 / 改密码 / 改名 / 新增 / 删除 / 核对 ＋ 三个必须记住的点 ＋
                      实测哈希对照）→ §4 排障速查 → §5 本次实测记录 → §6 逐条对照来源。
                      **含 5 个实测坑**：① 提示符里的名字是**主机名不是账号名**（去改 `passwd` 属白改）；
                      ② **只跑 `./build.sh buildroot_rootfs` 是最常见的坑** —— 它只刷新 Buildroot 自己的
                      `buildroot/buildroot/{target,images/rootfs.ext2}`，**不生成打包输入的 `rootfs.ext4`**
                      （生成它的是 `build/mkcmd.sh:2201 pack_buildroot_rootfs()`，它只被
                      `:2273 build_rootfs()` 调用，而 `build_rootfs()` 只被无参 `./build.sh` 的默认动作
                      `:2436 build_linuxdev()` 调用）⇒ `pack` 打进去的还是旧 rootfs，**烧完仍是 `TQ113`**
                      （2026-10-16 14:0x 实测复现并定位）；③ 走 `./build.sh kernel` 同样无效（压根不碰
                      rootfs）；④ 主机名要 `hostname`+`hosts` **两处同改**；
                      ⑤ `rootfs.ext4` 是 Android 稀疏镜像、`.img` 是整盘镜像，都**不能喂 `debugfs`**
                      （要读裸 ext4 用 `rootfs.ext2`）；账号部分另有
                      「双引号里粘贴带 `$` 的哈希会被 shell 展开」的坑。**本手册无官方手册对应章节（已查证），
                      故不标【官方】**，出处落到 SDK 配置文件/脚本行号（`etc/inittab:27`、`etc/profile:10`、
                      `BR2_TARGET_GENERIC_ROOT_PASSWD` 等）＋ 本机实测；直接以 HTML 形态维护（无 .md 母本）、
                      命令单条可执行、绝对路径、不用变量。
                      2026-10-16 经用户确认后登记（规则 7 第 2 条）；同日用户澄清"要改的是提示符里的
                      `TQ113`"（**那是主机名**），遂**以主机名为主线重写**并**取代原《镜像账号密码修改操作
                      手册.html》（已删）**；此前按用户"留一个方案就行"精简，删去原方案 B（板端 `passwd`）/
                      方案 C（串口加密码）/方案 D（新增账号）。
                      **2026-10-16 14:13 修正**：用户反馈"烧录镜像后还是 `TQ113`"，查明上一版 §2.3 让跑
                      `./build.sh buildroot_rootfs` 是错的（它不刷新打包输入的 `rootfs.ext4`），已改为
                      **无参 `./build.sh`**，并在 VM 上按新口径实测通过（build＋pack 共 8 分 27 秒；
                      最终 `.img` 里 `127.0.1.1 → t113`、`TQ113` 计数 0）。详见 `修改记录.md` 2026-10-16 14:13。

【约定与注意事项】
  - 本目录文档按需新建，新建须经用户确认并登记（规则 7 第 2 条）。
  - 构建口径以顶层 Makefile 为准：裸 `make` = Host Debug，`make arm` = 板端交叉产物，
    `make test` / `make asan` 驱动测试。
  - 部署口径（2026-09-30 用户定稿）：程序（bin/scripts）落 rootfs 的 /opt/ivsbox；数据
    （config/db/queue/ota/releases/secure + 熔断状态 + 日志）落 UDISK 的 /mnt/UDISK/ivsbox。
  - 涉及 T113 / 天嵌平台事实时，先查天嵌官方语雀资料并标注出处（规则 5）。

【相关文档】
  - ivsbox/docs/readme.txt（上级目录：规则 1/2/4 权威文档）
  - ivsbox/docs/功能开发计划.md（各阶段"做什么"）
  - ivsbox/docs/操作手册/日常开发闭环流程.md（本目录主手册）
  - 仓库根 AGENTS.md（规则 5 天嵌官方资料优先、规则 7 文档唯一性）
