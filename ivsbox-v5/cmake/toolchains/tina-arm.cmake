# Tina Linux / T113 交叉编译工具链
#
# 用法：
#   cmake -S . -B build/arm \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/tina-arm.cmake \
#         -DIVSBOX_BUILD_TESTS=OFF
#
# 前置：Tina SDK 已编译过一次，sysroot 已生成。
# 通过环境变量定位 SDK 根目录（不要写死在仓库里）：
#   TINA_SDK_ROOT=/path/to/tina-sdk
#
# 若 SDK 工具链前缀不同，用 IVSBOX_CROSS_PREFIX 覆盖，例如：
#   -DIVSBOX_CROSS_PREFIX=arm-openwrt-linux-muslgnueabi-

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED IVSBOX_CROSS_PREFIX)
    set(IVSBOX_CROSS_PREFIX "arm-openwrt-linux-" CACHE STRING "交叉工具链前缀")
endif()

if(NOT DEFINED ENV{TINA_SDK_ROOT})
    message(FATAL_ERROR
        "未设置 TINA_SDK_ROOT。请先 'source build/envsetup.sh && lunch' 后导出 SDK 根目录。")
endif()

set(TINA_SDK_ROOT "$ENV{TINA_SDK_ROOT}")

# sysroot：优先使用 SDK 的 staging_dir，缺失时回退到工具链自带的 sysroot
set(_IVSBOX_SYSROOT "${TINA_SDK_ROOT}/out/t113/staging_dir/target")
if(NOT EXISTS "${_IVSBOX_SYSROOT}")
    message(WARNING "未找到 ${_IVSBOX_SYSROOT}，将使用工具链自带 sysroot")
endif()

# 交叉工具链可执行文件由 PATH 提供（Tina SDK 的 staging_dir/host/bin 已在 PATH 中）
set(CMAKE_C_COMPILER   "${IVSBOX_CROSS_PREFIX}gcc")
set(CMAKE_AR           "${IVSBOX_CROSS_PREFIX}ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${IVSBOX_CROSS_PREFIX}ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP        "${IVSBOX_CROSS_PREFIX}strip"  CACHE FILEPATH "")

# 目标环境：只查 sysroot，不查宿主机
set(CMAKE_FIND_ROOT_PATH "${_IVSBOX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 板端不构建测试
set(IVSBOX_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# 板端优化：A7 无 NEON 依赖，关闭不必要特性；保留符号表便于 core dump 回溯（G10）
set(CMAKE_C_FLAGS_RELEASE "-O2 -g -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections")
