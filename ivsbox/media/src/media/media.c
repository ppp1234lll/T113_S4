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
    /* 初始化 V4L2。**允许失败**：摄像头是可选外设，而且可以后插 ——
     * `capture_process()` 每次抓拍前会重新找节点（2026-10-28 用户口径：
     * "禁止有先后顺序，摄像头不一定什么时候插入，需要插入后就能用"）。
     * 因此这里失败只降级抓拍能力，录像（走网络 RTSP）不受影响。 */
    if (capture_init(&g_capture_ctx, PATH_VIDEO_DEVICE) != 0) {
        log_warn("capture not ready at startup; will retry on each snapshot "
                 "(record unaffected)");
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

    /* 线程函数必须有返回值：原来走到 `}` 直接返回，`void *` 函数的返回值是
     * 不确定值（未定义行为）。当前没人在 join 它，所以现场没炸，但不是可留的写法。 */
    return NULL;
}

static void *monitor_worker(void *arg)
{
    (void)arg; /* 巡检靠全局 g_tasks，不需要参数 */
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
