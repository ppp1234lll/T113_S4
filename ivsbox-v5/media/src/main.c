/* main.c —— ivsbox-media 媒体进程入口（骨架）
 *
 * 目标形态（架构文档 §4.3、§8）：
 *   一个控制循环处理 IPC / 任务状态 / 进程信号；
 *   每个活动 RTSP 会话一条独立 pipeline，路数受 media.max_channels 限制；
 *   抓图任务进有界队列，默认串行或低并发；音频高优先级并可抢占非关键录像；
 *   存储巡检定时检查挂载状态、写入错误与水位。
 *
 * 资源约束（缺口清单 G1）：本进程须由 cgroup 限 CPU/内存，数值待 BSP 能力清单实测后确定。
 *
 * 尚未实现的骨架项：
 *   G1  media.max_channels 缺省值应由内存预算倒推（建议缺省 2、硬上限 3~4）
 *   G4  Web 下载媒体文件的跨进程路径尚未定（SCM_RIGHTS / 只读目录 / 本地 HTTP）
 */
#include <stdio.h>

#include "ivsbox/iv_ipc.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_version.h"

#define IVS_MOD "media"

int main(void)
{
    ivs_log_set_level(IVS_LOG_INFO);

    IVS_LOGI(IVS_MOD, "%s media %s (ipc proto v%u)", IVS_PROJECT_CODE, IVS_VERSION_STRING,
             (unsigned)IVS_IPC_PROTO_VERSION);
    IVS_LOGW(IVS_MOD, "skeleton only: rtsp, snapshot, record and audio are not implemented yet");
    IVS_LOGI(IVS_MOD, "exit");

    return 0;
}
