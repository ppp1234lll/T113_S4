# Tina Linux / T113 交叉编译工具链
#
# 用法：
#   cmake -S . -B build/arm \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/tina-arm.cmake \
#         -DIVSBOX_BUILD_TESTS=OFF
#
# 工具链前缀（实测值，可用 -DIVSBOX_CROSS_PREFIX= 覆盖）：
#   天嵌 TQT113 虚拟机：arm-linux-gnueabi-   （gcc 7.3.1 Linaro，软浮点，glibc 2.25）
#   全志原厂 Tina SDK  ：arm-openwrt-linux- 或 arm-openwrt-linux-muslgnueabi-
#
# sysroot 按以下顺序解析，命中即用：
#   1) -DIVSBOX_SYSROOT=<路径>（默认值见下方 IVSBOX_SYSROOT）
#   2) 已导出 TINA_SDK_ROOT 时，用 SDK 的 out/t113/staging_dir/target
#   3) 都不可用则告警，退回工具链自带 sysroot

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED IVSBOX_CROSS_PREFIX)
    set(IVSBOX_CROSS_PREFIX "arm-linux-gnueabi-" CACHE STRING "交叉工具链前缀")
endif()

if(NOT DEFINED IVSBOX_SYSROOT)
    set(IVSBOX_SYSROOT "/opt/EmbedSky/Tina/arm-buildroot-linux-gnueabi/sysroot"
        CACHE PATH "目标 sysroot（天嵌 TQT113 虚拟机默认位置）")
endif()

set(_IVSBOX_SYSROOT "")
if(EXISTS "${IVSBOX_SYSROOT}")
    set(_IVSBOX_SYSROOT "${IVSBOX_SYSROOT}")
elseif(DEFINED ENV{TINA_SDK_ROOT})
    set(_IVSBOX_SYSROOT "$ENV{TINA_SDK_ROOT}/out/t113/staging_dir/target")
    if(NOT EXISTS "${_IVSBOX_SYSROOT}")
        message(WARNING "未找到 ${_IVSBOX_SYSROOT}（部分 SDK 的 out 目录带板型后缀）")
        set(_IVSBOX_SYSROOT "")
    endif()
endif()

if(_IVSBOX_SYSROOT)
    message(STATUS "ivsbox: sysroot = ${_IVSBOX_SYSROOT}")
    set(CMAKE_SYSROOT "${_IVSBOX_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${_IVSBOX_SYSROOT}")
else()
    message(WARNING
        "未解析到 sysroot（IVSBOX_SYSROOT=${IVSBOX_SYSROOT}），改用工具链自带 sysroot；"
        "链接期若报缺库，请显式 -DIVSBOX_SYSROOT=<路径>")
endif()

# 交叉工具链可执行文件由 PATH 提供
set(CMAKE_C_COMPILER   "${IVSBOX_CROSS_PREFIX}gcc")
set(CMAKE_AR           "${IVSBOX_CROSS_PREFIX}ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${IVSBOX_CROSS_PREFIX}ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP        "${IVSBOX_CROSS_PREFIX}strip"  CACHE FILEPATH "")

# 目标环境：只查 sysroot，不查宿主机（CMAKE_FIND_ROOT_PATH 已在上方按 sysroot 设置）
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 板端不构建测试
set(IVSBOX_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# 板端优化：A7 无 NEON 依赖，关闭不必要特性；保留符号表便于 core dump 回溯（G10）
set(CMAKE_C_FLAGS_RELEASE "-O2 -g -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections")
