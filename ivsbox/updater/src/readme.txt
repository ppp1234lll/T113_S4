============================================================
updater/src  ——  ivsbox-updater 升级器源码目录（命令行入口）
============================================================

【目录作用】
  ivsbox-updater 升级器的 C 源码目录。顶层 Makefile 用 `$(wildcard updater/src/*.c)`
  收集本目录源码，链接 `-livmodules -livhal -livcore` 后产出独立可执行
  build/<target>/ivsbox-updater。

【文件说明】
  main.c    ivsbox-updater 命令行入口。**只做参数解析与输出**，不含任何安装语义：
            子命令 install / adopt / boot-begin / boot-ok / boot-fail / rollback /
            status（[--pending]）；
            选项 --app-root/--data-root/--lib-paths/--model/--hw/--max-boot/
                 --pubkey-hex/--fault-at/--restart/--reason/--version/-h。
            退出码：0 成功、1 用法参数错、2 操作失败（原因打到 stderr，
            形如 `install: failed (bad-package/ESIG)`）。
            `boot-begin` 成功时**只往 stdout 打一行**（程序目录绝对路径），
            好让 `DIR=$(... boot-begin ...)` 直接取用；其余诊断一律走 stderr。

【为什么这里没有实现】
  安装/切换/回滚的判据**只能有一份实现**，放在 `src/modules/upd/iv_upd.c`
  （进 libivmodules，可单测）。本目录若是自己再判一遍，单测钉住的就不是板端真跑
  的那份代码 —— "单测全绿、现场照坏"就是这么来的。

【运行方式】
  make host / make arm    分别产出 build/host/ivsbox-updater、build/arm/ivsbox-updater
  运行：ivsbox-updater <命令> [选项]（示例见 updater/readme.txt）

【约定与注意事项】
  - 本进程按需 root 运行，会改 rootfs 的程序目录并切符号链接。
  - `--restart` 走 `/etc/init.d/S65ivsboxd restart` 这条**固定字符串**，不拼接外部
    输入（命令行参数若参与拼串就是注入面）。
  - `--fault-at` 是掉电注入口，属验证用：它返回 EFAULT 且不做任何清理，
    用来复现"升级到一半掉电"。生产不带该参数。
  - 公钥/型号/硬件版本等平台事实以板端实测与发布侧产出为准，不得凭猜测写死。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S7.2 / §S7.3
  - ivsbox/include/ivsbox/iv_upd.h、ivsbox/src/modules/upd/readme.txt
