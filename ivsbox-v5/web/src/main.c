/* main.c —— ivsbox-web 管理进程入口（骨架）
 *
 * 目标形态（架构文档 §4.4、§9）：
 *   使用成熟 HTTP 服务实现，不自研 HTTP 解析器；
 *   所有状态与控制操作经 IPC 请求 ivsboxd 或 ivsbox-media；
 *   不直接写配置文件，不直接打开控制库或媒体库；
 *   摄像机 Web 反向代理只允许访问设备档案内的 IP 与允许端口，
 *   凭据在服务端使用，禁止下发浏览器或写入 URL。
 *
 * 权限约束：本进程为无特权用户，不访问串口、watchdog、配置密钥（§13.1）。
 *
 * 尚未实现的骨架项：
 *   G4  Web 下载媒体文件走哪条路径尚未定（推荐 SCM_RIGHTS 传 fd）
 *   G18 代理响应体与上传大小上限需与内存预算一起定
 */
#include <stdio.h>

#include "ivsbox/iv_ipc.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_version.h"

#define IVS_MOD "web"

int main(void)
{
    ivs_log_set_level(IVS_LOG_INFO);

    IVS_LOGI(IVS_MOD, "%s web %s (ipc proto v%u)", IVS_PROJECT_CODE, IVS_VERSION_STRING,
             (unsigned)IVS_IPC_PROTO_VERSION);
    IVS_LOGW(IVS_MOD, "skeleton only: http server, rest api and proxy are not implemented yet");
    IVS_LOGI(IVS_MOD, "exit");

    return 0;
}
