#ifndef __CAPTURE_H__
#define __CAPTURE_H__

#include <stddef.h>
#include <linux/videodev2.h>

typedef struct {
    int fd;
    void *mem;
    size_t mem_len;
    struct v4l2_buffer buf;
} capture_context_t;

int capture_init(capture_context_t *ctx, const char *device);
int capture_process(capture_context_t *ctx, const char *filename);
int capture_fini(capture_context_t *ctx);

#endif
