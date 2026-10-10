============================================================
src/modules/netmgr  ——  双 WAN 切换状态机（已实现，M3-S3.3）
============================================================

【目录作用】
  网络管理：无线优先、故障切有线、稳定回切的确定性状态机（架构 §7.2）。
  属模块层（`libivmodules.a`）。
  注意：配套的 **rtnetlink 监听（S3.1）落点在 `src/hardware/iv_netlink.c`**，
  不在本目录。

【文件说明】
  - iv_netmgr.c   实现（零点 malloc、不引 pthread/Reactor；I/O 全经函数指针注入）
  - 公共头        include/ivsbox/iv_netmgr.h
  - 单测          tests/unit/test_netmgr.c

【实现状态】
  已实现（2026-10-10 09:42，功能开发计划 M3-S3.3）。VM `make test` 24/24、
  asan/tsan/analyze/arm 全绿；反向证伪 6 条按预期；VM netns 真机与 T113 板端
  均已验证（详见 docs/代码说明.md 与 docs/修改记录.md 同日条）。

【约定与注意事项】
  - 状态：`WIRELESS_UP / WIRELESS_DEGRADED / SWITCH_TO_WIRED / WIRED_UP /
    SWITCH_TO_WIRELESS / BOTH_DOWN`；参数（fail_n / ok_n / hold_s / stable_s）
    当前由装配层传结构体，**未落 `ivsbox.json`**。
  - 事务执行 §7.2 **七步事务**：冻结低优先级发送 → 切默认出口 → 关闭绑定旧接口的
    TCP（含平台连接与远程流）→ 新接口重连 → 鉴权恢复 → 按优先级补传 →
    稳定期后恢复媒体大流量。**本模块只编排、不实现具体 I/O**：七步经
    `iv_netmgr_txn_ops_t` 注入（`NULL`＝该步不执行）；默认执行点
    `iv_netmgr_ops_default()` 只实现 `switch_route`（netlink 真改默认路由），
    其余四项待装配层补。
  - **"无线"＝4G `usb0`、"有线"＝`eth0`** —— 架构 §18.3 板端实测冻结（2026-10-10）。
    ⚠ WiFi（`wlan0`）**不是 WAN**，是给摄像机的 AP；⚠ `usb0` 当前**无自动拨号脚本**。
  - ⚠ `switch_route` 的 netlink 请求**必须带 `NLM_F_ACK`**（否则成功时内核不回包、
    阻塞 `recv()` 永久卡死）—— 详见代码内注释与 docs/代码说明.md。

【相关文档】
  ivsbox/docs/功能开发计划.md：§S3.1、§S3.3
  T113运维终端-系统架构设计.md：§7.2、§18.3
