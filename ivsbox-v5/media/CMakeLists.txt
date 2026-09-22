# ivsbox-media —— 媒体面（架构文档 §4.3、§8）
#
# 资源隔离：本进程须由 cgroup 限制 CPU 与内存（缺口 G1 的数值待实测后确定）。
# 依赖：FFmpeg libavformat（RTSP 直封装，不做转码）、ALSA（音频）。
#       具体依赖在 M0 BSP 能力清单确认后再接入，当前不强制查找。

add_executable(ivsbox-media
    src/main.c
)

target_link_libraries(ivsbox-media PRIVATE ivcore)

# TODO(M0): BSP 能力确认后启用
# find_package(PkgConfig REQUIRED)
# pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)
# target_link_libraries(ivsbox-media PRIVATE ${FFMPEG_LIBRARIES})
# target_include_directories(ivsbox-media PRIVATE ${FFMPEG_INCLUDE_DIRS})
# target_link_libraries(ivsbox-media PRIVATE asound)
