#include "record.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/log.h>
#include <libavutil/opt.h>

#include "defs.h"
#include "logger.h"

static int generate_record_path(int channel_id, char *out_path, size_t out_len);
static int make_path(const char *path);

void print_error(int err, const char *prefix) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, err_buf, sizeof(err_buf));
    log_err("%s: %s (error code: %d)", prefix, err_buf, err);
}

// --- 资源清理函数 ---

static void input_fini(record_context_t *ctx) {
    if (ctx && ctx->input_ctx) {
        avformat_close_input(&ctx->input_ctx);
        ctx->input_ctx = NULL;
    }
}

static void output_fini(record_context_t *ctx, int is_fatal) {
    if (!ctx || !ctx->output_ctx) return;

    // 只有在非致命错误（如正常切换文件）时才写入文件尾部
    if (!is_fatal && ctx->output_ctx->pb) {
        av_write_trailer(ctx->output_ctx);
    }

    if (ctx->output_ctx->pb) {
        avio_closep(&ctx->output_ctx->pb);
    }

    avformat_free_context(ctx->output_ctx);
    ctx->output_ctx = NULL; 
}

// --- 初始化函数 ---

static int input_init(record_context_t *ctx) {
    AVDictionary *options = NULL;
    
    // 使用 defs.h 中的协议配置
    char timeout_str[32];
    snprintf(timeout_str, sizeof(timeout_str), "%d", RTSP_CONN_TIMEOUT_MS);
    char buf_size_str[32];
    snprintf(buf_size_str, sizeof(buf_size_str), "%d", RTSP_BUFFER_SIZE);

    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", timeout_str, 0); 
    av_dict_set(&options, "buffer_size", buf_size_str, 0); 

    int ret = avformat_open_input(&ctx->input_ctx, ctx->url, NULL, &options);
    av_dict_free(&options);

    if (ret < 0) {
        log_err("record[%d]: failed to open RTSP %s", ctx->id, ctx->url);
        print_error(ret, "avformat_open_input");
        return -1;
    }

    // 增加分析时长到 5 秒（单位是微秒）
    ctx->input_ctx->max_analyze_duration = 5 * AV_TIME_BASE;
    // 增加探测缓冲区大小到 2MB
    ctx->input_ctx->probesize = 2 * 1024 * 1024;

    if ((ret = avformat_find_stream_info(ctx->input_ctx, NULL)) < 0) {
        log_err("record[%d]: failed to find stream info", ctx->id);
        print_error(ret, "avformat_find_stream_info");
        input_fini(ctx);
        return -1;
    }
    return 0;
}

static int output_init_smart(record_context_t *ctx, int allow_audio) {
    int ret = -1;
    char filename[256] = {0};

    if ((ret = generate_record_path(ctx->id, filename, sizeof(filename))) != 0) {
        log_err("record[%d]: failed to generate path", ctx->id);
        return -1;
    }

    if ((ret = avformat_alloc_output_context2(&ctx->output_ctx, NULL, "mp4", filename)) < 0) {
        log_err("record[%d]: failed to alloc output context", ctx->id);
        print_error(ret, "avformat_alloc_output_context2");
        return -1;
    }

    ctx->output_ctx->flags |= AVFMT_FLAG_GENPTS;
    // 放宽 MP4 封装器的标准检查
    ctx->output_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    for (int i = 0; i < (int)ctx->input_ctx->nb_streams; i++) {
        AVStream *in_stream = ctx->input_ctx->streams[i];

        // 如果本次尝试不允许音频，直接跳过音频流
        if (!allow_audio && in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            continue; 
        }

        AVStream *out_stream = avformat_new_stream(ctx->output_ctx, NULL);
        if (!out_stream) {
            log_err("record[%d]: failed to create new stream", ctx->id);
            goto err;
        }

        if ((ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar)) < 0) {
            log_err("record[%d]: failed to copy codec parameters", ctx->id);
            print_error(ret, "avcodec_parameters_copy");
            goto err;
        }

        switch (out_stream->codecpar->codec_id) {
            case AV_CODEC_ID_HEVC:
                out_stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
                break;

            case AV_CODEC_ID_PCM_ALAW:
                out_stream->codecpar->codec_tag = MKTAG('a', 'l', 'a', 'w');
                out_stream->codecpar->block_align = 1;
                break;

            case AV_CODEC_ID_PCM_MULAW:
                out_stream->codecpar->codec_tag = MKTAG('u', 'l', 'a', 'w');
                out_stream->codecpar->block_align = 1;
                break;

            default:
                // 对于 H.264 等格式，0 表示让 FFmpeg 自动选择默认 Tag
                out_stream->codecpar->codec_tag = 0;
                break;
        }

        // 同步 time_base
        out_stream->time_base = in_stream->time_base;
        
        // 索引保护（由于 context 定义数组大小为 8）
        if (i < RECORD_MAX_STREAMS) ctx->last_dts[i] = AV_NOPTS_VALUE;
    }

    if (!(ctx->output_ctx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&ctx->output_ctx->pb, filename, AVIO_FLAG_WRITE)) < 0) {
            log_err("record[%d]: failed to open output file %s", ctx->id, filename);
            print_error(ret, "avio_open");
            goto err;
        }
    }

    AVDictionary *options = NULL;
    // 碎片化MP4配置
    av_dict_set(&options, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    ret = avformat_write_header(ctx->output_ctx, &options);
    av_dict_free(&options);

    if (ret < 0) {
        log_err("record[%d]: failed to write header", ctx->id);
        print_error(ret, "avformat_write_header");
        goto err;
    }

    ctx->last_segment_time = time(NULL);
    log_info("record[%d]: started new segment: %s", ctx->id, filename);
    return 0;

err:
    output_fini(ctx, 1);
    return -1;
}

static int output_init(record_context_t *ctx) {
    // 第一次尝试：带音频
    if (output_init_smart(ctx, 1) >= 0) {
        return 0;
    }

    log_warn("record[%d]: audio init failed, retrying without audio...", ctx->id);

    // 第二次尝试：不带音频
    if (output_init_smart(ctx, 0) >= 0) { 
        return 0;
    }

    log_err("record[%d]: fatal - even no-audio init failed", ctx->id);

    return -1;
}

// --- 主业务逻辑 ---

int record_process(record_context_t *ctx, int segment_duration, exitFunc exit) {
    int cnt = 0;
    int fatal_error = 0;
    AVPacket *pkt = av_packet_alloc();

    if (input_init(ctx) < 0) {
        if (pkt) av_packet_free(&pkt);
        return -1;
    }

    if (output_init(ctx) < 0) {
        input_fini(ctx);
        if (pkt) av_packet_free(&pkt);
        return -1;
    }

    while (av_read_frame(ctx->input_ctx, pkt) >= 0) {
        unsigned int stream_idx = (unsigned int)pkt->stream_index;

        if (!ctx->output_ctx || stream_idx >= ctx->output_ctx->nb_streams || stream_idx >= 8) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream *in_stream = ctx->input_ctx->streams[stream_idx];
        AVStream *out_stream = ctx->output_ctx->streams[stream_idx];

        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);

        // 时间戳平滑处理
        if (ctx->last_dts[stream_idx] != AV_NOPTS_VALUE) {
            if (pkt->dts <= ctx->last_dts[stream_idx]) {
                pkt->dts = ctx->last_dts[stream_idx] + 1;
            }
        }
        if (pkt->pts < pkt->dts) pkt->pts = pkt->dts;
        ctx->last_dts[stream_idx] = pkt->dts;
        pkt->pos = -1;

        int ret = av_write_frame(ctx->output_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(ENOSPC) || errno == ENOSPC) {
                log_err("record[%d]: fatal - no space left on device", ctx->id);
                fatal_error = 1; 
                av_packet_unref(pkt);
                break; 
            }
            log_warn("record[%d]: write frame failed, ret=%d", ctx->id, ret);
            print_error(ret, "av_write_frame");
        }
        
        int is_key = (pkt->flags & AV_PKT_FLAG_KEY);
        av_packet_unref(pkt);

        // 切片逻辑：使用 defs.h 宏
        time_t current_time = time(NULL);
        int elapsed = (int)(current_time - ctx->last_segment_time);
        if ((elapsed >= segment_duration && is_key) || 
            (elapsed >= (segment_duration + RECORD_FORCE_SEG_LIMIT))) {
            
            log_info("record[%d]: segment duration reached, rotating...", ctx->id);
            output_fini(ctx, 0);
            if (output_init(ctx) < 0) {
                fatal_error = 1;
                break;
            }
        }

        // 外部退出检测（每50帧检测一次，降低 CPU 开销）
        if (++cnt >= 50) {
            cnt = 0;
            if (exit(ctx->id)) {
                log_info("record[%d]: exit requested by caller", ctx->id);
                break;
            }
        }
    }

    output_fini(ctx, fatal_error);
    input_fini(ctx);
    if (pkt) av_packet_free(&pkt);
    
    log_info("record[%d]: process stopped", ctx->id);
    return 0;
}

// --- 路径处理函数 ---

static int make_path(const char *path) {
    if (access(path, F_OK) == 0) return 0;
    char temp[256];
    strncpy(temp, path, sizeof(temp));
    temp[sizeof(temp)-1] = '\0';

    for (char *p = temp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (access(temp, F_OK) != 0) {
                if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
                    return -1;
                }
            }
            *p = '/';
        }
    }
    return (mkdir(temp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

static int generate_record_path(int channel_id, char *out_path, size_t out_len) {
    time_t now = time(NULL);
    struct tm local_time;
    if (!localtime_r(&now, &local_time)) return -1;

    char dir_path[128];
    snprintf(dir_path, sizeof(dir_path), "%s/%04d%02d%02d",
             PATH_RECORD_ROOT, 
             local_time.tm_year + 1900, 
             local_time.tm_mon + 1, 
             local_time.tm_mday);
    
    if (make_path(dir_path) != 0) {
        log_err("record[%d]: failed to create directory %s", channel_id, dir_path);
        return -1;
    }

    snprintf(out_path, out_len, "%s/channel%d_%02d%02d%02d.mp4",
             dir_path, channel_id, local_time.tm_hour, local_time.tm_min, local_time.tm_sec);

    return 0;
}
