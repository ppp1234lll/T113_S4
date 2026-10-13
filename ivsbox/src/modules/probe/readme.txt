============================================================
src/modules/probe  ——  统一探活引擎（iv_probe）＋ 摄像机网络状态探测（iv_camdet）
============================================================

【目录作用】
  本目录放两个**探活类**模块，都属模块层（`libivmodules.a`）：
  - **`iv_probe`**（M3-S3.2）：主网 / 双 WAN 共用的**四层探活与统计判定**引擎，向
    `iv_netmgr`、`iv_agent` 输出统一的可用性结论。
  - **`iv_camdet`**（M4-S4.1）：**摄像机网络状态探测**，三方式可选、运行时可切换，
    输出五态编码给上报帧（`CNS` 段）与故障自愈（`iv_recovery`）。

  两者互不依赖：`iv_camdet` 的 PING 方式在模块内**内嵌一个独立 `iv_probe` 实例**
  （不往 WAN 项表加项，避 `IV_PROBE_MAX_ITEMS=8` 顶满与"引擎无删项接口"）。

【文件说明】
  - `ivsbox/include/ivsbox/iv_probe.h` ＋ `ivsbox/src/modules/probe/iv_probe.c`
    （926 行）：四层探活引擎。`ivsbox/tests/unit/test_probe.c`。
  - `ivsbox/include/ivsbox/iv_camdet.h`（328 行）＋
    `ivsbox/src/modules/probe/iv_camdet.c`（908 行，**零 malloc**）：
    `ivsbox/tests/unit/test_camdet.c`（710 行，12 组 c01~c12）。

【实现状态】
  两个模块**均已实现**。
  - `iv_probe`：M3-S3.2，已接线（`iv_netmgr` 消费）。
  - `iv_camdet`：M4-S4.1，**已装配进 `iv_agent`**（camdet 起停、200 ms 节拍 tick、
    netlink 链路事件转接、平台命令 `0xA5` 经 `iv_proto` 路由到 `on_cfg_search_mode()`；
    `main.c` 读 6 路 `camera.ch.N.ip`；配置键 `camera.detect.mode`(1~3，默认 1) /
    `camera.detect.rtt_ms`(默认 200，10~10000)）。
  **未做（如实登记）**：`iv_camdet` 的 **VM netns veth 实测**与**板端对实验室摄像机
  实测**（M4 退出条件"实验室摄像机实测"仍欠）。

【约定与注意事项】
  - **四层探活（`iv_probe`）**：链路层＝rtnetlink 事件（`iv_probe_set_link`）；网络层＝
    raw socket ICMP（需 `CAP_NET_RAW`）；传输层＝非阻塞 TCP connect 完成即断开；
    应用层＝心跳/RTSP OPTIONS/SNMP GET 任一。统一输出＝状态 / 原因码 / 最近 RTT /
    滑动平均 / 丢包率 / 连续成败次数 / 最近成功时间。**ICMP 被禁（无 CAP 或被墙）时，
    TCP/应用层成功仍判可用**。
  - **`iv_camdet` 五态编码**＝现网 `NET_STATUS_E`：`0=无IP 1=正常 2=故障 4=延时 5=丢包`
    （**没有 3**；3 被主网"网线断开"占用）。
  - **三方式互斥、运行时可切换**（`camera.detect.mode`）：① **PING** —— 每轮每台 4 个
    echo（包间隔 5 s，整轮 30 s），全部应答且平均 RTT < `camera.detect.rtt_ms` → 1；
    ≥ 阈值 → 4（延时）；部分应答 → 5（丢包）；全无应答 → 2（故障）。② **端口扫描** ——
    逐台非阻塞 connect 7 端口表 `{80,554,8000,37777,5000,8080,8899}`，任一可连 → 1、
    全不可连 → 2。③ **RTSP** —— 向 `rtsp://<ip>:554/` 发 `OPTIONS`，收到 >0 字节 → 1、
    失败/超时 → 2。
  - **网线断开覆盖**：`iv_camdet_set_link(0)` 令所有已配 IP 通道报 2（对齐现网）。
  - **逐包采样技巧**：PING 靠"**包间隔（5 s）≫ tick 周期（200 ms）**"使 `sent`/`lost`
    增量恒 ≤1，从而不需要引擎暴露逐包事件。
  - **端口/RTSP 用 tick 推进的串行非阻塞状态机**，**不阻塞 Reactor** —— 这是对计划
    原文"RTSP 用慢任务池(S5)"的**有意偏离**（非阻塞状态机阻塞度为零；慢池留给 DNS /
    SNMP / ONVIF 那类长阻塞），已登记。
  - **单次回调约束（架构 §4.2）**：回调内禁止同步阻塞探测；禁止在回调内做长 IO。
  - 无 `CAP_NET_RAW` 时 `iv_camdet` **自动跳过需要 raw socket 的路径**而非整体失败。

【相关文档】
  ivsbox/docs/功能开发计划.md：§S3.2（统一探活引擎）、§S4.1（摄像机网络状态探测）
  ivsbox/docs/代码说明.adoc：2026-10-13 11:48 节（S4.1）
  T113运维终端-系统架构设计.md：§4.2（单次回调约束）
