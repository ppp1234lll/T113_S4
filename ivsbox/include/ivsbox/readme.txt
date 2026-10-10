============================================================
include/ivsbox  ——  全部公共头文件（跨层接口声明）
============================================================

【目录作用】
  本目录是 IVSBox 的公共头文件集合，各层源码统一以 #include "ivsbox/<名>.h" 引用。
  头文件只声明接口、类型与常量，实现分散在 src/core、src/hardware、src/modules。
  目录里的头与实现的归属层一一对应：core 头仅依赖 libc，hal 头依赖 core，modules 头依赖前两者。
  头文件是模块间唯一的契约，模块不得绕过它去访问对方私有数据。

【文件说明】
  ivsbox.h              公共总入口：版本宏 IVSBOX_VERSION_* 与 ivsbox_init/fini
  iv_chan.h             本地进程间通道（AF_UNIX+SOCK_SEQPACKET），只递完整消息、不解析载荷
  iv_clock.h            单调时钟接口（CLOCK_MONOTONIC，ms/us；时长计算唯一合法时源）
  iv_config.h           配置管家（json-c 后端，点分键读写、默认值、原子写、热更新、事务通知）
  iv_crc.h              CRC-8/SMBUS 与 CRC-16/MODBUS（与单片机现网实现严格对齐）
  iv_db.h               SQLite 单写者封装（control.db，WAL、完整性检查、事务宏）
  iv_err.h              跨端 uint32 故障码，与单片机工程共用码空间（不得单方面改值）
  iv_frame.h            采集板 UART 成帧/编码（上行头 0x0F0F、下行头 0xF0F0，CRC-8）
  iv_gps.h              GPS/NMEA 定位与时间源（vendored minmea，含上报策略判断）
  iv_health.h           健康线程 + 看门狗门控（判主循环/慢任务池是否活着，决定喂狗/停喂）
  iv_link.h             采集板 UART 命令级可靠收发（应答超时重传、链路状态维护）
  iv_log.h              统一日志门面（日期目录+小时文件落盘、级别过滤、故障码风暴抑制）
  iv_proto.h            云平台私有协议（二进制 ACK 帧 + ## 字符串数据帧，只做协议不做传输）
  iv_reactor.h          epoll 事件循环 Reactor（所有 fd 事件的唯一分发中心）
  iv_s2_uart.h          采集板串口装配接口（串口、可靠层、状态镜像的 Reactor 单写者）
  iv_ret.h              本地有符号返回码（负数）；与 iv_err.h 是两套，严禁混用
  iv_serial.h           串口 termios 薄封装（raw + 零流控 + 非阻塞，只搬字节）
  iv_status.h           采集板状态镜像（IO/阈值/继电器在内存中的权威副本）
  iv_taskpool.h         慢任务线程池（固定 2 worker，承接一切不可控阻塞操作）
  iv_version.h          版本串 + Git 短哈希（哈希由顶层 Makefile 编译期注入）
  iv_watchdog.h         /dev/watchdog 薄包装（喂狗等 syscall，不含任何判定逻辑）

【约定与注意事项】
  - 分类要点：iv_ret.h（本地负数返回码）与 iv_err.h（跨端 uint32 故障码）两套命名空间，绝不能混用。
  - 头文件不暴露第三方库类型（如 iv_config.h 完全不出现 json-c 的 json_object）。
  - 保持与实现同层依赖：core 头不可引出 pthread/第三方库。

【相关文档】
  架构 §15.5（依赖方向）、§4.2（Reactor）、§10.2（配置分类）、§12.1（日志）、§18.2（采集板帧）
