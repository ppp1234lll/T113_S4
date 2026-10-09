============================================================
tests/integration  ——  集成测试：需外部服务（ivsboxd）已在跑
============================================================

【目录作用】
  集成测试层，验的是"装好之后的进程"而非孤立模块，与 tests/unit 分工明确：
  unit 验模块本身（不依赖外部进程），integration 验真实的 ivsboxd 装配是否成立。
  本层用例是**客户端**，不自己起服务端，连本机通道与已运行的 ivsboxd 通信。

【文件说明】
  test_ctl_snapshot.c  ivsboxd 本地通道端到端往返（M1 退出条件第 2 条）：连
                       IV_CHAN_PATH_DEFAULT，验 system.snapshot 应答、request_id
                       原样回填、同连接二次请求、未实现方法无应答、连接可复用

【运行方式】
  # 前提：ivsboxd 必须先在跑
  build/host/ivsboxd &      # 宿主机：先起服务
  make itest                # 再构建并运行 tests/integration/*
  # 板端：/etc/init.d/S65ivsboxd start 后，直接跑 build/arm/tests/integration/*
  # 用例可选参数：test_ctl_snapshot [socket_path]（默认 /var/run/ivsbox/ctl）

【约定与注意事项】
  - 本目录**刻意不并入 make test**：其前提是"外部服务已在跑"，并进去会让 CI 因
    "没起服务"而假红（见 Makefile `itest` 注释、架构 §16.1）。
  - 用例对"服务尚未就绪"有重试（最多 5 s）；连接失败会明确提示"ivsboxd 没在跑"。
  - 测试不写任何文件，无临时产物需清理。
  - 板端验证需先按 §S65ivsboxd 拉起服务，再运行集成用例。

【相关文档】
  - ivsbox/docs/功能开发计划.md（M1 退出条件第 2 条；§0.1 R1 行）
  - T113运维终端-系统架构设计.md §15.3、§16.1
