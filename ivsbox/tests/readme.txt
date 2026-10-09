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
  本目录无直接源文件，只有三个下级测试子目录：
    unit/                 单元测试源码（`make test` 会逐个 RUN）
    fuzz/                 模糊测试 harness（`make fuzz`）
    integration/          集成测试（`make itest`，前提是 ivsboxd 已在跑）

【运行方式】
  make test    构建并运行 tests/unit/*（宿主，自包含）
  make asan    以 ASan/UBSan 重新构建并运行单测（产物 build/asan）
  make tsan    以 TSan 重新构建并运行单测（仅宿主，产物 build/tsan）
  make itest   构建并运行 tests/integration/*（需先起 ivsboxd）
  make fuzz    构建 tests/fuzz/* 的 fuzz harness

【约定与注意事项】
  - 单元测试必须自包含：不依赖外部进程、网络与真实硬件。需要设备的用例一律
    降级或做成可选（如串口用 pty、看门狗用 pipe 假 fd、GPS 用样本灌入）。
  - 集成测试刻意不并入 `make test`：其前提是"外部服务已在跑"，并进去会让 CI
    因"没起服务"而假红。这是测试体系的刻意取舍，见 Makefile `itest` 注释。
  - 顶层 Makefile 是唯一构建入口，本目录及子目录都不放 Makefile。

【相关文档】
  - ivsbox/docs/功能开发计划.md（§0.1 实现状态总表；M1/M2 各步"怎么验证"）
  - T113运维终端-系统架构设计.md §15.5（库分层与依赖方向）、§16.1（自动化测试）
