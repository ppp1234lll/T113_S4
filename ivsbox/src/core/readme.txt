============================================================
src/core  ——  libivcore：平台无关基础件（仅依赖 libc）
============================================================

【目录作用】
  core/ 编译出 libivcore.a，是依赖金字塔的最底层，按架构 §15.5 **只允许依赖 libc**：
  禁止 pthread、禁止任何第三方库。链接顺序里 libivcore 排在最后且带 -Wl,--no-undefined，
  因此 core 一旦反向依赖 libivhal/libivmodules 会当场链接失败（iv_reactor 自己取时即因此）。
  提供错误码、CRC、日志、SQLite 封装、事件循环、本地通道、版本等基础设施。

【文件说明】
  iv_chan.c             本地进程通道实现（AF_UNIX+SOCK_SEQPACKET），无隐藏 malloc、不产生 SIGPIPE
  iv_crc.c              CRC-8/SMBUS 与 CRC-16/MODBUS 逐位实现（无表、无外部库依赖）
  iv_db.c               SQLite 单写者封装（WAL、busy_timeout、header 损坏改名 *.corrupt 重建）
  iv_err.c              跨端故障码：码值 → 稳定 ASCII 符号名（只读查表）
  iv_log.c              统一日志门面实现（日期目录+小时文件落盘、风暴抑制、C11 自旋锁串行化）
  iv_reactor.c          epoll 事件循环 + 最小堆定时器 + 单个 timerfd（取时用 clock_gettime）
  iv_version.c          版本串与 Git 短哈希（无 HAL 依赖，可进 core 跑单测）

【约定与注意事项】
  - 硬纪律：仅 libc。需要 pthread 的模块必须落到 modules 层（如 iv_taskpool）。
  - iv_reactor 不调 iv_clock（那在 hal 层），直接用 libc clock_gettime(CLOCK_MONOTONIC)。
  - 头文件在 include/ivsbox/，本目录只放实现；风格见 ivsbox/.clang-format。
  - 源文件由顶层 Makefile 通配 src/core/*.c 发现。

【相关文档】
  架构 §4.2（主 Reactor）、§15.5（依赖方向）、§12.1（日志）；docs/代码说明.md
