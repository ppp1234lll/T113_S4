============================================================
web  ——  ivsbox-web 管理进程：REST API、静态页面、摄像机受控代理
============================================================

【目录作用】
  ivsbox-web 是三个常驻进程之一（管理面），以独立可执行、独立非特权用户运行。
  职责：对外提供 REST API 与静态页面，并作为摄像机的受控代理。数据一律经本地通道
  （include/ivsbox/iv_chan.h，AF_UNIX + SOCK_SEQPACKET）转发给 ivsboxd / ivsbox-media，
  不直接开数据库、不写配置文件。本目录只与 src/app（ivsboxd）平级，独立编译，
  不参与 libivcore/libivhal/libivmodules 分层链接。

【文件说明】
  本目录无直接源文件，只有下级源码子目录：
    src/    进程源码（入口 main.c，见 src/readme.txt）

【运行方式】
  make host    构建宿主机版本（产物 build/host/ivsbox-web）
  make arm     交叉编译板端版本（产物 build/arm/ivsbox-web）
  （守护与自启由 scripts/init.d 与 monitor.sh 负责）

【约定与注意事项】
  - 当前 src/main.c 只是入口占位桩，**进程功能尚未实现**（见 src/readme.txt）。
  - HTTP 库/uhttpd 选型、监听端口与各类上限、鉴权与审计口径见 功能开发计划.md
    §S5.1~§S5.4；数据面统一走本地通道，禁止直连数据库。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S5.1（ivsbox-web 服务）~§S5.4
  - T113运维终端-系统架构设计.md §15.5（独立可执行目标）
