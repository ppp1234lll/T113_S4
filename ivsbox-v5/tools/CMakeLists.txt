# IVSBox 主机侧工具
#
# 出厂工装：写设备 ID 与 MAC，仅工厂模式可运行（架构文档 §13.1）。

add_executable(ivsbox-provision
    provision/src/main.c
)

target_link_libraries(ivsbox-provision PRIVATE ivcore)

# TODO(M2): 以下两个模拟器是「两侧并行开发」的硬前置，优先级等同于业务代码
#   uart_simulator/      板间协议模拟器：模拟采集板，用于 T113 侧先行开发
#   platform_simulator/  平台协议模拟器：模拟平台侧，用于验证补传与命令分流
# 实现后在此 add_executable 接入（主机侧构建，不进板端 ipk）。
