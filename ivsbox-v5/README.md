# IVSBox 视频智能监控箱（T113 / Tina Linux）

> 项目代号 **IVSBox**　产品中文名 **视频智能监控箱**　英文全称 Intelligent Video Surveillance Box
> 目标平台：全志 T113 / Tina Linux（OpenWrt 体系）　主体语言：**C11**（不引入 Go / C++）

---

## 1. 本目录的定位

本目录是 **v5 整合版架构**对应的工程目录（骨架阶段）。

| 项 | 说明 |
|---|---|
| 架构基线 | `F:\debug\T113_TCP\T113运维终端-系统架构设计-v5-整合版.md` |
| 缺口清单 | `F:\AI设计\Workbuddy\linux\T113运维终端-v5架构缺口与风险清单.md`（G1~G19） |
| 旧脚手架 | `F:\AI设计\Workbuddy\linux\ivsbox\`（v3 版：core 层 + 38 项单测，保留备查，勿在其上继续开发） |
| 当前范围 | **只搭目录与可编译空骨架**。业务代码为空，风险项（G1~G19）推迟到第一版程序跑通后处理 |

命名约定：
- 目录 / 包名 `ivsbox`，进程名 `ivsboxd` / `ivsbox-web` / `ivsbox-media` / `ivsbox-updater` / `ivsbox-provision`
- **C 符号前缀保留 `gw_` 的历史约定已作废**：本版新增符号统一用 `ivs_` / `IVS_` 前缀（见 `include/ivsbox/`）

---

## 2. 目录结构（对应架构文档 §15）

```text
ivsbox-v5/
├── CMakeLists.txt              顶层构建：ivcore / ivhal / ivmodules / ivsboxd
├── cmake/toolchains/           tina-arm.cmake（板端交叉编译工具链）
├── include/ivsbox/             唯一的公共头目录（私有头不得放这里）
├── src/
│   ├── app/                    入口、装配、Reactor、命令路由、业务编排   §6
│   ├── core/                   事件、队列、定时器、日志、配置、状态机、CRC  §6
│   ├── hal/                    serial、netlink、watchdog、fs、clock、capability  §6
│   └── modules/                §6 的 12 个业务模块，见下表
├── media/                      ivsbox-media：RTSP / 抓图 / 录像 / 音频   §8
├── web/                        ivsbox-web：REST / 静态页 / 受控反代       §9
├── updater/                    ivsbox-updater：验签 / 安装 / 切换 / 回滚  §11
├── tools/
│   ├── provision/              ivsbox-provision：出厂写设备身份与 MAC     §13
│   ├── uart_simulator/         PC 端板间协议模拟器（M2 硬前置）
│   └── platform_simulator/     PC 端平台协议模拟器
├── tests/{unit,fuzz,integration}   §16
├── packaging/                  ipk / 分区布局
├── scripts/
│   ├── init.d/                 procd 服务脚本（三个常驻进程）
│   ├── hotplug/                4G 模组热插拔
│   ├── upgrade/                OTA 应用/回滚脚本
│   └── ci/                     构建期检查（G7 层边界检查在此落地）
└── docs/                       设计与接口冻结记录
```

### src/modules 一览（架构文档 §6）

| 模块目录 | 职责 | 对应功能项 |
|---|---|---|
| `link/` | UART 板间链路（帧解析、重同步、状态镜像） | 3 |
| `proto/` | 平台协议（表驱动命令路由、补传队列） | 4 |
| `netmgr/` | 网络检测与多 WAN 状态机 | 5 |
| `gps/` | GPS 与时间来源 | 9 |
| `probe/` | ICMP / TCP / RTSP 统一探活引擎 | 1、2 |
| `device/` | 摄像机与交换机档案（一机一档） | 14、15 |
| `snmp/` | SNMP 采集与 OID 模板 | 12、13 |
| `recovery/` | 自愈策略引擎 | 7 |
| `access/` | 门磁、门锁与审计 | 11 |
| `config/` | 配置事务与热更新 | 全部配置类 |
| `ota/` | 升级协调（实际安装由 ivsbox-updater 执行） | 6 |
| `tunnel/` | 可燃的远程访问通道 | 19、22 |

---

## 3. 构建

### 主机（单元测试 / 静态分析 / fuzz）

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host -j
ctest --test-dir build/host --output-on-failure
```

### 板端（Tina SDK 交叉工具链）

```sh
cmake -S . -B build/arm \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/tina-arm.cmake \
      -DIVSBOX_BUILD_TESTS=OFF \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/arm -j
```

产出二进制：`ivsboxd`、`ivsbox-media`、`ivsbox-web`、`ivsbox-updater`。

> **注意**：`ivsboxd` / `ivsbox-media` / `ivsbox-web` 当前是**骨架 stub**（打印版本后立即退出）。
> 在该阶段启用 `scripts/init.d` 下的 procd 服务会导致 respawn 循环，实机启用前请先完成对应阶段实现。

---

## 4. 分层与边界（架构文档 §6 依赖规则）

1. `core` 不依赖 Linux HAL，可在开发机单元测试。
2. `hal` 不包含业务策略。
3. 模块之间只经 `include/ivsbox/` 公共头互访，不得访问其它模块私有数据。
4. 业务状态只在 `app` 的 Reactor 上下文更新。
5. 第三方协议库通过适配层接入，不把第三方类型扩散到业务接口。

**状态**：以上目前只是构建组织约定，**尚无强制手段**（缺口 G7）。
预留位置：`scripts/ci/`。第一版程序跑通后再补编译期强制与 CI 检查。

---

## 5. 文件系统布局（架构文档 §10、§11.2）

| 路径 | 内容 | 唯一写入者 |
|---|---|---|
| `/opt/ivsbox/releases/<version>/` | 应用版本目录 | ivsbox-updater |
| `/opt/ivsbox/current` → `releases/<current>` | 当前版本符号链接 | ivsbox-updater |
| `/opt/ivsbox/previous` → `releases/<previous>` | 上一版本符号链接 | ivsbox-updater |
| `/data/ivsbox/db/control.db` | 设备档案、自愈审计、网络历史 | ivsboxd |
| `/data/ivsbox/db/media.db` | 录像与抓图索引 | ivsbox-media |
| `/data/ivsbox/config/` | 运行配置（按职责分文件） | ivsboxd |
| `/data/ivsbox/factory/` | 工厂身份（设备 ID / MAC） | ivsbox-provision |
| `/data/ivsbox/queue/` | 平台上报持久队列 | ivsboxd |
| `/data/ivsbox/ota/` | pending 状态与包校验信息 | ivsbox-updater |
| `/mnt/sd/ivsbox/media/` | 媒体文件 | ivsbox-media |

**SD 卡水位（§10.4）**：85% 清理最旧可删录像 → 90% 限制新任务 → 95% 停止录像并告警 → 低于 70% 恢复。
