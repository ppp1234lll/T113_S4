============================================================
mk  ——  Makefile include 片段（工具链选择与通用编译规则）
============================================================

【目录作用】
  mk/ 存放被顶层 ivsbox/Makefile 用 include 引入的构建片段，本身不是构建入口。
  纯 GNU Make，**不递归 make**：顶层 Makefile 是唯一构建入口，子目录一律不放 Makefile。
  按职责分两类：工具链配置（host / tina-arm）与通用编译/归档/链接规则。
  本目录只影响构建过程，不参与运行期的分层与依赖方向。

【文件说明】
  rules.mk              通用编译/归档/链接规则与扩展钩子（V=1 显示完整命令）
  toolchain-host.mk     宿主机 gcc 工具链（单测 / ASan/UBSan / 静态分析）
  toolchain-tina-arm.mk 板端交叉工具链 arm-linux-gnueabi-（天嵌 Tina Linux）

【约定与注意事项】
  - 顶层 Makefile 先按 TARGET 选择 toolchain 片段，再 include rules.mk。
  - 子目录不放 Makefile；新增源文件无需改本目录，由顶层按目录通配发现。
  - 第三方头文件路径、以及"vendored 源码去掉 -Werror"规则在顶层 Makefile，不在本目录。
  - 交叉工具链前缀可用 CROSS= 覆盖，sysroot 用 SYSROOT= 指定；SANITIZE= 透传 -fsanitize=。
  - ASan 与 TSan 不能同时启用，-fanalyzer 也别与消毒器混用。

【相关文档】
  架构 T113运维终端-系统架构设计.md §15.5（依赖方向与链接顺序）；ivsbox/README.md「构建」「目录结构」
