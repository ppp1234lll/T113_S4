============================================================
updater  ——  ivsbox-updater 升级器：验签、双版本切换、回滚
============================================================

【目录作用】
  ivsbox-updater 是升级器进程，按需以 root 运行，负责升级包的验签、安装与双版本
  切换、失败回滚。分工上 ivsboxd **只协调**（收命令/落一个请求文件），真正改盘的
  安装归本进程 —— 因为"换掉正在运行的程序"这段危险动作需要它自己的生命周期：
  不该继承主控的看门狗 fd，也不该在跑 reactor 的进程里阻塞。
  它独立编译成独立可执行目标，与三层业务库（libivcore/libivhal/libivmodules）一起
  链接，但不引用 ivsboxd 的 main/装配代码。

【文件说明】
  本目录无直接源文件，只有下级源码子目录：
    src/    升级器源码（命令行入口 main.c，见 src/readme.txt）

【运行方式】
  make host    构建宿主机版本（产物 build/host/ivsbox-updater）
  make arm     交叉编译板端版本（产物 build/arm/ivsbox-updater）

  常规调用者是 `scripts/monitor.sh`（消费 `<data_root>/ota/request` 里的请求）与
  `scripts/ivsbox-run.sh`（启动包装的 boot-begin/boot-ok/boot-fail）。手工排查时：

    ivsbox-updater status [--pending]
    ivsbox-updater install <包> [--restart]
    ivsbox-updater boot-begin | boot-ok | boot-fail | rollback

  **升级的判据只有一份实现**，在 `src/modules/upd/iv_upd.c`；本目录只做参数解析与
  输出（见 src/readme.txt）。

【约定与注意事项】
  - 安装/回滚必须由**引导副本**执行：`<app_root>/bin/ivsbox-updater`（裸布局那份，
    不参与版本轮换）。用 `releases/<ver>/bin/` 里的那一份去救人，等于让"待验对象"
    给自己判卷。
  - `install --restart` 会调 `/etc/init.d/S65ivsboxd restart`（固定字符串，不拼外部
    输入）；不带 `--restart` 时只改盘，由 monitor 自然重启。
  - `--pubkey-hex` 只给联调/产线用，命令行会明确告警；量产公钥由发布侧产出后写进
    `iv_ota_builtin_pubkey()`，**私钥绝不入库**。
  - 依赖 OpenSSL（Ed25519 走 EVP）。板端 sysroot 是 1.1.1a、宿主 3.0.2，两边都在
    `mk/toolchain-*.mk` 的 `LDLIBS` 里加了 `-lcrypto`。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S7.1（包格式与验签）、§S7.2（双版本切换）、§S7.3（回滚）
  - ivsbox/include/ivsbox/iv_upd.h（落盘顺序与返回码的权威描述）
  - ivsbox/src/modules/upd/readme.txt
  - T113运维终端-系统架构设计.md §11.2（应用级双版本）、§15.5（独立可执行目标）
