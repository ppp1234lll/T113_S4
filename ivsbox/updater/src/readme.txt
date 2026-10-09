============================================================
updater/src  ——  ivsbox-updater 升级器源码目录（当前仅入口占位桩）
============================================================

【目录作用】
  ivsbox-updater 升级器的 C 源码目录。顶层 Makefile 用 `$(wildcard updater/src/*.c)`
  收集本目录源码，编译链接为独立可执行 build/<target>/ivsbox-updater。本目录独立编译，
  不链接三个业务静态库。

【文件说明】
  main.c    ivsbox-updater 入口。**当前只是占位桩**：不含验签、安装、版本切换或
            回滚逻辑，main() 仅打印 "ivsbox-updater initialized" 后返回 0

【运行方式】
  make host / make arm    分别产出 build/host/ivsbox-updater、build/arm/ivsbox-updater
  运行：ivsbox-updater（当前除打印启动信息外无实际功能）

【约定与注意事项】
  - 占位桩尚未实现任何升级能力；包验签、双版本切换、断电回滚均待按 §S7.1~§S7.3 落地。
  - 该进程按需 root 运行，涉及签名/槽位等安全与平台事实，须先查天嵌官方资料或板端
    实测确认，不得凭猜测写死。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S7.1~§S7.3
