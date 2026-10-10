============================================================
src/modules  ——  libivmodules：业务模块层（一个子目录一个模块）
============================================================

【目录作用】
  modules/ 编译出 libivmodules.a，是业务实现层，依赖方向 libivcore → libivhal → libivmodules。
  纪律：一个子目录一个模块，**模块间禁止互改私有数据**（只经公共头 + 回调交互）。
  本层可使用第三方 vendored 库（third_party/，当前只有 minmea，随 modules 一起编译）。
  根层另放三个"跨模块/装配"实现文件，与各模块子目录并列（见下）。

【文件说明】
  iv_health.c           健康线程 + 看门狗门控（判主循环/慢任务池是否活着，决定喂狗/停喂）
  iv_modules.c          业务模块统一入口桩（iv_modules_init，后续按子模块拆分）
  iv_taskpool.c         慢任务线程池（固定 2 worker、定长队列，承接一切不可控阻塞操作）
  access/               门磁/门锁控制与审计（鉴权开锁、门事件上报）
  config/               配置管家（点分键读写、默认值补齐、原子写、热更新、事务通知）
  device/               摄像机/交换机档案与 ONVIF 发现（一机一档，主键 MAC/序列号）
  gps/                  GPS/NMEA 定位与时间源管理（上报策略收在模块内）
  link/                 采集板 UART 链路（成帧 iv_frame、命令级可靠层 iv_link）
  netmgr/               网络管理（rtnetlink 监听、双 WAN 切换状态机）
  ota/                  升级协调（只协调流程，验签与安装归 updater/）
  probe/                统一探活引擎（链路/ICMP/TCP/应用层四层探活与统计判定）
  proto/                云平台私有协议与持久上报队列
  recovery/             自愈策略引擎（故障确认→断电重启→验证→熔断）
  report/               上报装配层（采集板链路→状态镜像→平台报文→持久队列→TCP 传输）
  snmp/                 SNMP 采集与品牌 OID 模板
  status/               采集板状态镜像（IO/阈值/继电器在内存中的权威副本）
  tunnel/               可选远程访问通道（暂缓，平台立项后启用）

【约定与注意事项】
  - 模块间禁止直接读写对方私有数据结构；只经 include/ivsbox/ 的公共头交互。
  - 需要 pthread 或第三方库的实现只能放本层（core 层不允许）。
  - 协议字段/内存布局口径以架构 §18.2 等已冻结事实为准，不得凭猜测编造。
  - 子目录不放 Makefile；源文件由顶层通配 src/modules/*.c 与 src/modules/*/*.c 发现。

【相关文档】
  ivsbox/README.md「目录结构」；docs/功能开发计划.md（M2~M7 各模块）；架构 §15.5
