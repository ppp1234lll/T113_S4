/* main.c —— ivsbox-updater 升级代理入口（骨架）
 *
 * 目标形态（架构文档 §11）：
 *   下载、验签、空间检查、安装、切换与回滚全部在本进程内完成；
 *   ivsboxd 只负责接收命令、协调下载并上报进度。
 *
 * 安全约束（§11.1、§13.2）：
 *   升级包必须携带产品型号、硬件版本范围、软件版本、最低可回滚版本、
 *   文件清单、SHA-256 与数字签名；签名校验为**强制步骤**，
 *   CRC 与哈希不能替代发布者认证。
 *
 * 尚未实现的骨架项：
 *   G3  本进程为 root 且按需运行，但「非特权 ivsboxd 如何安全拉起它」尚未定义。
 *       实现时必须遵守：不接受任何来自网络或 Web 的路径/命令参数；
 *       本进程自行重新验签，不信任调用方的校验结果。
 *   G9  "连续启动失败计数" 的存储位置与防篡改手段尚未定
 *   G10 core dump 策略尚未定（本进程崩溃现场需要留证）
 */
#include <stdio.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_version.h"

#define IVS_MOD "updater"

int main(void)
{
    ivs_log_set_level(IVS_LOG_INFO);

    IVS_LOGI(IVS_MOD, "%s updater %s (stage: skeleton)", IVS_PROJECT_CODE, IVS_VERSION_STRING);
    IVS_LOGW(IVS_MOD, "skeleton only: verify, install, switch and rollback are not implemented");
    IVS_LOGI(IVS_MOD, "exit");

    return 0;
}
