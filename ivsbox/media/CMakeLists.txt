cmake_minimum_required(VERSION 3.8)

# 项目名称和语言
project(prj_media C)

# 定义可执行文件名称变量
set(APP_NAME media)

# 架构识别逻辑
if(CMAKE_CROSSCOMPILING)
    set(TARGET_ARCH "arm")
    message(STATUS "交叉编译环境已启动 (arm)")
else()
    set(TARGET_ARCH "x86")
    message(STATUS "本地编译环境 (x86)")
endif()

# 根据架构设置不同的路径和配置
if(TARGET_ARCH MATCHES "arm" OR TARGET_ARCH MATCHES "aarch64")
    set(FFMPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/lib_arm")
elseif(TARGET_ARCH MATCHES "x86" OR TARGET_ARCH MATCHES "amd64")
    set(FFMPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/lib_x86")
else()
    message(FATAL_ERROR "Unsupported architecture: ${TARGET_ARCH}")
endif()

# 收集所有的源文件
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.c")

# 生成可执行程序
add_executable(${APP_NAME} main.c ${SOURCES})

# 设置头文件目录
target_include_directories(${APP_NAME} PUBLIC include)
target_include_directories(${APP_NAME} PRIVATE
    src
    third_party/ffmpeg/include
)

# 设置链接库 (必须在 add_executable 之后)
target_link_libraries(${APP_NAME} PRIVATE 
    ${FFMPEG_LIB_DIR}/libavformat.a
    ${FFMPEG_LIB_DIR}/libavcodec.a
    ${FFMPEG_LIB_DIR}/libavutil.a
    ${FFMPEG_LIB_DIR}/libavdevice.a
    m
    z
    dl
    pthread
)
