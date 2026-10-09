============================================================
src  ——  ivsboxd 主控进程源码根（四层源码树）
============================================================

【目录作用】
  src/ 是 ivsboxd（主控进程）的源码根，按分层拆成四个子目录：
  app（入口/装配）→ core → hardware → modules。
  四个子目录分别编入不同目标：core→libivcore.a、hardware→libivhal.a、modules→libivmodules.a；
  app 的 .c 不参与任何静态库，只与三个 .a 一起链接成可执行文件 ivsboxd。
  依赖方向（由链接顺序写死）：libivcore(仅 libc) → libivhal → libivmodules → ivsboxd。

【文件说明】
  app/                  主控进程入口与骨架装配（见 src/app/readme.txt）
  core/                 平台无关基础件，仅依赖 libc（见 src/core/readme.txt）
  hardware/             Linux 硬件抽象层 HAL（见 src/hardware/readme.txt）
  modules/              业务模块，一个子目录一个模块（见 src/modules/readme.txt）

【约定与注意事项】
  - 分层纪律：core 仅 libc（禁 pthread、禁第三方库）；hal 可依赖 core/libc；
    modules 可依赖前两者 + third_party 里 vendored 的第三方库。
  - 源文件由顶层 Makefile 按目录通配发现，各子目录不放 Makefile。
  - 代码风格以 ivsbox/.clang-format 为准；换行/编码归一由仓库 .gitattributes 管。

【相关文档】
  架构 §15.5（分层与链接顺序）；ivsbox/README.md「目录结构」「构建」
