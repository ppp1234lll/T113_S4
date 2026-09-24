# 测试体系（架构文档 §16）
#
#   unit/         帧解析、配置事务、探活统计、网络切换、自愈规则、IPC 编解码
#   fuzz/         UART / 平台 / IPC / ONVIF / SNMP 输入边界、半包、粘包、超长数据
#   integration/  模拟器 + SQLite + 主控与媒体 IPC 联调
#
# 主机侧构建；板端构建时 IVSBOX_BUILD_TESTS=OFF。

add_executable(test_ipc_hdr
    unit/test_ipc_hdr.c
)
target_link_libraries(test_ipc_hdr PRIVATE ivcore)

add_test(NAME ipc_hdr COMMAND test_ipc_hdr)

# S02 步骤 1：CRC（zlib crc32 包装）；ZLIB 依赖经 ivcore PUBLIC 传递
add_executable(test_crc
    unit/test_crc.c
)
target_link_libraries(test_crc PRIVATE ivcore ivhal)

add_test(NAME crc COMMAND test_crc)

# TODO(M1): 配置事务、帧解析、探活统计、自愈规则的单元测试
# TODO(M2): 集成测试（uart_simulator + platform_simulator）
# TODO(M2): fuzz 目标（libFuzzer 或 afl，主机侧）
