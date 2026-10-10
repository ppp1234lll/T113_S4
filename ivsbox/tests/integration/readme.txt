============================================================
tests/integration  ——  集成测试：验端到端装配（部分需外部服务已在跑）
============================================================

【目录作用】
  集成测试层，验的是"装好之后的进程 / 整条链路"而非孤立模块，与 tests/unit 分工明确：
  unit 验模块本身（不依赖外部进程），integration 验真实的端到端装配是否成立。
  本层用例按"对端由谁起"分两类，边界必须写清：
    - **连外部服务**：用例只当客户端，连本机通道与已运行的 ivsboxd（test_ctl_snapshot）。
    - **自包含**：用例自己起全部对端（pty、回环 TCP、临时目录），**前置条件为零**，
      任何环境都能跑（test_e2e_report）。

【文件说明】
  test_ctl_snapshot.c  ivsboxd 本地通道端到端往返（M1 退出条件第 2 条）：连
                       IV_CHAN_PATH_DEFAULT，验 system.snapshot 应答、request_id
                       原样回填、同连接二次请求、未实现方法无应答、连接可复用。
                       **需外部服务**。
  test_e2e_report.c    S3.6 上报装配层端到端联动（**自包含，零 mock**）：假采集板＝
                       pty 对（master 由用例持有，slave 路径交给装配层开），假平台＝
                       127.0.0.1 临时端口上的真 TCP listener，持久队列＝临时目录。
                       验 0xE1 轮询帧字节级黄金、0xE1 应答→状态镜像刷新、立即上报→
                       平台收到（字段顺序 / `##` 壳 / `&&数据区&&` CRC）、断开→入队
                       积压、恢复→按序补发且队列清空、E3 查询→qn 原值回填、未注册
                       命令→回 ACK(0x01)。**无需外部服务**。

【运行方式】
  # 只跑自包含用例（无需先起任何服务）
  make TARGET=host build/host/tests/integration/test_e2e_report
  ./build/host/tests/integration/test_e2e_report

  # 跑整个目录（前提：ivsboxd 必须先在跑，否则 test_ctl_snapshot 会报"没在跑"）
  build/host/ivsboxd &      # 宿主机：先起服务
  make itest                # 再构建并运行 tests/integration/*
  # 板端：/etc/init.d/S65ivsboxd start 后，直接跑 build/arm/tests/integration/*
  # 用例可选参数：test_ctl_snapshot [socket_path]（默认 /var/run/ivsbox/ctl）

【约定与注意事项】
  - 本目录**刻意不并入 make test**：目录里既有"需外部服务"的用例，并进去会让 CI 因
    "没起服务"而假红（见 Makefile `itest` 注释、架构 §16.1）。**需要按单用例粒度执行**
    时用上面第一条命令。
  - test_ctl_snapshot 对"服务尚未就绪"有重试（最多 5 s）；连接失败会明确提示
    "ivsboxd 没在跑"。
  - test_e2e_report 的 pty 只走 POSIX 接口（`posix_openpt/grantpt/unlockpt/ptsname_r`）；
    板端没有 `socat`，所以"串口对"一律用 pty（VM 上同理）。
  - test_e2e_report 会在 `$TMPDIR`（默认 `/tmp`）下建一个临时目录做持久队列，**用例
    结束前删除**；test_ctl_snapshot 不写文件。
  - 板端验证需先按 §S65ivsboxd 拉起服务，再运行"需外部服务"的用例。

【相关文档】
  - ivsbox/docs/功能开发计划.md（M1 退出条件第 2 条；§0.1 R1 行与 S3.6 行；§S3.6）
  - T113运维终端-系统架构设计.md §15.3、§16.1、§16.2、§18.1
