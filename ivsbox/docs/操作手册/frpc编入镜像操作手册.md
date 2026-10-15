# frpc 编入镜像操作手册

> **这份手册只干一件事：把 frpc 二进制塞进 T113 的 rootfs 并打成新固件。**
> 不涉及通道怎么开、配置怎么写、运维侧怎么连（那些属 M8 使用侧，另见对应文档）。
>
> 适用：编译 VM `tqt113`（`embedsky` 用户）+ 天嵌 T113-S4 Tina Linux SDK。
> **全文命令一律用绝对路径，不使用 shell 变量** —— 变量跨终端/跨会话会丢，曾因此把文件误写到 `/usr/bin`。

---

## 1. 先记住这四条绝对路径

| 用途 | 绝对路径 |
|---|---|
| SDK 根 | `/opt/Embedsky/mount/t113_tina` |
| **放 frpc 的地方（rootfs overlay）** | `/opt/Embedsky/mount/t113_tina/buildroot/buildroot-201902/board/embedsky/tqt113-s3/rootfs_overlay/usr/bin/frpc` |
| 构建产物根 | `/opt/Embedsky/mount/t113_tina/out/t113_s4/evb1_auto/buildroot` |
| 最终固件 | `/opt/Embedsky/mount/t113_tina/out/t113_s4/evb1_auto/buildroot/t113_s4_linux_evb1_auto_uart0.img` |

> ⚠ **芯片是 `t113_s4`，但板级目录叫 `tqt113-s3` —— 这不是笔误，别改成 s4。**
> SDK 里 `buildroot/buildroot-201902/board/embedsky/` 下**只有 `tqt113-s3` 一个目录**，S4 复用它是天嵌的设计。
> 本次构建的有效配置 `/opt/Embedsky/mount/t113_tina/out/t113_s4/evb1_auto/buildroot/buildroot/.config`
> 里就明写 `BR2_ROOTFS_OVERLAY="board/embedsky/tqt113-s3/rootfs_overlay"`；天嵌官方一键脚本 `auto_build.sh`
> 里也是这个路径。

---

## 2. Step 1 —— 取 frpc（版本锁死 v0.68.0 / linux_arm）

### 2.1 为什么必须是这一版

| 项 | 值 |
|---|---|
| 版本 | **0.68.0** —— 必须与 frps、运维侧 visitor 同版本族 |
| 资产名 | `frp_0.68.0_linux_arm.tar.gz`（**不是** `_arm_hf`） |
| tarball SHA256 | `3944595c77cc8242a9d94a840b59966f018ddaeb3e93e4301d803e7c7ed31b8e` |
| 解出的 `frpc` 大小 | `15073464` 字节 |
| 解出的 `frpc` SHA256 | `9040d099f4cd992cd31f3d0057ec29cab2b388c74f1b59b9f7f65982ba571c71` |

选型依据（已实测）：`frpc` 是 `ELF 32-bit LSB, ARM, EABI5, statically linked`，含 VFP 指令；
板端内核配置 `/opt/Embedsky/mount/t113_tina/out/t113_s4/kernel/build/.config` 里有
`CONFIG_VFP=y` / `CONFIG_VFPv3=y` / `CONFIG_NEON=y`，**能跑**。
`linux_arm` 是静态 Go 二进制，不涉及 C 互操作，浮点 ABI 差异无影响；仅当真跑不起来才换 `_arm_hf`。

### 2.2 下载并验签（在编译 VM 上）

```bash
# 1) 切到临时目录
cd /tmp

# 2) 下载 v0.68.0 linux_arm 发布包（--retry 3：网络失败自动重试 3 次）
curl -L --retry 3 -O https://github.com/fatedier/frp/releases/download/v0.68.0/frp_0.68.0_linux_arm.tar.gz

# 3) 校验 tarball 的 SHA256，期望输出 frp_0.68.0_linux_arm.tar.gz: OK
echo '3944595c77cc8242a9d94a840b59966f018ddaeb3e93e4301d803e7c7ed31b8e  frp_0.68.0_linux_arm.tar.gz' | sha256sum -c -

# 4) 解包
tar xzf /tmp/frp_0.68.0_linux_arm.tar.gz
```

期望输出 `frp_0.68.0_linux_arm.tar.gz: OK`。
**校验不通过就不要往下走** —— 这是个要进固件的可执行文件，来源必须对得上。

---

## 3. Step 2 —— 放进 overlay

**放 `usr/bin`，不要放 `opt`**：天嵌一键脚本 `auto_build.sh` 会 `rm -rf .../rootfs_overlay/opt`
再重灌 PDA/Qt 内容，**不碰 `usr/bin`**。放 `opt` 会被静默清掉。

```bash
# 以 0755（rwxr-xr-x）权限把解出的 frpc 装进 rootfs overlay 的 usr/bin
install -m 0755 /tmp/frp_0.68.0_linux_arm/frpc /opt/Embedsky/mount/t113_tina/buildroot/buildroot-201902/board/embedsky/tqt113-s3/rootfs_overlay/usr/bin/frpc
```

立刻自检（三条都要对）：

```bash
# 1) 权限与大小，期望 -rwxr-xr-x / 15073464
ls -la /opt/Embedsky/mount/t113_tina/buildroot/buildroot-201902/board/embedsky/tqt113-s3/rootfs_overlay/usr/bin/frpc

# 2) 文件类型，期望 ELF 32-bit LSB executable, ARM, EABI5 ... statically linked
file /opt/Embedsky/mount/t113_tina/buildroot/buildroot-201902/board/embedsky/tqt113-s3/rootfs_overlay/usr/bin/frpc

# 3) 校验和，期望 9040d099f4cd992cd31f3d0057ec29cab2b388c74f1b59b9f7f65982ba571c71
sha256sum /opt/Embedsky/mount/t113_tina/buildroot/buildroot-201902/board/embedsky/tqt113-s3/rootfs_overlay/usr/bin/frpc
```

| 检查 | 期望 |
|---|---|
| 权限/大小 | `-rwxr-xr-x` / `15073464` |
| 类型 | `ELF 32-bit LSB executable, ARM, EABI5 ... statically linked` |
| SHA256 | `9040d099f4cd992cd31f3d0057ec29cab2b388c74f1b59b9f7f65982ba571c71` |

---

## 4. Step 3 —— 重新构建 rootfs + 打包

```bash
# 1) 进入 SDK 根目录
cd /opt/Embedsky/mount/t113_tina

# 2) 初始化构建环境（build.sh 依赖它导出的变量与函数）
source build/envsetup.sh

# 3) 后台执行全量构建；无参 ./build.sh 才会同时刷新 rootfs.ext4。
#    构建结束后把退出码写进日志，出现 BUILD_RC=0 即构建成功
( ./build.sh; echo "BUILD_RC=$?" ) > /tmp/build_frpc.log 2>&1 &

# 4) 实时跟踪构建日志；Ctrl+C 只退出 tail，不影响后台构建
tail -f /tmp/build_frpc.log

# 5) 构建成功后（日志里已出现 BUILD_RC=0）再后台执行打包，日志追加进同一文件
( ./build.sh pack; echo "PACK_RC=$?" ) >> /tmp/build_frpc.log 2>&1 &

# 6) 跟踪打包日志，出现 PACK_RC=0 即打包成功
tail -f /tmp/build_frpc.log
```

看到 `BUILD_RC=0` 与 `PACK_RC=0` 即成功。
1 核 VM 全量约 20~40 分钟 —— kernel / u-boot 都是增量，主要时间花在 rootfs。

### ⚠ 这一步有个坑：**不要只用 `./build.sh buildroot_rootfs`**

| 命令 | 重编 `target/` | 刷新 `rootfs.ext4`（`make_ext4`） | 可用 |
|---|---|---|---|
| `./build.sh buildroot_rootfs` | ✅ | **❌** | ❌ 后面 pack 打出来是旧 rootfs，frpc 不在里面 |
| `./build.sh`（无参） | ✅ | ✅ | ✅ |
| `./auto_build.sh t113_s4 buildroot` | ✅ | ✅ | ✅ 但会顺带清空重灌 `rootfs_overlay/opt` |

原因：`buildroot_rootfs` 在 `build/mkcommon.sh` 里只派发到 `build_buildroot_rootfs`，
**不包含 `pack_buildroot_rootfs`**；执行 `make_ext4` 的是后者，而它只被
`build_linuxdev`（＝ `./build.sh` 无参）经由 `build_rootfs` 调用。

---

## 5. Step 4 —— 验证真进镜像了

```bash
# 5a. overlay 是否被应用进 target
ls -la /opt/Embedsky/mount/t113_tina/out/t113_s4/evb1_auto/buildroot/buildroot/target/usr/bin/frpc

# 5b. 是否真进了 rootfs.ext4（免 root，e2fsprogs 的 debugfs 可直接读）
debugfs -R 'stat /usr/bin/frpc' /opt/Embedsky/mount/t113_tina/out/t113_s4/evb1_auto/buildroot/rootfs.ext4
# 期望：Type: regular   Mode: 0755   Size: 15073464

# 5c. 固件时间戳刷新了
ls -la /opt/Embedsky/mount/t113_tina/out/t113_s4/evb1_auto/buildroot/t113_s4_linux_evb1_auto_uart0.img
```

5b 是最硬的证据 —— 直接读镜像里的文件系统，绕开"构建脚本说成功了"。

### 容量不必担心

`/opt/Embedsky/mount/t113_tina/out/t113_s4/evb1_auto/pack_out/sys_partition.fex` 里 rootfs 分区
`size = 4194304`，**单位是 512 字节扇区 ⇒ 2 GiB**；当前 `rootfs.ext4` 仅约 560 MiB。
加 15 MB 余量充足。

---

## 6. Step 5 —— 刷机后在板端确认

用 PhoenixSuit / LiveSuit 刷入
`/opt/Embedsky/mount/t113_tina/out/t113_s4/evb1_auto/buildroot/t113_s4_linux_evb1_auto_uart0.img`。

板端执行：

```bash
# 查看版本，期望输出 0.68.0
/usr/bin/frpc -v

# 查看文件属性，期望 -rwxr-xr-x / 15073464
ls -la /usr/bin/frpc
```

期望 `frpc -v` 输出 `0.68.0`。

---

## 7. 排障速查（只管"编进镜像"这条链）

| 现象 | 原因 | 处理 |
|---|---|---|
| `debugfs ... /usr/bin/frpc` 报 not found | rootfs 没刷新 | 你很可能用了 `./build.sh buildroot_rootfs` —— 改回 `./build.sh` |
| `target/usr/bin/frpc` 有、`rootfs.ext4` 里没有 | 缺 `pack_buildroot_rootfs` | 同上；或没跑 `./build.sh pack` |
| overlay 里的 frpc 不见了 | 放到了 `rootfs_overlay/opt` | 重新放到 `usr/bin`（`auto_build.sh` 会清 `opt`） |
| `install: 无法创建普通文件 '/usr/bin/frpc': 权限不够` | 路径塌成了 VM 自己的 `/usr/bin`（多半是用了空变量 `"$OV/..."`） | 用本手册的**绝对路径**；**不要加 `sudo`**，那是往 VM 系统目录装，与板子无关 |
| `file` 显示 x86-64 / 别的架构 | 下错包 | 必须 `frp_0.68.0_linux_arm.tar.gz` |
| 板端 `frpc -v` 报 `cannot execute binary file` / SIGILL | 极少数情况浮点兼容问题 | 换 `frp_0.68.0_linux_arm_hf.tar.gz` 重做 Step 2~5 |

---

## 8. 收尾清理（规则 6）

```bash
# 删除解包目录、下载包与构建日志，不留临时文件
rm -rf /tmp/frp_0.68.0_linux_arm /tmp/frp_0.68.0_linux_arm.tar.gz /tmp/build_frpc.log
```

---

## 实现记录

| 功能 | 状态 | 完成时间 | 说明 |
|---|---|---|---|
| 命令拆分与注释（全部代码块） | 已实现 | 2026-10-15 16:58 | 依用户要求把复合命令（`&&` 串联、`{ ...; }` 分组）拆成单条指令，并逐条加注释说明预期输出；`.md` 与 HTML 副本同步 |
| frpc 编入镜像操作手册 | 已实现 | 2026-10-15 16:42 | 依用户要求新建：把 frpc 二进制编进 T113 rootfs 并打包的完整链路；全文绝对路径、不使用变量 |
| overlay 落点核实（`board/embedsky/tqt113-s3`） | 已核实 | 2026-10-15 16:35 | 芯片 `t113_s4` 但板级目录名为 `tqt113-s3`（天嵌设计，SDK 内无 s4 板目录） |
| 构建路径坑核实（`buildroot_rootfs` 不刷 `rootfs.ext4`） | 已核实 | 2026-10-15 16:25 | `mkcommon.sh` 派发链：`buildroot_rootfs` → `build_buildroot_rootfs`（无 `make_ext4`） |
| frpc 版本与完整性核实（v0.68.0 / linux_arm） | 已核实 | 2026-10-15 16:20 | 与官方 `frp_sha256_checksums.txt` 逐字节一致；ELF 架构与板端内核 VFP 能力已交叉核对 |
