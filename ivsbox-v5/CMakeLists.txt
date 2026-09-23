cmake_minimum_required(VERSION 3.16)

project(ivsbox
    VERSION 0.1.0
    DESCRIPTION "Intelligent Video Surveillance Box (T113 / Tina Linux)"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(IVSBOX_BUILD_TESTS "构建主机侧单元测试" ON)
option(IVSBOX_WERROR       "编译告警视为错误"   ON)

# ----------------------------------------------------------------------------
# 编译选项：与 v3 脚手架一致，主机与板端共用
# ----------------------------------------------------------------------------
add_compile_options(
    -Wall
    -Wextra
    -Wshadow
    -Wpointer-arith
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wvla
    -Wformat=2
)

if(IVSBOX_WERROR)
    add_compile_options(-Werror)
endif()

# ----------------------------------------------------------------------------
# POSIX / GNU 特性宏
#   CMAKE_C_EXTENSIONS OFF 会传 -std=c11，此时 __STRICT_ANSI__ 被定义，
#   glibc 的 features.h 便不再暴露 clock_gettime / CLOCK_MONOTONIC 等 POSIX 声明
#   （-Werror 下直接编译失败）。后续 S03 要用的 epoll / timerfd / signalfd /
#   accept4 同属这一类，故板端与 Linux 主机统一开启 _GNU_SOURCE。
#   Windows(MinGW) 主机走 std=c11 的 ISO 分支，不需要也不启用该宏。
# ----------------------------------------------------------------------------
if(UNIX)
    add_compile_definitions(_GNU_SOURCE)
endif()

set(IVSBOX_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ----------------------------------------------------------------------------
# 分层与边界（架构文档 §6）
#   ivcore  : 事件、队列、定时器、日志、配置、状态机、CRC
#             —— 不得依赖 Linux HAL，可在开发机单元测试
#   ivhal   : serial、netlink、watchdog、fs、clock、capability
#             —— 不得包含业务策略
#   ivmods  : 12 个业务模块，只经 include/ivsbox/ 公共头互访
#
#   TODO(G7): 目前仅靠约定，尚无编译期强制。第一版程序跑通后补
#             scripts/ci/ 检查脚本与私有头隔离。
# ----------------------------------------------------------------------------

add_library(ivcore STATIC
    src/core/iv_err.c
    src/core/iv_log.c
)
target_include_directories(ivcore PUBLIC ${IVSBOX_INCLUDE_DIR})

add_library(ivhal STATIC
    src/hal/iv_clock.c
)
target_include_directories(ivhal PUBLIC ${IVSBOX_INCLUDE_DIR})
target_link_libraries(ivhal PUBLIC ivcore)

# 模块源文件按目录自动收集；目录为空时跳过，便于分批实现
file(GLOB_RECURSE IVSBOX_MODULE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/*/*.c")

if(IVSBOX_MODULE_SOURCES)
    add_library(ivmods STATIC ${IVSBOX_MODULE_SOURCES})
    target_include_directories(ivmods PUBLIC ${IVSBOX_INCLUDE_DIR})
    target_link_libraries(ivmods PUBLIC ivcore ivhal)
    message(STATUS "ivsbox: 已纳入 ${CMAKE_CURRENT_SOURCE_DIR}/src/modules 下的模块源文件")
else()
    message(STATUS "ivsbox: src/modules 暂无源文件，跳过 ivmods 目标")
endif()

# ----------------------------------------------------------------------------
# 主控进程
# ----------------------------------------------------------------------------
add_executable(ivsboxd
    src/app/main.c
)
target_link_libraries(ivsboxd PRIVATE ivcore ivhal)
if(TARGET ivmods)
    target_link_libraries(ivsboxd PRIVATE ivmods)
endif()

# ----------------------------------------------------------------------------
# 其余进程与工具（各自独立子目录）
# ----------------------------------------------------------------------------
add_subdirectory(media)
add_subdirectory(web)
add_subdirectory(updater)
add_subdirectory(tools)

# TODO(M2): tools/uart_simulator 与 tools/platform_simulator 是板间与平台两侧
#           并行开发的硬前置，实现后接入 tools/CMakeLists.txt。

if(IVSBOX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
