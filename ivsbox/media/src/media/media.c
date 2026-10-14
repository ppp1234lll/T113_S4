#include "media.h"

#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "defs.h"
#include "logger.h"
#include "capture.h"
#include "record.h"

typedef struct
{
    bool is_running;
    bool is_recording;
    time_t last_keep_alive_time;
    record_context_t ctx;
} record_task_t;

static record_task_t g_tasks[MEDIA_MAX_CHANNELS];
static pthread_mutex_t task_lock = PTHREAD_MUTEX_INITIALIZER;
static capture_context_t g_capture_ctx;
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;

static int set_record_stop(int id);
static int get_record_status(int id);
static void *record_worker(void *arg);
static void *monitor_worker(void *arg);

// 初始化
int media_init()
{
    // 初始化 V4L2
    if (capture_init(&g_capture_ctx, PATH_VIDEO_DEVICE) != 0) {
        log_err("capture init failed!");
        // 即使失败也继续，防止录制业务受影响
    }

    pthread_t tid;

    for (int i = 0; i < MEDIA_MAX_CHANNELS; i++)
    {
        g_tasks[i].is_running = true;
        g_tasks[i].is_recording = false;
        g_tasks[i].last_keep_alive_time = time(NULL);
        g_tasks[i].ctx.id = i;
        g_tasks[i].ctx.url[0] = '\0';

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, MEDIA_STACK_SIZE);
        pthread_create(&tid, &attr, record_worker, &g_tasks[i]);
        pthread_attr_destroy(&attr);
    }

    pthread_create(&tid, NULL, monitor_worker, NULL);

    return 0;
}

// 销毁
int media_fini()
{
    for (int i = 0; i < MEDIA_MAX_CHANNELS; i++)
    {
        g_tasks[i].is_running = false;
    }

    capture_fini(&g_capture_ctx);

    return 0;
}


// 抓图：现在是同步阻塞调用
int media_capture(const char *path) {
    int ret = -1;

    pthread_mutex_lock(&capture_lock);
    ret = capture_process(&g_capture_ctx, path);
    pthread_mutex_unlock(&capture_lock);

    log_info("capture to %s, ret: %d", path, ret);

    return ret;
}

// 开始录制
int media_record_start(int id, const char *url)
{
    if (id < 0 || id >= MEDIA_MAX_CHANNELS) {
        return -1;
    }

    pthread_mutex_lock(&task_lock);
    strncpy(g_tasks[id].ctx.url, url, sizeof(g_tasks[id].ctx.url) - 1);
    g_tasks[id].is_recording = true;
    g_tasks[id].last_keep_alive_time = time(NULL);
    pthread_mutex_unlock(&task_lock);

    return 0;
}

// 结束录制
int media_record_stop(int id)
{
    if (id < 0 || id >= MEDIA_MAX_CHANNELS) {
        return -1;
    }

    return set_record_stop(id);
}

// 录制状态
int media_record_status(int id)
{
    if (id < 0 || id >= MEDIA_MAX_CHANNELS) {
        return -1;
    }

    bool is_recording = false;
    pthread_mutex_lock(&task_lock);
    is_recording = g_tasks[id].is_recording;
    if (is_recording) {
        g_tasks[id].last_keep_alive_time = time(NULL);
    }
    pthread_mutex_unlock(&task_lock);

    return is_recording ? Recording : NoRecord;
}

static int set_record_stop(int id)
{
    pthread_mutex_lock(&task_lock);
    g_tasks[id].is_recording = false;
    g_tasks[id].last_keep_alive_time = time(NULL);
    pthread_mutex_unlock(&task_lock);
    return 0;
}

static int get_record_status(int id)
{
    bool is_recording = false;
    pthread_mutex_lock(&task_lock);
    is_recording = g_tasks[id].is_recording;
    pthread_mutex_unlock(&task_lock);
    return is_recording ? Recording : NoRecord;
}

static int check_need_exit(int id)
{
    bool is_recording = get_record_status(id);
    return is_recording ? 0 : 1;
}

static void *record_worker(void *arg)
{
    int ret = -1;
    record_task_t *task = (record_task_t *)arg;
    int id = task->ctx.id;

    while (task->is_running)
    {
        if (get_record_status(id) == NoRecord)
        {
            usleep(100000);
            continue;
        }

        ret = record_process(&task->ctx, RECORD_SEGMENT_SEC, check_need_exit);

        set_record_stop(id);

        log_warn("record stop, ret: %d, url: %s\n", ret, task->ctx.url);
    }
}

static void *monitor_worker(void *arg)
{
    log_info("record monitor started");
    
    while (1)
    {
        sleep(10); 
        
        time_t now = time(NULL);
        
        for (int i = 0; i < MEDIA_MAX_CHANNELS; i++)
        {
            bool timeout_triggered = false;
            double diff_sec = 0;

            pthread_mutex_lock(&task_lock);
            if (g_tasks[i].is_recording)
            {
                diff_sec = difftime(now, g_tasks[i].last_keep_alive_time);
                if (diff_sec > RECORD_TIMEOUT_SEC)
                {
                    g_tasks[i].is_recording = false;
                    timeout_triggered = true;
                }
            }
            pthread_mutex_unlock(&task_lock);

            if (timeout_triggered)
            {
                log_warn("channel %d timeout (%.0f sec no query), auto stopping", i, diff_sec);
            }
        }
    }

    return NULL;
}
