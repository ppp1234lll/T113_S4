============================================================
src/modules/snmp  ——  自写最小 SNMPv2c 客户端（只 GET/GETNEXT）＋ OID 全可配（已实现，M4-S4.3）
============================================================

【目录作用】
  按配置里的 **OID 槽位**从摄像机 / 交换机取值，供平台上报与 Web 展示。
  属模块层（`libivmodules.a`）。**厂商中立、代码内不内置任何品牌 OID 特化表**
  ⇒ 换交换机/摄像机型号**只改配置、不改固件**。

【文件说明】
  - `ivsbox/include/ivsbox/iv_snmp.h`（191 行）：公开接口。
  - `ivsbox/src/modules/snmp/iv_snmp.c`（633 行，**零 malloc**）：游标式构造器
    `wr_t`/`wr_open`/`wr_close` ＋ 解析器 `rd_t`/`rd_tlv`/`rd_enter`/`rd_int`；
    `iv_snmp_oid_encode/decode`（base-128）、`iv_snmp_build_request`、
    `iv_snmp_parse_response`、`iv_snmp_transact`、`iv_snmp_get`、`iv_snmp_getnext`、
    `iv_snmp_walk`、`iv_snmp_oid_in_subtree`。
  - `ivsbox/tests/unit/test_snmp.c`（596 行，10 组 s01~s10）。

【实现状态】
  已实现（模块 ＋ 配置键 ＋ 单测 ＋ VM 全量验证 ＋ 反向证伪 8 条），2026-10-13。
  **未做（如实登记）**：① **装配轮询未接线** —— 计划要求收发全在慢任务池（M1-S5）、
  绝不阻塞 Reactor，`iv_snmp_transact()` 是**阻塞式**（`SO_RCVTIMEO` ＋ 重试）
  ⇒ **必须**在慢池内调用；② `control.db` 落盘（表结构待 S9）；③ **SET 写节点**
  （方案 B 只读；海康 `restartDev`、交换机 Port Admin Status / Pvid / PoE 只登记
  不实现）；④ **交换机部分卡现场三样输入：交换机 IP ＋ 读团体字 ＋ 型号/品牌**
  （摄像机部分不卡）；⑤ `snmpwalk -v2c` 旁证与 VM `snmpd`/`snmpsim` 假设备对测。

【约定与注意事项】
  - **为什么自写而不引 net-snmp**：板端 sysroot / 编译 VM / Tina **三处实测均无
    net-snmp** ⇒ 引它等于先移植一万多行库。本模块**只做 GET / GETNEXT**，
  **无 SET / TRAP / v3 / USM**。
  - **MIB ≠ 实现**：厂家 `.mib` 只回答"查哪些 OID"，BER 编解码与 UDP 收发仍须自写。
    三份厂家 MIB 的用途＝**抽 OID 蓝本**：海康 `.1.3.6.1.4.1.39165.1.<n>`、
    大华 `.1.3.6.1.4.1.1004849.1.1.{2.<n>,3,9.<n>}`、
    交换机 IF-MIB ＋ LLDP `.1.0.8802.1.1.2.1.4.1.1.*`
    （**不带末尾 `.0` 的＝表列，走 GETNEXT**；带 `.0` 的＝标量，走 GET）。
  - **OID 全可配的落点**：`camera.snmp.oid.<槽位>` / `switch.snmp.oid.<槽位>`
    —— **键名即语义、值即 OID 字符串**；另有
    `camera.snmp.{enable,community,timeout_ms,retries}`、
    `switch.snmp.{enable,port,community,timeout_ms,retries}`。
  - **为什么不能给每个 OID 再配 name/type 字段**：`camera.snmp.oid.cpuPercent`
    **恰为 4 段**，已到 `IV_CFG_DEPTH_MAX = 4` 的极限 ⇒ 再加一层就是第 5 段、
    **越界非法**。故 **name 由键名承担、type（`integer`/`string`/`counter`/`gauge`）
    与中文说明由默认值表的 desc 登记**（`save()` 渲染成注释，同源不过期）。
  - **槽位名必须厂家中立**：同一槽位只能存**一个** OID，而海康/大华同指标 OID 不同
    （CPU：`.39165.1.7.0` vs `1004849.1.1.3.0`）⇒ **不能** `hikCpu`/`dhCpu` 两套并存
    （相机只 1 路，白吃一倍槽位、直逼 128 上限）。做法＝槽位名只表语义，**值是哪种
    机型就填哪种**；出厂默认给**海康**一套，大华套由 Web（S5.1）一键切。
    摄像机为其它品牌时该组私有 OID 无应答属**预期**（报无应答、不乱填）。
  - **容量红线（实测数字）**：默认值表现 **119 条**（`camera.ch` 独占 48），
    距 `IV_CFG_ITEMS_MAX = 128` **余 9 槽** ⇒ 本轮**未动 M1**。再扩槽位须抬
    128→**192**（`cfg_item_t sorted[]` 约 34 KB→51 KB，仍远在 reactor 8 MB 栈内）
    —— **登记为待批准项**。
  - **应答处理**：**request-id 不匹配的包一律丢弃**（继续等）；
    `noSuchObject` / `noSuchInstance` / `endOfMibView` 一律映射"无值"、**不乱填**；
    `iv_snmp_walk` **子树边界终止** ＋ 行数上限（默认 64）＋ 无进展即止。
  - **对参考实现的一处修正（登记为修正、非偏离）**：参考 `snmp_udp.c` 的长度域
    **只写单字节**（>127 即发非法包）；本模块按 BER **长式**（`0x81`/`0x82`）实现。
    ⚠ 相关坑：`wr_close()` 的长式长度回填**必须倒序搬**（目标地址恒在源地址右边，
    正向搬会把首字节沿路刷满整段内容，表现为"包长度对、内容全同一个字节"）。
  - **`community` 属口令类**，架构 §10 本应走 `secure/`，但 **F8 密钥分离未实现**
    ⇒ 暂落普通配置，**登记为已知欠账**（与 `camera.ch.N.pass` 同类）。
  - **单次回调约束（架构 §4.2）**：回调内禁止同步 SNMP 调用。

【相关文档】
  ivsbox/docs/功能开发计划.md：§S4.3
  ivsbox/docs/代码说明.adoc：2026-10-13 12:36 节（S4.2 ＋ S4.3）
  T113运维终端-系统架构设计.md：§18.4
