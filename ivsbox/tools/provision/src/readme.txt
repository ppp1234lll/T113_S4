============================================================
tools/provision/src  ——  出厂工装源码目录（当前仅入口占位桩）
============================================================

【目录作用】
  ivsbox-provision 工装的 C 源码目录。顶层 Makefile 用 `$(wildcard tools/provision/src/*.c)`
  收集本目录源码，编译链接为独立可执行 build/<target>/ivsbox-provision。本目录不参与
  libivcore/libivhal/libivmodules 三层库，仅按需构建。

【文件说明】
  main.c    ivsbox-provision 入口。**当前只是占位桩**：不含任何参数解析或身份写入
            逻辑，main() 仅打印 "ivsbox-provision initialized" 后返回 0

【运行方式】
  make host / make arm    分别产出 build/host/ivsbox-provision、build/arm/ivsbox-provision
  运行：ivsbox-provision（当前除打印启动信息外无实际功能）

【约定与注意事项】
  - 占位桩尚未实现任何量产功能；设备 ID/MAC 写入、工厂模式标记校验、密钥落盘等
    均待后续按 §S5.5 落地。
  - 实现时身份写入的落点、结构与校验方式以 功能开发计划.md §S5.5 为纲，平台事实
    （分区节点、容量等）需先查天嵌官方资料或板端实测确认后再写死。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S5.5
