/*
 * IVSBox 公共头文件
 * 仅定义版本与顶层初始化接口，业务头文件后续按模块拆分。
 */
#ifndef IVSBOX_H
#define IVSBOX_H

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
