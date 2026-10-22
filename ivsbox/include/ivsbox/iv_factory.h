/*
 * IVSBox 恢复出厂 / 擦除存储（libivmodules）
 *
 * ============================ 它是什么 ============================
 * 把设备带回"出厂可交付"状态，对齐单片机参考 web 的系统操作页：
 *   ① **恢复出厂设置** —— 配置全部回内置默认值（`iv_config_reset_defaults()`）；
 *   ② **擦除存储** —— 清掉录像/快照（媒体根）、日志、数据库里的历史数据。
 * 两件事**解耦**：可以只恢复配置、也可以只清存储（现场最常见的是"卡满了，先把
 * 视频清掉"），所以这里用**位掩码**而不是一个布尔。
 *
 * ============================ 恢复出厂的范围（用户 2026-10-22 口径） ============================
 * **除工厂身份分区（devid / MAC / SN）外全部恢复默认值**，含：
 *   - `sys.device.name` 回空串；
 *   - `net.server.*` 回空地址（＝不启平台链路，`iv_report_reconnect()` 会断开）；
 *   - `camera.ch` **整段删除**（出厂态就是"一路摄像机都没有"）。
 * 工厂身份在 `private` 裸分区里、**根本不在配置中**，所以本模块天生碰不到它 ——
 * 这不是"忘了排掉"，是结构上不可能。
 *
 * **Web 口令不在这里**：口令哈希落在 `<secure>/web.passwd`，由 `ivsbox-web`
 * 自己写（单写者）。恢复出厂要重挂"必须改密"，由 Web 端在 ctl 返回成功后自己
 * 重置口令文件 —— 本模块不认识口令，也不该认识（模块层不碰鉴权）。
 *
 * ============================ 媒体删不删（用户 2026-10-22 口径） ============================
 * **默认不删**。`erase_mask` 不给就是只恢复配置；页面上"擦除存储"是独立的动作，
 * 且要二次确认 —— 删录像不可撤销，不能跟"恢复出厂"绑在一起默认执行。
 */
#ifndef IVSBOX_IV_FACTORY_H
#define IVSBOX_IV_FACTORY_H

#include <stdint.h>

#include "ivsbox/iv_config.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 擦除位。`ALL` 只为方便调用方，语义与三者相或等价。 */
#define IV_FACTORY_ERASE_MEDIA 0x01u /* 媒体根（SD 卡）下的录像/快照 */
#define IV_FACTORY_ERASE_LOG   0x02u /* <data_root>/log 下的按小时日志 */
#define IV_FACTORY_ERASE_DB    0x04u /* control.db 四张表的**数据行**（不删文件） */
#define IV_FACTORY_ERASE_ALL   0x07u

/* 递归擦除的深度上限。媒体目录按 `<年>/<月>/<日>/<文件>` 组织，4 层足够；
 * 给到 8 是给"将来多一层分组"留余量 —— 超过就报错而不是一直往里钻。 */
#define IV_FACTORY_WIPE_DEPTH_MAX 8u
#define IV_FACTORY_PATH_MAX       192u

typedef struct {
    /* `<data_root>`：`/mnt/UDISK/ivsbox`。空 ⇒ 擦日志返回 `IV_ENOTSUP`。 */
    const char *data_root;
    /* 媒体根：`IV_STORAGE_ROOT_DEFAULT`（`/mnt/sdcard`）或环境变量覆盖值。
     * 空 ⇒ 擦媒体返回 `IV_ENOTSUP`。 */
    const char *media_root;
} iv_factory_cfg_t;

typedef struct {
    unsigned      cfg_changed; /* 回默认的键数（含 camera.ch 的删除条数） */
    uint64_t      cfg_version; /* 回默认并落盘后的配置版本号 */
    int           cfg_saved;   /* 1 = 已落盘；0 = 只在内存（调用方要报错） */
    unsigned      erase_mask;  /* 实际执行的擦除位 */
    /* 删掉的**条目**数（文件与目录一起计）。不拆成两个数：`wipe` 的语义就是
     * "清空这一棵树"，拆开只会在两份计数之间制造对不上的机会。 */
    unsigned long entries_removed;
    int64_t       db_rows_removed;
} iv_factory_report_t;

/*
 * 恢复出厂：配置回默认 → 落盘 → 按掩码擦除。
 *
 * **顺序不能反**：先配置、后擦数据。配置写盘失败（`IV_EIO`/`IV_ENOSPC`）时
 * **一条数据都不擦**并立刻返回错误 —— "配置没复位但录像没了"是最坏的半成品。
 *
 * 返回：`IV_OK` / `IV_EINVAL`（参数或掩码非法）/ `IV_ENOTSUP`（要擦的东西没有
 * 根路径可用，即"能调但没接"）/ 配置写盘错误码。
 * `rep` 可为 NULL。`rep->cfg_changed`/`cfg_saved` 在返回错误时仍可能有效（例如
 * 只在内存改了但没落盘），调用方应按 `cfg_saved` 决定怎么报。
 */
int iv_factory_reset(iv_config_t *cfg, iv_store_t *store, const iv_factory_cfg_t *fc,
                     unsigned erase_mask, iv_factory_report_t *rep);

/*
 * 清空 `root` **下的全部内容**，`root` 本身不删（挂载点不能 rmdir）。
 *
 * 安全约束（每一条都是防"把系统删了"）：
 *   - `root` 必须是**绝对路径**、长度 >= 2、不能是 `/`、不能含 `..`；
 *   - 用 `lstat()` 判类型，**符号链接只 unlink、绝不跟随**（否则一个指向 `/` 的
 *     链接就能把整机删干净）；
 *   - 递归深度上限 `IV_FACTORY_WIPE_DEPTH_MAX`，超限报 `IV_ERANGE`；
 *   - 根不存在按"没有可擦的东西"处理（`IV_OK`，`*removed` 不增）。
 *
 * `*removed` 为删掉的条目数（文件 + 目录，可为 NULL）。
 */
int iv_factory_wipe(const char *root, unsigned long *removed);

/* 擦除位的名字（`"media"` / `"log"` / `"db"`），未知位返回 NULL；解析失败返回 0。
 * 给 ctl 解析 JSON 参数与拼响应共用，避免两处各写一份字符串。 */
const char *iv_factory_erase_name(unsigned bit);
unsigned    iv_factory_erase_bit(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_FACTORY_H */
