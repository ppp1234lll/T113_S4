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
 升级（modules，M7-S7.1）
    test_ota.c          iv_ota 升级包与验签：清单解析与路径围栏、版本号、Ed25519 验签（**测试侧用 OpenSSL 现造密钥并签名**，
                        设备侧只有验签）、型号/硬件版本/软件版本语义、逐文件 SHA-256、结构边界；两把密钥验"别人的签名也拒"
 升级（modules，M7-S7.2 ＋ S7.3）
    test_upd.c          iv_upd 安装 / 双版本切换 / 回滚 / 启动包装，15 组：
                        t01 空间判据（余量是**排他**下界）；t02 默认值、名字表与枚举稳定性；
                        t03 正常升级全链路（内容逐字节相等、bin/ 0755 与其余 0644、adopt
                        落的版本目录真实存在、两个符号链接都指对、pending.json 字节形状、
                        status 各字段、events.log、暂存目录刻意留作排查）；
                        t04 bootcount 连拉 3 次后第 4 次自动回滚 ＋ 终态拒绝 ＋ 回滚后仍能启动；
                        t05 自检通过（pending 保留但不再 active、bootcount 归零、幂等、不再回滚）；
                        t06 自检失败（reason=selftest-failed）与"无 pending 时 boot-fail 报 ESTATE"；
                        t07 previous 目录缺失 ⇒ 回滚报 ESTATE 而不是把盘改坏；
                        t08 **5 个掉电相位逐个钉住**（每个相位断言盘上状态 ＋ 下一轮该起哪个
                        版本 ＋ 重试行为）；t09 拒包不留残迹（型号不符、全零公钥 fail-closed）；
                        t10 ENOSPC（用 /proc/self 制造假 statfs，宿主空间真够时自报 SKIP）；
                        t11 iv_upd_check_elf 全分支（普通/依赖齐全/依赖缺失带名字/架构不符/
                        ET_REL/截断/纯文本/文件不存在/NULL）；t12 安装期准入拒绝
                        （非 ELF 程序、依赖无法满足）且**不留版本目录、不留 pending**；
                        t13 升级请求文件的往返与围栏（相对路径/含空白/不在 ota 目录/不存在/是目录）；
                        t14 回滚护栏 ＋ 空根上的 status ＋ NULL 容忍
                        t15 **复装"回滚目标"那一版必须被拒**（回滚把 current 退回旧版本
                        之后，"比 current 新、却正好等于 previous"的版本号重新可装；少了
                        护栏，install 会在 copy 前 `rm_rf` 掉回滚目标那一份）；断言两件事：
                        调用被拒 **且** 目标目录原封不动

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
  - **已知覆盖缺口（如实登记，勿当成已覆盖）**：`iv_upd` 的 `IV_UPD_EPERM`
    分支（装完回读发现 `bin/` 下文件没有执行位）没有单测 —— 它需要"只读挂载或
    无 mode 的文件系统"才造得出来，宿主机上不通用。当前只由代码路径保证，
    真机（rootfs 只读/异常挂载）遇到时按 `bin/` 回读判据报 EPERM。
    **反向证伪时另用两条变异证明"回读确实被执行、且它的判据被采信"**：
    ① 把回读条件取反（`== 0` → `!= 0`）⇒ 正常安装被判 EPERM；② 让落点计算不再给
    `bin/` 打执行位（PROG/DATA 互换）⇒ 回读抓到"真的没有执行位"。两条都会让
    t03 报红并已还原复绿；但**分支本身**（chmod 成功、回读失败）仍需只读介质才触发。
  - `test_upd` 里 t10 的 ENOSPC 用 `/proc/self` 制造假 statfs：**宿主上空间真够就
    自报 SKIP**（不假绿），要看该分支需在受限环境跑。

【相关文档】
  - ivsbox/docs/功能开发计划.md（M1-S2~S9、M2-S2.1~S2.6 各步"怎么验证"）
  - T113运维终端-系统架构设计.md §15.5、§16.1
