============================================================
third_party/minmea  ——  轻量级 GPS / NMEA 0183 解析库（vendored）
============================================================

【目录作用】
  minmea 上游库的**原样内嵌副本**，供 M2-S2.5 的 src/modules/gps/iv_gps.c 解析 GPS NMEA
  语句。上游地址 https://github.com/kosma/minmea，固定 commit
  c43c9e7c5ed788122a9d8a5445679e1594f8a030（refs/heads/master，2026-09-30 取）。

【文件说明】
  minmea.c         上游库实现（一字未改；由 Makefile 通配编进 libivmodules）
  minmea.h         上游库公共头（一字未改；语句结构体与解析函数声明）
  README.ivsbox.md 本工程的本地登记说明：来源、commit、SHA256、许可证、取舍与所用 API
  LICENSE.MIT      上游随附的 MIT 许可证文本（上游同时提供，供许可证更替时参考）
  COPYING          上游主许可证：WTFPL v2（Do What The Fuck You Want To Public License v2）

【约定与注意事项】
  - **上游文件一字未改**，SHA256 见 README.ivsbox.md，可用
    `sha256sum third_party/minmea/minmea.c third_party/minmea/minmea.h` 复核。
  - **单独编译标志**：Makefile 对该目录去掉 `-Werror` 单独编译，保证不改上游即可编过。
  - **许可证需人工确认**：主许可证为 WTFPL v2（无 OSI 认证，部分企业白名单不接受），
    退路为依上游声明改 MIT 或换用其它 MIT/BSD 实现——**属合规判断，待人工拍板**。
  - 构建系统无关的上游文件（tests.c / example.c / compat/ / CMakeLists.txt / .github/ 等）
    未纳入，取舍见 README.ivsbox.md §四。
  - 升级上游前先对照 README.ivsbox.md §五列出的实际使用 API 与两条已依赖语义。

【相关文档】
  - ivsbox/third_party/minmea/README.ivsbox.md（本库权威登记文件）
  - ivsbox/third_party/readme.txt（第三方库入库纪律）
  - ivsbox/Makefile（vendored 源码编译规则）
  - ivsbox/docs/功能开发计划.md（M2-S2.5 GPS 模块）
