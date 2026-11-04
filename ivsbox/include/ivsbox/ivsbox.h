/*
 * IVSBox 公共头文件
 * 仅定义版本与顶层初始化接口，业务头文件后续按模块拆分。
 */
#ifndef IVSBOX_H
#define IVSBOX_H

/* ============================ 发版就改这里，别只重新打包 ============================
 * 设备自报给平台的版本串是 `iv_version_sysver()` = `<MAJOR>.<MINOR>.<PATCH>.<构建日期>`
 * （`src/core/iv_version.c`），平台「设备详情」里的**系统版本 / 固件版本两栏都来自它**。
 *
 * 升级包清单里的 `version` 是另一套口径（`M.m.p`，只用来和本机比大小，见 `iv_ota.h`），
 * **换二进制不会让它变**：2026-11-02 就栽在这里 —— 连打了 1.0.1/1.0.2/1.0.3 三个包，
 * 包里的 `ivsboxd` 全是同一份、宏仍是 1.0.0，装完平台那两栏自然一动不动。
 *
 * 所以规矩是：**每发一个包，先把这里抬上去**（`tools/mkota.py build` 不给 --version
 * 时直接读这三个宏，包版本与设备自报版本同源，抬一处就够）。 */
/* 2026-11-03：1.0.6 → 1.0.7。1.0.6 这个号已经被三个不同内容的构建用过
 * （官方 `b38444b2`、上午的 `5c9d3c5c`、修完日志行的 `7e8fcfd4`），
 * 版本串却一模一样 ⇒ 现场分不清、OTA 也判不出"要不要装"。抬号是为了让
 * `releases/1.0.6` 恢复成官方内容、新构建独占 `1.0.7`。 */
#define IVSBOX_VERSION_MAJOR 1
#define IVSBOX_VERSION_MINOR 0
#define IVSBOX_VERSION_PATCH 0

#ifdef __cplusplus
extern "C" {
#endif

int ivsbox_init(void);
void ivsbox_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_H */
