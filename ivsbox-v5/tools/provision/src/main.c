/* main.c —— ivsbox-provision 出厂工装入口（骨架）
 *
 * 目标形态（架构文档 §10.1、§13.1）：
 *   写入设备 ID 与 MAC 等工厂身份到 /data/ivsbox/factory/ 或安全存储；
 *   身份数据独立于普通配置，写后加锁定位或受控维护流程，
 *   不得以普通 Web 配置方式修改。
 *
 * 权限约束：仅工厂模式可运行，量产后关闭入口。
 *
 * 尚未实现的骨架项：
 *   G3  与 ivsbox-updater 同属"按需特权进程"，触发机制尚未定义。
 */
#include <stdio.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_version.h"

#define IVS_MOD "provision"

int main(void)
{
    ivs_log_set_level(IVS_LOG_INFO);

    IVS_LOGI(IVS_MOD, "%s provision %s (stage: skeleton)", IVS_PROJECT_CODE,
             IVS_VERSION_STRING);
    IVS_LOGW(IVS_MOD, "skeleton only: device id and mac writing are not implemented");
    IVS_LOGI(IVS_MOD, "exit");

    return 0;
}
