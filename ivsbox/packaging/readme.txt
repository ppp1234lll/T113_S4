============================================================
packaging  ——  make package 产物归集目录（构建产物，不入库）
============================================================

【目录作用】
  顶层 Makefile 的 `make package` 目标把 ARM 交叉编译产物归集到本目录，供 OTA 升级包
  打包使用。归集内容为：bin/（ivsboxd 等 ARM 可执行文件）、scripts/（start_ivsbox.sh、
  monitor.sh）、init.d/（S65ivsboxd）。
  本目录内容属**构建产物，不入库**（make clean 会删除整个 packaging/），当前仅以
  .gitkeep 保留目录结构，**为空占位**。

【文件说明】
  .gitkeep         目录占位文件（当前为空占位；构建产物由 make package 生成，
                   执行后会出现 bin/、scripts/、init.d/ 三个子目录）

【约定与注意事项】
  - 生成方式：`make package`（先 `make arm`，再在 TARGET=arm 子 make 内 package-copy）。
  - package-copy 自带 **readelf 断言**：产物不是 `Machine: ARM` 则打包失败并报错，
    避免悄悄发出跑不起来的 x86 包。
  - 构建产物不入库，须被 .gitignore 覆盖；提交时不得夹带本目录内容（AGENTS.md 规则 3/6）。
  - `make clean` 会 `rm -rf build/ packaging/`，执行后本目录仅剩 .gitkeep。

【相关文档】
  - ivsbox/Makefile（package / package-copy / clean 目标）
  - ivsbox/README.md（目录结构与构建命令）
  - 仓库根 AGENTS.md（规则 3 提交范围、规则 6 清理构建产物）
