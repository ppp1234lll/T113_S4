#ifndef __RECORD_H__
#define __RECORD_H__

#include <libavformat/avformat.h>
#include "defs.h"

typedef int (*exitFunc)(int);

typedef struct
{
    int id;
    char url[256];
    AVFormatContext *input_ctx;
    AVFormatContext *output_ctx;
    time_t last_segment_time;
    // 记录每个 stream 上一次写入成功的 DTS
    int64_t last_dts[RECORD_MAX_STREAMS];
} record_context_t;

int record_process(record_context_t *ctx, int segment_duration, exitFunc exit);

#endif
