============================================================
web/src  ——  ivsbox-web 进程源码目录（当前仅入口占位桩）
============================================================

【目录作用】
  ivsbox-web 管理进程的 C 源码目录。顶层 Makefile 用 `$(wildcard web/src/*.c)` 收集
  本目录源码，编译链接为独立可执行 build/<target>/ivsbox-web。本目录独立编译，
  不链接三个业务静态库。

【文件说明】
  main.c    ivsbox-web 入口。**当前只是占位桩**：不含 HTTP 服务、REST 路由或代理
            逻辑，main() 仅打印 "ivsbox-web initialized" 后返回 0

【运行方式】
  make host / make arm    分别产出 build/host/ivsbox-web、build/arm/ivsbox-web
  运行：ivsbox-web（当前除打印启动信息外无实际功能）

【约定与注意事项】
  - 占位桩尚未实现任何服务能力；HTTP 库选型、静态页/REST、受控代理均待落地。
  - 实现口径：数据一律走本地通道请求 ivsboxd/ivsbox-media，独立非特权用户运行，
    各类连接数/请求体/超时上限可配（功能开发计划.md §S5.1~§S5.4）。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S5.1~§S5.4
