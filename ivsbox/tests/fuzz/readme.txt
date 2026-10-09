============================================================
tests/fuzz  ——  模糊测试：随机输入灌被测模块，撞崩溃/挂死
============================================================

【目录作用】
  模糊测试层，用伪随机字节流/事件序列持续驱动被测模块，配合 ASan/UBSan 暴露越界、
  UAF 等问题。harness 会调用被测模块，因此构建时链接三个静态库。运行时长由 wall
  clock 控制，结束打印统计，不产生持久化文件。

【文件说明】
  fuzz_frame.c   iv_frame 帧解析器模糊测试：xorshift64* 随机流，按 1/8 概率混入合法帧
                 提高同步字/CRC 路径命中率，随机 now_ms 喂 tick 覆盖超时路径
  fuzz_link.c    iv_link 可靠层模糊测试：随机选择 recv（随机块+1/8 混合法帧）/ send /
                 tick，回调内访问 data 边界，ASan/UBSan 下越界立即暴露
  .gitkeep       目录占位文件（保留空目录于 git；本目录当前并非空目录）

【运行方式】
  make fuzz                    构建本目录全部 harness（宿主）
  build/host/tests/fuzz/fuzz_frame [秒数]   直接跑，默认 10 s，0 只跑一轮自检
  build/host/tests/fuzz/fuzz_link  [秒数]   同上
  # 建议配合 `make asan` 口径重编后再跑，越界/UAF 才能被捕获

【约定与注意事项】
  - 每个 harness 用 xorshift64* 伪随机并做非零播种（全零态输出恒 0）。
  - fuzz 只保证"不崩溃、不挂死、ASan 零报告"，不做结果正确性断言。
  - 当前用例的验证证据见 功能开发计划.md §S2.2/§S2.3（10 s 灌流无 crash）。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S2.2（fuzz_frame）、§S2.3（fuzz_link）
  - T113运维终端-系统架构设计.md §16.1（模糊测试）
