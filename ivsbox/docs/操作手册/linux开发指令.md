# Linux 编译环境自检指令（在编译 VM 中按顺序执行）

> 用途：在编译 VM（`192.168.2.115`，用户 `embedsky`）中逐条执行，确认「主机编译 / ARM 交叉编译 / 测试」三类构建链路可用。
> 适用时机：第一次拿到 VM、重装或换 VM、换工具链、怀疑构建链某一段坏了。
> 构建口径：纯 GNU Make（架构文档 §15）。裸 `make` = Host Debug，`make arm` = 板端交叉产物。
> 编制日期：2026-09-24
> 修订：2026-09-24 16:55 —— 补 §0.5「首次克隆」（原文档从 §1 直接 `cd ~/work/T113_S4`，干净 VM 上会当场失败）；修正 §3 与 §9 的 `-dumpmachine` 期望值（实测为 `arm-linux-gnueabi`，原文误作 `arm-buildroot-linux-gnueabi`）。
> 关联：`AGENTS.md` 规则 1~8；《功能开发计划.md》M1-S1；《日常开发闭环流程.md》

---

## 0. 一句话

从 Windows 侧 ssh 进 VM，**新 VM 先做 §0.5（只做一次）**，然后按 §1~§7 **顺序**执行；每节都有「期望输出」，任一条不符就停在那一节排查，不要往下走。

```sh
# Windows 侧入口（其余命令都在 VM 内执行）
ssh tqt113
```

---

## 0.5 首次克隆（仅新 VM / 重装 / 换 VM 时执行一次）

`~/work/T113_S4` 是 VM 侧的**常驻工作副本**，§1 起的全部命令都假设它已经存在。新拿到一台 VM 时它不会自动出现，**必须先做这一节**；若 `ls -d ~/work/T113_S4` 已有输出（例如我 2026-09-24 16:48 已克隆好的那台），直接跳到 §1。

```sh
ls -d ~/work/T113_S4 2>/dev/null || {
  mkdir -p ~/work
  cd ~/work
  git clone https://github.com/ppp1234lll/T113_S4
}

cd ~/work/T113_S4
git log --oneline -1
git status -sb
```

期望：`git log --oneline -1` 与本机 main 的 HEAD 一致（刚克隆即最新）；`git status -sb` 只有一行 `## main...origin/main`（干净）。

说明：

- **VM 可直连 GitHub，无需代理**（2026-09-24 实测，`git ls-remote` 直通）。若后续网络策略变化导致克隆失败，再把仓库从 Windows 侧打包 `scp` 过来，**落到同一路径** `~/work/T113_S4`。
- 仓库约 1.1 MB，克隆很快；工具链、`make`、`gcc` 都在镜像里，克隆后即可直接构建。
- 目录名必须保持 `T113_S4` —— §1~§7 的相对路径都按 `~/work/T113_S4/ivsbox` 写死，改名会让后面的命令全部失效。

---

## 1. 登录与代码基线对齐

```sh
ssh tqt113                                        # Windows 侧执行，之后全部在 VM 内

cd ~/work/T113_S4
git fetch origin
git checkout main
git pull --ff-only
git log --oneline -1
git status -sb
```

期望：`git log --oneline -1` 与本机 main 的 HEAD 一致；`git status -sb` 只有一行 `## main...origin/main`（干净）。

---

## 2. 主机基础环境

```sh
uname -a
lsb_release -ds            # 期望 Ubuntu 22.04.x LTS
nproc
free -m | head -2
df -h ~ | tail -1          # 期望 build/ 落盘有 ≥ 2GB 余量
```

```sh
git --version
make --version | head -1   # 期望 GNU Make 4.x
gcc --version  | head -1   # 期望主机 gcc 11.x
```

---

## 3. 交叉工具链自检

```sh
command -v arm-linux-gnueabi-gcc
arm-linux-gnueabi-gcc -v 2>&1 | tail -1
arm-linux-gnueabi-ar  --version | head -1
arm-linux-gnueabi-gcc -print-sysroot
arm-linux-gnueabi-gcc -dumpmachine
```

期望：
- `arm-linux-gnueabi-gcc` 已在 `PATH` 内（天嵌工具链 `/opt/EmbedSky/Tina/bin`），无需额外配 PATH。
- `-dumpmachine` → `arm-linux-gnueabi`（**2026-09-24 实测值**；不要写成 `arm-buildroot-linux-gnueabi` —— 后者是 sysroot 目录名的一部分，不是 `-dumpmachine` 的输出）。
- `-print-sysroot` 输出一个**存在**的目录，记为 `$SYSROOT`（下一节用到）；实测为 `/opt/EmbedSky/Tina/arm-buildroot-linux-gnueabi/sysroot`。

```sh
SYSROOT=$(arm-linux-gnueabi-gcc -print-sysroot)
echo "$SYSROOT"
ls -d "$SYSROOT"
ls "$SYSROOT/lib/ld-linux"* 2>/dev/null || ls "$SYSROOT/lib/ld-"*
```

期望：能看到动态链接器（`ld-2.x.so`）。**记下这个版本号**——必须与板端 `/lib/ld-*.so` 指向的版本一致（现网为 glibc 2.25），否则交叉产物上板必报 `not found`。

---

## 4. 后续依赖库预检（M1 之后要用）

```sh
# sqlite3（M1-S9 用，链接 -lsqlite3）
ls "$SYSROOT/usr/include/sqlite3.h" "$SYSROOT/usr/lib/libsqlite3"* 2>/dev/null

# zlib（M1-S3 CRC 用，链接 -lz）
ls "$SYSROOT/usr/include/zlib.h" "$SYSROOT/usr/lib/libz"* 2>/dev/null

# 主机侧同款库（Host 单测要能编过）
ls /usr/include/sqlite3.h /usr/include/zlib.h 2>/dev/null
```

期望：头文件与 `lib*.so`/`lib*.a` 都在。`sqlite3.h` 缺失不会让当前构建失败（S1 尚未用到），但**要登记进"待安装库清单"**，别拖到 S9 才补。

---

## 5. Host 构建与单测（先绿再交叉）

```sh
cd ~/work/T113_S4/ivsbox

make clean                 # 先清，避免旧产物掩盖问题
make                       # = make host，Host Debug
ls -l build/host/ivsboxd
./build/host/ivsboxd ; echo HOST_RUN_EXIT=$?
```

期望：`build/host/ivsboxd` 存在且可执行，运行退出码 `HOST_RUN_EXIT=0`；当前 S1 阶段的输出为 `hello, ivsbox 1.0.0`。

```sh
make help
```

期望：打印 `all / host / arm / test / asan / fuzz / analyze / package / clean / help` 目标清单与可用变量。

```sh
make test
```

期望：逐个 `RUN build/host/tests/unit/...`；当前步骤若尚无测试用例，报告 0 用例即视为通过。**任一条用例非零退出即失败，不得进入 §6。**

```sh
make asan                  # 可选但建议：ASan/UBSan 下再跑一遍
```

---

## 6. ARM 交叉构建与产物校验

```sh
cd ~/work/T113_S4/ivsbox

SYSROOT=$(arm-linux-gnueabi-gcc -print-sysroot)
make clean
make arm SYSROOT="$SYSROOT"
ls -l build/arm/ivsboxd
```

```sh
file build/arm/ivsboxd
```

期望：`ELF 32-bit LSB executable, ARM, EABI5 ...`，`interpreter /lib/ld-linux.so.3`。

- 若显示 `x86-64` → 交叉编译没生效，检查 `mk/toolchain-tina-arm.mk` 是否被 `TARGET=arm` 走到（`make arm` 是否被误写成裸 `make`）。
- 若报 `arm-linux-gnueabi-gcc: not found` → §3 未通过。

```sh
arm-linux-gnueabi-readelf -d build/arm/ivsboxd | grep -E 'NEEDED|interpreter'
arm-linux-gnueabi-size   build/arm/ivsboxd
```

期望：`NEEDED` 只列 `libc.so.6`（`libivcore` 仅依赖 libc，这是依赖方向的硬约束）；`--no-undefined` 已保证不存在未解析符号。

```sh
# 反向确认：产物确实是 ARM 而不是被 Host 产物顶替
ls -l build/host/ivsboxd build/arm/ivsboxd
```

---

## 7. 收尾（规则 6）

```sh
cd ~/work/T113_S4
git status -sb
```

期望：干净——`build/` 已被 `ivsbox/.gitignore` 排除，不应出现散落的 `.o`。

```sh
# 需要时彻底清干净再离开
cd ~/work/T113_S4/ivsbox && make clean
```

---

## 8. 一键顺序版（照抄，逐段粘贴）

```sh
# ── 0. Windows 侧 ────────────────────────────────────────
# ssh tqt113

# ── 0.5 首次克隆（仅新 VM；已有副本则整段跳过）───────────
ls -d ~/work/T113_S4 >/dev/null 2>&1 || {
  mkdir -p ~/work && cd ~/work && git clone https://github.com/ppp1234lll/T113_S4
}

# ── 1. 基线 ─────────────────────────────────────────────
cd ~/work/T113_S4 && git fetch origin && git checkout main && git pull --ff-only
git log --oneline -1 && git status -sb

# ── 2. 主机基础环境 ──────────────────────────────────────
uname -a; lsb_release -ds; nproc; df -h ~ | tail -1
git --version; make --version | head -1; gcc --version | head -1

# ── 3. 交叉工具链 ───────────────────────────────────────
command -v arm-linux-gnueabi-gcc
arm-linux-gnueabi-gcc -v 2>&1 | tail -1
arm-linux-gnueabi-gcc -dumpmachine
SYSROOT=$(arm-linux-gnueabi-gcc -print-sysroot); echo "$SYSROOT"; ls "$SYSROOT/lib/ld-"*

# ── 4. 依赖库预检 ───────────────────────────────────────
ls "$SYSROOT/usr/include/sqlite3.h" "$SYSROOT/usr/include/zlib.h" 2>/dev/null
ls /usr/include/sqlite3.h /usr/include/zlib.h 2>/dev/null

# ── 5. Host 构建 + 单测 ─────────────────────────────────
cd ~/work/T113_S4/ivsbox
make clean && make && ./build/host/ivsboxd; echo HOST_RUN_EXIT=$?
make help
make test
make asan

# ── 6. ARM 交叉构建 + 产物校验 ──────────────────────────
SYSROOT=$(arm-linux-gnueabi-gcc -print-sysroot)
make clean && make arm SYSROOT="$SYSROOT"
file build/arm/ivsboxd
arm-linux-gnueabi-readelf -d build/arm/ivsboxd | grep -E 'NEEDED|interpreter'
arm-linux-gnueabi-size   build/arm/ivsboxd

# ── 7. 收尾 ─────────────────────────────────────────────
cd ~/work/T113_S4 && git status -sb
```

---

## 9. 结果记录表（执行后填写）

| # | 检查项 | 命令 | 期望 | 实测 | 结论 |
|---|---|---|---|---|---|
| 0 | SSH 免密 | `ssh tqt113 "echo ok"` | `ok` | | |
| 0.5 | 工作副本就位 | `ls -d ~/work/T113_S4` | 存在（新 VM 先做 §0.5 克隆） | | |
| 1 | 基线一致 | `git log --oneline -1` | 与本机 main 一致 | | |
| 2 | 主机工具 | `make --version` / `gcc --version` | 有输出 | | |
| 3 | 交叉工具链 | `arm-linux-gnueabi-gcc -dumpmachine` | `arm-linux-gnueabi` | | |
| 4 | sysroot 动态链接器 | `ls $SYSROOT/lib/ld-*` | 存在，版本与板端一致 | | |
| 5 | Host 构建 | `make` | `build/host/ivsboxd` | | |
| 6 | Host 运行 | `./build/host/ivsboxd` | 退出码 0 | | |
| 7 | 单测 | `make test` | 全绿 | | |
| 8 | ARM 构建 | `make arm` | `build/arm/ivsboxd` | | |
| 9 | 产物架构 | `file build/arm/ivsboxd` | ARM EABI5 | | |
| 10 | 依赖闭合 | `readelf -d` | `NEEDED` 仅 libc | | |
| 11 | 工作区 | `git status -sb` | 干净 | | |

---

## 10. 常见故障速查

| 现象 | 大概率原因 | 处理 |
|---|---|---|
| `ssh tqt113` 超时 | VM 没开机 / 不同网段 / IP 变了 | Windows 侧 `ping 192.168.2.115`；IP 变了改 `~/.ssh/config` |
| `cd: /home/embedsky/work/T113_S4: 没有那个文件或目录` | VM 上还没建工作副本（新 VM / 重装后） | 按 **§0.5** 克隆一次即可；之后日常只需 §1 的 `git pull --ff-only` |
| `Host key verification failed` | VM 重装或 IP 复用，指纹变了 | 删 `known_hosts` 旧条目后重连 |
| `arm-linux-gnueabi-gcc: not found` | 工具链不在 PATH / 换过 VM | 查 `/opt/EmbedSky/Tina/bin`；按环境搭建文档补 |
| `make: command not found` | shell 命令哈希未刷新 | `hash -r` 或重开终端 |
| `file` 显示 `x86-64` | 交叉编译未生效 | 确认执行的是 `make arm`（不是裸 `make`）；确认 `mk/toolchain-tina-arm.mk` 存在 |
| 编译报 `-Werror` 失败 | 默认告警即错误 | 按提示改代码；**不要**用关 `-Werror` 绕 |
| 链接报 undefined reference | 目标未定义 / 库链接顺序错 | 依赖方向必须 `libivcore → libivhal → libivmodules → ivsboxd`，顺序在 `Makefile` 写死 |
| `git fetch/push` 失败 | 本机走 `127.0.0.1` 代理且代理没起 | `git config --get http.proxy`；起代理或临时绕过；**失败必须如实上报**（规则 3.7） |
| 板端运行报 `not found` | sysroot 与板端 glibc 不一致 | 比对 `$SYSROOT/lib/ld-*` 与板端 `/lib/ld-*.so` |

---

## 11. 关联文档

| 文档 | 用途 |
|---|---|
| `AGENTS.md` | 规则 1~8（回写、记录、提交、代码说明、官方资料优先、清理、多 AI 共用、会话隔离） |
| `ivsbox/docs/功能开发计划.md` | M1-S1 的三要素与验收口径 |
| `ivsbox/docs/操作手册/日常开发闭环流程.md` | 完整闭环：VM 编译 → 上板 → COM4 串口验证 |
| `T113运维终端-系统架构设计.md` §15 | 构建体系与库分层的权威依据 |
