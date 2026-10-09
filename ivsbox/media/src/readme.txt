============================================================
media/src  ——  ivsbox-media 进程源码目录（当前仅入口占位桩）
============================================================

【目录作用】
  ivsbox-media 媒体进程的 C 源码目录。顶层 Makefile 用 `$(wildcard media/src/*.c)`
  收集本目录源码，编译链接为独立可执行 build/<target>/ivsbox-media。本目录独立编译，
  不链接三个业务静态库。

【文件说明】
  main.c    ivsbox-media 入口。**当前只是占位桩**：不含本地通道服务端、任务队列或
            RTSP/音频 pipeline，main() 仅打印 "ivsbox-media initialized" 后返回 0

【运行方式】
  make host / make arm    分别产出 build/host/ivsbox-media、build/arm/ivsbox-media
  运行：ivsbox-media（当前除打印启动信息外无实际功能）

【约定与注意事项】
  - 占位桩尚未实现任何媒体能力；控制循环、本地通道服务端、有界任务队列、资源限额
    均待按 §S6.1 落地。
  - 媒体前置参数（录像路数/码率/SD 容量/保留周期、音频硬件）以架构 §18.5/§18.6 为准，
    未冻结前不得凭猜测定数。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S6.1 及 M6 后续
