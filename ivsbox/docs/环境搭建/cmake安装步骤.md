# TQT113 编译环境：cmake 安装步骤

> 目标机：天嵌 TQT113 配套的 Ubuntu 22.04 编译虚拟机（实测环境：192.168.2.115，用户 `embedsky`）
> 适用工程：`ivsbox`（已改为纯 Makefile；本文保留作历史参考，当前构建不再使用 cmake）
> 编制日期：2026-09-23

---

## 0. 结论先说

**用 Ubuntu 官方源 apt 装即可，版本 3.22.1，满足工程要求。**

不需要 Kitware 官方源、不需要 snap、不需要源码编译。理由见 §1 的实测数据。

---

## 1. 环境实测记录（2026-09-23，只读探测）

| 检查项 | 命令 | 实测结果 |
|---|---|---|
| 发行版 | `lsb_release -d` | Ubuntu 22.04.2 LTS（内核 `6.8.0-138-generic`，x86_64） |
| apt 源 | `grep -rhv '^#' /etc/apt/sources.list /etc/apt/sources.list.d/` | `mirrors.tuna.tsinghua.edu.cn/ubuntu jammy/-updates/-backports` + `security.ubuntu.com` |
| cmake 可用版本 | `apt-cache policy cmake` | 候选 **3.22.1-1ubuntu1.22.04.2**（未安装） |
| 安装模拟 | `apt-get -s install cmake` | 通过；连带安装 `cmake-data`、`libjsoncpp25`、`librhash0`、`dh-elpa-helper` |
| 磁盘 | `df -h /` | 69G 总、37G 可用 |
| 外网 | `tcp` 连 `mirrors.tuna.tsinghua.edu.cn:443` | 通 |
| sudo | `sudo -n true` | **需要密码**（非免密） |
| 构建工具 | `make --version` / `ninja --version` | GNU Make 4.3 / ninja 1.8.2（后者在 `/opt/EmbedSky/Tina/bin/`） |
| 交叉工具链 | `command -v arm-linux-gnueabi-gcc` | `/opt/EmbedSky/Tina/bin/arm-linux-gnueabi-gcc`，**已在 PATH 中** |

> `PATH` 里已包含 `/opt/EmbedSky/Tina/bin`，因此 cmake 配置时能直接找到交叉编译器，
> **不需要额外配置 PATH，也不需要指定 `CMAKE_C_COMPILER` 绝对路径**。

---

## 2. 安装步骤

### 2.1 确认前提

```sh
lsb_release -d          # 应为 Ubuntu 22.04.x
df -h /                 # 根分区至少留出 1G（实际只需约 60MB）
```

### 2.2 更新软件包索引

```sh
sudo apt-get update
```

执行后会提示输入密码（本虚拟机默认用户 `embedsky`，密码按天嵌虚拟机说明；若你改过则用改后的）。

### 2.3 安装 cmake

```sh
sudo apt-get install -y cmake
```

### 2.4 验证安装

```sh
cmake --version
which cmake
```

期望输出（版本号以实际为准）：

```text
cmake version 3.22.1

CMake suite maintained and supported by Kitware (kitware.com/cmake).
/usr/bin/cmake
```

只要 `cmake version` ≥ **3.16** 即满足本工程要求。

### 2.5 顺带确认构建工具与工具链（S01 要用）

```sh
make --version
ninja --version
arm-linux-gnueabi-gcc -v 2>&1 | tail -n 1
```

期望：GNU Make 4.3 / ninja 1.8.2 / `gcc 版本 7.3.1 ... (Linaro GCC 7.3-2018.05)`。

这三条都正常，说明"cmake + 生成器 + 交叉编译器"三件套齐了，可以进入 ARM 构建。

---

## 3. 安装完成后的下一步（属 S01）

```sh
# 源码进 VM（计划走 git clone，见 docs/修改记录.md 的约定）
cd ~/work 2>/dev/null || mkdir -p ~/work && cd ~/work
git clone https://github.com/ppp1234lll/T113_S4.git
cd T113_S4/ivsbox-v5

# 配置 ARM 目标（工具链文件已按本机实测值改好，无需额外 -D 参数）
cmake -S . -B build/arm \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/tina-arm.cmake \
      -DIVSBOX_BUILD_TESTS=OFF \
      -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build build/arm -j

# 校验产物
file build/arm/ivsboxd
```

预期 `file` 输出：`ELF 32-bit LSB executable, ARM, EABI5 ...`。
若打印的是 `x86-64`，说明工具链文件没生效，检查 `-DCMAKE_TOOLCHAIN_FILE=` 是否漏写。

> **注意**：`build/` 不要落在 Windows 共享目录上，放在 VM 本地盘（如 `~/work/`），否则 `fsync`
> 与并发 IO 会变慢且容易出现异常。

---

## 4. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `sudo` 提示需要密码 | 该虚拟机 sudo 非免密（实测确认） | 输入用户密码；本次会话内多次 sudo 会有缓存 |
| `apt-get update` 慢或超时 | 走 https 清华镜像 | 先确认 `tcp` 能连 `mirrors.tuna.tsinghua.edu.cn:443`；不通再考虑换源，**不要盲目改源** |
| `cmake: command not found`（装完） | shell 命令哈希未刷新 | `hash -r` 或新开一个终端 |
| `cmake --version` 版本低于 3.16 | 装到了旧包 | 本机实测候选为 3.22.1，不会出现；若出现先查 `apt-cache policy cmake` |
| 需要比 3.22 更新的版本 | 工程当前不需要 | 暂不引入 Kitware 官方源，避免多处版本来源；确有需要时再单独评估 |

### 卸载 / 回退

```sh
sudo apt-get remove --purge cmake cmake-data
sudo apt-get autoremove -y
```

---

## 5. 参考来源

| 来源 | 用途 |
|---|---|
| 天嵌官方语雀《TQT113_Linux_应用开发手册》 | 交叉编译环境搭建方式、工具链安装位置 `/opt/EmbedSky/Tina`、虚拟机用户与密码约定 |
| 本机实测（2026-09-23，`tqt113` = 192.168.2.115） | §1 全部数据、§2.5 的 PATH 与版本 |
| Ubuntu jammy 软件源（清华镜像） | `cmake` 包版本 3.22.1 |
| CMake 官方下载页 `https://cmake.org/download/` | "apt 安装的版本足够时不必另设源"的判断依据 |

---

## 6. 执行记录

> 规则见项目根目录 `AGENTS.md` 规则 1。时间格式 `YYYY-MM-DD HH:mm`。

| 步骤 | 状态 | 完成时间 | 说明 |
|---|---|---|---|
| §1 环境只读探测 | 已实现 | 2026-09-23 16:57 | 全部数据为实测，非推测 |
| §2.2 `apt-get update` | 待执行 | — | 由使用者在 VM 内执行 |
| §2.3 `apt-get install cmake` | 待执行 | — | 需 sudo 密码 |
| §2.4 验证 `cmake --version` | 待执行 | — | 期望 ≥ 3.16 |
| §3 ARM 构建（S01 主体） | 未实现 | — | 见 `docs/IVSBox-功能开发列表.md` §10 的 S01 行 |
