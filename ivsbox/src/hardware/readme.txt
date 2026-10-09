============================================================
src/hardware  ——  libivhal：Linux 硬件抽象层
============================================================

【目录作用】
  hardware/ 编译出 libivhal.a，承接与操作系统/硬件打交道的最薄封装：串口、时钟、看门狗、HAL 入口。
  依赖方向 libivcore → libivhal → libivmodules → ivsboxd，hal 可依赖 core 与 libc。
  同一条纪律：不引入 pthread、不引用 iv_reactor（只交出 fd，由调用方注册进事件循环）；
  属 syscall 薄包装，不做业务判定、不写日志（错误一律靠返回码 + errno）。

【文件说明】
  iv_clock.c            单调时钟薄封装（clock_gettime(CLOCK_MONOTONIC) 的 ms/us 取值）
  iv_hal.c              HAL 统一入口桩（iv_hal_init；后续按 serial/netlink/watchdog/fs/clock/capability 拆分）
  iv_serial.c           串口 termios 薄封装（raw+零流控+非阻塞，波特率走显式白名单）
  iv_watchdog.c         /dev/watchdog syscall 薄包装（打开/设超时/喂狗/停喂，不含判定）

【约定与注意事项】
  - 板端串口设备是 /dev/ttySAC<n>（非 /dev/ttyS<n>）；ttySAC3 是内核 console（COM4），业务不可占用。
  - 采集板链路 /dev/ttySAC5 115200 8N1；GPS 链路 UART1(PD21/PD22) 9600 8N1——两条波特率不同，勿用默认值开 GPS 口。
  - 看门狗超时上限 16s；"该不该喂狗"由 src/modules/iv_health.c 判定，本层不管。
  - 不引 pthread、不引 iv_reactor、不写日志。

【相关文档】
  架构 §15.5；include/ivsbox/iv_serial.h、iv_watchdog.h、iv_clock.h 文件头；docs/代码说明.md
