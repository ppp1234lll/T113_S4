#include "logger.h"
#include "http_server.h"
#include "media.h"

#define Version "V0.0.1-20260417"

int main()
{
    log_init();
    log_info("--- System Starting, Version: %s ---", Version);

    // 初始化
    media_init();
    http_server_init();

    // 运行，阻塞进程
    http_server_start();

    log_info("System shutting down...");

    // 进程退出，销毁
    http_server_fini();
    media_fini();

    return 0;
}
