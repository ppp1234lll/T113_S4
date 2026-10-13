============================================================
src/modules/device  ——  摄像机多品牌发现：海康 SADP / 海康 UDP / 大华 / 宇视 / ONVIF，一机一档（已实现，M4-S4.2）
============================================================

【目录作用】
  在网内**发现摄像机并建立"一机一档"**，为探活（`iv_camdet`）、SNMP（`iv_snmp`）、
  平台/Web 的"摄像机搜索"提供稳定的设备身份。档案主键＝**MAC 优先、IP 次之**
  （改 IP 不重复建档）；IP 视为可变属性。属模块层（`libivmodules.a`）。

  四品牌协议**按参考项目顺序共用一条相位状态机**轮询，每品牌 3 轮。

【文件说明】
  - `ivsbox/include/ivsbox/iv_device.h`（292 行）：公开接口、报文黄金常量、
    `iv_device_iops_t`（socket/setsockopt/sendto/recvfrom/close/now_ms **全注入**
    ⇒ 单测零网络），以及**纯函数**报文接口（`iv_sadp_checksum/build_inquiry/parse`、
    `iv_hik_probe_build`、`iv_dahua_probe_build/parse`、`iv_onvif_probe_build/parse`）。
  - `ivsbox/src/modules/device/iv_device.c`（923 行，**零 malloc**）。
  - `ivsbox/tests/unit/test_device.c`（492 行，12 组 d01~d12）。

【实现状态】
  已实现（模块 ＋ 单测 ＋ VM 全量验证 ＋ 反向证伪 4 条），2026-10-13。
  **未做（如实登记）**：① **装配层触发未接线** —— 计划要求的"开机一次 ＋ 平台/Web
  手动触发 ＋ 慢任务池执行"均未接进 `iv_agent`；② **`control.db` 建档未做** ——
  全库目前**没有任何 `CREATE TABLE`**，表结构属 S9 尚未定义的交付物；③ ONVIF TCP
  （digest）深度信息未做；④ 真机实测（海康 192.168.1.200、大华 .106/.108）未做。

【约定与注意事项】
  - **探测顺序与报文口径（逐字誊自参考项目）**：
    1. **海康 SADP**：`AF_PACKET` 过滤 EtherType `0x8033` ＋ 66 字节 Inquiry，
       one's-complement 校验和；应答 `0xF6` 前缀、**型号偏移 354**、SN 偏移 38。
    2. **海康 UDP**：`239.255.255.250`（端口 37020），`<Probe><Types>inquiry</Types>`。
    3. **大华**：`255.255.255.255:5050` 32 字节私有帧 ＋ `239.255.255.251:{37810,37820,8087}`
       105 字节 `DHIP` ＋ JSON `DHDiscover.search`；应答需**跳过 32 字节 DHIP 头**再解 JSON。
    4. **宇视**：**实测其私有探测在参考项目里是注释掉的死码**，实际走
       `255.255.255.255:3702` 标准 ONVIF Probe，靠 `/macaddr/` 关键字识别。
    5. **ONVIF WS-Discovery**：`239.255.255.250:3702`，`dp0:NetworkVideoTransmitter`。
  - **IP 口径**：一律按**全工程 `ip_be` 约定** —— `ip_be` ＝ `struct in_addr.s_addr`
    的**内存语义**（点分字节的原样小端拷贝），**不是**数值大端；辅助函数
    `rd_ip4()`/`wr_ip4()` 就是普通 `memcpy`。**首版按数值大端打包，被 d02/d10 抓出。**
  - **型号/序列号提取**：统一走 `extract_after_key()`（跳过 `:`、引号、`>`、`=` 与
    空白，遇引号/`<`/空格收尾），同时覆盖 JSON `DeviceType` 与 XML
    `<DeviceType>X</DeviceType>` 两形态（**首版 XML 分支前缀长度差一，被 d07 抓出**）。
  - **对参考项目的登记增强**：参考项目**没有** ONVIF `ProbeMatches/XAddrs` 结构化
    解析（它只做关键字命中）⇒ 本模块另加"从 XAddrs 取 IP"一条（**登记为增强，非口径**）。
  - **权能**：SADP 需要一个 `AF_PACKET` raw fd（需 `CAP_NET_RAW`，与 ICMP 探活同级；
    `ivsboxd` 以 root 运行本具备）。**无该权能时自动跳过 SADP 相位**而非整体失败。
  - **命名坑**：**不引** `<linux/if_ether.h>` —— 它与 `<net/if.h>` 的 `struct ifmap`
    等重定义冲突 ⇒ `ETH_P_ALL` 本地定义（内核 UAPI 常量）。
  - 档案上限 `IV_DEV_MAX` ＝ 6（现网口径）。
  - **单次回调约束（架构 §4.2）**：回调内禁止同步长 IO；重活进慢任务池。

【相关文档】
  ivsbox/docs/功能开发计划.md：§S4.2
  ivsbox/docs/代码说明.adoc：2026-10-13 12:36 节（S4.2 ＋ S4.3）
  T113运维终端-系统架构设计.md：§18.4
