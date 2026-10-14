============================================================
tests  ——  工程全部自动化测试的根目录，按"是否依赖外部环境"分三层
============================================================

【目录作用】
  本目录收纳 IVSBox 的全部测试代码，按自包含程度分为三类，边界分明：
    unit/        单元测试，自包含，任何环境都能跑，由 `make test` 驱动；
    fuzz/        模糊测试，用随机输入灌被测模块，由 `make fuzz` 驱动；
    integration/ 集成测试，需外部服务（ivsboxd）已在跑，由 `make itest` 驱动。
  三者都由顶层 Makefile 统一构建，且与 ivsboxd 同源：都链接三个静态库
  （-livmodules -livhal -livcore），不引用 ivsboxd 的 main/装配代码。

【文件说明】
  本目录下有三个测试子目录，另有三个**不由 make 驱动**的现场工具脚本：
    unit/                 单元测试源码（`make test` 会逐个 RUN）
    fuzz/                 模糊测试 harness（`make fuzz`）
    integration/          集成测试（`make itest`，前提是 ivsboxd 已在跑）
    fault-inject.sh       故障注入执行清单与记录（M7-S7.4，§16.1"故障注入"行、
                          §16.2 场景 7/8/9）。逐项标 RUN / DELEGATE / MANUAL：
                          RUN 真注入（真起 monitor-web.sh ＋ monitor-media.sh，
                          `kill -STOP` 其一，断言"只重启对应服务"）；DELEGATE 调已有
                          用例且**输出含 SKIP 一律记 SKIPPED**（SKIP 不能当通过）；
                          MANUAL 只印真机注入方法与判据，记 NOT-RUN、不计入通过
    soak.sh               长稳测试（M7-S7.5，≥72h）：周期触发网络切换 / 媒体启停 /
                          通道查询，采 RSS、fd、线程数、control.db 大小并按小时算
                          最小二乘斜率。**斜率只在运行 ≥ --slope-min-hours 时才判定**
                          （默认 6h）——短跑的斜率是噪声，不拿它宣称"平稳"
    upgrade-boot.sh       升级 / 启动链路端到端（M7-S7.2 ＋ S7.3 的**装配层**证据）：
                          真跑 build/host/ivsbox-updater ＋ scripts/ivsbox-run.sh ＋
                          scripts/monitor.sh，盘面（app_root / releases / 链接 /
                          pending.json）在 /tmp 下现造。u1 裸布局；u2 current 链接生效；
                          u3 目标目录缺主控 ⇒ 退出码 127；u4 bootcount 超限 ⇒ 自动回滚；
                          u5 自检超时 ⇒ 回滚 **＋ 忽略 TERM 的实例被升级为 KILL**；
                          u6 包装脚本整份缺失时，陈旧就绪标记绝不许被当成战绩。
                          为什么必须有它：`monitor → 包装 → boot-begin → 换目录 exec`
                          这条**接线**在 C 单测里不存在，单测测不到（项目教训：接线在
                          装配层，只有真拉起进程的用例抓得到）。
                          ⚠ 假主控必须是**编译出来的 ELF**：monitor.sh 用 `ps -o comm`
                          判活，shell 脚本的 comm 是解释器名，拿脚本假装主控会让守护
                          逻辑根本进不去 —— "什么都没测"却报绿

【运行方式】
  make test    构建并运行 tests/unit/*（宿主，自包含）
  make asan    以 ASan/UBSan 重新构建并运行单测（产物 build/asan）
  make tsan    以 TSan 重新构建并运行单测（仅宿主，产物 build/tsan）
  make itest   构建并运行 tests/integration/*（需先起 ivsboxd）
  make fuzz    构建 tests/fuzz/* 的 fuzz harness
  sh tests/fault-inject.sh [--keep]         # 故障注入清单（宿主项真注入，真机项打印清单）
  sh tests/soak.sh --minutes 3              # 长稳冒烟（只验脚本与采样口径）
  sh tests/soak.sh --hours 72 --outdir DIR  # 长稳正式跑（现场），产出 CSV ＋ 报告
  sh tests/upgrade-boot.sh [--keep]         # 升级/启动链路（前提：先 `make host`）
  三个脚本**刻意不进 `make` 体系**：它们要驱动真实进程与真实网络/存储，
  产物也不是 ELF，塞进构建图只会让 `make test` 变慢且语义混乱。

【约定与注意事项】
  - 单元测试必须自包含：不依赖外部进程、网络与真实硬件。需要设备的用例一律
    降级或做成可选（如串口用 pty、看门狗用 pipe 假 fd、GPS 用样本灌入）。
  - 集成测试刻意不并入 `make test`：其前提是"外部服务已在跑"，并进去会让 CI
    因"没起服务"而假红。这是测试体系的刻意取舍，见 Makefile `itest` 注释。
  - 顶层 Makefile 是唯一构建入口，本目录及子目录都不放 Makefile。

【相关文档】
  - ivsbox/docs/功能开发计划.md（§0.1 实现状态总表；M1/M2 各步"怎么验证"）
  - T113运维终端-系统架构设计.md §15.5（库分层与依赖方向）、§16.1（自动化测试）
