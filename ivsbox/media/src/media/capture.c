#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "defs.h"
#include "logger.h"

int capture_init(capture_context_t *ctx, const char *device) {
    if (!ctx) return -1;

    // 1. 打开设备
    ctx->fd = open(device, O_RDWR);
    if (ctx->fd < 0) {
        log_err("capture init: failed to open %s (%s)", device, strerror(errno));
        return -1;
    }

    // 2. 设置格式 (使用 defs.h 中的宽高配置)
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAPTURE_WIDTH;
    fmt.fmt.pix.height = CAPTURE_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        log_err("capture init: S_FMT failed (%s)", strerror(errno));
        goto err;
    }

    // 3. 申请 1 个缓冲区
    struct v4l2_requestbuffers req = {0};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        log_err("capture init: REQBUFS failed (%s)", strerror(errno));
        goto err;
    }

    // 4. 查询并映射内存
    memset(&ctx->buf, 0, sizeof(ctx->buf));
    ctx->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ctx->buf.memory = V4L2_MEMORY_MMAP;
    ctx->buf.index = 0;
    if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &ctx->buf) < 0) {
        log_err("capture init: QUERYBUF failed (%s)", strerror(errno));
        goto err;
    }

    ctx->mem_len = ctx->buf.length;
    ctx->mem = mmap(NULL, ctx->buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, ctx->buf.m.offset);
    if (ctx->mem == MAP_FAILED) {
        log_err("capture init: mmap failed (%s)", strerror(errno));
        goto err;
    }

    // 5. 开启流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        log_err("capture init: STREAMON failed (%s)", strerror(errno));
        goto err;
    }

    log_info("capture init: v4l2 device %s ready (%dx%d MJPEG)", 
             device, CAPTURE_WIDTH, CAPTURE_HEIGHT);
    return 0;

err:
    if (ctx->fd >= 0) close(ctx->fd);
    return -1;
}

int capture_process(capture_context_t *ctx, const char *filename) {
    if (!ctx || ctx->fd < 0) {
        log_err("capture process: invalid context or device closed");
        return -1;
    }

    // 使用 defs.h 中的跳帧配置
    const int skip_frames = CAPTURE_SKIP_FRAMES; 
    int got = 0;

    log_info("capture process: starting snapshot to %s (skipping %d frames)", filename, skip_frames);

    for (int i = 0; i < skip_frames + 1; i++) {
        // 入队
        if (ioctl(ctx->fd, VIDIOC_QBUF, &ctx->buf) < 0) {
            log_err("capture process: QBUF failed at frame %d (%s)", i, strerror(errno));
            return -1;
        }

        // 出队 (阻塞)
        if (ioctl(ctx->fd, VIDIOC_DQBUF, &ctx->buf) < 0) {
            log_err("capture process: DQBUF failed at frame %d (%s)", i, strerror(errno));
            return -1;
        }

        // 只保存最后一帧
        if (i == skip_frames) {
            FILE *f = fopen(filename, "wb");
            if (f) {
                size_t written = fwrite(ctx->mem, 1, ctx->buf.bytesused, f);
                fclose(f);
                if (written == ctx->buf.bytesused) {
                    log_info("capture process: snapshot saved successfully (%u bytes)", ctx->buf.bytesused);
                    got = 1;
                } else {
                    log_err("capture process: file write incomplete");
                }
            } else {
                log_err("capture process: failed to open %s for writing (%s)", filename, strerror(errno));
            }
        }
    }
    return got ? 0 : -1;
}

int capture_fini(capture_context_t *ctx) {
    if (ctx && ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        munmap(ctx->mem, ctx->mem_len);
        close(ctx->fd);
        ctx->fd = -1;
        log_info("capture fini: device closed and resources released");
    }
    return 0;
}
