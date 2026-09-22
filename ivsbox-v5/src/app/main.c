/* main.c —— ivsboxd 主控进程入口（骨架）
 *
 * 目标形态（架构文档 §4.2）：
 *   一个 epoll Reactor 统一处理 UART / 平台 socket / rtnetlink / GPS fd / IPC /
 *   timerfd / signalfd / eventfd，作为**业务状态的唯一写入者**；
 *   有界慢任务池（2 个 worker）承担 SNMP / ONVIF / RTSP 探测 / DNS；
 *   独立健康线程按「原子进度号 + 关键链路时间戳」门控喂狗。
 *
 * 当前仅为可编译入口，Reactor、慢任务池与模块装配在 M1 实现。
 *
 * 尚未实现的骨架项（见缺口清单）：
 *   G2  喂狗判据：健康检查只看主循环进度，慢任务超时不得参与喂狗
 *   G5  启动就绪分级 L0~L5 与首次喂狗时机
 *   G8  procd respawn 熔断与 failsafe
 */
#include <stdbool.h>
#include <stdio.h>

#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_ipc.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_version.h"

#define IVS_MOD "app"

int main(void)
{
    ivs_log_set_level(IVS_LOG_INFO);

    IVS_LOGI(IVS_MOD, "%s %s (ipc proto v%u, cfg schema v%u)", IVS_PROJECT_CODE,
             IVS_VERSION_STRING, (unsigned)IVS_IPC_PROTO_VERSION,
             (unsigned)IVS_CONFIG_SCHEMA_VERSION);
    IVS_LOGI(IVS_MOD, "mono_clock_ready=%s", (ivs_clock_mono_ms() > 0u) ? "yes" : "degraded");
    IVS_LOGW(IVS_MOD, "skeleton only: reactor, ipc and modules are not implemented yet");
    IVS_LOGI(IVS_MOD, "exit");

    return 0;
}
