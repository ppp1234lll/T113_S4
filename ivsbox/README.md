# IVSBox

T113 运维终端主工程。

## 构建

```sh
make        # 宿主机构建（默认）
make arm    # ARM 交叉构建
make test   # 单元测试
make asan   # ASan/UBSan 测试
make clean  # 清理
make help   # 查看全部目标
```

## 目录结构

```text
ivsbox/
├── Makefile                顶层唯一构建入口（不递归 make，子目录不放 Makefile）
├── mk/                     Makefile include 片段
│   ├── rules.mk                通用编译/链接/自动依赖规则（-MMD -MP 等）
│   ├── toolchain-host.mk       宿主机工具链（单测 / ASan/UBSan）
│   └── toolchain-tina-arm.mk   板端交叉工具链（arm-linux-gnueabi-）
├── include/
│   └── ivsbox/             对外公共头文件（ivsbox.h 总入口；错误码/日志/时钟/CRC/IPC 头等）
├── src/                    ivsboxd 主控源码
│   ├── app/                入口与装配：main、Reactor 启动、健康线程、命令路由
│   ├── core/               平台无关基础件：错误码、日志、CRC、定时器、IPC 编解码、SQLite 封装
│   ├── hardware/           Linux 硬件抽象：串口(termios)、netlink、看门狗、时钟
│   └── modules/            业务模块，一个子目录一个模块，禁止互改私有数据
│       ├── link/           UART 板间链路（帧解析、可靠层、状态镜像）
│       ├── proto/          云平台私有协议（表驱动路由、分级发送队列、持久上报队列）
│       ├── netmgr/         网络管理（rtnetlink 监听、双 WAN 切换状态机）
│       ├── gps/            GPS 定位与时间源管理
│       ├── probe/          统一探活引擎（链路/ICMP/TCP/应用层四层）
│       ├── device/         摄像机与交换机档案（ONVIF 发现、一机一档）
│       ├── snmp/           SNMP 采集与品牌 OID 模板
│       ├── recovery/       自愈策略引擎（确认→断电重启→验证→熔断）
│       ├── access/         门磁/门锁控制与审计
│       ├── config/         配置读取、校验、原子写、热更新
│       ├── ota/            升级协调（只协调，验签安装归 updater）
│       └── tunnel/         可选远程访问通道（暂缓，平台立项后启）
├── tests/                  测试
│   ├── unit/               单元测试（Host 上 make test 运行）
│   ├── fuzz/               模糊测试（帧/协议输入边界）
│   └── integration/        集成测试（模拟器对联、IPC、SQLite）
├── tools/                  按需工具（非常驻）
│   ├── provision/          出厂工装：写设备 ID / MAC
│   ├── uart_simulator/     采集板串口模拟器（单测与故障注入）
│   └── platform_simulator/ 云平台模拟器（断线/丢包注入）
├── media/                  ivsbox-media 媒体进程：RTSP 抓图、断网录像、媒体索引、音频广播
├── web/                    ivsbox-web 管理进程：REST API、静态页面、摄像机受控代理
├── updater/                ivsbox-updater 升级器：验签、双版本切换、回滚（按需 root 运行）
├── scripts/
│   ├── init.d/             procd 服务脚本（ivsboxd / ivsbox-media / ivsbox-web）
│   └── ci/                 CI 辅助脚本
├── packaging/              打包归集目录（make package 产物，供 OTA 升级包使用）
└── docs/                   工程文档：修改记录、功能开发计划、代码说明、操作手册
```

依赖方向（由 Makefile 链接顺序写死）：`libivcore`(仅 libc) → `libivhal` → `libivmodules` → `ivsboxd`。

详细口径见根目录 `T113运维终端-系统架构设计.md` §15。
