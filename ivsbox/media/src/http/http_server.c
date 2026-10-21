#include "http_server.h"

#include "defs.h"
#include "logger.h"
#include "mongoose.h"
#include "media.h"

static bool s_signaled = false;
static struct mg_mgr mgr;

static int get_id(const struct mg_str *query);
static void handleMessage(struct mg_connection *c, int ev, void *ev_data);

// 信号处理函数（捕获 Ctrl+C）
static void signal_handler(int sig)
{
    (void)sig; /* 信号编号用不上，只借它的到来置退出标志 */
    s_signaled = true;
}

// 初始化
int http_server_init()
{
    mg_mgr_init(&mgr);
    mg_log_set(MG_LL_INFO);
    return 0;
}

// 启动
int http_server_start()
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 监听逻辑...
    // mg_http_listen(&mgr, "http://127.0.0.1:18000", handleMessage, NULL);
    mg_http_listen(&mgr, "http://0.0.0.0:18000", handleMessage, NULL);

    while (!s_signaled)
    {
        mg_mgr_poll(&mgr, 100);
    }

    return 0;
}

// 销毁
int http_server_fini()
{
    mg_mgr_free(&mgr);
    return 0;
}

// 消息分发
static void handleMessage(struct mg_connection *c, int ev, void *ev_data)
{
    int ret = -1;

    if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *msg = (struct mg_http_message *)ev_data;
        log_info("http request: %.*s?%.*s",
               (int)msg->uri.len, msg->uri.buf,
               (int)msg->query.len, msg->query.buf);

        // 抓图
        if (mg_match(msg->uri, mg_str(API_URI_CAPTURE), NULL))
        {
            char path[256] = {0};
            ret = mg_http_get_var(&msg->query, API_QUERY_PATH, path, sizeof(path));
            if (ret < 0)
            {
                mg_http_reply(c, 400, "", "Invalid path");
                return;
            }

            // 异步处理，不用判断返回值
            media_capture(path);
            mg_http_reply(c, 200, "", "OK");
        }
        // 开启录制
        else if (mg_match(msg->uri, mg_str(API_URI_RECORD_START), NULL))
        {
            int id = get_id(&msg->query);
            if (id < 0)
            {
                mg_http_reply(c, 400, "", "Invalid id");
                return;
            }

            char url[256] = {0};
            ret = mg_http_get_var(&msg->query, API_QUERY_URL, url, sizeof(url));
            if (ret < 0 || url[0] == '\0')
            {
                mg_http_reply(c, 400, "", "Invalid url");
                return;
            }

            log_info("url: %s", url);

            // 异步处理，不用判断返回值
            media_record_start(id, url);
            mg_http_reply(c, 200, "", "OK");
        }
        // 停止录制
        else if (mg_match(msg->uri, mg_str(API_URI_RECORD_STOP), NULL))
        {
            int id = get_id(&msg->query);
            if (id < 0)
            {
                mg_http_reply(c, 400, "", "Invalid id");
                return;
            }

            // 异步处理，不用判断返回值
            ret = media_record_stop(id);
            mg_http_reply(c, 200, "", "OK");
        }
        // 录制状态
        else if (mg_match(msg->uri, mg_str(API_URI_RECORD_STATUS), NULL))
        {
            int id = get_id(&msg->query);
            if (id < 0)
            {
                mg_http_reply(c, 400, "", "Invalid id");
                return;
            }

            ret = media_record_status(id);
            if (ret == Recording)
            {
                mg_http_reply(c, 200, "", "1");
            }
            else
            {
                mg_http_reply(c, 200, "", "0");
            }
            return;
        }
        else
        {
            mg_http_reply(c, 404, "", "Not found");
            return;
        }
    }

    return;
}

static int get_id(const struct mg_str *query)
{
    char id_str[8] = {0};
    int ret = mg_http_get_var(query, API_QUERY_ID, id_str, sizeof(id_str));

    /* 取不到 id 参数必须**当场失败**。这里原先是 `ret - 1;` 这条空语句
     * （语句无效，编译器只报 -Wunused-value，运行时什么都不做）：id_str 保持
     * 全 0，`atoi("")` 得 0 ⇒ **静默当成 0 号通道** —— 前端漏传 id 就会去操作
     * 0 号通道（开录/停录/查状态都中招）。三个调用点都按 `id < 0` 回 400，
     * 所以这里返回 -1 正是它们期待的语义。 */
    if (ret < 0)
    {
        return -1;
    }

    int id = atoi(id_str);
    if (id < 0 || id >= MEDIA_MAX_CHANNELS)
    {
        return -1;
    }

    return id;
}
