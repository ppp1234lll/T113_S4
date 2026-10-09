============================================================
third_party  ——  vendored 第三方库目录（原样内嵌，可 SHA256 复核）
============================================================

【目录作用】
  存放以**原样内嵌（vendored）**方式引入本工程的上游第三方库，按 `third_party/<库名>/`
  分子目录组织。库源码由顶层 Makefile 通配编进 libivmodules，供 modules 层调用
  （架构 §15.5 允许 modules 层使用外部代码；core / hal 仍只允许 libc）。
  当前仅含 minmea（NMEA 解析）。

【文件说明】
  minmea/          轻量级 GPS / NMEA 0183 解析库（见 minmea/readme.txt）

【约定与注意事项】（第三方库入库纪律）
  - **上游文件一字未改**：不改上游源码，否则 SHA256 不再可复核、"这就是上游那一份"不成立。
  - **SHA256 可复核**：每个库在本地登记文件中记录上游地址、固定 commit 与各文件 SHA256，
    便于随时复核是否被改动。
  - **单独编译标志**：Makefile 对 `third_party/%` 单独一条规则，编译时去掉本工程的 `-Werror`
    （保留 -Wall -Wextra 只为看得见警告），以免为编过而改上游文件。该纪律对后续所有
    vendored 库同样适用。
  - **许可证需人工确认**：各库许可证以本地登记文件为准，是否符合同意/公司白名单属合规判断，
    不由模块自行决定，须逐库人工拍板（minmea 的 WTFPL v2 见其登记文件）。
  - 因"原样不改"，`git diff --check` 可能在某些上游文件上稳定报尾随空白，属预期。

【相关文档】
  - ivsbox/third_party/minmea/README.ivsbox.md（来源、commit、SHA256、许可证说明）
  - ivsbox/Makefile（THIRD_PARTY_INC / THIRD_PARTY_SRCS / CFLAGS_THIRD_PARTY 规则）
  - 仓库根 AGENTS.md（规则 2 / 3；规则 5 平台事实优先查天嵌官方资料）
