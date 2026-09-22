# ivsbox-updater —— 升级代理（架构文档 §11）
#
# root 权限，仅在升级阶段启动。不接受任何来自网络或 Web 的路径/命令参数，
# 且必须自行重新验签（缺口 G3）。

add_executable(ivsbox-updater
    src/main.c
)

target_link_libraries(ivsbox-updater PRIVATE ivcore ivhal)

# TODO(M0): 密码库选型确定后接入（复用项目现有可信库）
# mbedTLS 或 OpenSSL：SHA-256 + Ed25519 / ECDSA 验签
