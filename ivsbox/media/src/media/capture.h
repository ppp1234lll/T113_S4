#ifndef __CAPTURE_H__
#define __CAPTURE_H__

#include <stddef.h>
#include <linux/videodev2.h>

/* 摄像头节点名长度上限（`/dev/video31` 15 B，留一倍余量）。 */
#define CAPTURE_DEV_NAME 32

typedef struct {
    int fd;
    void *mem;
    size_t mem_len;
    struct v4l2_buffer buf;

    /* ---- 2026-10-28 热插拔支持 ----
     * `device` 记住**当前真正打开的那个节点**。摄像头不一定 `/dev/video0`：
     * 后插的 UVC 可能被排到 video2/video4（板端 `tvd_0` 就占过一个号）。
     * fd < 0 时本字段是"优先尝试的节点"（`capture_init` 传进来的那个），
     * 打开成功后被改写成实际命中的节点。 */
    char device[CAPTURE_DEV_NAME];
    int  no_cam;   /* 1 = 上一次找设备没找到（只用于日志降噪，不影响逻辑） */
} capture_context_t;

/*
 * 打开摄像头。**允许失败**：设备不在（没插/后插）返回 -1，进程照常跑；
 * 之后每次 `capture_process()` 会自己再找一遍（见该函数说明）。
 * 重复调用等价于"重开"（先释放旧设备）。
 */
int capture_init(capture_context_t *ctx, const char *device);

/*
 * 抓一帧存成 `filename`。
 *
 * 两件事在 2026-10-28 补齐（用户口径）：
 *   1. **父目录不在就建**（`mkdir -p` 语义）：SD 卡重插、目录被删、工厂复位后
 *      第一次抓拍都不能因为"没有目录"而失败；
 *   2. **设备后插也能用**：`fd < 0` 时先重新找一遍节点（优先上次那个，
 *      然后扫 `/dev/video0..31`，只认「支持 VIDEO_CAPTURE+STREAMING 且能出
 *      MJPEG」的节点）；抓拍中途掉线（拔掉）会让下一次调用重新找。
 *      因此**没有"必须先插摄像头再起进程"的顺序要求**。
 */
int capture_process(capture_context_t *ctx, const char *filename);

/* 释放设备（幂等）。 */
int capture_fini(capture_context_t *ctx);

#endif
