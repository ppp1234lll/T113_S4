/*
 * V4L2 MJPEG 抓图（收编自 bird_media，2026-10-28 按用户口径补两件事）
 *
 * ============================ 这次改了什么、为什么 ============================
 * 板端实测（2026-10-28，192.168.1.120）两个缺陷，都不是"参数没配对"：
 *
 *   ① **没有目录就失败**：`/mnt/sdcard/snap` 不存在时 `fopen()` 直接 ENOENT，
 *      进程回了 HTTP 200 但文件没落盘，主板侧只看到一句
 *      `capture: peer 200 but /mnt/sdcard/snap/snap_…jpg missing`。
 *      ⇒ `capture_process()` 写文件前先按 `mkdir -p` 语义建父目录。
 *
 *   ② **必须先插摄像头再起进程**：`media_init()` 只在启动时 `capture_init()`
 *      一次，设备不在就永远 `fd = -1`；17:43 插上的摄像头，11:11 起的进程
 *      直到重启媒体进程才认。
 *      ⇒ `capture_process()` 在 `fd < 0` 时**先重新找一遍设备**；抓拍中途掉线
 *      （拔掉）时释放设备，下一次调用再找。**没有顺序要求**了。
 *
 * 找设备不能只看 `/dev/video0`：后插的 UVC 会被排到空出来的号上（板端
 * `tvd_0` 就占过一个号）。故先试"上次那个/调用方指定的"，失败后扫
 * `/dev/video0..31`，判据是**能力**而不是名字：
 *   - `VIDIOC_QUERYCAP`：`VIDEO_CAPTURE` ＋ `STREAMING`；
 *   - `VIDIOC_ENUM_FMT`：**支持 MJPEG**（本模块只做 MJPEG 直存，YUV 节点
 *     如 `tvd_0` 会被这一步挡掉）；
 *   - `VIDIOC_S_FMT` 回报的 pixelformat 必须真是 MJPEG（有的驱动会"best effort"
 *     悄悄换成别的格式，那存出来的文件就是废的）。
 *
 * 探测是静默的：扫 32 个节点时每个都打一条日志会把控制台淹没，只在
 * **全部失败**时打一句人话，以及找到设备时打一句"命中哪个节点"。
 */
#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "defs.h"
#include "logger.h"

/* 扫描上界：`/dev/video0..31`。板端常见 ≤ 8 个节点，32 足够且一次扫完是毫秒级。 */
#define CAPTURE_DEV_SCAN 32
/* 拼路径用的缓冲（`/dev/video31` 与抓拍文件名都远短于此）。 */
#define CAPTURE_PATH_MAX 256

/* --------------------------------------------------------------------------
 * 目录：mkdir -p 语义
 * -------------------------------------------------------------------------- */
static int mkdir_p(const char *path)
{
    char   buf[CAPTURE_PATH_MAX];
    size_t i;
    size_t n;

    if (path == NULL || path[0] == '\0')
        return -1;
    n = strlen(path);
    if (n + 1u > sizeof(buf))
        return -1;
    memcpy(buf, path, n + 1u);
    while (n > 1u && buf[n - 1u] == '/')
        buf[--n] = '\0';                       /* 去掉尾斜杠，别建出空名字段 */
    for (i = 1u; i < n; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return -1;
        buf[i] = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* 为即将写出的文件建好父目录。无 '/'（当前目录）时不算错。 */
static int mkdir_for_file(const char *filename)
{
    char  dir[CAPTURE_PATH_MAX];
    char *slash;
    size_t n;

    if (filename == NULL)
        return -1;
    n = strlen(filename);
    if (n + 1u > sizeof(dir))
        return -1;
    memcpy(dir, filename, n + 1u);
    slash = strrchr(dir, '/');
    if (slash == NULL || slash == dir)
        return 0;
    *slash = '\0';
    return mkdir_p(dir);
}

/* --------------------------------------------------------------------------
 * 设备探测与打开
 * -------------------------------------------------------------------------- */

/* 这个节点是不是"能出 MJPEG 的采集设备"。是则返回已打开的 fd，否则 -1。
 * **静默**（见文件头最后一段）。 */
static int dev_probe(const char *dev)
{
    struct v4l2_capability cap;
    struct v4l2_fmtdesc   fdsc;
    unsigned              caps;
    int                   fd;
    int                   ok = 0;

    /* O_NONBLOCK：设备正被别的进程占着时不会把这里挂住；STREAMON 之后要改回
     * 阻塞（DQBUF 靠阻塞等帧），见 dev_setup()。 */
    fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return -1;

    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        caps = (cap.device_caps != 0u) ? cap.device_caps : cap.capabilities;
        if ((caps & V4L2_CAP_VIDEO_CAPTURE) != 0u &&
            (caps & V4L2_CAP_STREAMING) != 0u)
            ok = 1;
    }

    if (ok) {
        ok = 0;
        memset(&fdsc, 0, sizeof(fdsc));
        fdsc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (fdsc.index = 0u; ioctl(fd, VIDIOC_ENUM_FMT, &fdsc) == 0; fdsc.index++) {
            if (fdsc.pixelformat == V4L2_PIX_FMT_MJPEG) {
                ok = 1;
                break;
            }
        }
    }
    if (!ok) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/* 在已打开的 fd 上把格式/缓冲/取流全部配好，填进 ctx。失败时由调用方 fini。 */
static int dev_setup(capture_context_t *ctx)
{
    struct v4l2_format         fmt;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type         type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int                        flags;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = CAPTURE_WIDTH;
    fmt.fmt.pix.height      = CAPTURE_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    if (ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        log_err("capture: %s S_FMT failed (%s)", ctx->device, strerror(errno));
        return -1;
    }
    /* 驱动"尽力而为"换了格式就是存出来也打不开，宁可当这个节点不能用 */
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        log_warn("capture: %s refused MJPEG (got 0x%08x)", ctx->device,
                 (unsigned)fmt.fmt.pix.pixelformat);
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.count  = 1;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        log_err("capture: %s REQBUFS failed (%s)", ctx->device, strerror(errno));
        return -1;
    }

    memset(&ctx->buf, 0, sizeof(ctx->buf));
    ctx->buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ctx->buf.memory = V4L2_MEMORY_MMAP;
    ctx->buf.index  = 0;
    if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &ctx->buf) < 0) {
        log_err("capture: %s QUERYBUF failed (%s)", ctx->device, strerror(errno));
        return -1;
    }

    ctx->mem_len = ctx->buf.length;
    ctx->mem = mmap(NULL, ctx->buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                    ctx->fd, ctx->buf.m.offset);
    if (ctx->mem == MAP_FAILED) {
        ctx->mem = NULL;
        ctx->mem_len = 0;
        log_err("capture: %s mmap failed (%s)", ctx->device, strerror(errno));
        return -1;
    }

    if (ioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        log_err("capture: %s STREAMON failed (%s)", ctx->device, strerror(errno));
        return -1;
    }

    /* 回到阻塞模式：DQBUF 要等帧，非阻塞下会一直 EAGAIN */
    flags = fcntl(ctx->fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(ctx->fd, F_SETFL, flags & ~O_NONBLOCK);

    return 0;
}

/* 试一个节点：探测 → 配置。成功返回 0，ctx->fd >= 0。 */
static int try_open(capture_context_t *ctx, const char *dev)
{
    int fd = dev_probe(dev);

    if (fd < 0)
        return -1;
    ctx->fd = fd;
    (void)snprintf(ctx->device, sizeof(ctx->device), "%s", dev);
    if (dev_setup(ctx) != 0) {
        (void)capture_fini(ctx);
        return -1;
    }
    return 0;
}

/* 找一个可用节点：先上次那个（或调用方指定的），再扫 0..31。 */
static int open_any(capture_context_t *ctx)
{
    char hint[CAPTURE_DEV_NAME];
    char dev[CAPTURE_DEV_NAME];
    int  i;

    (void)snprintf(hint, sizeof(hint), "%s", ctx->device);
    if (hint[0] != '\0' && try_open(ctx, hint) == 0)
        return 0;
    for (i = 0; i < CAPTURE_DEV_SCAN; i++) {
        (void)snprintf(dev, sizeof(dev), "/dev/video%d", i);
        if (hint[0] != '\0' && strcmp(dev, hint) == 0)
            continue;                       /* 上面已经试过了 */
        if (try_open(ctx, dev) == 0)
            return 0;
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * 对外接口
 * -------------------------------------------------------------------------- */

int capture_init(capture_context_t *ctx, const char *device)
{
    if (ctx == NULL)
        return -1;

    /* 重复 init ＝ 重开：旧设备不释放会漏 fd 与映射 */
    if (ctx->fd >= 0 || ctx->mem != NULL)
        (void)capture_fini(ctx);
    ctx->fd      = -1;
    ctx->mem     = NULL;
    ctx->mem_len = 0;
    if (device != NULL && device[0] != '\0')
        (void)snprintf(ctx->device, sizeof(ctx->device), "%s", device);

    if (open_any(ctx) != 0) {
        /* **不是错误**：摄像头可以后插，抓拍时会再找一遍（见 capture_process） */
        log_warn("capture init: no MJPEG capture device yet (%s), "
                 "will retry on each snapshot",
                 (device != NULL && device[0] != '\0') ? device : "auto");
        ctx->no_cam = 1;
        return -1;
    }

    ctx->no_cam = 0;
    log_info("capture init: %s ready (%dx%d MJPEG)", ctx->device, CAPTURE_WIDTH,
             CAPTURE_HEIGHT);
    return 0;
}

int capture_process(capture_context_t *ctx, const char *filename)
{
    const int skip_frames = CAPTURE_SKIP_FRAMES;
    int       got = 0;
    int       i;

    if (ctx == NULL || filename == NULL) {
        log_err("capture process: bad arguments");
        return -1;
    }

    /* ---- ① 设备后插：每次抓拍前把"没设备"这件事再试一次 ---- */
    if (ctx->fd < 0) {
        if (open_any(ctx) != 0) {
            if (ctx->no_cam == 0) {
                log_warn("capture: no camera found (plug a UVC camera and retry)");
                ctx->no_cam = 1;
            }
            return -1;
        }
        ctx->no_cam = 0;
        log_info("capture: camera appeared, using %s", ctx->device);
    }

    /* ---- ② 目录不在就建（SD 重插/删目录/工厂复位后都靠这一步） ---- */
    if (mkdir_for_file(filename) != 0) {
        log_warn("capture: cannot create dir for %s (%s)", filename, strerror(errno));
        /* 不立刻返回：目录可能本来就在（EEXIST 已吞掉），真写不进去时 fprintf
         * 那条错误日志会给出准确的 errno。 */
    }

    log_info("capture process: starting snapshot to %s (skipping %d frames)", filename,
             skip_frames);

    for (i = 0; i < skip_frames + 1; i++) {
        if (ioctl(ctx->fd, VIDIOC_QBUF, &ctx->buf) < 0) {
            log_err("capture process: QBUF failed at frame %d (%s)", i, strerror(errno));
            goto dev_lost;
        }
        if (ioctl(ctx->fd, VIDIOC_DQBUF, &ctx->buf) < 0) {
            log_err("capture process: DQBUF failed at frame %d (%s)", i, strerror(errno));
            goto dev_lost;
        }

        if (i == skip_frames) {
            FILE *f = fopen(filename, "wb");

            if (f == NULL) {
                log_err("capture process: failed to open %s for writing (%s)", filename,
                        strerror(errno));
                return -1;
            }
            {
                size_t written = fwrite(ctx->mem, 1, ctx->buf.bytesused, f);

                (void)fclose(f);
                if (written == ctx->buf.bytesused) {
                    log_info("capture process: snapshot saved successfully (%u bytes)",
                             ctx->buf.bytesused);
                    got = 1;
                } else {
                    log_err("capture process: file write incomplete (%lu of %u bytes)",
                            (unsigned long)written, ctx->buf.bytesused);
                }
            }
        }
    }
    return got ? 0 : -1;

dev_lost:
    /* 抓拍中途掉线（USB 被拔）：把设备彻底释放，下一次抓拍重新找节点。
     * 不释放的话 fd 已经失效，之后的 QBUF 会一直报同样的错。 */
    (void)capture_fini(ctx);
    ctx->no_cam = 1;
    log_warn("capture: device %s lost during snapshot (unplugged?), "
             "it will be reopened on the next snapshot",
             ctx->device);
    return -1;
}

int capture_fini(capture_context_t *ctx)
{
    if (ctx == NULL)
        return 0;

    if (ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        (void)ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        if (ctx->mem != NULL && ctx->mem_len > 0u)
            (void)munmap(ctx->mem, ctx->mem_len);
        (void)close(ctx->fd);
        ctx->fd = -1;
        ctx->mem = NULL;
        ctx->mem_len = 0;
        log_info("capture fini: device closed and resources released");
    }
    return 0;
}
