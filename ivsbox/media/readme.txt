============================================================
media  ——  ivsbox-media 媒体进程：RTSP 抓图、断网录像、媒体索引、音频广播
============================================================

【目录作用】
  ivsbox-media 是三个常驻进程之一（媒体面），以独立可执行运行。职责：RTSP 抓图、
  断网录像、媒体索引、音频广播。作为本地通道的服务端处理媒体类请求（record.start/stop、
  snapshot.take、media.list、audio.play/stop 等），每路 RTSP 会话独立 pipeline，数量受
  媒体路数上限约束；崩溃只重启自身。本目录独立编译，不参与三层业务库链接。

【文件说明】
  本目录无直接源文件，只有下级源码子目录：
    src/    进程源码（入口 main.c，见 src/readme.txt）

【运行方式】
  make host    构建宿主机版本（产物 build/host/ivsbox-media）
  make arm     交叉编译板端版本（产物 build/arm/ivsbox-media）
  （守护与自启由 scripts/init.d 与 monitor.sh 负责，崩溃由守护循环拉起）

【约定与注意事项】
  - 当前 src/main.c 只是入口占位桩，**进程功能尚未实现**（见 src/readme.txt）。
  - 媒体能力、资源限额、通道命令与心跳检测口径见 功能开发计划.md §S6.1 及其后续；
    录像路数/码率/SD 容量等前置见架构 §18.5，未冻结前不得凭猜测定数。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S6.1（ivsbox-media 骨架）及 M6 各步
  - T113运维终端-系统架构设计.md §15.5（独立可执行目标）
