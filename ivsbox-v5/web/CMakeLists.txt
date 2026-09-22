# ivsbox-web —— 管理面（架构文档 §4.4、§9）
#
# 无特权进程。不访问串口、watchdog、配置密钥，不直接打开控制库或媒体库。
# HTTP 服务选定一种并固化版本（§14）：BSP 的 uhttpd 或成熟嵌入式 HTTP 库。

add_executable(ivsbox-web
    src/main.c
)

target_link_libraries(ivsbox-web PRIVATE ivcore)

# TODO(M0): HTTP 选型确定后接入（二选一，不要同时引入）
#   方案 A: 复用 BSP 的 uhttpd，本进程只实现 CGI/后端接口
#   方案 B: 内嵌 civetweb / mongoose，独立监听 127.0.0.1 或局域网端口
