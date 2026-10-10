============================================================
tests/unit  ——  单元测试：自包含、可脱离一切外部环境运行
============================================================

【目录作用】
  IVSBox 的单元测试层，一个源文件对应一个被测模块/主题。每个文件都是独立可执行
  程序，`make test` 会逐个 `RUN` 并检查退出码。全部用例自包含：不依赖外部进程、
  网络或真实硬件；被测库为 libivmodules/libivhal/libivcore 三个静态库。

【文件说明】
  基础件（core，M1-S2/S3/S9/S7）
    test_basic.c        S2 基础件综合：iv_log 运行期级别/单行格式/风暴抑制、iv_clock 单调性、iv_strerror 冒烟
    test_err.c          iv_err 错误码黄金表，与单片机 error.h 码值逐条钉死、任一侧改动即失败
    test_log.c          iv_log 落盘路径与过期清理：<root>/YYYY-MM-DD/HH.log、保留天数、字节上限
    test_version.c      iv_version 版本串 <major>.<minor>.<patch>+g<git>、注入自洽与降级 "unknown"
    test_crc.c          iv_crc：标准向量 "123456789" 与单片机现网黄金表（CRC-8/SMBUS、CRC-16/MODBUS）
    test_chan.c         iv_chan 本地通道：消息头布局/编解码、socketpair 收发、超时、64 KiB 满载、对端关闭不 SIGPIPE
    test_db.c           iv_db SQLite 封装：open/建表/exec/事务回滚/WAL/标量查询/损坏库恢复/integrity_check
    test_reactor.c      iv_reactor：入参校验、fd 事件、定时器（取消/重挂/上限）、延迟释放、句柄归属
    test_taskpool.c     iv_taskpool 慢任务池：正常完成、队列满 IV_EBUSY、取消 QUEUED/RUNNING、截止时间
    test_health.c       iv_health 健康线程：用 pipe 代替 watchdog fd 数喂狗次数、按真实 reactor/taskpool 进度号判定
  硬件抽象（hal，M2-S2.1）
    test_hal.c          HAL 层桩：仅验 iv_hal_init() 返回 0
    test_serial.c       iv_serial：pty 造 tty 验 raw 模式/关流控/flock 独占/波特率白名单，真实串口为可选用例
    test_watchdog.c     iv_watchdog：全程用 pipe 假 fd，绝不打开真实 /dev/watchdog（ioctl 路径留板端实测）
  业务模块（modules，M2）
    test_frame.c        iv_frame UART 帧解析：正常/粘包/半包/噪声重同步/坏 CRC/超长/帧尾错/超时复位/build 错误路径
    test_link.c         iv_link UART 可靠层：重传字节一致、应答配对、同 cmd IV_EBUSY、上报 on_upstream、链路 DOWN/UP
    test_proto.c        iv_proto 平台协议：二进制与 ## 文本组帧黄金校验、解析/粘包、路由与自动 ACK、心跳
    test_gps.c          iv_gps：minmea 上游校验和样本 + 动态构造样本，无效定位不采信、校时三道闸
    test_status.c       iv_status 状态镜像：0xE1 全量/0xC1 门事件/0xC2 事件按 TAG 增量、valid 位、snapshot JSON
    test_s2_uart.c      采集板串口装配：假串口和 Reactor 验查询、部分写、应答、断线重连与停止
  配置（modules，M1-S8）
    test_config.c       iv_config：单文件口径、单句柄约束、数组/double 类型、中文注释往返、按一级分类子树的热更新事务

【运行方式】
  make test    构建并逐个运行全部单测（宿主 gcc）
  make asan    以 ASan/UBSan 重跑（build/asan，部分延迟释放/UAF 用例靠它验）
  make tsan    以 TSan 重跑（仅宿主，与 asan 互斥，build/tsan）
  make arm     交叉编译单测产物（板端可单独跑，输出与宿主逐字一致）

【约定与注意事项】
  - 新增被测模块时在 tests/unit/ 放一个 test_<模块>.c，无需改 Makefile（按目录通配收集）。
  - 需要设备/内核的用例不得成为默认失败源：能降级就降级（pty/pipe），不能就做成
    命令行参数可选，或移到 tests/integration/。
  - 单测产物 .o 由 Makefile 的 .PRECIOUS 保护，clean 后不会每次白重编。

【相关文档】
  - ivsbox/docs/功能开发计划.md（M1-S2~S9、M2-S2.1~S2.6 各步"怎么验证"）
  - T113运维终端-系统架构设计.md §15.5、§16.1
